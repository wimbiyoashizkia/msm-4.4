// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Park Ju Hyung aka arter97 <qkrwngud825@gmail.com>.
 *
 *
 * Adreno idler - Idling algorithm,
 * an efficient workaround for msm-adreno-tz's overheads.
 *
 * Main goal is to lower the power consumptions while maintaining high-performance.
 *
 * Since msm-adreno-tz tends to *not* use the lowest frequency even on idle,
 * Adreno idler replaces msm-adreno-tz's algorithm when it comes to
 * calculating idle frequency (mostly by ondemand's method).
 * The higher frequencies are not touched with this algorithm, so high-demanding
 * games will (most likely) not suffer from worsened performance.
 */

#include <linux/module.h>
#include <linux/devfreq.h>
#include <linux/msm_adreno_devfreq.h>

#define ADRENO_IDLER_MAJOR_VERSION 1
#define ADRENO_IDLER_MINOR_VERSION 1

/* 
 * stats.busy_time threshold for determining if the given workload is idle.
 * Any workload higher than this will be treated as a non-idle workload.
 * Adreno idler will more actively try to ramp down the frequency
 * if this is set to a higher value.
 */
static unsigned long idleworkload = 7000;

/* Number of events to wait before ramping down the frequency.
 * The idlewait'th events before current one must be all idle before
 * Adreno idler ramps down the frequency.
 * This implementation is to prevent micro-lags on scrolling or playing games.
 * Adreno idler will more actively try to ramp down the frequency
 * if this is set to a lower value.
 */
static unsigned int idlewait = 15;

/* Taken from ondemand */
static unsigned int downdifferential = 24;

/* Master switch to activate the whole routine */
static bool adreno_idler_active = true;

static unsigned int idlecount = 0;

int adreno_idler(struct devfreq_dev_status stats, struct devfreq *devfreq,
		 unsigned long *freq)
{
	if (!adreno_idler_active)
		return 0;

	if (stats.busy_time < idleworkload) {
		/* busy_time >= idleworkload should be considered as a non-idle workload. */
		idlecount++;
		if (*freq == devfreq->profile->freq_table[devfreq->profile->max_state - 1]) {
			/*
			 * Frequency is already at its lowest.
			  * No need to calculate things, so bail out.
			  */
			return 1;
		}
		if (idlecount >= idlewait &&
		    stats.busy_time * 100 < stats.total_time * downdifferential) {
			/* We are idle for (idlewait + 1)'th time! Ramp down the frequency now. */
			*freq = devfreq->profile->freq_table[devfreq->profile->max_state - 1];
			idlecount--;
			return 1;
		}
	} else {
		idlecount = 0;
		/* 
		 * Do not return 1 here and allow rest of the algorithm to
		 * figure out the appropriate frequency for current workload.
		 * It can even set it back to the lowest frequency.
		 */
	}
	return 0;
}
EXPORT_SYMBOL(adreno_idler);

static int __init adreno_idler_init(void)
{
	pr_info("adreno_idler: version %d.%d by arter97\n",
		 ADRENO_IDLER_MAJOR_VERSION,
		 ADRENO_IDLER_MINOR_VERSION);

	return 0;
}
subsys_initcall(adreno_idler_init);

static void __exit adreno_idler_exit(void)
{
	return;
}
module_exit(adreno_idler_exit);

MODULE_AUTHOR("Park Ju Hyung <qkrwngud825@gmail.com>");
MODULE_DESCRIPTION("'adreno_idler - A powersaver for Adreno TZ"
	"Control idle algorithm for Adreno GPU series");
MODULE_LICENSE("GPL");
