/* Copyright (c) 2012-2019, The Linux Foundation. All rights reserved.
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

#define pr_fmt(fmt)	"%s: " fmt, __func__

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/msm_mdp.h>
#include <linux/memblock.h>
#include <linux/sort.h>
#include <linux/kmemleak.h>
#include <sw_sync.h>

#include <soc/qcom/event_timer.h>
#include <linux/msm-bus.h>
#include "mdss.h"
#include "mdss_debug.h"
#include "mdss_fb.h"
#include "mdss_mdp.h"
#include "mdss_smmu.h"
#include "mdss_mdp_wfd.h"
#include "mdss_dsi_clk.h"

#define VSYNC_PERIOD 16
#define BORDERFILL_NDX	0x0BF000BF
#define CHECK_BOUNDS(offset, size, max_size) \
	(((size) > (max_size)) || ((offset) > ((max_size) - (size))))

#define IS_RIGHT_MIXER_OV(flags, dst_x, left_lm_w)	\
	((flags & MDSS_MDP_RIGHT_MIXER) || (dst_x >= left_lm_w))

#define BUF_POOL_SIZE 32

#define DFPS_DATA_MAX_HFP 8192
#define DFPS_DATA_MAX_HBP 8192
#define DFPS_DATA_MAX_HPW 8192
#define DFPS_DATA_MAX_FPS 0x7fffffff
#define DFPS_DATA_MAX_CLK_RATE 250000

static int mdss_mdp_overlay_free_fb_pipe(struct msm_fb_data_type *mfd);
static int mdss_mdp_overlay_fb_parse_dt(struct msm_fb_data_type *mfd);
static int mdss_mdp_overlay_off(struct msm_fb_data_type *mfd);
static void __overlay_kickoff_requeue(struct msm_fb_data_type *mfd);
static void __vsync_retire_signal(struct msm_fb_data_type *mfd, int val);
static int __vsync_set_vsync_handler(struct msm_fb_data_type *mfd);
static void __cwb_wq_handler(struct work_struct *cwb_work);
static int mdss_mdp_update_panel_info(struct msm_fb_data_type *mfd,
		int mode, int dest_ctrl);
static int mdss_mdp_set_cfg(struct msm_fb_data_type *mfd,
		struct mdp_set_cfg *cfg);

static inline bool is_ov_right_blend(struct mdp_rect *left_blend,
	struct mdp_rect *right_blend, u32 left_lm_w)
{
	return (((left_blend->x + left_blend->w) == right_blend->x)	&&
		((left_blend->x + left_blend->w) != left_lm_w)		&&
		(left_blend->x != right_blend->x)			&&
		(left_blend->y == right_blend->y)			&&
		(left_blend->h == right_blend->h));
}

/**
 * __is_more_decimation_doable() -
 * @pipe: pointer to pipe data structure
 *
 * if per pipe BW exceeds the limit and user
 * has not requested decimation then return
 * -E2BIG error back to user else try more
 * decimation based on following table config.
 *
 * ----------------------------------------------------------
 * error | split mode | src_split | v_deci |     action     |
 * ------|------------|-----------|--------|----------------|
 *       |            |           |   00   | return error   |
 *       |            |  enabled  |--------|----------------|
 *       |            |           |   >1   | more decmation |
 *       |     yes    |-----------|--------|----------------|
 *       |            |           |   00   | return error   |
 *       |            | disabled  |--------|----------------|
 *       |            |           |   >1   | return error   |
 * E2BIG |------------|-----------|--------|----------------|
 *       |            |           |   00   | return error   |
 *       |            |  enabled  |--------|----------------|
 *       |            |           |   >1   | more decmation |
 *       |     no     |-----------|--------|----------------|
 *       |            |           |   00   | return error   |
 *       |            | disabled  |--------|----------------|
 *       |            |           |   >1   | more decmation |
 * ----------------------------------------------------------
 */
static inline bool __is_more_decimation_doable(struct mdss_mdp_pipe *pipe)
{
	struct mdss_data_type *mdata = pipe->mixer_left->ctl->mdata;
	struct msm_fb_data_type *mfd = pipe->mixer_left->ctl->mfd;

	if (!mfd->split_mode && !pipe->vert_deci)
		return false;
	else if (mfd->split_mode && (!mdata->has_src_split ||
	   (mdata->has_src_split && !pipe->vert_deci)))
		return false;
	else
		return true;
}

static struct mdss_mdp_pipe *__overlay_find_pipe(
		struct msm_fb_data_type *mfd, u32 ndx)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *tmp, *pipe = NULL;

	mutex_lock(&mdp5_data->list_lock);
	list_for_each_entry(tmp, &mdp5_data->pipes_used, list) {
		if (tmp->ndx == ndx) {
			pipe = tmp;
			break;
		}
	}
	mutex_unlock(&mdp5_data->list_lock);

	return pipe;
}

static int mdss_mdp_overlay_get(struct msm_fb_data_type *mfd,
				struct mdp_overlay *req)
{
	struct mdss_mdp_pipe *pipe;

	pipe = __overlay_find_pipe(mfd, req->id);
	if (!pipe) {
		pr_err("invalid pipe ndx=%x\n", req->id);
		return pipe ? PTR_ERR(pipe) : -ENODEV;
	}

	*req = pipe->req_data;

	return 0;
}

static int mdss_mdp_ov_xres_check(struct msm_fb_data_type *mfd,
	struct mdp_overlay *req)
{
	u32 xres = 0;
	u32 left_lm_w = left_lm_w_from_mfd(mfd);
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);

	if (IS_RIGHT_MIXER_OV(req->flags, req->dst_rect.x, left_lm_w)) {
		if (mdata->has_src_split) {
			xres = left_lm_w;

			if (req->flags & MDSS_MDP_RIGHT_MIXER) {
				pr_warn("invalid use of RIGHT_MIXER flag.\n");
				/*
				 * if chip-set is capable of source split then
				 * all layers which are only on right LM should
				 * have their x offset relative to left LM's
				 * left-top or in other words relative to
				 * panel width.
				 * By modifying dst_x below, we are assuming
				 * that client is running in legacy mode
				 * chipset capable of source split.
				 */
				if (req->dst_rect.x < left_lm_w)
					req->dst_rect.x += left_lm_w;

				req->flags &= ~MDSS_MDP_RIGHT_MIXER;
			}
		} else if (req->dst_rect.x >= left_lm_w) {
			/*
			 * this is a step towards removing a reliance on
			 * MDSS_MDP_RIGHT_MIXER flags. With the new src split
			 * code, some clients of non-src-split chipsets have
			 * stopped sending MDSS_MDP_RIGHT_MIXER flag and
			 * modified their xres relative to full panel
			 * dimensions. In such cases, we need to deduct left
			 * layer mixer width before we programm this HW.
			 */
			req->dst_rect.x -= left_lm_w;
			req->flags |= MDSS_MDP_RIGHT_MIXER;
		}

		if (ctl->mixer_right) {
			xres += ctl->mixer_right->width;
		} else {
			pr_err("ov cannot be placed on right mixer\n");
			return -EPERM;
		}
	} else {
		if (ctl->mixer_left) {
			xres = ctl->mixer_left->width;
		} else {
			pr_err("ov cannot be placed on left mixer\n");
			return -EPERM;
		}

		if (mdata->has_src_split && ctl->mixer_right)
			xres += ctl->mixer_right->width;
	}

	if (CHECK_BOUNDS(req->dst_rect.x, req->dst_rect.w, xres)) {
		pr_err("dst_xres is invalid. dst_x:%d, dst_w:%d, xres:%d\n",
			req->dst_rect.x, req->dst_rect.w, xres);
		return -EOVERFLOW;
	}

	return 0;
}

int mdss_mdp_overlay_req_check(struct msm_fb_data_type *mfd,
			       struct mdp_overlay *req,
			       struct mdss_mdp_format_params *fmt)
{
	u32 yres;
	u32 min_src_size, min_dst_size;
	int content_secure;
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);

	yres = mfd->fbi->var.yres;

	content_secure = (req->flags & MDP_SECURE_OVERLAY_SESSION);
	if (!ctl->is_secure && content_secure &&
				 (mfd->panel.type == WRITEBACK_PANEL)) {
		pr_debug("return due to security concerns\n");
		return -EPERM;
	}
	if (mdata->mdp_rev >= MDSS_MDP_HW_REV_102) {
		min_src_size = fmt->is_yuv ? 2 : 1;
		min_dst_size = 1;
	} else {
		min_src_size = fmt->is_yuv ? 10 : 5;
		min_dst_size = 2;
	}

	if (req->z_order >= (mdata->max_target_zorder + MDSS_MDP_STAGE_0)) {
		pr_err("zorder %d out of range\n", req->z_order);
		return -ERANGE;
	}

	/*
	 * Cursor overlays are only supported for targets
	 * with dedicated cursors within VP
	 */
	if ((req->pipe_type == MDSS_MDP_PIPE_TYPE_CURSOR) &&
		((req->z_order != HW_CURSOR_STAGE(mdata)) ||
		 !mdata->ncursor_pipes ||
		 (req->src_rect.w > mdata->max_cursor_size))) {
		pr_err("Incorrect cursor overlay cursor_pipes=%d zorder=%d\n",
			mdata->ncursor_pipes, req->z_order);
		return -EINVAL;
	}

	if (req->src.width > MAX_IMG_WIDTH ||
	    req->src.height > MAX_IMG_HEIGHT ||
	    req->src_rect.w < min_src_size || req->src_rect.h < min_src_size ||
	    CHECK_BOUNDS(req->src_rect.x, req->src_rect.w, req->src.width) ||
	    CHECK_BOUNDS(req->src_rect.y, req->src_rect.h, req->src.height)) {
		pr_err("invalid source image img wh=%dx%d rect=%d,%d,%d,%d\n",
		       req->src.width, req->src.height,
		       req->src_rect.x, req->src_rect.y,
		       req->src_rect.w, req->src_rect.h);
		return -EOVERFLOW;
	}

	if (req->dst_rect.w < min_dst_size || req->dst_rect.h < min_dst_size) {
		pr_err("invalid destination resolution (%dx%d)",
		       req->dst_rect.w, req->dst_rect.h);
		return -EOVERFLOW;
	}

	if (req->horz_deci || req->vert_deci) {
		if (!mdata->has_decimation) {
			pr_err("No Decimation in MDP V=%x\n", mdata->mdp_rev);
			return -EINVAL;
		} else if ((req->horz_deci > MAX_DECIMATION) ||
				(req->vert_deci > MAX_DECIMATION))  {
			pr_err("Invalid decimation factors horz=%d vert=%d\n",
					req->horz_deci, req->vert_deci);
			return -EINVAL;
		} else if (req->flags & MDP_BWC_EN) {
			pr_err("Decimation can't be enabled with BWC\n");
			return -EINVAL;
		} else if (fmt->fetch_mode != MDSS_MDP_FETCH_LINEAR) {
			pr_err("Decimation can't be enabled with MacroTile format\n");
			return -EINVAL;
		}
	}

	if (!(req->flags & MDSS_MDP_ROT_ONLY)) {
		u32 src_w, src_h, dst_w, dst_h;

		if (CHECK_BOUNDS(req->dst_rect.y, req->dst_rect.h, yres)) {
			pr_err("invalid vertical destination: y=%d, h=%d\n",
				req->dst_rect.y, req->dst_rect.h);
			return -EOVERFLOW;
		}

		if (req->flags & MDP_ROT_90) {
			dst_h = req->dst_rect.w;
			dst_w = req->dst_rect.h;
		} else {
			dst_w = req->dst_rect.w;
			dst_h = req->dst_rect.h;
		}

		src_w = DECIMATED_DIMENSION(req->src_rect.w, req->horz_deci);
		src_h = DECIMATED_DIMENSION(req->src_rect.h, req->vert_deci);

		if (src_w > mdata->max_pipe_width) {
			pr_err("invalid source width=%d HDec=%d\n",
					req->src_rect.w, req->horz_deci);
			return -EINVAL;
		}

		if ((src_w * MAX_UPSCALE_RATIO) < dst_w) {
			pr_err("too much upscaling Width %d->%d\n",
			       req->src_rect.w, req->dst_rect.w);
			return -EINVAL;
		}

		if ((src_h * MAX_UPSCALE_RATIO) < dst_h) {
			pr_err("too much upscaling. Height %d->%d\n",
			       req->src_rect.h, req->dst_rect.h);
			return -EINVAL;
		}

		if (src_w > (dst_w * MAX_DOWNSCALE_RATIO)) {
			pr_err("too much downscaling. Width %d->%d H Dec=%d\n",
			       src_w, req->dst_rect.w, req->horz_deci);
			return -EINVAL;
		}

		if (src_h > (dst_h * MAX_DOWNSCALE_RATIO)) {
			pr_err("too much downscaling. Height %d->%d V Dec=%d\n",
			       src_h, req->dst_rect.h, req->vert_deci);
			return -EINVAL;
		}

		if (req->flags & MDP_BWC_EN) {
			if ((req->src.width != req->src_rect.w) ||
			    (req->src.height != req->src_rect.h)) {
				pr_err("BWC: mismatch of src img=%dx%d rect=%dx%d\n",
					req->src.width, req->src.height,
					req->src_rect.w, req->src_rect.h);
				return -EINVAL;
			}

			if ((req->flags & MDP_DECIMATION_EN) ||
					req->vert_deci || req->horz_deci) {
				pr_err("Can't enable BWC and decimation\n");
				return -EINVAL;
			}
		}

		if ((req->flags & MDP_DEINTERLACE) &&
					!req->scale.enable_pxl_ext) {
			if (req->flags & MDP_SOURCE_ROTATED_90) {
				if ((req->src_rect.w % 4) != 0) {
					pr_err("interlaced rect not h/4\n");
					return -EINVAL;
				}
			} else if ((req->src_rect.h % 4) != 0) {
				pr_err("interlaced rect not h/4\n");
				return -EINVAL;
			}
		}
	} else {
		if (req->flags & MDP_DEINTERLACE) {
			if ((req->src_rect.h % 4) != 0) {
				pr_err("interlaced rect h not multiple of 4\n");
				return -EINVAL;
			}
		}
	}

	if (fmt->is_yuv) {
		if ((req->src_rect.x & 0x1) || (req->src_rect.y & 0x1) ||
		    (req->src_rect.w & 0x1) || (req->src_rect.h & 0x1)) {
			pr_err("invalid odd src resolution or coordinates\n");
			return -EINVAL;
		}
	}

	return 0;
}

int mdp_pipe_tune_perf(struct mdss_mdp_pipe *pipe,
	u64 flags)
{
	struct mdss_data_type *mdata = pipe->mixer_left->ctl->mdata;
	struct mdss_mdp_perf_params perf;
	int rc;

	memset(&perf, 0, sizeof(perf));

	flags |= PERF_CALC_PIPE_APPLY_CLK_FUDGE |
		PERF_CALC_PIPE_CALC_SMP_SIZE;

	for (;;) {
		rc = mdss_mdp_perf_calc_pipe(pipe, &perf, NULL,
			flags);

		if (!rc && (perf.mdp_clk_rate <= mdata->max_mdp_clk_rate)) {
			rc = mdss_mdp_perf_bw_check_pipe(&perf, pipe);
			if (!rc) {
				break;
			} else if (rc == -E2BIG &&
				   !__is_more_decimation_doable(pipe)) {
				pr_debug("pipe%d exceeded per pipe BW\n",
					pipe->num);
				return rc;
			}
		}

		/*
		 * if decimation is available try to reduce minimum clock rate
		 * requirement by applying vertical decimation and reduce
		 * mdp clock requirement
		 */
		if (mdata->has_decimation && (pipe->vert_deci < MAX_DECIMATION)
			&& !pipe->bwc_mode && !pipe->scaler.enable &&
			mdss_mdp_is_linear_format(pipe->src_fmt))
			pipe->vert_deci++;
		else
			return -E2BIG;
	}

	return 0;
}

static int __mdss_mdp_validate_pxl_extn(struct mdss_mdp_pipe *pipe)
{
	int plane;

	for (plane = 0; plane < MAX_PLANES; plane++) {
		u32 hor_req_pixels, hor_fetch_pixels;
		u32 hor_ov_fetch, vert_ov_fetch;
		u32 vert_req_pixels, vert_fetch_pixels;
		u32 src_w = DECIMATED_DIMENSION(pipe->src.w, pipe->horz_deci);
		u32 src_h = DECIMATED_DIMENSION(pipe->src.h, pipe->vert_deci);

		/*
		 * plane 1 and 2 are for chroma and are same. While configuring
		 * HW, programming only one of the chroma components is
		 * sufficient.
		 */
		if (plane == 2)
			continue;

		/*
		 * For chroma plane, width is half for the following sub sampled
		 * formats. Except in case of decimation, where hardware avoids
		 * 1 line of decimation instead of downsampling.
		 */
		if (plane == 1 && !pipe->horz_deci &&
		    ((pipe->src_fmt->chroma_sample == MDSS_MDP_CHROMA_420) ||
		     (pipe->src_fmt->chroma_sample == MDSS_MDP_CHROMA_H2V1))) {
			src_w >>= 1;
		}

		if (plane == 1 && !pipe->vert_deci &&
		    ((pipe->src_fmt->chroma_sample == MDSS_MDP_CHROMA_420) ||
		     (pipe->src_fmt->chroma_sample == MDSS_MDP_CHROMA_H1V2)))
			src_h >>= 1;

		hor_req_pixels = pipe->scaler.roi_w[plane] +
			pipe->scaler.num_ext_pxls_left[plane] +
			pipe->scaler.num_ext_pxls_right[plane];

		hor_fetch_pixels = src_w +
			(pipe->scaler.left_ftch[plane] >> pipe->horz_deci) +
			pipe->scaler.left_rpt[plane] +
			(pipe->scaler.right_ftch[plane] >> pipe->horz_deci) +
			pipe->scaler.right_rpt[plane];

		hor_ov_fetch = src_w +
			(pipe->scaler.left_ftch[plane] >> pipe->horz_deci)+
			(pipe->scaler.right_ftch[plane] >> pipe->horz_deci);

		vert_req_pixels = pipe->scaler.num_ext_pxls_top[plane] +
			pipe->scaler.num_ext_pxls_btm[plane];

		vert_fetch_pixels =
			(pipe->scaler.top_ftch[plane] >> pipe->vert_deci) +
			pipe->scaler.top_rpt[plane] +
			(pipe->scaler.btm_ftch[plane] >> pipe->vert_deci)+
			pipe->scaler.btm_rpt[plane];

		vert_ov_fetch = src_h +
			(pipe->scaler.top_ftch[plane] >> pipe->vert_deci)+
			(pipe->scaler.btm_ftch[plane] >> pipe->vert_deci);

		if ((hor_req_pixels != hor_fetch_pixels) ||
			(hor_ov_fetch > pipe->img_width) ||
			(vert_req_pixels != vert_fetch_pixels) ||
			(vert_ov_fetch > pipe->img_height)) {
			pr_err("err: plane=%d h_req:%d h_fetch:%d v_req:%d v_fetch:%d\n",
					plane,
					hor_req_pixels, hor_fetch_pixels,
					vert_req_pixels, vert_fetch_pixels);
			pr_err("roi_w[%d]=%d, src_img:[%d, %d]\n",
					plane, pipe->scaler.roi_w[plane],
					pipe->img_width, pipe->img_height);
			pipe->scaler.enable = 0;
			return -EINVAL;
		}
	}

	return 0;
}

static int __mdss_mdp_validate_qseed3_cfg(struct mdss_mdp_pipe *pipe)
{
	int plane;

	for (plane = 0; plane < MAX_PLANES; plane++) {
		u32 hor_req_pixels, hor_fetch_pixels;
		u32 vert_req_pixels, vert_fetch_pixels;
		u32 src_w = pipe->src.w;
		u32 src_h = pipe->src.h;

		/*
		 * plane 1 and 2 are for chroma and are same. While configuring
		 * HW, programming only one of the chroma components is
		 * sufficient.
		 */
		if (plane == 2)
			continue;

		/*
		 * For chroma plane, width is half for the following sub sampled
		 * formats. Except in case of decimation, where hardware avoids
		 * 1 line of decimation instead of downsampling.
		 */
		if (plane == 1 && !pipe->horz_deci &&
		    ((pipe->src_fmt->chroma_sample == MDSS_MDP_CHROMA_420) ||
		     (pipe->src_fmt->chroma_sample == MDSS_MDP_CHROMA_H2V1)))
			src_w >>= 1;

		if (plane == 1 && !pipe->vert_deci &&
		    ((pipe->src_fmt->chroma_sample == MDSS_MDP_CHROMA_420) ||
		     (pipe->src_fmt->chroma_sample == MDSS_MDP_CHROMA_H1V2)))
			src_h >>= 1;

		hor_req_pixels = pipe->scaler.num_ext_pxls_left[plane];

		/**
		 * libscaler provides the fetch values before decimation
		 * and the rpt values are always 0, since qseed3 block
		 * internally does the repeat.
		 */
		hor_fetch_pixels = DECIMATED_DIMENSION(src_w +
				(int8_t)(pipe->scaler.left_ftch[plane]
					& 0xFF) +
				(int8_t)(pipe->scaler.right_ftch[plane]
					& 0xFF),
				pipe->horz_deci);

		vert_req_pixels = pipe->scaler.num_ext_pxls_top[plane];

		vert_fetch_pixels = DECIMATED_DIMENSION(src_h +
				(int8_t)(pipe->scaler.top_ftch[plane]
					& 0xFF)+
				(int8_t)(pipe->scaler.btm_ftch[plane]
					& 0xFF),
			pipe->vert_deci);

		if ((hor_req_pixels != hor_fetch_pixels) ||
			(hor_fetch_pixels > pipe->img_width) ||
			(vert_req_pixels != vert_fetch_pixels) ||
			(vert_fetch_pixels > pipe->img_height)) {
			pr_err("err: plane=%d h_req:%d h_fetch:%d v_req:%d v_fetch:%d src_img[%d %d]\n",

					plane,
					hor_req_pixels, hor_fetch_pixels,
					vert_req_pixels, vert_fetch_pixels,
					pipe->img_width, pipe->img_height);
			pipe->scaler.enable = 0;
			return -EINVAL;
		}
		/*
		 * alpha plane can only be scaled using bilinear or pixel
		 * repeat/drop, src_width and src_height are only specified
		 * for Y and UV plane
		 */
		if (plane != 3) {
			if ((hor_req_pixels !=
				pipe->scaler.src_width[plane]) ||
				(vert_req_pixels !=
				 pipe->scaler.src_height[plane])) {
				pr_err("roi_w[%d]=%d, scaler:[%d, %d], src_img:[%d, %d]\n",
					plane, pipe->scaler.roi_w[plane],
					pipe->scaler.src_width[plane],
					pipe->scaler.src_height[plane],
					pipe->img_width, pipe->img_height);
				pipe->scaler.enable = 0;
				return -EINVAL;
			}
		}
	}

	return 0;
}

int mdss_mdp_overlay_setup_scaling(struct mdss_mdp_pipe *pipe)
{
	u32 src;
	int rc = 0;
	struct mdss_data_type *mdata;

	mdata = mdss_mdp_get_mdata();
	if (pipe->scaler.enable) {
		if (test_bit(MDSS_CAPS_QSEED3, mdata->mdss_caps_map))
			rc = __mdss_mdp_validate_qseed3_cfg(pipe);
		else
			rc = __mdss_mdp_validate_pxl_extn(pipe);

		return rc;
	}

	memset(&pipe->scaler, 0, sizeof(struct mdp_scale_data_v2));
	src = DECIMATED_DIMENSION(pipe->src.w, pipe->horz_deci);
	rc = mdss_mdp_calc_phase_step(src, pipe->dst.w,
			&pipe->scaler.phase_step_x[0]);
	if (rc == -EOVERFLOW) {
		/* overflow on horizontal direction is acceptable */
		rc = 0;
	} else if (rc) {
		pr_err("Horizontal scaling calculation failed=%d! %d->%d\n",
				rc, src, pipe->dst.w);
		return rc;
	}

	src = DECIMATED_DIMENSION(pipe->src.h, pipe->vert_deci);
	rc = mdss_mdp_calc_phase_step(src, pipe->dst.h,
			&pipe->scaler.phase_step_y[0]);

	if ((rc == -EOVERFLOW) && (pipe->type == MDSS_MDP_PIPE_TYPE_VIG)) {
		/* overflow on Qseed2 scaler is acceptable */
		rc = 0;
	} else if (rc == -EOVERFLOW) {
		/* overflow expected and should fallback to GPU */
		rc = -ECANCELED;
	} else if (rc) {
		pr_err("Vertical scaling calculation failed=%d! %d->%d\n",
				rc, src, pipe->dst.h);
	}

	if (test_bit(MDSS_CAPS_QSEED3, mdata->mdss_caps_map) &&
			(pipe->type == MDSS_MDP_PIPE_TYPE_VIG))
		mdss_mdp_pipe_calc_qseed3_cfg(pipe);
	else
		mdss_mdp_pipe_calc_pixel_extn(pipe);

	return rc;
}

inline void mdss_mdp_overlay_set_chroma_sample(
	struct mdss_mdp_pipe *pipe)
{
	pipe->chroma_sample_v = pipe->chroma_sample_h = 0;

	switch (pipe->src_fmt->chroma_sample) {
	case MDSS_MDP_CHROMA_H1V2:
		pipe->chroma_sample_v = 1;
		break;
	case MDSS_MDP_CHROMA_H2V1:
		pipe->chroma_sample_h = 1;
		break;
	case MDSS_MDP_CHROMA_420:
		pipe->chroma_sample_v = 1;
		pipe->chroma_sample_h = 1;
		break;
	}
	if (pipe->horz_deci)
		pipe->chroma_sample_h = 0;
	if (pipe->vert_deci)
		pipe->chroma_sample_v = 0;
}

int mdss_mdp_overlay_pipe_setup(struct msm_fb_data_type *mfd,
	struct mdp_overlay *req, struct mdss_mdp_pipe **ppipe,
	struct mdss_mdp_pipe *left_blend_pipe, bool is_single_layer)
{
	struct mdss_mdp_format_params *fmt;
	struct mdss_mdp_pipe *pipe;
	struct mdss_mdp_mixer *mixer = NULL;
	u32 pipe_type, mixer_mux;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	int ret;
	u32 bwc_enabled;
	u32 rot90;
	bool is_vig_needed = false;
	u32 left_lm_w = left_lm_w_from_mfd(mfd);
	u32 flags = 0;
	u32 off = 0;

	if (mdp5_data->ctl == NULL)
		return -ENODEV;

	if (req->flags & MDP_ROT_90) {
		pr_err("unsupported inline rotation\n");
		return -EOPNOTSUPP;
	}

	if ((req->dst_rect.w > mdata->max_mixer_width) ||
		(req->dst_rect.h > MAX_DST_H)) {
		pr_err("exceeded max mixer supported resolution %dx%d\n",
				req->dst_rect.w, req->dst_rect.h);
		return -EOVERFLOW;
	}

	if (IS_RIGHT_MIXER_OV(req->flags, req->dst_rect.x, left_lm_w))
		mixer_mux = MDSS_MDP_MIXER_MUX_RIGHT;
	else
		mixer_mux = MDSS_MDP_MIXER_MUX_LEFT;

	pr_debug("ctl=%u req id=%x mux=%d z_order=%d flags=0x%x dst_x:%d\n",
		mdp5_data->ctl->num, req->id, mixer_mux, req->z_order,
		req->flags, req->dst_rect.x);

	fmt = mdss_mdp_get_format_params(req->src.format);
	if (!fmt) {
		pr_err("invalid pipe format %d\n", req->src.format);
		return -EINVAL;
	}

	bwc_enabled = req->flags & MDP_BWC_EN;
	rot90 = req->flags & MDP_SOURCE_ROTATED_90;

	/*
	 * Always set yuv rotator output to pseudo planar.
	 */
	if (bwc_enabled || rot90) {
		req->src.format =
			mdss_mdp_get_rotator_dst_format(req->src.format, rot90,
				bwc_enabled);
		fmt = mdss_mdp_get_format_params(req->src.format);
		if (!fmt) {
			pr_err("invalid pipe format %d\n", req->src.format);
			return -EINVAL;
		}
	}

	ret = mdss_mdp_ov_xres_check(mfd, req);
	if (ret)
		return ret;

	ret = mdss_mdp_overlay_req_check(mfd, req, fmt);
	if (ret)
		return ret;

	mixer = mdss_mdp_mixer_get(mdp5_data->ctl, mixer_mux);
	if (!mixer) {
		pr_err("unable to get mixer\n");
		return -ENODEV;
	}

	if ((mdata->has_non_scalar_rgb) &&
		((req->src_rect.w != req->dst_rect.w) ||
			(req->src_rect.h != req->dst_rect.h)))
		is_vig_needed = true;

	if (req->id == MSMFB_NEW_REQUEST) {
		switch (req->pipe_type) {
		case PIPE_TYPE_VIG:
			pipe_type = MDSS_MDP_PIPE_TYPE_VIG;
			break;
		case PIPE_TYPE_RGB:
			pipe_type = MDSS_MDP_PIPE_TYPE_RGB;
			break;
		case PIPE_TYPE_DMA:
			pipe_type = MDSS_MDP_PIPE_TYPE_DMA;
			break;
		case PIPE_TYPE_CURSOR:
			pipe_type = MDSS_MDP_PIPE_TYPE_CURSOR;
			break;
		case PIPE_TYPE_AUTO:
		default:
			if (req->flags & MDP_OV_PIPE_FORCE_DMA) {
				pipe_type = MDSS_MDP_PIPE_TYPE_DMA;
				/*
				 * For paths using legacy API's for pipe
				 * allocation, use offset of 2 for allocating
				 * right pipe for pipe type DMA. This is
				 * because from SDM 3.x.x. onwards one DMA
				 * pipe has two instances for multirect.
				 */
				off = (mixer_mux == MDSS_MDP_MIXER_MUX_RIGHT)
						? 2 : 0;
			} else if (fmt->is_yuv ||
				(req->flags & MDP_OV_PIPE_SHARE) ||
				is_vig_needed) {
				pipe_type = MDSS_MDP_PIPE_TYPE_VIG;
			} else {
				pipe_type = MDSS_MDP_PIPE_TYPE_RGB;
			}
			break;
		}

		pipe = mdss_mdp_pipe_alloc(mixer, off,
				pipe_type, left_blend_pipe);

		/* RGB pipes can be used instead of DMA */
		if (IS_ERR_OR_NULL(pipe) &&
		    (req->pipe_type == PIPE_TYPE_AUTO) &&
		    (pipe_type == MDSS_MDP_PIPE_TYPE_DMA)) {
			pr_debug("giving RGB pipe for fb%d. flags:0x%x\n",
				mfd->index, req->flags);
			pipe_type = MDSS_MDP_PIPE_TYPE_RGB;
			pipe = mdss_mdp_pipe_alloc(mixer, off, pipe_type,
				left_blend_pipe);
		}

		/* VIG pipes can also support RGB format */
		if (IS_ERR_OR_NULL(pipe) &&
		    (req->pipe_type == PIPE_TYPE_AUTO) &&
		    (pipe_type == MDSS_MDP_PIPE_TYPE_RGB)) {
			pr_debug("giving ViG pipe for fb%d. flags:0x%x\n",
				mfd->index, req->flags);
			pipe_type = MDSS_MDP_PIPE_TYPE_VIG;
			pipe = mdss_mdp_pipe_alloc(mixer, off, pipe_type,
				left_blend_pipe);
		}

		if (IS_ERR(pipe)) {
			return PTR_ERR(pipe);
		} else if (!pipe) {
			pr_err("error allocating pipe. flags=0x%x req->pipe_type=%d pipe_type=%d\n",
				req->flags, req->pipe_type, pipe_type);
			return -ENODEV;
		}

		ret = mdss_mdp_pipe_map(pipe);
		if (ret) {
			pr_err("unable to map pipe=%d\n", pipe->num);
			return ret;
		}

		mutex_lock(&mdp5_data->list_lock);
		list_add(&pipe->list, &mdp5_data->pipes_used);
		mutex_unlock(&mdp5_data->list_lock);
		pipe->mixer_left = mixer;
		pipe->mfd = mfd;
		pipe->play_cnt = 0;
	} else {
		pipe = __overlay_find_pipe(mfd, req->id);
		if (!pipe) {
			pr_err("invalid pipe ndx=%x\n", req->id);
			return -ENODEV;
		}

		ret = mdss_mdp_pipe_map(pipe);
		if (IS_ERR_VALUE(ret)) {
			pr_err("Unable to map used pipe%d ndx=%x\n",
					pipe->num, pipe->ndx);
			return ret;
		}

		if (is_vig_needed && (pipe->type != MDSS_MDP_PIPE_TYPE_VIG)) {
			pr_err("pipe is non-scalar ndx=%x\n", req->id);
			ret = -EINVAL;
			goto exit_fail;
		}

		if ((pipe->mixer_left != mixer) &&
				(pipe->type != MDSS_MDP_PIPE_TYPE_CURSOR)) {
			if (!mixer->ctl || (mixer->ctl->mfd != mfd)) {
				pr_err("Can't switch mixer %d->%d pnum %d!\n",
					pipe->mixer_left->num, mixer->num,
						pipe->num);
				ret = -EINVAL;
				goto exit_fail;
			}
			pr_debug("switching pipe%d mixer %d->%d stage%d\n",
				pipe->num,
				pipe->mixer_left ? pipe->mixer_left->num : -1,
				mixer->num, req->z_order);
			mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_left);
			pipe->mixer_left = mixer;
		}
	}

	if (left_blend_pipe) {
		if (pipe->priority <= left_blend_pipe->priority) {
			pr_err("priority limitation. left:%d right%d\n",
				left_blend_pipe->priority, pipe->priority);
			ret = -EBADSLT;
			goto exit_fail;
		} else {
			pr_debug("pipe%d is a right_pipe\n", pipe->num);
			pipe->is_right_blend = true;
		}
	} else if (pipe->is_right_blend) {
		/*
		 * pipe used to be right blend need to update mixer
		 * configuration to remove it as a right blend
		 */
		mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_left);
		mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_right);
		pipe->is_right_blend = false;
	}

	if (mfd->panel_orientation)
		req->flags ^= mfd->panel_orientation;

	req->priority = pipe->priority;
	if (!pipe->dirty && !memcmp(req, &pipe->req_data, sizeof(*req))) {
		pr_debug("skipping pipe_reconfiguration\n");
		goto skip_reconfigure;
	}

	pipe->flags = req->flags;
	if (bwc_enabled  &&  !mdp5_data->mdata->has_bwc) {
		pr_err("BWC is not supported in MDP version %x\n",
			mdp5_data->mdata->mdp_rev);
		pipe->bwc_mode = 0;
	} else {
		pipe->bwc_mode = pipe->mixer_left->rotator_mode ?
			0 : (bwc_enabled ? 1 : 0) ;
	}
	pipe->img_width = req->src.width & 0x3fff;
	pipe->img_height = req->src.height & 0x3fff;
	pipe->src.x = req->src_rect.x;
	pipe->src.y = req->src_rect.y;
	pipe->src.w = req->src_rect.w;
	pipe->src.h = req->src_rect.h;
	pipe->dst.x = req->dst_rect.x;
	pipe->dst.y = req->dst_rect.y;
	pipe->dst.w = req->dst_rect.w;
	pipe->dst.h = req->dst_rect.h;

	if (mixer->ctl) {
		pipe->dst.x += mixer->ctl->border_x_off;
		pipe->dst.y += mixer->ctl->border_y_off;
	}

	pipe->horz_deci = req->horz_deci;
	pipe->vert_deci = req->vert_deci;

	/*
	 * check if overlay span across two mixers and if source split is
	 * available. If yes, enable src_split_req flag so that during mixer
	 * staging, same pipe will be stagged on both layer mixers.
	 */
	if (mdata->has_src_split) {
		if ((pipe->type == MDSS_MDP_PIPE_TYPE_CURSOR) &&
				is_split_lm(mfd)) {
			pipe->src_split_req = true;
		} else if ((mixer_mux == MDSS_MDP_MIXER_MUX_LEFT) &&
		    ((req->dst_rect.x + req->dst_rect.w) > mixer->width)) {
			if (req->dst_rect.x >= mixer->width) {
				pr_err("%pS: err dst_x can't lie in right half",
					__builtin_return_address(0));
				pr_cont(" flags:0x%x dst x:%d w:%d lm_w:%d\n",
					req->flags, req->dst_rect.x,
					req->dst_rect.w, mixer->width);
				ret = -EINVAL;
				goto exit_fail;
			} else {
				pipe->src_split_req = true;
			}
		} else {
			if (pipe->src_split_req) {
				mdss_mdp_mixer_pipe_unstage(pipe,
					pipe->mixer_right);
				pipe->mixer_right = NULL;
			}
			pipe->src_split_req = false;
		}
	}

	memcpy(&pipe->scaler, &req->scale, sizeof(struct mdp_scale_data));
	pipe->src_fmt = fmt;
	mdss_mdp_overlay_set_chroma_sample(pipe);

	pipe->mixer_stage = req->z_order;
	pipe->is_fg = req->is_fg;
	pipe->alpha = req->alpha;
	pipe->transp = req->transp_mask;
	pipe->blend_op = req->blend_op;
	if (pipe->blend_op == BLEND_OP_NOT_DEFINED)
		pipe->blend_op = fmt->alpha_enable ?
					BLEND_OP_PREMULTIPLIED :
					BLEND_OP_OPAQUE;

	if (!fmt->alpha_enable && (pipe->blend_op != BLEND_OP_OPAQUE))
		pr_debug("Unintended blend_op %d on layer with no alpha plane\n",
			pipe->blend_op);

	if (fmt->is_yuv && !(pipe->flags & MDP_SOURCE_ROTATED_90) &&
			!pipe->scaler.enable) {
		pipe->overfetch_disable = OVERFETCH_DISABLE_BOTTOM;

		if (!(pipe->flags & MDSS_MDP_DUAL_PIPE) ||
		    IS_RIGHT_MIXER_OV(pipe->flags, pipe->dst.x, left_lm_w))
			pipe->overfetch_disable |= OVERFETCH_DISABLE_RIGHT;
		pr_debug("overfetch flags=%x\n", pipe->overfetch_disable);
	} else {
		pipe->overfetch_disable = 0;
	}
	pipe->bg_color = req->bg_color;

	if (pipe->type == MDSS_MDP_PIPE_TYPE_CURSOR)
		goto cursor_done;

	mdss_mdp_pipe_pp_clear(pipe);
	if (pipe->flags & MDP_OVERLAY_PP_CFG_EN) {
		memcpy(&pipe->pp_cfg, &req->overlay_pp_cfg,
					sizeof(struct mdp_overlay_pp_params));
		ret = mdss_mdp_pp_sspp_config(pipe);
		if (ret) {
			pr_err("failed to configure pp params ret %d\n", ret);
			goto exit_fail;
		}
	}

	/*
	 * Populate Color Space.
	 */
	if (pipe->src_fmt->is_yuv && (pipe->type == MDSS_MDP_PIPE_TYPE_VIG))
		pipe->csc_coeff_set = req->color_space;
	/*
	 * When scaling is enabled src crop and image
	 * width and height is modified by user
	 */
	if ((pipe->flags & MDP_DEINTERLACE) && !pipe->scaler.enable) {
		if (pipe->flags & MDP_SOURCE_ROTATED_90) {
			pipe->src.x = DIV_ROUND_UP(pipe->src.x, 2);
			pipe->src.x &= ~1;
			pipe->src.w /= 2;
			pipe->img_width /= 2;
		} else {
			pipe->src.h /= 2;
			pipe->src.y = DIV_ROUND_UP(pipe->src.y, 2);
			pipe->src.y &= ~1;
		}
	}

	if (is_single_layer)
		flags |= PERF_CALC_PIPE_SINGLE_LAYER;

	ret = mdp_pipe_tune_perf(pipe, flags);
	if (ret) {
		pr_debug("unable to satisfy performance. ret=%d\n", ret);
		goto exit_fail;
	}

	ret = mdss_mdp_overlay_setup_scaling(pipe);
	if (ret)
		goto exit_fail;

	if ((mixer->type == MDSS_MDP_MIXER_TYPE_WRITEBACK) &&
		(mdp5_data->mdata->wfd_mode == MDSS_MDP_WFD_SHARED))
		mdss_mdp_smp_release(pipe);

	ret = mdss_mdp_smp_reserve(pipe);
	if (ret) {
		pr_debug("mdss_mdp_smp_reserve failed. pnum:%d ret=%d\n",
			pipe->num, ret);
		goto exit_fail;
	}


	req->id = pipe->ndx;

cursor_done:
	req->vert_deci = pipe->vert_deci;

	pipe->req_data = *req;
	pipe->dirty = false;

	pipe->params_changed++;
skip_reconfigure:
	*ppipe = pipe;

	mdss_mdp_pipe_unmap(pipe);

	return ret;
exit_fail:
	mdss_mdp_pipe_unmap(pipe);

	mutex_lock(&mdp5_data->list_lock);
	if (pipe->play_cnt == 0) {
		pr_debug("failed for pipe %d\n", pipe->num);
		if (!list_empty(&pipe->list))
			list_del_init(&pipe->list);
		mdss_mdp_pipe_destroy(pipe);
	}

	/* invalidate any overlays in this framebuffer after failure */
	list_for_each_entry(pipe, &mdp5_data->pipes_used, list) {
		pr_debug("freeing allocations for pipe %d\n", pipe->num);
		mdss_mdp_smp_unreserve(pipe);
		pipe->params_changed = 0;
		pipe->dirty = true;
	}
	mutex_unlock(&mdp5_data->list_lock);
	return ret;
}

static int mdss_mdp_overlay_set(struct msm_fb_data_type *mfd,
				struct mdp_overlay *req)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret;

	ret = mutex_lock_interruptible(&mdp5_data->ov_lock);
	if (ret)
		return ret;

	if (mdss_fb_is_power_off(mfd)) {
		mutex_unlock(&mdp5_data->ov_lock);
		return -EPERM;
	}

	if (req->src.format == MDP_RGB_BORDERFILL) {
		req->id = BORDERFILL_NDX;
	} else {
		struct mdss_mdp_pipe *pipe;

		/* userspace zorder start with stage 0 */
		req->z_order += MDSS_MDP_STAGE_0;

		ret = mdss_mdp_overlay_pipe_setup(mfd, req, &pipe, NULL, false);

		req->z_order -= MDSS_MDP_STAGE_0;
	}

	mutex_unlock(&mdp5_data->ov_lock);

	return ret;
}

/*
 * it's caller responsibility to acquire mdp5_data->list_lock while calling
 * this function
 */
struct mdss_mdp_data *mdss_mdp_overlay_buf_alloc(struct msm_fb_data_type *mfd,
		struct mdss_mdp_pipe *pipe)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_data *buf;
	int i;

	if (list_empty(&mdp5_data->bufs_pool)) {
		pr_debug("allocating %u bufs for fb%d\n",
					BUF_POOL_SIZE, mfd->index);

		buf = kzalloc(sizeof(*buf) * BUF_POOL_SIZE, GFP_KERNEL);
		if (!buf) {
			pr_err("Unable to allocate buffer pool\n");
			return NULL;
		}

		list_add(&buf->chunk_list, &mdp5_data->bufs_chunks);
		kmemleak_not_leak(buf);

		for (i = 0; i < BUF_POOL_SIZE; i++) {
			buf->state = MDP_BUF_STATE_UNUSED;
			list_add(&buf[i].buf_list, &mdp5_data->bufs_pool);
		}
	}

	buf = list_first_entry(&mdp5_data->bufs_pool,
			struct mdss_mdp_data, buf_list);
	BUG_ON(buf->state != MDP_BUF_STATE_UNUSED);
	buf->state = MDP_BUF_STATE_READY;
	buf->last_alloc = local_clock();
	buf->last_pipe = pipe;

	list_move_tail(&buf->buf_list, &mdp5_data->bufs_used);
	list_add_tail(&buf->pipe_list, &pipe->buf_queue);

	pr_debug("buffer alloc: %pK\n", buf);

	return buf;
}

static
struct mdss_mdp_data *__mdp_overlay_buf_alloc(struct msm_fb_data_type *mfd,
		struct mdss_mdp_pipe *pipe)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_data *buf;

	mutex_lock(&mdp5_data->list_lock);
	buf = mdss_mdp_overlay_buf_alloc(mfd, pipe);
	mutex_unlock(&mdp5_data->list_lock);

	return buf;
}

static void mdss_mdp_overlay_buf_deinit(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_data *buf, *t;

	pr_debug("performing cleanup of buffers pool on fb%d\n", mfd->index);

	BUG_ON(!list_empty(&mdp5_data->bufs_used));

	list_for_each_entry_safe(buf, t, &mdp5_data->bufs_pool, buf_list)
		list_del(&buf->buf_list);

	list_for_each_entry_safe(buf, t, &mdp5_data->bufs_chunks, chunk_list) {
		list_del(&buf->chunk_list);
		kfree(buf);
	}
}

/*
 * it's caller responsibility to acquire mdp5_data->list_lock while calling
 * this function
 */
void mdss_mdp_overlay_buf_free(struct msm_fb_data_type *mfd,
		struct mdss_mdp_data *buf)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	if (!list_empty(&buf->pipe_list))
		list_del_init(&buf->pipe_list);

	mdss_mdp_data_free(buf, false, DMA_TO_DEVICE);

	buf->last_freed = local_clock();
	buf->state = MDP_BUF_STATE_UNUSED;

	pr_debug("buffer freed: %pK\n", buf);

	list_move_tail(&buf->buf_list, &mdp5_data->bufs_pool);
}

static void __mdp_overlay_buf_free(struct msm_fb_data_type *mfd,
		struct mdss_mdp_data *buf)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	mutex_lock(&mdp5_data->list_lock);
	mdss_mdp_overlay_buf_free(mfd, buf);
	mutex_unlock(&mdp5_data->list_lock);
}

static inline void __pipe_buf_mark_cleanup(struct msm_fb_data_type *mfd,
		struct mdss_mdp_data *buf)
{
	/* buffer still in bufs_used, marking it as cleanup will clean it up */
	buf->state = MDP_BUF_STATE_CLEANUP;
	list_del_init(&buf->pipe_list);
}

/**
 * __mdss_mdp_overlay_free_list_purge() - clear free list of buffers
 * @mfd:	Msm frame buffer data structure for the associated fb
 *
 * Frees memory and clears current list of buffers which are pending free
 */
static void __mdss_mdp_overlay_free_list_purge(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_data *buf, *t;

	pr_debug("purging fb%d free list\n", mfd->index);

	list_for_each_entry_safe(buf, t, &mdp5_data->bufs_freelist, buf_list)
		mdss_mdp_overlay_buf_free(mfd, buf);
}

static void __overlay_pipe_cleanup(struct msm_fb_data_type *mfd,
		struct mdss_mdp_pipe *pipe)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_data *buf, *tmpbuf;

	list_for_each_entry_safe(buf, tmpbuf, &pipe->buf_queue, pipe_list) {
		__pipe_buf_mark_cleanup(mfd, buf);
		list_move(&buf->buf_list, &mdp5_data->bufs_freelist);

		/*
		 * free the buffers on the same cycle instead of waiting for
		 * next kickoff
		 */
		mdss_mdp_overlay_buf_free(mfd, buf);
	}

	mdss_mdp_pipe_destroy(pipe);
}

/**
 * mdss_mdp_overlay_cleanup() - handles cleanup after frame commit
 * @mfd:           Msm frame buffer data structure for the associated fb
 * @destroy_pipes: list of pipes that should be destroyed as part of cleanup
 *
 * Goes through destroy_pipes list and ensures they are ready to be destroyed
 * and cleaned up. Also cleanup of any pipe buffers after flip.
 */
static void mdss_mdp_overlay_cleanup(struct msm_fb_data_type *mfd,
		struct list_head *destroy_pipes,
		bool locked)
{
	struct mdss_mdp_pipe *pipe, *tmp;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	bool recovery_mode = false;
	bool skip_fetch_halt, pair_found;
	struct mdss_mdp_data *buf, *tmpbuf;

	if (!locked)
		mutex_lock(&mdp5_data->list_lock);
	list_for_each_entry(pipe, destroy_pipes, list) {
		pair_found = false;
		skip_fetch_halt = false;
		tmp = pipe;

		/*
		 * Find if second rect is in the destroy list from the current
		 * position. So if both rects are part of the destroy list then
		 * fetch halt will be skipped for the 1st rect.
		 */
		list_for_each_entry_from(tmp, destroy_pipes, list) {
			if ((tmp != pipe) && (tmp->num == pipe->num)) {
				pair_found = true;
				break;
			}
		}

		/* skip fetch halt if pipe's other rect is still in use */
		if (!pair_found) {
			tmp = (struct mdss_mdp_pipe *)pipe->multirect.next;
			if (tmp)
				skip_fetch_halt =
					atomic_read(&tmp->kref.refcount);
		}

		/* make sure pipe fetch has been halted before freeing buffer */
		if (!skip_fetch_halt && mdss_mdp_pipe_fetch_halt(pipe, false)) {
			/*
			 * if pipe is not able to halt. Enter recovery mode,
			 * by un-staging any pipes that are attached to mixer
			 * so that any freed pipes that are not able to halt
			 * can be staged in solid fill mode and be reset
			 * with next vsync
			 */
			if (!recovery_mode) {
				recovery_mode = true;
				mdss_mdp_mixer_unstage_all(ctl->mixer_left);
				mdss_mdp_mixer_unstage_all(ctl->mixer_right);
			}
			pipe->params_changed++;
			pipe->unhalted = true;
			mdss_mdp_pipe_queue_data(pipe, NULL);
		}
	}

	if (recovery_mode) {
		pr_warn("performing recovery sequence for fb%d\n", mfd->index);
		__overlay_kickoff_requeue(mfd);
	}

	__mdss_mdp_overlay_free_list_purge(mfd);

	list_for_each_entry_safe(buf, tmpbuf, &mdp5_data->bufs_used, buf_list) {
		if (buf->state == MDP_BUF_STATE_CLEANUP)
			list_move(&buf->buf_list, &mdp5_data->bufs_freelist);
	}

	list_for_each_entry_safe(pipe, tmp, destroy_pipes, list) {
		list_del_init(&pipe->list);
		if (recovery_mode) {
			mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_left);
			mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_right);
			pipe->mixer_stage = MDSS_MDP_STAGE_UNUSED;
		}
		__overlay_pipe_cleanup(mfd, pipe);

		if (pipe->multirect.num == MDSS_MDP_PIPE_RECT0) {
			/*
			 * track only RECT0, since at any given point there
			 * can only be RECT0 only or RECT0 + RECT1
			 */
			ctl->mixer_left->next_pipe_map &= ~pipe->ndx;
			if (ctl->mixer_right)
				ctl->mixer_right->next_pipe_map &= ~pipe->ndx;
		}
	}
	if (!locked)
		mutex_unlock(&mdp5_data->list_lock);
}

void mdss_mdp_handoff_cleanup_pipes(struct msm_fb_data_type *mfd,
	u32 type)
{
	u32 i, npipes;
	struct mdss_mdp_pipe *pipe;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);

	switch (type) {
	case MDSS_MDP_PIPE_TYPE_VIG:
		pipe = mdata->vig_pipes;
		npipes = mdata->nvig_pipes;
		break;
	case MDSS_MDP_PIPE_TYPE_RGB:
		pipe = mdata->rgb_pipes;
		npipes = mdata->nrgb_pipes;
		break;
	case MDSS_MDP_PIPE_TYPE_DMA:
		pipe = mdata->dma_pipes;
		npipes = mdata->ndma_pipes;
		break;
	default:
		return;
	}

	for (i = 0; i < npipes; i++) {
		/* only check for first rect and ignore additional */
		if (pipe->is_handed_off) {
			pr_debug("Unmapping handed off pipe %d\n", pipe->num);
			list_move(&pipe->list, &mdp5_data->pipes_cleanup);
			mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_left);
			pipe->is_handed_off = false;
		}
		pipe += pipe->multirect.max_rects;
	}
}

/**
 * mdss_mdp_overlay_start() - Programs the MDP control data path to hardware
 * @mfd: Msm frame buffer structure associated with fb device.
 *
 * Program the MDP hardware with the control settings for the framebuffer
 * device. In addition to this, this function also handles the transition
 * from the the splash screen to the android boot animation when the
 * continuous splash screen feature is enabled.
 */
int mdss_mdp_overlay_start(struct msm_fb_data_type *mfd)
{
	int rc;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = mdp5_data->ctl;
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);

	if (mdss_mdp_ctl_is_power_on(ctl)) {
		if (!mdp5_data->mdata->batfet)
			mdss_mdp_batfet_ctrl(mdp5_data->mdata, true);
		mdss_mdp_release_splash_pipe(mfd);
		return 0;
	} else if (mfd->panel_info->cont_splash_enabled) {
		mutex_lock(&mdp5_data->list_lock);
		rc = list_empty(&mdp5_data->pipes_used);
		mutex_unlock(&mdp5_data->list_lock);
		if (rc) {
			pr_debug("empty kickoff on fb%d during cont splash\n",
					mfd->index);
			return 0;
		}
	} else if (mdata->handoff_pending) {
		pr_warn("fb%d: commit while splash handoff pending\n",
				mfd->index);
		return -EPERM;
	}

	pr_debug("starting fb%d overlay\n", mfd->index);

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);

	/*
	 * If idle pc feature is not enabled, then get a reference to the
	 * runtime device which will be released when overlay is turned off
	 */
	if (!mdp5_data->mdata->idle_pc_enabled ||
		(mfd->panel_info->type != MIPI_CMD_PANEL)) {
		rc = pm_runtime_get_sync(&mfd->pdev->dev);
		if (IS_ERR_VALUE(rc)) {
			pr_err("unable to resume with pm_runtime_get_sync rc=%d\n",
				rc);
			goto end;
		}
	}

	/*
	 * We need to do hw init before any hw programming.
	 * Also, hw init involves programming the VBIF registers which
	 * should be done only after attaching IOMMU which in turn would call
	 * in to TZ to restore security configs on the VBIF registers.
	 * This is not needed when continuous splash screen is enabled since
	 * we would have called in to TZ to restore security configs from LK.
	 */
	if (!mfd->panel_info->cont_splash_enabled) {
		rc = mdss_iommu_ctrl(1);
		if (IS_ERR_VALUE(rc)) {
			pr_err("iommu attach failed rc=%d\n", rc);
			goto end;
		}
		mdss_hw_init(mdss_res);
		mdss_iommu_ctrl(0);
	}

	/*
	 * Increment the overlay active count prior to calling ctl_start.
	 * This is needed to ensure that if idle power collapse kicks in
	 * right away, it would be handled correctly.
	 */
	atomic_inc(&mdp5_data->mdata->active_intf_cnt);
	rc = mdss_mdp_ctl_start(ctl, false);
	if (rc == 0) {
		mdss_mdp_ctl_notifier_register(mdp5_data->ctl,
				&mfd->mdp_sync_pt_data.notifier);
	} else {
		pr_err("mdp ctl start failed.\n");
		goto ctl_error;
	}

	/* Restore any previously configured PP features by resetting the dirty
	 * bits for enabled features. The dirty bits will be consumed during the
	 * first display commit when the PP hardware blocks are updated
	 */
	rc = mdss_mdp_pp_resume(mfd);
	if (rc && (rc != -EPERM) && (rc != -ENODEV))
		pr_err("PP resume err %d\n", rc);

	rc = mdss_mdp_splash_cleanup(mfd, true);
	if (!rc)
		goto end;

ctl_error:
	mdss_mdp_ctl_destroy(ctl);
	atomic_dec(&mdp5_data->mdata->active_intf_cnt);
	mdp5_data->ctl = NULL;
end:
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);
	return rc;
}

static void mdss_mdp_overlay_update_pm(struct mdss_overlay_private *mdp5_data)
{
	ktime_t wakeup_time;

	if (!mdp5_data->cpu_pm_hdl)
		return;

	if (mdss_mdp_display_wakeup_time(mdp5_data->ctl, &wakeup_time))
		return;

	activate_event_timer(mdp5_data->cpu_pm_hdl, wakeup_time);
}

static void __unstage_pipe_and_clean_buf(struct msm_fb_data_type *mfd,
	struct mdss_mdp_pipe *pipe, struct mdss_mdp_data *buf)
{

	pr_debug("unstaging pipe:%d rect:%d buf:%d\n",
		pipe->num, pipe->multirect.num, !buf);
	MDSS_XLOG(pipe->num, pipe->multirect.num, !buf);
	mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_left);
	mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_right);
	pipe->dirty = true;

	if (buf)
		__pipe_buf_mark_cleanup(mfd, buf);
}

static int __dest_scaler_setup(struct msm_fb_data_type *mfd)
{
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);

	mutex_lock(&ctl->ds_lock);

	if (ctl->mixer_left)
		mdss_mdp_dest_scaler_setup_locked(ctl->mixer_left);

	if (ctl->mixer_right)
		mdss_mdp_dest_scaler_setup_locked(ctl->mixer_right);

	mutex_unlock(&ctl->ds_lock);

	return 0;
}

static int __overlay_queue_pipes(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *pipe;
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	struct mdss_mdp_ctl *tmp;
	int ret = 0;

	list_for_each_entry(pipe, &mdp5_data->pipes_used, list) {
		struct mdss_mdp_data *buf;

		if (pipe->dirty) {
			pr_err("fb%d: pipe %d dirty! skipping configuration\n",
					mfd->index, pipe->num);
			continue;
		}

		/*
		 * When secure display is enabled, if there is a non secure
		 * display pipe, skip that
		 */
		if (mdss_get_sd_client_cnt() &&
			!(pipe->flags & MDP_SECURE_DISPLAY_OVERLAY_SESSION)) {
			pr_warn("Non secure pipe during secure display: %u: %16llx, skip\n",
					pipe->num, pipe->flags);
			continue;
		}
		/*
		 * When external is connected and no dedicated wfd is present,
		 * reprogram DMA pipe before kickoff to clear out any previous
		 * block mode configuration.
		 */
		if ((pipe->type == MDSS_MDP_PIPE_TYPE_DMA) &&
		    (ctl->shared_lock &&
		    (ctl->mdata->wfd_mode == MDSS_MDP_WFD_SHARED))) {
			if (ctl->mdata->mixer_switched) {
				ret = mdss_mdp_overlay_pipe_setup(mfd,
					&pipe->req_data, &pipe, NULL, false);
				pr_debug("reseting DMA pipe for ctl=%d",
					 ctl->num);
			}
			if (ret) {
				pr_err("can't reset DMA pipe ret=%d ctl=%d\n",
					ret, ctl->num);
				return ret;
			}

			tmp = mdss_mdp_ctl_mixer_switch(ctl,
					MDSS_MDP_WB_CTL_TYPE_LINE);
			if (!tmp)
				return -EINVAL;
			pipe->mixer_left = mdss_mdp_mixer_get(tmp,
					MDSS_MDP_MIXER_MUX_DEFAULT);
		}

		buf = list_first_entry_or_null(&pipe->buf_queue,
				struct mdss_mdp_data, pipe_list);
		if (buf) {
			switch (buf->state) {
			case MDP_BUF_STATE_READY:
				pr_debug("pnum=%d buf=%pK first buffer ready\n",
						pipe->num, buf);
				break;
			case MDP_BUF_STATE_ACTIVE:
				if (list_is_last(&buf->pipe_list,
						&pipe->buf_queue)) {
					pr_debug("pnum=%d no buf update\n",
							pipe->num);
				} else {
					struct mdss_mdp_data *tmp = buf;
					/*
					 * buffer flip, new buffer will
					 * replace currently active one,
					 * mark currently active for cleanup
					 */
					buf = list_next_entry(tmp, pipe_list);
					__pipe_buf_mark_cleanup(mfd, tmp);
				}
				break;
			default:
				pr_err("invalid state of buf %pK=%d\n",
						buf, buf->state);
				BUG();
				break;
			}
		}

		/* ensure pipes are reconfigured after power off/on */
		if (ctl->play_cnt == 0)
			pipe->params_changed++;

		if (buf && (buf->state == MDP_BUF_STATE_READY)) {
			buf->state = MDP_BUF_STATE_ACTIVE;
			ret = mdss_mdp_data_map(buf, false, DMA_TO_DEVICE);
		} else if (!pipe->params_changed &&
			   !mdss_mdp_is_roi_changed(pipe->mfd)) {

			/*
			 * no update for the given pipe nor any change in the
			 * ROI so skip pipe programming and continue with next.
			 */
			continue;
		} else if (buf) {
			BUG_ON(buf->state != MDP_BUF_STATE_ACTIVE);
			pr_debug("requeueing active buffer on pnum=%d\n",
					pipe->num);
		} else if ((pipe->flags & MDP_SOLID_FILL) == 0) {
			pr_warn("commit without buffer on pipe %d\n",
				pipe->num);
			ret = -EINVAL;
		}

		/*
		 * if we reach here without errors and buf == NULL
		 * then solid fill will be set
		 */
		if (!IS_ERR_VALUE(ret))
			ret = mdss_mdp_pipe_queue_data(pipe, buf);

		if (IS_ERR_VALUE(ret)) {
			pr_warn("Unable to queue data for pnum=%d rect=%d\n",
					pipe->num, pipe->multirect.num);

			/*
			 * If we fail for a multi-rect pipe, unstage both rects
			 * so we don't leave the pipe configured in multi-rect
			 * mode with only one rectangle staged.
			 */
			if (pipe->multirect.mode !=
					MDSS_MDP_PIPE_MULTIRECT_NONE) {
				struct mdss_mdp_pipe *next_pipe =
					(struct mdss_mdp_pipe *)
					pipe->multirect.next;

				if (next_pipe) {
					struct mdss_mdp_data *next_buf =
						list_first_entry_or_null(
							&next_pipe->buf_queue,
							struct mdss_mdp_data,
							pipe_list);

					__unstage_pipe_and_clean_buf(mfd,
						next_pipe, next_buf);
				} else {
					pr_warn("cannot find rect pnum=%d\n",
						pipe->num);
				}
			}

			__unstage_pipe_and_clean_buf(mfd, pipe, buf);
		}
	}

	return 0;
}

static void __overlay_kickoff_requeue(struct msm_fb_data_type *mfd)
{
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);

	mdss_mdp_display_commit(ctl, NULL, NULL);
	mdss_mdp_display_wait4comp(ctl);

	/* unstage any recovery pipes and re-queue used pipes */
	mdss_mdp_mixer_unstage_all(ctl->mixer_left);
	mdss_mdp_mixer_unstage_all(ctl->mixer_right);

	__overlay_queue_pipes(mfd);

	mdss_mdp_display_commit(ctl, NULL,  NULL);
	mdss_mdp_display_wait4comp(ctl);
}

static int mdss_mdp_commit_cb(enum mdp_commit_stage_type commit_stage,
	void *data)
{
	int ret = 0;
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)data;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl;

	switch (commit_stage) {
	case MDP_COMMIT_STAGE_SETUP_DONE:
		ctl = mfd_to_ctl(mfd);
		mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_CTX_DONE);
		mdp5_data->kickoff_released = true;
		mutex_unlock(&mdp5_data->ov_lock);
		break;
	case MDP_COMMIT_STAGE_READY_FOR_KICKOFF:
		mutex_lock(&mdp5_data->ov_lock);
		break;
	default:
		pr_err("Invalid commit stage %x", commit_stage);
		break;
	}

	return ret;
}

/**
 * __is_roi_valid() - Check if ctl roi is valid for a given pipe.
 * @pipe: pipe to check against.
 * @l_roi: roi of the left ctl path.
 * @r_roi: roi of the right ctl path.
 *
 * Validate roi against pipe's destination rectangle by checking following
 * conditions. If any of these conditions are met then return failure,
 * success otherwise.
 *
 * 1. Pipe has scaling and pipe's destination is intersecting with roi.
 * 2. Pipe's destination and roi do not overlap, In such cases, pipe should
 *    not be part of used list and should have been omitted by user program.
 */
static bool __is_roi_valid(struct mdss_mdp_pipe *pipe,
	struct mdss_rect *l_roi, struct mdss_rect *r_roi)
{
	bool ret = true;
	bool is_right_mixer = pipe->mixer_left->is_right_mixer;
	struct mdss_rect roi = is_right_mixer ? *r_roi : *l_roi;
	struct mdss_rect dst = pipe->dst;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	u32 left_lm_w = left_lm_w_from_mfd(pipe->mfd);

	if (pipe->src_split_req) {
		if (roi.w) {
			/* left_roi is valid */
			roi.w += r_roi->w;
		} else {
			/*
			 * if we come here then left_roi is zero but pipe's
			 * output is crossing LM boundary if it was Full Screen
			 * update. In such case, if right ROI's (x+w) is less
			 * than pipe's dst_x then #2 check will fail even
			 * though in full coordinate system it is valid.
			 * ex:
			 *    left_lm_w = 800;
			 *    pipe->dst.x = 400;
			 *    pipe->dst.w = 800;
			 *    r_roi.x + r_roi.w = 300;
			 * To avoid such pitfall, extend ROI for comparison.
			 */
			roi.w += left_lm_w + r_roi->w;
		}
	}

	if (mdata->has_src_split && is_right_mixer)
		dst.x -= left_lm_w;

	/* condition #1 above */
	if ((pipe->scaler.enable) ||
	    (pipe->src.w != dst.w) || (pipe->src.h != dst.h)) {
		struct mdss_rect res;

		mdss_mdp_intersect_rect(&res, &dst, &roi);

		if (!mdss_rect_cmp(&res, &dst)) {
			pr_err("error. pipe%d has scaling and its output is interesecting with roi.\n",
				pipe->num);
			pr_err("pipe_dst:-> %d %d %d %d roi:-> %d %d %d %d\n",
				dst.x, dst.y, dst.w, dst.h,
				roi.x, roi.y, roi.w, roi.h);
			ret = false;
			goto end;
		}
	}

	/* condition #2 above */
	if (!mdss_rect_overlap_check(&dst, &roi)) {
		pr_err("error. pipe%d's output is outside of ROI.\n",
			pipe->num);
		ret = false;
	}
end:
	return ret;
}

int mdss_mode_switch(struct msm_fb_data_type *mfd, u32 mode)
{
	struct mdss_rect l_roi, r_roi;
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *sctl;
	int rc = 0;

	pr_debug("fb%d switch to mode=%x\n", mfd->index, mode);
	ATRACE_FUNC();

	ctl->pending_mode_switch = mode;
	sctl = mdss_mdp_get_split_ctl(ctl);
	if (sctl)
		sctl->pending_mode_switch = mode;

	/* No need for mode validation. It has been done in ioctl call */
	if (mode == SWITCH_RESOLUTION) {
		if (ctl->ops.reconfigure) {
			/* wait for previous frame to complete before switch */
			if (ctl->ops.wait_pingpong)
				rc = ctl->ops.wait_pingpong(ctl, NULL);
			if (!rc && sctl && sctl->ops.wait_pingpong)
				rc = sctl->ops.wait_pingpong(sctl, NULL);
			if (rc) {
				pr_err("wait for pp failed before resolution switch\n");
				return rc;
			}

			/*
			* Configure the mixer parameters before the switch as
			* the DSC parameter calculation is based on the mixer
			* ROI. And set it to full ROI as driver expects the
			* first frame after the resolution switch to be a
			* full frame update.
			*/
			if (ctl->mixer_left) {
				l_roi = (struct mdss_rect) {0, 0,
					ctl->mixer_left->width,
					ctl->mixer_left->height};
				ctl->mixer_left->roi_changed = true;
				ctl->mixer_left->valid_roi = true;
			}
			if (ctl->mixer_right) {
				r_roi = (struct mdss_rect) {0, 0,
					ctl->mixer_right->width,
					ctl->mixer_right->height};
				ctl->mixer_right->roi_changed = true;
				ctl->mixer_right->valid_roi = true;
			}
			mdss_mdp_set_roi(ctl, &l_roi, &r_roi);

			mutex_lock(&mdp5_data->ov_lock);
			ctl->ops.reconfigure(ctl, mode, 1);
			mutex_unlock(&mdp5_data->ov_lock);
		/*
		 * For Video mode panels, reconfigure is not defined.
		 * So doing an explicit ctrl stop during resolution switch
		 * to balance the ctrl start at the end of this function.
		 */
		} else {
			mdss_mdp_ctl_stop(ctl, MDSS_PANEL_POWER_OFF);
		}
	} else if (mode == MIPI_CMD_PANEL) {
		/*
		 * Need to reset roi if there was partial update in previous
		 * Command frame
		 */
		l_roi = (struct mdss_rect){0, 0,
				ctl->mixer_left->width,
				ctl->mixer_left->height};
		if (ctl->mixer_right) {
			r_roi = (struct mdss_rect) {0, 0,
				ctl->mixer_right->width,
				ctl->mixer_right->height};
		}
		mdss_mdp_set_roi(ctl, &l_roi, &r_roi);
		mdss_mdp_switch_roi_reset(ctl);

		mdss_mdp_switch_to_cmd_mode(ctl, 1);
		mdss_mdp_update_panel_info(mfd, 1, 0);
		mdss_mdp_switch_to_cmd_mode(ctl, 0);
		mdss_mdp_ctl_stop(ctl, MDSS_PANEL_POWER_OFF);
	} else if (mode == MIPI_VIDEO_PANEL) {
		if (ctl->ops.wait_pingpong)
			rc = ctl->ops.wait_pingpong(ctl, NULL);
		mdss_mdp_update_panel_info(mfd, 0, 0);
		mdss_mdp_switch_to_vid_mode(ctl, 1);
		mdss_mdp_ctl_stop(ctl, MDSS_PANEL_POWER_OFF);
		mdss_mdp_switch_to_vid_mode(ctl, 0);
	} else {
		pr_err("Invalid mode switch arg %d\n", mode);
		return -EINVAL;
	}

	mdss_mdp_ctl_start(ctl, true);
	ATRACE_END(__func__);

	return 0;
}

int mdss_mode_switch_post(struct msm_fb_data_type *mfd, u32 mode)
{
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	struct mdss_mdp_ctl *sctl = mdss_mdp_get_split_ctl(ctl);
	struct dsi_panel_clk_ctrl clk_ctrl;
	int rc = 0;
	u32 frame_rate = 0;

	if (mode == MIPI_VIDEO_PANEL) {
		/*
		 * Need to make sure one frame has been sent in
		 * video mode prior to issuing the mode switch
		 * DCS to panel.
		 */
		frame_rate = mdss_panel_get_framerate
			(&(ctl->panel_data->panel_info));
		if (!(frame_rate >= 24 && frame_rate <= 240))
			frame_rate = 24;
		frame_rate = ((1000/frame_rate) + 1);
		msleep(frame_rate);

		pr_debug("%s, start\n", __func__);
		rc = mdss_mdp_ctl_intf_event(ctl,
			MDSS_EVENT_DSI_DYNAMIC_SWITCH,
			(void *) MIPI_VIDEO_PANEL, CTL_INTF_EVENT_FLAG_DEFAULT);
		pr_debug("%s, end\n", __func__);
	} else if (mode == MIPI_CMD_PANEL) {
		/*
		 * Needed to balance out clk refcount when going
		 * from video to command. This allows for idle
		 * power collapse to work as intended.
		 */
		clk_ctrl.state = MDSS_DSI_CLK_OFF;
		clk_ctrl.client = DSI_CLK_REQ_DSI_CLIENT;
		if (sctl)
			mdss_mdp_ctl_intf_event(sctl,
				MDSS_EVENT_PANEL_CLK_CTRL, (void *)&clk_ctrl,
				CTL_INTF_EVENT_FLAG_SKIP_BROADCAST);

		mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_PANEL_CLK_CTRL,
			(void *)&clk_ctrl, CTL_INTF_EVENT_FLAG_SKIP_BROADCAST);
	} else if (mode == SWITCH_RESOLUTION) {
		if (ctl->ops.reconfigure)
			rc = ctl->ops.reconfigure(ctl, mode, 0);
	}
	ctl->pending_mode_switch = 0;
	if (sctl)
		sctl->pending_mode_switch = 0;

	return rc;
}

static void __restore_pipe(struct mdss_mdp_pipe *pipe)
{

	if (!pipe->restore_roi)
		return;

	pr_debug("restoring pipe:%d dst from:{%d,%d,%d,%d} to:{%d,%d,%d,%d}\n",
		pipe->num, pipe->dst.x, pipe->dst.y,
		pipe->dst.w, pipe->dst.h, pipe->layer.dst_rect.x,
		pipe->layer.dst_rect.y, pipe->layer.dst_rect.w,
		pipe->layer.dst_rect.h);
	pr_debug("restoring pipe:%d src from:{%d,%d,%d,%d} to:{%d,%d,%d,%d}\n",
		pipe->num, pipe->src.x, pipe->src.y,
		pipe->src.w, pipe->src.h, pipe->layer.src_rect.x,
		pipe->layer.src_rect.y, pipe->layer.src_rect.w,
		pipe->layer.src_rect.h);

	pipe->src.x = pipe->layer.src_rect.x;
	pipe->src.y = pipe->layer.src_rect.y;
	pipe->src.w = pipe->layer.src_rect.w;
	pipe->src.h = pipe->layer.src_rect.h;

	pipe->dst.x = pipe->layer.dst_rect.x;
	pipe->dst.y = pipe->layer.dst_rect.y;
	pipe->dst.w = pipe->layer.dst_rect.w;
	pipe->dst.h = pipe->layer.dst_rect.h;

	pipe->restore_roi = false;
}

static void __restore_dest_scaler_roi(struct mdss_mdp_ctl *ctl)
{
	struct mdss_panel_info *pinfo = &ctl->panel_data->panel_info;
	struct mdss_mdp_ctl *sctl = mdss_mdp_get_split_ctl(ctl);

	if (is_dest_scaling_pu_enable(ctl->mixer_left)) {
		ctl->mixer_left->ds->panel_roi.x = 0;
		ctl->mixer_left->ds->panel_roi.y = 0;
		ctl->mixer_left->ds->panel_roi.w = get_panel_xres(pinfo);
		ctl->mixer_left->ds->panel_roi.h = get_panel_yres(pinfo);

		if ((ctl->mfd->split_mode == MDP_DUAL_LM_DUAL_DISPLAY) &&
				sctl) {
			if (ctl->panel_data->next)
				pinfo = &ctl->panel_data->next->panel_info;
			sctl->mixer_left->ds->panel_roi.x = 0;
			sctl->mixer_left->ds->panel_roi.y = 0;
			sctl->mixer_left->ds->panel_roi.w =
				get_panel_xres(pinfo);
			sctl->mixer_left->ds->panel_roi.h =
				get_panel_yres(pinfo);
		} else if (ctl->mfd->split_mode == MDP_DUAL_LM_SINGLE_DISPLAY) {
			ctl->mixer_right->ds->panel_roi.x =
				get_panel_xres(pinfo);
			ctl->mixer_right->ds->panel_roi.y = 0;
			ctl->mixer_right->ds->panel_roi.w =
				get_panel_xres(pinfo);
			ctl->mixer_right->ds->panel_roi.h =
				get_panel_yres(pinfo);
		}
	}
}

 /**
 * __adjust_pipe_rect() - Adjust pipe roi for dual partial update feature.
 * @pipe: pipe to check against.
 * @dual_roi: roi's for the dual partial roi.
 *
 * For dual PU ROI case, the layer mixer is configured
 * by merging the two width aligned ROIs (first_roi and
 * second_roi) vertically. So, the y-offset of all the
 * pipes belonging to the second_roi needs to adjusted
 * accordingly. Also check, if the pipe dest rect is
 * totally within first or second ROI.
 */
static int __adjust_pipe_rect(struct mdss_mdp_pipe *pipe,
		struct mdss_dsi_dual_pu_roi *dual_roi)
{
	u32 adjust_h;
	int ret = 0;
	struct mdss_rect res_rect;

	if (mdss_rect_overlap_check(&pipe->dst, &dual_roi->first_roi)) {
		mdss_mdp_intersect_rect(&res_rect, &pipe->dst,
				&dual_roi->first_roi);
		if (!mdss_rect_cmp(&res_rect, &pipe->dst)) {
			ret = -EINVAL;
			goto end;
		}

	} else if (mdss_rect_overlap_check(&pipe->dst, &dual_roi->second_roi)) {
		mdss_mdp_intersect_rect(&res_rect, &pipe->dst,
				&dual_roi->second_roi);
		if (!mdss_rect_cmp(&res_rect, &pipe->dst)) {
			ret = -EINVAL;
			goto end;
		}

		adjust_h = dual_roi->second_roi.y -
				(dual_roi->first_roi.y + dual_roi->first_roi.h);
		pipe->dst.y -= adjust_h;
		pipe->restore_roi = true;

	} else {
		ret = -EINVAL;
		goto end;
	}

	pr_debug("adjusted p:%d src:{%d,%d,%d,%d} dst:{%d,%d,%d,%d} r:%d\n",
		pipe->num, pipe->src.x, pipe->src.y,
		pipe->src.w, pipe->src.h, pipe->dst.x,
		pipe->dst.y, pipe->dst.w, pipe->dst.h,
		pipe->restore_roi);

end:
	if (ret) {
		pr_err("pipe:%d dst:{%d,%d,%d,%d} - not cropped for any PU ROI",
			pipe->num, pipe->dst.x, pipe->dst.y,
			pipe->dst.w, pipe->dst.h);
		pr_err("ROI0:{%d,%d,%d,%d} ROI1:{%d,%d,%d,%d}\n",
			dual_roi->first_roi.x, dual_roi->first_roi.y,
			dual_roi->first_roi.w, dual_roi->first_roi.h,
			dual_roi->second_roi.x, dual_roi->second_roi.y,
			dual_roi->second_roi.w, dual_roi->second_roi.h);
	}

	return ret;
}

static void __validate_and_set_roi(struct msm_fb_data_type *mfd,
	struct mdp_display_commit *commit)
{
	struct mdss_mdp_pipe *pipe;
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_panel_info *pinfo = &ctl->panel_data->panel_info;
	struct mdss_dsi_dual_pu_roi *dual_roi = &pinfo->dual_roi;
	struct mdss_rect l_roi = {0}, r_roi = {0};
	struct mdp_rect tmp_roi = {0};
	bool skip_partial_update = true;

	if (!commit)
		goto set_roi;

	if (!memcmp(&commit->l_roi, &tmp_roi, sizeof(tmp_roi)) &&
	    !memcmp(&commit->r_roi, &tmp_roi, sizeof(tmp_roi)))
		goto set_roi;

	rect_copy_mdp_to_mdss(&commit->l_roi, &l_roi);
	rect_copy_mdp_to_mdss(&commit->r_roi, &r_roi);

	/*
	 * In case of dual partial update ROI, update the two ROIs to dual_roi
	 * struct and combine both the ROIs and assign it as a merged ROI in
	 * l_roi, as MDP would need only the merged ROI information for all
	 * LM settings.
	 */
	if (pinfo->partial_update_enabled == PU_DUAL_ROI) {
		if (commit->flags & MDP_COMMIT_PARTIAL_UPDATE_DUAL_ROI) {

			if (!is_valid_pu_dual_roi(pinfo, &l_roi, &r_roi)) {
				pr_err("Invalid dual roi - fall back to full screen update\n");
				goto set_roi;
			}

			dual_roi->first_roi = (struct mdss_rect)
					{l_roi.x, l_roi.y, l_roi.w, l_roi.h};
			dual_roi->second_roi = (struct mdss_rect)
					{r_roi.x, r_roi.y, r_roi.w, r_roi.h};
			dual_roi->enabled = true;

			l_roi.h += r_roi.h;
			memset(&r_roi, 0, sizeof(struct mdss_rect));

			pr_debug("Dual ROI - first_roi:{%d,%d,%d,%d}, second_roi:{%d,%d,%d,%d}\n",
				dual_roi->first_roi.x, dual_roi->first_roi.y,
				dual_roi->first_roi.w, dual_roi->first_roi.h,
				dual_roi->second_roi.x, dual_roi->second_roi.y,
				dual_roi->second_roi.w, dual_roi->second_roi.h);
		} else {
			dual_roi->enabled = false;
		}
	}

	pr_debug("input: l_roi:-> %d %d %d %d r_roi:-> %d %d %d %d\n",
		l_roi.x, l_roi.y, l_roi.w, l_roi.h,
		r_roi.x, r_roi.y, r_roi.w, r_roi.h);

	/*
	 * Configure full ROI
	 * - If partial update is disabled
	 * - If it is the first frame update after dynamic resolution switch
	 */
	if (!ctl->panel_data->panel_info.partial_update_enabled
			|| (ctl->pending_mode_switch == SWITCH_RESOLUTION))
		goto set_roi;

	skip_partial_update = false;

	if (is_split_lm(mfd) && mdp5_data->mdata->has_src_split) {
		u32 left_lm_w = left_lm_w_from_mfd(mfd);
		struct mdss_rect merged_roi = l_roi;

		/*
		 * When source split is enabled on split LM displays,
		 * user program merges left and right ROI and sends
		 * it through l_roi. Split this merged ROI into
		 * left/right ROI for validation.
		 */
		mdss_rect_split(&merged_roi, &l_roi, &r_roi, left_lm_w);

		/*
		 * When source split is enabled on split LM displays,
		 * it is a HW requirement that both LM have same width
		 * if update is on both sides. Since ROIs are
		 * generated by user-land program, validate against
		 * this requirement.
		 */
		if (l_roi.w && r_roi.w && (l_roi.w != r_roi.w)) {
			pr_err("error. ROI's do not match. violating src_split requirement\n");
			pr_err("l_roi:-> %d %d %d %d r_roi:-> %d %d %d %d\n",
				l_roi.x, l_roi.y, l_roi.w, l_roi.h,
				r_roi.x, r_roi.y, r_roi.w, r_roi.h);
			skip_partial_update = true;
			goto set_roi;
		}
	}

	list_for_each_entry(pipe, &mdp5_data->pipes_used, list) {
		/*
		 * Restore the pipe src/dst ROI if it was altered
		 * in the previous kickoff.
		 */
		if (pipe->restore_roi)
			__restore_pipe(pipe);

		pr_debug("pipe:%d src:{%d,%d,%d,%d} dst:{%d,%d,%d,%d}\n",
			pipe->num, pipe->src.x, pipe->src.y,
			pipe->src.w, pipe->src.h, pipe->dst.x,
			pipe->dst.y, pipe->dst.w, pipe->dst.h);

		if (dual_roi->enabled) {
			if (__adjust_pipe_rect(pipe, dual_roi)) {
				skip_partial_update = true;
				break;
			}
		}

		if (!__is_roi_valid(pipe, &l_roi, &r_roi)) {
			skip_partial_update = true;
			pr_err("error. invalid pu config for pipe:%d dst:{%d,%d,%d,%d} dual_pu_roi:%d\n",
				pipe->num, pipe->dst.x, pipe->dst.y,
				pipe->dst.w, pipe->dst.h,
				dual_roi->enabled);
			break;
		}
	}

set_roi:
	if (skip_partial_update) {
		l_roi = (struct mdss_rect){0, 0,
				ctl->mixer_left->width,
				ctl->mixer_left->height};
		if (ctl->mixer_right) {
			r_roi = (struct mdss_rect) {0, 0,
					ctl->mixer_right->width,
					ctl->mixer_right->height};
		}

		if (pinfo->partial_update_enabled == PU_DUAL_ROI) {
			if (dual_roi->enabled) {
				/* we failed pu validation, restore pipes */
				list_for_each_entry(pipe,
					&mdp5_data->pipes_used, list)
				__restore_pipe(pipe);
			}
			dual_roi->enabled = false;
		}

		if (pinfo->partial_update_enabled)
			__restore_dest_scaler_roi(ctl);
	}

	pr_debug("after processing: %s l_roi:-> %d %d %d %d r_roi:-> %d %d %d %d, dual_pu_roi:%d\n",
		(l_roi.w && l_roi.h && r_roi.w && r_roi.h) ? "left+right" :
			((l_roi.w && l_roi.h) ? "left-only" : "right-only"),
		l_roi.x, l_roi.y, l_roi.w, l_roi.h,
		r_roi.x, r_roi.y, r_roi.w, r_roi.h,
		dual_roi->enabled);

	mdss_mdp_set_roi(ctl, &l_roi, &r_roi);
}

/*
 * For the pipe which is being used for Secure Display,
 * cleanup the previously queued buffers.
 */
static void __overlay_cleanup_secure_pipe(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *pipe;
	struct mdss_mdp_data *buf, *tmpbuf;

	list_for_each_entry(pipe, &mdp5_data->pipes_used, list) {
		if (pipe->flags & MDP_SECURE_DISPLAY_OVERLAY_SESSION) {
			list_for_each_entry_safe(buf, tmpbuf, &pipe->buf_queue,
							pipe_list) {
				if (buf->state == MDP_BUF_STATE_ACTIVE) {
					__pipe_buf_mark_cleanup(mfd, buf);
					list_move(&buf->buf_list,
						&mdp5_data->bufs_freelist);
					mdss_mdp_overlay_buf_free(mfd, buf);
				}
			}
		}
	}
}

/*
 * Check if there is any change in secure state and store it.
 */
static void __overlay_set_secure_transition_state(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *pipe;
	int sd_in_pipe = 0;
	int sc_in_pipe = 0;
	u64 pipes_flags = 0;

	list_for_each_entry(pipe, &mdp5_data->pipes_used, list) {
		pipes_flags |= pipe->flags;
		if (pipe->flags & MDP_SECURE_DISPLAY_OVERLAY_SESSION) {
			sd_in_pipe = 1;
			pr_debug("Secure pipe: %u : %16llx\n",
					pipe->num, pipe->flags);
		} else if (pipe->flags & MDP_SECURE_CAMERA_OVERLAY_SESSION) {
			sc_in_pipe = 1;
			pr_debug("Secure camera: %u: %16llx\n",
					pipe->num, pipe->flags);
		}
	}

	MDSS_XLOG(sd_in_pipe, sc_in_pipe, pipes_flags,
			mdp5_data->sc_enabled, mdp5_data->sd_enabled);
	pr_debug("sd:%d sd_in_pipe:%d sc:%d sc_in_pipe:%d flags:0x%llx\n",
			mdp5_data->sd_enabled, sd_in_pipe,
			mdp5_data->sc_enabled, sc_in_pipe, pipes_flags);

	/* Reset the secure transition state */
	mdp5_data->secure_transition_state = SECURE_TRANSITION_NONE;

	mdp5_data->cache_null_commit = list_empty(&mdp5_data->pipes_used);

	/*
	 * Secure transition would be NONE in two conditions:
	 * 1. All the features are already disabled and state remains
	 *    disabled for the pipes.
	 * 2. One of the features is already enabled and state remains
	 *    enabled for the pipes.
	 */
	if (!sd_in_pipe && !mdp5_data->sd_enabled &&
			!sc_in_pipe && !mdp5_data->sc_enabled)
		return;
	else if ((sd_in_pipe && mdp5_data->sd_enabled) ||
			(sc_in_pipe && mdp5_data->sc_enabled))
		return;

	/* Secure Display */
	if (!mdp5_data->sd_enabled && sd_in_pipe)
		mdp5_data->secure_transition_state |= SD_NON_SECURE_TO_SECURE;
	else if (mdp5_data->sd_enabled && !sd_in_pipe)
		mdp5_data->secure_transition_state |= SD_SECURE_TO_NON_SECURE;

	/* Secure Camera */
	if (!mdp5_data->sc_enabled && sc_in_pipe)
		mdp5_data->secure_transition_state |= SC_NON_SECURE_TO_SECURE;
	else if (mdp5_data->sc_enabled && !sc_in_pipe)
		mdp5_data->secure_transition_state |= SC_SECURE_TO_NON_SECURE;
}

/*
 * Enable/disable secure (display or camera) sessions
 */
static int __overlay_secure_ctrl(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	int ret = 0;

	if (mdp5_data->secure_transition_state == SECURE_TRANSITION_NONE)
		return ret;

	/* Secure Display */
	if (mdp5_data->secure_transition_state == SD_NON_SECURE_TO_SECURE) {
		if (!mdss_get_sd_client_cnt()) {
			MDSS_XLOG(0x11);
			/* wait for ping pong done */
			if (ctl->ops.wait_pingpong) {
				mdss_mdp_display_wait4pingpong(ctl, true);

				/*
				 * For command mode panels, there will not be
				 * a NULL commit preceding secure display. If
				 * a pipe is reused for secure display,
				 * cleanup buffers in the secure pipe before
				 * detaching IOMMU.
				 */
				__overlay_cleanup_secure_pipe(mfd);
			}
			/*
			 * unmap the previous commit buffers before
			 * transitioning to secure state
			 */
			mdss_mdp_overlay_cleanup(mfd,
					&mdp5_data->pipes_destroy,
					true);
			ret = mdss_mdp_secure_session_ctrl(1,
					MDP_SECURE_DISPLAY_OVERLAY_SESSION);
			if (ret) {
				pr_err("secure display enable fail:%d", ret);
				return ret;
			}
		}
		mdp5_data->sd_enabled = 1;
		mdss_update_sd_client(mdp5_data->mdata, true);
	} else if (mdp5_data->secure_transition_state ==
			SD_SECURE_TO_NON_SECURE) {
		/* disable the secure display on last client */
		if (mdss_get_sd_client_cnt() == 1) {
			MDSS_XLOG(0x22);
			if (ctl->ops.wait_pingpong)
				mdss_mdp_display_wait4pingpong(ctl, true);
			ret = mdss_mdp_secure_session_ctrl(0,
					MDP_SECURE_DISPLAY_OVERLAY_SESSION);
			if (ret) {
				pr_err("secure display disable fail:%d\n", ret);
				return ret;
			}
		}
		mdss_update_sd_client(mdp5_data->mdata, false);
		mdp5_data->sd_enabled = 0;
	}

	/* Secure Camera */
	if (mdp5_data->secure_transition_state == SC_NON_SECURE_TO_SECURE) {
		if (!mdss_get_sc_client_cnt()) {
			MDSS_XLOG(0x33);
			ret = mdss_mdp_secure_session_ctrl(1,
					MDP_SECURE_CAMERA_OVERLAY_SESSION);
			if (ret) {
				pr_err("secure camera enable fail:%d\n", ret);
				return ret;
			}
		}
		mdp5_data->sc_enabled = 1;
		mdss_update_sc_client(mdp5_data->mdata, true);
	} else if (mdp5_data->secure_transition_state ==
			SC_SECURE_TO_NON_SECURE) {
		/* disable the secure camera on last client */
		if (mdss_get_sc_client_cnt() == 1) {
			MDSS_XLOG(0x44);
			if (ctl->ops.wait_pingpong)
				mdss_mdp_display_wait4pingpong(ctl, true);
			ret = mdss_mdp_secure_session_ctrl(0,
					MDP_SECURE_CAMERA_OVERLAY_SESSION);
			if (ret) {
				pr_err("secure camera disable fail:%d\n", ret);
				return ret;
			}
		}
		mdss_update_sc_client(mdp5_data->mdata, false);
		mdp5_data->sc_enabled = 0;
	}

	MDSS_XLOG(ret);
	return ret;
}

int mdss_mdp_overlay_kickoff(struct msm_fb_data_type *mfd,
				struct mdp_display_commit *data)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *pipe, *tmp;
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	int ret = 0;
	struct mdss_mdp_commit_cb commit_cb;

	if (!ctl || !ctl->mixer_left)
		return -ENODEV;

	ATRACE_BEGIN(__func__);
	if (ctl->shared_lock) {
		mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_BEGIN);
		mutex_lock(ctl->shared_lock);
	}

	mutex_lock(&mdp5_data->ov_lock);
	ctl->bw_pending = 0;
	ret = mdss_mdp_overlay_start(mfd);
	if (ret) {
		pr_err("unable to start overlay %d (%d)\n", mfd->index, ret);
		mutex_unlock(&mdp5_data->ov_lock);
		if (ctl->shared_lock)
			mutex_unlock(ctl->shared_lock);
		return ret;
	}

	ret = mdss_iommu_ctrl(1);
	if (IS_ERR_VALUE(ret)) {
		pr_err("iommu attach failed rc=%d\n", ret);
		mutex_unlock(&mdp5_data->ov_lock);
		if (ctl->shared_lock)
			mutex_unlock(ctl->shared_lock);
		return ret;
	}

	mutex_lock(&mdp5_data->list_lock);

	if (!ctl->shared_lock)
		mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_BEGIN);

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);

	mdss_mdp_check_ctl_reset_status(ctl);
	__validate_and_set_roi(mfd, data);

	if (ctl->ops.wait_pingpong && mdp5_data->mdata->serialize_wait4pp)
		mdss_mdp_display_wait4pingpong(ctl, true);

	list_for_each_entry_safe(pipe, tmp, &mdp5_data->pipes_cleanup, list) {
		mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_left);
		mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_right);
		pipe->mixer_stage = MDSS_MDP_STAGE_UNUSED;
		list_move(&pipe->list, &mdp5_data->pipes_destroy);
	}

	__overlay_set_secure_transition_state(mfd);
	/*
	 * go to secure state if required, this should be done
	 * after moving the buffers from the previous commit to
	 * destroy list.
	 * For video mode panels, secure display/camera should be disabled
	 * after flushing the new buffer. Skip secure disable here for those
	 * cases.
	 */
	if (!((mfd->panel_info->type == MIPI_VIDEO_PANEL) &&
	    ((mdp5_data->secure_transition_state == SD_SECURE_TO_NON_SECURE) ||
	    (mdp5_data->secure_transition_state == SC_SECURE_TO_NON_SECURE)))) {
		ret = __overlay_secure_ctrl(mfd);
		if (IS_ERR_VALUE(ret)) {
			pr_err("secure operation failed %d\n", ret);
			goto commit_fail;
		}
	}

	/* call this function before any registers programming */
	if (ctl->ops.pre_programming)
		ctl->ops.pre_programming(ctl);

	ATRACE_BEGIN("sspp_programming");
	ret = __overlay_queue_pipes(mfd);
	ATRACE_END("sspp_programming");

	mutex_unlock(&mdp5_data->list_lock);

	mdp5_data->kickoff_released = false;
	ATRACE_BEGIN("dest_scaler_programming");
	ret = __dest_scaler_setup(mfd);
	ATRACE_END("dest_scaler_programming");

	if (mfd->panel.type == WRITEBACK_PANEL) {
		ATRACE_BEGIN("wb_kickoff");
		commit_cb.commit_cb_fnc = mdss_mdp_commit_cb;
		commit_cb.data = mfd;
		ret = mdss_mdp_wfd_kickoff(mdp5_data->wfd, &commit_cb);
		ATRACE_END("wb_kickoff");
	} else {
		ATRACE_BEGIN("display_commit");
		commit_cb.commit_cb_fnc = mdss_mdp_commit_cb;
		commit_cb.data = mfd;
		ret = mdss_mdp_display_commit(mdp5_data->ctl, NULL,
			&commit_cb);
		ATRACE_END("display_commit");
	}
	__vsync_set_vsync_handler(mfd);

	/*
	 * release the commit pending flag; we are releasing this flag
	 * after the commit, since now the transaction status
	 * in the cmd mode controllers is busy.
	 */
	mfd->atomic_commit_pending = false;

	if (!mdp5_data->kickoff_released)
		mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_CTX_DONE);

	if (IS_ERR_VALUE(ret))
		goto commit_fail;

	mutex_unlock(&mdp5_data->ov_lock);
	mdss_mdp_overlay_update_pm(mdp5_data);

	ATRACE_BEGIN("display_wait4comp");
	ret = mdss_mdp_display_wait4comp(mdp5_data->ctl);
	ATRACE_END("display_wait4comp");
	mdss_mdp_splash_cleanup(mfd, true);

	/*
	 * Wait for pingpong done only during resume for
	 * command mode panels. Ensure that one commit is
	 * sent before kickoff completes so that backlight
	 * update happens after it.
	 */
	if (mdss_fb_is_power_off(mfd) &&
		mfd->panel_info->type == MIPI_CMD_PANEL) {
		pr_debug("wait for pp done after resume for cmd mode\n");
		mdss_mdp_display_wait4pingpong(ctl, true);
	}

	/*
	 * Configure Timing Engine, if new fps was set.
	 * We need to do this after the wait for vsync
	 * to guarantee that mdp flush bit and dsi flush
	 * bit are set within the same vsync period
	 * regardless of  mdp revision.
	 */
	ATRACE_BEGIN("fps_update");
	ret = mdss_mdp_ctl_update_fps(ctl);
	ATRACE_END("fps_update");

	if (IS_ERR_VALUE(ret)) {
		pr_err("failed to update fps!\n");
		goto commit_fail;
	}

	ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_DSI_DYNAMIC_BITCLK,
		NULL, CTL_INTF_EVENT_FLAG_SKIP_BROADCAST);
	if (IS_ERR_VALUE(ret)) {
		pr_err("failed to update dynamic bit clk!\n");
		goto commit_fail;
	}

	mutex_lock(&mdp5_data->ov_lock);

	/* Disable secure display/camera for video mode panels */
	if ((mfd->panel_info->type == MIPI_VIDEO_PANEL) &&
	    ((mdp5_data->secure_transition_state == SD_SECURE_TO_NON_SECURE) ||
	    (mdp5_data->secure_transition_state == SC_SECURE_TO_NON_SECURE))) {
		ret = __overlay_secure_ctrl(mfd);
		if (IS_ERR_VALUE(ret)) {
			pr_err("secure operation failed %d\n", ret);
			goto commit_fail;
		}
	}

	mdss_fb_update_notify_update(mfd);
commit_fail:
	ATRACE_BEGIN("overlay_cleanup");
	mdss_mdp_overlay_cleanup(mfd, &mdp5_data->pipes_destroy, false);
	ATRACE_END("overlay_cleanup");
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);
	mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_FLUSHED);
	if (!mdp5_data->kickoff_released)
		mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_CTX_DONE);

	mutex_unlock(&mdp5_data->ov_lock);
	if (ctl->shared_lock)
		mutex_unlock(ctl->shared_lock);
	mdss_iommu_ctrl(0);
	ATRACE_END(__func__);

	return ret;
}

int mdss_mdp_overlay_release(struct msm_fb_data_type *mfd, int ndx)
{
	struct mdss_mdp_pipe *pipe, *tmp;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	u32 unset_ndx = 0;

	mutex_lock(&mdp5_data->list_lock);
	list_for_each_entry_safe(pipe, tmp, &mdp5_data->pipes_used, list) {
		if (pipe->ndx & ndx) {
			if (mdss_mdp_pipe_map(pipe)) {
				pr_err("Unable to map used pipe%d ndx=%x\n",
						pipe->num, pipe->ndx);
				continue;
			}

			unset_ndx |= pipe->ndx;

			pipe->file = NULL;
			list_move(&pipe->list, &mdp5_data->pipes_cleanup);

			mdss_mdp_pipe_unmap(pipe);

			if (unset_ndx == ndx)
				break;
		}
	}
	mutex_unlock(&mdp5_data->list_lock);

	if (unset_ndx != ndx) {
		pr_warn("Unable to unset pipe(s) ndx=0x%x unset=0x%x\n",
				ndx, unset_ndx);
		return -ENOENT;
	}

	return 0;
}

static int mdss_mdp_overlay_unset(struct msm_fb_data_type *mfd, int ndx)
{
	int ret = 0;
	struct mdss_overlay_private *mdp5_data;

	if (!mfd)
		return -ENODEV;

	mdp5_data = mfd_to_mdp5_data(mfd);

	if (!mdp5_data || !mdp5_data->ctl)
		return -ENODEV;

	ret = mutex_lock_interruptible(&mdp5_data->ov_lock);
	if (ret)
		return ret;

	if (ndx == BORDERFILL_NDX) {
		pr_debug("borderfill disable\n");
		mdp5_data->borderfill_enable = false;
		ret = 0;
		goto done;
	}

	if (mdss_fb_is_power_off(mfd)) {
		ret = -EPERM;
		goto done;
	}

	pr_debug("unset ndx=%x\n", ndx);

	ret = mdss_mdp_overlay_release(mfd, ndx);

done:
	mutex_unlock(&mdp5_data->ov_lock);

	return ret;
}

/**
 * mdss_mdp_overlay_release_all() - release any overlays associated with fb dev
 * @mfd:	Msm frame buffer structure associated with fb device
 * @release_all: ignore pid and release all the pipes
 *
 * Release any resources allocated by calling process, this can be called
 * on fb_release to release any overlays/rotator sessions left open.
 *
 * Return number of resources released
 */
static int __mdss_mdp_overlay_release_all(struct msm_fb_data_type *mfd,
	struct file *file)
{
	struct mdss_mdp_pipe *pipe, *tmp;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	u32 unset_ndx = 0;
	int cnt = 0;

	pr_debug("releasing all resources for fb%d file:%pK\n",
		mfd->index, file);

	mutex_lock(&mdp5_data->ov_lock);
	mutex_lock(&mdp5_data->list_lock);
	if (!mfd->ref_cnt && !list_empty(&mdp5_data->pipes_cleanup)) {
		pr_debug("fb%d:: free pipes present in cleanup list",
			mfd->index);
		cnt++;
	}

	list_for_each_entry_safe(pipe, tmp, &mdp5_data->pipes_used, list) {
		if (!file || pipe->file == file) {
			unset_ndx |= pipe->ndx;
			pipe->file = NULL;
			list_move(&pipe->list, &mdp5_data->pipes_cleanup);
			cnt++;
		}
	}

	pr_debug("mfd->ref_cnt=%d unset_ndx=0x%x cnt=%d\n",
		mfd->ref_cnt, unset_ndx, cnt);

	mutex_unlock(&mdp5_data->list_lock);
	mutex_unlock(&mdp5_data->ov_lock);

	return cnt;
}

static int mdss_mdp_overlay_queue(struct msm_fb_data_type *mfd,
				  struct msmfb_overlay_data *req)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *pipe;
	struct mdss_mdp_data *src_data;
	struct mdp_layer_buffer buffer;
	int ret;
	u64 flags;

	pipe = __overlay_find_pipe(mfd, req->id);
	if (!pipe) {
		pr_err("pipe ndx=%x doesn't exist\n", req->id);
		return -ENODEV;
	}

	if (pipe->dirty) {
		pr_warn("dirty pipe, will not queue pipe pnum=%d\n", pipe->num);
		return -ENODEV;
	}

	ret = mdss_mdp_pipe_map(pipe);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to map used pipe%d ndx=%x\n",
				pipe->num, pipe->ndx);
		return ret;
	}

	pr_debug("ov queue pnum=%d\n", pipe->num);

	if (pipe->flags & MDP_SOLID_FILL)
		pr_warn("Unexpected buffer queue to a solid fill pipe\n");

	flags = (pipe->flags & (MDP_SECURE_OVERLAY_SESSION |
		MDP_SECURE_DISPLAY_OVERLAY_SESSION |
		MDP_SECURE_CAMERA_OVERLAY_SESSION));

	mutex_lock(&mdp5_data->list_lock);
	src_data = mdss_mdp_overlay_buf_alloc(mfd, pipe);
	if (!src_data) {
		pr_err("unable to allocate source buffer\n");
		ret = -ENOMEM;
	} else {
		buffer.width = pipe->img_width;
		buffer.height = pipe->img_height;
		buffer.format = pipe->src_fmt->format;
		ret = mdss_mdp_data_get_and_validate_size(src_data, &req->data,
			1, flags, &mfd->pdev->dev, false, DMA_TO_DEVICE,
			&buffer);
		if (IS_ERR_VALUE(ret)) {
			mdss_mdp_overlay_buf_free(mfd, src_data);
			pr_err("src_data pmem error\n");
		}
	}
	mutex_unlock(&mdp5_data->list_lock);

	mdss_mdp_pipe_unmap(pipe);

	return ret;
}

static int mdss_mdp_overlay_play(struct msm_fb_data_type *mfd,
				 struct msmfb_overlay_data *req)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret = 0;

	pr_debug("play req id=%x\n", req->id);

	ret = mutex_lock_interruptible(&mdp5_data->ov_lock);
	if (ret)
		return ret;

	if (mdss_fb_is_power_off(mfd)) {
		ret = -EPERM;
		goto done;
	}

	if (req->id == BORDERFILL_NDX) {
		pr_debug("borderfill enable\n");
		mdp5_data->borderfill_enable = true;
		ret = mdss_mdp_overlay_free_fb_pipe(mfd);
	} else {
		ret = mdss_mdp_overlay_queue(mfd, req);
	}

done:
	mutex_unlock(&mdp5_data->ov_lock);

	return ret;
}

static int mdss_mdp_overlay_free_fb_pipe(struct msm_fb_data_type *mfd)
{
	struct mdss_mdp_pipe *pipe;
	u32 fb_ndx = 0;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	pipe = mdss_mdp_get_staged_pipe(mdp5_data->ctl,
		MDSS_MDP_MIXER_MUX_LEFT, MDSS_MDP_STAGE_BASE, false);
	if (pipe)
		fb_ndx |= pipe->ndx;

	pipe = mdss_mdp_get_staged_pipe(mdp5_data->ctl,
		MDSS_MDP_MIXER_MUX_RIGHT, MDSS_MDP_STAGE_BASE, false);
	if (pipe)
		fb_ndx |= pipe->ndx;

	if (fb_ndx) {
		pr_debug("unstaging framebuffer pipes %x\n", fb_ndx);
		mdss_mdp_overlay_release(mfd, fb_ndx);
	}
	return 0;
}

static int mdss_mdp_overlay_get_fb_pipe(struct msm_fb_data_type *mfd,
					struct mdss_mdp_pipe **ppipe,
					int mixer_mux, bool *pipe_allocated)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *pipe;
	int ret = 0;
	struct mdp_overlay *req = NULL;

	*pipe_allocated = false;
	pipe = mdss_mdp_get_staged_pipe(mdp5_data->ctl, mixer_mux,
		MDSS_MDP_STAGE_BASE, false);

	if (pipe == NULL) {
		struct fb_info *fbi = mfd->fbi;
		struct mdss_mdp_mixer *mixer;
		int bpp;
		bool rotate_180 = (fbi->var.rotate == FB_ROTATE_UD);
		struct mdss_data_type *mdata = mfd_to_mdata(mfd);
		bool split_lm = (fbi->var.xres > mdata->max_mixer_width ||
			is_split_lm(mfd));
		struct mdp_rect left_rect, right_rect;

		mixer = mdss_mdp_mixer_get(mdp5_data->ctl,
					MDSS_MDP_MIXER_MUX_LEFT);
		if (!mixer) {
			pr_err("unable to retrieve mixer\n");
			return -ENODEV;
		}

		req = kzalloc(sizeof(struct mdp_overlay), GFP_KERNEL);
		if (!req) {
			pr_err("not able to allocate memory for req\n");
			return -ENOMEM;
		}

		bpp = fbi->var.bits_per_pixel / 8;
		req->id = MSMFB_NEW_REQUEST;
		req->flags |= MDP_OV_PIPE_FORCE_DMA;
		req->src.format = mfd->fb_imgType;
		req->src.height = fbi->var.yres;
		req->src.width = fbi->fix.line_length / bpp;

		left_rect.x = 0;
		left_rect.w = MIN(fbi->var.xres, mixer->width);
		left_rect.y = 0;
		left_rect.h = req->src.height;

		right_rect.x = mixer->width;
		right_rect.w = fbi->var.xres - mixer->width;
		right_rect.y = 0;
		right_rect.h = req->src.height;

		if (mixer_mux == MDSS_MDP_MIXER_MUX_RIGHT) {
			if (req->src.width <= mixer->width) {
				pr_warn("right fb pipe not needed\n");
				ret = -EINVAL;
				goto done;
			}
			req->src_rect = req->dst_rect = right_rect;
			if (split_lm && rotate_180)
				req->src_rect = left_rect;
		} else {
			req->src_rect = req->dst_rect = left_rect;
			if (split_lm && rotate_180)
				req->src_rect = right_rect;
		}

		if (mfd->panel_orientation == MDP_ROT_180) {
			if (mixer_mux == MDSS_MDP_MIXER_MUX_RIGHT) {
				req->src_rect.x = 0;
				req->dst_rect.x = mixer->width;
			} else {
				req->src_rect.x = (split_lm) ? mixer->width : 0;
				req->dst_rect.x = 0;
			}
		}

		req->z_order = MDSS_MDP_STAGE_BASE;
		if (rotate_180)
			req->flags |= (MDP_FLIP_LR | MDP_FLIP_UD);

		pr_debug("allocating base pipe mux=%d\n", mixer_mux);

		ret = mdss_mdp_overlay_pipe_setup(mfd, req, &pipe, NULL,
			false);
		if (ret)
			goto done;

		*pipe_allocated = true;
	}
	pr_debug("ctl=%d pnum=%d\n", mdp5_data->ctl->num, pipe->num);

	*ppipe = pipe;

done:
	kfree(req);
	return ret;
}

static void mdss_mdp_overlay_pan_display(struct msm_fb_data_type *mfd)
{
	struct mdss_mdp_data *buf_l = NULL, *buf_r = NULL;
	struct mdss_mdp_pipe *l_pipe, *r_pipe;
	struct fb_info *fbi;
	struct mdss_overlay_private *mdp5_data;
	struct mdss_data_type *mdata;
	u32 offset;
	int bpp, ret;
	bool l_pipe_allocated = false, r_pipe_allocated = false;

	if (!mfd || !mfd->mdp.private1)
		return;

	mdata = mfd_to_mdata(mfd);
	fbi = mfd->fbi;
	mdp5_data = mfd_to_mdp5_data(mfd);

	if (!mdp5_data || !mdp5_data->ctl)
		return;

	/*
	 * Ignore writeback updates through pan_display as output
	 * buffer is not available.
	 */
	if (mfd->panel_info->type == WRITEBACK_PANEL) {
		pr_err_once("writeback update not supported through pan display\n");
		return;
	}

	if (IS_ERR_OR_NULL(mfd->fbmem_buf) || fbi->fix.smem_len == 0 ||
		mdp5_data->borderfill_enable) {
		mfd->mdp.kickoff_fnc(mfd, NULL);
		return;
	}

	if (mutex_lock_interruptible(&mdp5_data->ov_lock))
		return;

	if ((mdss_fb_is_power_off(mfd)) &&
		!((mfd->dcm_state == DCM_ENTER) &&
		(mfd->panel.type == MIPI_CMD_PANEL))) {
		mutex_unlock(&mdp5_data->ov_lock);
		return;
	}

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);

	bpp = fbi->var.bits_per_pixel / 8;
	offset = fbi->var.xoffset * bpp +
		 fbi->var.yoffset * fbi->fix.line_length;

	if (offset > fbi->fix.smem_len) {
		pr_err("invalid fb offset=%u total length=%u\n",
		       offset, fbi->fix.smem_len);
		goto clk_disable;
	}

	ret = mdss_mdp_overlay_get_fb_pipe(mfd, &l_pipe,
		MDSS_MDP_MIXER_MUX_LEFT, &l_pipe_allocated);
	if (ret) {
		pr_err("unable to allocate base pipe\n");
		goto pipe_release;
	}

	if (l_pipe_allocated &&
			(l_pipe->multirect.num == MDSS_MDP_PIPE_RECT1)) {
		pr_err("Invalid: L_Pipe-%d is assigned for RECT-%d\n",
				l_pipe->num, l_pipe->multirect.num);
		goto pipe_release;
	}

	if (mdss_mdp_pipe_map(l_pipe)) {
		pr_err("unable to map base pipe\n");
		goto pipe_release;
	}

	ret = mdss_mdp_overlay_start(mfd);
	if (ret) {
		pr_err("unable to start overlay %d (%d)\n", mfd->index, ret);
		goto pipe_release;
	}

	ret = mdss_iommu_ctrl(1);
	if (IS_ERR_VALUE(ret)) {
		pr_err("IOMMU attach failed\n");
		goto iommu_disable;
	}

	buf_l = __mdp_overlay_buf_alloc(mfd, l_pipe);
	if (!buf_l) {
		pr_err("unable to allocate memory for fb buffer\n");
		mdss_mdp_pipe_unmap(l_pipe);
		goto iommu_disable;
	}

	buf_l->p[0].srcp_table = mfd->fb_table;
	buf_l->p[0].srcp_dma_buf = mfd->fbmem_buf;
	buf_l->p[0].len = 0;
	buf_l->p[0].addr = 0;
	buf_l->p[0].offset = offset;
	buf_l->p[0].skip_detach = true;
	buf_l->p[0].mapped = false;
	buf_l->num_planes = 1;

	mdss_mdp_pipe_unmap(l_pipe);

	if (fbi->var.xres > mdata->max_pipe_width || is_split_lm(mfd)) {
		/*
		 * TODO: Need to revisit the function for panels with width more
		 * than max_pipe_width and less than max_mixer_width.
		 */
		ret = mdss_mdp_overlay_get_fb_pipe(mfd, &r_pipe,
			MDSS_MDP_MIXER_MUX_RIGHT, &r_pipe_allocated);
		if (ret) {
			pr_err("unable to allocate right base pipe\n");
			goto iommu_disable;
		}

		if (l_pipe_allocated && r_pipe_allocated &&
				(l_pipe->num != r_pipe->num) &&
				(r_pipe->multirect.num ==
				 MDSS_MDP_PIPE_RECT1)) {
			pr_err("Invalid: L_Pipe-%d,RECT-%d R_Pipe-%d,RECT-%d\n",
					l_pipe->num, l_pipe->multirect.num,
					r_pipe->num, l_pipe->multirect.num);
			goto iommu_disable;
		}

		if (mdss_mdp_pipe_map(r_pipe)) {
			pr_err("unable to map right base pipe\n");
			goto iommu_disable;
		}

		buf_r = __mdp_overlay_buf_alloc(mfd, r_pipe);
		if (!buf_r) {
			pr_err("unable to allocate memory for fb buffer\n");
			mdss_mdp_pipe_unmap(r_pipe);
			goto iommu_disable;
		}

		buf_r->p[0] = buf_l->p[0];
		buf_r->num_planes = 1;

		mdss_mdp_pipe_unmap(r_pipe);
	}
	mutex_unlock(&mdp5_data->ov_lock);

	if ((fbi->var.activate & FB_ACTIVATE_VBL) ||
	    (fbi->var.activate & FB_ACTIVATE_FORCE))
		mfd->mdp.kickoff_fnc(mfd, NULL);

	mdss_iommu_ctrl(0);
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);
	return;

iommu_disable:
	mdss_iommu_ctrl(0);

pipe_release:
	if (r_pipe_allocated)
		mdss_mdp_overlay_release(mfd, r_pipe->ndx);
	if (buf_l)
		__mdp_overlay_buf_free(mfd, buf_l);
	if (l_pipe_allocated)
		mdss_mdp_overlay_release(mfd, l_pipe->ndx);
clk_disable:
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);
	mutex_unlock(&mdp5_data->ov_lock);
}

static void remove_underrun_vsync_handler(struct work_struct *work)
{
	int rc;
	struct mdss_mdp_ctl *ctl =
		container_of(work, typeof(*ctl), remove_underrun_handler);

	if (!ctl || !ctl->ops.remove_vsync_handler) {
		pr_err("ctl or vsync handler is NULL\n");
		return;
	}

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);
	rc = ctl->ops.remove_vsync_handler(ctl,
			&ctl->recover_underrun_handler);
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);
}

static void mdss_mdp_recover_underrun_handler(struct mdss_mdp_ctl *ctl,
						ktime_t t)
{
	if (!ctl) {
		pr_err("ctl is NULL\n");
		return;
	}

	mdss_mdp_ctl_reset(ctl, true);
	schedule_work(&ctl->remove_underrun_handler);
}

/* function is called in irq context should have minimum processing */
static void mdss_mdp_overlay_handle_vsync(struct mdss_mdp_ctl *ctl,
						ktime_t t)
{
	struct msm_fb_data_type *mfd = NULL;
	struct mdss_overlay_private *mdp5_data = NULL;

	if (!ctl) {
		pr_err("ctl is NULL\n");
		return;
	}

	mfd = ctl->mfd;
	if (!mfd || !mfd->mdp.private1) {
		pr_warn("Invalid handle for vsync\n");
		return;
	}

	mdp5_data = mfd_to_mdp5_data(mfd);
	if (!mdp5_data) {
		pr_err("mdp5_data is NULL\n");
		return;
	}

	pr_debug("vsync on fb%d play_cnt=%d\n", mfd->index, ctl->play_cnt);

	mdp5_data->vsync_time = t;
	sysfs_notify_dirent(mdp5_data->vsync_event_sd);
}

/* function is called in irq context should have minimum processing */
static void mdss_mdp_overlay_handle_lineptr(struct mdss_mdp_ctl *ctl,
						ktime_t t)
{
	struct mdss_overlay_private *mdp5_data = NULL;

	if (!ctl || !ctl->mfd) {
		pr_warn("Invalid handle for lineptr\n");
		return;
	}

	mdp5_data = mfd_to_mdp5_data(ctl->mfd);
	if (!mdp5_data) {
		pr_err("mdp5_data is NULL\n");
		return;
	}

	pr_debug("lineptr irq on fb%d play_cnt=%d\n",
			ctl->mfd->index, ctl->play_cnt);

	mdp5_data->lineptr_time = t;
	sysfs_notify_dirent(mdp5_data->lineptr_event_sd);
}

int mdss_mdp_overlay_vsync_ctrl(struct msm_fb_data_type *mfd, int en)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	int rc;

	if (!ctl)
		return -ENODEV;

	mutex_lock(&mdp5_data->ov_lock);
	if (!ctl->ops.add_vsync_handler || !ctl->ops.remove_vsync_handler) {
		rc = -EOPNOTSUPP;
		pr_err_once("fb%d vsync handlers are not registered\n",
			mfd->index);
		goto end;
	}

	mdp5_data->vsync_en = en;

	if (!ctl->panel_data->panel_info.cont_splash_enabled
		&& (!mdss_mdp_ctl_is_power_on(ctl) ||
		mdss_panel_is_power_on_ulp(ctl->power_state))) {
		pr_debug("fb%d vsync pending first update en=%d, ctl power state:%d\n",
				mfd->index, en, ctl->power_state);
		rc = 0;
		goto end;
	}

	pr_debug("fb%d vsync en=%d\n", mfd->index, en);

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);
	if (en)
		rc = ctl->ops.add_vsync_handler(ctl, &ctl->vsync_handler);
	else
		rc = ctl->ops.remove_vsync_handler(ctl, &ctl->vsync_handler);
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);

end:
	mutex_unlock(&mdp5_data->ov_lock);
	return rc;
}

static ssize_t dynamic_fps_sysfs_rda_dfps(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t ret;
	struct mdss_panel_data *pdata;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	if (!mdp5_data->ctl || !mdss_mdp_ctl_is_power_on(mdp5_data->ctl))
		return 0;

	pdata = dev_get_platdata(&mfd->pdev->dev);
	if (!pdata) {
		pr_err("no panel connected for fb%d\n", mfd->index);
		return -ENODEV;
	}

	mutex_lock(&mdp5_data->dfps_lock);
	ret = snprintf(buf, PAGE_SIZE, "%d\n",
		       pdata->panel_info.mipi.frame_rate);
	pr_debug("%s: '%d'\n", __func__,
		pdata->panel_info.mipi.frame_rate);
	mutex_unlock(&mdp5_data->dfps_lock);

	return ret;
} /* dynamic_fps_sysfs_rda_dfps */

static int calc_extra_blanking(struct mdss_panel_data *pdata, u32 new_fps)
{
	int add_porches, diff;

	/* calculate extra: lines for vfp-method, pixels for hfp-method */
	diff = abs(pdata->panel_info.default_fps - new_fps);
	add_porches = mult_frac(pdata->panel_info.saved_total,
		diff, new_fps);

	return add_porches;
}

static void cache_initial_timings(struct mdss_panel_data *pdata)
{
	if (!pdata->panel_info.default_fps) {

		/*
		 * This value will change dynamically once the
		 * actual dfps update happen in hw.
		 */
		if (pdata->panel_info.type == DTV_PANEL)
			pdata->panel_info.current_fps =
				pdata->panel_info.lcdc.frame_rate;
		else
			pdata->panel_info.current_fps =
				mdss_panel_get_framerate(&pdata->panel_info);
		/*
		 * Keep the initial fps and porch values for this panel before
		 * any dfps update happen, this is to prevent losing precision
		 * in further calculations.
		 */
		if (pdata->panel_info.type == DTV_PANEL)
			pdata->panel_info.default_fps =
				pdata->panel_info.lcdc.frame_rate;
		else
			pdata->panel_info.default_fps =
				mdss_panel_get_framerate(&pdata->panel_info);

		if (pdata->panel_info.dfps_update ==
					DFPS_IMMEDIATE_PORCH_UPDATE_MODE_VFP) {
			pdata->panel_info.saved_total =
				mdss_panel_get_vtotal(&pdata->panel_info);
			pdata->panel_info.saved_fporch =
				pdata->panel_info.lcdc.v_front_porch;

		} else if (pdata->panel_info.dfps_update ==
				DFPS_IMMEDIATE_PORCH_UPDATE_MODE_HFP ||
			pdata->panel_info.dfps_update ==
				DFPS_IMMEDIATE_MULTI_UPDATE_MODE_CLK_HFP ||
			pdata->panel_info.dfps_update ==
				DFPS_IMMEDIATE_MULTI_MODE_HFP_CALC_CLK) {
			pdata->panel_info.saved_total =
				mdss_panel_get_htotal(&pdata->panel_info, true);
			pdata->panel_info.saved_fporch =
				pdata->panel_info.lcdc.h_front_porch;
		}
	}
}

static inline void dfps_update_fps(struct mdss_panel_info *pinfo, u32 fps)
{
	if (pinfo->type == DTV_PANEL)
		pinfo->lcdc.frame_rate = fps;
	else
		pinfo->mipi.frame_rate = fps;
}

static void dfps_update_panel_params(struct mdss_panel_data *pdata,
	struct dynamic_fps_data *data)
{
	u32 new_fps = data->fps;

	/* Keep initial values before any dfps update */
	cache_initial_timings(pdata);

	if (pdata->panel_info.dfps_update ==
			DFPS_IMMEDIATE_PORCH_UPDATE_MODE_VFP) {
		int add_v_lines;

		/* calculate extra vfp lines */
		add_v_lines = calc_extra_blanking(pdata, new_fps);

		/* update panel info with new values */
		pdata->panel_info.lcdc.v_front_porch =
			pdata->panel_info.saved_fporch + add_v_lines;

		dfps_update_fps(&pdata->panel_info, new_fps);

		pdata->panel_info.prg_fet =
			mdss_mdp_get_prefetch_lines(&pdata->panel_info, false);

	} else if (pdata->panel_info.dfps_update ==
			DFPS_IMMEDIATE_PORCH_UPDATE_MODE_HFP) {
		int add_h_pixels;

		/* calculate extra hfp pixels */
		add_h_pixels = calc_extra_blanking(pdata, new_fps);

		/* update panel info */
		if (pdata->panel_info.default_fps > new_fps)
			pdata->panel_info.lcdc.h_front_porch =
				pdata->panel_info.saved_fporch + add_h_pixels;
		else
			pdata->panel_info.lcdc.h_front_porch =
				pdata->panel_info.saved_fporch - add_h_pixels;

		dfps_update_fps(&pdata->panel_info, new_fps);
	} else if (pdata->panel_info.dfps_update ==
		DFPS_IMMEDIATE_MULTI_UPDATE_MODE_CLK_HFP) {

		pr_debug("hfp=%d, hbp=%d, hpw=%d, clk=%d, fps=%d\n",
			data->hfp, data->hbp, data->hpw,
			data->clk_rate, data->fps);

		pdata->panel_info.lcdc.h_front_porch = data->hfp;
		pdata->panel_info.lcdc.h_back_porch  = data->hbp;
		pdata->panel_info.lcdc.h_pulse_width = data->hpw;

		pdata->panel_info.clk_rate = data->clk_rate;
		if (pdata->panel_info.type == DTV_PANEL)
			pdata->panel_info.clk_rate *= 1000;

		dfps_update_fps(&pdata->panel_info, new_fps);
	} else if (pdata->panel_info.dfps_update ==
		DFPS_IMMEDIATE_MULTI_MODE_HFP_CALC_CLK) {

		pr_debug("hfp=%d, hbp=%d, hpw=%d, clk=%d, fps=%d\n",
			data->hfp, data->hbp, data->hpw,
			data->clk_rate, data->fps);

		pdata->panel_info.lcdc.h_front_porch = data->hfp;
		pdata->panel_info.lcdc.h_back_porch  = data->hbp;
		pdata->panel_info.lcdc.h_pulse_width = data->hpw;

		pdata->panel_info.clk_rate = data->clk_rate;

		dfps_update_fps(&pdata->panel_info, new_fps);
		mdss_panel_update_clk_rate(&pdata->panel_info, new_fps);
	} else {
		dfps_update_fps(&pdata->panel_info, new_fps);
		mdss_panel_update_clk_rate(&pdata->panel_info, new_fps);
	}
}

int mdss_mdp_dfps_update_params(struct msm_fb_data_type *mfd,
	struct mdss_panel_data *pdata, struct dynamic_fps_data *dfps_data)
{
	struct fb_var_screeninfo *var = &mfd->fbi->var;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	u32 dfps = dfps_data->fps;

	mutex_lock(&mdp5_data->dfps_lock);

	pr_debug("new_fps:%d\n", dfps);

	if (dfps < pdata->panel_info.min_fps) {
		pr_err("Unsupported FPS. min_fps = %d\n",
				pdata->panel_info.min_fps);
		mutex_unlock(&mdp5_data->dfps_lock);
		return -EINVAL;
	} else if (dfps > pdata->panel_info.max_fps) {
		pr_warn("Unsupported FPS. Configuring to max_fps = %d\n",
				pdata->panel_info.max_fps);
		dfps = pdata->panel_info.max_fps;
		dfps_data->fps = dfps;
	}

	dfps_update_panel_params(pdata, dfps_data);
	if (pdata->next)
		dfps_update_panel_params(pdata->next, dfps_data);

	/*
	 * Update the panel info in the upstream
	 * data, so any further call to get the screen
	 * info has the updated timings.
	 */
	mdss_panelinfo_to_fb_var(&pdata->panel_info, var);

	MDSS_XLOG(dfps);
	mutex_unlock(&mdp5_data->dfps_lock);

	return 0;
}


static ssize_t dynamic_fps_sysfs_wta_dfps(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int panel_fps, rc = 0;
	struct mdss_panel_data *pdata;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct dynamic_fps_data data = {0};

	if (!mdp5_data->ctl || !mdss_mdp_ctl_is_power_on(mdp5_data->ctl) ||
			mdss_panel_is_power_off(mfd->panel_power_state)) {
		pr_debug("panel is off\n");
		return count;
	}

	pdata = dev_get_platdata(&mfd->pdev->dev);
	if (!pdata) {
		pr_err("no panel connected for fb%d\n", mfd->index);
		return -ENODEV;
	}

	if (!pdata->panel_info.dynamic_fps) {
		pr_err_once("%s: Dynamic fps not enabled for this panel\n",
				__func__);
		return -EINVAL;
	}

	if (pdata->panel_info.dfps_update ==
		DFPS_IMMEDIATE_MULTI_UPDATE_MODE_CLK_HFP ||
		pdata->panel_info.dfps_update ==
		DFPS_IMMEDIATE_MULTI_MODE_HFP_CALC_CLK) {
		if (sscanf(buf, "%u %u %u %u %u",
		    &data.hfp, &data.hbp, &data.hpw,
		    &data.clk_rate, &data.fps) != 5) {
			pr_err("could not read input\n");
			return -EINVAL;
		}
	} else {
		rc = kstrtoint(buf, 10, &data.fps);
		if (rc) {
			pr_err("%s: kstrtoint failed. rc=%d\n", __func__, rc);
			return rc;
		}
	}

	if (pdata->panel_info.type == DTV_PANEL)
		panel_fps = pdata->panel_info.lcdc.frame_rate;
	else
		panel_fps = mdss_panel_get_framerate(&pdata->panel_info);

	if (data.fps == panel_fps) {
		pr_debug("%s: FPS is already %d\n",
			__func__, data.fps);
		return count;
	}

	if (data.hfp > DFPS_DATA_MAX_HFP || data.hbp > DFPS_DATA_MAX_HBP ||
		data.hpw > DFPS_DATA_MAX_HPW || data.fps > DFPS_DATA_MAX_FPS ||
		data.clk_rate > DFPS_DATA_MAX_CLK_RATE){
		pr_err("Data values out of bound.\n");
		return -EINVAL;
	}

	rc = mdss_mdp_dfps_update_params(mfd, pdata, &data);
	if (rc) {
		pr_err("failed to set dfps params\n");
		return rc;
	}

	return count;
} /* dynamic_fps_sysfs_wta_dfps */


static DEVICE_ATTR(dynamic_fps, S_IRUGO | S_IWUSR, dynamic_fps_sysfs_rda_dfps,
	dynamic_fps_sysfs_wta_dfps);

static struct attribute *dynamic_fps_fs_attrs[] = {
	&dev_attr_dynamic_fps.attr,
	NULL,
};
static struct attribute_group dynamic_fps_fs_attrs_group = {
	.attrs = dynamic_fps_fs_attrs,
};

static ssize_t mdss_mdp_vsync_show_event(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	u64 vsync_ticks;
	int ret;

	if (!mdp5_data->ctl ||
		(!mdp5_data->ctl->panel_data->panel_info.cont_splash_enabled
			&& !mdss_mdp_ctl_is_power_on(mdp5_data->ctl)))
		return -EAGAIN;

	vsync_ticks = ktime_to_ns(mdp5_data->vsync_time);

	pr_debug("fb%d vsync=%llu\n", mfd->index, vsync_ticks);
	ret = scnprintf(buf, PAGE_SIZE, "VSYNC=%llu\n", vsync_ticks);

	return ret;
}

static ssize_t mdss_mdp_lineptr_show_event(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	u64 lineptr_ticks;
	int ret;

	if (!mdp5_data->ctl ||
		(!mdp5_data->ctl->panel_data->panel_info.cont_splash_enabled
			&& !mdss_mdp_ctl_is_power_on(mdp5_data->ctl)))
		return -EPERM;

	lineptr_ticks = ktime_to_ns(mdp5_data->lineptr_time);

	pr_debug("fb%d lineptr=%llu\n", mfd->index, lineptr_ticks);
	ret = scnprintf(buf, PAGE_SIZE, "LINEPTR=%llu\n", lineptr_ticks);

	return ret;
}

static ssize_t mdss_mdp_lineptr_show_value(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret, lineptr_val;

	if (!mdp5_data->ctl ||
		(!mdp5_data->ctl->panel_data->panel_info.cont_splash_enabled
			&& !mdss_mdp_ctl_is_power_on(mdp5_data->ctl)))
		return -EPERM;

	lineptr_val = mfd->panel_info->te.wr_ptr_irq;

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", lineptr_val);

	return ret;
}

static ssize_t mdss_mdp_lineptr_set_value(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = mdp5_data->ctl;
	int ret, lineptr_value;

	if (!ctl || (!ctl->panel_data->panel_info.cont_splash_enabled
		&& !mdss_mdp_ctl_is_power_on(ctl)))
		return -EAGAIN;

	ret = kstrtoint(buf, 10, &lineptr_value);
	if (ret || (lineptr_value < 0)
		|| (lineptr_value > mfd->panel_info->yres)) {
		pr_err("Invalid input for lineptr\n");
		return -EINVAL;
	}

	if (!mdss_mdp_is_lineptr_supported(ctl)) {
		pr_err("lineptr not supported\n");
		return -ENOTSUPP;
	}

	mutex_lock(&mdp5_data->ov_lock);
	mfd->panel_info->te.wr_ptr_irq = lineptr_value;
	if (ctl && ctl->ops.update_lineptr)
		ctl->ops.update_lineptr(ctl, true);
	mutex_unlock(&mdp5_data->ov_lock);

	return count;
}

static ssize_t mdss_mdp_bl_show_event(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret;

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", mdp5_data->bl_events);
	return ret;
}

static ssize_t mdss_mdp_hist_show_event(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret;

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", mdp5_data->hist_events);
	return ret;
}

static ssize_t mdss_mdp_ad_show_event(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret;

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", mdp5_data->ad_events);
	return ret;
}

static ssize_t mdss_mdp_ad_bl_show_event(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret;

	ret = scnprintf(buf, PAGE_SIZE, "%d\n", mdp5_data->ad_bl_events);
	return ret;
}

static inline int mdss_mdp_ad_is_supported(struct msm_fb_data_type *mfd)
{
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	struct mdss_mdp_mixer *mixer;

	if (!ctl) {
		pr_debug("there is no ctl attached to fb\n");
		return 0;
	}

	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (mixer && (mixer->num > ctl->mdata->nad_cfgs)) {
		if (!mixer)
			pr_warn("there is no mixer attached to fb\n");
		else
			pr_debug("mixer attached (%d) doesnt support ad\n",
				 mixer->num);
		return 0;
	}

	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_RIGHT);
	if (mixer && (mixer->num > ctl->mdata->nad_cfgs))
		return 0;

	return 1;
}

static ssize_t mdss_mdp_ad_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret, state;

	state = mdss_mdp_ad_is_supported(mfd) ? mdp5_data->ad_state : -1;

	ret = scnprintf(buf, PAGE_SIZE, "%d", state);

	return ret;
}

static ssize_t mdss_mdp_ad_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret, ad;

	ret = kstrtoint(buf, 10, &ad);
	if (ret) {
		pr_err("Invalid input for ad\n");
		return -EINVAL;
	}

	mdp5_data->ad_state = ad;
	sysfs_notify(&dev->kobj, NULL, "ad");

	return count;
}

static ssize_t mdss_mdp_dyn_pu_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret, state;

	state = (mdp5_data->dyn_pu_state >= 0) ? mdp5_data->dyn_pu_state : -1;

	ret = scnprintf(buf, PAGE_SIZE, "%d", state);

	return ret;
}

static ssize_t mdss_mdp_dyn_pu_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = fbi->par;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	int ret, dyn_pu;

	ret = kstrtoint(buf, 10, &dyn_pu);
	if (ret) {
		pr_err("Invalid input for partial udpate: ret = %d\n", ret);
		return ret;
	}

	mdp5_data->dyn_pu_state = dyn_pu;
	sysfs_notify(&dev->kobj, NULL, "dyn_pu");

	return count;
}

static ssize_t mdss_mdp_panel_disable_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_mdp_ctl *ctl;
	struct mdss_panel_data *pdata;

	if (!mfd) {
		pr_err("Invalid mfd structure\n");
		return -EINVAL;
	}

	ctl = mfd_to_ctl(mfd);
	if (!ctl) {
		pr_err("Invalid ctl structure\n");
		return -EINVAL;
	}

	pdata = dev_get_platdata(&mfd->pdev->dev);

	ret = snprintf(buf, PAGE_SIZE, "%d\n",
		pdata->panel_disable_mode);

	return ret;
}

int mdss_mdp_enable_panel_disable_mode(struct msm_fb_data_type *mfd,
	bool disable_panel)
{
	struct mdss_mdp_ctl *ctl;
	int ret = 0;
	struct mdss_panel_data *pdata;

	ctl = mfd_to_ctl(mfd);
	if (!ctl) {
		pr_err("Invalid ctl structure\n");
		ret = -EINVAL;
		return ret;
	}

	pdata = dev_get_platdata(&mfd->pdev->dev);

	pr_debug("config panel %d\n", disable_panel);
	if (disable_panel) {
		/* first set the flag that we enter this mode */
		pdata->panel_disable_mode = true;

		/*
		 * setup any interface config that needs to change  before
		 * disabling the panel
		 */
		if (ctl->ops.panel_disable_cfg)
			ctl->ops.panel_disable_cfg(ctl, disable_panel);

		/* disable panel */
		ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_DISABLE_PANEL,
				NULL, CTL_INTF_EVENT_FLAG_DEFAULT);
		if (ret)
			pr_err("failed to disable panel! %d\n", ret);
	} else {
		/* restore any interface configuration */
		if (ctl->ops.panel_disable_cfg)
			ctl->ops.panel_disable_cfg(ctl, disable_panel);

		/*
		 * no other action is needed when reconfiguring, since all the
		 * re-configuration will happen during restore
		 */
		pdata->panel_disable_mode = false;
	}

	return ret;
}

static ssize_t mdss_mdp_panel_disable_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	int disable_panel, rc;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;

	if (!mfd) {
		pr_err("Invalid mfd structure\n");
		rc = -EINVAL;
		return rc;
	}

	rc = kstrtoint(buf, 10, &disable_panel);
	if (rc) {
		pr_err("kstrtoint failed. rc=%d\n", rc);
		return rc;
	}

	pr_debug("disable panel: %d ++\n", disable_panel);
	/* we only support disabling the panel from sysfs */
	if (disable_panel)
		mdss_mdp_enable_panel_disable_mode(mfd, true);

	return len;
}

static ssize_t mdss_mdp_cmd_autorefresh_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_mdp_ctl *ctl;

	if (!mfd) {
		pr_err("Invalid mfd structure\n");
		return -EINVAL;
	}

	ctl = mfd_to_ctl(mfd);
	if (!ctl) {
		pr_err("Invalid ctl structure\n");
		return -EINVAL;
	}


	if (mfd->panel_info->type != MIPI_CMD_PANEL) {
		pr_err("Panel doesnt support autorefresh\n");
		ret = -EINVAL;
	} else {
		ret = snprintf(buf, PAGE_SIZE, "%d\n",
			mdss_mdp_ctl_cmd_get_autorefresh(ctl));
	}
	return ret;
}

static ssize_t mdss_mdp_cmd_autorefresh_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	int frame_cnt, rc;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_mdp_ctl *ctl;

	if (!mfd) {
		pr_err("Invalid mfd structure\n");
		rc = -EINVAL;
		return rc;
	}

	ctl = mfd_to_ctl(mfd);
	if (!ctl) {
		pr_err("Invalid ctl structure\n");
		rc = -EINVAL;
		return rc;
	}

	if (mfd->panel_info->type != MIPI_CMD_PANEL) {
		pr_err("Panel doesnt support autorefresh\n");
		rc = -EINVAL;
		return rc;
	}

	rc = kstrtoint(buf, 10, &frame_cnt);
	if (rc) {
		pr_err("kstrtoint failed. rc=%d\n", rc);
		return rc;
	}

	rc = mdss_mdp_ctl_cmd_set_autorefresh(ctl, frame_cnt);
	if (rc) {
		pr_err("cmd_set_autorefresh failed, rc=%d, frame_cnt=%d\n",
			rc, frame_cnt);
		return rc;
	}

	if (frame_cnt) {
		/* enable/reconfig autorefresh */
		mfd->mdp_sync_pt_data.threshold = 2;
		mfd->mdp_sync_pt_data.retire_threshold = 0;
	} else {
		/* disable autorefresh */
		mfd->mdp_sync_pt_data.threshold = 1;
		mfd->mdp_sync_pt_data.retire_threshold = 1;
	}

	pr_debug("setting cmd autorefresh to cnt=%d\n", frame_cnt);

	return len;
}


/* Print the last CRC Value read for batch mode */
static ssize_t mdss_mdp_misr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_mdp_ctl *ctl;

	if (!mfd) {
		pr_err("Invalid mfd structure\n");
		return -EINVAL;
	}

	ctl = mfd_to_ctl(mfd);
	if (!ctl) {
		pr_err("Invalid ctl structure\n");
		return -EINVAL;
	}

	ret = mdss_dump_misr_data(&buf, PAGE_SIZE);

	return ret;
}

/*
 * Enable crc batch mode. By enabling this mode through sysfs
 * driver will keep collecting the misr in ftrace during interrupts,
 * until disabled.
 */
static ssize_t mdss_mdp_misr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t len)
{
	int enable_misr, rc;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	struct mdss_mdp_ctl *ctl;
	struct mdp_misr req, sreq;

	if (!mfd) {
		pr_err("Invalid mfd structure\n");
		rc = -EINVAL;
		return rc;
	}

	ctl = mfd_to_ctl(mfd);
	if (!ctl) {
		pr_err("Invalid ctl structure\n");
		rc = -EINVAL;
		return rc;
	}

	rc = kstrtoint(buf, 10, &enable_misr);
	if (rc) {
		pr_err("kstrtoint failed. rc=%d\n", rc);
		return rc;
	}

	req.block_id = DISPLAY_MISR_MAX;
	sreq.block_id = DISPLAY_MISR_MAX;

	pr_debug("intf_type:%d enable:%d\n", ctl->intf_type, enable_misr);
	if (ctl->intf_type == MDSS_INTF_DSI) {

		req.block_id = DISPLAY_MISR_DSI0;
		req.crc_op_mode = MISR_OP_BM;
		req.frame_count = 1;
		if (is_panel_split(mfd)) {

			sreq.block_id = DISPLAY_MISR_DSI1;
			sreq.crc_op_mode = MISR_OP_BM;
			sreq.frame_count = 1;
		}
	} else if (ctl->intf_type == MDSS_INTF_HDMI) {

		req.block_id = DISPLAY_MISR_HDMI;
		req.crc_op_mode = MISR_OP_BM;
		req.frame_count = 1;
	} else {
		pr_err("misr not supported fo this fb:%d\n", mfd->index);
		rc = -ENODEV;
		return rc;
	}

	if (enable_misr) {
		mdss_misr_set(mdata, &req , ctl);

		if ((ctl->intf_type == MDSS_INTF_DSI) && is_panel_split(mfd))
			mdss_misr_set(mdata, &sreq , ctl);

	} else {
		mdss_misr_disable(mdata, &req, ctl);

		if ((ctl->intf_type == MDSS_INTF_DSI) && is_panel_split(mfd))
			mdss_misr_disable(mdata, &sreq , ctl);
	}

	pr_debug("misr %s\n", enable_misr ? "enabled" : "disabled");

	return len;
}

static DEVICE_ATTR(msm_misr_en, S_IRUGO | S_IWUSR,
	mdss_mdp_misr_show, mdss_mdp_misr_store);
static DEVICE_ATTR(msm_cmd_autorefresh_en, S_IRUGO | S_IWUSR,
	mdss_mdp_cmd_autorefresh_show, mdss_mdp_cmd_autorefresh_store);
static DEVICE_ATTR(msm_disable_panel, S_IRUGO | S_IWUSR,
	mdss_mdp_panel_disable_show, mdss_mdp_panel_disable_store);
static DEVICE_ATTR(vsync_event, S_IRUGO, mdss_mdp_vsync_show_event, NULL);
static DEVICE_ATTR(lineptr_event, S_IRUGO, mdss_mdp_lineptr_show_event, NULL);
static DEVICE_ATTR(lineptr_value, S_IRUGO | S_IWUSR | S_IWGRP,
		mdss_mdp_lineptr_show_value, mdss_mdp_lineptr_set_value);
static DEVICE_ATTR(ad, S_IRUGO | S_IWUSR | S_IWGRP, mdss_mdp_ad_show,
	mdss_mdp_ad_store);
static DEVICE_ATTR(dyn_pu, S_IRUGO | S_IWUSR | S_IWGRP, mdss_mdp_dyn_pu_show,
	mdss_mdp_dyn_pu_store);
static DEVICE_ATTR(hist_event, S_IRUGO, mdss_mdp_hist_show_event, NULL);
static DEVICE_ATTR(bl_event, S_IRUGO, mdss_mdp_bl_show_event, NULL);
static DEVICE_ATTR(ad_event, S_IRUGO, mdss_mdp_ad_show_event, NULL);
static DEVICE_ATTR(ad_bl_event, S_IRUGO, mdss_mdp_ad_bl_show_event, NULL);

static struct attribute *mdp_overlay_sysfs_attrs[] = {
	&dev_attr_vsync_event.attr,
	&dev_attr_lineptr_event.attr,
	&dev_attr_lineptr_value.attr,
	&dev_attr_ad.attr,
	&dev_attr_dyn_pu.attr,
	&dev_attr_msm_misr_en.attr,
	&dev_attr_msm_cmd_autorefresh_en.attr,
	&dev_attr_msm_disable_panel.attr,
	&dev_attr_hist_event.attr,
	&dev_attr_bl_event.attr,
	&dev_attr_ad_event.attr,
	&dev_attr_ad_bl_event.attr,
	NULL,
};

static struct attribute_group mdp_overlay_sysfs_group = {
	.attrs = mdp_overlay_sysfs_attrs,
};

static void mdss_mdp_hw_cursor_setpos(struct mdss_mdp_mixer *mixer,
		struct mdss_rect *roi, u32 start_x, u32 start_y)
{
	int roi_xy = (roi->y << 16) | roi->x;
	int start_xy = (start_y << 16) | start_x;
	int roi_size = (roi->h << 16) | roi->w;

	if (!mixer) {
		pr_err("mixer not available\n");
		return;
	}
	mdp_mixer_write(mixer, MDSS_MDP_REG_LM_CURSOR_XY, roi_xy);
	mdp_mixer_write(mixer, MDSS_MDP_REG_LM_CURSOR_START_XY, start_xy);
	mdp_mixer_write(mixer, MDSS_MDP_REG_LM_CURSOR_SIZE, roi_size);
}

static void mdss_mdp_hw_cursor_setimage(struct mdss_mdp_mixer *mixer,
	struct fb_cursor *cursor, u32 cursor_addr, struct mdss_rect *roi)
{
	int calpha_en, transp_en, alpha, size;
	struct fb_image *img = &cursor->image;
	u32 blendcfg;
	int roi_size = 0;

	if (!mixer) {
		pr_err("mixer not available\n");
		return;
	}

	if (img->bg_color == 0xffffffff)
		transp_en = 0;
	else
		transp_en = 1;

	alpha = (img->fg_color & 0xff000000) >> 24;

	if (alpha)
		calpha_en = 0x0; /* xrgb */
	else
		calpha_en = 0x2; /* argb */

	roi_size = (roi->h << 16) | roi->w;
	size = (img->height << 16) | img->width;
	mdp_mixer_write(mixer, MDSS_MDP_REG_LM_CURSOR_IMG_SIZE, size);
	mdp_mixer_write(mixer, MDSS_MDP_REG_LM_CURSOR_SIZE, roi_size);
	mdp_mixer_write(mixer, MDSS_MDP_REG_LM_CURSOR_STRIDE,
				img->width * 4);
	mdp_mixer_write(mixer, MDSS_MDP_REG_LM_CURSOR_BASE_ADDR,
				cursor_addr);
	blendcfg = mdp_mixer_read(mixer, MDSS_MDP_REG_LM_CURSOR_BLEND_CONFIG);
	blendcfg &= ~0x1;
	blendcfg |= (transp_en << 3) | (calpha_en << 1);
	mdp_mixer_write(mixer, MDSS_MDP_REG_LM_CURSOR_BLEND_CONFIG,
				blendcfg);
	if (calpha_en)
		mdp_mixer_write(mixer, MDSS_MDP_REG_LM_CURSOR_BLEND_PARAM,
				alpha);

	if (transp_en) {
		mdp_mixer_write(mixer,
				MDSS_MDP_REG_LM_CURSOR_BLEND_TRANSP_LOW0,
				((img->bg_color & 0xff00) << 8) |
				(img->bg_color & 0xff));
		mdp_mixer_write(mixer,
				MDSS_MDP_REG_LM_CURSOR_BLEND_TRANSP_LOW1,
				((img->bg_color & 0xff0000) >> 16));
		mdp_mixer_write(mixer,
				MDSS_MDP_REG_LM_CURSOR_BLEND_TRANSP_HIGH0,
				((img->bg_color & 0xff00) << 8) |
				(img->bg_color & 0xff));
		mdp_mixer_write(mixer,
				MDSS_MDP_REG_LM_CURSOR_BLEND_TRANSP_HIGH1,
				((img->bg_color & 0xff0000) >> 16));
	}
}

static void mdss_mdp_hw_cursor_blend_config(struct mdss_mdp_mixer *mixer,
		struct fb_cursor *cursor)
{
	u32 blendcfg;
	if (!mixer) {
		pr_err("mixer not availbale\n");
		return;
	}

	blendcfg = mdp_mixer_read(mixer, MDSS_MDP_REG_LM_CURSOR_BLEND_CONFIG);
	if (!cursor->enable != !(blendcfg & 0x1)) {
		if (cursor->enable) {
			pr_debug("enable hw cursor on mixer=%d\n", mixer->num);
			blendcfg |= 0x1;
		} else {
			pr_debug("disable hw cursor on mixer=%d\n", mixer->num);
			blendcfg &= ~0x1;
		}

		mdp_mixer_write(mixer, MDSS_MDP_REG_LM_CURSOR_BLEND_CONFIG,
				   blendcfg);
		mixer->cursor_enabled = cursor->enable;
		mixer->params_changed++;
	}

}

static void mdss_mdp_set_rect(struct mdp_rect *rect, u16 x, u16 y, u16 w,
		u16 h)
{
	rect->x = x;
	rect->y = y;
	rect->w = w;
	rect->h = h;
}

static void mdss_mdp_curor_pipe_cleanup(struct msm_fb_data_type *mfd,
		int cursor_pipe)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	if (mdp5_data->cursor_ndx[cursor_pipe] != MSMFB_NEW_REQUEST) {
		mdss_mdp_overlay_release(mfd,
				mdp5_data->cursor_ndx[cursor_pipe]);
		mdp5_data->cursor_ndx[cursor_pipe] = MSMFB_NEW_REQUEST;
	}
}

int mdss_mdp_cursor_flush(struct msm_fb_data_type *mfd,
		struct mdss_mdp_pipe *pipe, int cursor_pipe)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = mdp5_data->ctl;
	struct mdss_mdp_ctl *sctl = NULL;
	u32 flush_bits = BIT(22 + pipe->num - MDSS_MDP_SSPP_CURSOR0);

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);

	mdss_mdp_ctl_write(ctl, MDSS_MDP_REG_CTL_FLUSH, flush_bits);
	MDSS_XLOG(ctl->intf_num, flush_bits);
	if ((!ctl->split_flush_en) && pipe->mixer_right) {
		sctl = mdss_mdp_get_split_ctl(ctl);
		if (!sctl) {
			pr_err("not able to get the other ctl\n");
			return -ENODEV;
		}
		mdss_mdp_ctl_write(sctl, MDSS_MDP_REG_CTL_FLUSH, flush_bits);
		MDSS_XLOG(sctl->intf_num, flush_bits);
	}

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);

	return 0;
}

static int mdss_mdp_cursor_pipe_setup(struct msm_fb_data_type *mfd,
		struct mdp_overlay *req, int cursor_pipe) {
	struct mdss_mdp_pipe *pipe;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	int ret = 0;
	u32 cursor_addr;
	struct mdss_mdp_data *buf = NULL;

	req->id = mdp5_data->cursor_ndx[cursor_pipe];
	ret = mdss_mdp_overlay_pipe_setup(mfd, req, &pipe, NULL, false);
	if (ret) {
		pr_err("cursor pipe setup failed, cursor_pipe:%d, ret:%d\n",
				cursor_pipe, ret);
		mdp5_data->cursor_ndx[cursor_pipe] = MSMFB_NEW_REQUEST;
		return ret;
	}

	pr_debug("req id:%d cursor_pipe:%d pnum:%d\n",
		req->id, cursor_pipe, pipe->ndx);

	if (mdata->mdss_util->iommu_attached()) {
		cursor_addr = mfd->cursor_buf_iova;
	} else {
		if (MDSS_LPAE_CHECK(mfd->cursor_buf_phys)) {
			pr_err("can't access phy mem >4GB w/o iommu\n");
			ret = -ERANGE;
			goto done;
		}
		cursor_addr = mfd->cursor_buf_phys;
	}

	buf = __mdp_overlay_buf_alloc(mfd, pipe);
	if (!buf) {
		pr_err("unable to allocate memory for cursor buffer\n");
		ret = -ENOMEM;
		goto done;
	}
	mdp5_data->cursor_ndx[cursor_pipe] = pipe->ndx;
	buf->p[0].addr = cursor_addr;
	buf->p[0].len = mdss_mdp_get_cursor_frame_size(mdata);
	buf->num_planes = 1;

	buf->state = MDP_BUF_STATE_ACTIVE;
	if (!(req->flags & MDP_SOLID_FILL))
		ret = mdss_mdp_pipe_queue_data(pipe, buf);
	else
		ret = mdss_mdp_pipe_queue_data(pipe, NULL);

	if (ret) {
		pr_err("cursor pipe queue data failed in async mode\n");
		return ret;
	}

	ret = mdss_mdp_cursor_flush(mfd, pipe, cursor_pipe);
done:
	if (ret && mdp5_data->cursor_ndx[cursor_pipe] == MSMFB_NEW_REQUEST)
		mdss_mdp_overlay_release(mfd, pipe->ndx);

	return ret;
}

static int mdss_mdp_hw_cursor_pipe_update(struct msm_fb_data_type *mfd,
				     struct fb_cursor *cursor)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_mixer *mixer;
	struct fb_image *img = &cursor->image;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	struct mdp_overlay *req = NULL;
	struct mdss_rect roi;
	int ret = 0;
	struct fb_var_screeninfo *var = &mfd->fbi->var;
	u32 xres = var->xres;
	u32 yres = var->yres;
	u32 start_x = img->dx;
	u32 start_y = img->dy;
	u32 left_lm_w = left_lm_w_from_mfd(mfd);
	struct platform_device *pdev = mfd->pdev;
	u32 cursor_frame_size = mdss_mdp_get_cursor_frame_size(mdata);

	ret = mutex_lock_interruptible(&mdp5_data->ov_lock);
	if (ret)
		return ret;

	if (mdss_fb_is_power_off(mfd)) {
		ret = -EPERM;
		goto done;
	}

	if (!cursor->enable) {
		mdss_mdp_curor_pipe_cleanup(mfd, CURSOR_PIPE_LEFT);
		mdss_mdp_curor_pipe_cleanup(mfd, CURSOR_PIPE_RIGHT);
		goto done;
	}

	mixer = mdss_mdp_mixer_get(mdp5_data->ctl, MDSS_MDP_MIXER_MUX_DEFAULT);
	if (!mixer) {
		ret = -ENODEV;
		goto done;
	}

	if (!mfd->cursor_buf && (cursor->set & FB_CUR_SETIMAGE)) {
		ret = mdss_smmu_dma_alloc_coherent(&pdev->dev,
			cursor_frame_size, (dma_addr_t *) &mfd->cursor_buf_phys,
			&mfd->cursor_buf_iova, &mfd->cursor_buf,
			GFP_KERNEL, MDSS_IOMMU_DOMAIN_UNSECURE);
		if (ret) {
			pr_err("can't allocate cursor buffer rc:%d\n", ret);
			goto done;
		}

		mixer->cursor_hotx = 0;
		mixer->cursor_hoty = 0;
	}

	pr_debug("mixer=%d enable=%x set=%x\n", mixer->num, cursor->enable,
			cursor->set);

	if (cursor->set & FB_CUR_SETHOT) {
		if ((cursor->hot.x < img->width) &&
			(cursor->hot.y < img->height)) {
			mixer->cursor_hotx = cursor->hot.x;
			mixer->cursor_hoty = cursor->hot.y;
			 /* Update cursor position */
			cursor->set |= FB_CUR_SETPOS;
		} else {
			pr_err("Invalid cursor hotspot coordinates\n");
			ret = -EINVAL;
			goto done;
		}
	}

	memset(&roi, 0, sizeof(struct mdss_rect));
	if (start_x > mixer->cursor_hotx) {
		start_x -= mixer->cursor_hotx;
	} else {
		roi.x = mixer->cursor_hotx - start_x;
		start_x = 0;
	}
	if (start_y > mixer->cursor_hoty) {
		start_y -= mixer->cursor_hoty;
	} else {
		roi.y = mixer->cursor_hoty - start_y;
		start_y = 0;
	}

	if ((img->width > mdata->max_cursor_size) ||
		(img->height > mdata->max_cursor_size) ||
		(img->depth != 32) || (start_x >= xres) ||
		(start_y >= yres)) {
		pr_err("Invalid cursor image coordinates\n");
		ret = -EINVAL;
		goto done;
	}

	roi.w = min(xres - start_x, img->width - roi.x);
	roi.h = min(yres - start_y, img->height - roi.y);

	if ((roi.w > mdata->max_cursor_size) ||
		(roi.h > mdata->max_cursor_size)) {
		pr_err("Invalid cursor ROI size\n");
		ret = -EINVAL;
		goto done;
	}

	req = kzalloc(sizeof(struct mdp_overlay), GFP_KERNEL);
	if (!req) {
		pr_err("not able to allocate memory for cursor req\n");
		ret = -ENOMEM;
		goto done;
	}

	req->pipe_type = PIPE_TYPE_CURSOR;
	req->z_order = HW_CURSOR_STAGE(mdata);

	req->src.width = img->width;
	req->src.height = img->height;
	req->src.format = mfd->fb_imgType;

	mdss_mdp_set_rect(&req->src_rect, roi.x, roi.y, roi.w, roi.h);
	mdss_mdp_set_rect(&req->dst_rect, start_x, start_y, roi.w, roi.h);

	req->bg_color = img->bg_color;
	req->alpha = (img->fg_color >> ((32 - var->transp.offset) - 8)) & 0xff;
	if (req->alpha)
		req->blend_op = BLEND_OP_PREMULTIPLIED;
	else
		req->blend_op = BLEND_OP_COVERAGE;
	req->transp_mask = img->bg_color & ~(0xff << var->transp.offset);

	if (mfd->cursor_buf && (cursor->set & FB_CUR_SETIMAGE)) {
		if (img->width * img->height * 4 > cursor_frame_size) {
			pr_err("cursor image size is too large\n");
			ret = -EINVAL;
			goto done;
		}

		ret = copy_from_user(mfd->cursor_buf, img->data,
				     img->width * img->height * 4);
		if (ret) {
			pr_err("copy_from_user error. rc=%d\n", ret);
			goto done;
		}

		mixer->cursor_hotx = 0;
		mixer->cursor_hoty = 0;
	}

	/*
	 * When source split is enabled, only CURSOR_PIPE_LEFT is used,
	 * with both mixers of the pipe staged all the time.
	 * When source split is disabled, 2 pipes are staged, with one
	 * pipe containing the actual data and another one a transparent
	 * solid fill when the data falls only in left or right dsi.
	 * Both are done to support async cursor functionality.
	 */
	if (mdata->has_src_split || (!is_split_lm(mfd))
			|| (mdata->ncursor_pipes == 1)) {
		ret = mdss_mdp_cursor_pipe_setup(mfd, req, CURSOR_PIPE_LEFT);
	} else if ((start_x + roi.w) <= left_lm_w) {
		ret = mdss_mdp_cursor_pipe_setup(mfd, req, CURSOR_PIPE_LEFT);
		if (ret)
			goto done;
		req->bg_color = 0;
		req->flags |= MDP_SOLID_FILL;
		req->dst_rect.x = left_lm_w;
		ret = mdss_mdp_cursor_pipe_setup(mfd, req, CURSOR_PIPE_RIGHT);
	} else if (start_x >= left_lm_w) {
		ret = mdss_mdp_cursor_pipe_setup(mfd, req, CURSOR_PIPE_RIGHT);
		if (ret)
			goto done;
		req->bg_color = 0;
		req->flags |= MDP_SOLID_FILL;
		req->dst_rect.x = 0;
		ret = mdss_mdp_cursor_pipe_setup(mfd, req, CURSOR_PIPE_LEFT);
	} else if ((start_x <= left_lm_w) && ((start_x + roi.w) >= left_lm_w)) {
		mdss_mdp_set_rect(&req->dst_rect, start_x, start_y,
				(left_lm_w - start_x), roi.h);
		mdss_mdp_set_rect(&req->src_rect, 0, 0, (left_lm_w -
				start_x), roi.h);
		ret = mdss_mdp_cursor_pipe_setup(mfd, req, CURSOR_PIPE_LEFT);
		if (ret)
			goto done;

		mdss_mdp_set_rect(&req->dst_rect, left_lm_w, start_y, ((start_x
				+ roi.w) - left_lm_w), roi.h);
		mdss_mdp_set_rect(&req->src_rect, (left_lm_w - start_x), 0,
				(roi.w - (left_lm_w - start_x)), roi.h);
		ret = mdss_mdp_cursor_pipe_setup(mfd, req, CURSOR_PIPE_RIGHT);
	} else {
		pr_err("Invalid case for cursor pipe setup\n");
		ret = -EINVAL;
	}

done:
	if (ret) {
		mdss_mdp_curor_pipe_cleanup(mfd, CURSOR_PIPE_LEFT);
		mdss_mdp_curor_pipe_cleanup(mfd, CURSOR_PIPE_RIGHT);
	}

	kfree(req);
	mutex_unlock(&mdp5_data->ov_lock);
	return ret;
}

static int mdss_mdp_hw_cursor_update(struct msm_fb_data_type *mfd,
				     struct fb_cursor *cursor)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_mixer *mixer_left = NULL;
	struct mdss_mdp_mixer *mixer_right = NULL;
	struct fb_image *img = &cursor->image;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	struct fbcurpos cursor_hot;
	struct mdss_rect roi;
	int ret = 0;
	u32 xres = mfd->fbi->var.xres;
	u32 yres = mfd->fbi->var.yres;
	u32 start_x = img->dx;
	u32 start_y = img->dy;
	u32 left_lm_w = left_lm_w_from_mfd(mfd);
	struct platform_device *pdev = mfd->pdev;
	u32 cursor_frame_size = mdss_mdp_get_cursor_frame_size(mdata);

	mixer_left = mdss_mdp_mixer_get(mdp5_data->ctl,
			MDSS_MDP_MIXER_MUX_DEFAULT);
	if (!mixer_left)
		return -ENODEV;
	if (is_split_lm(mfd)) {
		mixer_right = mdss_mdp_mixer_get(mdp5_data->ctl,
				MDSS_MDP_MIXER_MUX_RIGHT);
		if (!mixer_right)
			return -ENODEV;
	}

	if (!mfd->cursor_buf && (cursor->set & FB_CUR_SETIMAGE)) {
		ret = mdss_smmu_dma_alloc_coherent(&pdev->dev,
			cursor_frame_size, (dma_addr_t *) &mfd->cursor_buf_phys,
			&mfd->cursor_buf_iova, &mfd->cursor_buf,
			GFP_KERNEL, MDSS_IOMMU_DOMAIN_UNSECURE);
		if (ret) {
			pr_err("can't allocate cursor buffer rc:%d\n", ret);
			return ret;
		}
	}

	if ((img->width > mdata->max_cursor_size) ||
		(img->height > mdata->max_cursor_size) ||
		(img->depth != 32) || (start_x >= xres) || (start_y >= yres))
		return -EINVAL;

	pr_debug("enable=%x set=%x\n", cursor->enable, cursor->set);

	memset(&cursor_hot, 0, sizeof(struct fbcurpos));
	memset(&roi, 0, sizeof(struct mdss_rect));
	if (cursor->set & FB_CUR_SETHOT) {
		if ((cursor->hot.x < img->width) &&
			(cursor->hot.y < img->height)) {
			cursor_hot.x = cursor->hot.x;
			cursor_hot.y = cursor->hot.y;
			 /* Update cursor position */
			cursor->set |= FB_CUR_SETPOS;
		} else {
			pr_err("Invalid cursor hotspot coordinates\n");
			return -EINVAL;
		}
	}

	if (start_x > cursor_hot.x) {
		start_x -= cursor_hot.x;
	} else {
		roi.x = cursor_hot.x - start_x;
		start_x = 0;
	}
	if (start_y > cursor_hot.y) {
		start_y -= cursor_hot.y;
	} else {
		roi.y = cursor_hot.y - start_y;
		start_y = 0;
	}

	roi.w = min(xres - start_x, img->width - roi.x);
	roi.h = min(yres - start_y, img->height - roi.y);

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);

	if (mfd->cursor_buf && (cursor->set & FB_CUR_SETIMAGE)) {
		u32 cursor_addr;
		ret = copy_from_user(mfd->cursor_buf, img->data,
				     img->width * img->height * 4);
		if (ret) {
			pr_err("copy_from_user error. rc=%d\n", ret);
			mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);
			return ret;
		}

		if (mdata->mdss_util->iommu_attached()) {
			cursor_addr = mfd->cursor_buf_iova;
		} else {
			if (MDSS_LPAE_CHECK(mfd->cursor_buf_phys)) {
				pr_err("can't access phy mem >4GB w/o iommu\n");
				mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);
				return -ERANGE;
			}
			cursor_addr = mfd->cursor_buf_phys;
		}
		mdss_mdp_hw_cursor_setimage(mixer_left, cursor, cursor_addr,
				&roi);
		if (is_split_lm(mfd))
			mdss_mdp_hw_cursor_setimage(mixer_right, cursor,
					cursor_addr, &roi);
	}

	if ((start_x + roi.w) <= left_lm_w) {
		if (cursor->set & FB_CUR_SETPOS)
			mdss_mdp_hw_cursor_setpos(mixer_left, &roi, start_x,
					start_y);
		mdss_mdp_hw_cursor_blend_config(mixer_left, cursor);
		cursor->enable = false;
		mdss_mdp_hw_cursor_blend_config(mixer_right, cursor);
	} else if (start_x >= left_lm_w) {
		start_x -= left_lm_w;
		if (cursor->set & FB_CUR_SETPOS)
			mdss_mdp_hw_cursor_setpos(mixer_right, &roi, start_x,
					start_y);
		mdss_mdp_hw_cursor_blend_config(mixer_right, cursor);
		cursor->enable = false;
		mdss_mdp_hw_cursor_blend_config(mixer_left, cursor);
	} else {
		struct mdss_rect roi_right = roi;
		roi.w = left_lm_w - start_x;
		if (cursor->set & FB_CUR_SETPOS)
			mdss_mdp_hw_cursor_setpos(mixer_left, &roi, start_x,
					start_y);
		mdss_mdp_hw_cursor_blend_config(mixer_left, cursor);

		roi_right.x = 0;
		roi_right.w = (start_x + roi_right.w) - left_lm_w;
		start_x = 0;
		if (cursor->set & FB_CUR_SETPOS)
			mdss_mdp_hw_cursor_setpos(mixer_right, &roi_right,
					start_x, start_y);
		mdss_mdp_hw_cursor_blend_config(mixer_right, cursor);
	}

	mixer_left->ctl->flush_bits |= BIT(6) << mixer_left->num;
	if (is_split_lm(mfd))
		mixer_right->ctl->flush_bits |= BIT(6) << mixer_right->num;
	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);
	return 0;
}

static int mdss_bl_scale_config(struct msm_fb_data_type *mfd,
						struct mdp_bl_scale_data *data)
{
	int ret = 0;
	int curr_bl;
	mutex_lock(&mfd->bl_lock);
	curr_bl = mfd->bl_level;
	mfd->bl_scale = data->scale;
	pr_debug("update scale = %d\n", mfd->bl_scale);

	/* Update current backlight to use new scaling, if it is not zero */
	if (curr_bl)
		mdss_fb_set_backlight(mfd, curr_bl);

	mutex_unlock(&mfd->bl_lock);
	return ret;
}

static int mdss_mdp_pp_ioctl(struct msm_fb_data_type *mfd,
				void __user *argp)
{
	int ret;
	struct msmfb_mdp_pp mdp_pp;
	u32 copyback = 0;
	u32 copy_from_kernel = 0;

	ret = copy_from_user(&mdp_pp, argp, sizeof(mdp_pp));
	if (ret)
		return ret;

	/* Supprt only MDP register read/write and
	exit_dcm in DCM state*/
	if (mfd->dcm_state == DCM_ENTER &&
			(mdp_pp.op != mdp_op_calib_buffer &&
			mdp_pp.op != mdp_op_calib_dcm_state))
		return -EPERM;

	switch (mdp_pp.op) {
	case mdp_op_pa_cfg:
		ret = mdss_mdp_pa_config(mfd, &mdp_pp.data.pa_cfg_data,
					&copyback);
		break;

	case mdp_op_pa_v2_cfg:
		ret = mdss_mdp_pa_v2_config(mfd, &mdp_pp.data.pa_v2_cfg_data,
					&copyback);
		break;

	case mdp_op_pcc_cfg:
		ret = mdss_mdp_pcc_config(mfd, &mdp_pp.data.pcc_cfg_data,
					&copyback);
		break;

	case mdp_op_lut_cfg:
		switch (mdp_pp.data.lut_cfg_data.lut_type) {
		case mdp_lut_igc:
			ret = mdss_mdp_igc_lut_config(mfd,
					(struct mdp_igc_lut_data *)
					&mdp_pp.data.lut_cfg_data.data,
					&copyback, copy_from_kernel);
			break;

		case mdp_lut_pgc:
			ret = mdss_mdp_argc_config(mfd,
				&mdp_pp.data.lut_cfg_data.data.pgc_lut_data,
				&copyback);
			break;

		case mdp_lut_hist:
			ret = mdss_mdp_hist_lut_config(mfd,
				(struct mdp_hist_lut_data *)
				&mdp_pp.data.lut_cfg_data.data, &copyback);
			break;

		default:
			ret = -ENOTSUPP;
			break;
		}
		break;
	case mdp_op_dither_cfg:
		ret = mdss_mdp_dither_config(mfd,
				&mdp_pp.data.dither_cfg_data,
				&copyback,
				false);
		break;
	case mdp_op_gamut_cfg:
		ret = mdss_mdp_gamut_config(mfd,
				&mdp_pp.data.gamut_cfg_data,
				&copyback);
		break;
	case mdp_bl_scale_cfg:
		ret = mdss_bl_scale_config(mfd, (struct mdp_bl_scale_data *)
						&mdp_pp.data.bl_scale_data);
		break;
	case mdp_op_ad_cfg:
		ret = mdss_mdp_ad_config(mfd, &mdp_pp.data.ad_init_cfg);
		break;
	case mdp_op_ad_bl_cfg:
		ret = mdss_mdp_ad_bl_config(mfd, &mdp_pp.data.ad_bl_cfg);
		break;
	case mdp_op_ad_input:
		ret = mdss_mdp_ad_input(mfd, &mdp_pp.data.ad_input, 1);
		if (ret > 0) {
			ret = 0;
			copyback = 1;
		}
		break;
	case mdp_op_calib_cfg:
		ret = mdss_mdp_calib_config((struct mdp_calib_config_data *)
					 &mdp_pp.data.calib_cfg, &copyback);
		break;
	case mdp_op_calib_mode:
		ret = mdss_mdp_calib_mode(mfd, &mdp_pp.data.mdss_calib_cfg);
		break;
	case mdp_op_calib_buffer:
		ret = mdss_mdp_calib_config_buffer(
				(struct mdp_calib_config_buffer *)
				 &mdp_pp.data.calib_buffer, &copyback);
		break;
	case mdp_op_calib_dcm_state:
		ret = mdss_fb_dcm(mfd, mdp_pp.data.calib_dcm.dcm_state);
		break;
	case mdp_op_pa_dither_cfg:
		ret = mdss_mdp_pa_dither_config(mfd,
			&mdp_pp.data.dither_cfg_data);
		break;
	default:
		pr_err("Unsupported request to MDP_PP IOCTL. %d = op\n",
								mdp_pp.op);
		ret = -EINVAL;
		break;
	}
	if ((ret == 0) && copyback)
		ret = copy_to_user(argp, &mdp_pp, sizeof(struct msmfb_mdp_pp));
	return ret;
}

static int mdss_mdp_histo_ioctl(struct msm_fb_data_type *mfd, u32 cmd,
				void __user *argp)
{
	int ret = -ENOSYS;
	struct mdp_histogram_data hist;
	struct mdp_histogram_start_req hist_req;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	u32 block;

	if (!mdata)
		return -EPERM;

	switch (cmd) {
	case MSMFB_HISTOGRAM_START:
		if (mdss_fb_is_power_off(mfd))
			return -EPERM;

		ret = copy_from_user(&hist_req, argp, sizeof(hist_req));
		if (ret)
			return ret;

		ret = mdss_mdp_hist_start(&hist_req);
		break;

	case MSMFB_HISTOGRAM_STOP:
		ret = copy_from_user(&block, argp, sizeof(int));
		if (ret)
			return ret;

		ret = mdss_mdp_hist_stop(block);
		if (ret)
			return ret;
		break;

	case MSMFB_HISTOGRAM:
		if (mdss_fb_is_power_off(mfd)) {
			pr_err("mfd is turned off MSMFB_HISTOGRAM failed\n");
			return -EPERM;
		}

		ret = copy_from_user(&hist, argp, sizeof(hist));
		if (ret)
			return ret;

		ret = mdss_mdp_hist_collect(&hist);
		if (!ret)
			ret = copy_to_user(argp, &hist, sizeof(hist));
		break;
	default:
		break;
	}
	return ret;
}

static int mdss_fb_set_metadata(struct msm_fb_data_type *mfd,
				struct msmfb_metadata *metadata)
{
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	int ret = 0;
	if (!ctl)
		return  -EPERM;
	switch (metadata->op) {
	case metadata_op_vic:
		if (mfd->panel_info)
			mfd->panel_info->vic =
				metadata->data.video_info_code;
		else
			ret = -EINVAL;
		break;
	case metadata_op_crc:
		if (mdss_fb_is_power_off(mfd))
			return -EPERM;
		ret = mdss_misr_set(mdata, &metadata->data.misr_request, ctl);
		break;
	default:
		pr_warn("unsupported request to MDP META IOCTL\n");
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int mdss_fb_set_panel_ppm(struct msm_fb_data_type *mfd, s32 ppm)
{
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	int ret = 0;

	if (!ctl)
		return -EPERM;

	ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_UPDATE_PANEL_PPM,
			(void *) (unsigned long) ppm,
			CTL_INTF_EVENT_FLAG_DEFAULT);
	return ret;
}

static int mdss_fb_get_hw_caps(struct msm_fb_data_type *mfd,
		struct mdss_hw_caps *caps)
{
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	caps->mdp_rev = mdata->mdp_rev;
	caps->vig_pipes = mdata->nvig_pipes;
	caps->rgb_pipes = mdata->nrgb_pipes;
	caps->dma_pipes = mdata->ndma_pipes;
	if (mdata->has_bwc)
		caps->features |= MDP_BWC_EN;
	if (mdata->has_decimation)
		caps->features |= MDP_DECIMATION_EN;

	if (mdata->smp_mb_cnt) {
		caps->max_smp_cnt = mdata->smp_mb_cnt;
		caps->smp_per_pipe = mdata->smp_mb_per_pipe;
	}

	return 0;
}

static int mdss_fb_get_metadata(struct msm_fb_data_type *mfd,
				struct msmfb_metadata *metadata)
{
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	struct mdss_mdp_ctl *ctl = NULL;
	int ret = 0;

	switch (metadata->op) {
	case metadata_op_frame_rate:
		metadata->data.panel_frame_rate =
			mdss_panel_get_framerate(mfd->panel_info);
		pr_debug("current fps:%d\n", metadata->data.panel_frame_rate);
		break;
	case metadata_op_get_caps:
		ret = mdss_fb_get_hw_caps(mfd, &metadata->data.caps);
		break;
	case metadata_op_get_ion_fd:
		if (mfd->fb_ion_handle && mfd->fb_ion_client) {
			get_dma_buf(mfd->fbmem_buf);
			metadata->data.fbmem_ionfd =
				ion_share_dma_buf_fd(mfd->fb_ion_client,
					mfd->fb_ion_handle);
			if (metadata->data.fbmem_ionfd < 0) {
				dma_buf_put(mfd->fbmem_buf);
				pr_err("fd allocation failed. fd = %d\n",
						metadata->data.fbmem_ionfd);
			}
		}
		break;
	case metadata_op_crc:
		ctl = mfd_to_ctl(mfd);
		if (!ctl || mdss_fb_is_power_off(mfd))
			return -EPERM;
		ret = mdss_misr_get(mdata, &metadata->data.misr_request, ctl,
			ctl->is_video_mode);
		break;
	default:
		pr_warn("Unsupported request to MDP META IOCTL.\n");
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int __mdss_mdp_clean_dirty_pipes(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *pipe;
	int unset_ndx = 0;

	mutex_lock(&mdp5_data->list_lock);
	list_for_each_entry(pipe, &mdp5_data->pipes_used, list) {
		if (pipe->dirty)
			unset_ndx |= pipe->ndx;
	}
	mutex_unlock(&mdp5_data->list_lock);
	if (unset_ndx)
		mdss_mdp_overlay_release(mfd, unset_ndx);

	return unset_ndx;
}

static int mdss_mdp_overlay_precommit(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data;
	int ret;

	if (!mfd)
		return -ENODEV;

	mdp5_data = mfd_to_mdp5_data(mfd);
	if (!mdp5_data)
		return -ENODEV;

	ret = mutex_lock_interruptible(&mdp5_data->ov_lock);
	if (ret)
		return ret;

	/*
	 * we can assume that any pipes that are still dirty at this point are
	 * not properly tracked by user land. This could be for any reason,
	 * mark them for cleanup at this point.
	 */
	ret = __mdss_mdp_clean_dirty_pipes(mfd);
	if (ret) {
		pr_warn("fb%d: dirty pipes remaining %x\n",
				mfd->index, ret);
		ret = -EPIPE;
	}

	/*
	 * If we are in process of mode switch we may have an invalid state.
	 * We can allow commit to happen if there are no pipes attached as only
	 * border color will be seen regardless of resolution or mode.
	 */
	if ((mfd->switch_state != MDSS_MDP_NO_UPDATE_REQUESTED) &&
			(mfd->switch_state != MDSS_MDP_WAIT_FOR_COMMIT)) {
		if (list_empty(&mdp5_data->pipes_used)) {
			mfd->switch_state = MDSS_MDP_WAIT_FOR_COMMIT;
		} else {
			pr_warn("Invalid commit on fb%d with state=%d\n",
					mfd->index, mfd->switch_state);
			ret = -EINVAL;
		}
	}
	mutex_unlock(&mdp5_data->ov_lock);

	return ret;
}

/*
 * This routine serves two purposes.
 * 1. Propagate overlay_id returned from sorted list to original list
 *    to user-space.
 * 2. In case of error processing sorted list, map the error overlay's
 *    index to original list because user-space is not aware of the sorted list.
 */
static int __mdss_overlay_map(struct mdp_overlay *ovs,
	struct mdp_overlay *op_ovs, int num_ovs, int num_ovs_processed)
{
	int mapped = num_ovs_processed;
	int j, k;

	for (j = 0; j < num_ovs; j++) {
		for (k = 0; k < num_ovs; k++) {
			if ((ovs[j].dst_rect.x == op_ovs[k].dst_rect.x) &&
			    (ovs[j].z_order == op_ovs[k].z_order)) {
				op_ovs[k].id = ovs[j].id;
				op_ovs[k].priority = ovs[j].priority;
				break;
			}
		}

		if ((mapped != num_ovs) && (mapped == j)) {
			pr_debug("mapped %d->%d\n", mapped, k);
			mapped = k;
		}
	}

	return mapped;
}

static inline void __overlay_swap_func(void *a, void *b, int size)
{
	swap(*(struct mdp_overlay *)a, *(struct mdp_overlay *)b);
}

static inline int __zorder_dstx_cmp_func(const void *a, const void *b)
{
	int rc = 0;
	const struct mdp_overlay *ov1 = a;
	const struct mdp_overlay *ov2 = b;

	if (ov1->z_order < ov2->z_order)
		rc = -1;
	else if ((ov1->z_order == ov2->z_order) &&
		 (ov1->dst_rect.x < ov2->dst_rect.x))
		rc = -1;

	return rc;
}

/*
 * first sort list of overlays based on z_order and then within
 * same z_order sort them on dst_x.
 */
static int __mdss_overlay_src_split_sort(struct msm_fb_data_type *mfd,
	struct mdp_overlay *ovs, int num_ovs)
{
	int i;
	int left_lm_zo_cnt[MDSS_MDP_MAX_STAGE] = {0};
	int right_lm_zo_cnt[MDSS_MDP_MAX_STAGE] = {0};
	u32 left_lm_w = left_lm_w_from_mfd(mfd);

	sort(ovs, num_ovs, sizeof(struct mdp_overlay), __zorder_dstx_cmp_func,
		__overlay_swap_func);

	for (i = 0; i < num_ovs; i++) {
		if (ovs[i].z_order >= MDSS_MDP_MAX_STAGE) {
			pr_err("invalid stage:%u\n", ovs[i].z_order);
			return -EINVAL;
		}
		if (ovs[i].dst_rect.x < left_lm_w) {
			if (left_lm_zo_cnt[ovs[i].z_order] == 2) {
				pr_err("more than 2 ov @ stage%u on left lm\n",
					ovs[i].z_order);
				return -EINVAL;
			}
			left_lm_zo_cnt[ovs[i].z_order]++;
		} else {
			if (right_lm_zo_cnt[ovs[i].z_order] == 2) {
				pr_err("more than 2 ov @ stage%u on right lm\n",
					ovs[i].z_order);
				return -EINVAL;
			}
			right_lm_zo_cnt[ovs[i].z_order]++;
		}
	}

	return 0;
}

static int __handle_overlay_prepare(struct msm_fb_data_type *mfd,
	struct mdp_overlay_list *ovlist, struct mdp_overlay *ip_ovs)
{
	int ret, i;
	int new_reqs = 0, left_cnt = 0, right_cnt = 0;
	int num_ovs = ovlist->num_overlays;
	u32 left_lm_w = left_lm_w_from_mfd(mfd);
	u32 left_lm_ovs = 0, right_lm_ovs = 0;
	bool is_single_layer = false;

	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	struct mdp_overlay *sorted_ovs = NULL;
	struct mdp_overlay *req, *prev_req;

	struct mdss_mdp_pipe *pipe, *left_blend_pipe;
	struct mdss_mdp_pipe *right_plist[MAX_PIPES_PER_LM] = { 0 };
	struct mdss_mdp_pipe *left_plist[MAX_PIPES_PER_LM] = { 0 };

	bool sort_needed = mdata->has_src_split && (num_ovs > 1);

	ret = mutex_lock_interruptible(&mdp5_data->ov_lock);
	if (ret)
		return ret;

	if (mdss_fb_is_power_off(mfd)) {
		mutex_unlock(&mdp5_data->ov_lock);
		return -EPERM;
	}

	if (sort_needed) {
		sorted_ovs = kzalloc(num_ovs * sizeof(*ip_ovs), GFP_KERNEL);
		if (!sorted_ovs) {
			pr_err("error allocating ovlist mem\n");
			mutex_unlock(&mdp5_data->ov_lock);
			return -ENOMEM;
		}
		memcpy(sorted_ovs, ip_ovs, num_ovs * sizeof(*ip_ovs));
		ret = __mdss_overlay_src_split_sort(mfd, sorted_ovs, num_ovs);
		if (ret) {
			pr_err("src_split_sort failed. ret=%d\n", ret);
			kfree(sorted_ovs);
			mutex_unlock(&mdp5_data->ov_lock);
			return ret;
		}
	}

	pr_debug("prepare fb%d num_ovs=%d\n", mfd->index, num_ovs);

	for (i = 0; i < num_ovs; i++) {
		if (IS_RIGHT_MIXER_OV(ip_ovs[i].flags, ip_ovs[i].dst_rect.x,
			left_lm_w))
			right_lm_ovs++;
		else
			left_lm_ovs++;

		if ((left_lm_ovs > 1) && (right_lm_ovs > 1))
			break;
	}

	for (i = 0; i < num_ovs; i++) {
		left_blend_pipe = NULL;

		if (sort_needed) {
			req = &sorted_ovs[i];
			prev_req = (i > 0) ? &sorted_ovs[i - 1] : NULL;

			/*
			 * check if current overlay is at same z_order as
			 * previous one and qualifies as a right blend. If yes,
			 * pass a pointer to the pipe representing previous
			 * overlay or in other terms left blend overlay.
			 */
			if (prev_req && (prev_req->z_order == req->z_order) &&
			    is_ov_right_blend(&prev_req->dst_rect,
				    &req->dst_rect, left_lm_w)) {
				left_blend_pipe = pipe;
			}
		} else {
			req = &ip_ovs[i];
		}

		if (IS_RIGHT_MIXER_OV(ip_ovs[i].flags, ip_ovs[i].dst_rect.x,
			left_lm_w))
			is_single_layer = (right_lm_ovs == 1);
		else
			is_single_layer = (left_lm_ovs == 1);

		req->z_order += MDSS_MDP_STAGE_0;
		ret = mdss_mdp_overlay_pipe_setup(mfd, req, &pipe,
			left_blend_pipe, is_single_layer);
		req->z_order -= MDSS_MDP_STAGE_0;

		if (IS_ERR_VALUE(ret))
			goto validate_exit;

		pr_debug("pnum:%d id:0x%x flags:0x%x dst_x:%d l_blend_pnum%d\n",
			pipe->num, req->id, req->flags, req->dst_rect.x,
			left_blend_pipe ? left_blend_pipe->num : -1);

		/* keep track of the new overlays to unset in case of errors */
		if (pipe->play_cnt == 0)
			new_reqs |= pipe->ndx;

		if (IS_RIGHT_MIXER_OV(pipe->flags, pipe->dst.x, left_lm_w)) {
			if (right_cnt >= MAX_PIPES_PER_LM) {
				pr_err("too many pipes on right mixer\n");
				ret = -EINVAL;
				goto validate_exit;
			}
			right_plist[right_cnt] = pipe;
			right_cnt++;
		} else {
			if (left_cnt >= MAX_PIPES_PER_LM) {
				pr_err("too many pipes on left mixer\n");
				ret = -EINVAL;
				goto validate_exit;
			}
			left_plist[left_cnt] = pipe;
			left_cnt++;
		}
	}

	ret = mdss_mdp_perf_bw_check(mdp5_data->ctl, left_plist, left_cnt,
			right_plist, right_cnt);

validate_exit:
	if (sort_needed)
		ovlist->processed_overlays =
			__mdss_overlay_map(sorted_ovs, ip_ovs, num_ovs, i);
	else
		ovlist->processed_overlays = i;

	if (IS_ERR_VALUE(ret)) {
		pr_debug("err=%d total_ovs:%d processed:%d left:%d right:%d\n",
			ret, num_ovs, ovlist->processed_overlays, left_lm_ovs,
			right_lm_ovs);
		mdss_mdp_overlay_release(mfd, new_reqs);
	}
	mutex_unlock(&mdp5_data->ov_lock);

	kfree(sorted_ovs);

	return ret;
}

static int __handle_ioctl_overlay_prepare(struct msm_fb_data_type *mfd,
		void __user *argp)
{
	struct mdp_overlay_list ovlist;
	struct mdp_overlay *req_list[OVERLAY_MAX];
	struct mdp_overlay *overlays;
	int i, ret;

	if (!mfd_to_ctl(mfd))
		return -ENODEV;

	if (copy_from_user(&ovlist, argp, sizeof(ovlist)))
		return -EFAULT;

	if (ovlist.num_overlays > OVERLAY_MAX) {
		pr_err("Number of overlays exceeds max\n");
		return -EINVAL;
	}

	overlays = kmalloc(ovlist.num_overlays * sizeof(*overlays), GFP_KERNEL);
	if (!overlays) {
		pr_err("Unable to allocate memory for overlays\n");
		return -ENOMEM;
	}

	if (copy_from_user(req_list, ovlist.overlay_list,
				sizeof(struct mdp_overlay *) *
				ovlist.num_overlays)) {
		ret = -EFAULT;
		goto validate_exit;
	}

	for (i = 0; i < ovlist.num_overlays; i++) {
		if (copy_from_user(overlays + i, req_list[i],
				sizeof(struct mdp_overlay))) {
			ret = -EFAULT;
			goto validate_exit;
		}
	}

	ret = __handle_overlay_prepare(mfd, &ovlist, overlays);
	if (!IS_ERR_VALUE(ret)) {
		for (i = 0; i < ovlist.num_overlays; i++) {
			if (copy_to_user(req_list[i], overlays + i,
					sizeof(struct mdp_overlay))) {
				ret = -EFAULT;
				goto validate_exit;
			}
		}
	}

	if (copy_to_user(argp, &ovlist, sizeof(ovlist)))
		ret = -EFAULT;

validate_exit:
	kfree(overlays);

	return ret;
}

static int mdss_mdp_overlay_ioctl_handler(struct msm_fb_data_type *mfd,
					  u32 cmd, void __user *argp)
{
	struct mdp_overlay *req = NULL;
	int val, ret = -ENOSYS;
	struct msmfb_metadata metadata;
	struct mdp_pp_feature_version pp_feature_version;
	struct msmfb_overlay_data data;
	struct mdp_set_cfg cfg;

	switch (cmd) {
	case MSMFB_MDP_PP:
		ret = mdss_mdp_pp_ioctl(mfd, argp);
		break;
	case MSMFB_MDP_PP_GET_FEATURE_VERSION:
		ret = copy_from_user(&pp_feature_version, argp,
				     sizeof(pp_feature_version));
		if (ret) {
			pr_err("copy_from_user failed for pp_feature_version\n");
			ret = -EFAULT;
		} else {
			ret = mdss_mdp_pp_get_version(&pp_feature_version);
			if (!ret) {
				ret = copy_to_user(argp, &pp_feature_version,
						sizeof(pp_feature_version));
				if (ret) {
					pr_err("copy_to_user failed for pp_feature_version\n");
					ret = -EFAULT;
				}
			} else {
				pr_err("get pp version failed ret %d\n", ret);
			}
		}
		break;
	case MSMFB_HISTOGRAM_START:
	case MSMFB_HISTOGRAM_STOP:
	case MSMFB_HISTOGRAM:
		ret = mdss_mdp_histo_ioctl(mfd, cmd, argp);
		break;

	case MSMFB_OVERLAY_GET:
		req = kmalloc(sizeof(struct mdp_overlay), GFP_KERNEL);
		if (!req)
			return -ENOMEM;
		ret = copy_from_user(req, argp, sizeof(*req));
		if (!ret) {
			ret = mdss_mdp_overlay_get(mfd, req);

			if (!IS_ERR_VALUE(ret))
				ret = copy_to_user(argp, req, sizeof(*req));
		}

		if (ret)
			pr_debug("OVERLAY_GET failed (%d)\n", ret);
		break;

	case MSMFB_OVERLAY_SET:
		req = kmalloc(sizeof(struct mdp_overlay), GFP_KERNEL);
		if (!req)
			return -ENOMEM;
		ret = copy_from_user(req, argp, sizeof(*req));
		if (!ret) {
			ret = mdss_mdp_overlay_set(mfd, req);

			if (!IS_ERR_VALUE(ret))
				ret = copy_to_user(argp, req, sizeof(*req));
		}
		if (ret)
			pr_debug("OVERLAY_SET failed (%d)\n", ret);
		break;

	case MSMFB_OVERLAY_UNSET:
		if (!IS_ERR_VALUE(copy_from_user(&val, argp, sizeof(val))))
			ret = mdss_mdp_overlay_unset(mfd, val);
		break;

	case MSMFB_OVERLAY_PLAY:
		ret = copy_from_user(&data, argp, sizeof(data));
		if (!ret)
			ret = mdss_mdp_overlay_play(mfd, &data);

		if (ret)
			pr_debug("OVERLAY_PLAY failed (%d)\n", ret);
		break;

	case MSMFB_OVERLAY_VSYNC_CTRL:
		if (!copy_from_user(&val, argp, sizeof(val))) {
			ret = mdss_mdp_overlay_vsync_ctrl(mfd, val);
		} else {
			pr_err("MSMFB_OVERLAY_VSYNC_CTRL failed (%d)\n", ret);
			ret = -EFAULT;
		}
		break;

	case MSMFB_METADATA_SET:
		ret = copy_from_user(&metadata, argp, sizeof(metadata));
		if (ret)
			return ret;
		ret = mdss_fb_set_metadata(mfd, &metadata);
		break;

	case MSMFB_METADATA_GET:
		ret = copy_from_user(&metadata, argp, sizeof(metadata));
		if (ret)
			return ret;
		ret = mdss_fb_get_metadata(mfd, &metadata);
		if (!ret)
			ret = copy_to_user(argp, &metadata, sizeof(metadata));
		break;

	case MSMFB_OVERLAY_PREPARE:
		ret = __handle_ioctl_overlay_prepare(mfd, argp);
		break;
	case MSMFB_MDP_SET_CFG:
		ret = copy_from_user(&cfg, argp, sizeof(cfg));
		if (ret) {
			pr_err("copy failed MSMFB_MDP_SET_CFG ret %d\n", ret);
			ret = -EFAULT;
			break;
		}
		ret = mdss_mdp_set_cfg(mfd, &cfg);
		break;
	case MSMFB_MDP_SET_PANEL_PPM:
		ret = copy_from_user(&val, argp, sizeof(val));
		if (ret) {
			pr_err("copy failed MSMFB_MDP_SET_PANEL_PPM ret %d\n",
					ret);
			ret = -EFAULT;
			break;
		}
		ret = mdss_fb_set_panel_ppm(mfd, val);
		break;

	default:
		break;
	}

	kfree(req);
	return ret;
}

/**
 * __mdss_mdp_overlay_ctl_init - Helper function to intialize control structure
 * @mfd: msm frame buffer data structure associated with the fb device.
 *
 * Helper function that allocates and initializes the mdp control structure
 * for a frame buffer device. Whenver applicable, this function will also setup
 * the control for the split display path as well.
 *
 * Return: pointer to the newly allocated control structure.
 */
static struct mdss_mdp_ctl *__mdss_mdp_overlay_ctl_init(
	struct msm_fb_data_type *mfd)
{
	int rc = 0;
	struct mdss_mdp_ctl *ctl;
	struct mdss_panel_data *pdata;
	struct mdss_overlay_private *mdp5_data;

	if (!mfd)
		return ERR_PTR(-EINVAL);

	pdata = dev_get_platdata(&mfd->pdev->dev);
	if (!pdata) {
		pr_err("no panel connected for fb%d\n", mfd->index);
		rc = -ENODEV;
		goto error;
	}

	mdp5_data = mfd_to_mdp5_data(mfd);
	if (!mdp5_data) {
		rc = -EINVAL;
		goto error;
	}

	ctl = mdss_mdp_ctl_init(pdata, mfd);
	if (IS_ERR_OR_NULL(ctl)) {
		pr_err("Unable to initialize ctl for fb%d\n",
			mfd->index);
		rc = PTR_ERR(ctl);
		goto error;
	}
	ctl->is_master = true;
	ctl->vsync_handler.vsync_handler =
					mdss_mdp_overlay_handle_vsync;
	ctl->vsync_handler.cmd_post_flush = false;

	ctl->recover_underrun_handler.vsync_handler =
			mdss_mdp_recover_underrun_handler;
	ctl->recover_underrun_handler.cmd_post_flush = false;

	ctl->lineptr_handler.lineptr_handler =
					mdss_mdp_overlay_handle_lineptr;

	INIT_WORK(&ctl->remove_underrun_handler,
				remove_underrun_vsync_handler);

	if (mfd->split_mode == MDP_DUAL_LM_DUAL_DISPLAY) {
		/* enable split display */
		rc = mdss_mdp_ctl_split_display_setup(ctl, pdata->next);
		if (rc) {
			mdss_mdp_ctl_destroy(ctl);
			goto error;
		}
	}

	mdp5_data->ctl = ctl;
error:
	if (rc)
		return ERR_PTR(rc);
	else
		return ctl;
}

static void mdss_mdp_set_lm_flag(struct msm_fb_data_type *mfd)
{
	u32 width;
	struct mdss_data_type *mdata;

	/* if lm_widths are set, the split_mode would have been set */
	if (mfd->panel_info->lm_widths[0] && mfd->panel_info->lm_widths[1])
		return;

	mdata = mdss_mdp_get_mdata();
	width = mfd->fbi->var.xres;

	/* setting the appropriate split_mode for HDMI usecases */
	if ((mfd->split_mode == MDP_SPLIT_MODE_NONE ||
			mfd->split_mode == MDP_DUAL_LM_SINGLE_DISPLAY) &&
			(width > mdata->max_mixer_width)) {
		width /= 2;
		mfd->split_mode = MDP_DUAL_LM_SINGLE_DISPLAY;
		mfd->split_fb_left = width;
		mfd->split_fb_right = width;
	} else if (is_dual_lm_single_display(mfd) &&
		   (width <= mdata->max_mixer_width)) {
		mfd->split_mode = MDP_SPLIT_MODE_NONE;
		mfd->split_fb_left = 0;
		mfd->split_fb_right = 0;
	}
}

static void mdss_mdp_handle_invalid_switch_state(struct msm_fb_data_type *mfd)
{
	int rc = 0;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = mdp5_data->ctl;
	struct mdss_mdp_ctl *sctl = mdss_mdp_get_split_ctl(ctl);
	struct mdss_mdp_data *buf, *tmpbuf;

	mfd->switch_state = MDSS_MDP_NO_UPDATE_REQUESTED;

	/*
	 * Handle only for cmd mode panels as for video mode, buffers
	 * cannot be freed at this point. Needs revisting to handle the
	 * use case for video mode panels.
	 */
	if (mfd->panel_info->type == MIPI_CMD_PANEL) {
		if (ctl->ops.wait_pingpong)
			rc = ctl->ops.wait_pingpong(ctl, NULL);
		if (!rc && sctl && sctl->ops.wait_pingpong)
			rc = sctl->ops.wait_pingpong(sctl, NULL);
		if (rc) {
			pr_err("wait for pp failed\n");
			return;
		}

		mutex_lock(&mdp5_data->list_lock);
		list_for_each_entry_safe(buf, tmpbuf,
				&mdp5_data->bufs_used, buf_list)
			list_move(&buf->buf_list, &mdp5_data->bufs_freelist);
		mutex_unlock(&mdp5_data->list_lock);
	}
}

static int mdss_mdp_overlay_on(struct msm_fb_data_type *mfd)
{
	int rc;
	struct mdss_overlay_private *mdp5_data;
	struct mdss_mdp_ctl *ctl = NULL;

	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	mdp5_data = mfd_to_mdp5_data(mfd);
	if (!mdp5_data)
		return -EINVAL;

	mdss_mdp_set_lm_flag(mfd);

	if (!mdp5_data->ctl) {
		ctl = __mdss_mdp_overlay_ctl_init(mfd);
		if (IS_ERR_OR_NULL(ctl))
			return PTR_ERR(ctl);
	} else {
		ctl = mdp5_data->ctl;
	}

	if (mfd->panel_info->type == WRITEBACK_PANEL && !mdp5_data->wfd) {
		mdp5_data->wfd = mdss_mdp_wfd_init(&mfd->pdev->dev, ctl);
		if (IS_ERR_OR_NULL(mdp5_data->wfd)) {
			rc = PTR_ERR(mdp5_data->wfd);
			goto panel_on;
		}
	}

	if (mdss_fb_is_power_on(mfd)) {
		pr_debug("panel was never turned off\n");
		rc = mdss_mdp_ctl_start(ctl, false);
		goto panel_on;
	}

	rc = mdss_mdp_ctl_intf_event(mdp5_data->ctl, MDSS_EVENT_RESET,
		NULL, false);
	if (rc)
		goto panel_on;

	if (!mfd->panel_info->cont_splash_enabled &&
		(mfd->panel_info->type != DTV_PANEL) &&
			!mfd->panel_info->is_pluggable) {
		rc = mdss_mdp_overlay_start(mfd);
		if (rc)
			goto end;
		if (mfd->panel_info->type != WRITEBACK_PANEL) {
			atomic_inc(&mfd->mdp_sync_pt_data.commit_cnt);
			rc = mdss_mdp_overlay_kickoff(mfd, NULL);
		}
	} else {
		rc = mdss_mdp_ctl_setup(ctl);
		if (rc)
			goto end;
	}

panel_on:
	if (mdp5_data->vsync_en) {
		if ((ctl) && (ctl->ops.add_vsync_handler)) {
			pr_info("reenabling vsync for fb%d\n", mfd->index);
			mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);
			rc = ctl->ops.add_vsync_handler(ctl,
					 &ctl->vsync_handler);
			mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);
		}
	}
	if (IS_ERR_VALUE(rc)) {
		pr_err("Failed to turn on fb%d\n", mfd->index);
		mdss_mdp_overlay_off(mfd);
		goto end;
	}

end:
	return rc;
}

static int mdss_mdp_handoff_cleanup_ctl(struct msm_fb_data_type *mfd)
{
	int rc;
	int need_cleanup;
	struct mdss_overlay_private *mdp5_data;

	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	mdp5_data = mfd_to_mdp5_data(mfd);

	mdss_mdp_overlay_free_fb_pipe(mfd);

	mutex_lock(&mdp5_data->list_lock);
	need_cleanup = !list_empty(&mdp5_data->pipes_cleanup) ||
		!list_empty(&mdp5_data->pipes_used);
	mutex_unlock(&mdp5_data->list_lock);

	if (need_cleanup)
		mdss_mdp_overlay_kickoff(mfd, NULL);

	rc = mdss_mdp_ctl_stop(mdp5_data->ctl, mfd->panel_power_state);
	if (!rc) {
		if (mdss_fb_is_power_off(mfd)) {
			mutex_lock(&mdp5_data->list_lock);
			__mdss_mdp_overlay_free_list_purge(mfd);
			mutex_unlock(&mdp5_data->list_lock);
		}
	}

	rc = mdss_mdp_splash_cleanup(mfd, false);
	if (rc)
		pr_err("%s: failed splash clean up %d\n", __func__, rc);

	return rc;
}

static int mdss_mdp_overlay_off(struct msm_fb_data_type *mfd)
{
	int rc;
	struct mdss_overlay_private *mdp5_data;
	struct mdss_mdp_mixer *mixer;
	struct mdss_mdp_pipe *pipe, *tmp;
	int need_cleanup;
	int retire_cnt;
	bool destroy_ctl = false;

	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	mdp5_data = mfd_to_mdp5_data(mfd);

	if (!mdp5_data || !mdp5_data->ctl) {
		pr_err("ctl not initialized\n");
		return -ENODEV;
	}

	if (!mdss_mdp_ctl_is_power_on(mdp5_data->ctl)) {
		if (mfd->panel_reconfig) {
			if (mfd->panel_info->cont_splash_enabled)
				mdss_mdp_handoff_cleanup_ctl(mfd);

			mdp5_data->borderfill_enable = false;
			mdss_mdp_ctl_destroy(mdp5_data->ctl);
			mdp5_data->ctl = NULL;
		}
		return 0;
	}

	/*
	 * Keep a reference to the runtime pm until the overlay is turned
	 * off, and then release this last reference at the end. This will
	 * help in distinguishing between idle power collapse versus suspend
	 * power collapse
	 */
	pm_runtime_get_sync(&mfd->pdev->dev);

	if (mdss_fb_is_power_on_lp(mfd)) {
		pr_debug("panel not turned off. keeping overlay on\n");
		goto ctl_stop;
	}

	mutex_lock(&mdp5_data->ov_lock);

	mdss_mdp_overlay_free_fb_pipe(mfd);

	mixer = mdss_mdp_mixer_get(mdp5_data->ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (mixer)
		mixer->cursor_enabled = 0;

	mixer = mdss_mdp_mixer_get(mdp5_data->ctl, MDSS_MDP_MIXER_MUX_RIGHT);
	if (mixer)
		mixer->cursor_enabled = 0;

	mutex_lock(&mdp5_data->list_lock);
	if (!list_empty(&mdp5_data->pipes_used)) {
		list_for_each_entry_safe(
			pipe, tmp, &mdp5_data->pipes_used, list) {
			pipe->file = NULL;
			list_move(&pipe->list, &mdp5_data->pipes_cleanup);
		}
	}
	need_cleanup = !list_empty(&mdp5_data->pipes_cleanup);
	mutex_unlock(&mdp5_data->list_lock);
	mutex_unlock(&mdp5_data->ov_lock);

	destroy_ctl = !mfd->ref_cnt || mfd->panel_reconfig;

	mutex_lock(&mfd->switch_lock);
	if (mfd->switch_state != MDSS_MDP_NO_UPDATE_REQUESTED) {
		destroy_ctl = true;
		need_cleanup = false;
		pr_warn("fb%d blank while mode switch (%d) in progress\n",
				mfd->index, mfd->switch_state);
		mdss_mdp_handle_invalid_switch_state(mfd);
	}
	mutex_unlock(&mfd->switch_lock);

	if (need_cleanup) {
		pr_debug("cleaning up pipes on fb%d\n", mfd->index);
		mdss_mdp_overlay_kickoff(mfd, NULL);
	}

ctl_stop:
	/*
	 * If retire fences are still active wait for a vsync time
	 * for retire fence to be updated.
	 * As a last resort signal the timeline if vsync doesn't arrive.
	 */
	mutex_lock(&mfd->mdp_sync_pt_data.sync_mutex);
	retire_cnt = mdp5_data->retire_cnt;
	mutex_unlock(&mfd->mdp_sync_pt_data.sync_mutex);
	if (retire_cnt) {
		u32 fps = mdss_panel_get_framerate(mfd->panel_info);
		u32 vsync_time = 1000 / (fps ? : DEFAULT_FRAME_RATE);

		msleep(vsync_time);

		mutex_lock(&mfd->mdp_sync_pt_data.sync_mutex);
		retire_cnt = mdp5_data->retire_cnt;
		mutex_unlock(&mfd->mdp_sync_pt_data.sync_mutex);
		__vsync_retire_signal(mfd, retire_cnt);

		/*
		 * the retire work can still schedule after above retire_signal
		 * api call. Flush workqueue guarantees that current caller
		 * context is blocked till retire_work finishes. Any work
		 * schedule after flush call should not cause any issue because
		 * retire_signal api checks for retire_cnt with sync_mutex lock.
		 */

		kthread_flush_work(&mdp5_data->vsync_work);
	}

	mutex_lock(&mdp5_data->ov_lock);
	/* set the correct pipe_mapped before ctl_stop */
	mdss_mdp_mixer_update_pipe_map(mdp5_data->ctl,
			MDSS_MDP_MIXER_MUX_LEFT);
	mdss_mdp_mixer_update_pipe_map(mdp5_data->ctl,
			MDSS_MDP_MIXER_MUX_RIGHT);
	rc = mdss_mdp_ctl_stop(mdp5_data->ctl, mfd->panel_power_state);
	if (rc == 0) {
		if (mdss_fb_is_power_off(mfd)) {
			mutex_lock(&mdp5_data->list_lock);
			__mdss_mdp_overlay_free_list_purge(mfd);
			if (!mfd->ref_cnt)
				mdss_mdp_overlay_buf_deinit(mfd);
			mutex_unlock(&mdp5_data->list_lock);
			mdss_mdp_ctl_notifier_unregister(mdp5_data->ctl,
					&mfd->mdp_sync_pt_data.notifier);

			if (destroy_ctl) {
				mdp5_data->borderfill_enable = false;
				mdss_mdp_ctl_destroy(mdp5_data->ctl);
				mdp5_data->ctl = NULL;
			}

			atomic_dec(&mdp5_data->mdata->active_intf_cnt);

			if (!mdp5_data->mdata->idle_pc_enabled ||
				(mfd->panel_info->type != MIPI_CMD_PANEL)) {
				rc = pm_runtime_put(&mfd->pdev->dev);
				if (rc)
					pr_err("unable to suspend w/pm_runtime_put (%d)\n",
						rc);
			}
		}
	}
	mutex_unlock(&mdp5_data->ov_lock);

	if (mdp5_data->wfd) {
		mdss_mdp_wfd_deinit(mdp5_data->wfd);
		mdp5_data->wfd = NULL;
	}

	/* Release the last reference to the runtime device */
	rc = pm_runtime_put(&mfd->pdev->dev);
	if (rc)
		pr_err("unable to suspend w/pm_runtime_put (%d)\n", rc);

	return rc;
}

static int __mdss_mdp_ctl_handoff(struct msm_fb_data_type *mfd,
	struct mdss_mdp_ctl *ctl, struct mdss_data_type *mdata)
{
	int rc = 0;
	int i, j;
	u32 mixercfg;
	struct mdss_mdp_pipe *pipe = NULL;
	struct mdss_overlay_private *mdp5_data;

	if (!ctl || !mdata)
		return -EINVAL;

	mdp5_data = mfd_to_mdp5_data(mfd);

	for (i = 0; i < mdata->nmixers_intf; i++) {
		mixercfg = mdss_mdp_ctl_read(ctl, MDSS_MDP_REG_CTL_LAYER(i));
		pr_debug("for lm%d mixercfg = 0x%09x\n", i, mixercfg);

		j = MDSS_MDP_SSPP_VIG0;
		for (; j < MDSS_MDP_SSPP_CURSOR0 && mixercfg; j++) {
			u32 cfg = j * 3;
			if ((j == MDSS_MDP_SSPP_VIG3) ||
			    (j == MDSS_MDP_SSPP_RGB3)) {
				/* Add 2 to account for Cursor & Border bits */
				cfg += 2;
			}
			if (mixercfg & (0x7 << cfg)) {
				pr_debug("Pipe %d staged\n", j);
				/* bootloader display always uses RECT0 */
				pipe = mdss_mdp_pipe_search(mdata, BIT(j),
					MDSS_MDP_PIPE_RECT0);
				if (!pipe) {
					pr_warn("Invalid pipe %d staged\n", j);
					continue;
				}

				rc = mdss_mdp_pipe_handoff(pipe);
				if (rc) {
					pr_err("Failed to handoff pipe%d\n",
						pipe->num);
					goto exit;
				}

				pipe->mfd = mfd;
				mutex_lock(&mdp5_data->list_lock);
				list_add(&pipe->list, &mdp5_data->pipes_used);
				mutex_unlock(&mdp5_data->list_lock);

				rc = mdss_mdp_mixer_handoff(ctl, i, pipe);
				if (rc) {
					pr_err("failed to handoff mix%d\n", i);
					goto exit;
				}
			}
		}
	}
exit:
	return rc;
}

/**
 * mdss_mdp_overlay_handoff() - Read MDP registers to handoff an active ctl path
 * @mfd: Msm frame buffer structure associated with the fb device.
 *
 * This function populates the MDP software structures with the current state of
 * the MDP hardware to handoff any active control path for the framebuffer
 * device. This is needed to identify any ctl, mixers and pipes being set up by
 * the bootloader to display the splash screen when the continuous splash screen
 * feature is enabled in kernel.
 */
static int mdss_mdp_overlay_handoff(struct msm_fb_data_type *mfd)
{
	int rc = 0;
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = NULL;
	struct mdss_mdp_ctl *sctl = NULL;

	if (!mdp5_data->ctl) {
		ctl = __mdss_mdp_overlay_ctl_init(mfd);
		if (IS_ERR_OR_NULL(ctl)) {
			rc = PTR_ERR(ctl);
			goto error;
		}
	} else {
		ctl = mdp5_data->ctl;
	}

	/*
	 * vsync interrupt needs on during continuous splash, this is
	 * to initialize necessary ctl members here.
	 */
	rc = mdss_mdp_ctl_start(ctl, true);
	if (rc) {
		pr_err("Failed to initialize ctl\n");
		goto error;
	}

	ctl->clk_rate = mdss_mdp_get_clk_rate(MDSS_CLK_MDP_CORE, false);
	pr_debug("Set the ctl clock rate to %d Hz\n", ctl->clk_rate);

	rc = __mdss_mdp_ctl_handoff(mfd, ctl, mdata);
	if (rc) {
		pr_err("primary ctl handoff failed. rc=%d\n", rc);
		goto error;
	}

	if (mfd->split_mode == MDP_DUAL_LM_DUAL_DISPLAY) {
		sctl = mdss_mdp_get_split_ctl(ctl);
		if (!sctl) {
			pr_err("cannot get secondary ctl. fail the handoff\n");
			rc = -EPERM;
			goto error;
		}
		rc = __mdss_mdp_ctl_handoff(mfd, sctl, mdata);
		if (rc) {
			pr_err("secondary ctl handoff failed. rc=%d\n", rc);
			goto error;
		}
	}

	rc = mdss_mdp_smp_handoff(mdata);
	if (rc)
		pr_err("Failed to handoff smps\n");

	mdp5_data->handoff = true;

error:
	if (rc && ctl) {
		mdss_mdp_handoff_cleanup_pipes(mfd, MDSS_MDP_PIPE_TYPE_RGB);
		mdss_mdp_handoff_cleanup_pipes(mfd, MDSS_MDP_PIPE_TYPE_VIG);
		mdss_mdp_handoff_cleanup_pipes(mfd, MDSS_MDP_PIPE_TYPE_DMA);
		mdss_mdp_ctl_destroy(ctl);
		mdp5_data->ctl = NULL;
		mdp5_data->handoff = false;
	}

	return rc;
}

static void __vsync_retire_handle_vsync(struct mdss_mdp_ctl *ctl, ktime_t t)
{
	struct msm_fb_data_type *mfd = ctl->mfd;
	struct mdss_overlay_private *mdp5_data;

	if (!mfd || !mfd->mdp.private1) {
		pr_warn("Invalid handle for vsync\n");
		return;
	}

	mdp5_data = mfd_to_mdp5_data(mfd);
	kthread_queue_work(&mdp5_data->worker, &mdp5_data->vsync_work);
}

static void __vsync_retire_work_handler(struct kthread_work *work)
{
	struct mdss_overlay_private *mdp5_data =
		container_of(work, typeof(*mdp5_data), vsync_work);

	if (!mdp5_data->ctl || !mdp5_data->ctl->mfd)
		return;

	if (!mdp5_data->ctl->ops.remove_vsync_handler)
		return;

	__vsync_retire_signal(mdp5_data->ctl->mfd, 1);
}

static void __vsync_retire_signal(struct msm_fb_data_type *mfd, int val)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);

	mutex_lock(&mfd->mdp_sync_pt_data.sync_mutex);
	if (mdp5_data->retire_cnt > 0) {
		sw_sync_timeline_inc(mdp5_data->vsync_timeline, val);

		mdp5_data->retire_cnt -= min(val, mdp5_data->retire_cnt);
		pr_debug("Retire signaled! timeline val=%d remaining=%d\n",
				mdp5_data->vsync_timeline->value,
				mdp5_data->retire_cnt);
		if (mdp5_data->retire_cnt == 0) {
			mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON);
			mdp5_data->ctl->ops.remove_vsync_handler(mdp5_data->ctl,
					&mdp5_data->vsync_retire_handler);
			mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF);
		}
	}
	mutex_unlock(&mfd->mdp_sync_pt_data.sync_mutex);
}

static struct sync_fence *
__vsync_retire_get_fence(struct msm_sync_pt_data *sync_pt_data)
{
	struct msm_fb_data_type *mfd;
	struct mdss_overlay_private *mdp5_data;
	struct mdss_mdp_ctl *ctl;
	int value;

	mfd = container_of(sync_pt_data, typeof(*mfd), mdp_sync_pt_data);
	mdp5_data = mfd_to_mdp5_data(mfd);

	if (!mdp5_data || !mdp5_data->ctl)
		return ERR_PTR(-ENODEV);

	ctl = mdp5_data->ctl;
	if (!ctl->ops.add_vsync_handler)
		return ERR_PTR(-EOPNOTSUPP);

	if (!mdss_mdp_ctl_is_power_on(ctl)) {
		pr_debug("fb%d vsync pending first update\n", mfd->index);
		return ERR_PTR(-EPERM);
	}

	value = mdp5_data->vsync_timeline->value + 1 + mdp5_data->retire_cnt;
	mdp5_data->retire_cnt++;

	return mdss_fb_sync_get_fence(mdp5_data->vsync_timeline,
			"mdp-retire", value);
}

static void __cwb_wq_handler(struct work_struct *cwb_work)
{
	struct mdss_mdp_cwb *cwb = NULL;
	struct mdss_mdp_wb_data *cwb_data = NULL;

	cwb = container_of(cwb_work, struct mdss_mdp_cwb, cwb_work);
	blocking_notifier_call_chain(&cwb->notifier_head,
			MDP_NOTIFY_FRAME_DONE, NULL);

	/* free the buffer from cleanup queue */
	mutex_lock(&cwb->queue_lock);
	cwb_data = list_first_entry_or_null(&cwb->cleanup_queue,
			struct mdss_mdp_wb_data, next);
	 __list_del_entry(&cwb_data->next);
	mutex_unlock(&cwb->queue_lock);
	if (cwb_data == NULL) {
		pr_err("no output buffer for cwb cleanup\n");
		return;
	}
	mdss_mdp_data_free(&cwb_data->data, true, DMA_FROM_DEVICE);
	kfree(cwb_data);
}

static int __vsync_set_vsync_handler(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl;
	int rc;
	int retire_cnt;

	ctl = mdp5_data->ctl;
	mutex_lock(&mfd->mdp_sync_pt_data.sync_mutex);
	retire_cnt = mdp5_data->retire_cnt;
	mutex_unlock(&mfd->mdp_sync_pt_data.sync_mutex);
	if (!retire_cnt || mdp5_data->vsync_retire_handler.enabled)
		return 0;

	if (!ctl->ops.add_vsync_handler)
		return -EOPNOTSUPP;

	if (!mdss_mdp_ctl_is_power_on(ctl)) {
		pr_debug("fb%d vsync pending first update\n", mfd->index);
		return -EPERM;
	}

	rc = ctl->ops.add_vsync_handler(ctl,
			&mdp5_data->vsync_retire_handler);
	return rc;
}

static int __vsync_retire_setup(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	char name[24];
	struct sched_param param = { .sched_priority = 5 };

	snprintf(name, sizeof(name), "mdss_fb%d_retire", mfd->index);
	mdp5_data->vsync_timeline = sw_sync_timeline_create(name);
	if (mdp5_data->vsync_timeline == NULL) {
		pr_err("cannot vsync create time line");
		return -ENOMEM;
	}

	kthread_init_worker(&mdp5_data->worker);
	kthread_init_work(&mdp5_data->vsync_work, __vsync_retire_work_handler);

	mdp5_data->thread = kthread_run(kthread_worker_fn,
					&mdp5_data->worker, "vsync_retire_work");

	if (IS_ERR(mdp5_data->thread)) {
		pr_err("unable to start vsync thread\n");
		mdp5_data->thread = NULL;
		return -ENOMEM;
	}

	sched_setscheduler(mdp5_data->thread, SCHED_FIFO, &param);

	mfd->mdp_sync_pt_data.get_retire_fence = __vsync_retire_get_fence;

	mdp5_data->vsync_retire_handler.vsync_handler =
		__vsync_retire_handle_vsync;
	mdp5_data->vsync_retire_handler.cmd_post_flush = false;

	return 0;
}

static int mdss_mdp_update_panel_info(struct msm_fb_data_type *mfd,
		int mode, int dest_ctrl)
{
	int ret = 0;
	struct mdss_overlay_private *mdp5_data = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_ctl *ctl = mdp5_data->ctl;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	struct mdss_panel_data *pdata;
	struct mdss_mdp_ctl *sctl;

	if (ctl == NULL) {
		pr_debug("ctl not initialized\n");
		return 0;
	}

	ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_DSI_UPDATE_PANEL_DATA,
		(void *)(unsigned long)mode, CTL_INTF_EVENT_FLAG_DEFAULT);
	if (ret)
		pr_err("Dynamic switch to %s mode failed!\n",
					mode ? "command" : "video");

	if (dest_ctrl) {
		/*
		 * Destroy current ctrl sturcture as this is
		 * going to be re-initialized with the requested mode.
		 */
		mdss_mdp_ctl_destroy(mdp5_data->ctl);
		mdp5_data->ctl = NULL;
	} else {
		pdata = dev_get_platdata(&mfd->pdev->dev);

		if (mdp5_data->mdata->has_pingpong_split &&
			pdata->panel_info.use_pingpong_split)
			mfd->split_mode = MDP_PINGPONG_SPLIT;
		/*
		 * Dynamic change so we need to reconfig instead of
		 * destroying current ctrl sturcture.
		 */
		mdss_mdp_ctl_reconfig(ctl, pdata);

		/*
		 * Set flag when dynamic resolution switch happens before
		 * handoff of cont-splash
		 */
		if (mdata->handoff_pending)
			ctl->switch_with_handoff = true;

		sctl = mdss_mdp_get_split_ctl(ctl);
		if (sctl) {
			if (mfd->split_mode == MDP_DUAL_LM_DUAL_DISPLAY) {
				mdss_mdp_ctl_reconfig(sctl, pdata->next);
				sctl->border_x_off +=
					pdata->panel_info.lcdc.border_left +
					pdata->panel_info.lcdc.border_right;
			} else {
				/*
				 * todo: need to revisit this and properly
				 * cleanup slave resources
				 */
				mdss_mdp_ctl_destroy(sctl);
				ctl->mixer_right = NULL;
			}
		} else if (mfd->split_mode == MDP_DUAL_LM_DUAL_DISPLAY) {
			/* enable split display for the first time */
			ret = mdss_mdp_ctl_split_display_setup(ctl,
					pdata->next);
			if (ret) {
				mdss_mdp_ctl_destroy(ctl);
				mdp5_data->ctl = NULL;
			}
		}
	}

	return ret;
}

int mdss_mdp_input_event_handler(struct msm_fb_data_type *mfd)
{
	int rc = 0;
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);

	if (ctl && mdss_panel_is_power_on(ctl->power_state) &&
	    ctl->ops.early_wake_up_fnc)
		rc = ctl->ops.early_wake_up_fnc(ctl);

	return rc;
}

void mdss_mdp_footswitch_ctrl_handler(bool on)
{
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();

	mdss_mdp_footswitch_ctrl(mdata, on);
}

static void mdss_mdp_signal_retire_fence(struct msm_fb_data_type *mfd,
					 int retire_cnt)
{
	struct mdss_overlay_private *mdp5_data;

	if (!mfd)
		return;

	mdp5_data = mfd_to_mdp5_data(mfd);
	if (!mdp5_data->ctl || !mdp5_data->ctl->ops.remove_vsync_handler)
		return;

	__vsync_retire_signal(mfd, retire_cnt);
	pr_debug("Signaled (%d) pending retire fence\n", retire_cnt);
}

int mdss_mdp_overlay_init(struct msm_fb_data_type *mfd)
{
	struct device *dev = mfd->fbi->dev;
	struct msm_mdp_interface *mdp5_interface = &mfd->mdp;
	struct mdss_overlay_private *mdp5_data = NULL;
	struct irq_info *mdss_irq;
	char timeline_name[32];
	int rc;

	mdp5_data = kzalloc(sizeof(struct mdss_overlay_private), GFP_KERNEL);
	if (!mdp5_data) {
		pr_err("fail to allocate mdp5 private data structure");
		return -ENOMEM;
	}

	mdp5_data->mdata = dev_get_drvdata(mfd->pdev->dev.parent);
	if (!mdp5_data->mdata) {
		pr_err("unable to initialize overlay for fb%d\n", mfd->index);
		rc = -ENODEV;
		goto init_fail;
	}

	mdp5_interface->on_fnc = mdss_mdp_overlay_on;
	mdp5_interface->off_fnc = mdss_mdp_overlay_off;
	mdp5_interface->release_fnc = __mdss_mdp_overlay_release_all;
	mdp5_interface->do_histogram = NULL;
	if (mdp5_data->mdata->ncursor_pipes)
		mdp5_interface->cursor_update = mdss_mdp_hw_cursor_pipe_update;
	else
		mdp5_interface->cursor_update = mdss_mdp_hw_cursor_update;
	mdp5_interface->async_position_update =
		mdss_mdp_async_position_update;
	mdp5_interface->dma_fnc = mdss_mdp_overlay_pan_display;
	mdp5_interface->ioctl_handler = mdss_mdp_overlay_ioctl_handler;
	mdp5_interface->kickoff_fnc = mdss_mdp_overlay_kickoff;
	mdp5_interface->mode_switch = mdss_mode_switch;
	mdp5_interface->mode_switch_post = mdss_mode_switch_post;
	mdp5_interface->pre_commit_fnc = mdss_mdp_overlay_precommit;
	mdp5_interface->splash_init_fnc = mdss_mdp_splash_init;
	mdp5_interface->configure_panel = mdss_mdp_update_panel_info;
	mdp5_interface->input_event_handler = mdss_mdp_input_event_handler;
	mdp5_interface->signal_retire_fence = mdss_mdp_signal_retire_fence;

	/*
	 * Register footswitch control only for primary fb pm
	 * suspend/resume calls.
	 */
	if (mfd->panel_info->is_prim_panel)
		mdp5_interface->footswitch_ctrl =
			mdss_mdp_footswitch_ctrl_handler;

	if (mfd->panel_info->type == WRITEBACK_PANEL) {
		mdp5_interface->atomic_validate =
			mdss_mdp_layer_atomic_validate_wfd;
		mdp5_interface->pre_commit = mdss_mdp_layer_pre_commit_wfd;
		mdp5_interface->is_config_same = mdss_mdp_wfd_is_config_same;
	} else {
		mdp5_interface->atomic_validate =
			mdss_mdp_layer_atomic_validate;
		mdp5_interface->pre_commit = mdss_mdp_layer_pre_commit;
	}

	INIT_LIST_HEAD(&mdp5_data->pipes_used);
	INIT_LIST_HEAD(&mdp5_data->pipes_cleanup);
	INIT_LIST_HEAD(&mdp5_data->pipes_destroy);
	INIT_LIST_HEAD(&mdp5_data->bufs_pool);
	INIT_LIST_HEAD(&mdp5_data->bufs_chunks);
	INIT_LIST_HEAD(&mdp5_data->bufs_used);
	INIT_LIST_HEAD(&mdp5_data->bufs_freelist);
	INIT_LIST_HEAD(&mdp5_data->rot_proc_list);
	mutex_init(&mdp5_data->list_lock);
	mutex_init(&mdp5_data->ov_lock);
	mutex_init(&mdp5_data->dfps_lock);

	mfd->mdp.private1 = mdp5_data;
	mfd->wait_for_kickoff = true;

	mdp5_data->hw_refresh = true;
	mdp5_data->cursor_ndx[CURSOR_PIPE_LEFT] = MSMFB_NEW_REQUEST;
	mdp5_data->cursor_ndx[CURSOR_PIPE_RIGHT] = MSMFB_NEW_REQUEST;

	init_waitqueue_head(&mdp5_data->wb_waitq);
	atomic_set(&mdp5_data->wb_busy, 0);
	mutex_init(&mdp5_data->cwb.queue_lock);
	mutex_init(&mdp5_data->cwb.cwb_sync_pt_data.sync_mutex);
	INIT_LIST_HEAD(&mdp5_data->cwb.data_queue);
	INIT_LIST_HEAD(&mdp5_data->cwb.cleanup_queue);

	snprintf(timeline_name, sizeof(timeline_name), "cwb%d", mfd->index);
	mdp5_data->cwb.cwb_sync_pt_data.fence_name = "cwb-fence";
	mdp5_data->cwb.cwb_sync_pt_data.timeline =
		sw_sync_timeline_create(timeline_name);
	if (mdp5_data->cwb.cwb_sync_pt_data.timeline == NULL) {
		pr_err("failed to create sync pt timeline for cwb\n");
		return -ENOMEM;
	}

	blocking_notifier_chain_register(&mdp5_data->cwb.notifier_head,
			&mdp5_data->cwb.cwb_sync_pt_data.notifier);
	mdp5_data->cwb.cwb_work_queue = alloc_ordered_workqueue("%s",
			WQ_UNBOUND | WQ_MEM_RECLAIM, "cwb_wq");
	if (!mdp5_data->cwb.cwb_work_queue) {
		pr_err("failed to create cwb work queue\n");
		return -EPERM;
	}

	INIT_WORK(&mdp5_data->cwb.cwb_work, __cwb_wq_handler);

	rc = mdss_mdp_overlay_fb_parse_dt(mfd);
	if (rc)
		return rc;

	/*
	 * disable BWC if primary panel is video mode on specific
	 * chipsets to workaround HW problem.
	 */
	if (mdss_has_quirk(mdp5_data->mdata, MDSS_QUIRK_BWCPANIC) &&
	    mfd->panel_info->type == MIPI_VIDEO_PANEL && (0 == mfd->index))
		mdp5_data->mdata->has_bwc = false;

	mfd->panel_orientation = mfd->panel_info->panel_orientation;

	rc = sysfs_create_group(&dev->kobj, &mdp_overlay_sysfs_group);
	if (rc) {
		pr_err("vsync sysfs group creation failed, ret=%d\n", rc);
		goto init_fail;
	}

	mdp5_data->vsync_event_sd = sysfs_get_dirent(dev->kobj.sd,
						     "vsync_event");
	if (!mdp5_data->vsync_event_sd) {
		pr_err("vsync_event sysfs lookup failed\n");
		rc = -ENODEV;
		goto init_fail;
	}

	mdp5_data->lineptr_event_sd = sysfs_get_dirent(dev->kobj.sd,
						     "lineptr_event");
	if (!mdp5_data->lineptr_event_sd) {
		pr_err("lineptr_event sysfs lookup failed\n");
		rc = -ENODEV;
		goto init_fail;
	}

	mdp5_data->hist_event_sd = sysfs_get_dirent(dev->kobj.sd,
						    "hist_event");
	if (!mdp5_data->hist_event_sd) {
		pr_err("hist_event sysfs lookup failed\n");
		rc = -ENODEV;
		goto init_fail;
	}

	mdp5_data->bl_event_sd = sysfs_get_dirent(dev->kobj.sd,
							 "bl_event");
	if (!mdp5_data->bl_event_sd) {
		pr_err("bl_event sysfs lookup failed\n");
		rc = -ENODEV;
		goto init_fail;
	}

	mdp5_data->ad_event_sd = sysfs_get_dirent(dev->kobj.sd,
							 "ad_event");
	if (!mdp5_data->ad_event_sd) {
		pr_err("ad_event sysfs lookup failed\n");
		rc = -ENODEV;
		goto init_fail;
	}

	mdp5_data->ad_bl_event_sd = sysfs_get_dirent(dev->kobj.sd,
							 "ad_bl_event");
	if (!mdp5_data->ad_bl_event_sd) {
		pr_err("ad_bl_event sysfs lookup failed\n");
		rc = -ENODEV;
		goto init_fail;
	}

	rc = sysfs_create_link_nowarn(&dev->kobj,
			&mdp5_data->mdata->pdev->dev.kobj, "mdp");
	if (rc)
		pr_warn("problem creating link to mdp sysfs\n");

	rc = sysfs_create_link_nowarn(&dev->kobj,
			&mfd->pdev->dev.kobj, "mdss_fb");
	if (rc)
		pr_warn("problem creating link to mdss_fb sysfs\n");

	if (mfd->panel_info->type == MIPI_VIDEO_PANEL ||
	    mfd->panel_info->type == DTV_PANEL) {
		rc = sysfs_create_group(&dev->kobj,
			&dynamic_fps_fs_attrs_group);
		if (rc) {
			pr_err("Error dfps sysfs creation ret=%d\n", rc);
			goto init_fail;
		}
	}

	if (mfd->panel_info->mipi.dms_mode ||
			mfd->panel_info->type == MIPI_CMD_PANEL) {
		rc = __vsync_retire_setup(mfd);
		if (IS_ERR_VALUE(rc)) {
			pr_err("unable to create vsync timeline\n");
			goto init_fail;
		}
	}
	mfd->mdp_sync_pt_data.async_wait_fences = true;
	mdp5_data->vsync_en = false;

	pm_runtime_set_suspended(&mfd->pdev->dev);
	pm_runtime_enable(&mfd->pdev->dev);

	kobject_uevent(&dev->kobj, KOBJ_ADD);
	pr_debug("vsync kobject_uevent(KOBJ_ADD)\n");

	mdss_irq = mdss_intr_line();

	/* Adding event timer only for primary panel */
	if ((mfd->index == 0) && (mfd->panel_info->type != WRITEBACK_PANEL)) {
		mdp5_data->cpu_pm_hdl = add_event_timer(mdss_irq->irq,
				mdss_mdp_ctl_event_timer, (void *)mdp5_data);
		if (!mdp5_data->cpu_pm_hdl)
			pr_warn("%s: unable to add event timer\n", __func__);
	}

	if (mfd->panel_info->cont_splash_enabled) {
		rc = mdss_mdp_overlay_handoff(mfd);
		if (rc) {
			/*
			 * Even though handoff failed, it is not fatal.
			 * MDP can continue, just that we would have a longer
			 * delay in transitioning from splash screen to boot
			 * animation
			 */
			pr_warn("Overlay handoff failed for fb%d. rc=%d\n",
				mfd->index, rc);
			rc = 0;
		}
	}
	mdp5_data->dyn_pu_state = mfd->panel_info->partial_update_enabled;

	if (mdss_mdp_pp_overlay_init(mfd))
		pr_warn("Failed to initialize pp overlay data.\n");
	return rc;
init_fail:
	kfree(mdp5_data);
	return rc;
}

static int mdss_mdp_overlay_fb_parse_dt(struct msm_fb_data_type *mfd)
{
	int rc = 0;
	struct platform_device *pdev = mfd->pdev;
	struct mdss_overlay_private *mdp5_mdata = mfd_to_mdp5_data(mfd);

	mdp5_mdata->mixer_swap = of_property_read_bool(pdev->dev.of_node,
					   "qcom,mdss-mixer-swap");
	if (mdp5_mdata->mixer_swap) {
		pr_info("mixer swap is enabled for fb device=%s\n",
			pdev->name);
	}

	return rc;
}

static int mdss_mdp_scaler_lut_init(struct mdss_data_type *mdata,
		struct mdp_scale_luts_info *lut_tbl)
{
	struct mdss_mdp_qseed3_lut_tbl *qseed3_lut_tbl;
	int ret = 0;

	if (!mdata->scaler_off)
		return -EFAULT;

	mutex_lock(&mdata->scaler_off->scaler_lock);

	qseed3_lut_tbl = &mdata->scaler_off->lut_tbl;
	if ((lut_tbl->dir_lut_size !=
		DIR_LUT_IDX * DIR_LUT_COEFFS * sizeof(uint32_t)) ||
		(lut_tbl->cir_lut_size !=
		 CIR_LUT_IDX * CIR_LUT_COEFFS * sizeof(uint32_t)) ||
		(lut_tbl->sep_lut_size !=
		 SEP_LUT_IDX * SEP_LUT_COEFFS * sizeof(uint32_t))) {
		mutex_unlock(&mdata->scaler_off->scaler_lock);
		return -EINVAL;
	}

	if (!qseed3_lut_tbl->dir_lut) {
		qseed3_lut_tbl->dir_lut = devm_kzalloc(&mdata->pdev->dev,
				lut_tbl->dir_lut_size,
				GFP_KERNEL);
		if (!qseed3_lut_tbl->dir_lut) {
			ret = -ENOMEM;
			goto err;
		}
	}

	if (!qseed3_lut_tbl->cir_lut) {
		qseed3_lut_tbl->cir_lut = devm_kzalloc(&mdata->pdev->dev,
				lut_tbl->cir_lut_size,
				GFP_KERNEL);
		if (!qseed3_lut_tbl->cir_lut) {
			ret = -ENOMEM;
			goto fail_free_dir_lut;
		}
	}

	if (!qseed3_lut_tbl->sep_lut) {
		qseed3_lut_tbl->sep_lut = devm_kzalloc(&mdata->pdev->dev,
				lut_tbl->sep_lut_size,
				GFP_KERNEL);
		if (!qseed3_lut_tbl->sep_lut) {
			ret = -ENOMEM;
			goto fail_free_cir_lut;
		}
	}

	/* Invalidate before updating */
	qseed3_lut_tbl->valid = false;

	if (copy_from_user(qseed3_lut_tbl->dir_lut,
				(void *)(unsigned long)lut_tbl->dir_lut,
				lut_tbl->dir_lut_size)) {
			ret = -EINVAL;
			goto fail_free_sep_lut;
	}

	if (copy_from_user(qseed3_lut_tbl->cir_lut,
				(void *)(unsigned long)lut_tbl->cir_lut,
				lut_tbl->cir_lut_size)) {
			ret = -EINVAL;
			goto fail_free_sep_lut;
	}

	if (copy_from_user(qseed3_lut_tbl->sep_lut,
				(void *)(unsigned long)lut_tbl->sep_lut,
				lut_tbl->sep_lut_size)) {
			ret = -EINVAL;
			goto fail_free_sep_lut;
	}

	qseed3_lut_tbl->valid = true;
	mutex_unlock(&mdata->scaler_off->scaler_lock);

	return ret;

fail_free_sep_lut:
	devm_kfree(&mdata->pdev->dev, qseed3_lut_tbl->sep_lut);
fail_free_cir_lut:
	devm_kfree(&mdata->pdev->dev, qseed3_lut_tbl->cir_lut);
fail_free_dir_lut:
	devm_kfree(&mdata->pdev->dev, qseed3_lut_tbl->dir_lut);
err:
	qseed3_lut_tbl->dir_lut = NULL;
	qseed3_lut_tbl->cir_lut = NULL;
	qseed3_lut_tbl->sep_lut = NULL;
	qseed3_lut_tbl->valid = false;
	mutex_unlock(&mdata->scaler_off->scaler_lock);

	return ret;
}

static int mdss_mdp_set_cfg(struct msm_fb_data_type *mfd,
		struct mdp_set_cfg *cfg)
{
	struct mdss_data_type *mdata = mfd_to_mdata(mfd);
	int ret = -EINVAL;
	struct mdp_scale_luts_info luts_info;

	switch (cfg->flags) {
	case MDP_QSEED3_LUT_CFG:
		if (cfg->len != sizeof(luts_info)) {
			pr_err("invalid length %d expected %zd\n", cfg->len,
				sizeof(luts_info));
			ret = -EINVAL;
			break;
		}
		ret = copy_from_user(&luts_info,
				(void *)(unsigned long)cfg->payload, cfg->len);
		if (ret) {
			pr_err("qseed3 lut copy failed ret %d\n", ret);
			ret = -EFAULT;
			break;
		}
		ret = mdss_mdp_scaler_lut_init(mdata, &luts_info);
		break;
	default:
		break;
	}
	return ret;
}
