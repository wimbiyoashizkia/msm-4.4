/*
 *  linux/drivers/mmc/card/queue.c
 *
 *  Copyright (C) 2003 Russell King, All Rights Reserved.
 *  Copyright 2006-2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/bitops.h>
#include <linux/delay.h>

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include "queue.h"
#include "block.h"

#define MMC_QUEUE_BOUNCESZ	65536

/*
 * Based on benchmark tests the default num of requests to trigger the write
 * packing was determined, to keep the read latency as low as possible and
 * manage to keep the high write throughput.
 */
#define DEFAULT_NUM_REQS_TO_START_PACK 17

/*
 * Prepare a MMC request. This just filters out odd stuff.
 */
static int mmc_prep_request(struct request_queue *q, struct request *req)
{
	struct mmc_queue *mq = q->queuedata;

	/*
	 * We only like normal block requests and discards.
	 */
	if (req->cmd_type != REQ_TYPE_FS && !(req->cmd_flags & REQ_DISCARD)) {
		blk_dump_rq_flags(req, "MMC bad request");
		return BLKPREP_KILL;
	}

	if (mq && (mmc_card_removed(mq->card) || mmc_access_rpmb(mq)))
		return BLKPREP_KILL;

	req->cmd_flags |= REQ_DONTPREP;

	return BLKPREP_OK;
}

static struct request *mmc_peek_request(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	mq->cmdq_req_peeked = NULL;

	spin_lock_irq(q->queue_lock);
	if (!blk_queue_stopped(q))
		mq->cmdq_req_peeked = blk_peek_request(q);
	spin_unlock_irq(q->queue_lock);

	return mq->cmdq_req_peeked;
}

static bool mmc_check_blk_queue_start_tag(struct request_queue *q,
					  struct request *req)
{
	int ret;

	spin_lock_irq(q->queue_lock);
	ret = blk_queue_start_tag(q, req);
	spin_unlock_irq(q->queue_lock);

	return !!ret;
}

static inline void mmc_cmdq_ready_wait(struct mmc_host *host,
					struct mmc_queue *mq)
{
	struct mmc_cmdq_context_info *ctx = &host->cmdq_ctx;
	struct request_queue *q = mq->queue;

	/*
	 * Wait until all of the following conditions are true:
	 * 1. There is a request pending in the block layer queue
	 *    to be processed.
	 * 2. If the peeked request is flush/discard then there shouldn't
	 *    be any other direct command active.
	 * 3. cmdq state should be unhalted.
	 * 4. cmdq state shouldn't be in error state.
	 * 5. There is no outstanding RPMB request pending.
	 * 6. free tag available to process the new request.
	 *    (This must be the last condtion to check)
	 */
	wait_event_interruptible(ctx->wait, kthread_should_stop()
		|| (mmc_peek_request(mq) &&
		!((mq->cmdq_req_peeked->cmd_flags & (REQ_FLUSH | REQ_DISCARD))
		  && test_bit(CMDQ_STATE_DCMD_ACTIVE, &ctx->curr_state))
		&& !(!host->card->part_curr && !mmc_card_suspended(host->card)
		     && mmc_host_halt(host))
		&& !(!host->card->part_curr && mmc_host_cq_disable(host) &&
			!mmc_card_suspended(host->card))
		&& !test_bit(CMDQ_STATE_ERR, &ctx->curr_state)
		&& !atomic_read(&host->rpmb_req_pending)
		&& !mmc_check_blk_queue_start_tag(q, mq->cmdq_req_peeked)));
}

static int mmc_cmdq_thread(void *d)
{
	struct mmc_queue *mq = d;
	struct mmc_card *card = mq->card;
	struct mmc_host *host = card->host;

	struct sched_param scheduler_params = {0};

	scheduler_params.sched_priority = 1;

	sched_setscheduler(current, SCHED_FIFO, &scheduler_params);

	current->flags |= PF_MEMALLOC;
	if (card->host->wakeup_on_idle)
		set_wake_up_idle(true);

	while (1) {
		int ret = 0;

		mmc_cmdq_ready_wait(host, mq);
		if (kthread_should_stop())
			break;

		ret = mq->cmdq_issue_fn(mq, mq->cmdq_req_peeked);
		/*
		 * Don't requeue if issue_fn fails.
		 * Recovery will be come by completion softirq
		 * Also we end the request if there is a partition switch error,
		 * so we should not requeue the request here.
		 */
	} /* loop */

	return 0;
}

static int mmc_queue_thread(void *d)
{
	struct mmc_queue *mq = d;
	struct request_queue *q = mq->queue;
	struct mmc_card *card = mq->card;

	current->flags |= PF_MEMALLOC;
	if (card->host->wakeup_on_idle)
		set_wake_up_idle(true);

	down(&mq->thread_sem);
	do {
		struct request *req = NULL;
		unsigned int cmd_flags = 0;

		spin_lock_irq(q->queue_lock);
		set_current_state(TASK_INTERRUPTIBLE);
		req = blk_fetch_request(q);
		mq->mqrq_cur->req = req;
		spin_unlock_irq(q->queue_lock);

		if (req || mq->mqrq_prev->req) {
			set_current_state(TASK_RUNNING);
			cmd_flags = req ? req->cmd_flags : 0;
			mmc_blk_issue_rq(mq, req);
			cond_resched();
			if (test_bit(MMC_QUEUE_NEW_REQUEST, &mq->flags)) {
				clear_bit(MMC_QUEUE_NEW_REQUEST, &mq->flags);
				continue; /* fetch again */
			}

			/*
			 * Current request becomes previous request
			 * and vice versa.
			 * In case of special requests, current request
			 * has been finished. Do not assign it to previous
			 * request.
			 */
			if (cmd_flags & MMC_REQ_SPECIAL_MASK)
				mq->mqrq_cur->req = NULL;

			mq->mqrq_prev->brq.mrq.data = NULL;
			mq->mqrq_prev->req = NULL;
			swap(mq->mqrq_prev, mq->mqrq_cur);
		} else {
			if (kthread_should_stop()) {
				set_current_state(TASK_RUNNING);
				break;
			}
			up(&mq->thread_sem);
			schedule();
			down(&mq->thread_sem);
		}
	} while (1);
	up(&mq->thread_sem);

	return 0;
}

static void mmc_cmdq_dispatch_req(struct request_queue *q)
{
	struct mmc_queue *mq = q->queuedata;

	wake_up(&mq->card->host->cmdq_ctx.wait);
}

/*
 * Generic MMC request handler.  This is called for any queue on a
 * particular host.  When the host is not busy, we look for a request
 * on any queue on this host, and attempt to issue it.  This may
 * not be the queue we were asked to process.
 */
static void mmc_request_fn(struct request_queue *q)
{
	struct mmc_queue *mq = q->queuedata;
	struct request *req;
	unsigned long flags;
	struct mmc_context_info *cntx;

	if (!mq) {
		while ((req = blk_fetch_request(q)) != NULL) {
			req->cmd_flags |= REQ_QUIET;
			__blk_end_request_all(req, -EIO);
		}
		return;
	}

	cntx = &mq->card->host->context_info;
	if (!mq->mqrq_cur->req && mq->mqrq_prev->req) {
		/*
		 * New MMC request arrived when MMC thread may be
		 * blocked on the previous request to be complete
		 * with no current request fetched
		 */
		spin_lock_irqsave(&cntx->lock, flags);
		if (cntx->is_waiting_last_req) {
			cntx->is_new_req = true;
			wake_up_interruptible(&cntx->wait);
		}
		spin_unlock_irqrestore(&cntx->lock, flags);
	} else if (!mq->mqrq_cur->req && !mq->mqrq_prev->req)
		wake_up_process(mq->thread);
}

static struct scatterlist *mmc_alloc_sg(int sg_len, int *err)
{
	struct scatterlist *sg;

	sg = kmalloc(sizeof(struct scatterlist)*sg_len, GFP_KERNEL);
	if (!sg)
		*err = -ENOMEM;
	else {
		*err = 0;
		sg_init_table(sg, sg_len);
	}

	return sg;
}

static void mmc_queue_setup_discard(struct request_queue *q,
				    struct mmc_card *card)
{
	unsigned max_discard;

	max_discard = mmc_calc_max_discard(card);
	if (!max_discard)
		return;

	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, q);
	blk_queue_max_discard_sectors(q, max_discard);
	q->limits.discard_granularity = card->pref_erase << 9;
	/* granularity must not be greater than max. discard */
	if (card->pref_erase > max_discard)
		q->limits.discard_granularity = 0;
	if (mmc_can_secure_erase_trim(card))
		queue_flag_set_unlocked(QUEUE_FLAG_SECDISCARD, q);
}

/**
 * mmc_blk_cmdq_setup_queue
 * @mq: mmc queue
 * @card: card to attach to this queue
 *
 * Setup queue for CMDQ supporting MMC card
 */
void mmc_cmdq_setup_queue(struct mmc_queue *mq, struct mmc_card *card)
{
	struct mmc_host *host = card->host;
	unsigned block_size = 512;

	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, mq->queue);
	if (mmc_can_erase(card))
		mmc_queue_setup_discard(mq->queue, card);

	if (!mmc_dev(host)->dma_mask || !*mmc_dev(host)->dma_mask)
		blk_queue_bounce_limit(mq->queue, BLK_BOUNCE_HIGH);
	blk_queue_max_hw_sectors(mq->queue, min(host->max_blk_count,
						host->max_req_size / 512));
	blk_queue_max_segments(mq->queue, host->max_segs);

	if (mmc_card_mmc(card))
		block_size = card->ext_csd.data_sector_size;

	blk_queue_logical_block_size(mq->queue, block_size);
	blk_queue_max_segment_size(mq->queue,
			round_down(host->max_seg_size, block_size));

	dma_set_max_seg_size(mmc_dev(host), queue_max_segment_size(mq->queue));
}

/**
 * mmc_init_queue - initialise a queue structure.
 * @mq: mmc queue
 * @card: mmc card to attach this queue
 * @lock: queue lock
 * @subname: partition subname
 *
 * Initialise a MMC card request queue.
 */
int mmc_init_queue(struct mmc_queue *mq, struct mmc_card *card,
		   spinlock_t *lock, const char *subname, int area_type)
{
	struct mmc_host *host = card->host;
	u64 limit = BLK_BOUNCE_HIGH;
	int ret;
	struct mmc_queue_req *mqrq_cur = &mq->mqrq[0];
	struct mmc_queue_req *mqrq_prev = &mq->mqrq[1];

	if (mmc_dev(host)->dma_mask && *mmc_dev(host)->dma_mask)
		limit = (u64)dma_max_pfn(mmc_dev(host)) << PAGE_SHIFT;

	mq->card = card;
	if (card->ext_csd.cmdq_support &&
	    (area_type == MMC_BLK_DATA_AREA_MAIN)) {
		mq->queue = blk_init_queue(mmc_cmdq_dispatch_req, lock);
		if (!mq->queue)
			return -ENOMEM;
		mmc_cmdq_setup_queue(mq, card);
		ret = mmc_cmdq_init(mq, card);
		if (ret) {
			pr_err("%s: %d: cmdq: unable to set-up\n",
			       mmc_hostname(card->host), ret);
			blk_cleanup_queue(mq->queue);
		} else {
			sema_init(&mq->thread_sem, 1);
			/* hook for pm qos cmdq init */
			if (card->host->cmdq_ops->init)
				card->host->cmdq_ops->init(card->host);
			mq->queue->queuedata = mq;
			mq->thread = kthread_run(mmc_cmdq_thread, mq,
						 "mmc-cmdqd/%d%s",
						 host->index,
						 subname ? subname : "");
			if (IS_ERR(mq->thread)) {
				pr_err("%s: %d: cmdq: failed to start mmc-cmdqd thread\n",
						mmc_hostname(card->host), ret);
				ret = PTR_ERR(mq->thread);
			}

			return ret;
		}
	}

	mq->queue = blk_init_queue(mmc_request_fn, lock);
	if (!mq->queue)
		return -ENOMEM;

	mq->mqrq_cur = mqrq_cur;
	mq->mqrq_prev = mqrq_prev;
	mq->queue->queuedata = mq;
	mq->num_wr_reqs_to_start_packing =
		min_t(int, (int)card->ext_csd.max_packed_writes,
		     DEFAULT_NUM_REQS_TO_START_PACK);

	blk_queue_prep_rq(mq->queue, mmc_prep_request);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, mq->queue);
	queue_flag_clear_unlocked(QUEUE_FLAG_ADD_RANDOM, mq->queue);
	if (mmc_can_erase(card))
		mmc_queue_setup_discard(mq->queue, card);

#ifdef CONFIG_MMC_BLOCK_BOUNCE
	if (host->max_segs == 1) {
		unsigned int bouncesz;

		bouncesz = MMC_QUEUE_BOUNCESZ;

		if (bouncesz > host->max_req_size)
			bouncesz = host->max_req_size;
		if (bouncesz > host->max_seg_size)
			bouncesz = host->max_seg_size;
		if (bouncesz > (host->max_blk_count * 512))
			bouncesz = host->max_blk_count * 512;

		if (bouncesz > 512) {
			mqrq_cur->bounce_buf = kmalloc(bouncesz, GFP_KERNEL);
			if (!mqrq_cur->bounce_buf) {
				pr_warn("%s: unable to allocate bounce cur buffer\n",
					mmc_card_name(card));
			} else {
				mqrq_prev->bounce_buf =
						kmalloc(bouncesz, GFP_KERNEL);
				if (!mqrq_prev->bounce_buf) {
					pr_warn("%s: unable to allocate bounce prev buffer\n",
						mmc_card_name(card));
					kfree(mqrq_cur->bounce_buf);
					mqrq_cur->bounce_buf = NULL;
				}
			}
		}

		if (mqrq_cur->bounce_buf && mqrq_prev->bounce_buf) {
			blk_queue_bounce_limit(mq->queue, BLK_BOUNCE_ANY);
			blk_queue_max_hw_sectors(mq->queue, bouncesz / 512);
			blk_queue_max_segments(mq->queue, bouncesz / 512);
			blk_queue_max_segment_size(mq->queue, bouncesz);

			mqrq_cur->sg = mmc_alloc_sg(1, &ret);
			if (ret)
				goto cleanup_queue;

			mqrq_cur->bounce_sg =
				mmc_alloc_sg(bouncesz / 512, &ret);
			if (ret)
				goto cleanup_queue;

			mqrq_prev->sg = mmc_alloc_sg(1, &ret);
			if (ret)
				goto cleanup_queue;

			mqrq_prev->bounce_sg =
				mmc_alloc_sg(bouncesz / 512, &ret);
			if (ret)
				goto cleanup_queue;
		}
	}
#endif

	if (!mqrq_cur->bounce_buf && !mqrq_prev->bounce_buf) {
		unsigned int max_segs = host->max_segs;

		blk_queue_bounce_limit(mq->queue, limit);
		blk_queue_max_hw_sectors(mq->queue,
			min(host->max_blk_count, host->max_req_size / 512));
		blk_queue_max_segment_size(mq->queue, host->max_seg_size);
retry:
		blk_queue_max_segments(mq->queue, host->max_segs);

		mqrq_cur->sg = mmc_alloc_sg(host->max_segs, &ret);
		if (ret == -ENOMEM)
			goto cur_sg_alloc_failed;
		else if (ret)
			goto cleanup_queue;

		mqrq_prev->sg = mmc_alloc_sg(host->max_segs, &ret);
		if (ret == -ENOMEM)
			goto prev_sg_alloc_failed;
		else if (ret)
			goto cleanup_queue;

		goto success;

prev_sg_alloc_failed:
		kfree(mqrq_cur->sg);
		mqrq_cur->sg = NULL;
cur_sg_alloc_failed:
		host->max_segs /= 2;
		if (host->max_segs) {
			goto retry;
		} else {
			host->max_segs = max_segs;
			goto cleanup_queue;
		}
	}

success:
	sema_init(&mq->thread_sem, 1);

	/* hook for pm qos legacy init */
	if (card->host->ops->init)
		card->host->ops->init(card->host);

	mq->thread = kthread_run(mmc_queue_thread, mq, "mmcqd/%d%s",
		host->index, subname ? subname : "");

	if (IS_ERR(mq->thread)) {
		ret = PTR_ERR(mq->thread);
		goto free_bounce_sg;
	}

	return 0;
 free_bounce_sg:
	kfree(mqrq_cur->bounce_sg);
	mqrq_cur->bounce_sg = NULL;
	kfree(mqrq_prev->bounce_sg);
	mqrq_prev->bounce_sg = NULL;

 cleanup_queue:
	kfree(mqrq_cur->sg);
	mqrq_cur->sg = NULL;
	kfree(mqrq_cur->bounce_buf);
	mqrq_cur->bounce_buf = NULL;

	kfree(mqrq_prev->sg);
	mqrq_prev->sg = NULL;
	kfree(mqrq_prev->bounce_buf);
	mqrq_prev->bounce_buf = NULL;

	blk_cleanup_queue(mq->queue);
	return ret;
}

void mmc_cleanup_queue(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;
	struct mmc_queue_req *mqrq_cur = mq->mqrq_cur;
	struct mmc_queue_req *mqrq_prev = mq->mqrq_prev;

	/* Make sure the queue isn't suspended, as that will deadlock */
	mmc_queue_resume(mq);

	/* Then terminate our worker thread */
	kthread_stop(mq->thread);

	/* Empty the queue */
	spin_lock_irqsave(q->queue_lock, flags);
	q->queuedata = NULL;
	blk_start_queue(q);
	spin_unlock_irqrestore(q->queue_lock, flags);

	kfree(mqrq_cur->bounce_sg);
	mqrq_cur->bounce_sg = NULL;

	kfree(mqrq_cur->sg);
	mqrq_cur->sg = NULL;

	kfree(mqrq_cur->bounce_buf);
	mqrq_cur->bounce_buf = NULL;

	kfree(mqrq_prev->bounce_sg);
	mqrq_prev->bounce_sg = NULL;

	kfree(mqrq_prev->sg);
	mqrq_prev->sg = NULL;

	kfree(mqrq_prev->bounce_buf);
	mqrq_prev->bounce_buf = NULL;

	mq->card = NULL;
}

int mmc_packed_init(struct mmc_queue *mq, struct mmc_card *card)
{
	struct mmc_queue_req *mqrq_cur = &mq->mqrq[0];
	struct mmc_queue_req *mqrq_prev = &mq->mqrq[1];
	int ret = 0;


	mqrq_cur->packed = kzalloc(sizeof(struct mmc_packed), GFP_KERNEL);
	if (!mqrq_cur->packed) {
		pr_warn("%s: unable to allocate packed cmd for mqrq_cur\n",
			mmc_card_name(card));
		ret = -ENOMEM;
		goto out;
	}

	mqrq_prev->packed = kzalloc(sizeof(struct mmc_packed), GFP_KERNEL);
	if (!mqrq_prev->packed) {
		pr_warn("%s: unable to allocate packed cmd for mqrq_prev\n",
			mmc_card_name(card));
		kfree(mqrq_cur->packed);
		mqrq_cur->packed = NULL;
		ret = -ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&mqrq_cur->packed->list);
	INIT_LIST_HEAD(&mqrq_prev->packed->list);

out:
	return ret;
}

void mmc_packed_clean(struct mmc_queue *mq)
{
	struct mmc_queue_req *mqrq_cur = &mq->mqrq[0];
	struct mmc_queue_req *mqrq_prev = &mq->mqrq[1];

	kfree(mqrq_cur->packed);
	mqrq_cur->packed = NULL;
	kfree(mqrq_prev->packed);
	mqrq_prev->packed = NULL;
}

static void mmc_cmdq_softirq_done(struct request *rq)
{
	struct mmc_queue *mq = rq->q->queuedata;
	mq->cmdq_complete_fn(rq);
}

static void mmc_cmdq_error_work(struct work_struct *work)
{
	struct mmc_queue *mq = container_of(work, struct mmc_queue,
					    cmdq_err_work);

	mq->cmdq_error_fn(mq);
}

enum blk_eh_timer_return mmc_cmdq_rq_timed_out(struct request *req)
{
	struct mmc_queue *mq = req->q->queuedata;

	pr_err("%s: request with tag: %d flags: 0x%llx timed out\n",
	       mmc_hostname(mq->card->host), req->tag, req->cmd_flags);

	return mq->cmdq_req_timed_out(req);
}

int mmc_cmdq_init(struct mmc_queue *mq, struct mmc_card *card)
{
	int i, ret = 0;
	/* one slot is reserved for dcmd requests */
	int q_depth = card->ext_csd.cmdq_depth - 1;

	card->cmdq_init = false;
	if (!(card->host->caps2 & MMC_CAP2_CMD_QUEUE)) {
		ret = -ENOTSUPP;
		goto out;
	}

	init_waitqueue_head(&card->host->cmdq_ctx.queue_empty_wq);
	init_waitqueue_head(&card->host->cmdq_ctx.wait);

	mq->mqrq_cmdq = kzalloc(
			sizeof(struct mmc_queue_req) * q_depth, GFP_KERNEL);
	if (!mq->mqrq_cmdq) {
		pr_warn("%s: unable to allocate mqrq's for q_depth %d\n",
			mmc_card_name(card), q_depth);
		ret = -ENOMEM;
		goto out;
	}

	/* sg is allocated for data request slots only */
	for (i = 0; i < q_depth; i++) {
		mq->mqrq_cmdq[i].sg = mmc_alloc_sg(card->host->max_segs, &ret);
		if (ret) {
			pr_warn("%s: unable to allocate cmdq sg of size %d\n",
				mmc_card_name(card),
				card->host->max_segs);
			goto free_mqrq_sg;
		}
	}

	ret = blk_queue_init_tags(mq->queue, q_depth, NULL, BLK_TAG_ALLOC_FIFO);
	if (ret) {
		pr_warn("%s: unable to allocate cmdq tags %d\n",
				mmc_card_name(card), q_depth);
		goto free_mqrq_sg;
	}

	blk_queue_softirq_done(mq->queue, mmc_cmdq_softirq_done);
	INIT_WORK(&mq->cmdq_err_work, mmc_cmdq_error_work);
	init_completion(&mq->cmdq_shutdown_complete);
	init_completion(&mq->cmdq_pending_req_done);

	blk_queue_rq_timed_out(mq->queue, mmc_cmdq_rq_timed_out);
	blk_queue_rq_timeout(mq->queue, 120 * HZ);
	card->cmdq_init = true;

	goto out;

free_mqrq_sg:
	for (i = 0; i < q_depth; i++)
		kfree(mq->mqrq_cmdq[i].sg);
	kfree(mq->mqrq_cmdq);
	mq->mqrq_cmdq = NULL;
out:
	return ret;
}

void mmc_cmdq_clean(struct mmc_queue *mq, struct mmc_card *card)
{
	int i;
	int q_depth = card->ext_csd.cmdq_depth - 1;

	blk_free_tags(mq->queue->queue_tags);
	mq->queue->queue_tags = NULL;
	blk_queue_free_tags(mq->queue);

	for (i = 0; i < q_depth; i++)
		kfree(mq->mqrq_cmdq[i].sg);
	kfree(mq->mqrq_cmdq);
	mq->mqrq_cmdq = NULL;
}

/**
 * mmc_queue_suspend - suspend a MMC request queue
 * @mq: MMC queue to suspend
 * @wait: Wait till MMC request queue is empty
 *
 * Stop the block request queue, and wait for our thread to
 * complete any outstanding requests.  This ensures that we
 * won't suspend while a request is being processed.
 */
int mmc_queue_suspend(struct mmc_queue *mq, int wait)
{
	struct request_queue *q = mq->queue;
	unsigned long flags;
	int rc = 0;
	struct mmc_card *card = mq->card;
	struct request *req;

	if (card->cmdq_init && blk_queue_tagged(q)) {
		struct mmc_host *host = card->host;

		if (test_and_set_bit(MMC_QUEUE_SUSPENDED, &mq->flags))
			goto out;

		if (wait) {

			/*
			 * After blk_cleanup_queue is called, wait for all
			 * active_reqs to complete.
			 * Then wait for cmdq thread to exit before calling
			 * cmdq shutdown to avoid race between issuing
			 * requests and shutdown of cmdq.
			 */
			blk_cleanup_queue(q);

			if (host->cmdq_ctx.active_reqs)
				wait_for_completion(
						&mq->cmdq_shutdown_complete);
			kthread_stop(mq->thread);
			mq->cmdq_shutdown(mq);
		} else {
			spin_lock_irqsave(q->queue_lock, flags);
			blk_stop_queue(q);
			wake_up(&host->cmdq_ctx.wait);
			req = blk_peek_request(q);
			if (req || mq->cmdq_req_peeked ||
			    host->cmdq_ctx.active_reqs) {
				clear_bit(MMC_QUEUE_SUSPENDED, &mq->flags);
				blk_start_queue(q);
				rc = -EBUSY;
			}
			spin_unlock_irqrestore(q->queue_lock, flags);
		}

		goto out;
	}

	if (!(test_and_set_bit(MMC_QUEUE_SUSPENDED, &mq->flags))) {
		if (!wait) {
			/* suspend/stop the queue in case of suspend */
			spin_lock_irqsave(q->queue_lock, flags);
			blk_stop_queue(q);
			spin_unlock_irqrestore(q->queue_lock, flags);
		} else {
			/* shutdown the queue in case of shutdown/reboot */
			blk_cleanup_queue(q);
		}

		rc = down_trylock(&mq->thread_sem);
		if (rc && !wait) {
			/*
			 * Failed to take the lock so better to abort the
			 * suspend because mmcqd thread is processing requests.
			 */
			clear_bit(MMC_QUEUE_SUSPENDED, &mq->flags);
			spin_lock_irqsave(q->queue_lock, flags);
			blk_start_queue(q);
			spin_unlock_irqrestore(q->queue_lock, flags);
			rc = -EBUSY;
		} else if (rc && wait) {
			down(&mq->thread_sem);
			rc = 0;
		}
	}
out:
	return rc;
}

/**
 * mmc_queue_resume - resume a previously suspended MMC request queue
 * @mq: MMC queue to resume
 */
void mmc_queue_resume(struct mmc_queue *mq)
{
	struct request_queue *q = mq->queue;
	struct mmc_card *card = mq->card;
	unsigned long flags;

	if (test_and_clear_bit(MMC_QUEUE_SUSPENDED, &mq->flags)) {

		if (!(card->cmdq_init && blk_queue_tagged(q)))
			up(&mq->thread_sem);

		spin_lock_irqsave(q->queue_lock, flags);
		blk_start_queue(q);
		spin_unlock_irqrestore(q->queue_lock, flags);
	}
}

static unsigned int mmc_queue_packed_map_sg(struct mmc_queue *mq,
					    struct mmc_packed *packed,
					    struct scatterlist *sg,
					    enum mmc_packed_type cmd_type)
{
	struct scatterlist *__sg = sg;
	unsigned int sg_len = 0;
	struct request *req;

	if (mmc_packed_wr(cmd_type)) {
		unsigned int hdr_sz = mmc_large_sector(mq->card) ? 4096 : 512;
		unsigned int max_seg_sz = queue_max_segment_size(mq->queue);
		unsigned int len, remain, offset = 0;
		u8 *buf = (u8 *)packed->cmd_hdr;

		remain = hdr_sz;
		do {
			len = min(remain, max_seg_sz);
			sg_set_buf(__sg, buf + offset, len);
			offset += len;
			remain -= len;
			sg_unmark_end(__sg++);
			sg_len++;
		} while (remain);
	}

	list_for_each_entry(req, &packed->list, queuelist) {
		sg_len += blk_rq_map_sg(mq->queue, req, __sg);
		__sg = sg + (sg_len - 1);
		sg_unmark_end(__sg++);
	}
	sg_mark_end(sg + (sg_len - 1));
	return sg_len;
}

/*
 * Prepare the sg list(s) to be handed of to the host driver
 */
unsigned int mmc_queue_map_sg(struct mmc_queue *mq, struct mmc_queue_req *mqrq)
{
	unsigned int sg_len;
	size_t buflen;
	struct scatterlist *sg;
	enum mmc_packed_type cmd_type;
	int i;

	cmd_type = mqrq->cmd_type;

	if (!mqrq->bounce_buf) {
		if (mmc_packed_cmd(cmd_type))
			return mmc_queue_packed_map_sg(mq, mqrq->packed,
						       mqrq->sg, cmd_type);
		else
			return blk_rq_map_sg(mq->queue, mqrq->req, mqrq->sg);
	}

	BUG_ON(!mqrq->bounce_sg);

	if (mmc_packed_cmd(cmd_type))
		sg_len = mmc_queue_packed_map_sg(mq, mqrq->packed,
						 mqrq->bounce_sg, cmd_type);
	else
		sg_len = blk_rq_map_sg(mq->queue, mqrq->req, mqrq->bounce_sg);

	mqrq->bounce_sg_len = sg_len;

	buflen = 0;
	for_each_sg(mqrq->bounce_sg, sg, sg_len, i)
		buflen += sg->length;

	sg_init_one(mqrq->sg, mqrq->bounce_buf, buflen);

	return 1;
}

/*
 * If writing, bounce the data to the buffer before the request
 * is sent to the host driver
 */
void mmc_queue_bounce_pre(struct mmc_queue_req *mqrq)
{
	if (!mqrq->bounce_buf)
		return;

	if (rq_data_dir(mqrq->req) != WRITE)
		return;

	sg_copy_to_buffer(mqrq->bounce_sg, mqrq->bounce_sg_len,
		mqrq->bounce_buf, mqrq->sg[0].length);
}

/*
 * If reading, bounce the data from the buffer after the request
 * has been handled by the host driver
 */
void mmc_queue_bounce_post(struct mmc_queue_req *mqrq)
{
	if (!mqrq->bounce_buf)
		return;

	if (rq_data_dir(mqrq->req) != READ)
		return;

	sg_copy_from_buffer(mqrq->bounce_sg, mqrq->bounce_sg_len,
		mqrq->bounce_buf, mqrq->sg[0].length);
}
