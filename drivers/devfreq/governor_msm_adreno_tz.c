/* Copyright (c) 2010-2019, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/devfreq.h>
#include <linux/math64.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/ftrace.h>
#include <linux/mdss_refresh_rate.h>
#include <linux/mm.h>
#include <linux/msm_adreno_devfreq.h>
#include <asm/cacheflush.h>
#include <soc/qcom/scm.h>
#include "governor.h"

static DEFINE_SPINLOCK(tz_lock);
static DEFINE_SPINLOCK(sample_lock);
static DEFINE_SPINLOCK(suspend_lock);
/*
 * FLOOR is 5msec to capture up to 3 re-draws
 * per frame for 60fps content.
 */
#define FLOOR		        5000
/*
 * MIN_BUSY is 1 msec for the sample to be sent
 */
#define MIN_BUSY		1000
#define MAX_TZ_VERSION		0

/*
 * CEILING is 50msec, larger than any standard
 * frame length, but less than the idle timer.
 */
#define CEILING			50000
#define TZ_RESET_ID		0x3
#define TZ_UPDATE_ID		0x4
#define TZ_INIT_ID		0x6

#define TZ_RESET_ID_64          0x7
#define TZ_UPDATE_ID_64         0x8
#define TZ_INIT_ID_64           0x9

#define TZ_V2_UPDATE_ID_64         0xA
#define TZ_V2_INIT_ID_64           0xB
#define TZ_V2_INIT_CA_ID_64        0xC
#define TZ_V2_UPDATE_WITH_CA_ID_64 0xD

#define TAG "msm_adreno_tz: "

static u64 suspend_time;
static u64 suspend_start;
static unsigned int adrenoboost = 0;
static unsigned long acc_total, acc_relative_busy;

/*
 * Returns GPU suspend time in millisecond.
 */
u64 suspend_time_ms(void)
{
	u64 suspend_sampling_time;
	u64 time_diff = 0;

	if (suspend_start == 0)
		return 0;

	suspend_sampling_time = (u64)ktime_to_ms(ktime_get());
	time_diff = suspend_sampling_time - suspend_start;
	/* Update the suspend_start sample again */
	suspend_start = suspend_sampling_time;
	return time_diff;
}

static ssize_t adrenoboost_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;
	count += sprintf(buf, "%d\n", adrenoboost);

	return count;
}

static ssize_t adrenoboost_save(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int input;

	sscanf(buf, "%d ", &input);

	if (input > 3)
		adrenoboost = 0;
	else
		adrenoboost = input;

	return count;
}

static ssize_t gpu_load_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	unsigned long sysfs_busy_perc = 0;
	/*
	 * Average out the samples taken since last read
	 * This will keep the average value in sync with
	 * with the client sampling duration.
	 */
	spin_lock(&sample_lock);
	if (acc_total)
		sysfs_busy_perc = (acc_relative_busy * 100) / acc_total;

	/* Reset the parameters */
	acc_total = 0;
	acc_relative_busy = 0;
	spin_unlock(&sample_lock);
	return snprintf(buf, PAGE_SIZE, "%lu\n", sysfs_busy_perc);
}

/*
 * Returns the time in ms for which gpu was in suspend state
 * since last time the entry is read.
 */
static ssize_t suspend_time_show(struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	u64 time_diff = 0;

	spin_lock(&suspend_lock);
	time_diff = suspend_time_ms();
	/*
	 * Adding the previous suspend time also as the gpu
	 * can go and come out of suspend states in between
	 * reads also and we should have the total suspend
	 * since last read.
	 */
	time_diff += suspend_time;
	suspend_time = 0;
	spin_unlock(&suspend_lock);

	return snprintf(buf, PAGE_SIZE, "%llu\n", time_diff);
}

static DEVICE_ATTR(adrenoboost, 0644,
		adrenoboost_show,
		adrenoboost_save);

static DEVICE_ATTR(gpu_load, 0444, gpu_load_show, NULL);

static DEVICE_ATTR(suspend_time, 0444,
		suspend_time_show,
		NULL);

static const struct device_attribute *adreno_tz_attr_list[] = {
		&dev_attr_adrenoboost,
		&dev_attr_gpu_load,
		&dev_attr_suspend_time,
		NULL
};

void compute_work_load(struct devfreq_dev_status *stats,
		struct devfreq_msm_adreno_tz_data *priv,
		struct devfreq *devfreq)
{
	u64 busy;

	spin_lock(&sample_lock);
	/*
	 * Keep collecting the stats till the client
	 * reads it. Average of all samples and reset
	 * is done when the entry is read
	 */
	acc_total += stats->total_time;
	busy = (u64)stats->busy_time * stats->current_frequency;
	do_div(busy, devfreq->profile->freq_table[0]);
	acc_relative_busy += busy;

	spin_unlock(&sample_lock);
}

/* Trap into the TrustZone, and call funcs there. */
static int __secure_tz_reset_entry2(unsigned int *scm_data, u32 size_scm_data,
					bool is_64)
{
	int ret;
	/* sync memory before sending the commands to tz */
	__iowmb();

	if (!is_64) {
		spin_lock(&tz_lock);
		ret = scm_call_atomic2(SCM_SVC_IO, TZ_RESET_ID, scm_data[0],
					scm_data[1]);
		spin_unlock(&tz_lock);
	} else {
		if (is_scm_armv8()) {
			struct scm_desc desc = {0};
			desc.arginfo = 0;
			ret = scm_call2(SCM_SIP_FNID(SCM_SVC_DCVS,
					 TZ_RESET_ID_64), &desc);
		} else {
			ret = scm_call(SCM_SVC_DCVS, TZ_RESET_ID_64, scm_data,
				size_scm_data, NULL, 0);
		}
	}
	return ret;
}

static int __secure_tz_update_entry3(unsigned int *scm_data, u32 size_scm_data,
		int *val, u32 size_val, struct devfreq_msm_adreno_tz_data *priv)
{
	int ret;
	/* sync memory before sending the commands to tz */
	__iowmb();

	if (!priv->is_64) {
		spin_lock(&tz_lock);
		ret = scm_call_atomic3(SCM_SVC_IO, TZ_UPDATE_ID,
					scm_data[0], scm_data[1], scm_data[2]);
		spin_unlock(&tz_lock);
		*val = ret;
	} else {
		if (is_scm_armv8()) {
			unsigned int cmd_id;
			struct scm_desc desc = {0};
			desc.args[0] = scm_data[0];
			desc.args[1] = scm_data[1];
			desc.args[2] = scm_data[2];

			if (!priv->ctxt_aware_enable) {
				desc.arginfo = SCM_ARGS(3);
				cmd_id =  TZ_V2_UPDATE_ID_64;
			} else {
				/* Add context count infomration to update*/
				desc.args[3] = scm_data[3];
				desc.arginfo = SCM_ARGS(4);
				cmd_id =  TZ_V2_UPDATE_WITH_CA_ID_64;
			}
			ret = scm_call2(SCM_SIP_FNID(SCM_SVC_DCVS, cmd_id),
						&desc);
			*val = desc.ret[0];
		} else {
			ret = scm_call(SCM_SVC_DCVS, TZ_UPDATE_ID_64, scm_data,
				size_scm_data, val, size_val);
		}
	}
	return ret;
}

static int tz_init_ca(struct devfreq_msm_adreno_tz_data *priv)
{
	unsigned int tz_ca_data[2];
	struct scm_desc desc = {0};
	u8 *tz_buf;
	int ret;

	/* Set data for TZ */
	tz_ca_data[0] = priv->bin.ctxt_aware_target_pwrlevel;
	tz_ca_data[1] = priv->bin.ctxt_aware_busy_penalty;

	tz_buf = kzalloc(PAGE_ALIGN(sizeof(tz_ca_data)), GFP_KERNEL);
	if (!tz_buf)
		return -ENOMEM;

	memcpy(tz_buf, tz_ca_data, sizeof(tz_ca_data));
	/* Ensure memcpy completes execution */
	mb();
	dmac_flush_range(tz_buf,
		tz_buf + PAGE_ALIGN(sizeof(tz_ca_data)));

	desc.args[0] = virt_to_phys(tz_buf);
	desc.args[1] = sizeof(tz_ca_data);
	desc.arginfo = SCM_ARGS(2, SCM_RW, SCM_VAL);

	ret = scm_call2(SCM_SIP_FNID(SCM_SVC_DCVS,
			TZ_V2_INIT_CA_ID_64),
			&desc);

	kzfree(tz_buf);

	return ret;
}

static int tz_init(struct devfreq_msm_adreno_tz_data *priv,
			unsigned int *tz_pwrlevels, u32 size_pwrlevels,
			unsigned int *version, u32 size_version)
{
	int ret;
	/* Make sure all CMD IDs are avaialble */
	if (scm_is_call_available(SCM_SVC_DCVS, TZ_INIT_ID)) {
		ret = scm_call(SCM_SVC_DCVS, TZ_INIT_ID, tz_pwrlevels,
				size_pwrlevels, NULL, 0);
		*version = 0;

	} else if (scm_is_call_available(SCM_SVC_DCVS, TZ_INIT_ID_64) &&
			scm_is_call_available(SCM_SVC_DCVS, TZ_UPDATE_ID_64) &&
			scm_is_call_available(SCM_SVC_DCVS, TZ_RESET_ID_64)) {
		struct scm_desc desc = {0};
		u8 *tz_buf;

		if (!is_scm_armv8()) {
			ret = scm_call(SCM_SVC_DCVS, TZ_INIT_ID_64,
				       tz_pwrlevels, size_pwrlevels,
				       version, size_version);
			if (!ret)
				priv->is_64 = true;
			return ret;
		}

		tz_buf = kzalloc(PAGE_ALIGN(size_pwrlevels), GFP_KERNEL);
		if (!tz_buf)
			return -ENOMEM;
		memcpy(tz_buf, tz_pwrlevels, size_pwrlevels);
		/* Ensure memcpy completes execution */
		mb();
		dmac_flush_range(tz_buf, tz_buf + PAGE_ALIGN(size_pwrlevels));

		desc.args[0] = virt_to_phys(tz_buf);
		desc.args[1] = size_pwrlevels;
		desc.arginfo = SCM_ARGS(2, SCM_RW, SCM_VAL);

		ret = scm_call2(SCM_SIP_FNID(SCM_SVC_DCVS, TZ_V2_INIT_ID_64),
				&desc);
		*version = desc.ret[0];
		if (!ret)
			priv->is_64 = true;
		kzfree(tz_buf);
	} else
		ret = -EINVAL;

	 /* Initialize context aware feature, if enabled. */
	if (!ret && priv->ctxt_aware_enable) {
		if (priv->is_64 &&
			(scm_is_call_available(SCM_SVC_DCVS,
				TZ_V2_INIT_CA_ID_64)) &&
			(scm_is_call_available(SCM_SVC_DCVS,
				TZ_V2_UPDATE_WITH_CA_ID_64))) {
			ret = tz_init_ca(priv);
			/*
			 * If context aware feature intialization fails,
			 * just print an error message and return
			 * success as normal DCVS will still work.
			 */
			if (ret) {
				pr_err(TAG "tz: context aware DCVS init failed\n");
				priv->ctxt_aware_enable = false;
				return 0;
			}
		} else {
			pr_warn(TAG "tz: context aware DCVS not supported\n");
			priv->ctxt_aware_enable = false;
		}
	}

	return ret;
}

#ifdef CONFIG_ADRENO_IDLER
extern int adreno_idler(struct devfreq_dev_status stats, struct devfreq *devfreq,
		 unsigned long *freq);
#endif

static int adrenoboost_debug(struct devfreq *devfreq, unsigned long *freq,
			    int jump_dir);

/*
 * Mapping gpu level calculated linear
 * conservation half curve values into a
 * bell curve of conservation.
 */
static int conservation_map_up[] = { 15, 15, 10, 4, 5, 6, 12, 5, 5, 5 };
static int conservation_map_down[] = { 0, 1, 6, 6, 5, 0, 0, 5, 5, 5 };

/* 
 * Make boost multiplication/division
 * depending on current lvl, dampen the
 * high freq up scaling.
 */
static int lvl_multiplicator_map_1[] = { 5, 5, 6, 8, 9, 1, 1, 1, 1 };
static int lvl_divider_map_1[] = { 10, 10, 10, 10, 10, 1, 1, 1, 1 };

static int lvl_multiplicator_map_2[] = { 9, 1, 1, 1, 1, 10, 8, 1, 1 };
static int lvl_divider_map_2[] = { 10, 1, 1, 1, 1, 14, 12, 1, 1 };

static int lvl_multiplicator_map_3[] = { 10, 1, 1, 1, 1, 11, 9, 1, 1 };
static int lvl_divider_map_3[] = { 10, 1, 1, 1, 1, 15, 13, 1, 1 };

static int tz_get_target_freq(struct devfreq *devfreq, unsigned long *freq,
								u32 *flag)
{
	int result = 0;
	struct devfreq_msm_adreno_tz_data *priv = devfreq->data;
	struct devfreq_dev_status stats;
	int val, level = 0;
	unsigned int scm_data[4];
	int context_count = 0;
	int last_level = priv->bin.last_level;

	/* keeps stats.private_data == NULL   */
	result = devfreq->profile->get_dev_status(devfreq->dev.parent, &stats);
	if (result) {
		pr_err(TAG "get_status failed %d\n", result);
		return result;
	}

	/* Prevent overflow */
	if (stats.busy_time >= (1 << 24) || stats.total_time >= (1 << 24)) {
		stats.busy_time >>= 7;
		stats.total_time >>= 7;
	}

	*freq = stats.current_frequency;
#ifdef CONFIG_ADRENO_IDLER
	if (adreno_idler(stats, devfreq, freq)) {
		/* adreno_idler has asked to bail out now */
		return 0;
	}
#endif
	priv->bin.total_time += stats.total_time;
	
	/*
	 * Scale busy time up based on adrenoboost
	 * parameter, only if MIN_BUSY exceeded...
	 */
	switch (adrenoboost) {
		case 1:
			priv->bin.busy_time += (unsigned int)
				((stats.busy_time * (1 + adrenoboost) *
				lvl_multiplicator_map_1[last_level]) /
				lvl_divider_map_1[last_level]);
			break;
		case 2:
			priv->bin.busy_time += (unsigned int)
				((stats.busy_time * (1 + adrenoboost) *
				lvl_multiplicator_map_2[last_level]  * 7 ) /
				(lvl_divider_map_2[last_level] * 10));
			break;
		case 3:
			priv->bin.busy_time += (unsigned int)
				((stats.busy_time * (1 + adrenoboost) *
				lvl_multiplicator_map_3[last_level] * 8 ) /
				(lvl_divider_map_3[last_level] * 10));
			break;
		default:
			priv->bin.busy_time += stats.busy_time;
			break;
	}

	if (stats.private_data)
		context_count =  *((int *)stats.private_data);

	/* Update the GPU load statistics */
	compute_work_load(&stats, priv, devfreq);
	/*
	 * Do not waste CPU cycles running this algorithm if
	 * the GPU just started, or if less than FLOOR time
	 * has passed since the last run or the gpu hasn't been
	 * busier than MIN_BUSY.
	 */
	if ((stats.total_time == 0) ||
		(priv->bin.total_time < FLOOR) ||
		(unsigned int) priv->bin.busy_time < MIN_BUSY) {
		return 0;
	}

	level = devfreq_get_freq_level(devfreq, stats.current_frequency);
	if (level < 0) {
		pr_err(TAG "bad freq %ld\n", stats.current_frequency);
		return level;
	}

	/*
	 * If there is an extended block of busy processing,
	 * increase frequency.  Otherwise run the normal algorithm.
	 */
	if (!priv->disable_busy_time_burst &&
			priv->bin.busy_time > CEILING) {
		val = -1 * level;
	} else {
		unsigned int refresh_rate = dsi_panel_get_refresh_rate();

		scm_data[0] = level;
		scm_data[1] = priv->bin.total_time;
		if (refresh_rate > 60)
			scm_data[2] = priv->bin.busy_time * refresh_rate / 60;
		else
			scm_data[2] = priv->bin.busy_time;
		scm_data[3] = context_count;
		__secure_tz_update_entry3(scm_data, sizeof(scm_data),
					&val, sizeof(val), priv);
	}

	/*
	 * If the decision is to move to a different level, make sure the GPU
	 * frequency changes.
	 */
	if (!adrenoboost && val) {
		level += val;
		level = max(level, 0);
		level = min_t(int, level, devfreq->profile->max_state - 1);
		adrenoboost_debug(devfreq, freq, 0);
		priv->bin.last_level = level;
	} else {
		if (val) {
			priv->bin.cycles_keeping_level += 1 + abs(val / 2);

			/*
			 * Higher value change quantity means more
			 * addition to cycles_keeping_level for easier switching.
			 * going upwards in frequency. Make it harder on the low
			 * and high freqs, middle ground.
			 */
			if (val < 0 && priv->bin.cycles_keeping_level < conservation_map_up[ last_level ]) {
				adrenoboost_debug(devfreq, freq, 1);
			} else {
				/*
			 	 * Going downwards in frequency let it happen hard in
			 	 * the middle freqs
			 	 */
				if (val > 0 && priv->bin.cycles_keeping_level < conservation_map_down[ last_level ]) {
					adrenoboost_debug(devfreq, freq, 2);
				} else {
					level += val;
					level = max(level, 0);
					level = min_t(int, level, devfreq->profile->max_state - 1);
					/* Reset keep cylcles timer */
					priv->bin.cycles_keeping_level = 0;
					/* Set new last level */
					priv->bin.last_level = level;
					adrenoboost_debug(devfreq, freq, 0);
				}
			}
		}
	}
	priv->bin.total_time = 0;
	priv->bin.busy_time = 0;

	*freq = devfreq->profile->freq_table[level];
	return 0;
}

static int adrenoboost_debug(struct devfreq *devfreq, unsigned long *freq,
			    int jump_dir)
{
	struct devfreq_msm_adreno_tz_data *priv = devfreq->data;
	struct devfreq_dev_status stats;
	int level = 0;

	if (!jump_dir)
		pr_debug("adrenoboost jumping\n");
	else if (jump_dir == 1)
		pr_debug("adrenoboost not jumping UP\n");
	else
		pr_debug("adrenoboost not jumping DOWN\n");

	pr_debug("level = %d last_level = %d total=%d busy=%d original busy_time=%d\n",
			level, priv->bin.last_level,
			(int)priv->bin.total_time,
			(int)priv->bin.busy_time, (int)stats.busy_time);
	return 0;
}

static int tz_start(struct devfreq *devfreq)
{
	struct devfreq_msm_adreno_tz_data *priv;
	unsigned int tz_pwrlevels[MSM_ADRENO_MAX_PWRLEVELS + 1];
	int i, out, ret;
	unsigned int version;

	struct msm_adreno_extended_profile *gpu_profile = container_of(
					(devfreq->profile),
					struct msm_adreno_extended_profile,
					profile);

	/*
	 * Assuming that we have only one instance of the adreno device
	 * connected to this governor,
	 * can safely restore the pointer to the governor private data
	 * from the container of the device profile
	 */
	devfreq->data = gpu_profile->private_data;

	priv = devfreq->data;

	out = 1;
	if (devfreq->profile->max_state < MSM_ADRENO_MAX_PWRLEVELS) {
		for (i = 0; i < devfreq->profile->max_state; i++)
			tz_pwrlevels[out++] = devfreq->profile->freq_table[i];
		tz_pwrlevels[0] = i;
	} else {
		pr_err(TAG "tz_pwrlevels[] is too short\n");
		return -EINVAL;
	}

	ret = tz_init(priv, tz_pwrlevels, sizeof(tz_pwrlevels), &version,
				sizeof(version));
	if (ret != 0 || version > MAX_TZ_VERSION) {
		pr_err(TAG "tz_init failed\n");
		return ret;
	}

	for (i = 0; adreno_tz_attr_list[i] != NULL; i++)
		device_create_file(&devfreq->dev, adreno_tz_attr_list[i]);

	priv->bin.last_level = devfreq->profile->max_state - 1;

	return 0;
}

static int tz_stop(struct devfreq *devfreq)
{
	int i;

	for (i = 0; adreno_tz_attr_list[i] != NULL; i++)
		device_remove_file(&devfreq->dev, adreno_tz_attr_list[i]);

	/* leaving the governor and cleaning the pointer to private data */
	devfreq->data = NULL;
	return 0;
}

static int tz_suspend(struct devfreq *devfreq)
{
	struct devfreq_msm_adreno_tz_data *priv = devfreq->data;
	unsigned int scm_data[2] = {0, 0};
	__secure_tz_reset_entry2(scm_data, sizeof(scm_data), priv->is_64);

	priv->bin.total_time = 0;
	priv->bin.busy_time = 0;
	return 0;
}

static int tz_handler(struct devfreq *devfreq, unsigned int event, void *data)
{
	int result;

	switch (event) {
	case DEVFREQ_GOV_START:
		result = tz_start(devfreq);
		break;

	case DEVFREQ_GOV_STOP:
		spin_lock(&suspend_lock);
		suspend_start = 0;
		spin_unlock(&suspend_lock);
		result = tz_stop(devfreq);
		break;

	case DEVFREQ_GOV_SUSPEND:
		result = tz_suspend(devfreq);
		if (!result) {
			spin_lock(&suspend_lock);
			/* Collect the start sample for suspend time */
			suspend_start = (u64)ktime_to_ms(ktime_get());
			spin_unlock(&suspend_lock);
		}
		break;

	case DEVFREQ_GOV_RESUME:
		spin_lock(&suspend_lock);
		suspend_time += suspend_time_ms();
		/* Reset the suspend_start when gpu resumes */
		suspend_start = 0;
		spin_unlock(&suspend_lock);

	case DEVFREQ_GOV_INTERVAL:
		/* ignored, this governor doesn't use polling */
	default:
		result = 0;
		break;
	}

	return result;
}


static struct devfreq_governor msm_adreno_tz = {
	.name = "msm-adreno-tz",
	.get_target_freq = tz_get_target_freq,
	.event_handler = tz_handler,
};

static int __init msm_adreno_tz_init(void)
{
	return devfreq_add_governor(&msm_adreno_tz);
}
subsys_initcall(msm_adreno_tz_init);

static void __exit msm_adreno_tz_exit(void)
{
	int ret = devfreq_remove_governor(&msm_adreno_tz);
	if (ret)
		pr_err(TAG "failed to remove governor %d\n", ret);

}

module_exit(msm_adreno_tz_exit);

MODULE_LICENSE("GPLv2");
