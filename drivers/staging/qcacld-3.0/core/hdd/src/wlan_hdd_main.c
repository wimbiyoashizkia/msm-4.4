/*
 * Copyright (c) 2012-2020 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 *  DOC: wlan_hdd_main.c
 *
 *  WLAN Host Device Driver implementation
 *
 */

/* Include Files */
#include <wlan_hdd_includes.h>
#include <cds_api.h>
#include <cds_sched.h>
#include <linux/cpu.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <wlan_hdd_tx_rx.h>
#include <wni_api.h>
#include <wlan_hdd_cfg.h>
#include <wlan_ptt_sock_svc.h>
#include <dbglog_host.h>
#include <wlan_logging_sock_svc.h>
#include <wlan_hdd_wowl.h>
#include <wlan_hdd_misc.h>
#include <wlan_hdd_wext.h>
#include "wlan_hdd_trace.h"
#include "wlan_hdd_ioctl.h"
#include "wlan_hdd_ftm.h"
#include "wlan_hdd_power.h"
#include "wlan_hdd_stats.h"
#include "wlan_hdd_scan.h"
#include "wlan_hdd_request_manager.h"
#include "qdf_types.h"
#include "qdf_trace.h"
#include <cdp_txrx_peer_ops.h>
#include <cdp_txrx_stats.h>

#include <net/addrconf.h>
#include <linux/wireless.h>
#include <net/cfg80211.h>
#include <linux/inetdevice.h>
#include <net/addrconf.h>
#include "wlan_hdd_cfg80211.h"
#include "wlan_hdd_ext_scan.h"
#include "wlan_hdd_p2p.h"
#include <linux/rtnetlink.h>
#include "sap_api.h"
#include <linux/semaphore.h>
#include <linux/ctype.h>
#include <linux/compat.h>
#include <linux/reboot.h>
#ifdef MSM_PLATFORM
#include <soc/qcom/subsystem_restart.h>
#endif
#include <wlan_hdd_hostapd.h>
#include <wlan_hdd_softap_tx_rx.h>
#include "cfg_api.h"
#include "qwlan_version.h"
#include "wma_types.h"
#include "wlan_hdd_tdls.h"
#ifdef FEATURE_WLAN_CH_AVOID
#include "cds_regdomain.h"
#endif /* FEATURE_WLAN_CH_AVOID */
#include "cdp_txrx_flow_ctrl_v2.h"
#include "pld_common.h"
#include "wlan_hdd_ocb.h"
#include "wlan_hdd_nan.h"
#include "wlan_hdd_debugfs.h"
#include "wlan_hdd_debugfs_csr.h"
#include "wlan_hdd_driver_ops.h"
#include "epping_main.h"
#include "wlan_hdd_data_stall_detection.h"

#include <wlan_hdd_ipa.h>
#include "hif.h"
#include "wma.h"
#include "cds_concurrency.h"
#include "wlan_hdd_tsf.h"
#include "wlan_hdd_green_ap.h"
#include "bmi.h"
#include <wlan_hdd_regulatory.h>
#include "ol_rx_fwd.h"
#include "wlan_hdd_lpass.h"
#include "nan_api.h"
#include <wlan_hdd_napi.h>
#include "wlan_hdd_disa.h"
#include "ol_txrx.h"
#include "cds_utils.h"
#include "sir_api.h"
#include "wlan_hdd_spectralscan.h"
#include "sme_power_save_api.h"
#include "wlan_hdd_sysfs.h"
#ifdef WLAN_FEATURE_APF
#include "wlan_hdd_apf.h"
#endif

#ifdef CNSS_GENL
#include <net/cnss_nl.h>
#endif

#ifdef MODULE
#define WLAN_MODULE_NAME  module_name(THIS_MODULE)
#else
#define WLAN_MODULE_NAME  "wlan"
#endif

#ifdef TIMER_MANAGER
#define TIMER_MANAGER_STR " +TIMER_MANAGER"
#else
#define TIMER_MANAGER_STR ""
#endif

#ifdef MEMORY_DEBUG
#define MEMORY_DEBUG_STR " +MEMORY_DEBUG"
#else
#define MEMORY_DEBUG_STR ""
#endif

#ifdef PANIC_ON_BUG
#define PANIC_ON_BUG_STR " +PANIC_ON_BUG"
#else
#define PANIC_ON_BUG_STR ""
#endif

bool g_is_system_reboot_triggered;
int wlan_start_ret_val;
static DECLARE_COMPLETION(wlan_start_comp);
static unsigned int dev_num = 1;
static struct cdev wlan_hdd_state_cdev;
static struct class *class;
static dev_t device;
#ifndef MODULE
static struct gwlan_loader *wlan_loader;
struct gwlan_loader {
	struct kobject *boot_wlan_obj;
	struct attribute_group *attr_group;
};

static ssize_t wlan_boot_cb_fake(struct kobject *kobj,
			    struct kobj_attribute *attr,
			    const char *buf,
			    size_t count);

static struct kobj_attribute wlan_boot_attribute =
	__ATTR(boot_wlan, 0220, NULL, wlan_boot_cb_fake);

static struct attribute *attrs[] = {
	&wlan_boot_attribute.attr,
	NULL,
};

static struct work_struct boot_work;
#endif

#define HDD_OPS_INACTIVITY_TIMEOUT (120000)
#define MAX_OPS_NAME_STRING_SIZE 20
#define RATE_LIMIT_ERROR_LOG (256)

static qdf_timer_t hdd_drv_ops_inactivity_timer;
static struct task_struct *hdd_drv_ops_task;
static char drv_ops_string[MAX_OPS_NAME_STRING_SIZE];

/* the Android framework expects this param even though we don't use it */
#define BUF_LEN 20
static char fwpath_buffer[BUF_LEN];
static struct kparam_string fwpath = {
	.string = fwpath_buffer,
	.maxlen = BUF_LEN,
};

static char *country_code;
static int enable_11d = -1;
static int enable_dfs_chan_scan = -1;

/*
 * spinlock for synchronizing asynchronous request/response
 * (full description of use in wlan_hdd_main.h)
 */
DEFINE_SPINLOCK(hdd_context_lock);

DEFINE_MUTEX(hdd_init_deinit_lock);

#define WLAN_NLINK_CESIUM 30

static qdf_wake_lock_t wlan_wake_lock;

#define WOW_MAX_FILTER_LISTS 1
#define WOW_MAX_FILTERS_PER_LIST 4
#define WOW_MIN_PATTERN_SIZE 6
#define WOW_MAX_PATTERN_SIZE 64

#define IS_IDLE_STOP (!cds_is_driver_unloading() && \
		      !cds_is_driver_recovering())

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0))
static const struct wiphy_wowlan_support wowlan_support_reg_init = {
	.flags = WIPHY_WOWLAN_ANY |
		 WIPHY_WOWLAN_MAGIC_PKT |
		 WIPHY_WOWLAN_DISCONNECT |
		 WIPHY_WOWLAN_SUPPORTS_GTK_REKEY |
		 WIPHY_WOWLAN_GTK_REKEY_FAILURE |
		 WIPHY_WOWLAN_EAP_IDENTITY_REQ |
		 WIPHY_WOWLAN_4WAY_HANDSHAKE |
		 WIPHY_WOWLAN_RFKILL_RELEASE,
	.n_patterns = WOW_MAX_FILTER_LISTS * WOW_MAX_FILTERS_PER_LIST,
	.pattern_min_len = WOW_MIN_PATTERN_SIZE,
	.pattern_max_len = WOW_MAX_PATTERN_SIZE,
};
#endif

int limit_off_chan_tbl[HDD_MAX_AC][HDD_MAX_OFF_CHAN_ENTRIES] = {
	{ HDD_AC_BK_BIT, HDD_MAX_OFF_CHAN_TIME_FOR_BK },
	{ HDD_AC_BE_BIT, HDD_MAX_OFF_CHAN_TIME_FOR_BE },
	{ HDD_AC_VI_BIT, HDD_MAX_OFF_CHAN_TIME_FOR_VI },
	{ HDD_AC_VO_BIT, HDD_MAX_OFF_CHAN_TIME_FOR_VO },
};

struct notifier_block hdd_netdev_notifier;
struct notifier_block system_reboot_notifier;

struct sock *cesium_nl_srv_sock;
#ifdef FEATURE_WLAN_AUTO_SHUTDOWN
static void wlan_hdd_auto_shutdown_cb(void);
#endif

void hdd_start_complete(int ret)
{
	wlan_start_ret_val = ret;

	complete(&wlan_start_comp);
}

/**
 * hdd_set_rps_cpu_mask - set RPS CPU mask for interfaces
 * @hdd_ctx: pointer to hdd_context_t
 *
 * Return: none
 */
static void hdd_set_rps_cpu_mask(hdd_context_t *hdd_ctx)
{
	hdd_adapter_t *adapter;
	hdd_adapter_list_node_t *adapter_node, *next;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);
	while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
		adapter = adapter_node->pAdapter;
		if (NULL != adapter)
			hdd_send_rps_ind(adapter);
		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}
}

/**
 * wlan_hdd_txrx_pause_cb() - pause callback from txrx layer
 * @vdev_id: vdev_id
 * @action: action type
 * @reason: reason type
 *
 * Return: none
 */
void wlan_hdd_txrx_pause_cb(uint8_t vdev_id,
		enum netif_action_type action, enum netif_reason_type reason)
{
	hdd_context_t *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	hdd_adapter_t *adapter;

	if (!hdd_ctx) {
		hdd_err("hdd ctx is NULL");
		return;
	}
	adapter = hdd_get_adapter_by_vdev(hdd_ctx, vdev_id);

	wlan_hdd_netif_queue_control(adapter, action, reason);
}

/*
 * Store WLAN driver version and timestamp info in global variables such that
 * crash debugger can extract them from driver debug symbol and crashdump for
 * post processing
 */
#ifdef BUILD_TAG
uint8_t g_wlan_driver_version[] = QWLAN_VERSIONSTR "; " BUILD_TAG;
#else
uint8_t g_wlan_driver_version[] = QWLAN_VERSIONSTR;
#endif

/**
 * hdd_device_mode_to_string() - return string conversion of device mode
 * @device_mode: device mode
 *
 * This utility function helps log string conversion of device mode.
 *
 * Return: string conversion of device mode, if match found;
 *	   "Unknown" otherwise.
 */
const char *hdd_device_mode_to_string(uint8_t device_mode)
{
	switch (device_mode) {
	CASE_RETURN_STRING(QDF_STA_MODE);
	CASE_RETURN_STRING(QDF_SAP_MODE);
	CASE_RETURN_STRING(QDF_P2P_CLIENT_MODE);
	CASE_RETURN_STRING(QDF_P2P_GO_MODE);
	CASE_RETURN_STRING(QDF_FTM_MODE);
	CASE_RETURN_STRING(QDF_IBSS_MODE);
	CASE_RETURN_STRING(QDF_MONITOR_MODE);
	CASE_RETURN_STRING(QDF_P2P_DEVICE_MODE);
	CASE_RETURN_STRING(QDF_OCB_MODE);
	CASE_RETURN_STRING(QDF_NDI_MODE);
	default:
		return "Unknown";
	}
}

/**
 * hdd_validate_channel_and_bandwidth() - Validate the channel-bandwidth combo
 * @adapter: HDD adapter
 * @chan_number: Channel number
 * @chan_bw: Bandwidth
 *
 * Checks if the given bandwidth is valid for the given channel number.
 *
 * Return: 0 for success, non-zero for failure
 */
int hdd_validate_channel_and_bandwidth(hdd_adapter_t *adapter,
		uint32_t chan_number,
		enum phy_ch_width chan_bw)
{
	uint8_t chan[WNI_CFG_VALID_CHANNEL_LIST_LEN];
	uint32_t len = WNI_CFG_VALID_CHANNEL_LIST_LEN, i;
	bool found = false;
	tHalHandle hal;

	hal = WLAN_HDD_GET_HAL_CTX(adapter);
	if (!hal) {
		hdd_err("Invalid HAL context");
		return -EINVAL;
	}

	if (0 != sme_cfg_get_str(hal, WNI_CFG_VALID_CHANNEL_LIST, chan, &len)) {
		hdd_err("No valid channel list");
		return -EOPNOTSUPP;
	}

	for (i = 0; i < len; i++) {
		if (chan[i] == chan_number) {
			found = true;
			break;
		}
	}

	if (found == false) {
		hdd_err("Channel not in driver's valid channel list");
		return -EOPNOTSUPP;
	}

	if ((!CDS_IS_CHANNEL_24GHZ(chan_number)) &&
			(!CDS_IS_CHANNEL_5GHZ(chan_number))) {
		hdd_err("CH %d is not in 2.4GHz or 5GHz", chan_number);
		return -EINVAL;
	}

	if (CDS_IS_CHANNEL_24GHZ(chan_number)) {
		if (chan_bw == CH_WIDTH_80MHZ) {
			hdd_err("BW80 not possible in 2.4GHz band");
			return -EINVAL;
		}
		if ((chan_bw != CH_WIDTH_20MHZ) && (chan_number == 14) &&
				(chan_bw != CH_WIDTH_MAX)) {
			hdd_err("Only BW20 possible on channel 14");
			return -EINVAL;
		}
	}

	if (CDS_IS_CHANNEL_5GHZ(chan_number)) {
		if ((chan_bw != CH_WIDTH_20MHZ) && (chan_number == 165) &&
				(chan_bw != CH_WIDTH_MAX)) {
			hdd_err("Only BW20 possible on channel 165");
			return -EINVAL;
		}
	}

	return 0;
}

#ifdef MODULE
/**
 * hdd_wait_for_recovery_completion() - Wait for cds recovery completion
 *
 * Block the unloading of the driver (or) interface up until the
 * cds recovery is completed
 *
 * Return: true for recovery completion else false
 */
static bool hdd_wait_for_recovery_completion(void)
{
	int retry = 0;

	/* Wait for recovery to complete */
	while (cds_is_driver_recovering()) {
		if (retry == HDD_MOD_EXIT_SSR_MAX_RETRIES/2)
			hdd_err("Recovery in progress; wait here!!!");

		if (g_is_system_reboot_triggered) {
			hdd_info("System Reboot happening ignore unload!!");
			return false;
		}

		msleep(1000);
		if (retry++ == HDD_MOD_EXIT_SSR_MAX_RETRIES) {
			hdd_err("SSR never completed, error");
			/*
			 * Trigger the bug_on in the internal builds, in the
			 * customer builds self-recovery will be enabled
			 * in those cases just return error.
			 */
			if (cds_is_self_recovery_enabled())
				return false;
			QDF_BUG(0);
		}
	}

	hdd_info("Recovery completed successfully!");
	return true;
}
#endif


static int __hdd_netdev_notifier_call(struct notifier_block *nb,
				    unsigned long state, void *data)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0))
	struct netdev_notifier_info *dev_notif_info = data;
	struct net_device *dev = dev_notif_info->dev;
#else
	struct net_device *dev = data;
#endif
	hdd_adapter_t *adapter = WLAN_HDD_GET_PRIV_PTR(dev);
	hdd_context_t *hdd_ctx;

	ENTER_DEV(dev);

	/* Make sure that this callback corresponds to our device. */
	if ((strncmp(dev->name, "wlan", 4)) && (strncmp(dev->name, "p2p", 3)))
		return NOTIFY_DONE;

	if ((adapter->magic != WLAN_HDD_ADAPTER_MAGIC) ||
	    (adapter->dev != dev)) {
		hdd_err("device adapter is not matching!!!");
		return NOTIFY_DONE;
	}

	if (!dev->ieee80211_ptr) {
		hdd_debug("ieee80211_ptr is NULL!!!");
		return NOTIFY_DONE;
	}

	hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	if (NULL == hdd_ctx) {
		hdd_err("HDD Context Null Pointer");
		QDF_ASSERT(0);
		return NOTIFY_DONE;
	}

	if (hdd_ctx->driver_status == DRIVER_MODULES_CLOSED) {
		hdd_debug("%s: Driver module is closed", __func__);
		return NOTIFY_DONE;
	}

	if (cds_is_driver_recovering()) {
		hdd_debug("Driver is recovering");
		return NOTIFY_DONE;
	}

	if (cds_is_driver_in_bad_state()) {
		hdd_debug("Driver is in failed recovery state");
		return NOTIFY_DONE;
	}

	hdd_debug("%s New Net Device State = %lu",
	       dev->name, state);

	switch (state) {
	case NETDEV_REGISTER:
		break;

	case NETDEV_UNREGISTER:
		break;

	case NETDEV_UP:
		sme_ch_avoid_update_req(hdd_ctx->hHal);
		break;

	case NETDEV_DOWN:
		break;

	case NETDEV_CHANGE:
		if (true == adapter->isLinkUpSvcNeeded)
			complete(&adapter->linkup_event_var);
		break;

	case NETDEV_GOING_DOWN:
		if (adapter->scan_info.mScanPending != false) {
			unsigned long rc;

			INIT_COMPLETION(adapter->scan_info.
					abortscan_event_var);
			hdd_abort_mac_scan(adapter->pHddCtx,
					   adapter->sessionId,
					   INVALID_SCAN_ID,
					   eCSR_SCAN_ABORT_DEFAULT);
			rc = wait_for_completion_timeout(
				&adapter->scan_info.abortscan_event_var,
				msecs_to_jiffies(WLAN_WAIT_TIME_ABORTSCAN));
			if (!rc)
				hdd_err("Timeout occurred while waiting for abortscan");
		}
		cds_flush_work(&adapter->scan_block_work);
		hdd_debug("Scan is not Pending from user");
		/*
		 * After NETDEV_GOING_DOWN, kernel calls hdd_stop.Irrespective
		 * of return status of hdd_stop call, kernel resets the IFF_UP
		 * flag after which driver does not send the cfg80211_scan_done.
		 * Ensure to cleanup the scan queue in NETDEV_GOING_DOWN
		 */

		hdd_cleanup_scan_queue(hdd_ctx, adapter);
		break;

	default:
		break;
	}

	return NOTIFY_DONE;
}

/**
 * hdd_netdev_notifier_call() - netdev notifier callback function
 * @nb: pointer to notifier block
 * @state: state
 * @ndev: ndev pointer
 *
 * Return: 0 on success, error number otherwise.
 */
static int hdd_netdev_notifier_call(struct notifier_block *nb,
					unsigned long state,
					void *ndev)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_netdev_notifier_call(nb, state, ndev);
	cds_ssr_unprotect(__func__);

	return ret;
}

struct notifier_block hdd_netdev_notifier = {
	.notifier_call = hdd_netdev_notifier_call,
};

static int system_reboot_notifier_call(struct notifier_block *nb,
				       unsigned long msg_type, void *_unused)
{
	switch (msg_type) {
	case SYS_DOWN:
	case SYS_HALT:
	case SYS_POWER_OFF:
		g_is_system_reboot_triggered = true;
		hdd_info("reboot, reason: %ld", msg_type);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

struct notifier_block system_reboot_notifier = {
	.notifier_call = system_reboot_notifier_call,
};

/* variable to hold the insmod parameters */
static int con_mode;

/* Variable to hold connection mode including module parameter con_mode */
static int curr_con_mode;

/**
 * hdd_map_nl_chan_width() - Map NL channel width to internal representation
 * @ch_width: NL channel width
 *
 * Converts the NL channel width to the driver's internal representation
 *
 * Return: Converted channel width. In case of non matching NL channel width,
 * CH_WIDTH_MAX will be returned.
 */
enum phy_ch_width hdd_map_nl_chan_width(enum nl80211_chan_width ch_width)
{
	uint8_t fw_ch_bw;

	fw_ch_bw = wma_get_vht_ch_width();
	switch (ch_width) {
	case NL80211_CHAN_WIDTH_20_NOHT:
	case NL80211_CHAN_WIDTH_20:
		return CH_WIDTH_20MHZ;
	case NL80211_CHAN_WIDTH_40:
		return CH_WIDTH_40MHZ;
	case NL80211_CHAN_WIDTH_80:
		return CH_WIDTH_80MHZ;
	case NL80211_CHAN_WIDTH_80P80:
		if (fw_ch_bw == WNI_CFG_VHT_CHANNEL_WIDTH_80_PLUS_80MHZ)
			return CH_WIDTH_80P80MHZ;
		else if (fw_ch_bw == WNI_CFG_VHT_CHANNEL_WIDTH_160MHZ)
			return CH_WIDTH_160MHZ;
		else
			return CH_WIDTH_80MHZ;
	case NL80211_CHAN_WIDTH_160:
		if (fw_ch_bw >= WNI_CFG_VHT_CHANNEL_WIDTH_160MHZ)
			return CH_WIDTH_160MHZ;
		else
			return CH_WIDTH_80MHZ;
	case NL80211_CHAN_WIDTH_5:
		return CH_WIDTH_5MHZ;
	case NL80211_CHAN_WIDTH_10:
		return CH_WIDTH_10MHZ;
	default:
		hdd_warn("Invalid channel width %d, setting to default",
				ch_width);
		return CH_WIDTH_INVALID;
	}
}

/* wlan_hdd_find_opclass() - Find operating class for a channel
 * @hal: handler to HAL
 * @channel: channel id
 * @bw_offset: bandwidth offset
 *
 * Function invokes sme api to find the operating class
 *
 * Return: operating class
 */
uint8_t wlan_hdd_find_opclass(tHalHandle hal, uint8_t channel,
				uint8_t bw_offset)
{
	uint8_t opclass = 0;

	sme_get_opclass(hal, channel, bw_offset, &opclass);
	return opclass;
}

/**
 * hdd_qdf_trace_enable() - configure initial QDF Trace enable
 * @moduleId:	Module whose trace level is being configured
 * @bitmask:	Bitmask of log levels to be enabled
 *
 * Called immediately after the cfg.ini is read in order to configure
 * the desired trace levels.
 *
 * Return: None
 */
static void hdd_qdf_trace_enable(QDF_MODULE_ID moduleId, uint32_t bitmask)
{
	QDF_TRACE_LEVEL level;

	/*
	 * if the bitmask is the default value, then a bitmask was not
	 * specified in cfg.ini, so leave the logging level alone (it
	 * will remain at the "compiled in" default value)
	 */
	if (CFG_QDF_TRACE_ENABLE_DEFAULT == bitmask)
		return;

	/* a mask was specified.  start by disabling all logging */
	qdf_trace_set_value(moduleId, QDF_TRACE_LEVEL_NONE, 0);

	/* now cycle through the bitmask until all "set" bits are serviced */
	level = QDF_TRACE_LEVEL_FATAL;
	while (0 != bitmask) {
		if (bitmask & 1)
			qdf_trace_set_value(moduleId, level, 1);

		level++;
		bitmask >>= 1;
	}
}

/**
 * wlan_hdd_validate_context_in_loading() - check the HDD context in loading
 * @hdd_ctx:	HDD context pointer
 *
 * Return: 0 if the context is valid. Error code otherwise
 */
int wlan_hdd_validate_context_in_loading(hdd_context_t *hdd_ctx)
{
	if (NULL == hdd_ctx || NULL == hdd_ctx->config) {
		hdd_info("HDD context is Null");
		return -ENODEV;
	}

	if (cds_is_driver_recovering()) {
		hdd_info("Recovery in Progress. State: 0x%x Ignore!!!",
			cds_get_driver_state());
		return -EAGAIN;
	}

	if (hdd_ctx->start_modules_in_progress ||
	    hdd_ctx->stop_modules_in_progress) {
		hdd_info("Start/Stop Modules in progress. Ignore!!!");
		return -EAGAIN;
	}

	return 0;
}


/**
 * wlan_hdd_validate_context() - check the HDD context
 * @hdd_ctx:	HDD context pointer
 *
 * Return: 0 if the context is valid. Error code otherwise
 */
int wlan_hdd_validate_context(hdd_context_t *hdd_ctx)
{
	if (NULL == hdd_ctx || NULL == hdd_ctx->config) {
		hdd_debug("HDD context is Null");
		return -ENODEV;
	}

	if (cds_is_driver_recovering()) {
		hdd_debug("Recovery in Progress. State: 0x%x Ignore!!!",
			cds_get_driver_state());
		return -EAGAIN;
	}

	if (cds_is_load_or_unload_in_progress())
		return -EAGAIN;

	if (hdd_ctx->start_modules_in_progress ||
	    hdd_ctx->stop_modules_in_progress) {
		hdd_debug("Start/Stop Modules in progress. Ignore!!!");
		return -EAGAIN;
	}

	if (cds_is_driver_in_bad_state()) {
		hdd_debug("driver in bad State: 0x%x Ignore!!!",
			cds_get_driver_state());
		return -EAGAIN;
	}

	if (cds_is_fw_down()) {
		hdd_debug("FW is down: 0x%x Ignore!!!",
			cds_get_driver_state());
		return -EAGAIN;
	}

	if (cds_is_driver_thermal_mitigated()) {
		hdd_debug("Driver in thermal mitigated state: 0x%x Block!!!",
			  cds_get_driver_state());
		return -EAGAIN;
	}

	return 0;
}

int hdd_validate_adapter(hdd_adapter_t *adapter)
{
	if (!adapter) {
		hdd_err("adapter is null");
		return -EINVAL;
	}

	if (adapter->magic != WLAN_HDD_ADAPTER_MAGIC) {
		hdd_err("bad adapter magic");
		return -EINVAL;
	}

	if (!adapter->dev) {
		hdd_err("adapter net_device is null");
		return -EINVAL;
	}

	if (!(adapter->dev->flags & IFF_UP)) {
		hdd_info("adapter net_device is not up");
		return -EAGAIN;
	}

	if (adapter->sessionId == HDD_SESSION_ID_INVALID) {
		hdd_info("adapter session is not open");
		return -EAGAIN;
	}

	if (adapter->sessionId >= MAX_NUMBER_OF_ADAPTERS) {
		hdd_err("bad adapter session Id: %u", adapter->sessionId);
		return -EINVAL;
	}

	return 0;
}

QDF_STATUS __wlan_hdd_validate_mac_address(struct qdf_mac_addr *mac_addr,
					   const char *func)
{
	if (!mac_addr) {
		hdd_err("Received NULL mac address (via %s)", func);
		return QDF_STATUS_E_INVAL;
	}

	if (qdf_is_macaddr_zero(mac_addr)) {
		hdd_err("MAC is all zero (via %s)", func);
		return QDF_STATUS_E_INVAL;
	}

	if (qdf_is_macaddr_broadcast(mac_addr)) {
		hdd_err("MAC is Broadcast (via %s)", func);
		return QDF_STATUS_E_INVAL;
	}

	if (ETHER_IS_MULTICAST(mac_addr->bytes)) {
		hdd_err("MAC is Multicast (via %s)", func);
		return QDF_STATUS_E_INVAL;
	}

	return QDF_STATUS_SUCCESS;
}

/**
 * wlan_hdd_modules_are_enabled() - Check modules status
 * @hdd_ctx: HDD context pointer
 *
 * Check's the driver module's state and returns true if the
 * modules are enabled returns false if modules are closed.
 *
 * Return: True if modules are enabled or false.
 */
bool wlan_hdd_modules_are_enabled(hdd_context_t *hdd_ctx)
{
	mutex_lock(&hdd_ctx->iface_change_lock);
	if (hdd_ctx->driver_status != DRIVER_MODULES_ENABLED) {
		mutex_unlock(&hdd_ctx->iface_change_lock);
		hdd_notice("Modules not enabled, Present status: %d",
			   hdd_ctx->driver_status);
		return false;
	}
	mutex_unlock(&hdd_ctx->iface_change_lock);
	return true;
}

/**
 * hdd_set_ibss_power_save_params() - update IBSS Power Save params to WMA.
 * @hdd_adapter_t Hdd adapter.
 *
 * This function sets the IBSS power save config parameters to WMA
 * which will send it to firmware if FW supports IBSS power save
 * before vdev start.
 *
 * Return: QDF_STATUS QDF_STATUS_SUCCESS on Success and QDF_STATUS_E_FAILURE
 *         on failure.
 */
QDF_STATUS hdd_set_ibss_power_save_params(hdd_adapter_t *adapter)
{
	int ret;
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(adapter);

	if (hdd_ctx == NULL) {
		hdd_err("HDD context is null");
		return QDF_STATUS_E_FAILURE;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMA_VDEV_IBSS_SET_ATIM_WINDOW_SIZE,
				  hdd_ctx->config->ibssATIMWinSize,
				  VDEV_CMD);
	if (0 != ret) {
		hdd_err("WMA_VDEV_IBSS_SET_ATIM_WINDOW_SIZE failed %d", ret);
		return QDF_STATUS_E_FAILURE;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMA_VDEV_IBSS_SET_POWER_SAVE_ALLOWED,
				  hdd_ctx->config->isIbssPowerSaveAllowed,
				  VDEV_CMD);
	if (0 != ret) {
		hdd_err("WMA_VDEV_IBSS_SET_POWER_SAVE_ALLOWED failed %d",
			ret);
		return QDF_STATUS_E_FAILURE;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMA_VDEV_IBSS_SET_POWER_COLLAPSE_ALLOWED,
				  hdd_ctx->config->
				  isIbssPowerCollapseAllowed, VDEV_CMD);
	if (0 != ret) {
		hdd_err("WMA_VDEV_IBSS_SET_POWER_COLLAPSE_ALLOWED failed %d",
			ret);
		return QDF_STATUS_E_FAILURE;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMA_VDEV_IBSS_SET_AWAKE_ON_TX_RX,
				  hdd_ctx->config->isIbssAwakeOnTxRx,
				  VDEV_CMD);
	if (0 != ret) {
		hdd_err("WMA_VDEV_IBSS_SET_AWAKE_ON_TX_RX failed %d", ret);
		return QDF_STATUS_E_FAILURE;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMA_VDEV_IBSS_SET_INACTIVITY_TIME,
				  hdd_ctx->config->ibssInactivityCount,
				  VDEV_CMD);
	if (0 != ret) {
		hdd_err("WMA_VDEV_IBSS_SET_INACTIVITY_TIME failed %d", ret);
		return QDF_STATUS_E_FAILURE;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMA_VDEV_IBSS_SET_TXSP_END_INACTIVITY_TIME,
				  hdd_ctx->config->ibssTxSpEndInactivityTime,
				  VDEV_CMD);
	if (0 != ret) {
		hdd_err("WMA_VDEV_IBSS_SET_TXSP_END_INACTIVITY_TIME failed %d",
			ret);
		return QDF_STATUS_E_FAILURE;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMA_VDEV_IBSS_PS_SET_WARMUP_TIME_SECS,
				  hdd_ctx->config->ibssPsWarmupTime,
				  VDEV_CMD);
	if (0 != ret) {
		hdd_err("WMA_VDEV_IBSS_PS_SET_WARMUP_TIME_SECS failed %d",
			ret);
		return QDF_STATUS_E_FAILURE;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMA_VDEV_IBSS_PS_SET_1RX_CHAIN_IN_ATIM_WINDOW,
				  hdd_ctx->config->ibssPs1RxChainInAtimEnable,
				  VDEV_CMD);
	if (0 != ret) {
		hdd_err("WMA_VDEV_IBSS_PS_SET_1RX_CHAIN_IN_ATIM_WINDOW failed %d",
			ret);
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

#ifdef FEATURE_RUNTIME_PM
/**
 * hdd_runtime_suspend_context_init() - API to initialize HDD Runtime Contexts
 * @hdd_ctx: HDD context
 *
 * Return: None
 */
static void hdd_runtime_suspend_context_init(hdd_context_t *hdd_ctx)
{
	struct hdd_runtime_pm_context *ctx = &hdd_ctx->runtime_context;

	qdf_runtime_lock_init(&ctx->scan);
	qdf_runtime_lock_init(&ctx->roc);
	qdf_runtime_lock_init(&ctx->dfs);
	qdf_runtime_lock_init(&ctx->connect);
}

/**
 * hdd_runtime_suspend_context_deinit() - API to deinit HDD runtime context
 * @hdd_ctx: HDD Context
 *
 * Return: None
 */
static void hdd_runtime_suspend_context_deinit(hdd_context_t *hdd_ctx)
{
	struct hdd_runtime_pm_context *ctx = &hdd_ctx->runtime_context;

	qdf_runtime_lock_deinit(&ctx->scan);
	qdf_runtime_lock_deinit(&ctx->roc);
	qdf_runtime_lock_deinit(&ctx->dfs);
	qdf_runtime_lock_deinit(&ctx->connect);
}
#else /* FEATURE_RUNTIME_PM */
static void hdd_runtime_suspend_context_init(hdd_context_t *hdd_ctx) {}
static void hdd_runtime_suspend_context_deinit(hdd_context_t *hdd_ctx) {}
#endif /* FEATURE_RUNTIME_PM */

#define INTF_MACADDR_MASK       0x7

void hdd_update_macaddr(hdd_context_t *hdd_ctx,
			struct qdf_mac_addr hw_macaddr, bool generate_mac_auto)
{
	int8_t i;
	uint8_t macaddr_b3, tmp_br3;

	/*
	 * If "generate_mac_auto" is true, it indicates that all the
	 * addresses are derived addresses, else the first addresses
	 * is not derived address (It is provided by fw).
	 */
	if (!generate_mac_auto) {
		qdf_mem_copy(hdd_ctx->provisioned_mac_addr[0].bytes,
			     hw_macaddr.bytes, QDF_MAC_ADDR_SIZE);
		hdd_ctx->num_provisioned_addr++;
		hdd_info("hdd_ctx->provisioned_mac_addr[0]: "
			 MAC_ADDRESS_STR,
			 MAC_ADDR_ARRAY(hdd_ctx->
					provisioned_mac_addr[0].bytes));
	} else {
		qdf_mem_copy(hdd_ctx->derived_mac_addr[0].bytes,
			     hw_macaddr.bytes,
			     QDF_MAC_ADDR_SIZE);
		hdd_ctx->num_derived_addr++;
		hdd_info("hdd_ctx->derived_mac_addr[0]: "
			 MAC_ADDRESS_STR,
			 MAC_ADDR_ARRAY(hdd_ctx->derived_mac_addr[0].bytes));
	}

	for (i = hdd_ctx->num_derived_addr;
		i < QDF_MAX_CONCURRENCY_PERSONA - hdd_ctx->num_provisioned_addr;
		i++) {
		qdf_mem_copy(hdd_ctx->derived_mac_addr[i].bytes,
			     hw_macaddr.bytes,
			     QDF_MAC_ADDR_SIZE);
		macaddr_b3 = hdd_ctx->derived_mac_addr[i].bytes[3];
		tmp_br3 = ((macaddr_b3 >> 4 & INTF_MACADDR_MASK) + i) &
			  INTF_MACADDR_MASK;
		macaddr_b3 += tmp_br3;

		/* XOR-ing bit-24 of the mac address. This will give enough
		 * mac address range before collision
		 */
		macaddr_b3 ^= (1 << 7);

		/* Set locally administered bit */
		hdd_ctx->derived_mac_addr[i].bytes[0] |= 0x02;
		hdd_ctx->derived_mac_addr[i].bytes[3] = macaddr_b3;
		hdd_err("hdd_ctx->derived_mac_addr[%d]: "
			MAC_ADDRESS_STR, i,
			MAC_ADDR_ARRAY(hdd_ctx->derived_mac_addr[i].bytes));
		hdd_ctx->num_derived_addr++;
	}
}

static void hdd_update_tgt_services(hdd_context_t *hdd_ctx,
				    struct wma_tgt_services *cfg)
{
	struct hdd_config *config = hdd_ctx->config;

	/* Set up UAPSD */
	config->apUapsdEnabled &= cfg->uapsd;

	/* 11AC mode support */
	if ((config->dot11Mode == eHDD_DOT11_MODE_11ac ||
	     config->dot11Mode == eHDD_DOT11_MODE_11ac_ONLY) && !cfg->en_11ac)
		config->dot11Mode = eHDD_DOT11_MODE_AUTO;

	/* ARP offload: override user setting if invalid  */
	config->fhostArpOffload &= cfg->arp_offload;

#ifdef FEATURE_WLAN_SCAN_PNO
	/* PNO offload */
	hdd_debug("PNO Capability in f/w = %d", cfg->pno_offload);
	if (cfg->pno_offload)
		config->PnoOffload = true;
#endif
#ifdef FEATURE_WLAN_TDLS
	config->fEnableTDLSSupport &= cfg->en_tdls;
	config->fEnableTDLSOffChannel = config->fEnableTDLSOffChannel &&
						cfg->en_tdls_offchan;
	config->fEnableTDLSBufferSta = config->fEnableTDLSBufferSta &&
						cfg->en_tdls_uapsd_buf_sta;
	if (config->fTDLSUapsdMask && cfg->en_tdls_uapsd_sleep_sta)
		config->fEnableTDLSSleepSta = true;
	else
		config->fEnableTDLSSleepSta = false;
#endif
#ifdef WLAN_FEATURE_ROAM_OFFLOAD
	config->isRoamOffloadEnabled &= cfg->en_roam_offload;
#endif
	config->sap_get_peer_info &= cfg->get_peer_info_enabled;
	config->MAWCEnabled &= cfg->is_fw_mawc_capable;
	sme_update_tgt_services(hdd_ctx->hHal, cfg);

}

/**
 * hdd_update_vdev_nss() - sets the vdev nss
 * @hdd_ctx: HDD context
 *
 * Sets the Nss per vdev type based on INI
 *
 * Return: None
 */
static void hdd_update_vdev_nss(hdd_context_t *hdd_ctx)
{
	struct hdd_config *cfg_ini = hdd_ctx->config;
	uint8_t max_supp_nss = 1;

	if (cfg_ini->enable2x2 && !cds_is_sub_20_mhz_enabled())
		max_supp_nss = 2;

	sme_update_vdev_type_nss(hdd_ctx->hHal, max_supp_nss,
			cfg_ini->vdev_type_nss_2g, SIR_BAND_2_4_GHZ);

	sme_update_vdev_type_nss(hdd_ctx->hHal, max_supp_nss,
			cfg_ini->vdev_type_nss_5g, SIR_BAND_5_GHZ);
}

/**
 * hdd_update_wiphy_vhtcap() - Updates wiphy vhtcap fields
 * @hdd_ctx: HDD context
 *
 * Updates wiphy vhtcap fields
 *
 * Return: None
 */
static void hdd_update_wiphy_vhtcap(hdd_context_t *hdd_ctx)
{
	struct ieee80211_supported_band *band_5g =
		hdd_ctx->wiphy->bands[NL80211_BAND_5GHZ];
	uint32_t val;
	uint32_t value1;

	if (!band_5g) {
		hdd_debug("5GHz band disabled, skipping capability population");
		return;
	}

	val = hdd_ctx->config->txBFCsnValue;
	band_5g->vht_cap.cap |= (val << IEEE80211_VHT_CAP_BEAMFORMEE_STS_SHIFT);

	val = NUM_OF_SOUNDING_DIMENSIONS;
	band_5g->vht_cap.cap |=
		(val << IEEE80211_VHT_CAP_SOUNDING_DIMENSIONS_SHIFT);

	hdd_debug("Updated wiphy vhtcap:0x%x, CSNAntSupp:%d, NumSoundDim:%d",
		  band_5g->vht_cap.cap, hdd_ctx->config->txBFCsnValue, val);

	sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_RX_MCS_MAP, &value1);
	band_5g->vht_cap.vht_mcs.rx_mcs_map = value1;

	sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_TX_MCS_MAP, &value1);
	band_5g->vht_cap.vht_mcs.tx_mcs_map = value1;

}

/**
 * hdd_update_hw_dbs_capable() - sets the dbs capability of the device
 * @hdd_ctx: HDD context
 *
 * Sets the DBS capability as per INI and firmware capability
 *
 * Return: None
 */
static void hdd_update_hw_dbs_capable(hdd_context_t *hdd_ctx)
{
	struct hdd_config *cfg_ini = hdd_ctx->config;
	uint8_t hw_dbs_capable = 0;

	if (wma_is_hw_dbs_capable() &&
			((cfg_ini->dual_mac_feature_disable ==
			 ENABLE_DBS_CXN_AND_SCAN) ||
			(cfg_ini->dual_mac_feature_disable ==
			 ENABLE_DBS_CXN_AND_ENABLE_SCAN_WITH_ASYNC_SCAN_OFF) ||
			(cfg_ini->dual_mac_feature_disable ==
			 ENABLE_DBS_CXN_AND_DISABLE_SIMULTANEOUS_SCAN)))
		hw_dbs_capable = 1;

	sme_update_hw_dbs_capable(hdd_ctx->hHal, hw_dbs_capable);
}

static void hdd_update_tgt_ht_cap(hdd_context_t *hdd_ctx,
				  struct wma_tgt_ht_cap *cfg)
{
	QDF_STATUS status;
	uint32_t value, val32;
	uint16_t val16;
	struct hdd_config *pconfig = hdd_ctx->config;
	tSirMacHTCapabilityInfo *phtCapInfo;
	uint8_t mcs_set[SIZE_OF_SUPPORTED_MCS_SET];
	uint8_t enable_tx_stbc;

	/* check and update RX STBC */
	if (pconfig->enableRxSTBC && !cfg->ht_rx_stbc)
		pconfig->enableRxSTBC = cfg->ht_rx_stbc;

	/* get the MPDU density */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_MPDU_DENSITY, &value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get MPDU DENSITY");
		value = 0;
	}

	/*
	 * MPDU density:
	 * override user's setting if value is larger
	 * than the one supported by target
	 */
	if (value > cfg->mpdu_density) {
		status = sme_cfg_set_int(hdd_ctx->hHal, WNI_CFG_MPDU_DENSITY,
					 cfg->mpdu_density);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set MPDU DENSITY to CCM");
	}

	/* get the HT capability info */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_HT_CAP_INFO, &val32);
	if (QDF_STATUS_SUCCESS != status) {
		hdd_err("could not get HT capability info");
		return;
	}
	val16 = (uint16_t) val32;
	phtCapInfo = (tSirMacHTCapabilityInfo *) &val16;

	/* Set the LDPC capability */
	phtCapInfo->advCodingCap = cfg->ht_rx_ldpc;

	if (pconfig->ShortGI20MhzEnable && !cfg->ht_sgi_20)
		pconfig->ShortGI20MhzEnable = cfg->ht_sgi_20;

	if (pconfig->ShortGI40MhzEnable && !cfg->ht_sgi_40)
		pconfig->ShortGI40MhzEnable = cfg->ht_sgi_40;

	hdd_ctx->num_rf_chains = cfg->num_rf_chains;
	hdd_ctx->ht_tx_stbc_supported = cfg->ht_tx_stbc;

	enable_tx_stbc = pconfig->enableTxSTBC;

	if (pconfig->enable2x2 && (cfg->num_rf_chains == 2)) {
		pconfig->enable2x2 = 1;
	} else {
		pconfig->enable2x2 = 0;
		enable_tx_stbc = 0;
	}

	if (!(cfg->ht_tx_stbc && pconfig->enable2x2))
		enable_tx_stbc = 0;
	phtCapInfo->txSTBC = enable_tx_stbc;

	val32 = val16;
	status = sme_cfg_set_int(hdd_ctx->hHal, WNI_CFG_HT_CAP_INFO, val32);
	if (status != QDF_STATUS_SUCCESS)
		hdd_err("could not set HT capability to CCM");
#define WLAN_HDD_RX_MCS_ALL_NSTREAM_RATES 0xff
	value = SIZE_OF_SUPPORTED_MCS_SET;
	if (sme_cfg_get_str(hdd_ctx->hHal, WNI_CFG_SUPPORTED_MCS_SET, mcs_set,
			    &value) == QDF_STATUS_SUCCESS) {
		hdd_debug("Read MCS rate set");
		if (cfg->num_rf_chains > SIZE_OF_SUPPORTED_MCS_SET)
			cfg->num_rf_chains = SIZE_OF_SUPPORTED_MCS_SET;
		if (pconfig->enable2x2) {
			for (value = 0; value < cfg->num_rf_chains; value++)
				mcs_set[value] =
					WLAN_HDD_RX_MCS_ALL_NSTREAM_RATES;

			status =
				sme_cfg_set_str(hdd_ctx->hHal,
						WNI_CFG_SUPPORTED_MCS_SET,
						mcs_set,
						SIZE_OF_SUPPORTED_MCS_SET);
			if (status == QDF_STATUS_E_FAILURE)
				hdd_err("could not set MCS SET to CCM");
		}
	}
#undef WLAN_HDD_RX_MCS_ALL_NSTREAM_RATES
}

static void hdd_update_tgt_vht_cap(hdd_context_t *hdd_ctx,
				   struct wma_tgt_vht_cap *cfg)
{
	QDF_STATUS status;
	uint32_t value = 0;
	struct hdd_config *pconfig = hdd_ctx->config;
	struct wiphy *wiphy = hdd_ctx->wiphy;
	struct ieee80211_supported_band *band_5g =
		wiphy->bands[HDD_NL80211_BAND_5GHZ];
	uint32_t temp = 0;
	uint32_t ch_width = eHT_CHANNEL_WIDTH_80MHZ;
	uint32_t hw_rx_ldpc_enabled;
	struct wma_caps_per_phy caps_per_phy;
	uint32_t tx_highest_data_rate;
	uint32_t rx_highest_data_rate;

	if (!band_5g) {
		hdd_debug("5GHz band disabled, skipping capability population");
		return;
	}

	if (pconfig->enable2x2) {
		if (cfg->vht_short_gi_80 & WMI_VHT_CAP_SGI_80MHZ) {
			/* Update 2x2 Highest Short GI data rate */
			tx_highest_data_rate =
				VHT_TX_HIGHEST_SUPPORTED_DATA_RATE_2_2_SGI80;
			rx_highest_data_rate =
				VHT_RX_HIGHEST_SUPPORTED_DATA_RATE_2_2_SGI80;
		} else {
			/* Update 2x2 Rx Highest Long GI data Rate */
			tx_highest_data_rate =
					VHT_TX_HIGHEST_SUPPORTED_DATA_RATE_2_2;
			rx_highest_data_rate =
					VHT_RX_HIGHEST_SUPPORTED_DATA_RATE_2_2;
		}
	} else if (cfg->vht_short_gi_80 & WMI_VHT_CAP_SGI_80MHZ) {
		/* Update 1x1 Highest Short GI data rate */
		tx_highest_data_rate =
				VHT_TX_HIGHEST_SUPPORTED_DATA_RATE_1_1_SGI80;
		rx_highest_data_rate =
				VHT_RX_HIGHEST_SUPPORTED_DATA_RATE_1_1_SGI80;
	} else {
		 /* Update 1x1 Highest Long GI data rate */
		tx_highest_data_rate = VHT_TX_HIGHEST_SUPPORTED_DATA_RATE_1_1;
		rx_highest_data_rate = VHT_RX_HIGHEST_SUPPORTED_DATA_RATE_1_1;
	}

	status = sme_cfg_set_int(hdd_ctx->hHal,
				 WNI_CFG_VHT_RX_HIGHEST_SUPPORTED_DATA_RATE,
				 rx_highest_data_rate);

	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_err("Failed to set rx_supp_data_rate");

	status = sme_cfg_set_int(hdd_ctx->hHal,
				 WNI_CFG_VHT_TX_HIGHEST_SUPPORTED_DATA_RATE,
				 tx_highest_data_rate);

	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_err("Failed to set tx_supp_data_rate");

	/* Get the current MPDU length */
	status =
		sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_MAX_MPDU_LENGTH,
				&value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get MPDU LENGTH");
		value = 0;
	}

	/*
	 * VHT max MPDU length:
	 * override if user configured value is too high
	 * that the target cannot support
	 */
	if (value > cfg->vht_max_mpdu) {
		status = sme_cfg_set_int(hdd_ctx->hHal,
					 WNI_CFG_VHT_MAX_MPDU_LENGTH,
					 cfg->vht_max_mpdu);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set VHT MAX MPDU LENGTH");
	}

	sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_BASIC_MCS_SET, &temp);
	temp = (temp & VHT_MCS_1x1) | pconfig->vhtRxMCS;

	if (pconfig->enable2x2)
		temp = (temp & VHT_MCS_2x2) | (pconfig->vhtRxMCS2x2 << 2);

	if (sme_cfg_set_int(hdd_ctx->hHal, WNI_CFG_VHT_BASIC_MCS_SET, temp) ==
				QDF_STATUS_E_FAILURE) {
		hdd_err("Could not pass VHT_BASIC_MCS_SET to CCM");
	}

	sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_RX_MCS_MAP, &temp);
	temp = (temp & VHT_MCS_1x1) | pconfig->vhtRxMCS;
	if (pconfig->enable2x2)
		temp = (temp & VHT_MCS_2x2) | (pconfig->vhtRxMCS2x2 << 2);

	if (sme_cfg_set_int(hdd_ctx->hHal, WNI_CFG_VHT_RX_MCS_MAP, temp) ==
			QDF_STATUS_E_FAILURE) {
		hdd_err("Could not pass WNI_CFG_VHT_RX_MCS_MAP to CCM");
	}

	sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_TX_MCS_MAP, &temp);
	temp = (temp & VHT_MCS_1x1) | pconfig->vhtTxMCS;
	if (pconfig->enable2x2)
		temp = (temp & VHT_MCS_2x2) | (pconfig->vhtTxMCS2x2 << 2);

	hdd_debug("vhtRxMCS2x2 - %x temp - %u enable2x2 %d",
			pconfig->vhtRxMCS2x2, temp, pconfig->enable2x2);

	if (sme_cfg_set_int(hdd_ctx->hHal, WNI_CFG_VHT_TX_MCS_MAP, temp) ==
			QDF_STATUS_E_FAILURE) {
		hdd_err("Could not pass WNI_CFG_VHT_TX_MCS_MAP to CCM");
	}
	/* Get the current RX LDPC setting */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_LDPC_CODING_CAP,
				&value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get VHT LDPC CODING CAP");
		value = 0;
	}

	/* Set HW RX LDPC capability */
	hw_rx_ldpc_enabled = !!cfg->vht_rx_ldpc;
	if (hw_rx_ldpc_enabled != value) {
		status = sme_cfg_set_int(hdd_ctx->hHal,
					 WNI_CFG_VHT_LDPC_CODING_CAP,
					 hw_rx_ldpc_enabled);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set VHT LDPC CODING CAP to CCM");
	}

	/* Get current GI 80 value */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_SHORT_GI_80MHZ,
				&value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get SHORT GI 80MHZ");
		value = 0;
	}

	/* set the Guard interval 80MHz */
	if (value && !cfg->vht_short_gi_80) {
		status = sme_cfg_set_int(hdd_ctx->hHal,
					 WNI_CFG_VHT_SHORT_GI_80MHZ,
					 cfg->vht_short_gi_80);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set SHORT GI 80MHZ to CCM");
	}

	/* Get VHT TX STBC cap */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_TXSTBC, &value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get VHT TX STBC");
		value = 0;
	}

	/* VHT TX STBC cap */
	if (value && !cfg->vht_tx_stbc) {
		status = sme_cfg_set_int(hdd_ctx->hHal, WNI_CFG_VHT_TXSTBC,
					 cfg->vht_tx_stbc);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set the VHT TX STBC to CCM");
	}

	/* Get VHT RX STBC cap */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_RXSTBC, &value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get VHT RX STBC");
		value = 0;
	}

	/* VHT RX STBC cap */
	if (value && !cfg->vht_rx_stbc) {
		status = sme_cfg_set_int(hdd_ctx->hHal, WNI_CFG_VHT_RXSTBC,
					 cfg->vht_rx_stbc);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set the VHT RX STBC to CCM");
	}

	/* Get VHT SU Beamformer cap */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_SU_BEAMFORMER_CAP,
				 &value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get VHT SU BEAMFORMER CAP");
		value = 0;
	}

	/* set VHT SU Beamformer cap */
	if (value && !cfg->vht_su_bformer) {
		status = sme_cfg_set_int(hdd_ctx->hHal,
					 WNI_CFG_VHT_SU_BEAMFORMER_CAP,
					 cfg->vht_su_bformer);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set VHT SU BEAMFORMER CAP");
	}

	/* check and update SU BEAMFORMEE capabality */
	if (pconfig->enableTxBF && !cfg->vht_su_bformee)
		pconfig->enableTxBF = cfg->vht_su_bformee;

	status = sme_cfg_set_int(hdd_ctx->hHal,
				 WNI_CFG_VHT_SU_BEAMFORMEE_CAP,
				 pconfig->enableTxBF);

	if (status == QDF_STATUS_E_FAILURE)
		hdd_err("could not set VHT SU BEAMFORMEE CAP");

	/* Get VHT MU Beamformer cap */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_MU_BEAMFORMER_CAP,
				 &value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get VHT MU BEAMFORMER CAP");
		value = 0;
	}

	/* set VHT MU Beamformer cap */
	if (value && !cfg->vht_mu_bformer) {
		status = sme_cfg_set_int(hdd_ctx->hHal,
					 WNI_CFG_VHT_MU_BEAMFORMER_CAP,
					 cfg->vht_mu_bformer);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set the VHT MU BEAMFORMER CAP to CCM");
	}

	/* Get VHT MU Beamformee cap */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_MU_BEAMFORMEE_CAP,
				 &value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get VHT MU BEAMFORMEE CAP");
		value = 0;
	}

	/* set VHT MU Beamformee cap */
	if (value && !cfg->vht_mu_bformee) {
		status = sme_cfg_set_int(hdd_ctx->hHal,
					 WNI_CFG_VHT_MU_BEAMFORMEE_CAP,
					 cfg->vht_mu_bformee);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set VHT MU BEAMFORMER CAP");
	}

	/* Get VHT MAX AMPDU Len exp */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_AMPDU_LEN_EXPONENT,
				 &value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get VHT AMPDU LEN");
		value = 0;
	}

	/*
	 * VHT max AMPDU len exp:
	 * override if user configured value is too high
	 * that the target cannot support.
	 * Even though Rome publish ampdu_len=7, it can
	 * only support 4 because of some h/w bug.
	 */

	if (value > cfg->vht_max_ampdu_len_exp) {
		status = sme_cfg_set_int(hdd_ctx->hHal,
					 WNI_CFG_VHT_AMPDU_LEN_EXPONENT,
					 cfg->vht_max_ampdu_len_exp);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set the VHT AMPDU LEN EXP");
	}

	/* Get VHT TXOP PS CAP */
	status = sme_cfg_get_int(hdd_ctx->hHal, WNI_CFG_VHT_TXOP_PS, &value);

	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get VHT TXOP PS");
		value = 0;
	}

	/* set VHT TXOP PS cap */
	if (value && !cfg->vht_txop_ps) {
		status = sme_cfg_set_int(hdd_ctx->hHal, WNI_CFG_VHT_TXOP_PS,
					 cfg->vht_txop_ps);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set the VHT TXOP PS");
	}

	if (WMI_VHT_CAP_MAX_MPDU_LEN_11454 == cfg->vht_max_mpdu)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_11454;
	else if (WMI_VHT_CAP_MAX_MPDU_LEN_7935 == cfg->vht_max_mpdu)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_7991;
	else
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_MAX_MPDU_LENGTH_3895;


	if (cfg->supp_chan_width & (1 << eHT_CHANNEL_WIDTH_80P80MHZ)) {
		status = sme_cfg_set_int(hdd_ctx->hHal,
				WNI_CFG_VHT_SUPPORTED_CHAN_WIDTH_SET,
				VHT_CAP_160_AND_80P80_SUPP);
		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set the VHT CAP 160");
		band_5g->vht_cap.cap |=
			IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160_80PLUS80MHZ;
		ch_width = eHT_CHANNEL_WIDTH_80P80MHZ;
	} else if (cfg->supp_chan_width & (1 << eHT_CHANNEL_WIDTH_160MHZ)) {
		status = sme_cfg_set_int(hdd_ctx->hHal,
				WNI_CFG_VHT_SUPPORTED_CHAN_WIDTH_SET,
				VHT_CAP_160_SUPP);
		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set the VHT CAP 160");
		band_5g->vht_cap.cap |=
			IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_160MHZ;
		ch_width = eHT_CHANNEL_WIDTH_160MHZ;
	} else {
		status = sme_cfg_set_int(hdd_ctx->hHal,
				WNI_CFG_VHT_SUPPORTED_CHAN_WIDTH_SET,
				VHT_CAP_80_SUPP);
		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("could not set the VHT CH BW");
	}
	pconfig->vhtChannelWidth = QDF_MIN(pconfig->vhtChannelWidth,
			ch_width);
	/* Get the current GI 160 value */
	status = sme_cfg_get_int(hdd_ctx->hHal,
				WNI_CFG_VHT_SHORT_GI_160_AND_80_PLUS_80MHZ,
				&value);
	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("could not get GI 80 & 160");
		value = 0;
	}
	pconfig->ShortGI160MhzEnable = cfg->vht_short_gi_160;
	/* set the Guard interval 160MHz */
	if (value && !cfg->vht_short_gi_160) {
		status = sme_cfg_set_int(hdd_ctx->hHal,
			WNI_CFG_VHT_SHORT_GI_160_AND_80_PLUS_80MHZ,
			cfg->vht_short_gi_160);

		if (status == QDF_STATUS_E_FAILURE)
			hdd_err("failed to set SHORT GI 160MHZ");
	}

	if (cfg->vht_short_gi_160 & WMI_VHT_CAP_SGI_160MHZ)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_SHORT_GI_160;

	if (cfg->vht_rx_ldpc & WMI_VHT_CAP_RX_LDPC) {
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_RXLDPC;
		hdd_debug("VHT RxLDPC capability is set");
	} else {
		/*
		 * Get the RX LDPC capability for the NON DBS
		 * hardware mode for 5G band
		 */
		status = wma_get_caps_for_phyidx_hwmode(&caps_per_phy,
					HW_MODE_DBS_NONE, CDS_BAND_5GHZ);
		if ((QDF_IS_STATUS_SUCCESS(status)) &&
			(caps_per_phy.vht_5g & WMI_VHT_CAP_RX_LDPC)) {
			band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_RXLDPC;
			hdd_debug("VHT RX LDPC capability is set");
		}
	}

	pconfig->ShortGI80MhzEnable = cfg->vht_short_gi_80;
	if (cfg->vht_short_gi_80 & WMI_VHT_CAP_SGI_80MHZ)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_SHORT_GI_80;

	if (cfg->vht_tx_stbc & WMI_VHT_CAP_TX_STBC)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_TXSTBC;

	if (cfg->vht_rx_stbc & WMI_VHT_CAP_RX_STBC_1SS)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_RXSTBC_1;
	if (cfg->vht_rx_stbc & WMI_VHT_CAP_RX_STBC_2SS)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_RXSTBC_2;
	if (cfg->vht_rx_stbc & WMI_VHT_CAP_RX_STBC_3SS)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_RXSTBC_3;

	band_5g->vht_cap.cap |=
		(cfg->vht_max_ampdu_len_exp <<
		 IEEE80211_VHT_CAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT);

	if (cfg->vht_su_bformer & WMI_VHT_CAP_SU_BFORMER)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE;
	if (cfg->vht_su_bformee & WMI_VHT_CAP_SU_BFORMEE)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE;
	if (cfg->vht_mu_bformer & WMI_VHT_CAP_MU_BFORMER)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_MU_BEAMFORMER_CAPABLE;
	if (cfg->vht_mu_bformee & WMI_VHT_CAP_MU_BFORMEE)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE;

	if (cfg->vht_txop_ps & WMI_VHT_CAP_TXOP_PS)
		band_5g->vht_cap.cap |= IEEE80211_VHT_CAP_VHT_TXOP_PS;

	band_5g->vht_cap.vht_mcs.rx_highest = cpu_to_le16(rx_highest_data_rate);
	band_5g->vht_cap.vht_mcs.tx_highest = cpu_to_le16(tx_highest_data_rate);
}

/**
 * hdd_generate_macaddr_auto() - Auto-generate mac address
 * @hdd_ctx: Pointer to the HDD context
 *
 * Auto-generate mac address using device serial number.
 * Keep the first 3 bytes of OUI as before and replace
 * the last 3 bytes with the lower 3 bytes of serial number.
 *
 * Return: 0 for success
 *         Non zero failure code for errors
 */
static int hdd_generate_macaddr_auto(hdd_context_t *hdd_ctx)
{
	unsigned int serialno = 0;
	struct qdf_mac_addr mac_addr = {
		{0x00, 0x0A, 0xF5, 0x00, 0x00, 0x00}
	};

	serialno = pld_socinfo_get_serial_number(hdd_ctx->parent_dev);
	if (serialno == 0)
		return -EINVAL;

	serialno &= 0x00ffffff;

	mac_addr.bytes[3] = (serialno >> 16) & 0xff;
	mac_addr.bytes[4] = (serialno >> 8) & 0xff;
	mac_addr.bytes[5] = serialno & 0xff;

	hdd_update_macaddr(hdd_ctx, mac_addr, true);
	return 0;
}

/**
 * hdd_update_ra_rate_limit() - Update RA rate limit from target
 *  configuration to cfg_ini in HDD
 * @hdd_ctx: Pointer to hdd_ctx
 * @cfg: target configuration
 *
 * Return: None
 */
#ifdef FEATURE_WLAN_RA_FILTERING
static void hdd_update_ra_rate_limit(hdd_context_t *hdd_ctx,
				     struct wma_tgt_cfg *cfg)
{
	hdd_ctx->config->IsRArateLimitEnabled = cfg->is_ra_rate_limit_enabled;
}
#else
static void hdd_update_ra_rate_limit(hdd_context_t *hdd_ctx,
				     struct wma_tgt_cfg *cfg)
{
}
#endif

static void hdd_sar_target_config(hdd_context_t *hdd_ctx,
				  struct wma_tgt_cfg *cfg)
{
	hdd_ctx->sar_version = cfg->sar_version;
}

void hdd_update_tgt_cfg(void *context, void *param)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *) context;
	struct wma_tgt_cfg *cfg = param;
	uint8_t temp_band_cap;
	struct cds_config_info *cds_cfg = cds_get_ini_config();
	uint8_t antenna_mode;

	if (!hdd_ctx) {
		hdd_err("hdd context is NULL");
		return;
	}

	if (cds_cfg) {
		if (hdd_ctx->config->enable_sub_20_channel_width !=
			WLAN_SUB_20_CH_WIDTH_NONE && !cfg->sub_20_support) {
			hdd_warn("User requested sub 20 MHz channel width but unsupported by FW.");
			cds_cfg->sub_20_channel_width =
				WLAN_SUB_20_CH_WIDTH_NONE;
		} else {
			cds_cfg->sub_20_channel_width =
				hdd_ctx->config->enable_sub_20_channel_width;
		}
	}

	/* first store the INI band capability */
	temp_band_cap = hdd_ctx->config->nBandCapability;

	hdd_ctx->config->nBandCapability = cfg->band_cap;
	hdd_ctx->config->is_fils_roaming_supported =
			cfg->services.is_fils_roaming_supported;

	hdd_ctx->config->is_11k_offload_supported =
			cfg->services.is_11k_offload_supported;

	/* now overwrite the target band capability with INI
	 * setting if INI setting is a subset
	 */

	if ((hdd_ctx->config->nBandCapability == SIR_BAND_ALL) &&
	    (temp_band_cap != SIR_BAND_ALL))
		hdd_ctx->config->nBandCapability = temp_band_cap;
	else if ((hdd_ctx->config->nBandCapability != SIR_BAND_ALL) &&
		 (temp_band_cap != SIR_BAND_ALL) &&
		 (hdd_ctx->config->nBandCapability != temp_band_cap)) {
		hdd_warn("ini BandCapability not supported by the target");
	}

	hdd_ctx->curr_band = hdd_ctx->config->nBandCapability;

	if (!cds_is_driver_recovering() || cds_is_driver_in_bad_state()) {
		hdd_ctx->reg.reg_domain = cfg->reg_domain;
		hdd_ctx->reg.eeprom_rd_ext = cfg->eeprom_rd_ext;
	}

	/* This can be extended to other configurations like ht, vht cap... */
	if (!qdf_is_macaddr_zero(&cfg->hw_macaddr))
		qdf_mem_copy(&hdd_ctx->hw_macaddr, &cfg->hw_macaddr,
			     QDF_MAC_ADDR_SIZE);
	else
		hdd_info("hw_mac is zero");

	hdd_ctx->target_fw_version = cfg->target_fw_version;
	hdd_ctx->target_fw_vers_ext = cfg->target_fw_vers_ext;

	hdd_ctx->hw_bd_id = cfg->hw_bd_id;
	qdf_mem_copy(&hdd_ctx->hw_bd_info, &cfg->hw_bd_info,
		     sizeof(cfg->hw_bd_info));
	hdd_ctx->max_intf_count = cfg->max_intf_count;

	hdd_sar_target_config(hdd_ctx, cfg);
	hdd_lpass_target_config(hdd_ctx, cfg);
	hdd_green_ap_target_config(hdd_ctx, cfg);

	hdd_ctx->ap_arpns_support = cfg->ap_arpns_support;
	hdd_update_tgt_services(hdd_ctx, &cfg->services);

	hdd_update_tgt_ht_cap(hdd_ctx, &cfg->ht_cap);

	hdd_update_tgt_vht_cap(hdd_ctx, &cfg->vht_cap);

	hdd_update_vdev_nss(hdd_ctx);

	hdd_update_hw_dbs_capable(hdd_ctx);

	hdd_ctx->config->fine_time_meas_cap &= cfg->fine_time_measurement_cap;
	hdd_ctx->fine_time_meas_cap_target = cfg->fine_time_measurement_cap;
	hdd_debug("fine_time_meas_cap: 0x%x",
		hdd_ctx->config->fine_time_meas_cap);

	antenna_mode = (hdd_ctx->config->enable2x2 == 0x01) ?
			HDD_ANTENNA_MODE_2X2 : HDD_ANTENNA_MODE_1X1;
	hdd_update_smps_antenna_mode(hdd_ctx, antenna_mode);
	hdd_debug("Init current antenna mode: %d",
		 hdd_ctx->current_antenna_mode);

	hdd_ctx->apf_supported = (cfg->apf_enabled &&
				hdd_ctx->config->apf_packet_filter_enable);
	hdd_ctx->rcpi_enabled = cfg->rcpi_enabled;
	hdd_update_ra_rate_limit(hdd_ctx, cfg);

	if ((hdd_ctx->config->txBFCsnValue >
		WNI_CFG_VHT_CSN_BEAMFORMEE_ANT_SUPPORTED_FW_DEF) &&
						!cfg->tx_bfee_8ss_enabled)
		hdd_ctx->config->txBFCsnValue =
			WNI_CFG_VHT_CSN_BEAMFORMEE_ANT_SUPPORTED_FW_DEF;

	if (sme_cfg_set_int(hdd_ctx->hHal,
			WNI_CFG_VHT_CSN_BEAMFORMEE_ANT_SUPPORTED,
			hdd_ctx->config->txBFCsnValue) == QDF_STATUS_E_FAILURE)
		hdd_err("fw update WNI_CFG_VHT_CSN_BEAMFORMEE_ANT_SUPPORTED to CFG fails");


	hdd_debug("Target APF %d Host APF %d 8ss fw support %d txBFCsnValue %d",
		cfg->apf_enabled, hdd_ctx->config->apf_packet_filter_enable,
		cfg->tx_bfee_8ss_enabled, hdd_ctx->config->txBFCsnValue);

	/*
	 * Update txBFCsnValue and NumSoundingDim values to vhtcap in wiphy
	 */
	hdd_update_wiphy_vhtcap(hdd_ctx);
	/*
	 * If APF is supported, maxWowFilters set to
	 * WMA_STA_WOW_DEFAULT_PTRN_MAX	because we need atleast
	 * WMA_STA_WOW_DEFAULT_PTRN_MAX free slots to configure the STA mode
	 * wow pattern.
	 */
	if (hdd_ctx->apf_supported)
		hdd_ctx->config->maxWoWFilters = WMA_STA_WOW_DEFAULT_PTRN_MAX;

	hdd_ctx->wmi_max_len = cfg->wmi_max_len;

	/* Configure NAN datapath features */
	hdd_nan_datapath_target_config(hdd_ctx, cfg);

	hdd_ctx->lte_coex_ant_share = cfg->services.lte_coex_ant_share;
}

/**
 * hdd_dfs_indicate_radar() - handle radar detection on current SAP channel
 * @context:	HDD context pointer
 * @param:	HDD radar indication pointer
 *
 * This function is invoked in atomic context when a radar
 * is found on the SAP current operating channel and Data Tx
 * from netif has to be stopped to honor the DFS regulations.
 * Actions: Stop the netif Tx queues,Indicate Radar present
 * in HDD context for future usage.
 *
 * Return: true to allow radar indication to host else false
 */
bool hdd_dfs_indicate_radar(void *context, void *param)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *) context;
	struct wma_dfs_radar_ind *hdd_radar_event =
		(struct wma_dfs_radar_ind *)param;
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	hdd_adapter_t *adapter;
	QDF_STATUS status;
	hdd_ap_ctx_t *ap_ctx;

	if (!hdd_ctx || !hdd_radar_event ||
		 hdd_ctx->config->disableDFSChSwitch)
		return true;

	if (true == hdd_radar_event->dfs_radar_status) {
		if (qdf_atomic_inc_return(&hdd_ctx->dfs_radar_found) > 1) {
			/*
			 * Application already triggered channel switch
			 * on current channel, so return here.
			 */
			return false;
		}

		status = hdd_get_front_adapter(hdd_ctx, &adapterNode);
		while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
			adapter = adapterNode->pAdapter;
			ap_ctx = WLAN_HDD_GET_AP_CTX_PTR(adapter);
			if ((QDF_SAP_MODE == adapter->device_mode ||
			     QDF_P2P_GO_MODE == adapter->device_mode) &&
			     (CHANNEL_STATE_DFS ==
			     cds_get_channel_state(ap_ctx->operatingChannel))) {
				WLAN_HDD_GET_AP_CTX_PTR(adapter)->
				dfs_cac_block_tx = true;
				hdd_debug("tx blocked for session:%d",
					 adapter->sessionId);
			}

			status = hdd_get_next_adapter(hdd_ctx,
						      adapterNode,
						      &pNext);
			adapterNode = pNext;
		}
	}

	return true;
}

/**
 * hdd_is_valid_mac_address() - validate MAC address
 * @pMacAddr:	Pointer to the input MAC address
 *
 * This function validates whether the given MAC address is valid or not
 * Expected MAC address is of the format XX:XX:XX:XX:XX:XX
 * where X is the hexa decimal digit character and separated by ':'
 * This algorithm works even if MAC address is not separated by ':'
 *
 * This code checks given input string mac contains exactly 12 hexadecimal
 * digits and a separator colon : appears in the input string only after
 * an even number of hex digits.
 *
 * Return: 1 for valid and 0 for invalid
 */
bool hdd_is_valid_mac_address(const uint8_t *pMacAddr)
{
	int xdigit = 0;
	int separator = 0;

	while (*pMacAddr) {
		if (isxdigit(*pMacAddr)) {
			xdigit++;
		} else if (':' == *pMacAddr) {
			if (0 == xdigit || ((xdigit / 2) - 1) != separator)
				break;

			++separator;
		} else {
			/* Invalid MAC found */
			return 0;
		}
		++pMacAddr;
	}
	return xdigit == 12 && (separator == 5 || separator == 0);
}

/**
 * hdd_mon_mode_ether_setup() - Update monitor mode struct net_device.
 * @dev: Handle to struct net_device to be updated.
 *
 * Return: None
 */
static void hdd_mon_mode_ether_setup(struct net_device *dev)
{
	dev->header_ops         = NULL;
	dev->type               = ARPHRD_IEEE80211_RADIOTAP;
	dev->hard_header_len    = ETH_HLEN;
	dev->mtu                = ETH_DATA_LEN;
	dev->addr_len           = ETH_ALEN;
	dev->tx_queue_len       = 1000; /* Ethernet wants good queues */
	dev->flags              = IFF_BROADCAST|IFF_MULTICAST;
	dev->priv_flags        |= IFF_TX_SKB_SHARING;

	memset(dev->broadcast, 0xFF, ETH_ALEN);
}

/**
 * __hdd__mon_open() - HDD Open function
 * @dev: Pointer to net_device structure
 *
 * This is called in response to ifconfig up
 *
 * Return: 0 for success; non-zero for failure
 */
static int __hdd_mon_open(struct net_device *dev)
{
	int ret;
	hdd_adapter_t *adapter = WLAN_HDD_GET_PRIV_PTR(dev);

	ENTER_DEV(dev);

	if (WLAN_HDD_ADAPTER_MAGIC != adapter->magic) {
		hdd_err("Invalid magic");
		return -EINVAL;
	}

	hdd_mon_mode_ether_setup(dev);

	if (cds_get_conparam() == QDF_GLOBAL_MONITOR_MODE)
		ret = hdd_set_mon_rx_cb(dev);
	else
		ret = hdd_set_mon_mode_cb(dev);

	set_bit(DEVICE_IFACE_OPENED, &adapter->event_flags);
	return ret;
}

/**
 * hdd_mon_open() - Wrapper function for __hdd_mon_open to protect it from SSR
 * @dev:	Pointer to net_device structure
 *
 * This is called in response to ifconfig up
 *
 * Return: 0 for success; non-zero for failure
 */
static int hdd_mon_open(struct net_device *dev)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_mon_open(dev);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * wlan_hdd_update_dbs_scan_and_fw_mode_config() - Send updated dual mac scan
 * configuration and fw mode configuration
 *
 * This is called from hdd_start_adapter after the device specific adapter is
 * started
 *
 * Return: 0 for success; non-zero for failure
 */
static QDF_STATUS
wlan_hdd_update_dbs_scan_and_fw_mode_config(void)
{
	struct sir_dual_mac_config cfg = {0};
	QDF_STATUS status;
	uint32_t chnl_sel_logic_conc = 0, dbs_disable;
	hdd_context_t *hdd_ctx;

	hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	if (!hdd_ctx) {
		hdd_err("HDD context is NULL");
		return QDF_STATUS_E_FAILURE;
	}

	/*
	 * ROME platform doesn't support any DBS related commands in FW,
	 * so if driver sends wmi command with dual_mac_config with all set to
	 * 0 then FW wouldn't respond back and driver would timeout on waiting
	 * for response. Check if FW supports DBS to eliminate ROME vs
	 * NON-ROME platform.
	 */
	if (!wma_find_if_fw_supports_dbs())
		return QDF_STATUS_SUCCESS;

	cfg.scan_config = 0;
	cfg.fw_mode_config = 0;
	cfg.set_dual_mac_cb = cds_soc_set_dual_mac_cfg_cb;

	if (wma_is_hw_dbs_capable())
		chnl_sel_logic_conc =
			hdd_ctx->config->channel_select_logic_conc;

	dbs_disable = hdd_ctx->config->dual_mac_feature_disable;
	if (hdd_ctx->config->dual_mac_feature_disable !=
	    DISABLE_DBS_CXN_AND_SCAN) {
		status =
		wma_get_updated_scan_and_fw_mode_config(&cfg.scan_config,
							&cfg.fw_mode_config,
							dbs_disable,
							chnl_sel_logic_conc);

		if (status != QDF_STATUS_SUCCESS) {
			hdd_err("can't get updated scan and fw cfg status:%d",
				status);
			return status;
		}
	}

	hdd_debug("send scan_cfg: 0x%x fw_mode_cfg: 0x%x to fw",
		  cfg.scan_config, cfg.fw_mode_config);

	status = sme_soc_set_dual_mac_config(hdd_ctx->hHal, cfg);
	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("sme_soc_set_dual_mac_config failed %d",
			status);
		return status;
	}

	return QDF_STATUS_SUCCESS;
}
/**
 * hdd_start_adapter() - Wrapper function for device specific adapter
 * @adapter: pointer to HDD adapter
 *
 * This function is called to start the device specific adapter for
 * the mode passed in the adapter's device_mode.
 *
 * Return: 0 for success; non-zero for failure
 */
int hdd_start_adapter(hdd_adapter_t *adapter)
{

	int ret;
	enum tQDF_ADAPTER_MODE device_mode = adapter->device_mode;

	hdd_info("enter(%s)", netdev_name(adapter->dev));
	hdd_debug("Start_adapter for mode : %d", adapter->device_mode);

	switch (device_mode) {
	case QDF_P2P_CLIENT_MODE:
	case QDF_P2P_DEVICE_MODE:
	case QDF_OCB_MODE:
	case QDF_STA_MODE:
	case QDF_MONITOR_MODE:
		ret = hdd_start_station_adapter(adapter);
		if (ret)
			goto err_start_adapter;
		break;
	case QDF_P2P_GO_MODE:
	case QDF_SAP_MODE:
		ret = hdd_start_ap_adapter(adapter);
		if (ret)
			goto err_start_adapter;
		break;
	case QDF_IBSS_MODE:
		/*
		 * For IBSS interface is initialized as part of
		 * hdd_init_station_mode()
		 */
		return 0;
	case QDF_FTM_MODE:
		ret = hdd_start_ftm_adapter(adapter);
		if (ret)
			goto err_start_adapter;
		goto exit;
	default:
		hdd_err("Invalid session type %d", device_mode);
		QDF_ASSERT(0);
		goto err_start_adapter;
	}
	if (hdd_set_fw_params(adapter))
		hdd_err("Failed to set the FW params for the adapter!");

	/*
	 * Action frame registered in one adapter which will
	 * applicable to all interfaces
	 */
	ret = wlan_hdd_cfg80211_register_frames(adapter);

	if (ret < 0) {
		hdd_err("Failed to register frames - ret %d", ret);
		goto err_start_adapter;
	}
	wlan_hdd_update_dbs_scan_and_fw_mode_config();
exit:
	EXIT();
	return 0;
err_start_adapter:
	return -EINVAL;
}

/**
 * hdd_enable_power_management() - API to Enable Power Management
 *
 * API invokes Bus Interface Layer power management functionality
 *
 * Return: None
 */
static void hdd_enable_power_management(void)
{
	void *hif_ctx = cds_get_context(QDF_MODULE_ID_HIF);

	if (!hif_ctx) {
		hdd_err("Bus Interface Context is Invalid");
		return;
	}

	hif_enable_power_management(hif_ctx, cds_is_packet_log_enabled());
}

/**
 * hdd_disable_power_management() - API to disable Power Management
 *
 * API disable Bus Interface Layer Power management functionality
 *
 * Return: None
 */
static void hdd_disable_power_management(void)
{
	void *hif_ctx = cds_get_context(QDF_MODULE_ID_HIF);

	if (!hif_ctx) {
		hdd_err("Bus Interface Context is Invalid");
		return;
	}

	hif_disable_power_management(hif_ctx);
}

void hdd_update_hw_sw_info(hdd_context_t *hdd_ctx)
{
	void *hif_sc;
	size_t target_hw_name_len;
	const char *target_hw_name;
	uint8_t *buf;
	uint32_t buf_len;


	hif_sc = cds_get_context(QDF_MODULE_ID_HIF);
	if (!hif_sc) {
		hdd_err("HIF context is NULL");
		return;
	}

	hif_get_hw_info(hif_sc, &hdd_ctx->target_hw_version,
			&hdd_ctx->target_hw_revision,
			&target_hw_name);

	target_hw_name_len = strlen(target_hw_name) + 1;

	if (hdd_ctx->target_hw_name)
		qdf_mem_free(hdd_ctx->target_hw_name);

	hdd_ctx->target_hw_name = qdf_mem_malloc(target_hw_name_len);

	if (hdd_ctx->target_hw_name)
		qdf_mem_copy(hdd_ctx->target_hw_name, target_hw_name,
			     target_hw_name_len);

	buf = qdf_mem_malloc(WE_MAX_STR_LEN);
	if (buf) {
		buf_len = hdd_wlan_get_version(hdd_ctx, WE_MAX_STR_LEN, buf);
		hdd_info("%s", buf);
		qdf_mem_free(buf);
	}
}

/**
 * hdd_check_for_leaks() - Perform runtime memory leak checks
 *
 * This API triggers runtime memory leak detection. This feature enforces the
 * policy that any memory allocated at runtime must also be released at runtime.
 *
 * Allocating memory at runtime and releasing it at unload is effectively a
 * memory leak for configurations which never unload (e.g. LONU, statically
 * compiled driver). Such memory leaks are NOT false positives, and must be
 * fixed.
 *
 * Return: None
 */
static void hdd_check_for_leaks(void)
{
	/* DO NOT REMOVE these checks; for false positives, read above first */

	qdf_mc_timer_check_for_leaks();
	qdf_nbuf_map_check_for_leaks();
	qdf_mem_check_for_leaks();
}

#ifdef FW_THERMAL_THROTTLE_SUPPORT
/**
 * hdd_configure_thermal_mitigation() - Register/unregister thermal mitigation
 * @dev: The device structure
 * @state: true for register; false for unregister
 *
 * This API triggers regiters/unregisters thermal mitigation feature with the
 * thermal subsystem.
 *
 * Return: None
 */
static void hdd_configure_thermal_mitigation(hdd_context_t *hdd_ctx,
					     struct device *dev, bool state)
{
	if (state) {
		if (!pld_thermal_register(dev, HDD_THERMAL_MAX_STATE))
			hdd_ctx->is_thermal_system_registered = true;
	} else {
		if (hdd_ctx->is_thermal_system_registered) {
			pld_thermal_unregister(dev);
			hdd_ctx->is_thermal_system_registered = false;
		}
	}
}
#else
static void hdd_configure_thermal_mitigation(hdd_context_t *hdd_ctx,
					     struct device *dev, bool state)
{
}
#endif

uint32_t hdd_wlan_get_version(hdd_context_t *hdd_ctx,
			      const size_t version_len, uint8_t *version)
{
	uint32_t size;
	uint32_t msp_id = 0, mspid = 0, siid = 0, crmid = 0, sub_id = 0;

	if (!hdd_ctx) {
		hdd_err("Invalid context, HDD context is null");
		return 0;
	}

	if (!version || version_len == 0) {
		hdd_err("Invalid buffer pointr or buffer len\n");
		return 0;
	}

	msp_id = (hdd_ctx->target_fw_version & 0xf0000000) >> 28;
	mspid = (hdd_ctx->target_fw_version & 0xf000000) >> 24;
	siid = (hdd_ctx->target_fw_version & 0xf00000) >> 20;
	crmid = hdd_ctx->target_fw_version & 0x7fff;
	sub_id = (hdd_ctx->target_fw_vers_ext & 0xf0000000) >> 28;

	size = scnprintf(version, version_len,
			 "Host SW:%s, FW:%d.%d.%d.%d.%d, HW:%s, Board ver: %x Ref design id: %x, Customer id: %x, Project id: %x, Board Data Rev: %x",
			 QWLAN_VERSIONSTR,
			 msp_id, mspid, siid, crmid, sub_id,
			 hdd_ctx->target_hw_name,
			 hdd_ctx->hw_bd_info.bdf_version,
			 hdd_ctx->hw_bd_info.ref_design_id,
			 hdd_ctx->hw_bd_info.customer_id,
			 hdd_ctx->hw_bd_info.project_id,
			 hdd_ctx->hw_bd_info.board_data_rev);

	return size;
}

/**
 * hdd_wlan_start_modules() - Single driver state machine for starting modules
 * @hdd_ctx: HDD context
 * @adapter: HDD adapter
 * @reinit: flag to indicate from SSR or normal path
 *
 * This function maintains the driver state machine it will be invoked from
 * startup, reinit and change interface. Depending on the driver state shall
 * perform the opening of the modules.
 *
 * Return: 0 for success; non-zero for failure
 */
int hdd_wlan_start_modules(hdd_context_t *hdd_ctx, hdd_adapter_t *adapter,
			   bool reinit)
{
	int ret = 0;
	qdf_device_t qdf_dev;
	QDF_STATUS status;
	p_cds_contextType p_cds_context;
	bool unint = false;
	void *hif_ctx;

	ENTER();

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		hdd_err("Global Context is NULL");
		QDF_ASSERT(0);
		return -EINVAL;
	}

	hdd_debug("start modules called in  state! :%d reinit: %d",
			 hdd_ctx->driver_status, reinit);

	qdf_dev = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	if (!qdf_dev) {
		hdd_err("QDF Device Context is Invalid return");
		return -EINVAL;
	}

	qdf_cancel_delayed_work(&hdd_ctx->iface_idle_work);

	mutex_lock(&hdd_ctx->iface_change_lock);
	if (hdd_ctx->driver_status == DRIVER_MODULES_ENABLED) {
		mutex_unlock(&hdd_ctx->iface_change_lock);
		hdd_info("Driver modules already Enabled");
		EXIT();
		return 0;
	}

	hdd_ctx->start_modules_in_progress = true;

	switch (hdd_ctx->driver_status) {
	case DRIVER_MODULES_UNINITIALIZED:
		hdd_info("Wlan transition (UNINITIALIZED -> CLOSED)");
		unint = true;
		/* Fall through dont add break here */
	case DRIVER_MODULES_CLOSED:
		hdd_info("Wlan transition (CLOSED -> OPENED)");

		qdf_mem_set_domain(QDF_MEM_DOMAIN_ACTIVE);

		if (!reinit && !unint) {
			ret = pld_power_on(qdf_dev->dev);
			if (ret) {
				hdd_err("Failed to Powerup the device; errno: %d",
					ret);
				goto release_lock;
			}
		}

		pld_set_fw_log_mode(hdd_ctx->parent_dev,
				    hdd_ctx->config->enable_fw_log);

		ret = hdd_hif_open(qdf_dev->dev, qdf_dev->drv_hdl, qdf_dev->bid,
				   qdf_dev->bus_type,
				   (reinit == true) ?  HIF_ENABLE_TYPE_REINIT :
				   HIF_ENABLE_TYPE_PROBE);
		if (ret) {
			hdd_err("Failed to open hif; errno: %d", ret);
			goto power_down;
		}

		hif_ctx = cds_get_context(QDF_MODULE_ID_HIF);
		if (!hif_ctx) {
			hdd_err("hif context is null!!");
			ret = -EINVAL;
			goto power_down;
		}

		status = ol_cds_init(qdf_dev, hif_ctx);
		if (status != QDF_STATUS_SUCCESS) {
			hdd_err("No Memory to Create BMI Context; status: %d",
				status);
			ret = qdf_status_to_os_return(status);
			goto hif_close;
		}

		ret = hdd_update_config(hdd_ctx);
		if (ret) {
			hdd_err("Failed to update configuration; errno: %d",
				ret);
			goto ol_cds_free;
		}

		status = cds_open();
		if (!QDF_IS_STATUS_SUCCESS(status)) {
			hdd_err("Failed to Open CDS; status: %d", status);
			ret = (status == QDF_STATUS_E_NOMEM) ? -ENOMEM : -EINVAL;
			goto deinit_config;
		}

		hdd_runtime_suspend_context_init(hdd_ctx);

		hdd_ctx->hHal = cds_get_context(QDF_MODULE_ID_SME);
		if (NULL == hdd_ctx->hHal) {
			hdd_err("HAL context is null");
			ret = -EINVAL;
			goto close;
		}

		ret = hdd_register_cb(hdd_ctx);
		if (ret) {
			hdd_err("Failed to register HDD callbacks!");
			goto close;
		}

		status = cds_pre_enable(hdd_ctx->pcds_context);
		if (!QDF_IS_STATUS_SUCCESS(status)) {
			hdd_err("Failed to pre-enable CDS; status: %d", status);
			ret = qdf_status_to_os_return(status);
			goto deregister_cb;
		}


		status = hdd_ipa_init(hdd_ctx);
		if (status) {
			ret = qdf_status_to_os_return(status);
			goto err_post_disable;
		}

		hdd_sysfs_create_version_interface();

		hdd_ctx->driver_status = DRIVER_MODULES_OPENED;
		hdd_info("Wlan transition (now OPENED)");

		hdd_update_hw_sw_info(hdd_ctx);

		if (QDF_GLOBAL_FTM_MODE == hdd_get_conparam()) {
			sme_register_ftm_msg_processor(hdd_ctx->hHal,
						       hdd_ftm_mc_process_msg);
			break;
		}
		if (unint) {
			hdd_debug("In phase-1 initialization  don't enable modules");
			break;
		}

		if (reinit) {
			ret = hdd_ipa_uc_ssr_reinit(hdd_ctx);
			if (ret) {
				hdd_err("HDD IPA UC reinit failed; errno: %d",
					ret);
				goto err_ipa_cleanup;
			}
		}

	/* Fall through dont add break here */
	case DRIVER_MODULES_OPENED:
		hdd_info("Wlan transition (OPENED -> ENABLED)");
		if (!adapter) {
			hdd_err("adapter is Null");
			ret = -EINVAL;
			goto err_ipa_cleanup;
		}

		if (QDF_GLOBAL_FTM_MODE == hdd_get_conparam()) {
			hdd_err("in ftm mode, no need to configure cds modules");
			ret = -EINVAL;
			break;
		}

		ret = hdd_configure_cds(hdd_ctx, adapter);
		if (ret) {
			hdd_err("Failed to Enable cds modules; errno: %d", ret);
			ret = -EINVAL;
			goto err_ipa_cleanup;
		}

		hdd_enable_power_management();
		hdd_configure_thermal_mitigation(hdd_ctx, qdf_dev->dev, 1);
		hdd_info("Driver Modules Successfully Enabled");
		hdd_ctx->driver_status = DRIVER_MODULES_ENABLED;

		break;
	default:
		hdd_err("WLAN start invoked in wrong state! :%d\n",
				hdd_ctx->driver_status);
		ret = -EINVAL;
		goto release_lock;
	}
	hdd_ctx->start_modules_in_progress = false;
	mutex_unlock(&hdd_ctx->iface_change_lock);
	EXIT();
	return 0;

err_ipa_cleanup:
	hdd_ipa_cleanup(hdd_ctx);
	sme_destroy_config(hdd_ctx->hHal);
err_post_disable:
	cds_post_disable();

deregister_cb:
	hdd_deregister_cb(hdd_ctx);

close:
	hdd_ctx->driver_status = DRIVER_MODULES_CLOSED;
	cds_close(p_cds_context);

deinit_config:
	cds_deinit_ini_config();

ol_cds_free:
	ol_cds_free();

hif_close:
	hdd_hif_close(p_cds_context->pHIFContext);
power_down:
	if (!reinit && !unint)
		pld_power_off(qdf_dev->dev);
release_lock:
	hdd_ctx->start_modules_in_progress = false;
	mutex_unlock(&hdd_ctx->iface_change_lock);
	if (hdd_ctx->target_hw_name) {
		qdf_mem_free(hdd_ctx->target_hw_name);
		hdd_ctx->target_hw_name = NULL;
	}
	/* many adapter resources are not freed by design in SSR case */
	if (!reinit)
		hdd_check_for_leaks();

	qdf_mem_set_domain(QDF_MEM_DOMAIN_INIT);

	EXIT();

	return ret;
}

/**
 * __hdd_open() - HDD Open function
 * @dev:	Pointer to net_device structure
 *
 * This is called in response to ifconfig up
 *
 * Return: 0 for success; non-zero for failure
 */
static int __hdd_open(struct net_device *dev)
{
	hdd_adapter_t *adapter = WLAN_HDD_GET_PRIV_PTR(dev);
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	int ret;

	ENTER_DEV(dev);
	MTRACE(qdf_trace(QDF_MODULE_ID_HDD, TRACE_CODE_HDD_OPEN_REQUEST,
		adapter->sessionId, adapter->device_mode));

	/* Nothing to be done if device is unloading */
	if (cds_is_driver_unloading()) {
		hdd_err("Driver is unloading can not open the hdd");
		return -EBUSY;
	}

	if (cds_is_driver_recovering()) {
		hdd_err("WLAN is currently recovering; Please try again.");
		return -EBUSY;
	}

	if (qdf_atomic_read(&hdd_ctx->con_mode_flag)) {
		hdd_err("con_mode_handler is in progress; Please try again.");
		return -EBUSY;
	}

	mutex_lock(&hdd_init_deinit_lock);
	hdd_start_driver_ops_timer(eHDD_DRV_OP_IFF_UP);

	/*
	  * This scenario can be hit in cases where in the wlan driver after
	  * registering the netdevices and there is a failure in driver
	  * initialization. So return error gracefully because the netdevices
	  * will be de-registered as part of the load failure.
	  */
	if (!cds_is_driver_loaded()) {
		hdd_err("Failed to start the wlan driver!!");
		ret = -EIO;
		goto err_hdd_hdd_init_deinit_lock;
	}

	ret = hdd_wlan_start_modules(hdd_ctx, adapter, false);
	if (ret) {
		hdd_err("Failed to start WLAN modules return");
		goto err_hdd_hdd_init_deinit_lock;
	}


	if (!test_bit(SME_SESSION_OPENED, &adapter->event_flags)) {
		ret = hdd_start_adapter(adapter);
		if (ret) {
			hdd_err("Failed to start adapter :%d",
				adapter->device_mode);
			goto err_hdd_hdd_init_deinit_lock;
		}
	}

	if (adapter->device_mode == QDF_FTM_MODE)
		goto err_hdd_hdd_init_deinit_lock;

	set_bit(DEVICE_IFACE_OPENED, &adapter->event_flags);
	hdd_info("%s interface up, event-flags: %lx ", dev->name,
		 adapter->event_flags);

	if (hdd_conn_is_connected(WLAN_HDD_GET_STATION_CTX_PTR(adapter))) {
		hdd_info("Enabling Tx Queues");
		/* Enable TX queues only when we are connected */
		wlan_hdd_netif_queue_control(adapter,
					     WLAN_START_ALL_NETIF_QUEUE,
					     WLAN_CONTROL_PATH);
	}

	/* Enable carrier and transmit queues for NDI */
	if (WLAN_HDD_IS_NDI(adapter)) {
		hdd_debug("Enabling Tx Queues");
		wlan_hdd_netif_queue_control(adapter,
			WLAN_START_ALL_NETIF_QUEUE_N_CARRIER,
			WLAN_CONTROL_PATH);
	}

	if (adapter->device_mode == QDF_STA_MODE) {
		hdd_debug("Sending Start Lpass notification");
		hdd_lpass_notify_start(hdd_ctx, adapter);
	}

err_hdd_hdd_init_deinit_lock:
	hdd_stop_driver_ops_timer();
	mutex_unlock(&hdd_init_deinit_lock);
	return ret;
}


/**
 * hdd_open() - Wrapper function for __hdd_open to protect it from SSR
 * @dev:	Pointer to net_device structure
 *
 * This is called in response to ifconfig up
 *
 * Return: 0 for success; non-zero for failure
 */
static int hdd_open(struct net_device *dev)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_open(dev);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * __hdd_stop() - HDD stop function
 * @dev:	Pointer to net_device structure
 *
 * This is called in response to ifconfig down
 *
 * Return: 0 for success; non-zero for failure
 */
static int __hdd_stop(struct net_device *dev)
{
	hdd_adapter_t *adapter = WLAN_HDD_GET_PRIV_PTR(dev);
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	int ret;

	ENTER_DEV(dev);

	MTRACE(qdf_trace(QDF_MODULE_ID_HDD, TRACE_CODE_HDD_STOP_REQUEST,
			 adapter->sessionId, adapter->device_mode));

	ret = wlan_hdd_validate_context(hdd_ctx);
	if (0 != ret) {
		set_bit(DOWN_DURING_SSR, &adapter->event_flags);
		return ret;
	}

	/* Nothing to be done if the interface is not opened */
	if (false == test_bit(DEVICE_IFACE_OPENED, &adapter->event_flags)) {
		hdd_err("NETDEV Interface is not OPENED");
		return -ENODEV;
	}

	/*
	 * Disable TX on the interface, after this hard_start_xmit() will not
	 * be called on that interface
	 */
	hdd_info("Disabling queues, adapter device mode: %s(%d), event-flags: %lx ",
		 hdd_device_mode_to_string(adapter->device_mode),
		 adapter->device_mode, adapter->event_flags);

	wlan_hdd_netif_queue_control(adapter,
				     WLAN_STOP_ALL_NETIF_QUEUE_N_CARRIER,
				     WLAN_CONTROL_PATH);

	if (!wlan_hdd_is_session_type_monitor(adapter->device_mode)) {
		hdd_debug("Disabling Auto Power save timer");
		sme_ps_disable_auto_ps_timer(
			WLAN_HDD_GET_HAL_CTX(adapter),
			adapter->sessionId);
	}

	if (adapter->device_mode == QDF_STA_MODE) {
		hdd_debug("Sending Lpass stop notifcation");
		hdd_lpass_notify_stop(hdd_ctx);
	}

	if (wlan_hdd_is_session_type_monitor(adapter->device_mode))
		hdd_reset_mon_mode_cb();

	/*
	 * NAN data interface is different in some sense. The traffic on NDI is
	 * bursty in nature and depends on the need to transfer. The service
	 * layer may down the interface after the usage and up again when
	 * required. In some sense, the NDI is expected to be available
	 * (like SAP) iface until NDI delete request is issued by the service
	 * layer. Skip BSS termination and adapter deletion for NAN Data
	 * interface (NDI).
	 */
	if (WLAN_HDD_IS_NDI(adapter))
		return 0;

	/*
	 * The interface is marked as down for outside world (aka kernel)
	 * But the driver is pretty much alive inside. The driver needs to
	 * tear down the existing connection on the netdev (session)
	 * cleanup the data pipes and wait until the control plane is stabilized
	 * for this interface. The call also needs to wait until the above
	 * mentioned actions are completed before returning to the caller.
	 * Notice that hdd_stop_adapter is requested not to close the session
	 * That is intentional to be able to scan if it is a STA/P2P interface
	 */
	hdd_stop_adapter(hdd_ctx, adapter, true);

	/* DeInit the adapter. This ensures datapath cleanup as well */
	hdd_deinit_adapter(hdd_ctx, adapter, true);

	/* Make sure the interface is marked as closed */
	clear_bit(DEVICE_IFACE_OPENED, &adapter->event_flags);

	/*
	 * Find if any iface is up. If any iface is up then can't put device to
	 * sleep/power save mode
	 */
	if (hdd_check_for_opened_interfaces(hdd_ctx)) {
		hdd_debug("Closing all modules from the hdd_stop");
		qdf_sched_delayed_work(&hdd_ctx->iface_idle_work,
				       hdd_ctx->config->iface_change_wait_time);
		hdd_prevent_suspend_timeout(
			hdd_ctx->config->iface_change_wait_time,
			WIFI_POWER_EVENT_WAKELOCK_IFACE_CHANGE_TIMER);
	}

	EXIT();
	return 0;
}

/**
 * hdd_stop() - Wrapper function for __hdd_stop to protect it from SSR
 * @dev: pointer to net_device structure
 *
 * This is called in response to ifconfig down
 *
 * Return: 0 for success and error number for failure
 */
static int hdd_stop(struct net_device *dev)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_stop(dev);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * __hdd_uninit() - HDD uninit function
 * @dev:	Pointer to net_device structure
 *
 * This is called during the netdev unregister to uninitialize all data
 * associated with the device
 *
 * Return: None
 */
static void __hdd_uninit(struct net_device *dev)
{
	hdd_adapter_t *adapter = WLAN_HDD_GET_PRIV_PTR(dev);

	ENTER_DEV(dev);

	do {
		if (WLAN_HDD_ADAPTER_MAGIC != adapter->magic) {
			hdd_err("Invalid magic");
			break;
		}

		if (NULL == adapter->pHddCtx) {
			hdd_err("NULL hdd_ctx");
			break;
		}

		if (dev != adapter->dev)
			hdd_err("Invalid device reference");

		hdd_deinit_adapter(adapter->pHddCtx, adapter, true);

		/* after uninit our adapter structure will no longer be valid */
		adapter->dev = NULL;
		adapter->magic = 0;
	} while (0);

	EXIT();
}

/**
 * hdd_uninit() - Wrapper function to protect __hdd_uninit from SSR
 * @dev: pointer to net_device structure
 *
 * This is called during the netdev unregister to uninitialize all data
 * associated with the device
 *
 * Return: none
 */
static void hdd_uninit(struct net_device *dev)
{
	cds_ssr_protect(__func__);
	__hdd_uninit(dev);
	cds_ssr_unprotect(__func__);
}

static int hdd_open_cesium_nl_sock(void)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0))
	struct netlink_kernel_cfg cfg = {
		.groups = WLAN_NLINK_MCAST_GRP_ID,
		.input = NULL
	};
#endif
	int ret = 0;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 6, 0))
	cesium_nl_srv_sock = netlink_kernel_create(&init_net, WLAN_NLINK_CESIUM,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 7, 0))
						   THIS_MODULE,
#endif
						   &cfg);
#else
	cesium_nl_srv_sock = netlink_kernel_create(&init_net, WLAN_NLINK_CESIUM,
						   WLAN_NLINK_MCAST_GRP_ID,
						   NULL, NULL, THIS_MODULE);
#endif

	if (cesium_nl_srv_sock == NULL) {
		hdd_err("NLINK:  cesium netlink_kernel_create failed");
		ret = -ECONNREFUSED;
	}

	return ret;
}

static void hdd_close_cesium_nl_sock(void)
{
	if (NULL != cesium_nl_srv_sock) {
		netlink_kernel_release(cesium_nl_srv_sock);
		cesium_nl_srv_sock = NULL;
	}
}

/**
 * __hdd_set_mac_address() - set the user specified mac address
 * @dev:	Pointer to the net device.
 * @addr:	Pointer to the sockaddr.
 *
 * This function sets the user specified mac address using
 * the command ifconfig wlanX hw ether <mac adress>.
 *
 * Return: 0 for success, non zero for failure
 */
static int __hdd_set_mac_address(struct net_device *dev, void *addr)
{
	hdd_adapter_t *adapter = WLAN_HDD_GET_PRIV_PTR(dev);
	hdd_adapter_t *adapter_temp;
	hdd_context_t *hdd_ctx;
	struct sockaddr *psta_mac_addr = addr;
	QDF_STATUS qdf_ret_status = QDF_STATUS_SUCCESS;
	int ret;
	struct qdf_mac_addr mac_addr;

	ENTER_DEV(dev);

	if (netif_running(dev)) {
		hdd_err("On iface up, set mac address change isn't supported");
		return -EBUSY;
	}

	hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	ret = wlan_hdd_validate_context(hdd_ctx);
	if (0 != ret)
		return ret;

	qdf_mem_copy(&mac_addr, psta_mac_addr->sa_data, QDF_MAC_ADDR_SIZE);
	adapter_temp = hdd_get_adapter_by_macaddr(hdd_ctx, mac_addr.bytes);
	if (adapter_temp) {
		if (!qdf_str_cmp(adapter_temp->dev->name, dev->name))
			return 0;
		hdd_err("%s adapter exist with same address " MAC_ADDRESS_STR,
			adapter_temp->dev->name,
			MAC_ADDR_ARRAY(mac_addr.bytes));
		return -EINVAL;
	}

	qdf_ret_status = wlan_hdd_validate_mac_address(&mac_addr);
	if (QDF_IS_STATUS_ERROR(qdf_ret_status))
		return -EINVAL;

	hdd_info("Changing MAC to " MAC_ADDRESS_STR " of the interface %s ",
		 MAC_ADDR_ARRAY(mac_addr.bytes), dev->name);

	memcpy(&adapter->macAddressCurrent, psta_mac_addr->sa_data, ETH_ALEN);
	memcpy(dev->dev_addr, psta_mac_addr->sa_data, ETH_ALEN);

	EXIT();
	return qdf_ret_status;
}

/**
 * hdd_set_mac_address() - Wrapper function to protect __hdd_set_mac_address()
 *			function from SSR
 * @dev: pointer to net_device structure
 * @addr: Pointer to the sockaddr
 *
 * This function sets the user specified mac address using
 * the command ifconfig wlanX hw ether <mac adress>.
 *
 * Return: 0 for success.
 */
static int hdd_set_mac_address(struct net_device *dev, void *addr)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_set_mac_address(dev, addr);
	cds_ssr_unprotect(__func__);

	return ret;
}

static uint8_t *wlan_hdd_get_derived_intf_addr(hdd_context_t *hdd_ctx)
{
	int i;

	for (i = 0; i < hdd_ctx->num_derived_addr; i++) {
		if (!qdf_atomic_test_and_set_bit(
					i, &hdd_ctx->derived_intf_addr_mask))
			break;
	}
	if (hdd_ctx->num_derived_addr == i) {
		hdd_err("No free derived Addresses");
		return NULL;
	}
	hdd_info("Assigning MAC from derived list" MAC_ADDRESS_STR,
		 MAC_ADDR_ARRAY(hdd_ctx->derived_mac_addr[i].bytes));
	return &hdd_ctx->derived_mac_addr[i].bytes[0];
}

static uint8_t *wlan_hdd_get_provisioned_intf_addr(hdd_context_t *hdd_ctx)
{
	int i;

	for (i = 0; i < hdd_ctx->num_provisioned_addr; i++) {
		if (!qdf_atomic_test_and_set_bit(
				i, &hdd_ctx->provisioned_intf_addr_mask))
			break;
	}
	if (hdd_ctx->num_provisioned_addr == i) {
		hdd_err("No free provisioned Addresses");
		return NULL;
	}
	hdd_info("Assigning MAC from provisioned list" MAC_ADDRESS_STR,
		 MAC_ADDR_ARRAY(hdd_ctx->provisioned_mac_addr[i].bytes));
	return &hdd_ctx->provisioned_mac_addr[i].bytes[0];
}

uint8_t *wlan_hdd_get_intf_addr(hdd_context_t *hdd_ctx,
				enum tQDF_ADAPTER_MODE interface_type)
{
	uint8_t *mac_addr = NULL;

	if (qdf_atomic_test_bit(interface_type,
				(unsigned long *)
				(&hdd_ctx->config->provisioned_intf_pool)))
		mac_addr = wlan_hdd_get_provisioned_intf_addr(hdd_ctx);

	if ((!mac_addr) &&
	    (qdf_atomic_test_bit(interface_type,
				 (unsigned long *)
				 (&hdd_ctx->config->derived_intf_pool))))
		mac_addr = wlan_hdd_get_derived_intf_addr(hdd_ctx);

	if (!mac_addr)
		hdd_err("MAC is not available in both the lists");
	return mac_addr;
}

void wlan_hdd_release_intf_addr(hdd_context_t *hdd_ctx, uint8_t *releaseAddr)
{
	int i;

	for (i = 0; i < hdd_ctx->num_provisioned_addr; i++) {
		if (!memcmp(releaseAddr,
			    &hdd_ctx->provisioned_mac_addr[i].bytes[0],
			    6)) {
			qdf_atomic_clear_bit(i, &hdd_ctx->
					     provisioned_intf_addr_mask);
			hdd_info("Releasing MAC from provisioned list");
			hdd_info(MAC_ADDRESS_STR,
				 MAC_ADDR_ARRAY(hdd_ctx->
					       provisioned_mac_addr[i].bytes));
			break;
		}
	}

	if (i == hdd_ctx->num_provisioned_addr) {
		for (i = 0; i < hdd_ctx->num_derived_addr; i++) {
			if (!memcmp(releaseAddr,
				    &hdd_ctx->derived_mac_addr[i].bytes[0],
				    6)) {
				qdf_atomic_clear_bit(
						i, &hdd_ctx->
						derived_intf_addr_mask);
				hdd_info("Releasing MAC from derived list");
				hdd_info(MAC_ADDRESS_STR,
					 MAC_ADDR_ARRAY(
						hdd_ctx->
						derived_mac_addr[i].bytes));
				break;
			}
		}
		if (i == hdd_ctx->num_derived_addr) {
			hdd_err("Releasing non existing MAC" MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(releaseAddr));
		}
	}
}

#ifdef WLAN_FEATURE_PACKET_FILTERING
/**
 * __hdd_set_multicast_list() - set the multicast address list
 * @dev:	Pointer to the WLAN device.
 * @skb:	Pointer to OS packet (sk_buff).
 *
 * This funciton sets the multicast address list.
 *
 * Return: None
 */
static void __hdd_set_multicast_list(struct net_device *dev)
{
	hdd_adapter_t *adapter = WLAN_HDD_GET_PRIV_PTR(dev);
	int mc_count;
	int i = 0, status;
	struct netdev_hw_addr *ha;
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	static const uint8_t ipv6_router_solicitation[]
			= {0x33, 0x33, 0x00, 0x00, 0x00, 0x02};

	ENTER_DEV(dev);

	if (QDF_GLOBAL_FTM_MODE == hdd_get_conparam())
		return;

	status = wlan_hdd_validate_context(hdd_ctx);
	if (0 != status)
		return;

	status = hdd_validate_adapter(adapter);
	if (status)
		return;

	if (hdd_ctx->driver_status == DRIVER_MODULES_CLOSED) {
		hdd_err("Driver Modules are closed, cannot set mc addr list");
		return;
	}


	if (!hdd_ctx->config->fEnableMCAddrList) {
		hdd_debug("gMCAddrListEnable ini param not enabled");
		adapter->mc_addr_list.mc_cnt = 0;
		return;
	}

	/* Delete already configured multicast address list */
	if (adapter->mc_addr_list.mc_cnt > 0)
		if (wlan_hdd_set_mc_addr_list(adapter, false)) {
			hdd_debug("failed to clear mc addr list");
			return;
		}

	if (dev->flags & IFF_ALLMULTI) {
		hdd_debug("allow all multicast frames");
		adapter->mc_addr_list.mc_cnt = 0;
	} else {
		mc_count = netdev_mc_count(dev);
		hdd_debug("mc_count : %u", mc_count);

		if (mc_count > WLAN_HDD_MAX_MC_ADDR_LIST) {
			hdd_debug("Exceeded max MC filter addresses (%d). Allowing all MC frames by disabling MC address filtering",
				   WLAN_HDD_MAX_MC_ADDR_LIST);

			if (wlan_hdd_set_mc_addr_list(adapter, false))
				hdd_debug("failed to clear mc addr list");

			adapter->mc_addr_list.mc_cnt = 0;
			return;
		}

		adapter->mc_addr_list.mc_cnt = mc_count;

		netdev_for_each_mc_addr(ha, dev) {
			hdd_debug("ha_addr[%d] "MAC_ADDRESS_STR,
				i, MAC_ADDR_ARRAY(ha->addr));

			if (i == mc_count)
				break;
			/*
			 * Skip following addresses:
			 * 1)IPv6 router solicitation address
			 * 2)Any other address pattern if its set during
			 *  RXFILTER REMOVE driver command based on
			 *  addr_filter_pattern
			 */
			if ((!memcmp(ha->addr, ipv6_router_solicitation,
			   ETH_ALEN)) ||
			   (adapter->addr_filter_pattern && (!memcmp(ha->addr,
			    &adapter->addr_filter_pattern, 1)))) {
				hdd_debug("MC/BC filtering Skip addr ="MAC_ADDRESS_STR,
					MAC_ADDR_ARRAY(ha->addr));
				adapter->mc_addr_list.mc_cnt--;
				continue;
			}

			memset(&(adapter->mc_addr_list.addr[i][0]), 0,
			       ETH_ALEN);
			memcpy(&(adapter->mc_addr_list.addr[i][0]), ha->addr,
			       ETH_ALEN);
			hdd_debug("mlist[%d] = " MAC_ADDRESS_STR, i,
			       MAC_ADDR_ARRAY(adapter->mc_addr_list.addr[i]));
			i++;
		}
	}
	if (hdd_ctx->config->active_mode_offload) {
		hdd_debug("enable mc filtering");
		if (wlan_hdd_set_mc_addr_list(adapter, true))
			hdd_err("Failed: to set mc addr list");
	} else {
		hdd_debug("skip mc filtering enable it during cfg80211 suspend");
	}
	EXIT();
}

/**
 * hdd_set_multicast_list() - SSR wrapper function for __hdd_set_multicast_list
 * @dev: pointer to net_device
 *
 * Return: none
 */
static void hdd_set_multicast_list(struct net_device *dev)
{
	cds_ssr_protect(__func__);
	__hdd_set_multicast_list(dev);
	cds_ssr_unprotect(__func__);
}
#endif

/**
 * hdd_select_queue() - used by Linux OS to decide which queue to use first
 * @dev:	Pointer to the WLAN device.
 * @skb:	Pointer to OS packet (sk_buff).
 *
 * This function is registered with the Linux OS for network
 * core to decide which queue to use first.
 *
 * Return: ac, Queue Index/access category corresponding to UP in IP header
 */
static uint16_t hdd_select_queue(struct net_device *dev, struct sk_buff *skb
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0))
			  , void *accel_priv
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0))
			  , select_queue_fallback_t fallback
#endif
)
{
	return hdd_wmm_select_queue(dev, skb);
}

static const struct net_device_ops wlan_drv_ops = {
	.ndo_open = hdd_open,
	.ndo_stop = hdd_stop,
	.ndo_uninit = hdd_uninit,
	.ndo_start_xmit = hdd_hard_start_xmit,
	.ndo_tx_timeout = hdd_tx_timeout,
	.ndo_get_stats = hdd_get_stats,
	.ndo_do_ioctl = hdd_ioctl,
	.ndo_set_mac_address = hdd_set_mac_address,
	.ndo_select_queue = hdd_select_queue,
#ifdef WLAN_FEATURE_PACKET_FILTERING
	.ndo_set_rx_mode = hdd_set_multicast_list,
#endif
};

/* Monitor mode net_device_ops, doesnot Tx and most of operations. */
static const struct net_device_ops wlan_mon_drv_ops = {
	.ndo_open = hdd_mon_open,
	.ndo_stop = hdd_stop,
	.ndo_get_stats = hdd_get_stats,
};

/**
 * hdd_set_station_ops() - update net_device ops for monitor mode
 * @pWlanDev: Handle to struct net_device to be updated.
 * Return: None
 */
void hdd_set_station_ops(struct net_device *pWlanDev)
{
	if (QDF_GLOBAL_MONITOR_MODE == cds_get_conparam())
		pWlanDev->netdev_ops = &wlan_mon_drv_ops;
	else
		pWlanDev->netdev_ops = &wlan_drv_ops;
}

/**
 * hdd_adapter_init_action_frame_random_mac() - Initialze attributes needed for
 * randomization of SA in management action frames
 * @adapter: Pointer to adapter
 *
 * Return: None
 */
static void hdd_adapter_init_action_frame_random_mac(hdd_adapter_t *adapter)
{
	spin_lock_init(&adapter->random_mac_lock);
	qdf_mem_zero(adapter->random_mac, sizeof(adapter->random_mac));
}

/**
 * hdd_alloc_station_adapter() - allocate the station hdd adapter
 * @hdd_ctx: global hdd context
 * @macAddr: mac address to assign to the interface
 * @name: User-visible name of the interface
 * @session_type: interface type to be created
 *
 * hdd adapter pointer would point to the netdev->priv space, this function
 * would retrive the pointer, and setup the hdd adapter configuration.
 *
 * Return: the pointer to hdd adapter, otherwise NULL
 */
static hdd_adapter_t *hdd_alloc_station_adapter(hdd_context_t *hdd_ctx,
						tSirMacAddr macAddr,
						unsigned char name_assign_type,
						const char *name,
						uint8_t session_type)
{
	struct net_device *pWlanDev = NULL;
	hdd_adapter_t *adapter = NULL;
	hdd_station_ctx_t *sta_ctx;
	QDF_STATUS qdf_status;
	/*
	 * cfg80211 initialization and registration....
	 */
	pWlanDev = alloc_netdev_mq(sizeof(hdd_adapter_t), name,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 17, 0)) || defined(WITH_BACKPORTS)
				   name_assign_type,
#endif
			((QDF_GLOBAL_MONITOR_MODE == cds_get_conparam() ||
			  wlan_hdd_is_session_type_monitor(session_type)) ?
			  hdd_mon_mode_ether_setup : ether_setup),
			NUM_TX_QUEUES);

	if (pWlanDev != NULL) {

		/* Save the pointer to the net_device in the HDD adapter */
		adapter = (hdd_adapter_t *) netdev_priv(pWlanDev);

		qdf_mem_zero(adapter, sizeof(hdd_adapter_t));
		sta_ctx = &adapter->sessionCtx.station;
		qdf_mem_set(sta_ctx->conn_info.staId,
			sizeof(sta_ctx->conn_info.staId),
			HDD_WLAN_INVALID_STA_ID);
		adapter->dev = pWlanDev;
		adapter->pHddCtx = hdd_ctx;
		adapter->magic = WLAN_HDD_ADAPTER_MAGIC;
		adapter->sessionId = HDD_SESSION_ID_INVALID;

		qdf_status = qdf_event_create(
				&adapter->qdf_session_open_event);
		if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
			hdd_err("Session open QDF event init failed!");
			goto err_qdf_init;
		}

		qdf_status = qdf_event_create(
				&adapter->qdf_session_close_event);
		if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
			hdd_err("Session close QDF event init failed!");
			goto err_qdf_init;
		}

		adapter->offloads_configured = false;
		adapter->isLinkUpSvcNeeded = false;
		adapter->higherDtimTransition = true;
		adapter->send_mode_change = true;
		/* Init the net_device structure */
		strlcpy(pWlanDev->name, name, IFNAMSIZ);

		qdf_mem_copy(pWlanDev->dev_addr, (void *)macAddr,
			     sizeof(tSirMacAddr));
		qdf_mem_copy(adapter->macAddressCurrent.bytes, macAddr,
			     sizeof(tSirMacAddr));
		pWlanDev->watchdog_timeo = HDD_TX_TIMEOUT;

		if (hdd_ctx->config->enable_ip_tcp_udp_checksum_offload)
			pWlanDev->features |=
				NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM;
		pWlanDev->features |= NETIF_F_RXCSUM;

		hdd_set_tso_flags(hdd_ctx, pWlanDev);

		if (wlan_hdd_is_session_type_monitor(session_type))
			pWlanDev->netdev_ops = &wlan_mon_drv_ops;
		else
			hdd_set_station_ops(adapter->dev);

		hdd_dev_setup_destructor(pWlanDev);
		pWlanDev->ieee80211_ptr = &adapter->wdev;
		pWlanDev->tx_queue_len = HDD_NETDEV_TX_QUEUE_LEN;
		adapter->wdev.wiphy = hdd_ctx->wiphy;
		adapter->wdev.netdev = pWlanDev;
		/* set pWlanDev's parent to underlying device */
		SET_NETDEV_DEV(pWlanDev, hdd_ctx->parent_dev);
		hdd_wmm_init(adapter);
		spin_lock_init(&adapter->pause_map_lock);
		adapter->start_time = adapter->last_time = qdf_system_ticks();
	}
	return adapter;

err_qdf_init:
	free_netdev(adapter->dev);
	return NULL;
}

static QDF_STATUS hdd_register_interface(hdd_adapter_t *adapter, bool rtnl_held)
{
	struct net_device *dev = adapter->dev;
	int ret;

	ENTER();

	if (rtnl_held) {
		if (strnchr(dev->name, IFNAMSIZ - 1, '%')) {

			ret = dev_alloc_name(dev, dev->name);
			if (ret < 0) {
				hdd_err(
				    "unable to get dev name: %s, err = 0x%x",
				    dev->name, ret);
				return QDF_STATUS_E_FAILURE;
			}
		}

		ret = register_netdevice(dev);
		if (ret) {
			hdd_err("register_netdevice(%s) failed, err = 0x%x",
				dev->name, ret);
			return QDF_STATUS_E_FAILURE;
		}
	} else {
		ret = register_netdev(dev);
		if (ret) {
			hdd_err("register_netdev(%s) failed, err = 0x%x",
				dev->name, ret);
			return QDF_STATUS_E_FAILURE;
		}
	}
	set_bit(NET_DEVICE_REGISTERED, &adapter->event_flags);

	EXIT();

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS hdd_sme_close_session_callback(void *pContext)
{
	hdd_adapter_t *adapter = pContext;

	if (NULL == adapter) {
		hdd_err("NULL adapter");
		return QDF_STATUS_E_INVAL;
	}

	if (WLAN_HDD_ADAPTER_MAGIC != adapter->magic) {
		hdd_err("Invalid magic");
		return QDF_STATUS_NOT_INITIALIZED;
	}

	/*
	 * For NAN Data interface, the close session results in the final
	 * indication to the userspace
	 */
	if (adapter->device_mode == QDF_NDI_MODE)
		hdd_ndp_session_end_handler(adapter);

	clear_bit(SME_SESSION_OPENED, &adapter->event_flags);

	/*
	 * We can be blocked while waiting for scheduled work to be
	 * flushed, and the adapter structure can potentially be freed, in
	 * which case the magic will have been reset.  So make sure the
	 * magic is still good, and hence the adapter structure is still
	 * valid, before signaling completion
	 */
	if (WLAN_HDD_ADAPTER_MAGIC == adapter->magic)
		qdf_event_set(&adapter->qdf_session_close_event);

	return QDF_STATUS_SUCCESS;
}

/**
 * hdd_check_and_init_tdls() - check and init TDLS operation for desired mode
 * @adapter: pointer to device adapter
 * @type: type of interface
 *
 * This routine will check the mode of adapter and if it is required then it
 * will initialize the TDLS operations
 *
 * Return: QDF_STATUS
 */
#ifdef FEATURE_WLAN_TDLS
static QDF_STATUS hdd_check_and_init_tdls(hdd_adapter_t *adapter, uint32_t type)
{
	if (QDF_IBSS_MODE != type) {
		if (0 != wlan_hdd_tdls_init(adapter)) {
			hdd_err("wlan_hdd_tdls_init failed");
			return QDF_STATUS_E_FAILURE;
		}
		set_bit(TDLS_INIT_DONE, &adapter->event_flags);
	}
	return QDF_STATUS_SUCCESS;
}
#else
static QDF_STATUS hdd_check_and_init_tdls(hdd_adapter_t *adapter, uint32_t type)
{
	return QDF_STATUS_SUCCESS;
}
#endif

/**
 * hdd_update_ini_params() - Update config values to default ini param values
 * @hdd_ctx: hdd context
 *
 * This function is used to update the specific config values that can change
 * at run time to its default ini param values. For example in LONU driver
 * if initial_scan_no_dfs_chnl is set to 1 by default, after scan request it
 * is setting to 0 in csr_scan_request() which leads to scan DFS channels also.
 * So, we need to update initial_scan_no_dfs_chnl to its default ini value.
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS hdd_update_ini_params(hdd_context_t *hdd_ctx)
{
	QDF_STATUS status;
	tSmeConfigParams *sme_config;

	sme_config = qdf_mem_malloc(sizeof(*sme_config));
	if (!sme_config) {
		hdd_err("unable to allocate sme_config");
		return QDF_STATUS_E_NOMEM;
	}

	status = sme_get_config_param(hdd_ctx->hHal, sme_config);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("sme get config failed");
		goto err;
	}

	sme_config->csrConfig.initial_scan_no_dfs_chnl =
		hdd_ctx->config->initial_scan_no_dfs_chnl;

	status = sme_update_config(hdd_ctx->hHal, sme_config);
	if (QDF_IS_STATUS_ERROR(status))
		hdd_err("sme update config failed");
err:
	qdf_mem_free(sme_config);
	return status;
}

QDF_STATUS hdd_init_station_mode(hdd_adapter_t *adapter)
{
	struct net_device *pWlanDev = adapter->dev;
	hdd_station_ctx_t *pHddStaCtx = &adapter->sessionCtx.station;
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	QDF_STATUS qdf_ret_status = QDF_STATUS_SUCCESS;
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	uint32_t type, subType;
	int ret_val;

	status = qdf_event_reset(&adapter->qdf_session_open_event);
	if (QDF_STATUS_SUCCESS != status) {
		hdd_err("failed to reinit session open event");
		goto error_sme_open;
	}
	sme_set_curr_device_mode(hdd_ctx->hHal, adapter->device_mode);
	sme_set_pdev_ht_vht_ies(hdd_ctx->hHal, hdd_ctx->config->enable2x2);
	status = cds_get_vdev_types(adapter->device_mode, &type, &subType);
	if (QDF_STATUS_SUCCESS != status) {
		hdd_err("failed to get vdev type");
		goto error_sme_open;
	}
	/* Open a SME session for future operation */
	qdf_ret_status =
		sme_open_session(hdd_ctx->hHal, hdd_sme_roam_callback, adapter,
				 (uint8_t *) &adapter->macAddressCurrent,
				 &adapter->sessionId, type, subType);
	if (!QDF_IS_STATUS_SUCCESS(qdf_ret_status)) {
		hdd_alert("sme_open_session() failed, status code %08d [x%08x]",
		       qdf_ret_status, qdf_ret_status);
		status = QDF_STATUS_E_FAILURE;
		goto error_sme_open;
	}
	/* Block on a completion variable. Can't wait forever though. */
	status = qdf_wait_for_event_completion(&adapter->qdf_session_open_event,
			WLAN_WAIT_TIME_SESSIONOPENCLOSE);
	if (QDF_STATUS_SUCCESS != status) {
		if (adapter->qdf_session_open_event.force_set) {
			/*
			 * SSR/PDR has caused shutdown, which has forcefully
			 * set the event. Return without the closing session.
			 */
			adapter->sessionId = HDD_SESSION_ID_INVALID;
			hdd_err("Session open event forcefully set");
			goto error_sme_open;
		} else {
			if (QDF_STATUS_E_TIMEOUT == status)
				hdd_err("Session failed to open within timeout period");
			else
				hdd_err("Failed to wait for session open event(status-%d)",
					status);

			goto error_register_wext;
		}
	}

	if (adapter->device_mode == QDF_STA_MODE) {
		hdd_debug("setting RTT mac randomization param: %d",
			hdd_ctx->config->enable_rtt_mac_randomization);
		ret_val = sme_cli_set_command(adapter->sessionId,
			WMI_VDEV_PARAM_ENABLE_DISABLE_RTT_INITIATOR_RANDOM_MAC,
			hdd_ctx->config->enable_rtt_mac_randomization,
			VDEV_CMD);
		if (0 != ret_val)
			hdd_err("RTT mac randomization param set failed %d",
				ret_val);
	}
	/*
	 * 1) When DBS hwmode is disabled from INI then send HT/VHT IE as per
	 *    non-dbs hw mode, so that there is no limitation applied for 2G/5G.
	 * 2) When DBS hw mode is enabled, master Rx LDPC is enabled, 2G RX LDPC
	 *    support is enabled, and if it is STA connection then send HT/VHT
	 *    IE as per non-dbs hw mode, so that there is no limitation applied
	 *    for first connection (initial connections as well as roaming
	 *    scenario). As soon as second connection comes up policy manager
	 *    will take care of imposing Rx LDPC limitation of STA connection
	 *    (for current connection as well as roaming scenario).
	 * 3) When DBS hw mode is supported but RX LDPC is disabled or 2G RXLPDC
	 *    support is disabled then send HT/VHT IE as per DBS hw mode, so
	 *    that STA will not use Rx LDPC for 2G connection.
	 */
	if (!wma_is_hw_dbs_capable() ||
		(((QDF_STA_MODE == adapter->device_mode) &&
			hdd_ctx->config->enable_rx_ldpc &&
			hdd_ctx->config->rx_ldpc_support_for_2g) &&
					!wma_is_current_hwmode_dbs())) {
		hdd_notice("send HT/VHT IE per band using nondbs hwmode");
		sme_set_vdev_ies_per_band(adapter->sessionId, false);
	} else {
		hdd_notice("send HT/VHT IE per band using dbs hwmode");
		sme_set_vdev_ies_per_band(adapter->sessionId, true);
	}

	/* Register wireless extensions */
	qdf_ret_status = hdd_register_wext(pWlanDev);
	if (QDF_STATUS_SUCCESS != qdf_ret_status) {
		hdd_err("hdd_register_wext() failed, status code %08d [x%08x]",
		       qdf_ret_status, qdf_ret_status);
		status = QDF_STATUS_E_FAILURE;
		goto error_register_wext;
	}
	hdd_conn_set_connection_state(adapter, eConnectionState_NotConnected);

	qdf_mem_set(pHddStaCtx->conn_info.staId,
		sizeof(pHddStaCtx->conn_info.staId), HDD_WLAN_INVALID_STA_ID);

	/* set fast roaming capability in sme session */
	status = sme_config_fast_roaming(hdd_ctx->hHal,
					 adapter->sessionId, true);
	/* Set the default operation channel */
	pHddStaCtx->conn_info.operationChannel =
		hdd_ctx->config->OperatingChannel;

	/* Make the default Auth Type as OPEN */
	pHddStaCtx->conn_info.authType = eCSR_AUTH_TYPE_OPEN_SYSTEM;

	status = hdd_init_tx_rx(adapter);
	if (QDF_STATUS_SUCCESS != status) {
		hdd_err("hdd_init_tx_rx() failed, status code %08d [x%08x]",
		       status, status);
		goto error_init_txrx;
	}

	set_bit(INIT_TX_RX_SUCCESS, &adapter->event_flags);

	status = hdd_wmm_adapter_init(adapter);
	if (QDF_STATUS_SUCCESS != status) {
		hdd_err("hdd_wmm_adapter_init() failed, status code %08d [x%08x]",
		       status, status);
		goto error_wmm_init;
	}

	set_bit(WMM_INIT_DONE, &adapter->event_flags);

	ret_val = sme_cli_set_command(adapter->sessionId,
				      WMI_PDEV_PARAM_BURST_ENABLE,
				      HDD_ENABLE_SIFS_BURST_DEFAULT,
				      PDEV_CMD);

	if (0 != ret_val) {
		hdd_err("WMI_PDEV_PARAM_BURST_ENABLE set failed %d",
		       ret_val);
	}

	status = hdd_check_and_init_tdls(adapter, type);
	if (status != QDF_STATUS_SUCCESS)
		goto error_tdls_init;

	status = hdd_lro_enable(hdd_ctx, adapter);
	if (status)
		/* Err code from errno.h */
		hdd_debug("LRO is disabled either because of kernel doesnot support or disabled in INI or via vendor commandi. err code %d",
			  status);

	/* rcpi info initialization */
	qdf_mem_zero(&adapter->rcpi, sizeof(adapter->rcpi));

	if ((adapter->device_mode == QDF_STA_MODE) ||
	    (adapter->device_mode == QDF_P2P_CLIENT_MODE)) {
		ret_val = hdd_update_ini_params(hdd_ctx);
		return ret_val;
	}

	return QDF_STATUS_SUCCESS;

error_tdls_init:
	clear_bit(WMM_INIT_DONE, &adapter->event_flags);
	hdd_wmm_adapter_close(adapter);
error_wmm_init:
	clear_bit(INIT_TX_RX_SUCCESS, &adapter->event_flags);
	hdd_deinit_tx_rx(adapter);
error_init_txrx:
	hdd_unregister_wext(pWlanDev);
error_register_wext:
	if (adapter->sessionId != HDD_SESSION_ID_INVALID) {
		qdf_event_reset(&adapter->qdf_session_close_event);
		if (QDF_STATUS_SUCCESS == sme_close_session(hdd_ctx->hHal,
						adapter->sessionId, true,
						hdd_sme_close_session_callback,
						adapter)) {
			/*
			 * Block on a completion variable.
			 * Can't wait forever though.
			 */
			status = qdf_wait_for_event_completion(
				&adapter->qdf_session_close_event,
				WLAN_WAIT_TIME_SESSIONOPENCLOSE);
			if (QDF_STATUS_SUCCESS != status)
				hdd_err("Unable to close session (%u)",
					status);
		}
		adapter->sessionId = HDD_SESSION_ID_INVALID;
	}
error_sme_open:
	return status;
}

void hdd_cleanup_actionframe(hdd_context_t *hdd_ctx, hdd_adapter_t *adapter)
{
	hdd_cfg80211_state_t *cfgState;

	cfgState = WLAN_HDD_GET_CFG_STATE_PTR(adapter);

	if (NULL != cfgState->buf) {
		unsigned long rc;

		hdd_debug("Wait for ack for the previous action frame");
		rc = wait_for_completion_timeout(
			&adapter->tx_action_cnf_event,
			msecs_to_jiffies(ACTION_FRAME_TX_TIMEOUT));
		if (!rc) {
			hdd_debug("HDD Wait for Action Confirmation Failed!!");
			/*
			 * Inform tx status as FAILURE to upper layer and free
			 * cfgState->buf
			 */
			 hdd_send_action_cnf(adapter, false);
		} else
			hdd_debug("Wait complete");
	}
}

/**
 * hdd_cleanup_actionframe_no_wait() - Clean up pending action frame
 * @hdd_ctx: global hdd context
 * @adapter: pointer to adapter
 *
 * This function used to cancel the pending action frame without waiting for
 * ack.
 *
 * Return: None.
 */
void hdd_cleanup_actionframe_no_wait(hdd_context_t *hdd_ctx,
		hdd_adapter_t *adapter)
{
	hdd_cfg80211_state_t *cfgState;

	cfgState = WLAN_HDD_GET_CFG_STATE_PTR(adapter);

	if (cfgState->buf) {
		/*
		 * Inform tx status as FAILURE to upper layer and free
		 * cfgState->buf
		 */
		hdd_debug("Cleanup previous action frame without waiting for the ack");
		hdd_send_action_cnf(adapter, false);
	}
}

/**
 * hdd_cleanup_actionframe_all_adapters() - Clean up pending action frame
 * @hdd_ctx: global hdd context
 *
 * This function cleans up pending action frame without waiting for
 * ack on all adapters.
 *
 * Return: None
 */
static QDF_STATUS hdd_cleanup_actionframe_all_adapters(hdd_context_t *hdd_ctx)
{
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;

	ENTER();

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);
	while (adapter_node && QDF_IS_STATUS_SUCCESS(status)) {
		adapter = adapter_node->pAdapter;
		hdd_cleanup_actionframe_no_wait(hdd_ctx, adapter);
		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}

	EXIT();
	return QDF_STATUS_SUCCESS;
}

/**
 * hdd_station_adapter_deinit() - De-initialize the station adapter
 * @hdd_ctx: global hdd context
 * @adapter: HDD adapter
 * @rtnl_held: Used to indicate whether or not the caller is holding
 *             the kernel rtnl_mutex
 *
 * This function De-initializes the STA/P2P/OCB adapter.
 *
 * Return: None.
 */
static void hdd_station_adapter_deinit(hdd_context_t *hdd_ctx,
				       hdd_adapter_t *adapter,
				       bool rtnl_held)
{
	ENTER_DEV(adapter->dev);

	if (adapter->dev) {
		if (rtnl_held)
			adapter->dev->wireless_handlers = NULL;
		else {
			rtnl_lock();
			adapter->dev->wireless_handlers = NULL;
			rtnl_unlock();
		}
	}

	if (test_bit(INIT_TX_RX_SUCCESS, &adapter->event_flags)) {
		hdd_deinit_tx_rx(adapter);
		clear_bit(INIT_TX_RX_SUCCESS, &adapter->event_flags);
	}

	if (test_bit(WMM_INIT_DONE, &adapter->event_flags)) {
		hdd_wmm_adapter_close(adapter);
		clear_bit(WMM_INIT_DONE, &adapter->event_flags);
	}

	hdd_cleanup_actionframe(hdd_ctx, adapter);

	EXIT();
}

/**
 * hdd_ap_adapter_deinit() - De-initialize the ap adapter
 * @hdd_ctx: global hdd context
 * @adapter: HDD adapter
 * @rtnl_held: the rtnl lock hold flag
 * This function De-initializes the AP/P2PGo adapter.
 *
 * Return: None.
 */
static void hdd_ap_adapter_deinit(hdd_context_t *hdd_ctx,
				  hdd_adapter_t *adapter,
				  bool rtnl_held)
{
	ENTER_DEV(adapter->dev);

	if (test_bit(WMM_INIT_DONE, &adapter->event_flags)) {
		hdd_wmm_adapter_close(adapter);
		clear_bit(WMM_INIT_DONE, &adapter->event_flags);
	}
	qdf_atomic_set(&adapter->sessionCtx.ap.acs_in_progress, 0);
	wlan_hdd_undo_acs(adapter);

	hdd_cleanup_actionframe(hdd_ctx, adapter);

	hdd_unregister_hostapd(adapter, rtnl_held);

	EXIT();
}

void hdd_deinit_adapter(hdd_context_t *hdd_ctx, hdd_adapter_t *adapter,
			bool rtnl_held)
{
	ENTER();

	switch (adapter->device_mode) {
	case QDF_STA_MODE:
	case QDF_P2P_CLIENT_MODE:
	case QDF_P2P_DEVICE_MODE:
	{
		hdd_station_adapter_deinit(hdd_ctx, adapter, rtnl_held);
		break;
	}

	case QDF_SAP_MODE:
	case QDF_P2P_GO_MODE:
	{

		hdd_ap_adapter_deinit(hdd_ctx, adapter, rtnl_held);
		break;
	}

	default:
		break;
	}

	EXIT();
}

#ifdef WLAN_NS_OFFLOAD
/**
 * hdd_ns_offload_info_lock_create() - Create mutex lock for ns offload info
 * @adapter: pointer to adapter for which lock is to be created
 *
 * Return: None
 */
static void hdd_ns_offload_info_lock_create(hdd_adapter_t *adapter)
{
	qdf_mutex_create(&adapter->ns_offload_info_lock);
}

/**
 * hdd_ns_offload_info_lock_destroy() - Destroy mutex lock for ns offload info
 * @adapter: pointer to adapter for which lock is to be destroyed
 *
 * Return: None
 */
static void hdd_ns_offload_info_lock_destroy(hdd_adapter_t *adapter)
{
	qdf_mutex_destroy(&adapter->ns_offload_info_lock);
}
#else
/**
 * hdd_ns_offload_info_lock_create() - Create mutex lock for ns offload info
 * @adapter: pointer to adapter for which lock is to be created
 *
 * Return: None
 */
static void hdd_ns_offload_info_lock_create(hdd_adapter_t *adapter)
{
}

/**
 * hdd_ns_offload_info_lock_destroy() - Destroy mutex lock for ns offload info
 * @adapter: pointer to adapter for which lock is to be destroyed
 *
 * Return: None
 */
static void hdd_ns_offload_info_lock_destroy(hdd_adapter_t *adapter)
{
}
#endif

static void hdd_cleanup_adapter(hdd_context_t *hdd_ctx, hdd_adapter_t *adapter,
				bool rtnl_held)
{
	struct net_device *pWlanDev = NULL;

	if (adapter)
		pWlanDev = adapter->dev;
	else {
		hdd_err("adapter is Null");
		return;
	}

	qdf_list_destroy(&adapter->blocked_scan_request_q);
	qdf_mutex_destroy(&adapter->blocked_scan_request_q_lock);

	wlan_hdd_debugfs_csr_deinit(adapter);
	qdf_mutex_destroy(&adapter->arp_offload_info_lock);
	hdd_ns_offload_info_lock_destroy(adapter);
	hdd_apf_context_destroy(adapter);

	hdd_debugfs_exit(adapter);

	/*
	 * The adapter is marked as closed. When hdd_wlan_exit() call returns,
	 * the driver is almost closed and cannot handle either control
	 * messages or data. However, unregister_netdevice() call above will
	 * eventually invoke hdd_stop(ndo_close) driver callback, which attempts
	 * to close the active connections(basically excites control path) which
	 * is not right. Setting this flag helps hdd_stop() to recognize that
	 * the interface is closed and restricts any operations on that
	 */
	clear_bit(DEVICE_IFACE_OPENED, &adapter->event_flags);

	if (test_bit(NET_DEVICE_REGISTERED, &adapter->event_flags)) {
		if (rtnl_held)
			unregister_netdevice(pWlanDev);
		else
			unregister_netdev(pWlanDev);
		/*
		 * Note that the adapter is no longer valid at this point
		 * since the memory has been reclaimed
		 */
	}
}

static QDF_STATUS hdd_check_for_existing_macaddr(hdd_context_t *hdd_ctx,
						 tSirMacAddr macAddr)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	hdd_adapter_t *adapter;
	QDF_STATUS status;

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);
	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;
		if (adapter
		    && !qdf_mem_cmp(adapter->macAddressCurrent.bytes,
				       macAddr, sizeof(tSirMacAddr))) {
			return QDF_STATUS_E_FAILURE;
		}
		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}
	return QDF_STATUS_SUCCESS;
}

#ifdef CONFIG_FW_LOGS_BASED_ON_INI
/**
 * hdd_set_fw_log_params() - Set log parameters to FW
 * @hdd_ctx: HDD Context
 * @adapter: HDD Adapter
 *
 * This function set the FW Debug log level based on the INI.
 *
 * Return: None
 */
static void hdd_set_fw_log_params(hdd_context_t *hdd_ctx,
				  hdd_adapter_t *adapter)
{
	uint8_t count = 0, numentries = 0,
			moduleloglevel[FW_MODULE_LOG_LEVEL_STRING_LENGTH];
	uint32_t value = 0;
	int ret;

	if (QDF_GLOBAL_FTM_MODE == cds_get_conparam() ||
	    (!hdd_ctx->config->enable_fw_log)) {
		hdd_debug("enable_fw_log not enabled in INI or in FTM mode return");
		return;
	}

	/* Enable FW logs based on INI configuration */
	hdd_ctx->fw_log_settings.dl_type =
			hdd_ctx->config->enableFwLogType;
	ret = sme_cli_set_command(adapter->sessionId,
			WMI_DBGLOG_TYPE,
			hdd_ctx->config->enableFwLogType,
			DBG_CMD);
	if (ret != 0)
		hdd_warn("Failed to enable FW log type ret %d",
			ret);

	hdd_ctx->fw_log_settings.dl_loglevel =
			hdd_ctx->config->enableFwLogLevel;
	ret = sme_cli_set_command(adapter->sessionId,
			WMI_DBGLOG_LOG_LEVEL,
			hdd_ctx->config->enableFwLogLevel,
			DBG_CMD);
	if (ret != 0)
		hdd_warn("Failed to enable FW log level ret %d",
			ret);

	hdd_string_to_u8_array(
		hdd_ctx->config->enableFwModuleLogLevel,
		moduleloglevel,
		&numentries,
		FW_MODULE_LOG_LEVEL_STRING_LENGTH);

	while (count < numentries) {
		/*
		 * FW module log level input string looks like
		 * below:
		 * gFwDebugModuleLoglevel=<FW Module ID>,
		 * <Log Level>,...
		 * For example:
		 * gFwDebugModuleLoglevel=
		 * 1,0,2,1,3,2,4,3,5,4,6,5,7,6
		 * Above input string means :
		 * For FW module ID 1 enable log level 0
		 * For FW module ID 2 enable log level 1
		 * For FW module ID 3 enable log level 2
		 * For FW module ID 4 enable log level 3
		 * For FW module ID 5 enable log level 4
		 * For FW module ID 6 enable log level 5
		 * For FW module ID 7 enable log level 6
		 */

		if ((moduleloglevel[count] > WLAN_MODULE_ID_MAX)
			|| (moduleloglevel[count + 1] > DBGLOG_LVL_MAX)) {
			hdd_err("Module id %d and dbglog level %d input length is more than max",
				moduleloglevel[count],
				moduleloglevel[count + 1]);
			return;
		}

		value = moduleloglevel[count] << 16;
		value |= moduleloglevel[count + 1];
		ret = sme_cli_set_command(adapter->sessionId,
				WMI_DBGLOG_MOD_LOG_LEVEL,
				value, DBG_CMD);
		if (ret != 0)
			hdd_err("Failed to enable FW module log level %d ret %d",
				value, ret);

		count += 2;
	}

}
#else
static void hdd_set_fw_log_params(hdd_context_t *hdd_ctx,
				  hdd_adapter_t *adapter)
{
}

#endif

/**
 * hdd_configure_chain_mask() - programs chain mask to firmware
 * @adapter: HDD adapter
 *
 * Return: 0 on success or errno on failure
 */
static int hdd_configure_chain_mask(hdd_adapter_t *adapter)
{
	int ret_val;
	QDF_STATUS status;
	struct wma_caps_per_phy non_dbs_phy_cap;
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(adapter);

	hdd_debug("enable2x2: %d, lte_coex: %d, ChainMask1x1: tx: %d rx: %d",
		  hdd_ctx->config->enable2x2, hdd_ctx->lte_coex_ant_share,
		  hdd_ctx->config->txchainmask1x1,
		  hdd_ctx->config->rxchainmask1x1);
	hdd_debug("disable_DBS: %d, tx_chain_mask_2g: %d, rx_chain_mask_2g: %d",
		  hdd_ctx->config->dual_mac_feature_disable,
		  hdd_ctx->config->tx_chain_mask_2g,
		  hdd_ctx->config->rx_chain_mask_2g);
	hdd_debug("tx_chain_mask_5g: %d, rx_chain_mask_5g: %d",
		  hdd_ctx->config->tx_chain_mask_5g,
		  hdd_ctx->config->rx_chain_mask_5g);

	status = wma_get_caps_for_phyidx_hwmode(&non_dbs_phy_cap,
						HW_MODE_DBS_NONE,
						CDS_BAND_ALL);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_info("couldn't get phy caps. skip chain mask programming");
		return 0;
	}

	if (non_dbs_phy_cap.tx_chain_mask_2G < 3 ||
	    non_dbs_phy_cap.rx_chain_mask_2G < 3 ||
	    non_dbs_phy_cap.tx_chain_mask_5G < 3 ||
	    non_dbs_phy_cap.rx_chain_mask_5G < 3) {
		hdd_info("firmware not capable. skip chain mask programming");
		return 0;
	}

	if (hdd_ctx->config->enable2x2) {
		hdd_info("2x2 enabled. skip chain mask programming");
		return 0;
	}

	if (hdd_ctx->config->dual_mac_feature_disable !=
	    DISABLE_DBS_CXN_AND_SCAN) {
		hdd_info("DBS enabled(%d). skip chain mask programming",
			 hdd_ctx->config->dual_mac_feature_disable);
		return 0;
	}

	if (hdd_ctx->lte_coex_ant_share) {
		hdd_info("lte ant sharing enabled. skip chainmask programming");
		return 0;
	}

	if (hdd_ctx->config->txchainmask1x1) {
		ret_val = sme_cli_set_command(adapter->sessionId,
					      WMI_PDEV_PARAM_TX_CHAIN_MASK,
					      hdd_ctx->config->txchainmask1x1,
					      PDEV_CMD);
		if (ret_val)
			goto error;
	}

	if (hdd_ctx->config->rxchainmask1x1) {
		ret_val = sme_cli_set_command(adapter->sessionId,
					      WMI_PDEV_PARAM_RX_CHAIN_MASK,
					      hdd_ctx->config->rxchainmask1x1,
					      PDEV_CMD);
		if (ret_val)
			goto error;
	}

	if (hdd_ctx->config->txchainmask1x1 ||
	    hdd_ctx->config->rxchainmask1x1) {
		hdd_info("band agnostic tx/rx chain mask set. skip per band chain mask");
		return 0;
	}

	if (hdd_ctx->config->tx_chain_mask_2g) {
		ret_val = sme_cli_set_command(adapter->sessionId,
				WMI_PDEV_PARAM_TX_CHAIN_MASK_2G,
				hdd_ctx->config->tx_chain_mask_2g, PDEV_CMD);
		if (0 != ret_val)
			goto error;
	}

	if (hdd_ctx->config->rx_chain_mask_2g) {
		ret_val = sme_cli_set_command(adapter->sessionId,
				WMI_PDEV_PARAM_RX_CHAIN_MASK_2G,
				hdd_ctx->config->rx_chain_mask_2g, PDEV_CMD);
		if (0 != ret_val)
			goto error;
	}

	if (hdd_ctx->config->tx_chain_mask_5g) {
		ret_val = sme_cli_set_command(adapter->sessionId,
				WMI_PDEV_PARAM_TX_CHAIN_MASK_5G,
				hdd_ctx->config->tx_chain_mask_5g, PDEV_CMD);
		if (0 != ret_val)
			goto error;
	}

	if (hdd_ctx->config->rx_chain_mask_5g) {
		ret_val = sme_cli_set_command(adapter->sessionId,
				WMI_PDEV_PARAM_RX_CHAIN_MASK_5G,
				hdd_ctx->config->rx_chain_mask_5g, PDEV_CMD);
		if (0 != ret_val)
			goto error;
	}

	return 0;

error:
	hdd_err("WMI PDEV set param failed %d", ret_val);
	return -EINVAL;
}

/**
 * hdd_set_fw_params() - Set parameters to firmware
 * @adapter: HDD adapter
 *
 * This function Sets various parameters to fw once the
 * adapter is started.
 *
 * Return: 0 on success or errno on failure
 */
int hdd_set_fw_params(hdd_adapter_t *adapter)
{
	int ret;
	hdd_context_t *hdd_ctx;

	ENTER_DEV(adapter->dev);

	hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	if (!hdd_ctx)
		return -EINVAL;

	if (cds_get_conparam() == QDF_GLOBAL_FTM_MODE) {
		hdd_debug("FTM Mode is active; nothing to do");
		return 0;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				WMI_PDEV_PARAM_DTIM_SYNTH,
				hdd_ctx->config->enable_lprx, PDEV_CMD);
	if (ret) {
		hdd_err("Failed to set LPRx");
		goto error;
	}

	ret = sme_cli_set_command(
			adapter->sessionId,
			WMI_PDEV_PARAM_1CH_DTIM_OPTIMIZED_CHAIN_SELECTION,
			hdd_ctx->config->enable_dtim_selection_diversity,
			PDEV_CMD);
	if (ret) {
		hdd_err("Failed to set DTIM_OPTIMIZED_CHAIN_SELECTION");
		goto error;
	}

	ret = sme_cli_set_command(
			adapter->sessionId,
			WMI_PDEV_PARAM_TX_SCH_DELAY,
			hdd_ctx->config->enable_tx_sch_delay,
			PDEV_CMD);
	if (ret) {
		hdd_err("Failed to set WMI_PDEV_PARAM_TX_SCH_DELAY");
		goto error;
	}

	ret = sme_cli_set_command(
			adapter->sessionId,
			WMI_PDEV_PARAM_SECONDARY_RETRY_ENABLE,
			hdd_ctx->config->enable_secondary_rate,
			PDEV_CMD);
	if (ret) {
		hdd_err("Failed to set WMI_PDEV_PARAM_SECONDARY_RETRY_ENABLE");
		goto error;
	}

	if (adapter->device_mode == QDF_STA_MODE) {
		sme_set_smps_cfg(adapter->sessionId,
					HDD_STA_SMPS_PARAM_UPPER_BRSSI_THRESH,
					hdd_ctx->config->upper_brssi_thresh);

		sme_set_smps_cfg(adapter->sessionId,
					HDD_STA_SMPS_PARAM_LOWER_BRSSI_THRESH,
					hdd_ctx->config->lower_brssi_thresh);

		sme_set_smps_cfg(adapter->sessionId,
					HDD_STA_SMPS_PARAM_DTIM_1CHRX_ENABLE,
					hdd_ctx->config->enable_dtim_1chrx);
	}

	if (hdd_ctx->config->enable2x2) {
		hdd_debug("configuring 2x2 mode fw params");

		ret = sme_cli_set_command(adapter->sessionId,
				       WMI_PDEV_PARAM_ENABLE_CCK_TXFIR_OVERRIDE,
				    hdd_ctx->config->enable_cck_tx_fir_override,
					  PDEV_CMD);
		if (ret) {
			hdd_err("WMI_PDEV_PARAM_ENABLE_CCK_TXFIR_OVERRIDE set failed %d",
				ret);
			goto error;
		}
	} else {
#define HDD_DTIM_1CHAIN_RX_ID 0x5
#define HDD_SMPS_PARAM_VALUE_S 29
		hdd_debug("configuring 1x1 mode fw params");

		/*
		 * Disable DTIM 1 chain Rx when in 1x1,
		 * we are passing two value
		 * as param_id << 29 | param_value.
		 * Below param_value = 0(disable)
		 */
		ret = sme_cli_set_command(adapter->sessionId,
					  WMI_STA_SMPS_PARAM_CMDID,
					  HDD_DTIM_1CHAIN_RX_ID <<
					  HDD_SMPS_PARAM_VALUE_S,
					  VDEV_CMD);
		if (ret) {
			hdd_err("DTIM 1 chain set failed %d", ret);
			goto error;
		}

#undef HDD_DTIM_1CHAIN_RX_ID
#undef HDD_SMPS_PARAM_VALUE_S

		if (hdd_configure_chain_mask(adapter))
			goto error;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMI_PDEV_PARAM_HYST_EN,
				  hdd_ctx->config->enableMemDeepSleep,
				  PDEV_CMD);
	if (ret) {
		hdd_err("WMI_PDEV_PARAM_HYST_EN set failed %d", ret);
		goto error;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMI_VDEV_PARAM_ENABLE_RTSCTS,
				  hdd_ctx->config->rts_profile,
				  VDEV_CMD);
	if (ret) {
		hdd_err("FAILED TO SET RTSCTS Profile ret:%d", ret);
		goto error;
	}

	hdd_debug("SET AMSDU num %d", hdd_ctx->config->max_amsdu_num);
	ret = sme_cli_set_command(adapter->sessionId,
				  GEN_VDEV_PARAM_AMSDU,
				  hdd_ctx->config->max_amsdu_num,
				  GEN_CMD);
	if (ret) {
		hdd_err("GEN_VDEV_PARAM_AMSDU set failed %d", ret);
		goto error;
	}

	hdd_set_fw_log_params(hdd_ctx, adapter);
	EXIT();

	return 0;

error:
	return -EINVAL;
}

/**
 * hdd_attach_adapter() - attach the given adapter to the hdd context
 * @hdd_ctx: the hdd context to attach to
 * @adapter: the hdd adapter to attach
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS hdd_attach_adapter(hdd_context_t *hdd_ctx,
				     hdd_adapter_t *adapter)
{
	QDF_STATUS status;
	hdd_adapter_list_node_t *node;
	int i;

	node = NULL;
	for (i = 0; i < CSR_ROAM_SESSION_MAX; ++i) {
		if (hdd_ctx->adapter_nodes[i].pAdapter)
			continue;

		node = &hdd_ctx->adapter_nodes[i];
		break;
	}

	if (!node)
		return QDF_STATUS_E_RESOURCES;

	node->pAdapter = adapter;

	status = hdd_add_adapter_back(hdd_ctx, node);
	if (QDF_IS_STATUS_ERROR(status))
		node->pAdapter = NULL;

	return status;
}

/**
 * hdd_init_completion() - Initialize Completion Variables
 * @adapter: HDD adapter
 *
 * This function Initialize the completion variables for
 * a particular adapter
 *
 * Return: None
 */

static void hdd_init_completion(hdd_adapter_t *adapter)
{
	QDF_STATUS qdf_status;

	init_completion(&adapter->disconnect_comp_var);
	init_completion(&adapter->roaming_comp_var);
	init_completion(&adapter->linkup_event_var);
	init_completion(&adapter->cancel_rem_on_chan_var);
	init_completion(&adapter->rem_on_chan_ready_event);
	init_completion(&adapter->sta_authorized_event);
	init_completion(&adapter->offchannel_tx_event);
	init_completion(&adapter->tx_action_cnf_event);
	init_completion(&adapter->ibss_peer_info_comp);
	qdf_status = qdf_event_create(&adapter->change_country_code);
	if (QDF_IS_STATUS_ERROR(qdf_status)) {
		hdd_err("Change country code event init failed!");
	}
	init_completion(&adapter->scan_info.abortscan_event_var);
	init_completion(&adapter->lfr_fw_status.disable_lfr_event);

	hdd_tdls_init_completion(adapter);
}

/**
 * hdd_open_adapter() - open and setup the hdd adatper
 * @hdd_ctx: global hdd context
 * @session_type: type of the interface to be created
 * @iface_name: User-visible name of the interface
 * @macAddr: MAC address to assign to the interface
 * @name_assign_type: the name of assign type of the netdev
 * @rtnl_held: the rtnl lock hold flag
 *
 * This function open and setup the hdd adpater according to the device
 * type request, assign the name, the mac address assigned, and then prepared
 * the hdd related parameters, queue, lock and ready to start.
 *
 * Return: the pointer of hdd adapter, otherwise NULL.
 */
hdd_adapter_t *hdd_open_adapter(hdd_context_t *hdd_ctx, uint8_t session_type,
				const char *iface_name, tSirMacAddr macAddr,
				unsigned char name_assign_type,
				bool rtnl_held)
{
	hdd_adapter_t *adapter = NULL;
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	hdd_cfg80211_state_t *cfgState;

	if (hdd_ctx->current_intf_count >= hdd_ctx->max_intf_count) {
		/*
		 * Max limit reached on the number of vdevs configured by the
		 * host. Return error
		 */
		hdd_err("Unable to add virtual intf: currentVdevCnt=%d,hostConfiguredVdevCnt=%d",
			hdd_ctx->current_intf_count, hdd_ctx->max_intf_count);
		return NULL;
	}

	status = wlan_hdd_validate_mac_address((struct qdf_mac_addr *)macAddr);
	if (QDF_IS_STATUS_ERROR(status)) {
		/* Not received valid macAddr */
		hdd_err("Unable to add virtual intf: Not able to get valid mac address");
		return NULL;
	}

	status = hdd_check_for_existing_macaddr(hdd_ctx, macAddr);
	if (QDF_STATUS_E_FAILURE == status) {
		hdd_err("Duplicate MAC addr: " MAC_ADDRESS_STR
				" already exists",
				MAC_ADDR_ARRAY(macAddr));
		return NULL;
	}

	switch (session_type) {
	case QDF_STA_MODE:
		if (!hdd_ctx->config->mac_provision) {
			/* Reset locally administered bit if the device mode is
			 * STA
			 */
			WLAN_HDD_RESET_LOCALLY_ADMINISTERED_BIT(macAddr);
			hdd_debug("locally administered bit reset in sta mode: "
				 MAC_ADDRESS_STR, MAC_ADDR_ARRAY(macAddr));
			/*
			 * After resetting locally administered bit
			 * again check if the new mac address is already
			 * exists.
			 */
			status = hdd_check_for_existing_macaddr(hdd_ctx,
								macAddr);
			if (QDF_STATUS_E_FAILURE == status) {
				hdd_err("Duplicate MAC addr: " MAC_ADDRESS_STR
					" already exists",
					MAC_ADDR_ARRAY(macAddr));
				return NULL;
			}
		}
	/* fall through */
	case QDF_P2P_CLIENT_MODE:
	case QDF_P2P_DEVICE_MODE:
	case QDF_OCB_MODE:
	case QDF_NDI_MODE:
	case QDF_MONITOR_MODE:
		adapter = hdd_alloc_station_adapter(hdd_ctx, macAddr,
						    name_assign_type,
						    iface_name, session_type);

		if (NULL == adapter) {
			hdd_err("failed to allocate adapter for session %d",
					session_type);
			return NULL;
		}

		if (QDF_P2P_CLIENT_MODE == session_type)
			adapter->wdev.iftype = NL80211_IFTYPE_P2P_CLIENT;
		else if (QDF_P2P_DEVICE_MODE == session_type)
			adapter->wdev.iftype = NL80211_IFTYPE_P2P_DEVICE;
		else if (QDF_MONITOR_MODE == session_type)
			adapter->wdev.iftype = NL80211_IFTYPE_MONITOR;
		else
			adapter->wdev.iftype = NL80211_IFTYPE_STATION;

		adapter->device_mode = session_type;


		/* initialize action frame random mac info */
		hdd_adapter_init_action_frame_random_mac(adapter);

		/*
		 * Workqueue which gets scheduled in IPv4 notification
		 * callback
		 */
		INIT_WORK(&adapter->ipv4NotifierWorkQueue,
			  hdd_ipv4_notifier_work_queue);

#ifdef WLAN_NS_OFFLOAD
		/*
		 * Workqueue which gets scheduled in IPv6
		 * notification callback.
		 */
		INIT_WORK(&adapter->ipv6NotifierWorkQueue,
			  hdd_ipv6_notifier_work_queue);
#endif
		status = hdd_register_interface(adapter, rtnl_held);
		if (QDF_STATUS_SUCCESS != status) {
			hdd_deinit_adapter(hdd_ctx, adapter, rtnl_held);
			goto err_free_netdev;
		}

		/* Stop the Interface TX queue. */
		hdd_info("Disabling queues");
		wlan_hdd_netif_queue_control(adapter,
					WLAN_STOP_ALL_NETIF_QUEUE_N_CARRIER,
					WLAN_CONTROL_PATH);

		/* Initialize NAN Data Interface */
		if (QDF_NDI_MODE == session_type) {
			status = hdd_init_nan_data_mode(adapter);
			if (QDF_STATUS_SUCCESS != status)
				goto err_free_netdev;
		}

		break;

	case QDF_P2P_GO_MODE:
	case QDF_SAP_MODE:
		adapter = hdd_wlan_create_ap_dev(hdd_ctx, macAddr,
						 name_assign_type,
						 (uint8_t *) iface_name);
		if (NULL == adapter) {
			hdd_err("failed to allocate adapter for session %d",
					  session_type);
			return NULL;
		}

		adapter->wdev.iftype =
			(session_type ==
			 QDF_SAP_MODE) ? NL80211_IFTYPE_AP :
			NL80211_IFTYPE_P2P_GO;
		adapter->device_mode = session_type;

		status = hdd_register_interface(adapter, rtnl_held);
		if (QDF_STATUS_SUCCESS != status) {
			hdd_deinit_adapter(hdd_ctx, adapter, rtnl_held);
			goto err_free_netdev;
		}
		hdd_info("Disabling queues");
		wlan_hdd_netif_queue_control(adapter,
					WLAN_STOP_ALL_NETIF_QUEUE_N_CARRIER,
					WLAN_CONTROL_PATH);

		/*
		 * Workqueue which gets scheduled in IPv4 notification
		 * callback
		 */
		INIT_WORK(&adapter->ipv4NotifierWorkQueue,
			  hdd_ipv4_notifier_work_queue);

#ifdef WLAN_NS_OFFLOAD
		/*
		 * Workqueue which gets scheduled in IPv6
		 * notification callback.
		 */
		INIT_WORK(&adapter->ipv6NotifierWorkQueue,
			  hdd_ipv6_notifier_work_queue);
#endif
		break;
	case QDF_FTM_MODE:
		adapter = hdd_alloc_station_adapter(hdd_ctx, macAddr,
						    name_assign_type,
						    "wlan0", session_type);
		if (NULL == adapter) {
			hdd_err("Failed to allocate adapter for FTM mode");
			return NULL;
		}
		adapter->wdev.iftype = NL80211_IFTYPE_STATION;
		adapter->device_mode = session_type;
		status = hdd_register_interface(adapter, rtnl_held);
		if (QDF_STATUS_SUCCESS != status) {
			hdd_deinit_adapter(hdd_ctx, adapter, rtnl_held);
			goto err_free_netdev;
		}
		/* Stop the Interface TX queue. */
		hdd_info("Disabling queues");
		wlan_hdd_netif_queue_control(adapter,
					WLAN_STOP_ALL_NETIF_QUEUE_N_CARRIER,
					WLAN_CONTROL_PATH);
		break;
	default:
		hdd_err("Invalid session type %d", session_type);
		QDF_ASSERT(0);
		return NULL;
	}

	hdd_init_completion(adapter);
	INIT_WORK(&adapter->scan_block_work, wlan_hdd_cfg80211_scan_block_cb);
	qdf_list_create(&adapter->blocked_scan_request_q,
			CFG_MAX_SCAN_COUNT_MAX);
	qdf_mutex_create(&adapter->blocked_scan_request_q_lock);

	cfgState = WLAN_HDD_GET_CFG_STATE_PTR(adapter);
	mutex_init(&cfgState->remain_on_chan_ctx_lock);

	if (QDF_STATUS_SUCCESS == status)
		status = hdd_attach_adapter(hdd_ctx, adapter);

	if (QDF_STATUS_SUCCESS != status) {
		if (adapter) {
			hdd_cleanup_adapter(hdd_ctx, adapter, rtnl_held);
			adapter = NULL;
		}

		return NULL;
	}

	if (QDF_STATUS_SUCCESS == status) {
		cds_set_concurrency_mode(session_type);

		/* Adapter successfully added. Increment the vdev count */
		hdd_ctx->current_intf_count++;

		hdd_debug("current_intf_count=%d",
		       hdd_ctx->current_intf_count);

		cds_check_and_restart_sap_with_non_dfs_acs();
	}

	if (QDF_STATUS_SUCCESS != hdd_debugfs_init(adapter))
		hdd_err("Interface %s wow debug_fs init failed",
			netdev_name(adapter->dev));

	hdd_info("%s interface created. iftype: %d", netdev_name(adapter->dev),
		 session_type);

	qdf_mutex_create(&adapter->arp_offload_info_lock);
	hdd_ns_offload_info_lock_create(adapter);
	hdd_apf_context_init(adapter);

	if (adapter->device_mode == QDF_STA_MODE)
		wlan_hdd_debugfs_csr_init(adapter);

	return adapter;

err_free_netdev:
	wlan_hdd_release_intf_addr(hdd_ctx, adapter->macAddressCurrent.bytes);
	free_netdev(adapter->dev);

	return NULL;
}

QDF_STATUS hdd_close_adapter(hdd_context_t *hdd_ctx, hdd_adapter_t *adapter,
			     bool rtnl_held)
{
	hdd_adapter_list_node_t *adapterNode, *pCurrent, *pNext;
	QDF_STATUS status;

	hdd_info("enter(%s)", netdev_name(adapter->dev));

	status = hdd_get_front_adapter(hdd_ctx, &pCurrent);
	if (QDF_STATUS_SUCCESS != status) {
		hdd_warn("adapter list empty %d",
		       status);
		return status;
	}

	while (pCurrent->pAdapter != adapter) {
		status = hdd_get_next_adapter(hdd_ctx, pCurrent, &pNext);
		if (QDF_STATUS_SUCCESS != status)
			break;

		pCurrent = pNext;
	}
	adapterNode = pCurrent;
	if (QDF_STATUS_SUCCESS == status) {
		hdd_bus_bw_compute_timer_stop(hdd_ctx);

		/* cleanup adapter */
		cds_clear_concurrency_mode(adapter->device_mode);
		hdd_cleanup_adapter(hdd_ctx, adapterNode->pAdapter, rtnl_held);
		hdd_remove_adapter(hdd_ctx, adapterNode);
		adapterNode->pAdapter = NULL;
		adapterNode = NULL;

		/* conditionally restart the bw timer */
		hdd_bus_bw_compute_timer_try_start(hdd_ctx);

		/* Adapter removed. Decrement vdev count */
		if (hdd_ctx->current_intf_count != 0)
			hdd_ctx->current_intf_count--;

		/* Fw will take care incase of concurrency */
		return QDF_STATUS_SUCCESS;
	}

	EXIT();
	return QDF_STATUS_E_FAILURE;
}

/**
 * hdd_close_all_adapters - Close all open adapters
 * @hdd_ctx:	Hdd context
 * rtnl_held:	True if RTNL lock held
 *
 * Close all open adapters.
 *
 * Return: QDF status code
 */
QDF_STATUS hdd_close_all_adapters(hdd_context_t *hdd_ctx, bool rtnl_held)
{
	hdd_adapter_list_node_t *pHddAdapterNode;
	QDF_STATUS status;

	ENTER();

	do {
		status = hdd_remove_front_adapter(hdd_ctx, &pHddAdapterNode);
		if (pHddAdapterNode && QDF_STATUS_SUCCESS == status) {
			wlan_hdd_release_intf_addr(hdd_ctx,
			pHddAdapterNode->pAdapter->macAddressCurrent.bytes);
			cds_clear_concurrency_mode(
				pHddAdapterNode->pAdapter->device_mode);
			hdd_cleanup_adapter(hdd_ctx, pHddAdapterNode->pAdapter,
					    rtnl_held);

			pHddAdapterNode->pAdapter = NULL;

			/* Adapter removed. Decrement vdev count */
			if (hdd_ctx->current_intf_count != 0)
				hdd_ctx->current_intf_count--;
		}
	} while (NULL != pHddAdapterNode && QDF_STATUS_E_EMPTY != status);

	EXIT();

	return QDF_STATUS_SUCCESS;
}

void wlan_hdd_reset_prob_rspies(hdd_adapter_t *pHostapdAdapter)
{
	struct qdf_mac_addr *bssid = NULL;
	tSirUpdateIE updateIE;

	switch (pHostapdAdapter->device_mode) {
	case QDF_STA_MODE:
	case QDF_P2P_CLIENT_MODE:
	{
		hdd_station_ctx_t *pHddStaCtx =
			WLAN_HDD_GET_STATION_CTX_PTR(pHostapdAdapter);
		bssid = &pHddStaCtx->conn_info.bssId;
		break;
	}
	case QDF_SAP_MODE:
	case QDF_P2P_GO_MODE:
	case QDF_IBSS_MODE:
	{
		bssid = &pHostapdAdapter->macAddressCurrent;
		break;
	}
	case QDF_FTM_MODE:
	case QDF_P2P_DEVICE_MODE:
	default:
		/*
		 * wlan_hdd_reset_prob_rspies should not have been called
		 * for these kind of devices
		 */
		hdd_err("Unexpected request for the current device type %d",
		       pHostapdAdapter->device_mode);
		return;
	}

	qdf_copy_macaddr(&updateIE.bssid, bssid);
	updateIE.smeSessionId = pHostapdAdapter->sessionId;
	updateIE.ieBufferlength = 0;
	updateIE.pAdditionIEBuffer = NULL;
	updateIE.append = true;
	updateIE.notify = false;
	if (sme_update_add_ie(WLAN_HDD_GET_HAL_CTX(pHostapdAdapter),
			      &updateIE,
			      eUPDATE_IE_PROBE_RESP) == QDF_STATUS_E_FAILURE) {
		hdd_err("Could not pass on PROBE_RSP_BCN data to PE");
	}
}

/**
 * hdd_wait_for_sme_close_sesion() - Close and wait for SME session close
 * @hdd_ctx: HDD context which is already NULL validated
 * @adapter: HDD adapter which is already NULL validated
 * @flush_all_sme_cmds: whether all commands needs to be flushed
 *
 * Close the SME session and wait for its completion, if needed.
 *
 * Return: None
 */
static void hdd_wait_for_sme_close_sesion(hdd_context_t *hdd_ctx,
					hdd_adapter_t *adapter,
					bool flush_all_sme_cmds)
{
	QDF_STATUS status = QDF_STATUS_E_FAILURE;

	if (!test_bit(SME_SESSION_OPENED, &adapter->event_flags)) {
		hdd_err("session is not opened:%d", adapter->sessionId);
		return;
	}

	status = qdf_event_reset(&adapter->qdf_session_close_event);
	if (QDF_STATUS_SUCCESS != status) {
		hdd_err("failed to reinit session_close QDF event");
		return;
	}
	if (QDF_STATUS_SUCCESS ==
			sme_close_session(hdd_ctx->hHal, adapter->sessionId,
				flush_all_sme_cmds,
				hdd_sme_close_session_callback,
				adapter)) {
		/*
		 * Block on a completion variable. Can't wait
		 * forever though.
		 */
		status = qdf_wait_for_event_completion(
				&adapter->qdf_session_close_event,
				WLAN_WAIT_TIME_SESSIONOPENCLOSE);
		if (QDF_STATUS_SUCCESS != status) {
			hdd_err("failure waiting for session_close QDF event");
			if (adapter->device_mode == QDF_NDI_MODE)
				hdd_ndp_session_end_handler(adapter);
			sme_print_commands(hdd_ctx->hHal);
			clear_bit(SME_SESSION_OPENED, &adapter->event_flags);
		}
		adapter->sessionId = HDD_SESSION_ID_INVALID;
	}
}

/**
 * hdd_roc_req_q_flush() - Flush the RoC request queue for a given adapter
 * @hdd_ctx: the global HDD context
 * @adapter: the adapter for whose RoC requests should be flushed
 *
 * Pop each RoC request from the queue. If the request's adapter matches the
 * given adapter, free the request. Otherwise, requeue the request at the back
 * of the queue. Continue until queue size number of requests have been
 * processed.
 *
 * Return: None
 */
static void hdd_roc_req_q_flush(hdd_context_t *hdd_ctx, hdd_adapter_t *adapter)
{
	qdf_list_t *req_q = &hdd_ctx->hdd_roc_req_q;
	qdf_list_node_t *node;
	hdd_roc_req_t *req;
	uint32_t count;

	hdd_debug("Flushing RoC queue for adapter %u", adapter->sessionId);

	flush_delayed_work(&hdd_ctx->roc_req_work);

	qdf_spin_lock(&hdd_ctx->hdd_roc_req_q_lock);
	for (count = qdf_list_size(req_q); count > 0; count--) {
		if (QDF_IS_STATUS_ERROR(qdf_list_remove_front(req_q, &node))) {
			qdf_spin_unlock(&hdd_ctx->hdd_roc_req_q_lock);

			hdd_err("Unexpected number of requests in RoC queue");
			QDF_BUG(0);

			return;
		}

		req = qdf_container_of(node, hdd_roc_req_t, node);
		if (req->pAdapter != adapter) {
			qdf_list_insert_back(req_q, node);
			continue;
		}

		if (req->pRemainChanCtx)
			qdf_mem_free(req->pRemainChanCtx);

		qdf_mem_free(req);
	}
	qdf_spin_unlock(&hdd_ctx->hdd_roc_req_q_lock);
}

QDF_STATUS hdd_stop_adapter(hdd_context_t *hdd_ctx, hdd_adapter_t *adapter,
			    const bool bCloseSession)
{
	QDF_STATUS qdf_ret_status = QDF_STATUS_SUCCESS;
	hdd_wext_state_t *pWextState = WLAN_HDD_GET_WEXT_STATE_PTR(adapter);
	union iwreq_data wrqu;
	tSirUpdateIE updateIE;
	unsigned long rc;
	hdd_scaninfo_t *scan_info = NULL;
	void *sap_ctx;
	tsap_Config_t *sap_config;

	hdd_info("enter(%s)", netdev_name(adapter->dev));

	if (!test_bit(SME_SESSION_OPENED, &adapter->event_flags)) {
		hdd_err("session %d is not open %lu",
			adapter->sessionId, adapter->event_flags);
		return -ENODEV;
	}

	/* Need to clean up blocked scan request */
	wlan_hdd_cfg80211_scan_block_cb(&adapter->scan_block_work);
	scan_info = &adapter->scan_info;
	hdd_info("Disabling queues");
	wlan_hdd_netif_queue_control(adapter,
				     WLAN_STOP_ALL_NETIF_QUEUE_N_CARRIER,
				     WLAN_CONTROL_PATH);
	switch (adapter->device_mode) {
	case QDF_STA_MODE:
	case QDF_P2P_CLIENT_MODE:
	case QDF_IBSS_MODE:
	case QDF_P2P_DEVICE_MODE:
	case QDF_NDI_MODE:

		if ((QDF_NDI_MODE == adapter->device_mode) ||
			hdd_conn_is_connected(
				WLAN_HDD_GET_STATION_CTX_PTR(adapter)) ||
			hdd_is_connecting(
				WLAN_HDD_GET_STATION_CTX_PTR(adapter))) {
			INIT_COMPLETION(adapter->disconnect_comp_var);
			/*
			 * For NDI do not use pWextState from sta_ctx, if needed
			 * extract from ndi_ctx.
			 */
			if (QDF_NDI_MODE == adapter->device_mode)
				qdf_ret_status = sme_roam_disconnect(
					hdd_ctx->hHal,
					adapter->sessionId,
					eCSR_DISCONNECT_REASON_NDI_DELETE);
			else if (pWextState->roamProfile.BSSType ==
						eCSR_BSS_TYPE_START_IBSS)
				qdf_ret_status = sme_roam_disconnect(
					hdd_ctx->hHal,
					adapter->sessionId,
					eCSR_DISCONNECT_REASON_IBSS_LEAVE);
			else if (QDF_STA_MODE == adapter->device_mode)
				 wlan_hdd_disconnect(adapter,
						eCSR_DISCONNECT_REASON_DEAUTH);
			else
				qdf_ret_status = sme_roam_disconnect(
					hdd_ctx->hHal,
					adapter->sessionId,
					eCSR_DISCONNECT_REASON_UNSPECIFIED);
			/* success implies disconnect command got
			 * queued up successfully
			 */
			if (qdf_ret_status == QDF_STATUS_SUCCESS &&
					QDF_STA_MODE != adapter->device_mode) {
				rc = wait_for_completion_timeout(
					&adapter->disconnect_comp_var,
					msecs_to_jiffies
						(WLAN_WAIT_TIME_DISCONNECT));
				if (!rc)
					hdd_warn("wait on disconnect_comp_var failed");
			}
			if (qdf_ret_status != QDF_STATUS_SUCCESS)
				hdd_warn("failed to post disconnect");
			memset(&wrqu, '\0', sizeof(wrqu));
			wrqu.ap_addr.sa_family = ARPHRD_ETHER;
			memset(wrqu.ap_addr.sa_data, '\0', ETH_ALEN);
			wireless_send_event(adapter->dev, SIOCGIWAP, &wrqu,
					    NULL);
		}
		if (scan_info != NULL && scan_info->mScanPending)
			wlan_hdd_scan_abort(adapter);

		hdd_lro_disable(hdd_ctx, adapter);
		wlan_hdd_tdls_exit(adapter);
		wlan_hdd_cleanup_remain_on_channel_ctx(adapter);
		hdd_clear_fils_connection_info(adapter);
		qdf_ret_status = sme_roam_del_pmkid_from_cache(
							hdd_ctx->hHal,
							adapter->sessionId,
							NULL, true);
		if (QDF_IS_STATUS_ERROR(qdf_ret_status))
			hdd_err("Cannot flush PMKIDCache");

#ifdef WLAN_OPEN_SOURCE
		cancel_work_sync(&adapter->ipv4NotifierWorkQueue);
#endif

		hdd_deregister_tx_flow_control(adapter);

#ifdef WLAN_NS_OFFLOAD
#ifdef WLAN_OPEN_SOURCE
		cancel_work_sync(&adapter->ipv6NotifierWorkQueue);
#endif
#endif

		/*
		 * During vdev destroy, if any STA is in connecting state the
		 * roam command will be in active queue and thus vdev destroy is
		 * queued in pending queue. In case STA tries to connect to
		 * multiple BSSID and fails to connect, due to auth/assoc
		 * timeouts it may take more than vdev destroy time to get
		 * completed. So before vdev destroy is queued abort any STA
		 * ongoing connection to avoid vdev destroy timeout.
		 */
		if (test_bit(SME_SESSION_OPENED, &adapter->event_flags))
			hdd_abort_ongoing_sta_connection(hdd_ctx);
		/*
		 * It is possible that the caller of this function does not
		 * wish to close the session
		 */
		if (true == bCloseSession)
			hdd_wait_for_sme_close_sesion(hdd_ctx, adapter, false);
		break;

	case QDF_MONITOR_MODE:
		wlan_hdd_scan_abort(adapter);
		hdd_deregister_tx_flow_control(adapter);

		/*
		 * It is possible that the caller of this function does not
		 * wish to close the session
		 */
		if (bCloseSession)
			hdd_wait_for_sme_close_sesion(hdd_ctx, adapter, true);
		break;

	case QDF_SAP_MODE:
		if (test_bit(ACS_PENDING, &adapter->event_flags)) {
			cds_flush_delayed_work(&adapter->acs_pending_work);
			clear_bit(ACS_PENDING, &adapter->event_flags);
		}

		/* Diassociate with all the peers before stop ap post */
		if (test_bit(SOFTAP_BSS_STARTED, &adapter->event_flags))
			wlan_hdd_del_station(adapter);

		hdd_ipa_flush(hdd_ctx);
		qdf_flush_work(&hdd_ctx->sap_pre_cac_work);

	case QDF_P2P_GO_MODE:
		if (hdd_ctx->config->conc_custom_rule1 &&
			(QDF_SAP_MODE == adapter->device_mode)) {
			/*
			 * Before stopping the sap adapter, lets make sure there
			 * is no sap restart work pending.
			 */
			cds_flush_work(&hdd_ctx->sap_start_work);
			hdd_debug("Canceled the pending SAP restart work");
			cds_change_sap_restart_required_status(false);
		}
		cds_flush_work(&adapter->sap_stop_bss_work);

		/* Any softap specific cleanup here... */
		sap_config = &adapter->sessionCtx.ap.sapConfig;
		wlansap_reset_sap_config_add_ie(sap_config, eUPDATE_IE_ALL);
		qdf_atomic_set(&adapter->sessionCtx.ap.acs_in_progress, 0);
		wlan_hdd_undo_acs(adapter);
		if (adapter->device_mode == QDF_P2P_GO_MODE)
			wlan_hdd_cleanup_remain_on_channel_ctx(adapter);

		hdd_deregister_tx_flow_control(adapter);

		/*
		 * During vdev destroy, if any STA is in connecting state the
		 * roam command will be in active queue and thus vdev destroy is
		 * queued in pending queue. In case STA tries to connect to
		 * multiple BSSID and fails to connect, due to auth/assoc
		 * timeouts it may take more than vdev destroy time to get
		 * complete. So before vdev destroy is queued abort any STA
		 * ongoing connection to avoid vdev destroy timeout.
		 */
		if (test_bit(SME_SESSION_OPENED, &adapter->event_flags))
			hdd_abort_ongoing_sta_connection(hdd_ctx);

		mutex_lock(&hdd_ctx->sap_lock);
		if (test_bit(SOFTAP_BSS_STARTED, &adapter->event_flags)) {
			QDF_STATUS status;
			QDF_STATUS qdf_status;

			/* Stop Bss. */
			status = wlansap_stop_bss(
					WLAN_HDD_GET_SAP_CTX_PTR(adapter));

			if (QDF_IS_STATUS_SUCCESS(status)) {
				hdd_hostapd_state_t *hostapd_state =
					WLAN_HDD_GET_HOSTAP_STATE_PTR(adapter);
				qdf_event_reset(&hostapd_state->
						qdf_stop_bss_event);
				qdf_status =
					qdf_wait_for_event_completion(
					&hostapd_state->qdf_stop_bss_event,
					SME_CMD_TIMEOUT_VALUE);

				if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
					hdd_err("failure waiting for wlansap_stop_bss %d",
						qdf_status);
				}
			} else {
				hdd_err("failure in wlansap_stop_bss");
			}
			clear_bit(SOFTAP_BSS_STARTED, &adapter->event_flags);
			cds_decr_session_set_pcl(
						     adapter->device_mode,
							adapter->sessionId);

			qdf_copy_macaddr(&updateIE.bssid,
					 &adapter->macAddressCurrent);
			updateIE.smeSessionId = adapter->sessionId;
			updateIE.ieBufferlength = 0;
			updateIE.pAdditionIEBuffer = NULL;
			updateIE.append = false;
			updateIE.notify = false;
			/* Probe bcn reset */
			if (sme_update_add_ie(WLAN_HDD_GET_HAL_CTX(adapter),
					      &updateIE, eUPDATE_IE_PROBE_BCN)
			    == QDF_STATUS_E_FAILURE) {
				hdd_err("Could not pass on PROBE_RSP_BCN data to PE");
			}
			/* Assoc resp reset */
			if (sme_update_add_ie(WLAN_HDD_GET_HAL_CTX(adapter),
					      &updateIE,
					      eUPDATE_IE_ASSOC_RESP) ==
			    QDF_STATUS_E_FAILURE) {
				hdd_err("Could not pass on ASSOC_RSP data to PE");
			}
			/* Reset WNI_CFG_PROBE_RSP Flags */
			wlan_hdd_reset_prob_rspies(adapter);
		}
		clear_bit(SOFTAP_INIT_DONE, &adapter->event_flags);
		qdf_mem_free(adapter->sessionCtx.ap.beacon);
		adapter->sessionCtx.ap.beacon = NULL;
		if (true == bCloseSession)
			hdd_wait_for_sme_close_sesion(hdd_ctx, adapter, true);

		sap_ctx = WLAN_HDD_GET_SAP_CTX_PTR(adapter);
		if (wlansap_stop(sap_ctx) != QDF_STATUS_SUCCESS)
			hdd_err("Failed:wlansap_stop");

		if (wlansap_close(sap_ctx) != QDF_STATUS_SUCCESS)
			hdd_err("Failed:WLANSAP_close");
		clear_bit(SME_SESSION_OPENED, &adapter->event_flags);
		adapter->sessionCtx.ap.sapContext = NULL;
		mutex_unlock(&hdd_ctx->sap_lock);

		wlan_hdd_check_conc_and_update_tdls_state(hdd_ctx, false);
#ifdef WLAN_OPEN_SOURCE
		cancel_work_sync(&adapter->ipv4NotifierWorkQueue);
#endif

#ifdef WLAN_NS_OFFLOAD
#ifdef WLAN_OPEN_SOURCE
		cancel_work_sync(&adapter->ipv6NotifierWorkQueue);
#endif
#endif
		break;
	case QDF_OCB_MODE:
		ol_txrx_clear_peer(WLAN_HDD_GET_STATION_CTX_PTR(adapter)->
			conn_info.staId[0]);
		break;
	default:
		break;
	}

	if (adapter->scan_info.default_scan_ies) {
		qdf_mem_free(adapter->scan_info.default_scan_ies);
		adapter->scan_info.default_scan_ies = NULL;
	}

	hdd_roc_req_q_flush(hdd_ctx, adapter);

	EXIT();
	return QDF_STATUS_SUCCESS;
}

/**
 * hdd_deinit_all_adapters - deinit all adapters
 * @hdd_ctx:   HDD context
 * @rtnl_held: True if RTNL lock held
 *
 */
void  hdd_deinit_all_adapters(hdd_context_t *hdd_ctx, bool rtnl_held)
{
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;

	ENTER();

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);

	while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
		adapter = adapter_node->pAdapter;
		hdd_deinit_adapter(hdd_ctx, adapter, rtnl_held);
		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}

	EXIT();
}

QDF_STATUS hdd_stop_all_adapters(hdd_context_t *hdd_ctx, bool close_session)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;

	ENTER();

	qdf_flush_work(&hdd_ctx->sap_pre_cac_work);
	cds_flush_sta_ap_intf_work(hdd_ctx);

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);

	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;
		hdd_stop_adapter(hdd_ctx, adapter, close_session);
		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}

	EXIT();

	return QDF_STATUS_SUCCESS;
}

#ifdef QCA_LL_LEGACY_TX_FLOW_CONTROL
/**
 * hdd_adapter_abort_tx_flow() - Abort the tx flow control
 * @pAdapter: pointer to hdd_adapter_t
 *
 * Resume tx and stop the tx flow control timer if the tx is paused and the flow
 * control timer is running. This function is called by SSR to avoid the
 * inconsistency of tx status before and after SSR.
 *
 * Return: void
 */
static void hdd_adapter_abort_tx_flow(hdd_adapter_t *adapter)
{
	if ((adapter->hdd_stats.hddTxRxStats.is_txflow_paused == TRUE) &&
		(QDF_TIMER_STATE_RUNNING ==
		qdf_mc_timer_get_current_state(&adapter->tx_flow_control_timer))) {
		hdd_tx_resume_timer_expired_handler(adapter);
		qdf_mc_timer_stop(&adapter->tx_flow_control_timer);
	}
}
#else
static void hdd_adapter_abort_tx_flow(hdd_adapter_t *pAdapter)
{
	return;
}
#endif

QDF_STATUS hdd_reset_all_adapters(hdd_context_t *hdd_ctx)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;

	ENTER();

	qdf_flush_work(&hdd_ctx->sap_pre_cac_work);
	cds_flush_sta_ap_intf_work(hdd_ctx);

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);

	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;

		hdd_adapter_abort_tx_flow(adapter);

		if ((adapter->device_mode == QDF_STA_MODE) ||
		    (adapter->device_mode == QDF_P2P_CLIENT_MODE)) {
			/* Stop tdls timers */
			hdd_tdls_timers_stop(adapter);
			adapter->sessionCtx.station.hdd_ReassocScenario = false;
		}

		hdd_info("Disabling queues");
		hdd_cleanup_actionframe(hdd_ctx, adapter);
		if (hdd_ctx->config->sap_internal_restart &&
		    adapter->device_mode == QDF_SAP_MODE) {
			wlan_hdd_netif_queue_control(adapter,
						     WLAN_STOP_ALL_NETIF_QUEUE,
						     WLAN_CONTROL_PATH);
			if (test_bit(SOFTAP_BSS_STARTED,
				     &adapter->event_flags))
				hdd_sap_indicate_disconnect_for_sta(adapter);
			if (test_bit(DEVICE_IFACE_OPENED,
				     &adapter->event_flags))
				hdd_sap_destroy_events(adapter);
			clear_bit(SOFTAP_BSS_STARTED, &adapter->event_flags);
		} else {
			wlan_hdd_netif_queue_control(adapter,
					   WLAN_STOP_ALL_NETIF_QUEUE_N_CARRIER,
					   WLAN_CONTROL_PATH);
		}

		/* Cleanup pending roc request */
		wlan_hdd_cleanup_remain_on_channel_ctx(adapter);

		hdd_deinit_tx_rx(adapter);
		hdd_lro_disable(hdd_ctx, adapter);

		if (adapter->device_mode == QDF_STA_MODE)
			hdd_clear_fils_connection_info(adapter);

		cds_decr_session_set_pcl(adapter->device_mode,
						adapter->sessionId);
		if (test_bit(WMM_INIT_DONE, &adapter->event_flags)) {
			hdd_wmm_adapter_close(adapter);
			clear_bit(WMM_INIT_DONE, &adapter->event_flags);
		}

		if (adapter->device_mode == QDF_SAP_MODE) {
			/*
			 * If adapter is SAP, set session ID to invalid
			 * since SAP session will be cleanup during SSR.
			 */
			wlansap_set_invalid_session(
				WLAN_HDD_GET_SAP_CTX_PTR(adapter));

			wlansap_cleanup_cac_timer(
				WLAN_HDD_GET_SAP_CTX_PTR(adapter));
		}

		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}

	EXIT();

	return QDF_STATUS_SUCCESS;
}

bool hdd_check_for_opened_interfaces(hdd_context_t *hdd_ctx)
{
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	QDF_STATUS status;
	bool close_modules = true;

	if (QDF_GLOBAL_FTM_MODE == hdd_get_conparam()) {
		hdd_info("FTM mode, don't close the module");
		return false;
	}

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);
	while ((NULL != adapter_node) && (QDF_STATUS_SUCCESS == status)) {
		if (test_bit(DEVICE_IFACE_OPENED,
		    &adapter_node->pAdapter->event_flags) ||
		    test_bit(SME_SESSION_OPENED,
		    &adapter_node->pAdapter->event_flags)) {
			hdd_debug("Still other ifaces are up cannot close modules");
			close_modules = false;
			break;
		}
		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}

	return close_modules;
}

bool hdd_is_interface_up(hdd_adapter_t *adapter)
{
	if (test_bit(DEVICE_IFACE_OPENED, &adapter->event_flags))
		return true;
	else
		return false;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)) \
	&& !defined(WITH_BACKPORTS) && !defined(IEEE80211_PRIVACY)
struct cfg80211_bss *hdd_cfg80211_get_bss(struct wiphy *wiphy,
					  struct ieee80211_channel *channel,
					  const u8 *bssid, const u8 *ssid,
					  size_t ssid_len)
{
	return cfg80211_get_bss(wiphy, channel, bssid,
				ssid, ssid_len,
				WLAN_CAPABILITY_ESS,
				WLAN_CAPABILITY_ESS);
}
#else
struct cfg80211_bss *hdd_cfg80211_get_bss(struct wiphy *wiphy,
					  struct ieee80211_channel *channel,
					  const u8 *bssid, const u8 *ssid,
					  size_t ssid_len)
{
	return cfg80211_get_bss(wiphy, channel, bssid,
				ssid, ssid_len,
				IEEE80211_BSS_TYPE_ESS,
				IEEE80211_PRIVACY_ANY);
}
#endif

#if defined CFG80211_CONNECT_BSS || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0))
#if defined CFG80211_CONNECT_TIMEOUT_REASON_CODE || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0))
/**
 * hdd_convert_timeout_reason() - Convert to kernel specific enum
 * @timeout_reason: reason for connect timeout
 *
 * This function is used to convert host timeout
 * reason enum to kernel specific enum.
 *
 * Return: nl timeout enum
 */
static enum nl80211_timeout_reason hdd_convert_timeout_reason(
						tSirResultCodes timeout_reason)
{
	switch (timeout_reason) {
	case eSIR_SME_JOIN_TIMEOUT_RESULT_CODE:
		return NL80211_TIMEOUT_SCAN;
	case eSIR_SME_AUTH_TIMEOUT_RESULT_CODE:
		return NL80211_TIMEOUT_AUTH;
	case eSIR_SME_ASSOC_TIMEOUT_RESULT_CODE:
		return NL80211_TIMEOUT_ASSOC;
	default:
		return NL80211_TIMEOUT_UNSPECIFIED;
	}
}

/**
 * hdd_cfg80211_connect_timeout() - API to send connection timeout reason
 * @dev: network device
 * @bssid: bssid to which we want to associate
 * @timeout_reason: reason for connect timeout
 *
 * This API is used to send connection timeout reason to supplicant
 *
 * Return: void
 */
static void hdd_cfg80211_connect_timeout(struct net_device *dev,
					 const u8 *bssid,
					 tSirResultCodes timeout_reason)
{
	enum nl80211_timeout_reason nl_timeout_reason;

	nl_timeout_reason = hdd_convert_timeout_reason(timeout_reason);

	cfg80211_connect_timeout(dev, bssid, NULL, 0, GFP_KERNEL,
				 nl_timeout_reason);
}

/**
 * __hdd_connect_bss() - API to send connection status to supplicant
 * @dev: network device
 * @bssid: bssid to which we want to associate
 * @req_ie: Request Information Element
 * @req_ie_len: len of the req IE
 * @resp_ie: Response IE
 * @resp_ie_len: len of ht response IE
 * @status: status
 * @gfp: Kernel Flag
 * @timeout_reason: reason for connect timeout
 *
 * Return: void
 */
static void __hdd_connect_bss(struct net_device *dev, const u8 *bssid,
			      struct cfg80211_bss *bss, const u8 *req_ie,
			      size_t req_ie_len, const u8 *resp_ie,
			      size_t resp_ie_len, int status, gfp_t gfp,
			      tSirResultCodes timeout_reason)
{
	enum nl80211_timeout_reason nl_timeout_reason;

	nl_timeout_reason = hdd_convert_timeout_reason(timeout_reason);

	cfg80211_connect_bss(dev, bssid, bss, req_ie, req_ie_len,
			     resp_ie, resp_ie_len, status, gfp,
			     nl_timeout_reason);
}
#else
#if defined CFG80211_CONNECT_TIMEOUT || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0))
static void hdd_cfg80211_connect_timeout(struct net_device *dev,
					 const u8 *bssid,
					 tSirResultCodes timeout_reason)
{
	cfg80211_connect_timeout(dev, bssid, NULL, 0, GFP_KERNEL);
}
#endif

static void __hdd_connect_bss(struct net_device *dev, const u8 *bssid,
			      struct cfg80211_bss *bss, const u8 *req_ie,
			      size_t req_ie_len, const u8 *resp_ie,
			      size_t resp_ie_len, int status, gfp_t gfp,
			      tSirResultCodes timeout_reason)
{
	cfg80211_connect_bss(dev, bssid, bss, req_ie, req_ie_len,
			     resp_ie, resp_ie_len, status, gfp);
}
#endif

/**
 * hdd_connect_bss() - API to send connection status to supplicant
 * @dev: network device
 * @bssid: bssid to which we want to associate
 * @req_ie: Request Information Element
 * @req_ie_len: len of the req IE
 * @resp_ie: Response IE
 * @resp_ie_len: len of ht response IE
 * @status: status
 * @gfp: Kernel Flag
 * @connect_timeout: If timed out waiting for Auth/Assoc/Probe resp
 * @timeout_reason: reason for connect timeout
 *
 * The API is a wrapper to send connection status to supplicant
 *
 * Return: Void
 */
#if defined CFG80211_CONNECT_TIMEOUT || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0))
static void hdd_connect_bss(struct net_device *dev, const u8 *bssid,
			struct cfg80211_bss *bss, const u8 *req_ie,
			size_t req_ie_len, const u8 *resp_ie,
			size_t resp_ie_len, int status, gfp_t gfp,
			bool connect_timeout,
			tSirResultCodes timeout_reason)
{
	if (connect_timeout)
		hdd_cfg80211_connect_timeout(dev, bssid, timeout_reason);
	else
		__hdd_connect_bss(dev, bssid, bss, req_ie, req_ie_len, resp_ie,
				  resp_ie_len, status, gfp, timeout_reason);
}
#else
static void hdd_connect_bss(struct net_device *dev, const u8 *bssid,
			struct cfg80211_bss *bss, const u8 *req_ie,
			size_t req_ie_len, const u8 *resp_ie,
			size_t resp_ie_len, int status, gfp_t gfp,
			bool connect_timeout,
			tSirResultCodes timeout_reason)
{
	__hdd_connect_bss(dev, bssid, bss, req_ie, req_ie_len, resp_ie,
			  resp_ie_len, status, gfp, timeout_reason);
}
#endif

#if defined(WLAN_FEATURE_FILS_SK)
#if defined(CFG80211_CONNECT_DONE) || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0))
#if defined(CFG80211_FILS_SK_OFFLOAD_SUPPORT) || \
		 (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0))
/**
 * hdd_populate_fils_params() - Populate FILS keys to connect response
 * @fils_params: connect response to supplicant
 * @fils_kek: FILS kek
 * @fils_kek_len: FILS kek length
 * @pmk: FILS PMK
 * @pmk_len: FILS PMK length
 * @pmkid: PMKID
 * @fils_seq_num: FILS Seq number
 *
 * Return: None
 */
static void hdd_populate_fils_params(struct cfg80211_connect_resp_params
				     *fils_params, const uint8_t *fils_kek,
				     size_t fils_kek_len, const uint8_t *pmk,
				     size_t pmk_len, const uint8_t *pmkid,
				     uint16_t fils_seq_num)
{
	/* Increament seq number to be used for next FILS */
	fils_params->fils_erp_next_seq_num = fils_seq_num + 1;
	fils_params->update_erp_next_seq_num = true;
	fils_params->fils_kek = fils_kek;
	fils_params->fils_kek_len = fils_kek_len;
	fils_params->pmk = pmk;
	fils_params->pmk_len = pmk_len;
	fils_params->pmkid = pmkid;
}
#else
static inline void hdd_populate_fils_params(struct cfg80211_connect_resp_params
					    *fils_params, const uint8_t
					    *fils_kek, size_t fils_kek_len,
					    const uint8_t *pmk, size_t pmk_len,
					    const uint8_t *pmkid,
					    uint16_t fils_seq_num)
{ }
#endif

void hdd_update_hlp_info(struct net_device *dev, tCsrRoamInfo *roam_info)
{
	struct sk_buff *skb;
	uint16_t skb_len;
	struct llc_snap_hdr_t *llc_hdr;
	QDF_STATUS status;
	uint8_t *hlp_data;
	uint16_t hlp_data_len;
	struct fils_join_rsp_params *roam_fils_params
				= roam_info->fils_join_rsp;
	hdd_adapter_t *padapter = WLAN_HDD_GET_PRIV_PTR(dev);

	if (!roam_fils_params)
		return;

	if (!roam_fils_params->hlp_data_len)
		return;

	hlp_data = roam_fils_params->hlp_data;
	hlp_data_len = roam_fils_params->hlp_data_len;

	/* Calculate skb length */
	skb_len = (2 * ETH_ALEN) + hlp_data_len;
	skb = qdf_nbuf_alloc(NULL, skb_len, 0, 4, false);
	if (skb == NULL) {
		hdd_err("HLP packet nbuf alloc fails");
		return;
	}

	qdf_mem_copy(skb_put(skb, ETH_ALEN), roam_fils_params->dst_mac.bytes,
				 QDF_MAC_ADDR_SIZE);
	qdf_mem_copy(skb_put(skb, ETH_ALEN), roam_fils_params->src_mac.bytes,
				 QDF_MAC_ADDR_SIZE);

	llc_hdr = (struct llc_snap_hdr_t *) hlp_data;
	if (IS_SNAP(llc_hdr)) {
		hlp_data += LLC_SNAP_HDR_OFFSET_ETHERTYPE;
		hlp_data_len += LLC_SNAP_HDR_OFFSET_ETHERTYPE;
	}

	qdf_mem_copy(skb_put(skb, hlp_data_len), hlp_data, hlp_data_len);

	/*
	 * This HLP packet is formed from HLP info encapsulated
	 * in assoc response frame which is AEAD encrypted.
	 * Hence, this checksum validation can be set unnecessary.
	 * i.e. network layer need not worry about checksum.
	 */
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	status = hdd_rx_packet_cbk(padapter, skb);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("Sending HLP packet fails");
		return;
	}
	hdd_debug("send HLP packet to netif successfully");
}

/**
 * hdd_connect_done() - Wrapper API to call cfg80211_connect_done
 * @dev: network device
 * @bssid: bssid to which we want to associate
 * @bss: cfg80211 bss info
 * @roam_info: information about connected bss
 * @req_ie: Request Information Element
 * @req_ie_len: len of the req IE
 * @resp_ie: Response IE
 * @resp_ie_len: len of ht response IE
 * @status: status
 * @gfp: allocation flags
 * @connect_timeout: If timed out waiting for Auth/Assoc/Probe resp
 * @timeout_reason: reason for connect timeout
 *
 * This API is used as wrapper to send FILS key/sequence number
 * params etc. to supplicant in case of FILS connection
 *
 * Return: None
 */
static void hdd_connect_done(struct net_device *dev, const u8 *bssid,
			     struct cfg80211_bss *bss, tCsrRoamInfo *roam_info,
			     const u8 *req_ie, size_t req_ie_len,
			     const u8 *resp_ie, size_t resp_ie_len, u16 status,
			     gfp_t gfp, bool connect_timeout,
			     tSirResultCodes timeout_reason)
{
	struct cfg80211_connect_resp_params fils_params;
	hdd_adapter_t *adapter = WLAN_HDD_GET_PRIV_PTR(dev);
	struct fils_join_rsp_params *roam_fils_params =
				roam_info->fils_join_rsp;

	qdf_mem_zero(&fils_params, sizeof(fils_params));

	if (!roam_fils_params) {
		fils_params.status = WLAN_STATUS_UNSPECIFIED_FAILURE;
		hdd_populate_fils_params(&fils_params, NULL, 0, NULL,
					 0, NULL, roam_info->fils_seq_num);
	} else {
		fils_params.status = status;
		fils_params.bssid = bssid;
		fils_params.timeout_reason =
				hdd_convert_timeout_reason(timeout_reason);
		fils_params.req_ie = req_ie;
		fils_params.req_ie_len = req_ie_len;
		fils_params.resp_ie = resp_ie;
		fils_params.resp_ie_len = resp_ie_len;
		fils_params.bss = bss;
		hdd_populate_fils_params(&fils_params, roam_fils_params->kek,
					 roam_fils_params->kek_len,
					 roam_fils_params->fils_pmk,
					 roam_fils_params->fils_pmk_len,
					 roam_fils_params->fils_pmkid,
					 roam_info->fils_seq_num);
		hdd_save_gtk_params(adapter, roam_info, false);
	}
	hdd_debug("FILS indicate connect status %d seq no %d",
		  fils_params.status,
		  fils_params.fils_erp_next_seq_num);

	cfg80211_connect_done(dev, &fils_params, gfp);

	if (roam_fils_params && roam_fils_params->hlp_data_len)
		hdd_update_hlp_info(dev, roam_info);

	/* Clear all the FILS key info */
	if (roam_fils_params && roam_fils_params->fils_pmk)
		qdf_mem_free(roam_fils_params->fils_pmk);
	if (roam_fils_params)
		qdf_mem_free(roam_fils_params);
	roam_info->fils_join_rsp = NULL;
}
#else
static inline void hdd_connect_done(struct net_device *dev, const u8 *bssid,
				    struct cfg80211_bss *bss, tCsrRoamInfo
				    *roam_info, const u8 *req_ie,
				    size_t req_ie_len, const u8 *resp_ie,
				    size_t resp_ie_len, u16 status, gfp_t gfp,
				    bool connect_timeout,
				    tSirResultCodes timeout_reason)
{ }
#endif
#endif

#if defined(WLAN_FEATURE_FILS_SK) && \
	(defined(CFG80211_CONNECT_DONE) || \
		(LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)))
/**
 * hdd_fils_update_connect_results() - API to send fils connection status to
 * supplicant.
 * @dev: network device
 * @bssid: bssid to which we want to associate
 * @bss: cfg80211 bss info
 * @roam_info: information about connected bss
 * @req_ie: Request Information Element
 * @req_ie_len: len of the req IE
 * @resp_ie: Response IE
 * @resp_ie_len: len of ht response IE
 * @status: status
 * @gfp: allocation flags
 * @connect_timeout: If timed out waiting for Auth/Assoc/Probe resp
 * @timeout_reason: reason for connect timeout
 *
 * The API is a wrapper to send connection status to supplicant
 *
 * Return: 0 if success else failure
 */
static int hdd_fils_update_connect_results(struct net_device *dev,
			const u8 *bssid,
			struct cfg80211_bss *bss,
			tCsrRoamInfo *roam_info, const u8 *req_ie,
			size_t req_ie_len, const u8 *resp_ie,
			size_t resp_ie_len, u16 status, gfp_t gfp,
			bool connect_timeout,
			tSirResultCodes timeout_reason)
{
	ENTER();
	if (!roam_info || !roam_info->is_fils_connection)
		return -EINVAL;

	hdd_connect_done(dev, bssid, bss, roam_info, req_ie, req_ie_len,
			 resp_ie, resp_ie_len, status, gfp, connect_timeout,
			 timeout_reason);
	return 0;
}
#else
static inline int hdd_fils_update_connect_results(struct net_device *dev,
			const u8 *bssid,
			struct cfg80211_bss *bss,
			tCsrRoamInfo *roam_info, const u8 *req_ie,
			size_t req_ie_len, const u8 *resp_ie,
			size_t resp_ie_len, u16 status, gfp_t gfp,
			bool connect_timeout,
			tSirResultCodes timeout_reason)
{
	return -EINVAL;
}
#endif

/**
 * hdd_connect_result() - API to send connection status to supplicant
 * @dev: network device
 * @bssid: bssid to which we want to associate
 * @roam_info: information about connected bss
 * @req_ie: Request Information Element
 * @req_ie_len: len of the req IE
 * @resp_ie: Response IE
 * @resp_ie_len: len of ht response IE
 * @status: status
 * @gfp: Kernel Flag
 * @connect_timeout: If timed out waiting for Auth/Assoc/Probe resp
 * @timeout_reason: reason for connect timeout
 *
 * The API is a wrapper to send connection status to supplicant
 * and allow runtime suspend
 *
 * Return: Void
 */
void hdd_connect_result(struct net_device *dev, const u8 *bssid,
			tCsrRoamInfo *roam_info, const u8 *req_ie,
			size_t req_ie_len, const u8 *resp_ie,
			size_t resp_ie_len, u16 status, gfp_t gfp,
			bool connect_timeout,
			tSirResultCodes timeout_reason)
{
	hdd_adapter_t *padapter = (hdd_adapter_t *) netdev_priv(dev);
	struct cfg80211_bss *bss = NULL;
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(padapter);

	if (WLAN_STATUS_SUCCESS == status) {
		struct ieee80211_channel *chan;
		int freq;
		int chan_no = roam_info->pBssDesc->channelId;

		if (chan_no <= 14)
			freq = ieee80211_channel_to_frequency(chan_no,
				HDD_NL80211_BAND_2GHZ);
		else
			freq = ieee80211_channel_to_frequency(chan_no,
				HDD_NL80211_BAND_5GHZ);

		chan = ieee80211_get_channel(padapter->wdev.wiphy, freq);
		bss = hdd_cfg80211_get_bss(padapter->wdev.wiphy, chan, bssid,
			roam_info->u.pConnectedProfile->SSID.ssId,
			roam_info->u.pConnectedProfile->SSID.length);
	}

	if (hdd_fils_update_connect_results(dev, bssid, bss,
			roam_info, req_ie, req_ie_len, resp_ie,
			resp_ie_len, status, gfp, connect_timeout,
			timeout_reason) != 0) {
		hdd_connect_bss(dev, bssid, bss, req_ie,
			req_ie_len, resp_ie, resp_ie_len,
			status, gfp, connect_timeout, timeout_reason);
	}
	qdf_runtime_pm_allow_suspend(&hdd_ctx->runtime_context.connect);
	hdd_allow_suspend(WIFI_POWER_EVENT_WAKELOCK_CONNECT);
}
#else
void hdd_connect_result(struct net_device *dev, const u8 *bssid,
			tCsrRoamInfo *roam_info, const u8 *req_ie,
			size_t req_ie_len, const u8 *resp_ie,
			size_t resp_ie_len, u16 status, gfp_t gfp,
			bool connect_timeout,
			tSirResultCodes timeout_reason)
{
	hdd_adapter_t *padapter = (hdd_adapter_t *) netdev_priv(dev);
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(padapter);

	cfg80211_connect_result(dev, bssid, req_ie, req_ie_len,
				resp_ie, resp_ie_len, status, gfp);
	qdf_runtime_pm_allow_suspend(&hdd_ctx->runtime_context.connect);
	hdd_allow_suspend(WIFI_POWER_EVENT_WAKELOCK_CONNECT);
}
#endif


#ifdef MSM_PLATFORM
/**
 * hdd_stop_p2p_go() - call cfg80211 API to stop P2P GO
 * @adapter: pointer to adapter
 *
 * This function calls cfg80211 API to stop P2P GO
 *
 * Return: None
 */
static void hdd_stop_p2p_go(hdd_adapter_t *adapter)
{
	hdd_debug("[SSR] send stop ap to supplicant");
	cfg80211_ap_stopped(adapter->dev, GFP_KERNEL);
}

static inline void hdd_delete_sta(hdd_adapter_t *adapter)
{
}
#else
static inline void hdd_stop_p2p_go(hdd_adapter_t *adapter)
{
}

/**
 * hdd_delete_sta() - call cfg80211 API to delete STA
 * @adapter: pointer to adapter
 *
 * This function calls cfg80211 API to delete STA
 *
 * Return: None
 */
static void hdd_delete_sta(hdd_adapter_t *adapter)
{
	struct qdf_mac_addr bcast_mac = QDF_MAC_ADDR_BROADCAST_INITIALIZER;

	hdd_debug("[SSR] send restart supplicant");
	/* event supplicant to restart */
	cfg80211_del_sta(adapter->dev,
			 (const u8 *)&bcast_mac.bytes[0],
			 GFP_KERNEL);
}
#endif

QDF_STATUS hdd_start_all_adapters(hdd_context_t *hdd_ctx)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;
	eConnectionState connState;

	ENTER();

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);
	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;

		if (!hdd_is_interface_up(adapter))
			goto get_adapter;

		hdd_wmm_init(adapter);

		adapter->scan_info.mScanPending = false;

		switch (adapter->device_mode) {
		case QDF_STA_MODE:
		case QDF_P2P_CLIENT_MODE:
		case QDF_P2P_DEVICE_MODE:

			connState = (WLAN_HDD_GET_STATION_CTX_PTR(adapter))
					->conn_info.connState;

			hdd_init_station_mode(adapter);
			/* Open the gates for HDD to receive Wext commands */
			adapter->isLinkUpSvcNeeded = false;

			/* Indicate disconnect event to supplicant
			 * if associated previously
			 */
			if (eConnectionState_Associated == connState ||
			    eConnectionState_IbssConnected == connState ||
			    eConnectionState_NotConnected == connState ||
			    eConnectionState_IbssDisconnected == connState ||
			    eConnectionState_Disconnecting == connState) {
				union iwreq_data wrqu;

				memset(&wrqu, '\0', sizeof(wrqu));
				wrqu.ap_addr.sa_family = ARPHRD_ETHER;
				memset(wrqu.ap_addr.sa_data, '\0', ETH_ALEN);
				wireless_send_event(adapter->dev, SIOCGIWAP,
						    &wrqu, NULL);
				adapter->sessionCtx.station.
				hdd_ReassocScenario = false;

				/* indicate disconnected event to nl80211 */
				wlan_hdd_cfg80211_indicate_disconnect(
						adapter->dev, false,
						WLAN_REASON_UNSPECIFIED);
			} else if (eConnectionState_Connecting == connState) {
				/*
				 * Indicate connect failure to supplicant if we
				 * were in the process of connecting
				 */
				hdd_connect_result(adapter->dev, NULL, NULL,
						   NULL, 0, NULL, 0,
						   WLAN_STATUS_ASSOC_DENIED_UNSPEC,
						   GFP_KERNEL, false, 0);
			}

			hdd_register_tx_flow_control(adapter,
					hdd_tx_resume_timer_expired_handler,
					hdd_tx_resume_cb,
					hdd_tx_flow_control_is_pause);
			if (adapter->device_mode == QDF_STA_MODE) {
				hdd_debug("Sending the Lpass start notification after re_init");
				hdd_lpass_notify_start(hdd_ctx, adapter);
			}
			break;

		case QDF_SAP_MODE:
			if (hdd_ctx->config->sap_internal_restart)
				hdd_init_ap_mode(adapter, true);

			break;

		case QDF_P2P_GO_MODE:
			hdd_delete_sta(adapter);
			break;
		case QDF_MONITOR_MODE:
			if (wlan_hdd_is_session_type_monitor(
						QDF_MONITOR_MODE)) {
				hdd_set_mon_mode_cb(adapter->dev);
				break;
			}
			hdd_init_station_mode(adapter);
			hdd_set_mon_rx_cb(adapter->dev);
			wlan_hdd_set_mon_chan(adapter, adapter->mon_chan,
					      adapter->mon_bandwidth);
			break;

		default:
			break;
		}
		/*
		 * Action frame registered in one adapter which will
		 * applicable to all interfaces
		 */
		wlan_hdd_cfg80211_register_frames(adapter);

get_adapter:
		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);
	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;

		if (!hdd_is_interface_up(adapter))
			goto get_adapter_sec;

		if (adapter->device_mode == QDF_P2P_GO_MODE)
			hdd_stop_p2p_go(adapter);
get_adapter_sec:
		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}

	EXIT();

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS hdd_get_front_adapter(hdd_context_t *hdd_ctx,
				 hdd_adapter_list_node_t **padapterNode)
{
	QDF_STATUS status;

	qdf_spin_lock_bh(&hdd_ctx->hdd_adapter_lock);
	status = qdf_list_peek_front(&hdd_ctx->hddAdapters,
				     (qdf_list_node_t **) padapterNode);
	qdf_spin_unlock_bh(&hdd_ctx->hdd_adapter_lock);
	return status;
}

QDF_STATUS hdd_get_next_adapter(hdd_context_t *hdd_ctx,
				hdd_adapter_list_node_t *adapterNode,
				hdd_adapter_list_node_t **pNextAdapterNode)
{
	QDF_STATUS status;

	qdf_spin_lock_bh(&hdd_ctx->hdd_adapter_lock);
	status = qdf_list_peek_next(&hdd_ctx->hddAdapters,
				    (qdf_list_node_t *) adapterNode,
				    (qdf_list_node_t **) pNextAdapterNode);

	qdf_spin_unlock_bh(&hdd_ctx->hdd_adapter_lock);
	return status;
}

QDF_STATUS hdd_remove_adapter(hdd_context_t *hdd_ctx,
			      hdd_adapter_list_node_t *adapterNode)
{
	QDF_STATUS status;

	qdf_spin_lock_bh(&hdd_ctx->hdd_adapter_lock);
	status = qdf_list_remove_node(&hdd_ctx->hddAdapters,
				      &adapterNode->node);
	qdf_spin_unlock_bh(&hdd_ctx->hdd_adapter_lock);
	return status;
}

QDF_STATUS hdd_remove_front_adapter(hdd_context_t *hdd_ctx,
				    hdd_adapter_list_node_t **padapterNode)
{
	QDF_STATUS status;

	qdf_spin_lock_bh(&hdd_ctx->hdd_adapter_lock);
	status = qdf_list_remove_front(&hdd_ctx->hddAdapters,
				       (qdf_list_node_t **) padapterNode);
	qdf_spin_unlock_bh(&hdd_ctx->hdd_adapter_lock);
	return status;
}

QDF_STATUS hdd_add_adapter_back(hdd_context_t *hdd_ctx,
				hdd_adapter_list_node_t *adapterNode)
{
	QDF_STATUS status;

	qdf_spin_lock_bh(&hdd_ctx->hdd_adapter_lock);
	status = qdf_list_insert_back(&hdd_ctx->hddAdapters,
				      (qdf_list_node_t *) adapterNode);
	qdf_spin_unlock_bh(&hdd_ctx->hdd_adapter_lock);
	return status;
}

QDF_STATUS hdd_add_adapter_front(hdd_context_t *hdd_ctx,
				 hdd_adapter_list_node_t *adapterNode)
{
	QDF_STATUS status;

	qdf_spin_lock_bh(&hdd_ctx->hdd_adapter_lock);
	status = qdf_list_insert_front(&hdd_ctx->hddAdapters,
				       (qdf_list_node_t *) adapterNode);
	qdf_spin_unlock_bh(&hdd_ctx->hdd_adapter_lock);
	return status;
}

hdd_adapter_t *hdd_get_adapter_by_macaddr(hdd_context_t *hdd_ctx,
					  tSirMacAddr macAddr)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	hdd_adapter_t *adapter;
	QDF_STATUS status;

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);

	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;

		if (adapter
		    && !qdf_mem_cmp(adapter->macAddressCurrent.bytes,
				       macAddr, sizeof(tSirMacAddr)))
			return adapter;

		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}

	return NULL;

}

hdd_adapter_t *hdd_get_adapter_by_vdev(hdd_context_t *hdd_ctx,
				       uint32_t vdev_id)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	hdd_adapter_t *adapter;
	QDF_STATUS qdf_status;

	qdf_status = hdd_get_front_adapter(hdd_ctx, &adapterNode);

	while ((NULL != adapterNode) && (QDF_STATUS_SUCCESS == qdf_status)) {
		adapter = adapterNode->pAdapter;

		if (adapter->sessionId == vdev_id)
			return adapter;

		qdf_status =
			hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}

	return NULL;
}

/**
 * hdd_get_adapter_by_sme_session_id() - Return adapter with
 * the sessionid
 * @hdd_ctx: hdd context.
 * @sme_session_id: sme session is for the adapter to get.
 *
 * This function is used to get the adapter with provided session id
 *
 * Return: adapter pointer if found
 *
 */
hdd_adapter_t *hdd_get_adapter_by_sme_session_id(hdd_context_t *hdd_ctx,
						uint32_t sme_session_id)
{
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	hdd_adapter_t *adapter;
	QDF_STATUS qdf_status;


	qdf_status = hdd_get_front_adapter(hdd_ctx, &adapter_node);

	while ((NULL != adapter_node) &&
			(QDF_STATUS_SUCCESS == qdf_status)) {
		adapter = adapter_node->pAdapter;

		if (adapter &&
			 adapter->sessionId == sme_session_id)
			return adapter;

		qdf_status =
			hdd_get_next_adapter(hdd_ctx,
				 adapter_node, &next);
		adapter_node = next;
	}
	return NULL;
}

hdd_adapter_t *hdd_get_adapter_by_iface_name(hdd_context_t *hdd_ctx,
					     const char *iface_name)
{
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	hdd_adapter_t *adapter;
	QDF_STATUS qdf_status;

	qdf_status = hdd_get_front_adapter(hdd_ctx, &adapter_node);

	while ((NULL != adapter_node) &&
			(QDF_STATUS_SUCCESS == qdf_status)) {
		adapter = adapter_node->pAdapter;

		if (adapter &&
			 !qdf_str_cmp(adapter->dev->name, iface_name))
			return adapter;

		qdf_status =
			hdd_get_next_adapter(hdd_ctx,
				 adapter_node, &next);
		adapter_node = next;
	}
	return NULL;
}

/**
 * hdd_get_adapter() - to get adapter matching the mode
 * @hdd_ctx: hdd context
 * @mode: adapter mode
 *
 * This routine will return the pointer to adapter matching
 * with the passed mode.
 *
 * Return: pointer to adapter or null
 */
hdd_adapter_t *hdd_get_adapter(hdd_context_t *hdd_ctx,
			enum tQDF_ADAPTER_MODE mode)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	hdd_adapter_t *adapter;
	QDF_STATUS status;

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);

	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;

		if (adapter && (mode == adapter->device_mode))
			return adapter;

		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}

	return NULL;

}

/**
 * hdd_is_adapter_valid() - Check if adapter is valid
 * @hdd_ctx: hdd context
 * @adapter: pointer to adapter
 *
 * Return: true if adapter address is valid or false otherwise
 */
bool hdd_is_adapter_valid(hdd_context_t *hdd_ctx, hdd_adapter_t *adapter)
{
	hdd_adapter_list_node_t *adapter_node = NULL, *p_next = NULL;
	hdd_adapter_t *p_adapter;
	QDF_STATUS status;

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);

	while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
		p_adapter = adapter_node->pAdapter;

		if (p_adapter && (p_adapter == adapter))
			return true;

		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &p_next);
		adapter_node = p_next;
	}

	return false;

}

/**
 * hdd_get_operating_channel() - return operating channel of the device mode
 * @hdd_ctx:	Pointer to the HDD context.
 * @mode:	Device mode for which operating channel is required.
 *              Suported modes:
 *			QDF_STA_MODE,
 *			QDF_P2P_CLIENT_MODE,
 *			QDF_SAP_MODE,
 *			QDF_P2P_GO_MODE.
 *
 * This API returns the operating channel of the requested device mode
 *
 * Return: channel number. "0" id the requested device is not found OR it is
 *	   not connected.
 */
uint8_t hdd_get_operating_channel(hdd_context_t *hdd_ctx,
			enum tQDF_ADAPTER_MODE mode)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;
	uint8_t operatingChannel = 0;

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);

	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;

		if (mode == adapter->device_mode) {
			switch (adapter->device_mode) {
			case QDF_STA_MODE:
			case QDF_P2P_CLIENT_MODE:
				if (hdd_conn_is_connected
					    (WLAN_HDD_GET_STATION_CTX_PTR
						(adapter))) {
					operatingChannel =
						(WLAN_HDD_GET_STATION_CTX_PTR
						(adapter))->conn_info.
							operationChannel;
				}
				break;
			case QDF_SAP_MODE:
			case QDF_P2P_GO_MODE:
				/* softap connection info */
				if (test_bit
					    (SOFTAP_BSS_STARTED,
					    &adapter->event_flags))
					operatingChannel =
						(WLAN_HDD_GET_AP_CTX_PTR
						(adapter))->operatingChannel;
				break;
			default:
				break;
			}

			/* Found the device of interest. break the loop */
			break;
		}

		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}
	return operatingChannel;
}

static inline QDF_STATUS hdd_unregister_wext_all_adapters(hdd_context_t *
							  hdd_ctx)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;

	ENTER();

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);

	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;
		if ((adapter->device_mode == QDF_STA_MODE) ||
		    (adapter->device_mode == QDF_P2P_CLIENT_MODE) ||
		    (adapter->device_mode == QDF_IBSS_MODE) ||
		    (adapter->device_mode == QDF_P2P_DEVICE_MODE) ||
		    (adapter->device_mode == QDF_SAP_MODE) ||
		    (adapter->device_mode == QDF_P2P_GO_MODE)) {
			wlan_hdd_cfg80211_deregister_frames(adapter);
			hdd_unregister_wext(adapter->dev);
		}
		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}

	EXIT();

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS hdd_abort_mac_scan_all_adapters(hdd_context_t *hdd_ctx)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;

	ENTER();

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);

	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;
		if ((adapter->device_mode == QDF_STA_MODE) ||
		    (adapter->device_mode == QDF_P2P_CLIENT_MODE) ||
		    (adapter->device_mode == QDF_IBSS_MODE) ||
		    (adapter->device_mode == QDF_P2P_DEVICE_MODE) ||
		    (adapter->device_mode == QDF_SAP_MODE) ||
		    (adapter->device_mode == QDF_P2P_GO_MODE)) {
			wlan_hdd_scan_abort(adapter);
		}
		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}

	EXIT();

	return QDF_STATUS_SUCCESS;
}

/**
 * hdd_abort_sched_scan_all_adapters() - stops scheduled (PNO) scans for all
 * adapters
 * @hdd_ctx: The HDD context containing the adapters to operate on
 *
 * return: QDF_STATUS_SUCCESS
 */
static QDF_STATUS hdd_abort_sched_scan_all_adapters(hdd_context_t *hdd_ctx)
{
	hdd_adapter_list_node_t *adapter_node = NULL, *next_node = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;
	int err;

	ENTER();

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);

	while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
		adapter = adapter_node->pAdapter;
		if ((adapter->device_mode == QDF_STA_MODE) ||
		    (adapter->device_mode == QDF_P2P_CLIENT_MODE) ||
		    (adapter->device_mode == QDF_IBSS_MODE) ||
		    (adapter->device_mode == QDF_P2P_DEVICE_MODE) ||
		    (adapter->device_mode == QDF_SAP_MODE) ||
		    (adapter->device_mode == QDF_P2P_GO_MODE)) {
			err = wlan_hdd_sched_scan_stop(adapter->dev);
			if (err)
				hdd_err("Unable to stop scheduled scan");
		}
		status = hdd_get_next_adapter(hdd_ctx, adapter_node,
					      &next_node);
		adapter_node = next_node;
	}

	EXIT();

	return QDF_STATUS_SUCCESS;
}

#ifdef WLAN_NS_OFFLOAD
/**
 * hdd_wlan_unregister_ip6_notifier() - unregister IPv6 change notifier
 * @hdd_ctx: Pointer to hdd context
 *
 * Unregister for IPv6 address change notifications.
 *
 * Return: None
 */
static void hdd_wlan_unregister_ip6_notifier(hdd_context_t *hdd_ctx)
{
	unregister_inet6addr_notifier(&hdd_ctx->ipv6_notifier);
}

/**
 * hdd_wlan_register_ip6_notifier() - register IPv6 change notifier
 * @hdd_ctx: Pointer to hdd context
 *
 * Register for IPv6 address change notifications.
 *
 * Return: 0 on success and errno on failure.
 */
static int hdd_wlan_register_ip6_notifier(hdd_context_t *hdd_ctx)
{
	int ret;

	hdd_ctx->ipv6_notifier.notifier_call = wlan_hdd_ipv6_changed;
	ret = register_inet6addr_notifier(&hdd_ctx->ipv6_notifier);
	if (ret) {
		hdd_err("Failed to register IPv6 notifier: %d", ret);
		goto out;
	}

	hdd_debug("Registered IPv6 notifier");
out:
	return ret;
}
#else
/**
 * hdd_wlan_unregister_ip6_notifier() - unregister IPv6 change notifier
 * @hdd_ctx: Pointer to hdd context
 *
 * Unregister for IPv6 address change notifications.
 *
 * Return: None
 */
static void hdd_wlan_unregister_ip6_notifier(hdd_context_t *hdd_ctx)
{
}

/**
 * hdd_wlan_register_ip6_notifier() - register IPv6 change notifier
 * @hdd_ctx: Pointer to hdd context
 *
 * Register for IPv6 address change notifications.
 *
 * Return: None
 */
static int hdd_wlan_register_ip6_notifier(hdd_context_t *hdd_ctx)
{
	return 0;
}
#endif

/**
 * hdd_register_notifiers - Register netdev notifiers.
 * @hdd_ctx: HDD context
 *
 * Register netdev notifiers like IPv4 and IPv6.
 *
 * Return: 0 on success and errno on failure
 */
static int hdd_register_notifiers(hdd_context_t *hdd_ctx)
{
	int ret;

	ret = hdd_wlan_register_ip6_notifier(hdd_ctx);
	if (ret)
		goto out;

	hdd_ctx->ipv4_notifier.notifier_call = wlan_hdd_ipv4_changed;
	ret = register_inetaddr_notifier(&hdd_ctx->ipv4_notifier);
	if (ret) {
		hdd_err("Failed to register IPv4 notifier: %d", ret);
		goto unregister_ip6_notifier;
	}

	return 0;

unregister_ip6_notifier:
	hdd_wlan_unregister_ip6_notifier(hdd_ctx);
out:
	return ret;

}

/**
 * hdd_unregister_notifiers - Unregister netdev notifiers.
 * @hdd_ctx: HDD context
 *
 * Unregister netdev notifiers like IPv4 and IPv6.
 *
 * Return: None.
 */
void hdd_unregister_notifiers(hdd_context_t *hdd_ctx)
{
	hdd_wlan_unregister_ip6_notifier(hdd_ctx);

	unregister_inetaddr_notifier(&hdd_ctx->ipv4_notifier);
}

/**
 * hdd_exit_netlink_services - Exit netlink services
 * @hdd_ctx: HDD context
 *
 * Exit netlink services like cnss_diag, cesium netlink socket, ptt socket and
 * nl service.
 *
 * Return: None.
 */
static void hdd_exit_netlink_services(hdd_context_t *hdd_ctx)
{
	spectral_scan_deactivate_service();
	cnss_diag_deactivate_service();
	hdd_close_cesium_nl_sock();
	ptt_sock_deactivate_svc();
	oem_deactivate_service();
	nl_srv_exit();
}

/**
 * hdd_init_netlink_services- Init netlink services
 * @hdd_ctx: HDD context
 *
 * Init netlink services like cnss_diag, cesium netlink socket, ptt socket and
 * nl service.
 *
 * Return: 0 on success and errno on failure.
 */
static int hdd_init_netlink_services(hdd_context_t *hdd_ctx)
{
	int ret;

	ret = wlan_hdd_nl_init(hdd_ctx);
	if (ret) {
		hdd_err("nl_srv_init failed: %d", ret);
		goto out;
	}
	cds_set_radio_index(hdd_ctx->radio_index);

	ret = oem_activate_service(hdd_ctx);
	if (ret) {
		hdd_err("oem_activate_service failed: %d", ret);
		goto err_nl_srv;
	}

	ptt_sock_activate_svc();

	ret = hdd_open_cesium_nl_sock();
	if (ret)
		hdd_err("hdd_open_cesium_nl_sock failed ret: %d", ret);

	ret = cnss_diag_activate_service();
	if (ret) {
		hdd_err("cnss_diag_activate_service failed: %d", ret);
		goto err_close_cesium;
	}

	ret = spectral_scan_activate_service();
	if (ret) {
		hdd_alert("spectral_scan_activate_service failed: %d", ret);
		goto err_cnss_diag;
	}

	return 0;

err_cnss_diag:
	cnss_diag_deactivate_service();
err_close_cesium:
	hdd_close_cesium_nl_sock();
	ptt_sock_deactivate_svc();
	oem_deactivate_service();
err_nl_srv:
	nl_srv_exit();
out:
	return ret;
}

/**
 * hdd_rx_wake_lock_destroy() - Destroy RX wakelock
 * @hdd_ctx:	HDD context.
 *
 * Destroy RX wakelock.
 *
 * Return: None.
 */
static void hdd_rx_wake_lock_destroy(hdd_context_t *hdd_ctx)
{
	qdf_wake_lock_destroy(&hdd_ctx->rx_wake_lock);
}

/**
 * hdd_rx_wake_lock_create() - Create RX wakelock
 * @hdd_ctx:	HDD context.
 *
 * Create RX wakelock.
 *
 * Return: None.
 */
static void hdd_rx_wake_lock_create(hdd_context_t *hdd_ctx)
{
	qdf_wake_lock_create(&hdd_ctx->rx_wake_lock, "qcom_rx_wakelock");
}

/**
 * hdd_roc_context_init() - Init ROC context
 * @hdd_ctx:	HDD context.
 *
 * Initialize ROC context.
 *
 * Return: 0 on success and errno on failure.
 */
static int hdd_roc_context_init(hdd_context_t *hdd_ctx)
{
	qdf_spinlock_create(&hdd_ctx->hdd_roc_req_q_lock);
	qdf_list_create(&hdd_ctx->hdd_roc_req_q, MAX_ROC_REQ_QUEUE_ENTRY);
	qdf_idr_create(&hdd_ctx->p2p_idr);

	INIT_DELAYED_WORK(&hdd_ctx->roc_req_work, wlan_hdd_roc_request_dequeue);

	return 0;
}

/**
 * hdd_roc_context_destroy() - Destroy ROC context
 * @hdd_ctx:	HDD context.
 *
 * Destroy roc list and spinlock.
 *
 * Return: None.
 */
static void hdd_roc_context_destroy(hdd_context_t *hdd_ctx)
{
	qdf_idr_destroy(&hdd_ctx->p2p_idr);
	qdf_list_destroy(&hdd_ctx->hdd_roc_req_q);
	qdf_spinlock_destroy(&hdd_ctx->hdd_roc_req_q_lock);
}

/**
 * hdd_context_deinit() - Deinitialize HDD context
 * @hdd_ctx:    HDD context.
 *
 * Deinitialize HDD context along with all the feature specific contexts but
 * do not free hdd context itself. Caller of this API is supposed to free
 * HDD context.
 *
 * return: 0 on success and errno on failure.
 */
static int hdd_context_deinit(hdd_context_t *hdd_ctx)
{
	qdf_wake_lock_destroy(&hdd_ctx->monitor_mode_wakelock);

	wlan_hdd_cfg80211_deinit(hdd_ctx->wiphy);

	hdd_roc_context_destroy(hdd_ctx);

	hdd_sap_context_destroy(hdd_ctx);

	hdd_rx_wake_lock_destroy(hdd_ctx);

	hdd_tdls_context_destroy(hdd_ctx);

	hdd_scan_context_destroy(hdd_ctx);

	qdf_list_destroy(&hdd_ctx->hddAdapters);

	return 0;
}

/**
 * hdd_context_destroy() - Destroy HDD context
 * @hdd_ctx:	HDD context to be destroyed.
 *
 * Free config and HDD context as well as destroy all the resources.
 *
 * Return: None
 */
static void hdd_context_destroy(hdd_context_t *hdd_ctx)
{
	cds_set_context(QDF_MODULE_ID_HDD, NULL);

	wlan_hdd_deinit_tx_rx_histogram(hdd_ctx);

	hdd_context_deinit(hdd_ctx);

	hdd_free_probe_req_ouis(hdd_ctx);

	qdf_mem_free(hdd_ctx->config);
	hdd_ctx->config = NULL;

	wiphy_free(hdd_ctx->wiphy);
}

/**
 * wlan_destroy_bug_report_lock() - Destroy bug report lock
 *
 * This function is used to destroy bug report lock
 *
 * Return: None
 */
static void wlan_destroy_bug_report_lock(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		hdd_err("cds context is NULL");
		return;
	}

	qdf_spinlock_destroy(&p_cds_context->bug_report_lock);
}

/**
 * hdd_wlan_exit() - HDD WLAN exit function
 * @hdd_ctx:	Pointer to the HDD Context
 *
 * This is the driver exit point (invoked during rmmod)
 *
 * Return: None
 */
static void hdd_wlan_exit(hdd_context_t *hdd_ctx)
{
	struct wiphy *wiphy = hdd_ctx->wiphy;
	int driver_status;

	ENTER();

	hdd_unregister_notifiers(hdd_ctx);

	if (QDF_TIMER_STATE_RUNNING ==
	    qdf_mc_timer_get_current_state(&hdd_ctx->tdls_source_timer)) {
		qdf_mc_timer_stop(&hdd_ctx->tdls_source_timer);
	}

#ifdef FEATURE_WLAN_AP_AP_ACS_OPTIMIZE
	if (QDF_TIMER_STATE_RUNNING ==
	    qdf_mc_timer_get_current_state(&hdd_ctx->skip_acs_scan_timer)) {
		qdf_mc_timer_stop(&hdd_ctx->skip_acs_scan_timer);
	}

	qdf_spin_lock(&hdd_ctx->acs_skip_lock);
	qdf_mem_free(hdd_ctx->last_acs_channel_list);
	hdd_ctx->last_acs_channel_list = NULL;
	hdd_ctx->num_of_channels = 0;
	qdf_spin_unlock(&hdd_ctx->acs_skip_lock);
#endif

	mutex_lock(&hdd_ctx->iface_change_lock);
	driver_status = hdd_ctx->driver_status;
	mutex_unlock(&hdd_ctx->iface_change_lock);

	/*
	 * Powersave Offload Case
	 * Disable Idle Power Save Mode
	 */
	hdd_set_idle_ps_config(hdd_ctx, false);

	if (driver_status != DRIVER_MODULES_CLOSED) {
		hdd_unregister_wext_all_adapters(hdd_ctx);
		/*
		 * Cancel any outstanding scan requests.  We are about to close
		 * all of our adapters, but an adapter structure is what SME
		 * passes back to our callback function.  Hence if there
		 * are any outstanding scan requests then there is a
		 * race condition between when the adapter is closed and
		 * when the callback is invoked.  We try to resolve that
		 * race condition here by canceling any outstanding scans
		 * before we close the adapters.
		 * Note that the scans may be cancelled in an asynchronous
		 * manner, so ideally there needs to be some kind of
		 * synchronization.  Rather than introduce a new
		 * synchronization here, we will utilize the fact that we are
		 * about to Request Full Power, and since that is synchronized,
		 * the expectation is that by the time Request Full Power has
		 * completed, all scans will be cancelled
		 */
		hdd_cleanup_scan_queue(hdd_ctx, NULL);
		hdd_abort_mac_scan_all_adapters(hdd_ctx);
		hdd_abort_sched_scan_all_adapters(hdd_ctx);

		if (wlan_hdd_is_session_type_monitor(QDF_MONITOR_MODE))
			hdd_reset_mon_mode_cb();

		hdd_stop_all_adapters(hdd_ctx, true);
		hdd_deinit_all_adapters(hdd_ctx, false);
	}

	unregister_reboot_notifier(&system_reboot_notifier);
	unregister_netdevice_notifier(&hdd_netdev_notifier);

	/* Free up RoC request queue and flush workqueue */
	cds_flush_work(&hdd_ctx->roc_req_work);

	hdd_wlan_stop_modules(hdd_ctx, false);

	hdd_driver_memdump_deinit();

	qdf_mc_timer_destroy(&hdd_ctx->tdls_source_timer);

#ifdef FEATURE_WLAN_AP_AP_ACS_OPTIMIZE
	if (!QDF_IS_STATUS_SUCCESS
		    (qdf_mc_timer_destroy(&hdd_ctx->skip_acs_scan_timer))) {
		hdd_err("Cannot deallocate ACS Skip timer");
	}
#endif

	qdf_nbuf_deinit_replenish_timer();
	hdd_bus_bandwidth_destroy(hdd_ctx);

	if (QDF_GLOBAL_MONITOR_MODE ==  hdd_get_conparam()) {
		hdd_info("Release wakelock for monitor mode!");
		qdf_wake_lock_release(&hdd_ctx->monitor_mode_wakelock,
				WIFI_POWER_EVENT_WAKELOCK_MONITOR_MODE);
	}

	qdf_spinlock_destroy(&hdd_ctx->hdd_adapter_lock);
	qdf_spinlock_destroy(&hdd_ctx->sta_update_info_lock);
	qdf_spinlock_destroy(&hdd_ctx->connection_status_lock);
	qdf_mutex_destroy(&hdd_ctx->cache_channel_lock);

	/*
	 * Close CDS
	 * This frees pMac(HAL) context. There should not be any call
	 * that requires pMac access after this.
	 */

	hdd_green_ap_deinit(hdd_ctx);
	hdd_request_manager_deinit();

	hdd_close_all_adapters(hdd_ctx, false);

	wlansap_global_deinit();
	/*
	 * If there is re_init failure wiphy would have already de-registered
	 * check the wiphy status status before un-registering again
	 */
	if (wiphy && wiphy->registered) {
		wiphy_unregister(wiphy);
		wlan_hdd_cfg80211_deinit(wiphy);
		hdd_lpass_notify_stop(hdd_ctx);
	}

	hdd_exit_netlink_services(hdd_ctx);
	mutex_destroy(&hdd_ctx->iface_change_lock);
	hdd_context_destroy(hdd_ctx);
}

void __hdd_wlan_exit(void)
{
	hdd_context_t *hdd_ctx;

	ENTER();

	hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	if (!hdd_ctx) {
		hdd_err("Invalid HDD Context");
		EXIT();
		return;
	}

	/* Do all the cleanup before deregistering the driver */
	hdd_wlan_exit(hdd_ctx);

	EXIT();
}

#ifdef FEATURE_WLAN_AP_AP_ACS_OPTIMIZE
/**
 * hdd_skip_acs_scan_timer_handler() - skip ACS scan timer timeout handler
 * @data: pointer to hdd_context_t
 *
 * This function will reset acs_scan_status to eSAP_DO_NEW_ACS_SCAN.
 * Then new ACS request will do a fresh scan without reusing the cached
 * scan information.
 *
 * Return: void
 */
static void hdd_skip_acs_scan_timer_handler(void *data)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *) data;

	hdd_debug("ACS Scan result expired. Reset ACS scan skip");
	hdd_ctx->skip_acs_scan_status = eSAP_DO_NEW_ACS_SCAN;
	qdf_spin_lock(&hdd_ctx->acs_skip_lock);
	qdf_mem_free(hdd_ctx->last_acs_channel_list);
	hdd_ctx->last_acs_channel_list = NULL;
	hdd_ctx->num_of_channels = 0;
	qdf_spin_unlock(&hdd_ctx->acs_skip_lock);

	if (!hdd_ctx->hHal)
		return;
	sme_scan_flush_result(hdd_ctx->hHal);
}
#endif

#ifdef QCA_HT_2040_COEX
/**
 * hdd_wlan_set_ht2040_mode() - notify FW with HT20/HT40 mode
 * @adapter: pointer to adapter
 * @staId: station id
 * @macAddrSTA: station MAC address
 * @channel_type: channel type
 *
 * This function notifies FW with HT20/HT40 mode
 *
 * Return: 0 if successful, error number otherwise
 */
int hdd_wlan_set_ht2040_mode(hdd_adapter_t *adapter, uint16_t staId,
			     struct qdf_mac_addr macAddrSTA, int channel_type)
{
	int status;
	QDF_STATUS qdf_status;
	hdd_context_t *hdd_ctx = NULL;

	hdd_ctx = WLAN_HDD_GET_CTX(adapter);

	status = wlan_hdd_validate_context(hdd_ctx);
	if (status)
		return status;

	if (!hdd_ctx->hHal)
		return -EINVAL;

	qdf_status = sme_notify_ht2040_mode(hdd_ctx->hHal, staId, macAddrSTA,
					    adapter->sessionId, channel_type);
	if (QDF_STATUS_SUCCESS != qdf_status) {
		hdd_err("Fail to send notification with ht2040 mode");
		return -EINVAL;
	}

	return 0;
}
#endif

/**
 * hdd_wlan_notify_modem_power_state() - notify FW with modem power status
 * @state: state
 *
 * This function notifies FW with modem power status
 *
 * Return: 0 if successful, error number otherwise
 */
int hdd_wlan_notify_modem_power_state(int state)
{
	int status;
	QDF_STATUS qdf_status;
	hdd_context_t *hdd_ctx;

	hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	status = wlan_hdd_validate_context(hdd_ctx);
	if (status)
		return status;

	if (!hdd_ctx->hHal)
		return -EINVAL;

	qdf_status = sme_notify_modem_power_state(hdd_ctx->hHal, state);
	if (QDF_STATUS_SUCCESS != qdf_status) {
		hdd_err("Fail to send notification with modem power state %d",
		       state);
		return -EINVAL;
	}
	return 0;
}

/**
 *
 * hdd_post_cds_enable_config() - HDD post cds start config helper
 * @adapter - Pointer to the HDD
 *
 * Return: None
 */
QDF_STATUS hdd_post_cds_enable_config(hdd_context_t *hdd_ctx)
{
	QDF_STATUS qdf_ret_status;

	/*
	 * Send ready indication to the HDD.  This will kick off the MAC
	 * into a 'running' state and should kick off an initial scan.
	 */
	qdf_ret_status = sme_hdd_ready_ind(hdd_ctx->hHal);
	if (!QDF_IS_STATUS_SUCCESS(qdf_ret_status)) {
		hdd_err("sme_hdd_ready_ind() failed with status code %08d [x%08x]",
			qdf_ret_status, qdf_ret_status);
		return QDF_STATUS_E_FAILURE;
	}

	sme_generic_change_country_code(hdd_ctx->hHal,
					hdd_ctx->reg.alpha2);

	return QDF_STATUS_SUCCESS;
}

hdd_adapter_t *hdd_get_first_valid_adapter(void)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	hdd_adapter_t *adapter;
	QDF_STATUS status;
	hdd_context_t *hdd_ctx;

	hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);

	if (!hdd_ctx) {
		hdd_err("HDD context not valid");
		return NULL;
	}

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);

	while (adapterNode != NULL && status == QDF_STATUS_SUCCESS) {
		adapter = adapterNode->pAdapter;
		if (adapter && adapter->magic == WLAN_HDD_ADAPTER_MAGIC)
			return adapter;
		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}


	return NULL;
}

/* wake lock APIs for HDD */
void hdd_prevent_suspend(uint32_t reason)
{
	qdf_wake_lock_acquire(&wlan_wake_lock, reason);
}

void hdd_allow_suspend(uint32_t reason)
{
	qdf_wake_lock_release(&wlan_wake_lock, reason);
}

void hdd_prevent_suspend_timeout(uint32_t timeout, uint32_t reason)
{
	cds_host_diag_log_work(&wlan_wake_lock, timeout, reason);
	qdf_wake_lock_timeout_acquire(&wlan_wake_lock, timeout);
}

/**
 * hdd_exchange_version_and_caps() - exchange version and capability with target
 * @hdd_ctx:	Pointer to HDD context
 *
 * This is the HDD function to exchange version and capability information
 * between Host and Target
 *
 * This function gets reported version of FW.
 * It also finds the version of target headers used to compile the host;
 * It compares the above two and prints a warning if they are different;
 * It gets the SW and HW version string;
 * Finally, it exchanges capabilities between host and target i.e. host
 * and target exchange a msg indicating the features they support through a
 * bitmap
 *
 * Return: None
 */
void hdd_exchange_version_and_caps(hdd_context_t *hdd_ctx)
{

	tSirVersionType versionCompiled;
	tSirVersionType versionReported;
	tSirVersionString versionString;
	uint8_t fwFeatCapsMsgSupported = 0;
	QDF_STATUS vstatus;

	memset(&versionCompiled, 0, sizeof(versionCompiled));
	memset(&versionReported, 0, sizeof(versionReported));

	/* retrieve and display WCNSS version information */
	do {

		vstatus = sme_get_wcnss_wlan_compiled_version(hdd_ctx->hHal,
							      &versionCompiled);
		if (!QDF_IS_STATUS_SUCCESS(vstatus)) {
			hdd_err("unable to retrieve WCNSS WLAN compiled version");
			break;
		}

		vstatus = sme_get_wcnss_wlan_reported_version(hdd_ctx->hHal,
							      &versionReported);
		if (!QDF_IS_STATUS_SUCCESS(vstatus)) {
			hdd_err("unable to retrieve WCNSS WLAN reported version");
			break;
		}

		if ((versionCompiled.major != versionReported.major) ||
		    (versionCompiled.minor != versionReported.minor) ||
		    (versionCompiled.version != versionReported.version) ||
		    (versionCompiled.revision != versionReported.revision)) {
			pr_err("%s: WCNSS WLAN Version %u.%u.%u.%u, "
			       "Host expected %u.%u.%u.%u\n",
			       WLAN_MODULE_NAME,
			       (int)versionReported.major,
			       (int)versionReported.minor,
			       (int)versionReported.version,
			       (int)versionReported.revision,
			       (int)versionCompiled.major,
			       (int)versionCompiled.minor,
			       (int)versionCompiled.version,
			       (int)versionCompiled.revision);
		} else {
			pr_info("%s: WCNSS WLAN version %u.%u.%u.%u\n",
				WLAN_MODULE_NAME,
				(int)versionReported.major,
				(int)versionReported.minor,
				(int)versionReported.version,
				(int)versionReported.revision);
		}

		vstatus = sme_get_wcnss_software_version(hdd_ctx->hHal,
							 versionString,
							 sizeof(versionString));
		if (!QDF_IS_STATUS_SUCCESS(vstatus)) {
			hdd_err("unable to retrieve WCNSS software version string");
			break;
		}

		pr_info("%s: WCNSS software version %s\n",
			WLAN_MODULE_NAME, versionString);

		vstatus = sme_get_wcnss_hardware_version(hdd_ctx->hHal,
							 versionString,
							 sizeof(versionString));
		if (!QDF_IS_STATUS_SUCCESS(vstatus)) {
			hdd_err("unable to retrieve WCNSS hardware version string");
			break;
		}

		pr_info("%s: WCNSS hardware version %s\n",
			WLAN_MODULE_NAME, versionString);

		/*
		 * 1.Check if FW version is greater than 0.1.1.0. Only then
		 * send host-FW capability exchange message
		 * 2.Host-FW capability exchange message  is only present on
		 * target 1.1 so send the message only if it the target is 1.1
		 * minor numbers for different target branches:
		 * 0 -> (1.0)Mainline Build
		 * 1 -> (1.1)Mainline Build
		 * 2->(1.04) Stability Build
		 */
		if (((versionReported.major > 0) || (versionReported.minor > 1)
		     || ((versionReported.minor >= 1)
			 && (versionReported.version >= 1)))
		    && ((versionReported.major == 1)
			&& (versionReported.minor >= 1)))
			fwFeatCapsMsgSupported = 1;

		if (fwFeatCapsMsgSupported) {
			/*
			 * Indicate if IBSS heartbeat monitoring needs to be
			 * offloaded
			 */
			if (!hdd_ctx->config->enableIbssHeartBeatOffload) {
				sme_disable_feature_capablity
					(IBSS_HEARTBEAT_OFFLOAD);
			}

			sme_feature_caps_exchange(hdd_ctx->hHal);
		}

	} while (0);

}

/* Initialize channel list in sme based on the country code */
QDF_STATUS hdd_set_sme_chan_list(hdd_context_t *hdd_ctx)
{
	return sme_init_chan_list(hdd_ctx->hHal, hdd_ctx->reg.alpha2,
				  hdd_ctx->reg.cc_src);
}

/**
 * hdd_is_5g_supported() - check if hardware supports 5GHz
 * @hdd_ctx:	Pointer to the hdd context
 *
 * HDD function to know if hardware supports 5GHz
 *
 * Return:  true if hardware supports 5GHz
 */
bool hdd_is_5g_supported(hdd_context_t *hdd_ctx)
{
	if (!hdd_ctx || !hdd_ctx->config)
		return true;

	if (hdd_ctx->curr_band != SIR_BAND_2_4_GHZ)
		return true;
	else
		return false;
}

static int hdd_wiphy_init(hdd_context_t *hdd_ctx)
{
	struct wiphy *wiphy;
	int ret_val;

	wiphy = hdd_ctx->wiphy;

	/*
	 * The channel information in
	 * wiphy needs to be initialized before wiphy registration
	 */
	ret_val = hdd_regulatory_init(hdd_ctx, wiphy);
	if (ret_val) {
		hdd_err("regulatory init failed");
		return ret_val;
	}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 11, 0))
	wiphy->wowlan = &wowlan_support_reg_init;
#else
	wiphy->wowlan.flags = WIPHY_WOWLAN_ANY |
			      WIPHY_WOWLAN_MAGIC_PKT |
			      WIPHY_WOWLAN_DISCONNECT |
			      WIPHY_WOWLAN_SUPPORTS_GTK_REKEY |
			      WIPHY_WOWLAN_GTK_REKEY_FAILURE |
			      WIPHY_WOWLAN_EAP_IDENTITY_REQ |
			      WIPHY_WOWLAN_4WAY_HANDSHAKE |
			      WIPHY_WOWLAN_RFKILL_RELEASE;

	wiphy->wowlan.n_patterns = (WOW_MAX_FILTER_LISTS *
				    WOW_MAX_FILTERS_PER_LIST);
	wiphy->wowlan.pattern_min_len = WOW_MIN_PATTERN_SIZE;
	wiphy->wowlan.pattern_max_len = WOW_MAX_PATTERN_SIZE;
#endif

	/* registration of wiphy dev with cfg80211 */
	ret_val = wlan_hdd_cfg80211_register(wiphy);
	if (0 > ret_val) {
		hdd_err("wiphy registration failed");
		return ret_val;
	}

	pld_increment_driver_load_cnt(hdd_ctx->parent_dev);

	hdd_program_country_code(hdd_ctx);

	return ret_val;
}

/**
 * hdd_pld_request_bus_bandwidth() - Function to control bus bandwidth
 * @hdd_ctx - handle to hdd context
 * @tx_packets - transmit packet count
 * @rx_packets - receive packet count
 *
 * The function controls the bus bandwidth and dynamic control of
 * tcp delayed ack configuration
 *
 * Returns: None
 */
#ifdef MSM_PLATFORM
/**
 * hdd_display_periodic_stats() - Function to display periodic stats
 * @hdd_ctx - handle to hdd context
 * @bool data_in_interval - true, if data detected in bw time interval
 *
 * The periodicity is determined by hdd_ctx->config->periodic_stats_disp_time.
 * Stats show up in wlan driver logs.
 *
 * Returns: None
 */
static inline void hdd_display_periodic_stats(hdd_context_t *hdd_ctx, bool data_in_interval)
{
	static uint32_t counter;
	static bool data_in_time_period;
	ol_txrx_pdev_handle pdev;
	uint32_t bw_compute_interval;

	if (hdd_ctx->config->periodic_stats_disp_time == 0)
		return;

	pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	if (!pdev) {
		hdd_err("pdev is NULL");
		return;
	}

	counter++;
	if (data_in_interval == true)
		data_in_time_period = data_in_interval;

	bw_compute_interval = GET_BW_COMPUTE_INTV(hdd_ctx->config);
	if (counter * bw_compute_interval >=
		hdd_ctx->config->periodic_stats_disp_time * 1000) {
		if (data_in_time_period) {
			ol_txrx_display_stats(WLAN_TXRX_STATS,
				QDF_STATS_VERB_LVL_LOW);
			wlan_hdd_display_netif_queue_history(hdd_ctx,
						QDF_STATS_VERB_LVL_LOW);
			qdf_dp_trace_dump_stats();
		}
		counter = 0;
		data_in_time_period = false;
	}
}
static void hdd_pld_request_bus_bandwidth(hdd_context_t *hdd_ctx,
					  const uint64_t tx_packets,
					  const uint64_t rx_packets)
{
	uint64_t total_pkts = tx_packets + rx_packets;
	uint64_t temp_rx = 0;
	uint64_t temp_tx = 0;
	enum pld_bus_width_type next_vote_level = PLD_BUS_WIDTH_NONE;
	static enum wlan_tp_level next_rx_level = WLAN_SVC_TP_NONE;
	enum wlan_tp_level next_tx_level = WLAN_SVC_TP_NONE;
	uint32_t delack_timer_cnt = hdd_ctx->config->tcp_delack_timer_count;
	uint16_t index = 0;
	bool vote_level_change = false;
	bool rx_level_change = false;
	bool tx_level_change = false;

	if (total_pkts > hdd_ctx->config->busBandwidthHighThreshold)
		next_vote_level = PLD_BUS_WIDTH_HIGH;
	else if (total_pkts > hdd_ctx->config->busBandwidthMediumThreshold)
		next_vote_level = PLD_BUS_WIDTH_MEDIUM;
	else if (total_pkts > hdd_ctx->config->busBandwidthLowThreshold)
		next_vote_level = PLD_BUS_WIDTH_LOW;
	else
		next_vote_level = PLD_BUS_WIDTH_NONE;

	if (hdd_ctx->cur_vote_level != next_vote_level) {
		hdd_debug("trigger level %d, tx_packets: %lld, rx_packets: %lld",
			 next_vote_level, tx_packets, rx_packets);
		hdd_ctx->cur_vote_level = next_vote_level;
		vote_level_change = true;
		pld_request_bus_bandwidth(hdd_ctx->parent_dev, next_vote_level);
		if (next_vote_level == PLD_BUS_WIDTH_LOW) {
			if (hdd_ctx->hbw_requested) {
				pld_remove_pm_qos(hdd_ctx->parent_dev);
				hdd_ctx->hbw_requested = false;
			}
			if (cds_sched_handle_throughput_req(false))
				hdd_warn("low bandwidth set rx affinity fail");
		} else {
			if (!hdd_ctx->hbw_requested) {
				pld_request_pm_qos(hdd_ctx->parent_dev, 1);
				hdd_ctx->hbw_requested = true;
			}

			if (cds_sched_handle_throughput_req(true))
				hdd_warn("high bandwidth set rx affinity fail");
		}
		hdd_napi_apply_throughput_policy(hdd_ctx, tx_packets,
						 rx_packets);
	}

	qdf_dp_trace_throttle_live_mode(
		(next_vote_level > PLD_BUS_WIDTH_NONE) ? true : false);

	/* fine-tuning parameters for RX Flows */
	temp_rx = (rx_packets + hdd_ctx->prev_rx) / 2;

	hdd_ctx->prev_rx = rx_packets;

	if (temp_rx < hdd_ctx->config->busBandwidthLowThreshold)
		hdd_disable_rx_ol_for_low_tput(hdd_ctx, true);
	else
		hdd_disable_rx_ol_for_low_tput(hdd_ctx, false);

	if (temp_rx > hdd_ctx->config->tcpDelackThresholdHigh) {
		if ((hdd_ctx->cur_rx_level != WLAN_SVC_TP_HIGH) &&
		   (++hdd_ctx->rx_high_ind_cnt == delack_timer_cnt)) {
			next_rx_level = WLAN_SVC_TP_HIGH;
		}
	} else {
		hdd_ctx->rx_high_ind_cnt = 0;
		next_rx_level = WLAN_SVC_TP_LOW;
	}

	if (hdd_ctx->cur_rx_level != next_rx_level) {
		struct wlan_rx_tp_data rx_tp_data = {0};

		hdd_debug("TCP DELACK trigger level %d, average_rx: %llu",
		       next_rx_level, temp_rx);
		hdd_ctx->cur_rx_level = next_rx_level;
		rx_level_change = true;
		/* Send throughput indication only if it is enabled.
		 * Disabling tcp_del_ack will revert the tcp stack behavior
		 * to default delayed ack. Note that this will disable the
		 * dynamic delayed ack mechanism across the system
		 */
		if (hdd_ctx->tcp_delack_on)
			rx_tp_data.rx_tp_flags |= TCP_DEL_ACK_IND;

		if (hdd_ctx->config->enable_tcp_adv_win_scale)
			rx_tp_data.rx_tp_flags |= TCP_ADV_WIN_SCL;

		rx_tp_data.level = next_rx_level;
		wlan_hdd_send_svc_nlink_msg(hdd_ctx->radio_index,
				WLAN_SVC_WLAN_TP_IND, &rx_tp_data,
				sizeof(rx_tp_data));
	}

	/* fine-tuning parameters for TX Flows */
	temp_tx = (tx_packets + hdd_ctx->prev_tx) / 2;
	hdd_ctx->prev_tx = tx_packets;
	if (temp_tx > hdd_ctx->config->tcp_tx_high_tput_thres)
		next_tx_level = WLAN_SVC_TP_HIGH;
	else
		next_tx_level = WLAN_SVC_TP_LOW;

	if ((hdd_ctx->config->enable_tcp_limit_output) &&
	    (hdd_ctx->cur_tx_level != next_tx_level)) {
		hdd_debug("change TCP TX trigger level %d, average_tx: %llu",
				next_tx_level, temp_tx);
		hdd_ctx->cur_tx_level = next_tx_level;
		tx_level_change = true;
		wlan_hdd_send_svc_nlink_msg(hdd_ctx->radio_index,
				WLAN_SVC_WLAN_TP_TX_IND,
				&next_tx_level,
				sizeof(next_tx_level));
	}

	index = hdd_ctx->hdd_txrx_hist_idx;
	if (vote_level_change || tx_level_change || rx_level_change) {
		hdd_ctx->hdd_txrx_hist[index].next_tx_level = next_tx_level;
		hdd_ctx->hdd_txrx_hist[index].next_rx_level = next_rx_level;
		hdd_ctx->hdd_txrx_hist[index].next_vote_level = next_vote_level;
		hdd_ctx->hdd_txrx_hist[index].interval_rx = rx_packets;
		hdd_ctx->hdd_txrx_hist[index].interval_tx = tx_packets;
		hdd_ctx->hdd_txrx_hist[index].qtime = qdf_get_log_timestamp();
		hdd_ctx->hdd_txrx_hist_idx++;
		hdd_ctx->hdd_txrx_hist_idx &= NUM_TX_RX_HISTOGRAM_MASK;
	}

	hdd_display_periodic_stats(hdd_ctx, (total_pkts > 0) ? true : false);
}

#define HDD_BW_GET_DIFF(_x, _y) (unsigned long)((ULONG_MAX - (_y)) + (_x) + 1)
static void hdd_bus_bw_compute_cbk(void *priv)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *)priv;
	hdd_adapter_t *adapter = NULL;
	hdd_adapter_t *sap_adapter = NULL;
	uint64_t tx_packets = 0, rx_packets = 0;
	uint64_t fwd_tx_packets = 0, fwd_rx_packets = 0;
	uint64_t fwd_tx_packets_diff = 0, fwd_rx_packets_diff = 0;
	uint64_t total_tx = 0, total_rx = 0;
	hdd_adapter_list_node_t *adapterNode = NULL;
	QDF_STATUS status = 0;
	A_STATUS ret;
	bool connected = false;
	uint32_t ipa_tx_packets = 0, ipa_rx_packets = 0;

	if (wlan_hdd_validate_context(hdd_ctx))
		return;

	for (status = hdd_get_front_adapter(hdd_ctx, &adapterNode);
	     NULL != adapterNode && QDF_STATUS_SUCCESS == status;
	     status =
		     hdd_get_next_adapter(hdd_ctx, adapterNode, &adapterNode)) {

		if (adapterNode->pAdapter == NULL)
			continue;
		adapter = adapterNode->pAdapter;
		/*
		 * Validate magic so we don't end up accessing
		 * an invalid adapter.
		 */
		if (adapter->magic != WLAN_HDD_ADAPTER_MAGIC)
			continue;

		if ((adapter->device_mode == QDF_STA_MODE ||
		     adapter->device_mode == QDF_P2P_CLIENT_MODE) &&
		    WLAN_HDD_GET_STATION_CTX_PTR(adapter)->conn_info.connState
		    != eConnectionState_Associated) {

			continue;
		}

		if ((adapter->device_mode == QDF_SAP_MODE ||
		     adapter->device_mode == QDF_P2P_GO_MODE) &&
		    WLAN_HDD_GET_AP_CTX_PTR(adapter)->bApActive == false) {

			continue;
		}

		if (adapter->device_mode == QDF_SAP_MODE)
			sap_adapter = adapter;

		tx_packets += HDD_BW_GET_DIFF(adapter->stats.tx_packets,
					      adapter->prev_tx_packets);
		rx_packets += HDD_BW_GET_DIFF(adapter->stats.rx_packets,
					      adapter->prev_rx_packets);

		if (adapter->device_mode == QDF_SAP_MODE ||
				adapter->device_mode == QDF_P2P_GO_MODE ||
				adapter->device_mode == QDF_IBSS_MODE) {

			ret = ol_get_intra_bss_fwd_pkts_count(
				adapter->sessionId,
				&fwd_tx_packets, &fwd_rx_packets);
			if (ret == A_OK) {
				fwd_tx_packets_diff += HDD_BW_GET_DIFF(
					fwd_tx_packets,
					adapter->prev_fwd_tx_packets);
				fwd_rx_packets_diff += HDD_BW_GET_DIFF(
					fwd_tx_packets,
					adapter->prev_fwd_rx_packets);
			}
		}

		total_rx += adapter->stats.rx_packets;
		total_tx += adapter->stats.tx_packets;

		spin_lock_bh(&hdd_ctx->bus_bw_lock);
		adapter->prev_tx_packets = adapter->stats.tx_packets;
		adapter->prev_rx_packets = adapter->stats.rx_packets;
		adapter->prev_fwd_tx_packets = fwd_tx_packets;
		adapter->prev_fwd_rx_packets = fwd_rx_packets;
		spin_unlock_bh(&hdd_ctx->bus_bw_lock);
		connected = true;
	}

	if (!connected) {
		hdd_err("bus bandwidth timer running in disconnected state");
		return;
	}

	/* add intra bss forwarded tx and rx packets */
	tx_packets += fwd_tx_packets_diff;
	rx_packets += fwd_rx_packets_diff;

	if (hdd_ipa_is_fw_wdi_actived(hdd_ctx)) {
		hdd_ipa_uc_stat_query(hdd_ctx, &ipa_tx_packets,
				      &ipa_rx_packets);
		tx_packets += (uint64_t)ipa_tx_packets;
		rx_packets += (uint64_t)ipa_rx_packets;

		if (sap_adapter) {
			sap_adapter->stats.tx_packets += ipa_tx_packets;
			sap_adapter->stats.rx_packets += ipa_rx_packets;
		}

		hdd_ipa_set_perf_level(hdd_ctx, tx_packets, rx_packets);
		hdd_ipa_uc_stat_request(hdd_ctx, 2);
	}

	hdd_pld_request_bus_bandwidth(hdd_ctx, tx_packets, rx_packets);

	qdf_mc_timer_start(&hdd_ctx->bus_bw_timer,
			   hdd_ctx->config->busBandwidthComputeInterval);
}

int hdd_bus_bandwidth_init(hdd_context_t *hdd_ctx)
{
	ENTER();
	spin_lock_init(&hdd_ctx->bus_bw_lock);

	qdf_mc_timer_init(&hdd_ctx->bus_bw_timer,
			  QDF_TIMER_TYPE_SW,
			  hdd_bus_bw_compute_cbk, (void *)hdd_ctx);

	EXIT();
	return 0;
}

void hdd_bus_bandwidth_destroy(hdd_context_t *hdd_ctx)
{
	ENTER();
	if (qdf_mc_timer_get_current_state(&hdd_ctx->bus_bw_timer) ==
	    QDF_TIMER_STATE_RUNNING &&
	    hdd_ctx->config->enable_tcp_delack)
		hdd_reset_tcp_delack(hdd_ctx);

	hdd_bus_bw_compute_timer_stop(hdd_ctx);
	qdf_mc_timer_destroy(&hdd_ctx->bus_bw_timer);
	EXIT();
}
#endif

/**
 * wlan_hdd_init_tx_rx_histogram() - init tx/rx histogram stats
 * @hdd_ctx: hdd context
 *
 * Return: 0 for success or error code
 */
static int wlan_hdd_init_tx_rx_histogram(hdd_context_t *hdd_ctx)
{
	hdd_ctx->hdd_txrx_hist = qdf_mem_malloc(
		(sizeof(struct hdd_tx_rx_histogram) * NUM_TX_RX_HISTOGRAM));
	if (hdd_ctx->hdd_txrx_hist == NULL) {
		hdd_err("Failed malloc for hdd_txrx_hist");
		return -ENOMEM;
	}
	return 0;
}

/**
 * wlan_hdd_deinit_tx_rx_histogram() - deinit tx/rx histogram stats
 * @hdd_ctx: hdd context
 *
 * Return: none
 */
void wlan_hdd_deinit_tx_rx_histogram(hdd_context_t *hdd_ctx)
{
	if (!hdd_ctx || hdd_ctx->hdd_txrx_hist == NULL)
		return;

	qdf_mem_free(hdd_ctx->hdd_txrx_hist);
	hdd_ctx->hdd_txrx_hist = NULL;
}

#ifdef WLAN_DEBUG
static uint8_t *convert_level_to_string(uint32_t level)
{
	switch (level) {
	/* initialize the wlan sub system */
	case WLAN_SVC_TP_NONE:
		return "NONE";
	case WLAN_SVC_TP_LOW:
		return "LOW";
	case WLAN_SVC_TP_MEDIUM:
		return "MED";
	case WLAN_SVC_TP_HIGH:
		return "HIGH";
	default:
		return "INVAL";
	}
}
#endif


/**
 * wlan_hdd_display_tx_rx_histogram() - display tx rx histogram
 * @hdd_ctx: hdd context
 *
 * Return: none
 */
void wlan_hdd_display_tx_rx_histogram(hdd_context_t *hdd_ctx)
{
	int i;

#ifdef MSM_PLATFORM
	hdd_debug("BW compute Interval: %dms",
		hdd_ctx->config->busBandwidthComputeInterval);
	hdd_debug("BW High TH: %d BW Med TH: %d BW Low TH: %d",
		hdd_ctx->config->busBandwidthHighThreshold,
		hdd_ctx->config->busBandwidthMediumThreshold,
		hdd_ctx->config->busBandwidthLowThreshold);
	hdd_debug("Enable TCP DEL ACK: %d",
		hdd_ctx->tcp_delack_on);
	hdd_debug("TCP DEL High TH: %d TCP DEL Low TH: %d",
		hdd_ctx->config->tcpDelackThresholdHigh,
		hdd_ctx->config->tcpDelackThresholdLow);
	hdd_debug("TCP TX HIGH TP TH: %d (Use to set tcp_output_bytes_limit)",
		hdd_ctx->config->tcp_tx_high_tput_thres);
#endif

	hdd_debug("Total entries: %d Current index: %d",
		NUM_TX_RX_HISTOGRAM, hdd_ctx->hdd_txrx_hist_idx);

	hdd_debug("[index][timestamp]: interval_rx, interval_tx, bus_bw_level, RX TP Level, TX TP Level");

	for (i = 0; i < NUM_TX_RX_HISTOGRAM; i++) {
		/* using hdd_log to avoid printing function name */
		if (hdd_ctx->hdd_txrx_hist[i].qtime > 0)
			hdd_log(QDF_TRACE_LEVEL_DEBUG,
				"[%3d][%15llu]: %6llu, %6llu, %s, %s, %s",
				i, hdd_ctx->hdd_txrx_hist[i].qtime,
				hdd_ctx->hdd_txrx_hist[i].interval_rx,
				hdd_ctx->hdd_txrx_hist[i].interval_tx,
				convert_level_to_string(
					hdd_ctx->hdd_txrx_hist[i].
						next_vote_level),
				convert_level_to_string(
					hdd_ctx->hdd_txrx_hist[i].
						next_rx_level),
				convert_level_to_string(
					hdd_ctx->hdd_txrx_hist[i].
						next_tx_level));
	}
}

/**
 * wlan_hdd_clear_tx_rx_histogram() - clear tx rx histogram
 * @hdd_ctx: hdd context
 *
 * Return: none
 */
void wlan_hdd_clear_tx_rx_histogram(hdd_context_t *hdd_ctx)
{
	hdd_ctx->hdd_txrx_hist_idx = 0;
	qdf_mem_zero(hdd_ctx->hdd_txrx_hist,
		(sizeof(struct hdd_tx_rx_histogram) * NUM_TX_RX_HISTOGRAM));
}


/* length of the netif queue log needed per adapter */
#define ADAP_NETIFQ_LOG_LEN ((20 * WLAN_REASON_TYPE_MAX) + 50)

/**
 *
 * hdd_display_netif_queue_history_compact() - display compact netifq history
 * @pHddCtx: hdd context
 *
 * Return: none
 */
static void
hdd_display_netif_queue_history_compact(hdd_context_t *hdd_ctx)
{
	int adapter_num = 0;
	int i;
	int bytes_written;
	uint32_t tbytes;
	qdf_time_t total, pause, unpause, curr_time, delta;
	QDF_STATUS status;
	char temp_str[20 * WLAN_REASON_TYPE_MAX];
	char *comb_log_str;
	uint32_t comb_log_str_size;
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	hdd_adapter_t *adapter = NULL;
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;

	if (pdev == NULL) {
		hdd_err("pdev is null");
		return;
	}

	comb_log_str_size = (ADAP_NETIFQ_LOG_LEN * MAX_NUMBER_OF_ADAPTERS) + 1;
	comb_log_str = qdf_mem_malloc(comb_log_str_size);
	if (!comb_log_str) {
		hdd_err("failed to alloc comb_log_str");
		return;
	}

	bytes_written = 0;
	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);
	while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
		adapter = adapter_node->pAdapter;

		curr_time = qdf_system_ticks();
		total = curr_time - adapter->start_time;
		delta = curr_time - adapter->last_time;

		if (adapter->pause_map) {
			pause = adapter->total_pause_time + delta;
			unpause = adapter->total_unpause_time;
		} else {
			unpause = adapter->total_unpause_time + delta;
			pause = adapter->total_pause_time;
		}

		tbytes = 0;
		qdf_mem_set(temp_str, 0, sizeof(temp_str));
		for (i = WLAN_CONTROL_PATH; i < WLAN_REASON_TYPE_MAX; i++) {
			if (adapter->queue_oper_stats[i].pause_count == 0)
				continue;
			tbytes +=
				snprintf(
					&temp_str[tbytes],
					(tbytes >= sizeof(temp_str) ?
					0 : sizeof(temp_str) - tbytes),
					"%d(%d,%d) ",
					i,
					adapter->queue_oper_stats[i].
								pause_count,
					adapter->queue_oper_stats[i].
								unpause_count);

		}
		if (tbytes >= sizeof(temp_str))
			hdd_warn("log truncated");

		bytes_written += snprintf(&comb_log_str[bytes_written],
			bytes_written >= comb_log_str_size ? 0 :
					comb_log_str_size - bytes_written,
			"[%d %d] (%d) %u/%ums %s|",
			adapter->sessionId, adapter->device_mode,
			adapter->pause_map,
			qdf_system_ticks_to_msecs(pause),
			qdf_system_ticks_to_msecs(total),
			temp_str);

		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
		adapter_num++;
	}

	/* using QDF_TRACE to avoid printing function name */
	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO_LOW,
		"STATS |%d/%d|%s",
		pdev->tx_desc.num_free,
		pdev->tx_desc.pool_size, comb_log_str);

	if (bytes_written >= comb_log_str_size)
		hdd_warn("log string truncated");

	qdf_mem_free(comb_log_str);
}

/**
 * wlan_hdd_display_netif_queue_history() - display netif queue history
 * @pHddCtx: hdd context
 *
 * Return: none
 */
void wlan_hdd_display_netif_queue_history(hdd_context_t *hdd_ctx,
					enum qdf_stats_verb_lvl verb_lvl)
{

	hdd_adapter_t *adapter = NULL;
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	QDF_STATUS status;
	int i;
	qdf_time_t total, pause, unpause, curr_time, delta;

	if (verb_lvl == QDF_STATS_VERB_LVL_LOW) {
		hdd_display_netif_queue_history_compact(hdd_ctx);
		return;
	}

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);
	while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
		adapter = adapter_node->pAdapter;

		hdd_debug("Netif queue operation statistics:");
		hdd_debug("Session_id %d device mode %d",
			adapter->sessionId, adapter->device_mode);
		hdd_debug("Current pause_map value %x", adapter->pause_map);
		curr_time = qdf_system_ticks();
		total = curr_time - adapter->start_time;
		delta = curr_time - adapter->last_time;
		if (adapter->pause_map) {
			pause = adapter->total_pause_time + delta;
			unpause = adapter->total_unpause_time;
		} else {
			unpause = adapter->total_unpause_time + delta;
			pause = adapter->total_pause_time;
		}
		hdd_debug("Total: %ums Pause: %ums Unpause: %ums",
			qdf_system_ticks_to_msecs(total),
			qdf_system_ticks_to_msecs(pause),
			qdf_system_ticks_to_msecs(unpause));
		hdd_debug("reason_type: pause_cnt: unpause_cnt: pause_time");

		for (i = WLAN_CONTROL_PATH; i < WLAN_REASON_TYPE_MAX; i++) {
			qdf_time_t pause_delta = 0;

			if (adapter->pause_map & (1 << i))
				pause_delta = delta;

			/* using hdd_log to avoid printing function name */
			hdd_log(QDF_TRACE_LEVEL_DEBUG,
				"%s: %d: %d: %ums",
				hdd_reason_type_to_string(i),
				adapter->queue_oper_stats[i].pause_count,
				adapter->queue_oper_stats[i].unpause_count,
				qdf_system_ticks_to_msecs(
				adapter->queue_oper_stats[i].total_pause_time +
				pause_delta));
		}

		hdd_debug("Netif queue operation history:");
		hdd_debug("Total entries: %d current index %d",
			WLAN_HDD_MAX_HISTORY_ENTRY, adapter->history_index);

		hdd_debug("index: time: action_type: reason_type: pause_map");

		for (i = 0; i < WLAN_HDD_MAX_HISTORY_ENTRY; i++) {
			/* using hdd_log to avoid printing function name */
			if (adapter->queue_oper_history[i].time == 0)
				continue;
			hdd_log(QDF_TRACE_LEVEL_DEBUG,
				"%d: %u: %s: %s: %x",
				i, qdf_system_ticks_to_msecs(
					adapter->queue_oper_history[i].time),
				hdd_action_type_to_string(
				adapter->queue_oper_history[i].netif_action),
				hdd_reason_type_to_string(
				adapter->queue_oper_history[i].netif_reason),
				adapter->queue_oper_history[i].pause_map);
		}

		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}
}

/**
 * wlan_hdd_clear_netif_queue_history() - clear netif queue operation history
 * @hdd_ctx: hdd context
 *
 * Return: none
 */
void wlan_hdd_clear_netif_queue_history(hdd_context_t *hdd_ctx)
{
	hdd_adapter_t *adapter = NULL;
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	QDF_STATUS status;

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);
	while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
		adapter = adapter_node->pAdapter;

		qdf_mem_zero(adapter->queue_oper_stats,
					sizeof(adapter->queue_oper_stats));
		qdf_mem_zero(adapter->queue_oper_history,
					sizeof(adapter->queue_oper_history));
		adapter->history_index = 0;
		adapter->start_time = adapter->last_time = qdf_system_ticks();
		adapter->total_pause_time = 0;
		adapter->total_unpause_time = 0;
		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}
}

/**
 * hdd_11d_scan_done() - callback for 11d scan completion of flushing results
 * @halHandle:	Hal handle
 * @pContext:	Pointer to the context
 * @sessionId:	Session ID
 * @scanId:	Scan ID
 * @status:	Status
 *
 * This is the callback to be executed when 11d scan is completed to flush out
 * the scan results
 *
 * 11d scan is done during driver load and is a passive scan on all
 * channels supported by the device, 11d scans may find some APs on
 * frequencies which are forbidden to be used in the regulatory domain
 * the device is operating in. If these APs are notified to the supplicant
 * it may try to connect to these APs, thus flush out all the scan results
 * which are present in SME after 11d scan is done.
 *
 * Return:  QDF_STATUS_SUCCESS
 */
static QDF_STATUS hdd_11d_scan_done(tHalHandle halHandle, void *pContext,
				    uint8_t sessionId, uint32_t scanId,
				    eCsrScanStatus status)
{
	ENTER();

	sme_scan_flush_result(halHandle);

	EXIT();

	return QDF_STATUS_SUCCESS;
}

#ifdef WLAN_FEATURE_OFFLOAD_PACKETS
/**
 * hdd_init_offloaded_packets_ctx() - Initialize offload packets context
 * @hdd_ctx: hdd global context
 *
 * Return: none
 */
static void hdd_init_offloaded_packets_ctx(hdd_context_t *hdd_ctx)
{
	uint8_t i;

	mutex_init(&hdd_ctx->op_ctx.op_lock);
	for (i = 0; i < MAXNUM_PERIODIC_TX_PTRNS; i++) {
		hdd_ctx->op_ctx.op_table[i].request_id = MAX_REQUEST_ID;
		hdd_ctx->op_ctx.op_table[i].pattern_id = i;
	}
}
#else
static void hdd_init_offloaded_packets_ctx(hdd_context_t *hdd_ctx)
{
}
#endif

#ifdef WLAN_FEATURE_WOW_PULSE
/**
 * wlan_hdd_set_wow_pulse() - call SME to send wmi cmd of wow pulse
 * @phddctx: hdd_context_t structure pointer
 * @enable: enable or disable this behaviour
 *
 * Return: int
 */
static int wlan_hdd_set_wow_pulse(hdd_context_t *phddctx, bool enable)
{
	struct hdd_config *pcfg_ini = phddctx->config;
	struct wow_pulse_mode wow_pulse_set_info;
	QDF_STATUS status;

	hdd_debug("wow pulse enable flag is %d", enable);

	if (false == phddctx->config->wow_pulse_support)
		return 0;

	/* prepare the request to send to SME */
	if (enable == true) {
		wow_pulse_set_info.wow_pulse_enable = true;
		wow_pulse_set_info.wow_pulse_pin =
				pcfg_ini->wow_pulse_pin;
		wow_pulse_set_info.wow_pulse_interval_low =
				pcfg_ini->wow_pulse_interval_low;
		wow_pulse_set_info.wow_pulse_interval_high =
				pcfg_ini->wow_pulse_interval_high;
	} else {
		wow_pulse_set_info.wow_pulse_enable = false;
		wow_pulse_set_info.wow_pulse_pin = 0;
		wow_pulse_set_info.wow_pulse_interval_low = 0;
		wow_pulse_set_info.wow_pulse_interval_high = 0;
	}
	hdd_debug("enable %d pin %d low %d high %d",
		wow_pulse_set_info.wow_pulse_enable,
		wow_pulse_set_info.wow_pulse_pin,
		wow_pulse_set_info.wow_pulse_interval_low,
		wow_pulse_set_info.wow_pulse_interval_high);

	status = sme_set_wow_pulse(&wow_pulse_set_info);
	if (QDF_STATUS_E_FAILURE == status) {
		hdd_debug("sme_set_wow_pulse failure!");
		return -EIO;
	}
	hdd_debug("sme_set_wow_pulse success!");
	return 0;
}
#else
static inline int wlan_hdd_set_wow_pulse(hdd_context_t *phddctx, bool enable)
{
	return 0;
}
#endif

#ifdef WLAN_FEATURE_FASTPATH
/**
 * hdd_enable_fastpath() - Enable fastpath if enabled in config INI
 * @hdd_cfg: hdd config
 * @context: lower layer context
 *
 * Return: none
 */
void hdd_enable_fastpath(struct hdd_config *hdd_cfg,
				void *context)
{
	if (hdd_cfg->fastpath_enable)
		hif_enable_fastpath(context);
}
#endif

#if defined(FEATURE_WLAN_CH_AVOID)
/**
 * hdd_set_thermal_level_cb() - set thermal level callback function
 * @context:	hdd context pointer
 * @level:	thermal level
 *
 * Change IPA data path to SW path when the thermal throttle level greater
 * than 0, and restore the original data path when throttle level is 0
 *
 * Return: none
 */
static void hdd_set_thermal_level_cb(void *context, u_int8_t level)
{
	hdd_context_t *hdd_ctx = context;

	/* Change IPA to SW path when throttle level greater than 0 */
	if (level > THROTTLE_LEVEL_0)
		hdd_ipa_send_mcc_scc_msg(hdd_ctx, true);
	else
		/* restore original concurrency mode */
		hdd_ipa_send_mcc_scc_msg(hdd_ctx, hdd_ctx->mcc_mode);
}

/**
 * hdd_restart_sap() - Restarts SAP on the given channel
 * @adapter: AP adapter
 * @channel: Channel
 *
 * Restarts the SAP interface by invoking the function which executes the
 * callback to perform channel switch using (E)CSA.
 *
 * Return: None
 */
static void hdd_restart_sap(hdd_adapter_t *adapter, uint8_t channel)
{
	hdd_ap_ctx_t *hdd_ap_ctx;
	tHalHandle *hal_handle;

	if (!adapter) {
		hdd_err("invalid adapter");
		return;
	}

	hdd_ap_ctx = WLAN_HDD_GET_AP_CTX_PTR(adapter);

	hal_handle = WLAN_HDD_GET_HAL_CTX(adapter);
	if (!hal_handle) {
		hdd_err("invalid HAL handle");
		return;
	}

	hdd_ap_ctx->sapConfig.channel = channel;
	hdd_ap_ctx->sapConfig.ch_params.ch_width = CH_WIDTH_MAX;

	hdd_debug("chan:%d width:%d",
		channel, hdd_ap_ctx->sapConfig.ch_width_orig);

	cds_set_channel_params(hdd_ap_ctx->sapConfig.channel,
			hdd_ap_ctx->sapConfig.sec_ch,
			&hdd_ap_ctx->sapConfig.ch_params);

	cds_change_sap_channel_with_csa(adapter, hdd_ap_ctx);
}

/**
 * hdd_send_svc_coex_info() - set and send coex info to application
 * @hdd_ctx: pointer to hdd context
 * @adapter: HDD adapter
 * @restart_chan: new safe channel which SAP will switch to
 *
 * This function set coex info depend on different case, then
 * send to application
 *
 * Return: None
 */
static void hdd_send_svc_coex_info(hdd_context_t *hdd_ctx,
				   hdd_adapter_t *adapter,
				   uint8_t restart_chan)
{
	struct lte_coex_info coex_info;
	uint8_t op_chan;

	coex_info.recfg_channel = restart_chan;
	coex_info.recfg_mode = WLAN_SVC_RECFG_ECSA;
	op_chan = adapter->sessionCtx.ap.operatingChannel;
	/*
	 * It needs to restart hostapd to config new safe channel
	 * for following case due to DFS channel limitation.
	 * 1: Only one SAP which is starting on DFS channel but cac
	 *    doesn't complete yet, need to check SAP started state
	 * For below two cases, if switch with eCSA mode with one SAP
	 * firstly, it will cause MCC which is forbidden on DFS channel.
	 * 2: SAP+SAP, one SAP started on DFS channel
	 * 3: SAP+SAP, recfg_channel is DFS channel
	 */
	if (CHANNEL_STATE_DFS == cds_get_channel_state(op_chan)) {
		if (hdd_get_con_sap_adapter(adapter, true) ||
		    !wlansap_check_sap_started(
					WLAN_HDD_GET_SAP_CTX_PTR(adapter)))
			coex_info.recfg_mode = WLAN_SVC_RECFG_RESTART_HOSTAPD;
	}

	if (CHANNEL_STATE_DFS == cds_get_channel_state(restart_chan) &&
	    hdd_get_con_sap_adapter(adapter, true))
		coex_info.recfg_mode = WLAN_SVC_RECFG_RESTART_HOSTAPD;

	hdd_debug("sending coex indication with sap recfg channel: %d, "
		  "ecsa mode: %d", coex_info.recfg_channel,
		  coex_info.recfg_mode);

	wlan_hdd_send_svc_nlink_msg(hdd_ctx->radio_index, WLAN_SVC_LTE_COEX_IND,
				    &coex_info, sizeof(struct lte_coex_info));
}

/**
 * hdd_store_sap_restart_channel() - store sap restart channel
 * @restart_chan: restart channel
 * @restart_chan_store: pointer to restart channel store
 *
 * The function will store new sap restart channel.
 *
 * Return - none
 */
static void
hdd_store_sap_restart_channel(uint8_t restart_chan, uint8_t *restart_chan_store)
{
	uint8_t i;

	for (i = 0; i < SAP_MAX_NUM_SESSION; i++) {
		if (*(restart_chan_store + i) == restart_chan)
			return;

		if (*(restart_chan_store + i))
			continue;

		*(restart_chan_store + i) = restart_chan;
		return;
	}
}

/**
 * hdd_unsafe_channel_restart_sap() - restart sap if sap is on unsafe channel
 * @hdd_ctx: hdd context pointer
 *
 * hdd_unsafe_channel_restart_sap check all unsafe channel list
 * and if ACS is enabled, driver will ask userspace to restart the
 * sap. User space on LTE coex indication restart driver.
 *
 * Return - none
 */
void hdd_unsafe_channel_restart_sap(hdd_context_t *hdd_ctxt)
{
	QDF_STATUS status;
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	hdd_adapter_t *adapter_temp;
	uint32_t i;
	bool found = false;
	uint8_t restart_chan;
	uint8_t restart_chan_store[SAP_MAX_NUM_SESSION] = {0};

	status = hdd_get_front_adapter(hdd_ctxt, &adapter_node);
	while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
		adapter_temp = adapter_node->pAdapter;

		if (!adapter_temp) {
			hdd_err("adapter is NULL, moving to next one");
			goto next_adapater;
		}

		if (!((adapter_temp->device_mode == QDF_SAP_MODE) &&
		   (adapter_temp->sessionCtx.ap.sapConfig.acs_cfg.acs_mode))) {
			hdd_debug("skip device mode:%d acs:%d",
				adapter_temp->device_mode,
				adapter_temp->sessionCtx.ap.sapConfig.
				acs_cfg.acs_mode);
			goto next_adapater;
		}

		found = false;
		/*
		 * If STA+SAP is doing SCC & g_sta_sap_scc_on_lte_coex_chan
		 * is set, no need to move SAP.
		 */
		if (cds_is_sta_sap_scc(
			adapter_temp->sessionCtx.ap.operatingChannel) &&
			hdd_ctxt->config->sta_sap_scc_on_lte_coex_chan)
			hdd_debug("SAP is allowed on SCC channel, no need to move SAP");
		else {
			for (i = 0; i < hdd_ctxt->unsafe_channel_count; i++) {
				if (adapter_temp->sessionCtx.ap.operatingChannel ==
					hdd_ctxt->unsafe_channel_list[i]) {
					found = true;
					hdd_debug("operating ch:%d is unsafe",
					adapter_temp->sessionCtx.ap.operatingChannel);
					break;
				}
			}
		}

		if (!found) {
			hdd_store_sap_restart_channel(
				adapter_temp->sessionCtx.ap.operatingChannel,
				restart_chan_store);
			hdd_debug("ch:%d is safe. no need to change channel",
				adapter_temp->sessionCtx.ap.operatingChannel);
			goto next_adapater;
		}

		restart_chan = 0;
		for (i = 0; i < SAP_MAX_NUM_SESSION; i++) {
			if (!restart_chan_store[i])
				continue;

			if (cds_is_force_scc() &&
			    CDS_IS_SAME_BAND_CHANNELS(
						restart_chan_store[i],
						adapter_temp->sessionCtx.ap.
						operatingChannel)) {
				restart_chan = restart_chan_store[i];
				break;
			}
		}
		if (!restart_chan)
			restart_chan =
				wlansap_get_safe_channel_from_pcl_and_acs_range(
					adapter_temp->sessionCtx.ap.sapContext);
		if (!restart_chan) {
			hdd_err("fail to restart SAP");
		} else {
			/* SAP restart due to unsafe channel. While restarting
			 * the SAP, make sure to clear acs_channel, channel to
			 * reset to 0. Otherwise these settings will override
			 * the ACS while restart.
			 */
			hdd_ctxt->acs_policy.acs_channel = AUTO_CHANNEL_SELECT;
			adapter_temp->sessionCtx.ap.sapConfig.channel =
							AUTO_CHANNEL_SELECT;
			hdd_send_svc_coex_info(hdd_ctxt, adapter_temp,
					       restart_chan);
			hdd_debug("driver to start sap: %d",
				hdd_ctxt->config->sap_internal_restart);
			if (hdd_ctxt->config->sap_internal_restart) {
				hdd_restart_sap(adapter_temp, restart_chan);
				hdd_store_sap_restart_channel(
							restart_chan,
							restart_chan_store);
			}
			else
				return;
		}

next_adapater:
		status = hdd_get_next_adapter(hdd_ctxt, adapter_node, &next);
		adapter_node = next;
	}
}

int hdd_clone_local_unsafe_chan(hdd_context_t *hdd_ctx,
	uint16_t **local_unsafe_list, uint16_t *local_unsafe_list_count)
{
	uint32_t size;
	uint16_t *unsafe_list;
	uint16_t chan_count;

	if (!hdd_ctx || !local_unsafe_list_count || !local_unsafe_list_count)
		return -EINVAL;

	chan_count = QDF_MIN(hdd_ctx->unsafe_channel_count,
			     NUM_CHANNELS);
	if (chan_count) {
		size = chan_count * sizeof(hdd_ctx->unsafe_channel_list[0]);
		unsafe_list = qdf_mem_malloc(size);
		if (!unsafe_list) {
			hdd_err("No memory for unsafe chan list size%d",
				size);
			return -ENOMEM;
		}
		qdf_mem_copy(unsafe_list, hdd_ctx->unsafe_channel_list, size);
	} else {
		unsafe_list = NULL;
	}

	*local_unsafe_list = unsafe_list;
	*local_unsafe_list_count = chan_count;

	return 0;
}

bool hdd_local_unsafe_channel_updated(hdd_context_t *hdd_ctx,
	uint16_t *local_unsafe_list, uint16_t local_unsafe_list_count)
{
	int i, j;

	if (local_unsafe_list_count != hdd_ctx->unsafe_channel_count)
		return true;

	for (i = 0; i < local_unsafe_list_count; i++) {
		for (j = 0; j < local_unsafe_list_count; j++)
			if (local_unsafe_list[i] ==
			    hdd_ctx->unsafe_channel_list[j])
				break;
		if (j >= local_unsafe_list_count)
			break;
	}
	if (i >= local_unsafe_list_count) {
		hdd_info("unsafe chan list same");
		return false;
	}

	return true;
}

/**
 * hdd_ch_avoid_cb() - Avoid notified channels from FW handler
 * @adapter:	HDD adapter pointer
 * @indParam:	Channel avoid notification parameter
 *
 * Avoid channel notification from FW handler.
 * FW will send un-safe channel list to avoid over wrapping.
 * hostapd should not use notified channel
 *
 * Return: None
 */
void hdd_ch_avoid_cb(void *hdd_context, void *indi_param)
{
	hdd_context_t *hdd_ctxt;
	tSirChAvoidIndType *ch_avoid_indi;
	uint8_t range_loop;
	enum channel_enum channel_loop, start_channel_idx = INVALID_CHANNEL,
					end_channel_idx = INVALID_CHANNEL;
	uint16_t start_channel;
	uint16_t end_channel;
	v_CONTEXT_t cds_context;
	tHddAvoidFreqList hdd_avoid_freq_list;
	uint32_t i;
	uint16_t *local_unsafe_list;
	uint16_t local_unsafe_list_count;

	/* Basic sanity */
	if (!hdd_context || !indi_param) {
		hdd_err("Invalid arguments");
		return;
	}

	hdd_ctxt = (hdd_context_t *) hdd_context;
	ch_avoid_indi = (tSirChAvoidIndType *) indi_param;
	cds_context = hdd_ctxt->pcds_context;

	/* Make unsafe channel list */
	hdd_debug("band count %d",
	       ch_avoid_indi->avoid_range_count);

	/* generate vendor specific event */
	qdf_mem_zero((void *)&hdd_avoid_freq_list, sizeof(tHddAvoidFreqList));
	for (i = 0; i < ch_avoid_indi->avoid_range_count; i++) {
		if ((RESTART_24G_ONLY == hdd_ctxt->config->
			restart_beaconing_on_chan_avoid_event) &&
			CDS_IS_CHANNEL_5GHZ(ieee80211_frequency_to_channel(
			ch_avoid_indi->avoid_freq_range[i].start_freq))) {
			hdd_debug("skipping 5Ghz LTE Coex unsafe channel range");
			continue;
		}
		hdd_avoid_freq_list.avoidFreqRange[i].startFreq =
			ch_avoid_indi->avoid_freq_range[i].start_freq;
		hdd_avoid_freq_list.avoidFreqRange[i].endFreq =
			ch_avoid_indi->avoid_freq_range[i].end_freq;
	}
	hdd_avoid_freq_list.avoidFreqRangeCount =
		ch_avoid_indi->avoid_range_count;

	if (hdd_clone_local_unsafe_chan(hdd_ctxt,
					&local_unsafe_list,
					&local_unsafe_list_count) != 0)
		return;
	/* clear existing unsafe channel cache */
	hdd_ctxt->unsafe_channel_count = 0;
	qdf_mem_zero(hdd_ctxt->unsafe_channel_list,
					sizeof(hdd_ctxt->unsafe_channel_list));

	for (range_loop = 0; range_loop < ch_avoid_indi->avoid_range_count;
								range_loop++) {
		if (hdd_ctxt->unsafe_channel_count >= NUM_CHANNELS) {
			hdd_warn("LTE Coex unsafe channel list full");
			break;
		}

		start_channel = ieee80211_frequency_to_channel(
			ch_avoid_indi->avoid_freq_range[range_loop].start_freq);
		end_channel   = ieee80211_frequency_to_channel(
			ch_avoid_indi->avoid_freq_range[range_loop].end_freq);
		hdd_debug("start %d : %d, end %d : %d",
			ch_avoid_indi->avoid_freq_range[range_loop].start_freq,
			start_channel,
			ch_avoid_indi->avoid_freq_range[range_loop].end_freq,
			end_channel);

		/* do not process frequency bands that are not mapped to
		 * predefined channels
		 */
		if (start_channel == 0 || end_channel == 0)
			continue;

		for (channel_loop = CHAN_ENUM_1; channel_loop <=
					CHAN_ENUM_184; channel_loop++) {
			if (CDS_CHANNEL_FREQ(channel_loop) >=
						ch_avoid_indi->avoid_freq_range[
						range_loop].start_freq) {
				start_channel_idx = channel_loop;
				break;
			}
		}
		for (channel_loop = CHAN_ENUM_1; channel_loop <=
					CHAN_ENUM_184; channel_loop++) {
			if (CDS_CHANNEL_FREQ(channel_loop) >=
						ch_avoid_indi->avoid_freq_range[
						range_loop].end_freq) {
				end_channel_idx = channel_loop;
				if (CDS_CHANNEL_FREQ(channel_loop) >
						ch_avoid_indi->avoid_freq_range[
						range_loop].end_freq)
					if (end_channel_idx)
						end_channel_idx--;
				break;
			}
		}

		if (start_channel_idx == INVALID_CHANNEL ||
					end_channel_idx == INVALID_CHANNEL)
			continue;

		for (channel_loop = start_channel_idx; channel_loop <=
					end_channel_idx; channel_loop++) {
			hdd_ctxt->unsafe_channel_list[
				hdd_ctxt->unsafe_channel_count++] =
				CDS_CHANNEL_NUM(channel_loop);
			if (hdd_ctxt->unsafe_channel_count >=
							NUM_CHANNELS) {
				hdd_warn("LTECoex unsafe ch list full");
				break;
			}
		}
	}

	hdd_debug("number of unsafe channels is %d ",
	       hdd_ctxt->unsafe_channel_count);

	if (pld_set_wlan_unsafe_channel(hdd_ctxt->parent_dev,
					hdd_ctxt->unsafe_channel_list,
				hdd_ctxt->unsafe_channel_count)) {
		hdd_err("Failed to set unsafe channel");

		/* clear existing unsafe channel cache */
		hdd_ctxt->unsafe_channel_count = 0;
		qdf_mem_zero(hdd_ctxt->unsafe_channel_list,
			sizeof(hdd_ctxt->unsafe_channel_list));
		qdf_mem_free(local_unsafe_list);
		return;
	}

	cds_save_wlan_unsafe_channels(hdd_ctxt->unsafe_channel_list,
			hdd_ctxt->unsafe_channel_count);

	for (channel_loop = 0;
	     channel_loop < hdd_ctxt->unsafe_channel_count; channel_loop++) {
		hdd_debug("channel %d is not safe ",
		       hdd_ctxt->unsafe_channel_list[channel_loop]);
	}

	/*
	 * first update the unsafe channel list to the platform driver and
	 * send the avoid freq event to the application
	 */
	if (hdd_ctxt->config->restart_beaconing_on_chan_avoid_event &&
	    hdd_local_unsafe_channel_updated(hdd_ctxt,
					     local_unsafe_list,
					     local_unsafe_list_count)) {
		wlan_hdd_send_avoid_freq_event(hdd_ctxt, &hdd_avoid_freq_list);

		if (!hdd_ctxt->unsafe_channel_count) {
			hdd_debug("no unsafe channels - not restarting SAP");
			qdf_mem_free(local_unsafe_list);
			return;
		}

		hdd_unsafe_channel_restart_sap(hdd_ctxt);
	}
	qdf_mem_free(local_unsafe_list);
}

/**
 * hdd_init_channel_avoidance() - Initialize channel avoidance
 * @hdd_ctx:	HDD global context
 *
 * Initialize the channel avoidance logic by retrieving the unsafe
 * channel list from the platform driver and plumbing the data
 * down to the lower layers.  Then subscribe to subsequent channel
 * avoidance events.
 *
 * Return: None
 */
static void hdd_init_channel_avoidance(hdd_context_t *hdd_ctx)
{
	uint16_t unsafe_channel_count;
	int index;

	pld_get_wlan_unsafe_channel(hdd_ctx->parent_dev,
				    hdd_ctx->unsafe_channel_list,
				     &(hdd_ctx->unsafe_channel_count),
				     sizeof(uint16_t) * NUM_CHANNELS);

	hdd_debug("num of unsafe channels is %d",
	       hdd_ctx->unsafe_channel_count);

	unsafe_channel_count = QDF_MIN((uint16_t)hdd_ctx->unsafe_channel_count,
				       (uint16_t)NUM_CHANNELS);

	for (index = 0; index < unsafe_channel_count; index++) {
		hdd_debug("channel %d is not safe",
		       hdd_ctx->unsafe_channel_list[index]);

	}

	/* Plug in avoid channel notification callback */
	sme_add_ch_avoid_callback(hdd_ctx->hHal, hdd_ch_avoid_cb);
}
#else
static void hdd_init_channel_avoidance(hdd_context_t *hdd_ctx)
{
}
static void hdd_set_thermal_level_cb(void *context, u_int8_t level)
{
}
#endif /* defined(FEATURE_WLAN_CH_AVOID) */

/**
 * hdd_indicate_mgmt_frame() - Wrapper to indicate management frame to
 * user space
 * @frame_ind: Management frame data to be informed.
 *
 * This function is used to indicate management frame to
 * user space
 *
 * Return: None
 *
 */
void hdd_indicate_mgmt_frame(tSirSmeMgmtFrameInd *frame_ind)
{
	hdd_context_t *hdd_ctx = NULL;
	hdd_adapter_t *adapter = NULL;
	void *cds_context = NULL;
	int i;
	hdd_adapter_list_node_t *adapter_node, *next;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	struct ieee80211_mgmt *mgmt =
		(struct ieee80211_mgmt *)frame_ind->frameBuf;

	/* Get the global VOSS context.*/
	cds_context = cds_get_global_context();
	if (!cds_context) {
		hdd_err("Global CDS context is Null");
		return;
	}
	/* Get the HDD context.*/
	hdd_ctx = (hdd_context_t *)cds_get_context(QDF_MODULE_ID_HDD);

	if (0 != wlan_hdd_validate_context(hdd_ctx))
		return;

	if (frame_ind->frame_len < ieee80211_hdrlen(mgmt->frame_control)) {
		hdd_err(" Invalid frame length");
		return;
	}

	if (SME_SESSION_ID_ANY == frame_ind->sessionId) {
		for (i = 0; i < CSR_ROAM_SESSION_MAX; i++) {
			adapter =
				hdd_get_adapter_by_sme_session_id(hdd_ctx, i);
			if (adapter)
				break;
		}
	} else if (SME_SESSION_ID_BROADCAST == frame_ind->sessionId) {
		status = hdd_get_front_adapter(hdd_ctx, &adapter_node);
		while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
			adapter = adapter_node->pAdapter;
			if ((NULL != adapter) &&
			    (WLAN_HDD_ADAPTER_MAGIC == adapter->magic)) {
				__hdd_indicate_mgmt_frame(adapter,
						frame_ind->frame_len,
						frame_ind->frameBuf,
						frame_ind->frameType,
						frame_ind->rxChan,
						frame_ind->rxRssi,
						frame_ind->rx_flags);
			}
			status = hdd_get_next_adapter(hdd_ctx,
						adapter_node, &next);
			adapter_node = next;
		}
		adapter = NULL;
	} else {
		adapter = hdd_get_adapter_by_sme_session_id(hdd_ctx,
					frame_ind->sessionId);
	}

	if ((NULL != adapter) &&
		(WLAN_HDD_ADAPTER_MAGIC == adapter->magic))
		__hdd_indicate_mgmt_frame(adapter,
						frame_ind->frame_len,
						frame_ind->frameBuf,
						frame_ind->frameType,
						frame_ind->rxChan,
						frame_ind->rxRssi,
						frame_ind->rx_flags);
}

/**
 * hdd_override_ini_config - Override INI config
 * @hdd_ctx: HDD context
 *
 * Override INI config based on module parameter.
 *
 * Return: None
 */
static void hdd_override_ini_config(hdd_context_t *hdd_ctx)
{

	if (0 == enable_dfs_chan_scan || 1 == enable_dfs_chan_scan) {
		hdd_ctx->config->enableDFSChnlScan = enable_dfs_chan_scan;
		hdd_debug("Module enable_dfs_chan_scan set to %d",
			   enable_dfs_chan_scan);
	}
	if (0 == enable_11d || 1 == enable_11d) {
		hdd_ctx->config->Is11dSupportEnabled = enable_11d;
		hdd_debug("Module enable_11d set to %d", enable_11d);
	}

	if (!hdd_ipa_is_present()) {
		hdd_ctx->config->IpaConfig = 0;
		hdd_debug("IpaConfig override to %d",
			hdd_ctx->config->IpaConfig);
	}

	if (!hdd_ctx->config->rssi_assoc_reject_enabled ||
	    !hdd_ctx->config->enable_bcast_probe_rsp) {
		hdd_debug("OCE disabled, rssi_assoc_reject_enabled: %d enable_bcast_probe_rsp: %d",
			  hdd_ctx->config->rssi_assoc_reject_enabled,
			  hdd_ctx->config->enable_bcast_probe_rsp);
		hdd_ctx->config->oce_sta_enabled = 0;
	}
}

/**
 * hdd_set_trace_level_for_each - Set trace level for each INI config
 * @hdd_ctx - HDD context
 *
 * Set trace level for each module based on INI config.
 *
 * Return: None
 */
static void hdd_set_trace_level_for_each(hdd_context_t *hdd_ctx)
{
	hdd_qdf_trace_enable(QDF_MODULE_ID_WMI,
			     hdd_ctx->config->qdf_trace_enable_wdi);
	hdd_qdf_trace_enable(QDF_MODULE_ID_HDD,
			     hdd_ctx->config->qdf_trace_enable_hdd);
	hdd_qdf_trace_enable(QDF_MODULE_ID_SME,
			     hdd_ctx->config->qdf_trace_enable_sme);
	hdd_qdf_trace_enable(QDF_MODULE_ID_PE,
			     hdd_ctx->config->qdf_trace_enable_pe);
	hdd_qdf_trace_enable(QDF_MODULE_ID_WMA,
			     hdd_ctx->config->qdf_trace_enable_wma);
	hdd_qdf_trace_enable(QDF_MODULE_ID_SYS,
			     hdd_ctx->config->qdf_trace_enable_sys);
	hdd_qdf_trace_enable(QDF_MODULE_ID_QDF,
			     hdd_ctx->config->qdf_trace_enable_qdf);
	hdd_qdf_trace_enable(QDF_MODULE_ID_SAP,
			     hdd_ctx->config->qdf_trace_enable_sap);
	hdd_qdf_trace_enable(QDF_MODULE_ID_HDD_SOFTAP,
			     hdd_ctx->config->qdf_trace_enable_hdd_sap);
	hdd_qdf_trace_enable(QDF_MODULE_ID_BMI,
				hdd_ctx->config->qdf_trace_enable_bmi);
	hdd_qdf_trace_enable(QDF_MODULE_ID_CFG,
				hdd_ctx->config->qdf_trace_enable_cfg);
	hdd_qdf_trace_enable(QDF_MODULE_ID_EPPING,
				hdd_ctx->config->qdf_trace_enable_epping);
	hdd_qdf_trace_enable(QDF_MODULE_ID_QDF_DEVICE,
				hdd_ctx->config->qdf_trace_enable_qdf_devices);
	hdd_qdf_trace_enable(QDF_MODULE_ID_TXRX,
				hdd_ctx->config->cfd_trace_enable_txrx);
	hdd_qdf_trace_enable(QDF_MODULE_ID_HTC,
				hdd_ctx->config->qdf_trace_enable_htc);
	hdd_qdf_trace_enable(QDF_MODULE_ID_HIF,
				hdd_ctx->config->qdf_trace_enable_hif);
	hdd_qdf_trace_enable(QDF_MODULE_ID_HDD_SAP_DATA,
				hdd_ctx->config->qdf_trace_enable_hdd_sap_data);
	hdd_qdf_trace_enable(QDF_MODULE_ID_HDD_DATA,
				hdd_ctx->config->qdf_trace_enable_hdd_data);

	hdd_cfg_print(hdd_ctx);
}

/**
 * hdd_context_init() - Initialize HDD context
 * @hdd_ctx:	HDD context.
 *
 * Initialize HDD context along with all the feature specific contexts.
 *
 * return: 0 on success and errno on failure.
 */
static int hdd_context_init(hdd_context_t *hdd_ctx)
{
	int ret;

	hdd_ctx->ioctl_scan_mode = eSIR_ACTIVE_SCAN;
	hdd_ctx->max_intf_count = CSR_ROAM_SESSION_MAX;

	init_completion(&hdd_ctx->mc_sus_event_var);
	init_completion(&hdd_ctx->ready_to_suspend);

	qdf_spinlock_create(&hdd_ctx->connection_status_lock);
	qdf_spinlock_create(&hdd_ctx->sta_update_info_lock);
	qdf_spinlock_create(&hdd_ctx->hdd_adapter_lock);

	qdf_list_create(&hdd_ctx->hddAdapters, MAX_NUMBER_OF_ADAPTERS);

	ret = hdd_scan_context_init(hdd_ctx);
	if (ret)
		goto list_destroy;

	hdd_tdls_context_init(hdd_ctx, false);

	hdd_rx_wake_lock_create(hdd_ctx);

	ret = hdd_sap_context_init(hdd_ctx);
	if (ret)
		goto scan_destroy;

	ret = hdd_roc_context_init(hdd_ctx);
	if (ret)
		goto sap_destroy;

	wlan_hdd_cfg80211_extscan_init(hdd_ctx);

	hdd_init_offloaded_packets_ctx(hdd_ctx);

	ret = wlan_hdd_cfg80211_init(hdd_ctx->parent_dev, hdd_ctx->wiphy,
				     hdd_ctx->config);
	if (ret)
		goto roc_destroy;

	qdf_wake_lock_create(&hdd_ctx->monitor_mode_wakelock,
						 "monitor_mode_wakelock");

	return 0;

roc_destroy:
	hdd_roc_context_destroy(hdd_ctx);

sap_destroy:
	hdd_sap_context_destroy(hdd_ctx);

scan_destroy:
	hdd_scan_context_destroy(hdd_ctx);
	hdd_rx_wake_lock_destroy(hdd_ctx);
	hdd_tdls_context_destroy(hdd_ctx);

list_destroy:
	qdf_list_destroy(&hdd_ctx->hddAdapters);
	return ret;
}

/*
 * enum hdd_block_shutdown - Control if driver allows modem shutdown
 * @HDD_UNBLOCK_MODEM_SHUTDOWN: Unblock shutdown
 * @HDD_BLOCK_MODEM_SHUTDOWN: Block shutdown
 *
 * On calling pld_block_shutdown API with the given values, modem
 * graceful shutdown is blocked/unblocked.
 */
enum hdd_block_shutdown {
	HDD_UNBLOCK_MODEM_SHUTDOWN,
	HDD_BLOCK_MODEM_SHUTDOWN,
};

/**
 * ie_whitelist_attrs_init() - initialize ie whitelisting attributes
 * @hdd_ctx: pointer to hdd context
 *
 * Return: status of initialization
 *         0 - success
 *         negative value - failure
 */
static int ie_whitelist_attrs_init(hdd_context_t *hdd_ctx)
{
	int ret;

	if (!hdd_ctx->config->probe_req_ie_whitelist)
		return 0;

	if (!hdd_validate_prb_req_ie_bitmap(hdd_ctx)) {
		hdd_err("invalid ie bitmap and ouis: disable ie whitelisting");
		hdd_ctx->config->probe_req_ie_whitelist = false;
		return -EINVAL;
	}

	/* parse ini string probe req oui */
	ret = hdd_parse_probe_req_ouis(hdd_ctx);
	if (ret) {
		hdd_err("parsing error: disable ie whitelisting");
		hdd_ctx->config->probe_req_ie_whitelist = false;
	}

	return ret;
}



/**
 * hdd_iface_change_callback() - Function invoked when stop modules expires
 * @priv: pointer to hdd context
 *
 * This function is invoked when the timer waiting for the interface change
 * expires, it shall cut-down the power to wlan and stop all the modules.
 *
 * Return: void
 */
static void hdd_iface_change_callback(void *priv)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *) priv;
	int ret;
	int status = wlan_hdd_validate_context(hdd_ctx);

	if (status)
		return;

	ENTER();
	hdd_debug("Interface change timer expired close the modules!");

	/* Block the modem graceful shutdown till stop modules is completed */
	pld_block_shutdown(hdd_ctx->parent_dev, HDD_BLOCK_MODEM_SHUTDOWN);

	ret = hdd_wlan_stop_modules(hdd_ctx, false);
	if (ret)
		hdd_err("Failed to stop modules");

	pld_block_shutdown(hdd_ctx->parent_dev, HDD_UNBLOCK_MODEM_SHUTDOWN);
	EXIT();
}

/**
 * hdd_context_create() - Allocate and inialize HDD context.
 * @dev:	Device Pointer to the underlying device
 *
 * Allocate and initialize HDD context. HDD context is allocated as part of
 * wiphy allocation and then context is initialized.
 *
 * Return: HDD context on success and ERR_PTR on failure
 */
static hdd_context_t *hdd_context_create(struct device *dev)
{
	QDF_STATUS status;
	int ret = 0;
	hdd_context_t *hdd_ctx;
	v_CONTEXT_t p_cds_context;

	ENTER();

	p_cds_context = cds_get_global_context();
	if (p_cds_context == NULL) {
		hdd_err("Failed to get CDS global context");
		ret = -EINVAL;
		goto err_out;
	}

	hdd_ctx = hdd_cfg80211_wiphy_alloc(sizeof(hdd_context_t));

	if (hdd_ctx == NULL) {
		ret = -ENOMEM;
		goto err_out;
	}

	qdf_create_delayed_work(&hdd_ctx->iface_idle_work,
				hdd_iface_change_callback,
				(void *)hdd_ctx);

	mutex_init(&hdd_ctx->iface_change_lock);

	hdd_ctx->pcds_context = p_cds_context;
	hdd_ctx->parent_dev = dev;
	hdd_ctx->last_scan_reject_session_id = 0xFF;

	hdd_ctx->config = qdf_mem_malloc(sizeof(struct hdd_config));
	if (hdd_ctx->config == NULL) {
		hdd_err("Failed to alloc memory for HDD config!");
		ret = -ENOMEM;
		goto err_free_hdd_context;
	}

	/* Read and parse the qcom_cfg.ini file */
	status = hdd_parse_config_ini(hdd_ctx);
	if (QDF_STATUS_SUCCESS != status) {
		hdd_err("Error (status: %d) parsing INI file: %s", status,
			  WLAN_INI_FILE);
		ret = -EINVAL;
		goto err_free_config;
	}

	ie_whitelist_attrs_init(hdd_ctx);

	if (hdd_ctx->config->fhostNSOffload)
		hdd_ctx->ns_offload_enable = true;

	cds_set_fatal_event(hdd_ctx->config->enable_fatal_event);

	hdd_override_ini_config(hdd_ctx);

	ret = hdd_context_init(hdd_ctx);

	if (ret)
		goto err_free_config;

	/* Uses to enabled logging after SSR */
	hdd_ctx->fw_log_settings.enable = hdd_ctx->config->enable_fw_log;

	if (QDF_GLOBAL_FTM_MODE == hdd_get_conparam())
		goto skip_multicast_logging;

	cds_set_multicast_logging(hdd_ctx->config->multicast_host_fw_msgs);

	ret = wlan_hdd_init_tx_rx_histogram(hdd_ctx);
	if (ret)
		goto err_deinit_hdd_context;

	wlan_logging_set_log_to_console(hdd_ctx->config->wlanLoggingToConsole);
	wlan_logging_set_active(hdd_ctx->config->wlanLoggingEnable);

	hdd_ctx->is_ssr_in_progress = false;

	/*
	 * Update QDF trace levels based upon the code. The multicast
	 * levels of the code need not be set when the logger thread
	 * is not enabled.
	 */
	if (cds_is_multicast_logging())
		wlan_logging_set_log_level();

	cds_set_context(QDF_MODULE_ID_HDD, hdd_ctx);

skip_multicast_logging:
	hdd_set_trace_level_for_each(hdd_ctx);

	return hdd_ctx;

err_deinit_hdd_context:
	hdd_context_deinit(hdd_ctx);

err_free_config:
	qdf_mem_free(hdd_ctx->config);

err_free_hdd_context:
	hdd_free_probe_req_ouis(hdd_ctx);
	mutex_destroy(&hdd_ctx->iface_change_lock);
	wiphy_free(hdd_ctx->wiphy);

err_out:
	return ERR_PTR(ret);
}

#ifdef WLAN_OPEN_P2P_INTERFACE
/**
 * hdd_open_p2p_interface - Open P2P interface
 * @hdd_ctx:	HDD context
 * @rtnl_held:	True if RTNL lock held
 *
 * Open P2P interface during probe. This function called to open the P2P
 * interface at probe along with STA interface.
 *
 * Return: 0 on success and errno on failure
 */
static int hdd_open_p2p_interface(hdd_context_t *hdd_ctx, bool rtnl_held)
{
	hdd_adapter_t *adapter;
	uint8_t *p2p_dev_addr;
	bool is_p2p_locally_administered = false;

	if (hdd_ctx->config->isP2pDeviceAddrAdministrated) {
		if (hdd_ctx->num_provisioned_addr &&
		    !(hdd_ctx->provisioned_mac_addr[0].bytes[0] & 0x02)) {
			qdf_mem_copy(hdd_ctx->p2pDeviceAddress.bytes,
				     hdd_ctx->provisioned_mac_addr[0].bytes,
				     sizeof(tSirMacAddr));

			/*
			 * Generate the P2P Device Address.  This consists of
			 * the device's primary MAC address with the locally
			 * administered bit set.
			 */
			hdd_ctx->p2pDeviceAddress.bytes[0] |= 0x02;
			is_p2p_locally_administered = true;
		} else if (!(hdd_ctx->derived_mac_addr[0].bytes[0] & 0x02)) {
			qdf_mem_copy(hdd_ctx->p2pDeviceAddress.bytes,
				     hdd_ctx->derived_mac_addr[0].bytes,
				     sizeof(tSirMacAddr));
			/*
			 * Generate the P2P Device Address.  This consists of
			 * the device's primary MAC address with the locally
			 * administered bit set.
			 */
			hdd_ctx->p2pDeviceAddress.bytes[0] |= 0x02;
			is_p2p_locally_administered = true;
		}
	}
	if (!is_p2p_locally_administered) {
		p2p_dev_addr = wlan_hdd_get_intf_addr(hdd_ctx,
						      QDF_P2P_DEVICE_MODE);
		if (!p2p_dev_addr) {
			hdd_err("Failed to allocate mac_address for p2p_device");
			return -ENOSPC;
		}

		qdf_mem_copy(&hdd_ctx->p2pDeviceAddress.bytes[0], p2p_dev_addr,
				 QDF_MAC_ADDR_SIZE);

	}

	adapter = hdd_open_adapter(hdd_ctx, QDF_P2P_DEVICE_MODE, "p2p%d",
				   &hdd_ctx->p2pDeviceAddress.bytes[0],
				   NET_NAME_UNKNOWN, rtnl_held);

	if (NULL == adapter) {
		hdd_err("Failed to do hdd_open_adapter for P2P Device Interface");
		return -ENOSPC;
	}
	return 0;
}
#else
static inline int hdd_open_p2p_interface(hdd_context_t *hdd_ctx,
					 bool rtnl_held)
{
	return 0;
}
#endif

static int hdd_open_ocb_interface(hdd_context_t *hdd_ctx, bool rtnl_held)
{
	hdd_adapter_t *adapter;
	int ret = 0;

	adapter = hdd_open_adapter(hdd_ctx, QDF_OCB_MODE, "wlanocb%d",
				   wlan_hdd_get_intf_addr(hdd_ctx,
							  QDF_OCB_MODE),
				   NET_NAME_UNKNOWN, rtnl_held);
	if (adapter == NULL) {
		hdd_err("Failed to open 802.11p interface");
		ret = -ENOSPC;
	}

	return ret;
}

/**
 * hdd_start_station_adapter()- Start the Station Adapter
 * @adapter: HDD adapter
 *
 * This function initializes the adapter for the station mode.
 *
 * Return: 0 on success or errno on failure.
 */
int hdd_start_station_adapter(hdd_adapter_t *adapter)
{
	QDF_STATUS status;

	ENTER_DEV(adapter->dev);

	status = hdd_init_station_mode(adapter);

	if (QDF_STATUS_SUCCESS != status) {
		hdd_err("Error Initializing station mode: %d", status);
		return qdf_status_to_os_return(status);
	}

	hdd_register_tx_flow_control(adapter,
		hdd_tx_resume_timer_expired_handler,
		hdd_tx_resume_cb,
		hdd_tx_flow_control_is_pause);

	EXIT();
	return 0;
}

/**
 * hdd_start_ap_adapter()- Start AP Adapter
 * @adapter: HDD adapter
 *
 * This function initializes the adapter for the AP mode.
 *
 * Return: 0 on success errno on failure.
 */
int hdd_start_ap_adapter(hdd_adapter_t *adapter)
{
	QDF_STATUS status;

	ENTER();

	status = hdd_init_ap_mode(adapter, false);

	if (QDF_STATUS_SUCCESS != status) {
		hdd_err("Error Initializing the AP mode: %d", status);
		return qdf_status_to_os_return(status);
	}

	hdd_register_tx_flow_control(adapter,
		hdd_softap_tx_resume_timer_expired_handler,
		hdd_softap_tx_resume_cb,
		hdd_tx_flow_control_is_pause);

	EXIT();
	return 0;
}

/**
 * hdd_start_ftm_adapter()- Start FTM adapter
 * @adapter: HDD adapter
 *
 * This function initializes the adapter for the FTM mode.
 *
 * Return: 0 on success or errno on failure.
 */
int hdd_start_ftm_adapter(hdd_adapter_t *adapter)
{
	QDF_STATUS qdf_status;

	ENTER_DEV(adapter->dev);

	qdf_status = hdd_init_tx_rx(adapter);

	if (QDF_STATUS_SUCCESS != qdf_status) {
		hdd_err("Failed to start FTM adapter: %d", qdf_status);
		return qdf_status_to_os_return(qdf_status);
	}

	return 0;
	EXIT();
}

static int hdd_open_concurrent_interface(hdd_context_t *hdd_ctx, bool rtnl_held)
{
	hdd_adapter_t *adapter;

	adapter = hdd_open_adapter(hdd_ctx, QDF_STA_MODE,
				   hdd_ctx->config->enableConcurrentSTA,
				   wlan_hdd_get_intf_addr(hdd_ctx,
							  QDF_STA_MODE),
				   NET_NAME_UNKNOWN, rtnl_held);

	if (adapter == NULL)
		return -ENOSPC;

	return 0;
}

#ifdef WLAN_FEATURE_PKT_CAPTURE

/**
 * wlan_hdd_is_session_type_monitor() - check if session type is MONITOR
 * @session_type: session type
 *
 * Return: True - if session type for adapter is monitor, else False
 *
 */
bool wlan_hdd_is_session_type_monitor(uint8_t session_type)
{
	if (cds_get_pktcap_mode_enable() &&
	    QDF_MONITOR_MODE == session_type)
		return true;
	else
		return false;
}

/**
 * wlan_hdd_check_mon_concurrency() - check if MONITOR and STA concurrency
 * is UP when packet capture mode is enabled.
 * @void
 *
 * Return: True - if STA and monitor concurrency is there, else False
 *
 */
bool wlan_hdd_check_mon_concurrency(void)
{
	if (cds_get_pktcap_mode_enable()) {
		if (cds_get_concurrency_mode() ==
		    (QDF_STA_MASK | QDF_MONITOR_MASK)) {
			hdd_err("STA + MON mode is UP");
			return true;
		}
	}
	return false;
}

/**
 * wlan_hdd_del_monitor() - delete monitor interface
 * @hdd_ctx: pointer to hdd context
 * @adapter: adapter to be deleted
 * @rtnl_held: rtnl lock held
 *
 * This function is invoked to delete monitor interface.
 *
 * Return: None
 */
void wlan_hdd_del_monitor(hdd_context_t *hdd_ctx,
			  hdd_adapter_t *adapter, bool rtnl_held)
{
	wlan_hdd_release_intf_addr(hdd_ctx,
				   adapter->macAddressCurrent.bytes);
	hdd_stop_adapter(hdd_ctx, adapter, true);
	hdd_deinit_adapter(hdd_ctx, adapter, true);
	hdd_close_adapter(hdd_ctx, adapter, true);

	hdd_open_p2p_interface(hdd_ctx, true);
}

/**
 * wlan_hdd_add_monitor_check() - check for monitor intf and add if needed
 * @hdd_ctx: pointer to hdd context
 * @adapter: output pointer to hold created monitor adapter
 * @type: type of the interface
 * @name: name of the interface
 * @rtnl_held: True if RTNL lock is held
 * @name_assign_type: the name of assign type of the netdev
 *
 * Return: 0 - on success
 *         err code - on failure
 */
int
wlan_hdd_add_monitor_check(hdd_context_t *hdd_ctx, hdd_adapter_t **adapter,
			   enum nl80211_iftype type, const char *name,
			   bool rtnl_held, unsigned char name_assign_type)
{
	hdd_adapter_t *sta_adapter;
	hdd_adapter_t *mon_adapter;
	uint32_t mode;

	if (!cds_get_pktcap_mode_enable())
		return 0;

	/*
	 * If add interface request is for monitor mode, then it can run in
	 * parallel with only one station interface.
	 * If there is no existing station interface return error
	 */
	if (type != NL80211_IFTYPE_MONITOR)
		return 0;

	if (hdd_ctx->no_of_open_sessions[QDF_MONITOR_MODE]) {
		hdd_err("monitor mode already exists, only one is possible");
		return -EBUSY;
	}

	/* Ensure there is only one station interface */
	if (hdd_ctx->no_of_open_sessions[QDF_STA_MODE] != 1) {
		hdd_err("cannot add monitor mode, due to %u sta interfaces",
			hdd_ctx->no_of_open_sessions[QDF_STA_MODE]);
		return -EINVAL;
	}

	sta_adapter = hdd_get_adapter(hdd_ctx, QDF_STA_MODE);
	if (!sta_adapter) {
		hdd_err("No station adapter");
		return -EINVAL;
	}

	if (hdd_ctx->no_of_open_sessions[QDF_SAP_MODE]) {
		hdd_err("cannot add monitor mode, due to SAP concurrency");
		return -EINVAL;
	}

	/* delete p2p interface */
	for (mode = QDF_P2P_CLIENT_MODE; mode <= QDF_P2P_DEVICE_MODE; mode++) {
		hdd_adapter_t *adapter;

		if (mode == QDF_FTM_MODE ||
		    mode == QDF_MONITOR_MODE ||
		    mode == QDF_IBSS_MODE)
			continue;

		adapter = hdd_get_adapter(hdd_ctx, mode);
		if (adapter) {
			wlan_hdd_release_intf_addr(
					hdd_ctx,
					adapter->macAddressCurrent.bytes);
			hdd_stop_adapter(hdd_ctx, adapter, true);
			hdd_close_adapter(hdd_ctx, adapter, rtnl_held);
		}
	}

	mon_adapter = hdd_open_adapter(hdd_ctx, QDF_MONITOR_MODE, name,
				       wlan_hdd_get_intf_addr(
				       hdd_ctx,
				       QDF_MONITOR_MODE),
				       name_assign_type, rtnl_held);
	if (!mon_adapter) {
		hdd_err("hdd_open_adapter failed");
		hdd_open_p2p_interface(hdd_ctx, rtnl_held);
		return -EINVAL;
	}

	*adapter = mon_adapter;
	return 0;
}
#endif /* WLAN_FEATURE_PKT_CAPTURE */

/**
 * hdd_open_interfaces - Open all required interfaces
 * hdd_ctx:	HDD context
 * rtnl_held: True if RTNL lock is held
 *
 * Open all the interfaces like STA, P2P and OCB based on the configuration.
 *
 * Return: 0 if all interfaces were created, otherwise negative errno
 */
static int hdd_open_interfaces(hdd_context_t *hdd_ctx, bool rtnl_held)
{
	hdd_adapter_t *adapter;
	int ret;

	if (hdd_ctx->config->dot11p_mode == WLAN_HDD_11P_STANDALONE)
		/* Create only 802.11p interface */
		return hdd_open_ocb_interface(hdd_ctx, rtnl_held);

	adapter = hdd_open_adapter(hdd_ctx, QDF_STA_MODE, "wlan%d",
				   wlan_hdd_get_intf_addr(hdd_ctx,
							  QDF_STA_MODE),
				   NET_NAME_UNKNOWN, rtnl_held);

	if (adapter == NULL)
		return -ENOSPC;

	if (strlen(hdd_ctx->config->enableConcurrentSTA) != 0) {
		ret = hdd_open_concurrent_interface(hdd_ctx, rtnl_held);
		if (ret)
			pr_err("Cannot create concurrent STA interface");
	}

	ret = hdd_open_p2p_interface(hdd_ctx, rtnl_held);
	if (ret)
		goto err_close_adapters;

	/* Open 802.11p Interface */
	if (hdd_ctx->config->dot11p_mode == WLAN_HDD_11P_CONCURRENT) {
		ret = hdd_open_ocb_interface(hdd_ctx, rtnl_held);
		if (ret)
			goto err_close_adapters;
	}

	return 0;

err_close_adapters:
	hdd_close_all_adapters(hdd_ctx, rtnl_held);
	return ret;
}

/**
 * hdd_update_country_code - Update country code
 * @hdd_ctx: HDD context
 * @adapter: Primary adapter context
 *
 * Update country code based on module parameter country_code at SME and wait
 * for the settings to take effect.
 *
 * Return: 0 on success and errno on failure
 */
static int hdd_update_country_code(hdd_context_t *hdd_ctx,
				  hdd_adapter_t *adapter)
{
	QDF_STATUS status;
	int ret = 0;

	if (country_code == NULL)
		return 0;

	qdf_event_reset(&adapter->change_country_code);

	status = sme_change_country_code(hdd_ctx->hHal,
					 wlan_hdd_change_country_code_callback,
					 country_code, adapter,
					 hdd_ctx->pcds_context, true,
					 true);


	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("SME Change Country code from module param fail ret=%d",
			ret);
		return -EINVAL;
	}

	status = qdf_wait_for_event_completion(&adapter->change_country_code,
					       WLAN_WAIT_TIME_COUNTRY);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("SME while setting country code timed out");
		ret = -ETIMEDOUT;
	}

	return ret;
}

#ifdef QCA_LL_TX_FLOW_CONTROL_V2
/**
 * hdd_txrx_populate_cds_config() - Populate txrx cds configuration
 * @cds_cfg: CDS Configuration
 * @hdd_ctx: Pointer to hdd context
 *
 * Return: none
 */
static inline void hdd_txrx_populate_cds_config(struct cds_config_info
						*cds_cfg,
						hdd_context_t *hdd_ctx)
{
	cds_cfg->tx_flow_stop_queue_th =
		hdd_ctx->config->TxFlowStopQueueThreshold;
	cds_cfg->tx_flow_start_queue_offset =
		hdd_ctx->config->TxFlowStartQueueOffset;
}
#else
static inline void hdd_txrx_populate_cds_config(struct cds_config_info
						*cds_cfg,
						hdd_context_t *hdd_ctx)
{
}
#endif

#ifdef FEATURE_WLAN_RA_FILTERING
/**
 * hdd_ra_populate_cds_config() - Populate RA filtering cds configuration
 * @cds_cfg: CDS Configuration
 * @hdd_ctx: Pointer to hdd context
 *
 * Return: none
 */
static inline void hdd_ra_populate_cds_config(struct cds_config_info *cds_cfg,
			      hdd_context_t *hdd_ctx)
{
	cds_cfg->ra_ratelimit_interval =
		hdd_ctx->config->RArateLimitInterval;
	cds_cfg->is_ra_ratelimit_enabled =
		hdd_ctx->config->IsRArateLimitEnabled;
}
#else
static inline void hdd_ra_populate_cds_config(struct cds_config_info *cds_cfg,
			     hdd_context_t *hdd_ctx)
{
}
#endif

#ifdef FW_THERMAL_THROTTLE_SUPPORT
/**
 * hdd_populate_thermal_cfg - Populate the thermal config ini values in CDS cfg
 * @cds_cfg: CDS config structure
 * @hdd_ctx: HDD context structure
 *
 * Return: None
 */
static inline void hdd_populate_thermal_cfg(struct cds_config_info *cds_cfg,
					    hdd_context_t *hdd_ctx)
{
	cds_cfg->thermal_sampling_time = hdd_ctx->config->thermal_sampling_time;
	cds_cfg->thermal_throt_dc = hdd_ctx->config->thermal_throt_dc;
}
#else
static inline void hdd_populate_thermal_cfg(struct cds_config_info *cds_cfg,
					    hdd_context_t *hdd_ctx)
{
}
#endif

/**
 * hdd_update_cds_config() - API to update cds configuration parameters
 * @hdd_ctx: HDD Context
 *
 * Return: 0 for Success, errno on failure
 */
static int hdd_update_cds_config(hdd_context_t *hdd_ctx)
{
	struct cds_config_info *cds_cfg;

	cds_cfg = (struct cds_config_info *)qdf_mem_malloc(sizeof(*cds_cfg));
	if (!cds_cfg) {
		hdd_err("failed to allocate cds config");
		return -ENOMEM;
	}

	cds_cfg->driver_type = QDF_DRIVER_TYPE_PRODUCTION;
	if (!hdd_ctx->config->nMaxPsPoll ||
			!hdd_ctx->config->enablePowersaveOffload) {
		cds_cfg->powersave_offload_enabled =
			hdd_ctx->config->enablePowersaveOffload;
	} else {
		if ((hdd_ctx->config->enablePowersaveOffload ==
				PS_QPOWER_NODEEPSLEEP) ||
				(hdd_ctx->config->enablePowersaveOffload ==
				 PS_LEGACY_NODEEPSLEEP))
			cds_cfg->powersave_offload_enabled =
				PS_LEGACY_NODEEPSLEEP;
		else
			cds_cfg->powersave_offload_enabled =
				PS_LEGACY_DEEPSLEEP;
		hdd_info("Qpower disabled in cds config, %d",
				cds_cfg->powersave_offload_enabled);
	}
	cds_cfg->sta_dynamic_dtim = hdd_ctx->config->enableDynamicDTIM;
	cds_cfg->sta_mod_dtim = hdd_ctx->config->enableModulatedDTIM;
	cds_cfg->sta_maxlimod_dtim = hdd_ctx->config->fMaxLIModulatedDTIM;
	cds_cfg->wow_enable = hdd_ctx->config->wowEnable;
	cds_cfg->max_wow_filters = hdd_ctx->config->maxWoWFilters;
	cds_cfg->etsi_srd_chan_in_master_mode =
		hdd_ctx->config->etsi_srd_chan_in_master_mode;
	cds_cfg->dot11p_mode = hdd_ctx->config->dot11p_mode;

	/* Here ol_ini_info is used to store ini status of arp offload
	 * ns offload and others. Currently 1st bit is used for arp
	 * off load and 2nd bit for ns offload currently, rest bits are unused
	 */
	if (hdd_ctx->config->fhostArpOffload)
		cds_cfg->ol_ini_info = cds_cfg->ol_ini_info | 0x1;
	if (hdd_ctx->config->fhostNSOffload)
		cds_cfg->ol_ini_info = cds_cfg->ol_ini_info | 0x2;

	/*
	 * Copy the DFS Phyerr Filtering Offload status.
	 * This parameter reflects the value of the
	 * dfs_phyerr_filter_offload flag as set in the ini.
	 */
	cds_cfg->dfs_phyerr_filter_offload =
		hdd_ctx->config->fDfsPhyerrFilterOffload;
	if (hdd_ctx->config->ssdp)
		cds_cfg->ssdp = hdd_ctx->config->ssdp;

	cds_cfg->force_target_assert_enabled =
		hdd_ctx->config->crash_inject_enabled;

	cds_cfg->enable_mc_list = hdd_ctx->config->fEnableMCAddrList;
	cds_cfg->dfs_master_enable = hdd_ctx->config->enableDFSMasterCap;
	cds_cfg->ap_maxoffload_peers = hdd_ctx->config->apMaxOffloadPeers;

	cds_cfg->ap_maxoffload_reorderbuffs =
		hdd_ctx->config->apMaxOffloadReorderBuffs;

	cds_cfg->ap_disable_intrabss_fwd =
		hdd_ctx->config->apDisableIntraBssFwd;

	cds_cfg->dfs_pri_multiplier =
		hdd_ctx->config->dfsRadarPriMultiplier;
	cds_cfg->reorder_offload =
			hdd_ctx->config->reorderOffloadSupport;

	/* IPA micro controller data path offload resource config item */
	cds_cfg->uc_offload_enabled = hdd_ipa_uc_is_enabled(hdd_ctx);
	if (!is_power_of_2(hdd_ctx->config->IpaUcTxBufCount)) {
		/* IpaUcTxBufCount should be power of 2 */
		hdd_debug("Round down IpaUcTxBufCount %d to nearest power of 2",
			hdd_ctx->config->IpaUcTxBufCount);
		hdd_ctx->config->IpaUcTxBufCount =
			rounddown_pow_of_two(
				hdd_ctx->config->IpaUcTxBufCount);
		if (!hdd_ctx->config->IpaUcTxBufCount) {
			hdd_err("Failed to round down IpaUcTxBufCount");
			goto exit;
		}
		hdd_debug("IpaUcTxBufCount rounded down to %d",
			hdd_ctx->config->IpaUcTxBufCount);
	}
	cds_cfg->uc_txbuf_count = hdd_ctx->config->IpaUcTxBufCount;
	cds_cfg->uc_txbuf_size = hdd_ctx->config->IpaUcTxBufSize;
	if (!is_power_of_2(hdd_ctx->config->IpaUcRxIndRingCount)) {
		/* IpaUcRxIndRingCount should be power of 2 */
		hdd_debug("Round down IpaUcRxIndRingCount %d to nearest power of 2",
			hdd_ctx->config->IpaUcRxIndRingCount);
		hdd_ctx->config->IpaUcRxIndRingCount =
			rounddown_pow_of_two(
				hdd_ctx->config->IpaUcRxIndRingCount);
		if (!hdd_ctx->config->IpaUcRxIndRingCount) {
			hdd_err("Failed to round down IpaUcRxIndRingCount");
			goto exit;
		}
		hdd_debug("IpaUcRxIndRingCount rounded down to %d",
			hdd_ctx->config->IpaUcRxIndRingCount);
	}
	cds_cfg->uc_rxind_ringcount =
		hdd_ctx->config->IpaUcRxIndRingCount;
	cds_cfg->uc_tx_partition_base =
				hdd_ctx->config->IpaUcTxPartitionBase;
	cds_cfg->max_scan = hdd_ctx->config->max_scan_count;

	cds_cfg->ip_tcp_udp_checksum_offload =
		hdd_ctx->config->enable_ip_tcp_udp_checksum_offload;
	cds_cfg->enable_rxthread = hdd_ctx->enableRxThread;
	cds_cfg->ce_classify_enabled =
		hdd_ctx->config->ce_classify_enabled;
	cds_cfg->apf_packet_filter_enable =
		hdd_ctx->config->apf_packet_filter_enable;
	cds_cfg->tx_chain_mask_cck = hdd_ctx->config->tx_chain_mask_cck;
	cds_cfg->self_gen_frm_pwr = hdd_ctx->config->self_gen_frm_pwr;
	cds_cfg->max_station = hdd_ctx->config->maxNumberOfPeers;
	cds_cfg->sub_20_channel_width = WLAN_SUB_20_CH_WIDTH_NONE;
	cds_cfg->flow_steering_enabled = hdd_ctx->config->flow_steering_enable;
	cds_cfg->max_msdus_per_rxinorderind =
		hdd_ctx->config->max_msdus_per_rxinorderind;
	cds_cfg->self_recovery_enabled = hdd_ctx->config->enableSelfRecovery;
	cds_cfg->fw_timeout_crash = hdd_ctx->config->fw_timeout_crash;
	cds_cfg->active_uc_apf_mode = hdd_ctx->config->active_uc_apf_mode;
	cds_cfg->active_mc_bc_apf_mode = hdd_ctx->config->active_mc_bc_apf_mode;
	cds_cfg->auto_power_save_fail_mode =
		hdd_ctx->config->auto_pwr_save_fail_mode;

	cds_cfg->ito_repeat_count = hdd_ctx->config->ito_repeat_count;
	cds_cfg->bandcapability = hdd_ctx->config->nBandCapability;
	cds_cfg->delay_before_vdev_stop =
		hdd_ctx->config->delay_before_vdev_stop;

	hdd_ra_populate_cds_config(cds_cfg, hdd_ctx);
	hdd_txrx_populate_cds_config(cds_cfg, hdd_ctx);
	hdd_nan_populate_cds_config(cds_cfg, hdd_ctx);
	hdd_lpass_populate_cds_config(cds_cfg, hdd_ctx);
	cds_init_ini_config(cds_cfg);
	hdd_populate_thermal_cfg(cds_cfg, hdd_ctx);
	return 0;

exit:
	qdf_mem_free(cds_cfg);
	return -EINVAL;
}

/**
 * hdd_init_thermal_info - Initialize thermal level
 * @hdd_ctx:	HDD context
 *
 * Initialize thermal level at SME layer and set the thermal level callback
 * which would be called when a configured thermal threshold is hit.
 *
 * Return: 0 on success and errno on failure
 */
static int hdd_init_thermal_info(hdd_context_t *hdd_ctx)
{
	tSmeThermalParams thermal_param;
	QDF_STATUS status;

	thermal_param.smeThermalMgmtEnabled =
		hdd_ctx->config->thermalMitigationEnable;
	thermal_param.smeThrottlePeriod = hdd_ctx->config->throttlePeriod;

	thermal_param.sme_throttle_duty_cycle_tbl[0] =
		hdd_ctx->config->throttle_dutycycle_level0;
	thermal_param.sme_throttle_duty_cycle_tbl[1] =
		hdd_ctx->config->throttle_dutycycle_level1;
	thermal_param.sme_throttle_duty_cycle_tbl[2] =
		hdd_ctx->config->throttle_dutycycle_level2;
	thermal_param.sme_throttle_duty_cycle_tbl[3] =
		hdd_ctx->config->throttle_dutycycle_level3;

	thermal_param.smeThermalLevels[0].smeMinTempThreshold =
		hdd_ctx->config->thermalTempMinLevel0;
	thermal_param.smeThermalLevels[0].smeMaxTempThreshold =
		hdd_ctx->config->thermalTempMaxLevel0;
	thermal_param.smeThermalLevels[1].smeMinTempThreshold =
		hdd_ctx->config->thermalTempMinLevel1;
	thermal_param.smeThermalLevels[1].smeMaxTempThreshold =
		hdd_ctx->config->thermalTempMaxLevel1;
	thermal_param.smeThermalLevels[2].smeMinTempThreshold =
		hdd_ctx->config->thermalTempMinLevel2;
	thermal_param.smeThermalLevels[2].smeMaxTempThreshold =
		hdd_ctx->config->thermalTempMaxLevel2;
	thermal_param.smeThermalLevels[3].smeMinTempThreshold =
		hdd_ctx->config->thermalTempMinLevel3;
	thermal_param.smeThermalLevels[3].smeMaxTempThreshold =
		hdd_ctx->config->thermalTempMaxLevel3;

	status = sme_init_thermal_info(hdd_ctx->hHal, thermal_param);

	if (!QDF_IS_STATUS_SUCCESS(status))
		return qdf_status_to_os_return(status);

	sme_add_set_thermal_level_callback(hdd_ctx->hHal,
					   hdd_set_thermal_level_cb);

	return 0;

}

#if defined(CONFIG_HDD_INIT_WITH_RTNL_LOCK)
/**
 * hdd_hold_rtnl_lock - Hold RTNL lock
 *
 * Hold RTNL lock
 *
 * Return: True if held and false otherwise
 */
static inline bool hdd_hold_rtnl_lock(void)
{
	rtnl_lock();
	return true;
}

/**
 * hdd_release_rtnl_lock - Release RTNL lock
 *
 * Release RTNL lock
 *
 * Return: None
 */
static inline void hdd_release_rtnl_lock(void)
{
	rtnl_unlock();
}
#else
static inline bool hdd_hold_rtnl_lock(void) { return false; }
static inline void hdd_release_rtnl_lock(void) { }
#endif

#if !defined(REMOVE_PKT_LOG)

/* MAX iwpriv command support */
#define PKTLOG_SET_BUFF_SIZE	3
#define PKTLOG_CLEAR_BUFF	4
#define MAX_PKTLOG_SIZE		16

/**
 * hdd_pktlog_set_buff_size() - set pktlog buffer size
 * @hdd_ctx: hdd context
 * @set_value2: pktlog buffer size value
 *
 *
 * Return: 0 for success or error.
 */
static int hdd_pktlog_set_buff_size(hdd_context_t *hdd_ctx, int set_value2)
{
	struct sir_wifi_start_log start_log = { 0 };
	QDF_STATUS status;

	start_log.ring_id = RING_ID_PER_PACKET_STATS;
	start_log.verbose_level = WLAN_LOG_LEVEL_OFF;
	start_log.ini_triggered = cds_is_packet_log_enabled();
	start_log.user_triggered = 1;
	start_log.size = set_value2;
	start_log.is_pktlog_buff_clear = false;

	status = sme_wifi_start_logger(hdd_ctx->hHal, start_log);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("sme_wifi_start_logger failed(err=%d)", status);
		EXIT();
		return -EINVAL;
	}

	return 0;
}

/**
 * hdd_pktlog_clear_buff() - clear pktlog buffer
 * @hdd_ctx: hdd context
 *
 * Return: 0 for success or error.
 */
static int hdd_pktlog_clear_buff(hdd_context_t *hdd_ctx)
{
	struct sir_wifi_start_log start_log;
	QDF_STATUS status;

	start_log.ring_id = RING_ID_PER_PACKET_STATS;
	start_log.verbose_level = WLAN_LOG_LEVEL_OFF;
	start_log.ini_triggered = cds_is_packet_log_enabled();
	start_log.user_triggered = 1;
	start_log.size = 0;
	start_log.is_pktlog_buff_clear = true;

	status = sme_wifi_start_logger(hdd_ctx->hHal, start_log);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("sme_wifi_start_logger failed(err=%d)", status);
		EXIT();
		return -EINVAL;
	}

	return 0;
}

/**
 * hdd_process_pktlog_command() - process pktlog command
 * @hdd_ctx: hdd context
 * @set_value: value set by user
 * @set_value2: pktlog buffer size value
 *
 * This function process pktlog command.
 * set_value2 only matters when set_value is 3 (set buff size)
 * otherwise we ignore it.
 *
 * Return: 0 for success or error.
 */
int hdd_process_pktlog_command(hdd_context_t *hdd_ctx, uint32_t set_value,
			       int set_value2)
{
	int ret;
	bool enable;
	uint8_t user_triggered = 0;

	ret = wlan_hdd_validate_context(hdd_ctx);
	if (0 != ret)
		return ret;

	hdd_debug("set pktlog %d, set size %d", set_value, set_value2);

	if (set_value > PKTLOG_CLEAR_BUFF) {
		hdd_err("invalid pktlog value %d", set_value);
		return -EINVAL;
	}

	if (set_value == PKTLOG_SET_BUFF_SIZE) {
		if (set_value2 <= 0) {
			hdd_err("invalid pktlog size %d", set_value2);
			return -EINVAL;
		} else if (set_value2 > MAX_PKTLOG_SIZE) {
			hdd_err("Pktlog buff size is too large. max value is 16MB.\n");
			return -EINVAL;
		}
		return hdd_pktlog_set_buff_size(hdd_ctx, set_value2);
	} else if (set_value == PKTLOG_CLEAR_BUFF) {
		return hdd_pktlog_clear_buff(hdd_ctx);
	}

	/*
	 * set_value = 0 then disable packetlog
	 * set_value = 1 enable packetlog forcefully
	 * set_vlaue = 2 then disable packetlog if disabled through ini or
	 *                     enable packetlog with AUTO type.
	 */
	enable = ((set_value > 0) && cds_is_packet_log_enabled()) ?
			 true : false;

	if (1 == set_value) {
		enable = true;
		user_triggered = 1;
	}

	return hdd_pktlog_enable_disable(hdd_ctx, enable, user_triggered, 0);
}
/**
 * hdd_pktlog_enable_disable() - Enable/Disable packet logging
 * @hdd_ctx: HDD context
 * @enable: Flag to enable/disable
 * @user_triggered: triggered through iwpriv
 * @size: buffer size to be used for packetlog
 *
 * Return: 0 on success; error number otherwise
 */
int hdd_pktlog_enable_disable(hdd_context_t *hdd_ctx, bool enable,
				uint8_t user_triggered, int size)
{
	struct sir_wifi_start_log start_log;
	QDF_STATUS status;

	start_log.ring_id = RING_ID_PER_PACKET_STATS;
	start_log.verbose_level =
			enable ? WLAN_LOG_LEVEL_ACTIVE : WLAN_LOG_LEVEL_OFF;
	start_log.ini_triggered = cds_is_packet_log_enabled();
	start_log.user_triggered = user_triggered;
	start_log.size = size;
	start_log.is_pktlog_buff_clear = false;
	/*
	 * Use "is_iwpriv_command" flag to distinguish iwpriv command from other
	 * commands. Host uses this flag to decide whether to send pktlog
	 * disable command to fw without sending pktlog enable command
	 * previously. For eg, If vendor sends pktlog disable command without
	 * sending pktlog enable command, then host discards the packet
	 * but for iwpriv command, host will send it to fw.
	 */
	start_log.is_iwpriv_command = 1;
	status = sme_wifi_start_logger(hdd_ctx->hHal, start_log);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("sme_wifi_start_logger failed(err=%d)", status);
		EXIT();
		return -EINVAL;
	}

	if (enable == true)
		hdd_ctx->is_pktlog_enabled = 1;
	else
		hdd_ctx->is_pktlog_enabled = 0;

	return 0;
}
#endif /* REMOVE_PKT_LOG */


#ifdef FEATURE_WLAN_MCC_TO_SCC_SWITCH
/**
 * hdd_register_for_sap_restart_with_channel_switch() - Register for SAP channel
 * switch without restart
 *
 * Registers callback function to change the operating channel of SAP by using
 * channel switch announcements instead of restarting SAP.
 *
 * Return: QDF_STATUS
 */
QDF_STATUS hdd_register_for_sap_restart_with_channel_switch(void)
{
	QDF_STATUS status;

	status = cds_register_sap_restart_channel_switch_cb(
			hdd_sap_restart_with_channel_switch);
	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_err("restart cb registration failed");

	return status;
}
#endif

void hdd_free_mac_address_lists(hdd_context_t *hdd_ctx)
{
	hdd_info("Resetting MAC address lists");
	qdf_mem_set(hdd_ctx->provisioned_mac_addr,
		    sizeof(hdd_ctx->provisioned_mac_addr), 0);
	qdf_mem_set(hdd_ctx->derived_mac_addr,
		    sizeof(hdd_ctx->derived_mac_addr), 0);
	hdd_ctx->num_provisioned_addr = 0;
	hdd_ctx->num_derived_addr = 0;
	hdd_ctx->provisioned_intf_addr_mask = 0;
	hdd_ctx->derived_intf_addr_mask = 0;
}

/**
 * hdd_get_platform_wlan_mac_buff() - API to query platform driver
 *                                    for MAC address
 * @dev: Device Pointer
 * @num: Number of Valid Mac address
 *
 * Return: Pointer to MAC address buffer
 */
static uint8_t *hdd_get_platform_wlan_mac_buff(struct device *dev,
					       uint32_t *num)
{
	return pld_get_wlan_mac_address(dev, num);
}

/**
 * hdd_get_platform_wlan_derived_mac_buff() - API to query platform driver
 *                                    for derived MAC address
 * @dev: Device Pointer
 * @num: Number of Valid Mac address
 *
 * Return: Pointer to MAC address buffer
 */
static uint8_t *hdd_get_platform_wlan_derived_mac_buff(struct device *dev,
						       uint32_t *num)
{
	return pld_get_wlan_derived_mac_address(dev, num);
}

/**
 * hdd_populate_random_mac_addr() - API to populate random mac addresses
 * @hdd_ctx: HDD Context
 * @num: Number of random mac addresses needed
 *
 * Generate random addresses using bit manipulation on the base mac address
 *
 * Return: None
 */
void hdd_populate_random_mac_addr(hdd_context_t *hdd_ctx, uint32_t num)
{
	uint32_t idx = hdd_ctx->num_derived_addr;
	uint32_t iter;
	uint8_t *buf = NULL;
	uint8_t macaddr_b3, tmp_br3;
	/*
	 * Consider first provisioned mac address as source address to derive
	 * remaining addresses
	 */

	uint8_t *src = hdd_ctx->provisioned_mac_addr[0].bytes;

	for (iter = 0; iter < num; ++iter, ++idx) {
		buf = hdd_ctx->derived_mac_addr[idx].bytes;
		qdf_mem_copy(buf, src, QDF_MAC_ADDR_SIZE);
		macaddr_b3 = buf[3];
		tmp_br3 = ((macaddr_b3 >> 4 & INTF_MACADDR_MASK) + idx) &
			INTF_MACADDR_MASK;
		macaddr_b3 += tmp_br3;
		macaddr_b3 ^= (1 << INTF_MACADDR_MASK);
		buf[0] |= 0x02;
		buf[3] = macaddr_b3;
		hdd_debug(MAC_ADDRESS_STR, MAC_ADDR_ARRAY(buf));
		hdd_ctx->num_derived_addr++;
	}
}

/**
 * hdd_platform_wlan_mac() - API to get mac addresses from platform driver
 * @hdd_ctx: HDD Context
 *
 * API to get mac addresses from platform driver and update the driver
 * structures and configure FW with the base mac address.
 * Return: int
 */
static int hdd_platform_wlan_mac(hdd_context_t *hdd_ctx)
{
	uint32_t no_of_mac_addr, iter;
	uint32_t max_mac_addr = QDF_MAX_CONCURRENCY_PERSONA;
	uint32_t mac_addr_size = QDF_MAC_ADDR_SIZE;
	uint8_t *addr, *buf;
	struct device *dev = hdd_ctx->parent_dev;
	tSirMacAddr mac_addr;
	QDF_STATUS status;

	addr = hdd_get_platform_wlan_mac_buff(dev, &no_of_mac_addr);

	if (no_of_mac_addr == 0 || !addr)
		return -EINVAL;

	hdd_free_mac_address_lists(hdd_ctx);

	if (no_of_mac_addr > max_mac_addr)
		no_of_mac_addr = max_mac_addr;

	qdf_mem_copy(&mac_addr, addr, mac_addr_size);

	for (iter = 0; iter < no_of_mac_addr; ++iter, addr += mac_addr_size) {
		buf = hdd_ctx->provisioned_mac_addr[iter].bytes;
		qdf_mem_copy(buf, addr, QDF_MAC_ADDR_SIZE);
		hdd_info("provisioned MAC Addr [%d]" MAC_ADDRESS_STR, iter,
			 MAC_ADDR_ARRAY(buf));
	}


	hdd_ctx->num_provisioned_addr = no_of_mac_addr;

	if (hdd_ctx->config->mac_provision) {
		addr = hdd_get_platform_wlan_derived_mac_buff(dev,
							      &no_of_mac_addr);

		if (no_of_mac_addr == 0 || !addr)
			hdd_warn("No derived address from platform driver");
		else if (no_of_mac_addr >
			 (max_mac_addr - hdd_ctx->num_provisioned_addr))
			no_of_mac_addr = (max_mac_addr -
					  hdd_ctx->num_provisioned_addr);

		for (iter = 0; iter < no_of_mac_addr; ++iter,
		     addr += mac_addr_size) {
			buf = hdd_ctx->derived_mac_addr[iter].bytes;
			qdf_mem_copy(buf, addr, QDF_MAC_ADDR_SIZE);
			hdd_info("derived MAC Addr [%d]" MAC_ADDRESS_STR, iter,
				 MAC_ADDR_ARRAY(buf));
		}
		hdd_ctx->num_derived_addr = no_of_mac_addr;
	}

	no_of_mac_addr = hdd_ctx->num_provisioned_addr +
					 hdd_ctx->num_derived_addr;
	if (no_of_mac_addr < max_mac_addr)
		hdd_populate_random_mac_addr(hdd_ctx, max_mac_addr -
					     no_of_mac_addr);

	status = sme_set_custom_mac_addr(mac_addr);
	if (!QDF_IS_STATUS_SUCCESS(status))
		return -EAGAIN;

	return 0;
}

/**
 * hdd_update_mac_addr_to_fw() - API to update wlan mac addresses to FW
 * @hdd_ctx: HDD Context
 *
 * Update MAC address to FW. If MAC address passed by FW is invalid, host
 * will generate its own MAC and update it to FW.
 *
 * Return: 0 for success
 *         Non-zero error code for failure
 */
static int hdd_update_mac_addr_to_fw(hdd_context_t *hdd_ctx)
{
	tSirMacAddr customMacAddr;
	QDF_STATUS status;

	if (hdd_ctx->num_provisioned_addr)
		qdf_mem_copy(&customMacAddr,
			     &hdd_ctx->provisioned_mac_addr[0].bytes[0],
			     sizeof(tSirMacAddr));
	else
		qdf_mem_copy(&customMacAddr,
			     &hdd_ctx->derived_mac_addr[0].bytes[0],
			     sizeof(tSirMacAddr));
	status = sme_set_custom_mac_addr(customMacAddr);
	if (!QDF_IS_STATUS_SUCCESS(status))
		return -EAGAIN;
	return 0;
}

/**
 * hdd_initialize_mac_address() - API to get wlan mac addresses
 * @hdd_ctx: HDD Context
 *
 * Get MAC addresses from platform driver or wlan_mac.bin. If platform driver
 * is provisioned with mac addresses, driver uses it, else it will use
 * wlan_mac.bin to update HW MAC addresses.
 *
 * Return: None
 */
static int hdd_initialize_mac_address(hdd_context_t *hdd_ctx)
{
	QDF_STATUS status;
	int ret;
	bool update_mac_addr_to_fw = true;

	ret = hdd_platform_wlan_mac(hdd_ctx);
	if (hdd_ctx->config->mac_provision || !ret) {
		hdd_info("using MAC address from platform driver");
		return ret;
	}

	hdd_info("MAC is not programmed in platform driver ret: %d, use wlan_mac.bin",
		 ret);

	status = hdd_update_mac_config(hdd_ctx);

	if (QDF_IS_STATUS_SUCCESS(status)) {
		hdd_info("using MAC address from wlan_mac.bin");
		return 0;
	}

	hdd_info("using default MAC address");

	/* Use fw provided MAC */
	if (!qdf_is_macaddr_zero(&hdd_ctx->hw_macaddr)) {
		hdd_update_macaddr(hdd_ctx, hdd_ctx->hw_macaddr, false);
		update_mac_addr_to_fw = false;
	} else if (hdd_generate_macaddr_auto(hdd_ctx) != 0) {
		struct qdf_mac_addr mac_addr;

		hdd_err("MAC failure from device serial no.");
		cds_rand_get_bytes(0, (uint8_t *)(&mac_addr), sizeof(mac_addr));
		/*
		 * Reset multicast bit (bit-0) and set
		 * locally-administered bit
		 */
		mac_addr.bytes[0] = 0x2;
		hdd_update_macaddr(hdd_ctx, mac_addr, true);
	}

	if (update_mac_addr_to_fw) {
		ret = hdd_update_mac_addr_to_fw(hdd_ctx);
		if (ret)
			hdd_err("MAC address out-of-sync, ret:%d", ret);
	}
	return 0;
}

static int hdd_set_smart_chainmask_enabled(hdd_context_t *hdd_ctx)
{
	int vdev_id = 0;
	int param_id = WMI_PDEV_PARAM_SMART_CHAINMASK_SCHEME;
	int value = hdd_ctx->config->smart_chainmask_enabled;
	int vpdev = PDEV_CMD;
	int ret;

	ret = sme_cli_set_command(vdev_id, param_id, value, vpdev);
	if (ret)
		hdd_err("WMI_PDEV_PARAM_SMART_CHAINMASK_SCHEME failed %d", ret);

	return ret;
}

static int hdd_set_alternative_chainmask_enabled(hdd_context_t *hdd_ctx)
{
	int vdev_id = 0;
	int param_id = WMI_PDEV_PARAM_ALTERNATIVE_CHAINMASK_SCHEME;
	int value = hdd_ctx->config->alternative_chainmask_enabled;
	int vpdev = PDEV_CMD;
	int ret;

	ret = sme_cli_set_command(vdev_id, param_id, value, vpdev);
	if (ret)
		hdd_err("WMI_PDEV_PARAM_ALTERNATIVE_CHAINMASK_SCHEME failed %d",
			ret);

	return ret;
}

static int hdd_set_ani_enabled(hdd_context_t *hdd_ctx)
{
	int vdev_id = 0;
	int param_id = WMI_PDEV_PARAM_ANI_ENABLE;
	int value = hdd_ctx->config->ani_enabled;
	int vpdev = PDEV_CMD;
	int ret;

	ret = sme_cli_set_command(vdev_id, param_id, value, vpdev);
	if (ret)
		hdd_err("WMI_PDEV_PARAM_ANI_ENABLE failed %d", ret);

	return ret;
}

/**
 * hdd_send_all_sme_action_ouis() - send all action oui extensions to firmware
 * @hdd_ctx: pointer to hdd context
 *
 * Return: None
 */
static void hdd_send_all_sme_action_ouis(hdd_context_t *hdd_ctx)
{
	QDF_STATUS qdf_status;
	uint32_t i;

	if (!hdd_ctx->config->enable_action_oui)
		return;

	for (i = 0; i < WMI_ACTION_OUI_MAXIMUM_ID; i++) {
		qdf_status = sme_send_action_oui(hdd_ctx->hHal, i);
		/* print the error and continue for another action */
		if (!QDF_IS_STATUS_SUCCESS(qdf_status))
			hdd_err("Failed to send action OUI: %u", i);
	}
}

/**
 * hdd_send_coex_config_params() - Send coex config params to FW
 * @hdd_ctx: HDD context
 * @adapter: Primary adapter context
 *
 * This function is used to send all coex config related params to FW
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS hdd_send_coex_config_params(hdd_context_t *hdd_ctx,
					      hdd_adapter_t *adapter)
{
	QDF_STATUS status;
	struct coex_config_params coex_cfg_params = {0};
	struct hdd_config *config;

	if (!hdd_ctx) {
		hdd_err("hdd_ctx is invalid");
		return QDF_STATUS_E_INVAL;
	}

	if (!adapter) {
		hdd_err("adapter is invalid");
		return QDF_STATUS_E_INVAL;
	}

	config = hdd_ctx->config;
	if (!config) {
		hdd_err("hdd config got NULL");
		return QDF_STATUS_E_NULL_VALUE;
	}

	coex_cfg_params.vdev_id = adapter->sessionId;
	coex_cfg_params.config_type = COEX_CONFIG_TX_POWER;
	coex_cfg_params.config_value[0] = config->set_max_tx_power_for_btc;

	status = sme_send_coex_config_cmd(&coex_cfg_params);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("Failed to send coex Tx power");
		return QDF_STATUS_E_FAILURE;
	}

	coex_cfg_params.config_type = COEX_CONFIG_HANDOVER_RSSI;
	coex_cfg_params.config_value[0] = config->set_wlan_low_rssi_threshold;

	status = sme_send_coex_config_cmd(&coex_cfg_params);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("Failed to send coex handover RSSI");
		return QDF_STATUS_E_FAILURE;
	}

	coex_cfg_params.config_type = COEX_CONFIG_BTC_MODE;
	coex_cfg_params.config_value[0] = config->set_btc_mode;

	status = sme_send_coex_config_cmd(&coex_cfg_params);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("Failed to send coex BTC mode");
		return QDF_STATUS_E_FAILURE;
	}

	coex_cfg_params.config_type = COEX_CONFIG_ANTENNA_ISOLATION;
	coex_cfg_params.config_value[0] = config->set_antenna_isolation;

	status = sme_send_coex_config_cmd(&coex_cfg_params);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("Failed to send coex antenna isolation");
		return QDF_STATUS_E_FAILURE;
	}

	coex_cfg_params.config_type = COEX_CONFIG_BT_LOW_RSSI_THRESHOLD;
	coex_cfg_params.config_value[0] = config->set_bt_low_rssi_threshold;

	status = sme_send_coex_config_cmd(&coex_cfg_params);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("Failed to send coex BT low RSSI threshold");
		return QDF_STATUS_E_FAILURE;
	}

	coex_cfg_params.config_type = COEX_CONFIG_BT_INTERFERENCE_LEVEL;
	coex_cfg_params.config_value[0] = config->set_bt_interference_low_ll;
	coex_cfg_params.config_value[1] = config->set_bt_interference_low_ul;
	coex_cfg_params.config_value[2] = config->set_bt_interference_medium_ll;
	coex_cfg_params.config_value[3] = config->set_bt_interference_medium_ul;
	coex_cfg_params.config_value[4] = config->set_bt_interference_high_ll;
	coex_cfg_params.config_value[5] = config->set_bt_interference_high_ul;

	status = sme_send_coex_config_cmd(&coex_cfg_params);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("Failed to send coex BT interference level");
		return QDF_STATUS_E_FAILURE;
	}
	return QDF_STATUS_SUCCESS;
}

/**
 * hdd_pre_enable_configure() - Configurations prior to cds_enable
 * @hdd_ctx:	HDD context
 *
 * Pre configurations to be done at lower layer before calling cds enable.
 *
 * Return: 0 on success and errno on failure.
 */
static int hdd_pre_enable_configure(hdd_context_t *hdd_ctx)
{
	int ret;
	QDF_STATUS status;

	ol_txrx_register_pause_cb(wlan_hdd_txrx_pause_cb);

	/*
	 * Set 802.11p config
	 * TODO-OCB: This has been temporarily added here to ensure this
	 * parameter is set in CSR when we init the channel list. This should
	 * be removed once the 5.9 GHz channels are added to the regulatory
	 * domain.
	 */
	hdd_set_dot11p_config(hdd_ctx);

	/*
	 * Note that the cds_pre_enable() sequence triggers the cfg download.
	 * The cfg download must occur before we update the SME config
	 * since the SME config operation must access the cfg database
	 */
	status = hdd_set_sme_config(hdd_ctx);

	if (QDF_STATUS_SUCCESS != status) {
		hdd_err("Failed hdd_set_sme_config: %d", status);
		ret = qdf_status_to_os_return(status);
		goto out;
	}

	hdd_set_all_sme_action_ouis(hdd_ctx);

	ret = sme_cli_set_command(0, WMI_PDEV_PARAM_TX_CHAIN_MASK_1SS,
				  hdd_ctx->config->tx_chain_mask_1ss,
				  PDEV_CMD);
	if (0 != ret) {
		hdd_err("WMI_PDEV_PARAM_TX_CHAIN_MASK_1SS failed %d", ret);
		goto out;
	}

	ret = hdd_set_smart_chainmask_enabled(hdd_ctx);
	if (ret)
		goto out;

	ret = hdd_set_alternative_chainmask_enabled(hdd_ctx);
	if (ret)
		goto out;

	ret = hdd_set_ani_enabled(hdd_ctx);
	if (ret)
		goto out;

	ret = sme_cli_set_command(0, WMI_PDEV_PARAM_ARP_AC_OVERRIDE,
				  hdd_ctx->config->arp_ac_category,
				  PDEV_CMD);
	if (0 != ret) {
		hdd_err("WMI_PDEV_PARAM_ARP_AC_OVERRIDE ac: %d ret: %d",
			hdd_ctx->config->arp_ac_category, ret);
		goto out;
	}

	ret = hdd_apply_cached_country_info(hdd_ctx);

	if (0 != ret) {
		hdd_err("reg info update failed");
		goto out;
	}

	status = hdd_set_sme_chan_list(hdd_ctx);
	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("Failed to init channel list: %d", status);
		ret = qdf_status_to_os_return(status);
		goto out;
	}

	/* Apply the cfg.ini to cfg.dat */
	if (!hdd_update_config_cfg(hdd_ctx)) {
		hdd_err("config update failed");
		ret = -EINVAL;
		goto out;
	}

	hdd_init_channel_avoidance(hdd_ctx);

	/* update enable sap mandatory chan list */
	cds_enable_disable_sap_mandatory_chan_list(
			hdd_ctx->config->enable_sap_mandatory_chan_list);

	cds_set_cur_conc_system_pref(hdd_ctx->config->conc_system_pref);
out:
	return ret;
}

/**
 * wlan_hdd_p2p_lo_event_callback - P2P listen offload stop event handler
 * @context_ptr - hdd context pointer
 * @event_ptr - event structure pointer
 *
 * This is the p2p listen offload stop event handler, it sends vendor
 * event back to supplicant to notify the stop reason.
 *
 * Return: None
 */
static void wlan_hdd_p2p_lo_event_callback(void *context_ptr,
				void *event_ptr)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *)context_ptr;
	struct sir_p2p_lo_event *evt = event_ptr;
	struct sk_buff *vendor_event;
	hdd_adapter_t *adapter;

	ENTER();

	if (hdd_ctx == NULL) {
		hdd_err("Invalid HDD context pointer");
		return;
	}

	adapter = hdd_get_adapter_by_vdev(hdd_ctx, evt->vdev_id);
	if (!adapter) {
		hdd_err("Cannot find adapter by vdev_id = %d",
				evt->vdev_id);
		return;
	}

	vendor_event =
		cfg80211_vendor_event_alloc(hdd_ctx->wiphy,
			&(adapter->wdev), sizeof(uint32_t) + NLMSG_HDRLEN,
			QCA_NL80211_VENDOR_SUBCMD_P2P_LO_EVENT_INDEX,
			GFP_KERNEL);

	if (!vendor_event) {
		hdd_err("cfg80211_vendor_event_alloc failed");
		return;
	}

	if (nla_put_u32(vendor_event,
			QCA_WLAN_VENDOR_ATTR_P2P_LISTEN_OFFLOAD_STOP_REASON,
			evt->reason_code)) {
		hdd_err("nla put failed");
		kfree_skb(vendor_event);
		return;
	}

	cfg80211_vendor_event(vendor_event, GFP_KERNEL);
	hdd_debug("Sent P2P_LISTEN_OFFLOAD_STOP event for vdev_id = %d",
			evt->vdev_id);
}

/**
 * hdd_adaptive_dwelltime_init() - initialization for adaptive dwell time config
 * @hdd_ctx: HDD context
 *
 * This function sends the adaptive dwell time config configuration to the
 * firmware via WMA
 *
 * Return: 0 - success, < 0 - failure
 */
static int hdd_adaptive_dwelltime_init(hdd_context_t *hdd_ctx)
{
	QDF_STATUS status;
	struct adaptive_dwelltime_params dwelltime_params;

	dwelltime_params.is_enabled =
			hdd_ctx->config->adaptive_dwell_mode_enabled;
	dwelltime_params.dwelltime_mode =
			hdd_ctx->config->global_adapt_dwelltime_mode;
	dwelltime_params.lpf_weight =
			hdd_ctx->config->adapt_dwell_lpf_weight;
	dwelltime_params.passive_mon_intval =
			hdd_ctx->config->adapt_dwell_passive_mon_intval;
	dwelltime_params.wifi_act_threshold =
			hdd_ctx->config->adapt_dwell_wifi_act_threshold;

	status = sme_set_adaptive_dwelltime_config(hdd_ctx->hHal,
						   &dwelltime_params);

	hdd_debug("Sending Adaptive Dwelltime Configuration to fw");
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("Failed to send Adaptive Dwelltime configuration!");
		return -EAGAIN;
	}
	return 0;
}

int hdd_dbs_scan_selection_init(hdd_context_t *hdd_ctx)
{
	QDF_STATUS status;
	struct wmi_dbs_scan_sel_params dbs_scan_params;
	uint32_t i = 0;
	uint8_t count = 0, numentries = 0;
	uint8_t dbs_scan_config[CFG_DBS_SCAN_PARAM_PER_CLIENT
				* CFG_DBS_SCAN_CLIENTS_MAX];

	hdd_ctx->is_dbs_scan_duty_cycle_enabled = false;

	/* check if DBS is enabled or supported */
	if ((hdd_ctx->config->dual_mac_feature_disable ==
	     DISABLE_DBS_CXN_AND_SCAN) ||
	    (hdd_ctx->config->dual_mac_feature_disable ==
	     ENABLE_DBS_CXN_AND_DISABLE_DBS_SCAN))
		return -EINVAL;

	hdd_string_to_u8_array(hdd_ctx->config->dbs_scan_selection,
			       dbs_scan_config, &numentries,
			       (CFG_DBS_SCAN_PARAM_PER_CLIENT
				* CFG_DBS_SCAN_CLIENTS_MAX));

	if (!numentries) {
		hdd_debug("Do not send scan_selection_config");
		return 0;
	}

	/* hdd_set_fw_log_params */
	dbs_scan_params.num_clients = 0;
	while (count < (numentries - 2)) {
		dbs_scan_params.module_id[i] = dbs_scan_config[count];
		dbs_scan_params.num_dbs_scans[i] = dbs_scan_config[count + 1];
		dbs_scan_params.num_non_dbs_scans[i] =
			dbs_scan_config[count + 2];
		dbs_scan_params.num_clients++;
		hdd_debug("module:%d NDS:%d NNDS:%d",
			dbs_scan_params.module_id[i],
			dbs_scan_params.num_dbs_scans[i],
			dbs_scan_params.num_non_dbs_scans[i]);
		count += CFG_DBS_SCAN_PARAM_PER_CLIENT;
		i++;
	}

	dbs_scan_params.pdev_id = 0;

	hdd_debug("clients:%d pdev:%d",
		  dbs_scan_params.num_clients, dbs_scan_params.pdev_id);

	status = sme_set_dbs_scan_selection_config(hdd_ctx->hHal,
						   &dbs_scan_params);
	hdd_debug("Sending DBS Scan Selection Configuration to fw");
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("Failed to send DBS Scan selection configuration!");
		return -EAGAIN;
	}

	hdd_ctx->is_dbs_scan_duty_cycle_enabled = true;
	return 0;
}

#ifdef FEATURE_WLAN_AUTO_SHUTDOWN
/**
 * hdd_set_auto_shutdown_cb() - Set auto shutdown callback
 * @hdd_ctx:	HDD context
 *
 * Set auto shutdown callback to get indications from firmware to indicate
 * userspace to shutdown WLAN after a configured amount of inactivity.
 *
 * Return: 0 on success and errno on failure.
 */
static int hdd_set_auto_shutdown_cb(hdd_context_t *hdd_ctx)
{
	QDF_STATUS status;

	if (!hdd_ctx->config->WlanAutoShutdown)
		return 0;

	status = sme_set_auto_shutdown_cb(hdd_ctx->hHal,
					  wlan_hdd_auto_shutdown_cb);
	if (status != QDF_STATUS_SUCCESS)
		hdd_err("Auto shutdown feature could not be enabled: %d",
			status);

	return qdf_status_to_os_return(status);
}
#else
static int hdd_set_auto_shutdown_cb(hdd_context_t *hdd_ctx)
{
	return 0;
}
#endif

#ifdef MWS_COEX
/**
 * hdd_set_mws_coex() - Set MWS coex configurations
 * @adapter: HDD adapter
 *
 * This function sends MWS-COEX 4G quick FTDM and
 * MWS-COEX 5G-NR power limit to FW
 *
 * Return: 0 on success and errno on failure.
 */
static int hdd_init_mws_coex(hdd_adapter_t *adapter)
{
	int ret = 0;
	hdd_context_t *hdd_ctx;

	if (!adapter) {
		hdd_err("Invalid Param");
		return -EINVAL;
	}
	hdd_ctx = WLAN_HDD_GET_CTX(adapter);

	ret = sme_cli_set_command(adapter->sessionId,
				  WMI_PDEV_PARAM_MWSCOEX_4G_ALLOW_QUICK_FTDM,
				  hdd_ctx->config->g_mws_coex_4g_quick_tdm,
				  PDEV_CMD);
	if (ret) {
		hdd_warn("Unable to send MWS-COEX 4G quick FTDM policy");
		return ret;
	}

	ret = sme_cli_set_command(adapter->sessionId,
				  WMI_PDEV_PARAM_MWSCOEX_SET_5GNR_PWR_LIMIT,
				  hdd_ctx->config->g_mws_coex_5g_nr_pwr_limit,
				  PDEV_CMD);
	if (ret) {
		hdd_warn("Unable to send MWS-COEX 4G quick FTDM policy");
		return ret;
	}
	return ret;
}
#else
static int hdd_init_mws_coex(hdd_adapter_t *adapter)
{
	return 0;
}
#endif

/**
 * hdd_features_init() - Init features
 * @hdd_ctx:	HDD context
 * @adapter:	Primary adapter context
 *
 * Initialize features and their feature context after WLAN firmware is up.
 *
 * Return: 0 on success and errno on failure.
 */
static int hdd_features_init(hdd_context_t *hdd_ctx, hdd_adapter_t *adapter)
{
	tSirTxPowerLimit hddtxlimit;
	QDF_STATUS status;
	struct sme_5g_band_pref_params band_pref_params;
	int ret, set_value;
	uint32_t num_abg_tx_chains = 0, num_11b_tx_chains = 0;
	uint32_t num_11ag_tx_chains = 0;

	ENTER();

	sme_set_chip_pwr_save_fail_cb(hdd_ctx->hHal,
				      hdd_chip_pwr_save_fail_detected_cb);

	if (hdd_ctx->config->max_mpdus_inampdu) {
		set_value = hdd_ctx->config->max_mpdus_inampdu;
		sme_cli_set_command(0, WMI_PDEV_PARAM_MAX_MPDUS_IN_AMPDU,
				    set_value, PDEV_CMD);
	}

	if (hdd_ctx->config->enable_rts_sifsbursting) {
		set_value = hdd_ctx->config->enable_rts_sifsbursting;
		sme_cli_set_command(0,
				    WMI_PDEV_PARAM_ENABLE_RTS_SIFS_BURSTING,
				    set_value, PDEV_CMD);
	}

	if (hdd_ctx->config->sap_get_peer_info) {
		set_value = hdd_ctx->config->sap_get_peer_info;
		sme_cli_set_command(0,
				    WMI_PDEV_PARAM_PEER_STATS_INFO_ENABLE,
				    set_value, PDEV_CMD);
	}

	if (hdd_ctx->config->is_force_1x1)
		sme_cli_set_command(0, WMI_PDEV_PARAM_SET_IOT_PATTERN,
				    1, PDEV_CMD);

	num_11b_tx_chains = hdd_ctx->config->num_11b_tx_chains;
	num_11ag_tx_chains = hdd_ctx->config->num_11ag_tx_chains;
	if (!hdd_ctx->config->enable2x2) {
		if (num_11b_tx_chains > 1)
			num_11b_tx_chains = 1;
		if (num_11ag_tx_chains > 1)
			num_11ag_tx_chains = 1;
	}
	WMI_PDEV_PARAM_SET_11B_TX_CHAIN_NUM(num_abg_tx_chains,
					    num_11b_tx_chains);
	WMI_PDEV_PARAM_SET_11AG_TX_CHAIN_NUM(num_abg_tx_chains,
					     num_11ag_tx_chains);
	sme_cli_set_command(0, WMI_PDEV_PARAM_ABG_MODE_TX_CHAIN_NUM,
			    num_abg_tx_chains, PDEV_CMD);

	ret = hdd_update_country_code(hdd_ctx, adapter);
	if (ret) {
		hdd_err("Failed to update country code: %d", ret);
		goto out;
	}

	hdd_init_mws_coex(adapter);

	hdd_send_coex_config_params(hdd_ctx, adapter);

	/* FW capabilities received, Set the Dot11 mode */
	sme_setdef_dot11mode(hdd_ctx->hHal);
	sme_set_prefer_80MHz_over_160MHz(hdd_ctx->hHal,
			hdd_ctx->config->sta_prefer_80MHz_over_160MHz);

	sme_set_etsi_srd_ch_in_master_mode(hdd_ctx->hHal,
			hdd_ctx->config->etsi_srd_chan_in_master_mode);

	sme_set_allow_adj_ch_bcn(hdd_ctx->hHal,
			hdd_ctx->config->allow_adj_ch_bcn);

	if (hdd_ctx->config->fIsImpsEnabled)
		hdd_set_idle_ps_config(hdd_ctx, true);
	else
		hdd_set_idle_ps_config(hdd_ctx, false);

	/* Send Enable/Disable data stall detection cmd to FW */
	sme_cli_set_command(0, WMI_PDEV_PARAM_DATA_STALL_DETECT_ENABLE,
		hdd_ctx->config->enable_data_stall_det, PDEV_CMD);

	if (hdd_ctx->config->enable_go_cts2self_for_sta)
		sme_set_cts2self_for_p2p_go(hdd_ctx->hHal);

	if (sme_set_vc_mode_config(hdd_ctx->config->vc_mode_cfg_bitmap))
		hdd_warn("Error in setting Voltage Corner mode config to FW");

	if (hdd_rx_ol_init(hdd_ctx))
		hdd_warn("Unable to initialize LRO in fw");

	if (hdd_adaptive_dwelltime_init(hdd_ctx))
		hdd_warn("Unable to send adaptive dwelltime setting to FW");

	if (hdd_dbs_scan_selection_init(hdd_ctx))
		hdd_warn("Unable to send DBS scan selection setting to FW");

	ret = hdd_init_thermal_info(hdd_ctx);
	if (ret) {
		hdd_err("Error while initializing thermal information");
		goto deregister_frames;
	}

	/**
	 * In case of SSR/PDR, if pktlog was enabled manually before
	 * SSR/PDR, Then enabled it again automatically after Wlan
	 * device up.
	 */
	if (cds_is_driver_recovering()) {
		if (hdd_ctx->is_pktlog_enabled)
			hdd_pktlog_enable_disable(hdd_ctx, true, 0, 0);
	} else if (cds_is_packet_log_enabled())
		hdd_pktlog_enable_disable(hdd_ctx, true, 0, 0);

	hddtxlimit.txPower2g = hdd_ctx->config->TxPower2g;
	hddtxlimit.txPower5g = hdd_ctx->config->TxPower5g;
	status = sme_txpower_limit(hdd_ctx->hHal, &hddtxlimit);
	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_err("Error setting txlimit in sme: %d", status);

	wlan_hdd_tsf_init(hdd_ctx);

	if (hdd_ctx->config->goptimize_chan_avoid_event) {
		status = sme_enable_disable_chanavoidind_event(
							hdd_ctx->hHal, 0);
		if (!QDF_IS_STATUS_SUCCESS(status)) {
			hdd_err("Failed to disable Chan Avoidance Indication");
			goto deregister_frames;
		}
	}

	if (hdd_ctx->config->enable_5g_band_pref) {
		band_pref_params.rssi_boost_threshold_5g =
				hdd_ctx->config->rssi_boost_threshold_5g;
		band_pref_params.rssi_boost_factor_5g =
				hdd_ctx->config->rssi_boost_factor_5g;
		band_pref_params.max_rssi_boost_5g =
				hdd_ctx->config->max_rssi_boost_5g;
		band_pref_params.rssi_penalize_threshold_5g =
				hdd_ctx->config->rssi_penalize_threshold_5g;
		band_pref_params.rssi_penalize_factor_5g =
				hdd_ctx->config->rssi_penalize_factor_5g;
		band_pref_params.max_rssi_penalize_5g =
				hdd_ctx->config->max_rssi_penalize_5g;
		sme_set_5g_band_pref(hdd_ctx->hHal, &band_pref_params);
	}

	/* register P2P Listen Offload event callback */
	if (wma_is_p2p_lo_capable())
		sme_register_p2p_lo_event(hdd_ctx->hHal, hdd_ctx,
				wlan_hdd_p2p_lo_event_callback);

	/* register spectral scan callback */
	hdd_register_spectral_scan_cb(hdd_ctx, spectral_scan_callback);

	ret = hdd_set_auto_shutdown_cb(hdd_ctx);

	if (ret)
		goto deregister_frames;

	wlan_hdd_init_chan_info(hdd_ctx);

	EXIT();
	return 0;

deregister_frames:
	wlan_hdd_cfg80211_deregister_frames(adapter);
out:
	return -EINVAL;

}


/**
 * hdd_features_deinit() - Deinit features
 * @hdd_ctx:	HDD context
 *
 * De-Initialize features and their feature context.
 *
 * Return: none.
 */
static void hdd_features_deinit(hdd_context_t *hdd_ctx)
{
	wlan_hdd_deinit_chan_info(hdd_ctx);
	wlan_hdd_tsf_deinit(hdd_ctx);
}

/**
 * hdd_set_rx_mode_rps() - Enable/disable RPS in SAP mode
 * @hdd_context_t *hdd_ctx
 * @hdd_adapter_t *padapter
 * @bool enble
 *
 * Return: none
 */
void hdd_set_rx_mode_rps(hdd_context_t *hdd_ctx, void *padapter,
			 bool enable)
{
	struct cds_config_info *cds_cfg = cds_get_ini_config();
	hdd_adapter_t *adapter = padapter;

	if (adapter && hdd_ctx && cds_cfg &&
	    !hdd_ctx->rps && cds_cfg->uc_offload_enabled) {
		if (enable && !cds_cfg->rps_enabled)
			hdd_send_rps_ind(adapter);
		else if (!enable && cds_cfg->rps_enabled)
			hdd_send_rps_disable_ind(adapter);
	}
}

#ifdef MSM_PLATFORM
static void hdd_register_lro_callbacks(struct cds_dp_cbacks *dp_cbacks)
{
	dp_cbacks->hdd_en_lro_in_cc_cb = hdd_enable_rx_ol_in_concurrency;
	dp_cbacks->hdd_disble_lro_in_cc_cb = hdd_disable_rx_ol_in_concurrency;
}
#else
static void hdd_register_lro_callbacks(struct cds_dp_cbacks *dp_cbacks)
{
	dp_cbacks->hdd_en_lro_in_cc_cb = NULL;
	dp_cbacks->hdd_disble_lro_in_cc_cb = NULL;
}
#endif

/**
 * hdd_configure_cds() - Configure cds modules
 * @hdd_ctx:	HDD context
 * @adapter:	Primary adapter context
 *
 * Enable Cds modules after WLAN firmware is up.
 *
 * Return: 0 on success and errno on failure.
 */
int hdd_configure_cds(hdd_context_t *hdd_ctx, hdd_adapter_t *adapter)
{
	int ret;
	QDF_STATUS status;
	/* structure of SME function pointers to be used by CDS */
	struct cds_sme_cbacks sme_cbacks;
	enum dfs_region dfs_reg;

	/* structure of datapath function pointers to be used by CDS */
	struct cds_dp_cbacks dp_cbacks = {0};

	ret = hdd_pre_enable_configure(hdd_ctx);
	if (ret) {
		hdd_err("Failed to pre-configure cds");
		goto out;
	}

	/* Always get latest IPA resources allocated from cds_open and configure
	 * IPA module before configuring them to FW. Sequence required as crash
	 * observed otherwise.
	 */
	if (hdd_ipa_uc_ol_init(hdd_ctx)) {
		hdd_err("Failed to setup pipes");
		goto out;
	}

	/*
	 * Start CDS which starts up the SME/MAC/HAL modules and everything
	 * else
	 */
	status = cds_enable(hdd_ctx->pcds_context);

	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("cds_enable failed");
		goto out;
	}

	status = hdd_post_cds_enable_config(hdd_ctx);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("hdd_post_cds_enable_config failed");
		goto cds_disable;
	}

	ret = hdd_features_init(hdd_ctx, adapter);
	if (ret)
		goto cds_disable;

	sme_cbacks.sme_get_valid_channels = sme_cfg_get_str;
	sme_cbacks.sme_get_nss_for_vdev = sme_get_vdev_type_nss;
	status = cds_init_policy_mgr(&sme_cbacks);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("Policy manager initialization failed");
		goto hdd_features_deinit;
	}

	if (hdd_ctx->ol_enable) {
		hdd_register_lro_callbacks(&dp_cbacks);
	}

	dp_cbacks.hdd_set_rx_mode_rps_cb = hdd_set_rx_mode_rps;

	dp_cbacks.ol_txrx_update_mac_id_cb = ol_txrx_update_mac_id;
	dp_cbacks.hdd_ipa_set_mcc_mode_cb = hdd_ipa_set_mcc_mode;
	if (cds_register_dp_cb(&dp_cbacks) != QDF_STATUS_SUCCESS)
		hdd_err("Unable to register datapath callbacks in CDS");
	if (cds_register_mode_change_cb(wlan_hdd_send_mode_change_event))
		hdd_err("Unable to register mode change callback in CDS");
	if (hdd_ctx->config->enable_phy_reg_retention)
		sme_cli_set_command(0, WMI_PDEV_PARAM_FAST_PWR_TRANSITION,
			hdd_ctx->config->enable_phy_reg_retention, PDEV_CMD);
	sme_cli_set_command(0, WMI_PDEV_PARAM_GCMP_SUPPORT_ENABLE,
			    hdd_ctx->config->gcmp_enabled, PDEV_CMD);

	sme_cli_set_command(0, (int)WMI_PDEV_AUTO_DETECT_POWER_FAILURE,
			    hdd_ctx->config->auto_pwr_save_fail_mode, PDEV_CMD);

	cds_get_dfs_region(&dfs_reg);
	cds_set_wma_dfs_region(dfs_reg);

	if (hdd_enable_egap(hdd_ctx))
		hdd_debug("enhance green ap is not enabled");

	if (0 != wlan_hdd_set_wow_pulse(hdd_ctx, true))
		hdd_debug("Failed to set wow pulse");

	hdd_send_all_sme_action_ouis(hdd_ctx);

	return 0;

hdd_features_deinit:
	hdd_deregister_cb(hdd_ctx);
	wlan_hdd_cfg80211_deregister_frames(adapter);
cds_disable:
	cds_disable(hdd_ctx->pcds_context);
out:
	return -EINVAL;
}

/**
 * hdd_deconfigure_cds() -De-Configure cds
 * @hdd_ctx:	HDD context
 *
 * Deconfigure Cds modules before WLAN firmware is down.
 *
 * Return: 0 on success and errno on failure.
 */
static int hdd_deconfigure_cds(hdd_context_t *hdd_ctx)
{
	QDF_STATUS qdf_status;
	int ret = 0;

	ENTER();

	/* De-init features */
	hdd_features_deinit(hdd_ctx);

	sme_destroy_config(hdd_ctx->hHal);

	/* De-init Policy Manager */
	if (!QDF_IS_STATUS_SUCCESS(cds_deinit_policy_mgr())) {
		hdd_err("Failed to deinit policy manager");
		/* Proceed and complete the clean up */
		ret = -EINVAL;
	}

	qdf_status = cds_deregister_dp_cb();
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		hdd_err("Failed to deregister datapath callbacks from CDS :%d",
			qdf_status);
	}

	qdf_status = cds_deregister_mode_change_cb();
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		hdd_err("Failed to deregister mode change callback from CDS :%d",
			qdf_status);
	}

	qdf_status = cds_disable(hdd_ctx->pcds_context);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		hdd_err("Failed to Disable the CDS Modules! :%d",
			qdf_status);
		ret = -EINVAL;
	}

	if (hdd_ipa_uc_ol_deinit(hdd_ctx)) {
		hdd_err("Failed to disconnect pipes");
		ret = -EINVAL;
	}

	EXIT();
	return ret;
}



/**
 * hdd_wlan_stop_modules - Single driver state machine for stoping modules
 * @hdd_ctx: HDD context
 * @ftm_mode: ftm mode
 *
 * This function maintains the driver state machine it will be invoked from
 * exit, shutdown and con_mode change handler. Depending on the driver state
 * shall perform the stopping/closing of the modules.
 *
 * Return: 0 for success; non-zero for failure
 */
int hdd_wlan_stop_modules(hdd_context_t *hdd_ctx, bool ftm_mode)
{
	void *hif_ctx;
	qdf_device_t qdf_ctx;
	QDF_STATUS qdf_status;
	int ret = 0;
	bool is_recover_stop = cds_is_driver_recovering();
	int active_threads;
	int debugfs_threads;

	ENTER();
	hdd_alert("stop WLAN module: entering driver status=%d",
		  hdd_ctx->driver_status);

	qdf_ctx = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	if (!qdf_ctx) {
		hdd_err("QDF device context NULL");
		return -EINVAL;
	}

	mutex_lock(&hdd_ctx->iface_change_lock);
	hdd_ctx->stop_modules_in_progress = true;
	cds_set_module_stop_in_progress(true);

	active_threads = cds_return_external_threads_count();
	debugfs_threads = hdd_return_debugfs_threads_count();
	if (active_threads > 0 || debugfs_threads > 0 ||
	    hdd_ctx->isWiphySuspended) {
		hdd_warn("External threads %d, Debugfs threads %d, wiphy suspend %d",
			 active_threads, debugfs_threads,
			 hdd_ctx->isWiphySuspended);

		if (active_threads)
			cds_print_external_threads();

		if (IS_IDLE_STOP && !ftm_mode) {
			mutex_unlock(&hdd_ctx->iface_change_lock);
			qdf_sched_delayed_work(&hdd_ctx->iface_idle_work,
				       hdd_ctx->config->iface_change_wait_time);
			hdd_prevent_suspend_timeout(
				hdd_ctx->config->iface_change_wait_time,
				WIFI_POWER_EVENT_WAKELOCK_IFACE_CHANGE_TIMER);
			hdd_ctx->stop_modules_in_progress = false;
			cds_set_module_stop_in_progress(false);
			return 0;
		}
	}

	hdd_info("Present Driver Status: %d", hdd_ctx->driver_status);

	/* free user wowl patterns */
	hdd_free_user_wowl_ptrns();

	switch (hdd_ctx->driver_status) {
	case DRIVER_MODULES_UNINITIALIZED:
		hdd_info("Modules not initialized just return");
		goto done;
	case DRIVER_MODULES_CLOSED:
		hdd_info("Modules already closed");
		goto done;
	case DRIVER_MODULES_ENABLED:
		hdd_info("Wlan transition (OPENED <- ENABLED)");

		hdd_configure_thermal_mitigation(hdd_ctx, qdf_ctx->dev, 0);
		hdd_disable_power_management();
		if (hdd_deconfigure_cds(hdd_ctx)) {
			hdd_err("Failed to de-configure CDS");
			QDF_ASSERT(0);
			ret = -EINVAL;
		}

		hdd_debug("successfully Disabled the CDS modules!");
		hdd_ctx->driver_status = DRIVER_MODULES_OPENED;

		break;
	case DRIVER_MODULES_OPENED:
		hdd_debug("Closing CDS modules!");

		break;
	default:
		hdd_err("Trying to stop wlan in a wrong state: %d",
				hdd_ctx->driver_status);
		QDF_ASSERT(0);
		ret = -EINVAL;
		goto done;
	}

	hdd_ipa_cleanup(hdd_ctx);
	hdd_sysfs_destroy_version_interface();

	qdf_status = cds_post_disable();
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		hdd_err("Failed to process post CDS disable Modules! :%d",
			qdf_status);
		ret = -EINVAL;
		QDF_ASSERT(0);
	}

	/* De-register the SME callbacks */
	hdd_deregister_cb(hdd_ctx);

	hdd_runtime_suspend_context_deinit(hdd_ctx);

	qdf_status = cds_close(hdd_ctx->pcds_context);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		hdd_warn("Failed to stop CDS: %d", qdf_status);
		ret = -EINVAL;
		QDF_ASSERT(0);
	}

	hif_ctx = cds_get_context(QDF_MODULE_ID_HIF);
	if (!hif_ctx) {
		hdd_err("Hif context is Null");
		ret = -EINVAL;
	}

	if (hdd_ctx->target_hw_name) {
		qdf_mem_free(hdd_ctx->target_hw_name);
		hdd_ctx->target_hw_name = NULL;
	}

	hdd_hif_close(hif_ctx);

	ol_cds_free();

	if (IS_IDLE_STOP && cds_is_target_ready()) {
		ret = pld_power_off(qdf_ctx->dev);
		if (ret)
			hdd_err("CNSS power down failed put device into Low power mode:%d",
				ret);
	}
	/* Free the cache channels of the command SET_DISABLE_CHANNEL_LIST */
	wlan_hdd_free_cache_channels(hdd_ctx);
	hdd_driver_mem_cleanup();

	/* many adapter resources are not freed by design in SSR case */
	if (!is_recover_stop)
		hdd_check_for_leaks();

	qdf_mem_set_domain(QDF_MEM_DOMAIN_INIT);

	/* Once the firmware sequence is completed reset this flag */
	hdd_ctx->imps_enabled = false;
	hdd_ctx->driver_status = DRIVER_MODULES_CLOSED;
	hdd_info("Wlan transition (now CLOSED)");

done:
	hdd_ctx->stop_modules_in_progress = false;
	cds_set_module_stop_in_progress(false);
	mutex_unlock(&hdd_ctx->iface_change_lock);
	hdd_alert("stop WLAN module: exit driver status=%d",
		  hdd_ctx->driver_status);

	EXIT();

	return ret;
}


/**
 * hdd_state_info_dump() - prints state information of hdd layer
 * @buf: buffer pointer
 * @size: size of buffer to be filled
 *
 * This function is used to dump state information of hdd layer
 *
 * Return: None
 */
static void hdd_state_info_dump(char **buf_ptr, uint16_t *size)
{
	hdd_context_t *hdd_ctx;
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	QDF_STATUS status;
	hdd_station_ctx_t *hdd_sta_ctx;
	hdd_adapter_t *adapter;
	uint16_t len = 0;
	char *buf = *buf_ptr;

	hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	if (!hdd_ctx) {
		hdd_err("Failed to get hdd context ");
		return;
	}

	hdd_debug("size of buffer: %d", *size);

	len += scnprintf(buf + len, *size - len,
		"\n isWiphySuspended %d", hdd_ctx->isWiphySuspended);
	len += scnprintf(buf + len, *size - len,
		"\n isMcThreadSuspended %d",
		hdd_ctx->isMcThreadSuspended);

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);

	while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
		adapter = adapter_node->pAdapter;
		if (adapter->dev)
			len += scnprintf(buf + len, *size - len,
				"\n device name: %s", adapter->dev->name);
		len += scnprintf(buf + len, *size - len,
				"\n device_mode: %d", adapter->device_mode);
		switch (adapter->device_mode) {
		case QDF_STA_MODE:
		case QDF_P2P_CLIENT_MODE:
			hdd_sta_ctx = WLAN_HDD_GET_STATION_CTX_PTR(adapter);
			len += scnprintf(buf + len, *size - len,
				"\n connState: %d",
				hdd_sta_ctx->conn_info.connState);
			break;

		default:
			break;
		}
		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}

	*size -= len;
	*buf_ptr += len;
}

/**
 * hdd_register_debug_callback() - registration function for hdd layer
 * to print hdd state information
 *
 * Return: None
 */
static void hdd_register_debug_callback(void)
{
	qdf_register_debug_callback(QDF_MODULE_ID_HDD, &hdd_state_info_dump);
}

/*
 * wlan_init_bug_report_lock() - Initialize bug report lock
 *
 * This function is used to create bug report lock
 *
 * Return: None
 */
static void wlan_init_bug_report_lock(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		hdd_err("cds context is NULL");
		return;
	}

	qdf_spinlock_create(&p_cds_context->bug_report_lock);
}

void hdd_dp_trace_init(struct hdd_config *config)
{

	bool live_mode = DP_TRACE_CONFIG_DEFAULT_LIVE_MODE;
	uint8_t thresh = DP_TRACE_CONFIG_DEFAULT_THRESH;
	uint16_t thresh_time_limit = DP_TRACE_CONFIG_DEFAULT_THRESH_TIME_LIMIT;
	uint8_t verbosity = DP_TRACE_CONFIG_DEFAULT_VERBOSTY;
	uint8_t proto_bitmap = DP_TRACE_CONFIG_DEFAULT_BITMAP;
	uint8_t config_params[DP_TRACE_CONFIG_NUM_PARAMS];
	uint8_t num_entries = 0;
	uint32_t bw_compute_interval;

	hdd_string_to_u8_array(config->dp_trace_config, config_params,
				&num_entries, sizeof(config_params));

	/* calculating, num bw timer intervals in a second (1000ms) */
	bw_compute_interval = GET_BW_COMPUTE_INTV(config);
	if (bw_compute_interval <= 1000 && bw_compute_interval > 0)
		thresh_time_limit =
			(1000 / bw_compute_interval);
	else if (bw_compute_interval > 1000) {
		hdd_err("busBandwidthComputeInterval > 1000, using 1000");
		thresh_time_limit = 1;
	} else
		hdd_err("busBandwidthComputeInterval is 0, using defaults");

	switch (num_entries) {
	case 4:
		proto_bitmap = config_params[3];
		/* fall through */
	case 3:
		verbosity = config_params[2];
		/* fall through */
	case 2:
		thresh = config_params[1];
		/* fall through */
	case 1:
		live_mode = config_params[0];
		/* fall through */
	default:
		hdd_debug("live_mode %u thresh %u time_limit %u verbosity %u bitmap 0x%x",
			live_mode, thresh, thresh_time_limit,
			verbosity, proto_bitmap);
		break;
	};

	qdf_dp_trace_init(live_mode, thresh, thresh_time_limit,
			verbosity, proto_bitmap);

}
/**
 * hdd_wlan_startup() - HDD init function
 * @dev:	Pointer to the underlying device
 *
 * This is the driver startup code executed once a WLAN device has been detected
 *
 * Return:  0 for success, < 0 for failure
 */
int hdd_wlan_startup(struct device *dev)
{
	QDF_STATUS status;
	hdd_context_t *hdd_ctx;
	int ret;
	bool rtnl_held;

	ENTER();

	hdd_ctx = hdd_context_create(dev);

	if (IS_ERR(hdd_ctx))
		return PTR_ERR(hdd_ctx);

	qdf_nbuf_init_replenish_timer();

	ret = hdd_init_netlink_services(hdd_ctx);
	if (ret)
		goto err_hdd_free_context;

	ret = qdf_mutex_create(&hdd_ctx->cache_channel_lock);
	if (QDF_IS_STATUS_ERROR(ret))
		goto err_hdd_free_context;

	hdd_request_manager_init();
	hdd_green_ap_init(hdd_ctx);

	hdd_init_spectral_scan(hdd_ctx);

#ifdef FEATURE_WLAN_AP_AP_ACS_OPTIMIZE
	status = qdf_mc_timer_init(&hdd_ctx->skip_acs_scan_timer,
				   QDF_TIMER_TYPE_SW,
				   hdd_skip_acs_scan_timer_handler,
				   (void *)hdd_ctx);
	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_err("Failed to init ACS Skip timer");
	qdf_spinlock_create(&hdd_ctx->acs_skip_lock);
#endif
	qdf_atomic_init(&hdd_ctx->con_mode_flag);
	qdf_mc_timer_init(&hdd_ctx->tdls_source_timer,
			  QDF_TIMER_TYPE_SW,
			  wlan_hdd_change_tdls_mode,
			  hdd_ctx);

	hdd_bus_bandwidth_init(hdd_ctx);
	hdd_driver_memdump_init();

	ret = hdd_wlan_start_modules(hdd_ctx, NULL, false);
	if (ret) {
		hdd_err("Failed to start modules: %d", ret);
		goto err_exit_nl_srv;
	}

	wlan_hdd_update_wiphy(hdd_ctx);

	hdd_ctx->hHal = cds_get_context(QDF_MODULE_ID_SME);

	if (NULL == hdd_ctx->hHal) {
		hdd_err("HAL context is null");
		goto err_stop_modules;
	}

	ret = hdd_wiphy_init(hdd_ctx);
	if (ret) {
		hdd_err("Failed to initialize wiphy: %d", ret);
		goto err_stop_modules;
	}

	if (hdd_ctx->config->enable_dp_trace)
		hdd_dp_trace_init(hdd_ctx->config);


	ret = hdd_initialize_mac_address(hdd_ctx);
	if (ret) {
		hdd_err("MAC initializtion failed: %d", ret);
		goto err_wiphy_unregister;
	}

	ret = register_netdevice_notifier(&hdd_netdev_notifier);
	if (ret) {
		hdd_err("register_netdevice_notifier failed: %d", ret);
		goto err_wiphy_unregister;
	}

	ret = register_reboot_notifier(&system_reboot_notifier);
	if (ret) {
		hdd_err("Failed to register reboot notifier: %d", ret);
		goto err_unregister_netdev;
	}

	rtnl_held = hdd_hold_rtnl_lock();

	ret = hdd_open_interfaces(hdd_ctx, rtnl_held);
	if (ret) {
		hdd_err("Failed to open interfaces: %d", ret);
		goto err_release_rtnl_lock;
	}

	hdd_release_rtnl_lock();
	rtnl_held = false;

	wlan_hdd_update_11n_mode(hdd_ctx->config);

	hdd_lpass_notify_wlan_version(hdd_ctx);

	if (hdd_ctx->rps)
		hdd_set_rps_cpu_mask(hdd_ctx);

	ret = hdd_register_notifiers(hdd_ctx);
	if (ret)
		goto err_close_adapters;

	status = wlansap_global_init();
	if (QDF_IS_STATUS_ERROR(status))
		goto err_close_adapters;

	if (hdd_ctx->config->fIsImpsEnabled)
		hdd_set_idle_ps_config(hdd_ctx, true);
	else
		hdd_set_idle_ps_config(hdd_ctx, false);

	qdf_sched_delayed_work(&hdd_ctx->iface_idle_work,
			       hdd_ctx->config->iface_change_wait_time);
	hdd_prevent_suspend_timeout(
		hdd_ctx->config->iface_change_wait_time,
		WIFI_POWER_EVENT_WAKELOCK_IFACE_CHANGE_TIMER);

	goto success;

err_close_adapters:
	hdd_close_all_adapters(hdd_ctx, rtnl_held);

err_release_rtnl_lock:
	unregister_reboot_notifier(&system_reboot_notifier);
	if (rtnl_held)
		hdd_release_rtnl_lock();

err_unregister_netdev:
	unregister_netdevice_notifier(&hdd_netdev_notifier);

err_wiphy_unregister:
	wiphy_unregister(hdd_ctx->wiphy);

err_stop_modules:
	hdd_wlan_stop_modules(hdd_ctx, false);

err_exit_nl_srv:
	hdd_driver_memdump_deinit();

	qdf_mc_timer_destroy(&hdd_ctx->tdls_source_timer);
	hdd_bus_bandwidth_destroy(hdd_ctx);

	hdd_green_ap_deinit(hdd_ctx);
	hdd_request_manager_deinit();
	hdd_exit_netlink_services(hdd_ctx);

err_hdd_free_context:
	if (cds_is_fw_down())
		hdd_err("Not setting the complete event as fw is down");
	else
		hdd_start_complete(ret);

	qdf_nbuf_deinit_replenish_timer();
	hdd_context_destroy(hdd_ctx);
	return ret;

success:
	EXIT();
	return 0;
}

/**
 * hdd_wlan_update_target_info() - update target type info
 * @hdd_ctx: HDD context
 * @context: hif context
 *
 * Update target info received from firmware in hdd context
 * Return:None
 */

void hdd_wlan_update_target_info(hdd_context_t *hdd_ctx, void *context)
{
	struct hif_target_info *tgt_info = hif_get_target_info_handle(context);

	if (!tgt_info) {
		hdd_err("Target info is Null");
		return;
	}

	hdd_ctx->target_type = tgt_info->target_type;
}

void hdd_get_nud_stats_cb(void *data, struct rsp_stats *rsp, void *context)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *)data;
	int status;
	hdd_adapter_t *adapter = NULL;
	struct hdd_request *request = NULL;

	ENTER();

	if (!rsp) {
		hdd_err("data is null");
		return;
	}

	status = wlan_hdd_validate_context(hdd_ctx);
	if (status != 0)
		return;

	request = hdd_request_get(context);
	if (!request) {
		hdd_err("obselete request");
		return;
	}

	adapter = hdd_get_adapter_by_vdev(hdd_ctx, rsp->vdev_id);
	if ((NULL == adapter) || (WLAN_HDD_ADAPTER_MAGIC != adapter->magic)) {
		hdd_err("Invalid adapter or adapter has invalid magic");
		hdd_request_put(request);
		return;
	}

	hdd_debug("rsp->arp_req_enqueue :%x", rsp->arp_req_enqueue);
	hdd_debug("rsp->arp_req_tx_success :%x", rsp->arp_req_tx_success);
	hdd_debug("rsp->arp_req_tx_failure :%x", rsp->arp_req_tx_failure);
	hdd_debug("rsp->arp_rsp_recvd :%x", rsp->arp_rsp_recvd);
	hdd_debug("rsp->out_of_order_arp_rsp_drop_cnt :%x",
		   rsp->out_of_order_arp_rsp_drop_cnt);
	hdd_debug("rsp->dad_detected :%x", rsp->dad_detected);
	hdd_debug("rsp->connect_status :%x", rsp->connect_status);
	hdd_debug("rsp->ba_session_establishment_status :%x",
		   rsp->ba_session_establishment_status);

	adapter->hdd_stats.hdd_arp_stats.rx_fw_cnt = rsp->arp_rsp_recvd;
	adapter->dad |= rsp->dad_detected;
	adapter->con_status = rsp->connect_status;

	/* Flag true indicates connectivity check stats present. */
	if (rsp->connect_stats_present) {
		hdd_notice("rsp->tcp_ack_recvd :%x", rsp->tcp_ack_recvd);
		hdd_notice("rsp->icmpv4_rsp_recvd :%x", rsp->icmpv4_rsp_recvd);
		adapter->hdd_stats.hdd_tcp_stats.rx_fw_cnt = rsp->tcp_ack_recvd;
		adapter->hdd_stats.hdd_icmpv4_stats.rx_fw_cnt =
							rsp->icmpv4_rsp_recvd;
	}

	hdd_request_complete(request);
	hdd_request_put(request);

	EXIT();
}

/**
 * hdd_register_cb - Register HDD callbacks.
 * @hdd_ctx: HDD context
 *
 * Register the HDD callbacks to CDS/SME.
 *
 * Return: 0 for success or Error code for failure
 */
int hdd_register_cb(hdd_context_t *hdd_ctx)
{
	QDF_STATUS status;
	int ret = 0;

	if (QDF_GLOBAL_FTM_MODE == hdd_get_conparam()) {
		hdd_err("in ftm mode, no need to register callbacks");
		return ret;
	}

	ENTER();

	sme_register11d_scan_done_callback(hdd_ctx->hHal, hdd_11d_scan_done);

	sme_register_oem_data_rsp_callback(hdd_ctx->hHal,
					hdd_send_oem_data_rsp_msg);

	sme_register_mgmt_frame_ind_callback(hdd_ctx->hHal,
					     hdd_indicate_mgmt_frame);
	sme_set_tsfcb(hdd_ctx->hHal, hdd_get_tsf_cb, hdd_ctx);
	sme_nan_register_callback(hdd_ctx->hHal,
				  wlan_hdd_cfg80211_nan_callback);
	sme_stats_ext_register_callback(hdd_ctx->hHal,
					wlan_hdd_cfg80211_stats_ext_callback);
	sme_stats_ext2_register_callback(hdd_ctx->hHal,
					wlan_hdd_cfg80211_stats_ext2_callback);

	sme_ext_scan_register_callback(hdd_ctx->hHal,
				       wlan_hdd_cfg80211_extscan_callback);

	status = cds_register_sap_restart_channel_switch_cb(
			hdd_sap_restart_with_channel_switch);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("restart cb registration failed");
		ret = -EINVAL;
		return ret;
	}

	sme_set_rssi_threshold_breached_cb(hdd_ctx->hHal,
					   hdd_rssi_threshold_breached);

	status = sme_apf_offload_register_callback(hdd_ctx->hHal,
						   hdd_get_apf_capabilities_cb);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("set apf offload callback failed");
		ret = -EINVAL;
		return ret;
	}

	sme_set_link_layer_stats_ind_cb(hdd_ctx->hHal,
				wlan_hdd_cfg80211_link_layer_stats_callback);

	sme_rso_cmd_status_cb(hdd_ctx->hHal, wlan_hdd_rso_cmd_status_cb);

	sme_set_link_layer_ext_cb(hdd_ctx->hHal,
			wlan_hdd_cfg80211_link_layer_stats_ext_callback);

	status = sme_set_lost_link_info_cb(hdd_ctx->hHal,
					   hdd_lost_link_info_cb);
	/* print error and not block the startup process */
	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_err("set lost link info callback failed");

	ret = hdd_register_data_stall_detect_cb();
	if (ret) {
		hdd_err("Register data stall detect detect callback failed.");
		return ret;
	}

	wlan_hdd_dcc_register_for_dcc_stats_event(hdd_ctx);

	status = sme_set_bt_activity_info_cb(hdd_ctx->hHal,
					     hdd_bt_activity_cb);
	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_err("set bt activity info callback failed");

	status = sme_congestion_register_callback(hdd_ctx->hHal,
					     hdd_update_cca_info_cb);
	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_err("set congestion callback failed");

	sme_apf_read_memory_register_callback(hdd_ctx->hHal,
					  hdd_apf_read_memory_callback);

	EXIT();

	return ret;
}

/**
 * hdd_deregister_cb() - De-Register HDD callbacks.
 * @hdd_ctx: HDD context
 *
 * De-Register the HDD callbacks to CDS/SME.
 *
 * Return: void
 */
void hdd_deregister_cb(hdd_context_t *hdd_ctx)
{
	QDF_STATUS status;
	int ret;

	if (QDF_GLOBAL_FTM_MODE == hdd_get_conparam()) {
		hdd_err("in ftm mode, no need to deregister callbacks");
		return;
	}

	ENTER();

	status = sme_deregister_for_dcc_stats_event(hdd_ctx->hHal);
	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_err("De-register of dcc stats callback failed: %d",
			status);

	sme_reset_link_layer_stats_ind_cb(hdd_ctx->hHal);
	status = sme_apf_offload_deregister_callback(hdd_ctx->hHal);
	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_warn("De-register apf offload callback failed: %d",
			status);
	sme_reset_rssi_threshold_breached_cb(hdd_ctx->hHal);

	status = cds_deregister_sap_restart_channel_switch_cb();
	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_warn("De-register restart cb registration failed: %d",
			status);

	sme_stats_ext_register_callback(hdd_ctx->hHal,
					wlan_hdd_cfg80211_stats_ext_callback);

	sme_nan_deregister_callback(hdd_ctx->hHal);
	status = sme_reset_tsfcb(hdd_ctx->hHal);
	if (!QDF_IS_STATUS_SUCCESS(status))
		hdd_err("Failed to de-register tsfcb the callback:%d",
			status);

	ret = hdd_deregister_data_stall_detect_cb();
	if (ret)
		hdd_err("Failed to de-register data stall detect event callback");

	sme_deregister_oem_data_rsp_callback(hdd_ctx->hHal);
	sme_deregister11d_scan_done_callback(hdd_ctx->hHal);
	sme_apf_read_memory_deregister_callback(hdd_ctx->hHal);

	EXIT();
}

/**
 * hdd_softap_sta_deauth() - handle deauth req from HDD
 * @adapter:	Pointer to the HDD
 * @enable:	bool value
 *
 * This to take counter measure to handle deauth req from HDD
 *
 * Return: None
 */
QDF_STATUS hdd_softap_sta_deauth(hdd_adapter_t *adapter,
				 struct tagCsrDelStaParams *pDelStaParams)
{
	QDF_STATUS qdf_status = QDF_STATUS_E_FAULT;

	ENTER();

	hdd_debug("hdd_softap_sta_deauth:(%pK, false)",
	       (WLAN_HDD_GET_CTX(adapter))->pcds_context);

	/* Ignore request to deauth bcmc station */
	if (pDelStaParams->peerMacAddr.bytes[0] & 0x1)
		return qdf_status;

	qdf_status =
		wlansap_deauth_sta(WLAN_HDD_GET_SAP_CTX_PTR(adapter),
				   pDelStaParams);

	EXIT();
	return qdf_status;
}

/**
 * hdd_softap_sta_disassoc() - take counter measure to handle deauth req from HDD
 * @adapter:	Pointer to the HDD
 * @p_del_sta_params: pointer to station deletion parameters
 *
 * This to take counter measure to handle deauth req from HDD
 *
 * Return: None
 */
void hdd_softap_sta_disassoc(hdd_adapter_t *adapter,
			     struct tagCsrDelStaParams *pDelStaParams)
{
	ENTER();

	hdd_debug("hdd_softap_sta_disassoc:(%pK, false)",
	       (WLAN_HDD_GET_CTX(adapter))->pcds_context);

	/* Ignore request to disassoc bcmc station */
	if (pDelStaParams->peerMacAddr.bytes[0] & 0x1)
		return;

	wlansap_disassoc_sta(WLAN_HDD_GET_SAP_CTX_PTR(adapter),
			     pDelStaParams);
}

void hdd_softap_tkip_mic_fail_counter_measure(hdd_adapter_t *adapter,
					      bool enable)
{
	ENTER();

	hdd_debug("hdd_softap_tkip_mic_fail_counter_measure:(%pK, false)",
	       (WLAN_HDD_GET_CTX(adapter))->pcds_context);

	wlansap_set_counter_measure(WLAN_HDD_GET_SAP_CTX_PTR(adapter),
				    (bool) enable);
}

/**
 * hdd_issta_p2p_clientconnected() - check if sta or p2p client is connected
 * @hdd_ctx:	HDD Context
 *
 * API to find if there is any STA or P2P-Client is connected
 *
 * Return: true if connected; false otherwise
 */
QDF_STATUS hdd_issta_p2p_clientconnected(hdd_context_t *hdd_ctx)
{
	return sme_is_sta_p2p_client_connected(hdd_ctx->hHal);
}

/**
 * wlan_hdd_disable_roaming() - disable roaming on all STAs except the input one
 * @cur_adapter: Current HDD adapter passed from caller
 *
 * This function loops through all adapters and disables roaming on each STA
 * mode adapter except the current adapter passed from the caller
 *
 * Return: None
 */
void wlan_hdd_disable_roaming(hdd_adapter_t *cur_adapter)
{
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(cur_adapter);
	hdd_adapter_t *adapter = NULL;
	hdd_adapter_list_node_t *adapter_node = NULL;
	hdd_adapter_list_node_t *next = NULL;
	QDF_STATUS status;
	hdd_wext_state_t *wext_state;
	hdd_station_ctx_t *sta_ctx;
	tCsrRoamProfile *roam_profile;

	if (!cds_is_sta_active_connection_exists()) {
		hdd_debug("No active sta session");
		return;
	}

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);
	while (QDF_IS_STATUS_SUCCESS(status) && adapter_node) {
		adapter = adapter_node->pAdapter;
		wext_state = WLAN_HDD_GET_WEXT_STATE_PTR(adapter);
		sta_ctx = WLAN_HDD_GET_STATION_CTX_PTR(adapter);
		roam_profile = &wext_state->roamProfile;

		if (cur_adapter->sessionId != adapter->sessionId &&
		    adapter->device_mode == QDF_STA_MODE &&
		    hdd_conn_is_connected(sta_ctx)) {
			hdd_debug("%d Disable roaming",
				  adapter->sessionId);
			sme_stop_roaming(WLAN_HDD_GET_HAL_CTX(adapter),
					 adapter->sessionId,
					 eCsrDriverDisabled);
		}
		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}
}

/**
 * wlan_hdd_enable_roaming() - enable roaming on all STAs except the input one
 * @cur_adapter: Current HDD adapter passed from caller
 *
 * This function loops through all adapters and enables roaming on each STA
 * mode adapter except the current adapter passed from the caller
 *
 * Return: None
 */
void wlan_hdd_enable_roaming(hdd_adapter_t *cur_adapter)
{
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(cur_adapter);
	hdd_adapter_t *adapter = NULL;
	hdd_adapter_list_node_t *adapter_node = NULL;
	hdd_adapter_list_node_t *next = NULL;
	QDF_STATUS status;
	hdd_wext_state_t *wext_state;
	hdd_station_ctx_t *sta_ctx;
	tCsrRoamProfile *roam_profile;

	if (!cds_is_sta_active_connection_exists()) {
		hdd_debug("No active sta session");
		return;
	}

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);
	while (QDF_IS_STATUS_SUCCESS(status) && adapter_node) {
		adapter = adapter_node->pAdapter;
		wext_state = WLAN_HDD_GET_WEXT_STATE_PTR(adapter);
		sta_ctx = WLAN_HDD_GET_STATION_CTX_PTR(adapter);
		roam_profile = &wext_state->roamProfile;

		if (cur_adapter->sessionId != adapter->sessionId &&
		    adapter->device_mode == QDF_STA_MODE &&
		    hdd_conn_is_connected(sta_ctx)) {
			hdd_debug("%d Enable roaming",
				  adapter->sessionId);
			sme_start_roaming(WLAN_HDD_GET_HAL_CTX(adapter),
					  adapter->sessionId,
					  REASON_DRIVER_ENABLED);
		}
		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}
}

/**
 * nl_srv_bcast_svc() - Wrapper function to send bcast msgs to SVC mcast group
 * @skb: sk buffer pointer
 *
 * Sends the bcast message to SVC multicast group with generic nl socket
 * if CNSS_GENL is enabled. Else, use the legacy netlink socket to send.
 *
 * Return: None
 */
static void nl_srv_bcast_svc(struct sk_buff *skb)
{
#ifdef CNSS_GENL
	nl_srv_bcast(skb, CLD80211_MCGRP_SVC_MSGS, WLAN_NL_MSG_SVC);
#else
	nl_srv_bcast(skb);
#endif
}

void wlan_hdd_send_svc_nlink_msg(int radio, int type, void *data, int len)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	tAniMsgHdr *ani_hdr;
	void *nl_data = NULL;
	int flags = GFP_KERNEL;
	struct radio_index_tlv *radio_info;
	int tlv_len;

	if (in_interrupt() || irqs_disabled() || in_atomic())
		flags = GFP_ATOMIC;

	skb = alloc_skb(NLMSG_SPACE(WLAN_NL_MAX_PAYLOAD), flags);

	if (skb == NULL)
		return;

	nlh = (struct nlmsghdr *)skb->data;
	nlh->nlmsg_pid = 0;     /* from kernel */
	nlh->nlmsg_flags = 0;
	nlh->nlmsg_seq = 0;
	nlh->nlmsg_type = WLAN_NL_MSG_SVC;

	ani_hdr = NLMSG_DATA(nlh);
	ani_hdr->type = type;

	switch (type) {
	case WLAN_SVC_FW_CRASHED_IND:
	case WLAN_SVC_FW_SHUTDOWN_IND:
	case WLAN_SVC_WLAN_AUTO_SHUTDOWN_IND:
	case WLAN_SVC_WLAN_AUTO_SHUTDOWN_CANCEL_IND:
		ani_hdr->length = 0;
		nlh->nlmsg_len = NLMSG_LENGTH((sizeof(tAniMsgHdr)));
		break;
	case WLAN_SVC_WLAN_STATUS_IND:
	case WLAN_SVC_WLAN_VERSION_IND:
	case WLAN_SVC_DFS_CAC_START_IND:
	case WLAN_SVC_DFS_CAC_END_IND:
	case WLAN_SVC_DFS_RADAR_DETECT_IND:
	case WLAN_SVC_DFS_ALL_CHANNEL_UNAVAIL_IND:
	case WLAN_SVC_WLAN_TP_IND:
	case WLAN_SVC_WLAN_TP_TX_IND:
	case WLAN_SVC_RPS_ENABLE_IND:
	case WLAN_SVC_CORE_MINFREQ:
	case WLAN_SVC_LTE_COEX_IND:
		ani_hdr->length = len;
		nlh->nlmsg_len = NLMSG_LENGTH((sizeof(tAniMsgHdr) + len));
		nl_data = (char *)ani_hdr + sizeof(tAniMsgHdr);
		memcpy(nl_data, data, len);
		break;

	default:
		hdd_err("WLAN SVC: Attempt to send unknown nlink message %d",
		       type);
		kfree_skb(skb);
		return;
	}

	/*
	 * Add radio index at the end of the svc event in TLV format to maintain
	 * the backward compatibility with userspace applications.
	 */

	tlv_len = 0;

	if ((sizeof(*ani_hdr) + len + sizeof(struct radio_index_tlv))
		< WLAN_NL_MAX_PAYLOAD) {
		radio_info  = (struct radio_index_tlv *)((char *) ani_hdr +
		sizeof(*ani_hdr) + len);
		radio_info->type = (unsigned short) WLAN_SVC_WLAN_RADIO_INDEX;
		radio_info->length = (unsigned short) sizeof(radio_info->radio);
		radio_info->radio = radio;
		tlv_len = sizeof(*radio_info);
		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_DEBUG,
			"Added radio index tlv - radio index %d",
			radio_info->radio);
	}

	nlh->nlmsg_len += tlv_len;
	skb_put(skb, NLMSG_SPACE(sizeof(tAniMsgHdr) + len + tlv_len));

	nl_srv_bcast_svc(skb);
}

#ifdef FEATURE_WLAN_AUTO_SHUTDOWN
void wlan_hdd_auto_shutdown_cb(void)
{
	hdd_context_t *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);

	if (!hdd_ctx)
		return;

	hdd_debug("Wlan Idle. Sending Shutdown event..");
	wlan_hdd_send_svc_nlink_msg(hdd_ctx->radio_index,
			WLAN_SVC_WLAN_AUTO_SHUTDOWN_IND, NULL, 0);
}

void wlan_hdd_auto_shutdown_enable(hdd_context_t *hdd_ctx, bool enable)
{
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;
	bool ap_connected = false, sta_connected = false;
	tHalHandle hal_handle;

	hal_handle = hdd_ctx->hHal;
	if (hal_handle == NULL)
		return;

	if (hdd_ctx->config->WlanAutoShutdown == 0)
		return;

	if (enable == false) {
		if (sme_set_auto_shutdown_timer(hal_handle, 0) !=
							QDF_STATUS_SUCCESS) {
			hdd_err("Failed to stop wlan auto shutdown timer");
		}
		wlan_hdd_send_svc_nlink_msg(hdd_ctx->radio_index,
			WLAN_SVC_WLAN_AUTO_SHUTDOWN_CANCEL_IND, NULL, 0);
		return;
	}

	/* To enable shutdown timer check conncurrency */
	if (cds_concurrent_open_sessions_running()) {
		status = hdd_get_front_adapter(hdd_ctx, &adapterNode);

		while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
			adapter = adapterNode->pAdapter;
			if (adapter
			    && adapter->device_mode ==
			    QDF_STA_MODE) {
				if (WLAN_HDD_GET_STATION_CTX_PTR(adapter)->
				    conn_info.connState ==
				    eConnectionState_Associated) {
					sta_connected = true;
					break;
				}
			}
			if (adapter
			    && adapter->device_mode == QDF_SAP_MODE) {
				if (WLAN_HDD_GET_AP_CTX_PTR(adapter)->
				    bApActive == true) {
					ap_connected = true;
					break;
				}
			}
			status = hdd_get_next_adapter(hdd_ctx,
						      adapterNode,
						      &pNext);
			adapterNode = pNext;
		}
	}

	if (ap_connected == true || sta_connected == true) {
		hdd_debug("CC Session active. Shutdown timer not enabled");
		return;
	}

	if (sme_set_auto_shutdown_timer(hal_handle,
					hdd_ctx->config->
					WlanAutoShutdown)
		!= QDF_STATUS_SUCCESS)
			hdd_err("Failed to start wlan auto shutdown timer");
	else
		hdd_debug("Auto Shutdown timer for %d seconds enabled",
			  hdd_ctx->config->WlanAutoShutdown);

}
#endif

hdd_adapter_t *hdd_get_con_sap_adapter(hdd_adapter_t *this_sap_adapter,
							bool check_start_bss)
{
	hdd_context_t *hdd_ctx = WLAN_HDD_GET_CTX(this_sap_adapter);
	hdd_adapter_t *adapter, *con_sap_adapter;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	hdd_adapter_list_node_t *adapterNode = NULL, *pNext = NULL;

	con_sap_adapter = NULL;

	status = hdd_get_front_adapter(hdd_ctx, &adapterNode);
	while (NULL != adapterNode && QDF_STATUS_SUCCESS == status) {
		adapter = adapterNode->pAdapter;
		if (adapter && ((adapter->device_mode == QDF_SAP_MODE) ||
				(adapter->device_mode == QDF_P2P_GO_MODE)) &&
						adapter != this_sap_adapter) {
			if (check_start_bss) {
				if (test_bit(SOFTAP_BSS_STARTED,
						&adapter->event_flags)) {
					con_sap_adapter = adapter;
					break;
				}
			} else {
				con_sap_adapter = adapter;
				break;
			}
		}
		status = hdd_get_next_adapter(hdd_ctx, adapterNode, &pNext);
		adapterNode = pNext;
	}

	return con_sap_adapter;
}

#ifdef MSM_PLATFORM
static inline bool hdd_adapter_is_client(hdd_adapter_t *adapter)
{
	return adapter->device_mode == QDF_STA_MODE ||
		adapter->device_mode == QDF_P2P_CLIENT_MODE;
}

static inline bool hdd_adapter_is_ap(hdd_adapter_t *adapter)
{
	return adapter->device_mode == QDF_SAP_MODE ||
		adapter->device_mode == QDF_P2P_GO_MODE;
}

static bool hdd_any_adapter_is_assoc(hdd_context_t *hdd_ctx)
{
	QDF_STATUS status;
	hdd_adapter_list_node_t *node;

	status = hdd_get_front_adapter(hdd_ctx, &node);
	while (QDF_IS_STATUS_SUCCESS(status) && node) {
		hdd_adapter_t *adapter = node->pAdapter;
		if (adapter &&
		    hdd_adapter_is_client(adapter) &&
		    WLAN_HDD_GET_STATION_CTX_PTR(adapter)->
			conn_info.connState == eConnectionState_Associated) {
			return true;
		}

		if (adapter &&
		    hdd_adapter_is_ap(adapter) &&
		    WLAN_HDD_GET_AP_CTX_PTR(adapter)->bApActive) {
			return true;
		}

		status = hdd_get_next_adapter(hdd_ctx, node, &node);
	}

	return false;
}

void hdd_bus_bw_compute_timer_start(hdd_context_t *hdd_ctx)
{
	ENTER();

	if (QDF_TIMER_STATE_RUNNING ==
	    qdf_mc_timer_get_current_state(&hdd_ctx->bus_bw_timer))
		return;

	qdf_mc_timer_start(&hdd_ctx->bus_bw_timer,
			   hdd_ctx->config->busBandwidthComputeInterval);
	EXIT();
}

void hdd_bus_bw_compute_timer_try_start(hdd_context_t *hdd_ctx)
{
	ENTER();

	if (QDF_TIMER_STATE_RUNNING ==
	    qdf_mc_timer_get_current_state(&hdd_ctx->bus_bw_timer))
		return;

	if (hdd_any_adapter_is_assoc(hdd_ctx))
		qdf_mc_timer_start(&hdd_ctx->bus_bw_timer,
				   hdd_ctx->config->
				   busBandwidthComputeInterval);
	EXIT();

}

void hdd_bus_bw_compute_timer_stop(hdd_context_t *hdd_ctx)
{
	ENTER();

	if (QDF_TIMER_STATE_RUNNING !=
	    qdf_mc_timer_get_current_state(&hdd_ctx->bus_bw_timer)) {
		/* trying to stop timer, when not running is not good */
		hdd_debug("bus band width compute timer is not running");
		return;
	}

	hdd_ipa_set_perf_level(hdd_ctx, 0, 0);
	qdf_mc_timer_stop(&hdd_ctx->bus_bw_timer);
	if (hdd_ctx->config->enable_tcp_delack)
		hdd_reset_tcp_delack(hdd_ctx);

	EXIT();
}

void hdd_bus_bw_compute_timer_try_stop(hdd_context_t *hdd_ctx)
{
	ENTER();

	if (QDF_TIMER_STATE_RUNNING !=
	    qdf_mc_timer_get_current_state(&hdd_ctx->bus_bw_timer)) {
		/* trying to stop timer, when not running is not good */
		hdd_debug("bus band width compute timer is not running");
		return;
	}

	if (!hdd_any_adapter_is_assoc(hdd_ctx)) {
		/* reset the ipa perf level */
		hdd_ipa_set_perf_level(hdd_ctx, 0, 0);
		qdf_mc_timer_stop(&hdd_ctx->bus_bw_timer);
		if (hdd_ctx->config->enable_tcp_delack)
			hdd_reset_tcp_delack(hdd_ctx);
	}
	EXIT();
}
#endif

/**
 * wlan_hdd_check_custom_con_channel_rules() - This function checks the sap's
 *                                            and sta's operating channel.
 * @sta_adapter:  Describe the first argument to foobar.
 * @ap_adapter:   Describe the second argument to foobar.
 * @roam_profile: Roam profile of AP to which STA wants to connect.
 * @concurrent_chnl_same: If both SAP and STA channels are same then
 *                        set this flag to true else false.
 *
 * This function checks the sap's operating channel and sta's operating channel.
 * if both are same then it will return false else it will restart the sap in
 * sta's channel and return true.
 *
 * Return: QDF_STATUS_SUCCESS or QDF_STATUS_E_FAILURE.
 */
QDF_STATUS wlan_hdd_check_custom_con_channel_rules(hdd_adapter_t *sta_adapter,
						  hdd_adapter_t *ap_adapter,
						  tCsrRoamProfile *roam_profile,
						  tScanResultHandle *scan_cache,
						  bool *concurrent_chnl_same)
{
	hdd_ap_ctx_t *hdd_ap_ctx;
	uint8_t channel_id;
	QDF_STATUS status;
	enum tQDF_ADAPTER_MODE device_mode = ap_adapter->device_mode;
	*concurrent_chnl_same = true;

	hdd_ap_ctx = WLAN_HDD_GET_AP_CTX_PTR(ap_adapter);
	status =
	 sme_get_ap_channel_from_scan_cache(WLAN_HDD_GET_HAL_CTX(sta_adapter),
					    roam_profile,
					    scan_cache,
					    &channel_id);
	if (QDF_STATUS_SUCCESS == status) {
		if ((QDF_SAP_MODE == device_mode) &&
			(channel_id < SIR_11A_CHANNEL_BEGIN)) {
			if (hdd_ap_ctx->operatingChannel != channel_id) {
				*concurrent_chnl_same = false;
				hdd_debug("channels are different");
			}
		} else if ((QDF_P2P_GO_MODE == device_mode) &&
				(channel_id >= SIR_11A_CHANNEL_BEGIN)) {
			if (hdd_ap_ctx->operatingChannel != channel_id) {
				*concurrent_chnl_same = false;
				hdd_debug("channels are different");
			}
		}
	} else {
		/*
		 * Lets handle worst case scenario here, Scan cache lookup is
		 * failed so we have to stop the SAP to avoid any channel
		 * discrepancy  between SAP's channel and STA's channel.
		 * Return the status as failure so caller function could know
		 * that scan look up is failed.
		 */
		hdd_err("Finding AP from scan cache failed");
		return QDF_STATUS_E_FAILURE;
	}
	return QDF_STATUS_SUCCESS;
}

/**
 * wlan_hdd_stop_sap() - This function stops bss of SAP.
 * @ap_adapter: SAP adapter
 *
 * This function will process the stopping of sap adapter.
 *
 * Return: None
 */
void wlan_hdd_stop_sap(hdd_adapter_t *ap_adapter)
{
	hdd_ap_ctx_t *hdd_ap_ctx;
	hdd_hostapd_state_t *hostapd_state;
	QDF_STATUS qdf_status;
	hdd_context_t *hdd_ctx;

	if (NULL == ap_adapter) {
		hdd_err("ap_adapter is NULL here");
		return;
	}

	hdd_ap_ctx = WLAN_HDD_GET_AP_CTX_PTR(ap_adapter);
	hdd_ctx = WLAN_HDD_GET_CTX(ap_adapter);
	if (wlan_hdd_validate_context(hdd_ctx))
		return;

	mutex_lock(&hdd_ctx->sap_lock);
	if (test_bit(SOFTAP_BSS_STARTED, &ap_adapter->event_flags)) {
		wlan_hdd_del_station(ap_adapter);
		hdd_cleanup_actionframe(hdd_ctx, ap_adapter);
		hostapd_state = WLAN_HDD_GET_HOSTAP_STATE_PTR(ap_adapter);
		hdd_debug("Now doing SAP STOPBSS");
		qdf_event_reset(&hostapd_state->qdf_stop_bss_event);
		if (QDF_STATUS_SUCCESS == wlansap_stop_bss(hdd_ap_ctx->
							sapContext)) {
			qdf_status = qdf_wait_for_event_completion(
					&hostapd_state->qdf_stop_bss_event,
					SME_CMD_TIMEOUT_VALUE);
			if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
				mutex_unlock(&hdd_ctx->sap_lock);
				hdd_err("SAP Stop Failed");
				return;
			}
		}
		clear_bit(SOFTAP_BSS_STARTED, &ap_adapter->event_flags);
		cds_decr_session_set_pcl(ap_adapter->device_mode,
						ap_adapter->sessionId);
		hdd_debug("SAP Stop Success");
	} else {
		hdd_err("Can't stop ap because its not started");
	}
	mutex_unlock(&hdd_ctx->sap_lock);
}

/**
 * wlan_hdd_start_sap() - this function starts bss of SAP.
 * @ap_adapter: SAP adapter
 *
 * This function will process the starting of sap adapter.
 *
 * Return: None
 */
void wlan_hdd_start_sap(hdd_adapter_t *ap_adapter, bool reinit)
{
	hdd_ap_ctx_t *hdd_ap_ctx;
	hdd_hostapd_state_t *hostapd_state;
	QDF_STATUS qdf_status;
	hdd_context_t *hdd_ctx;
	tsap_Config_t *sap_config;

	if (NULL == ap_adapter) {
		hdd_err("ap_adapter is NULL here");
		return;
	}

	if (QDF_SAP_MODE != ap_adapter->device_mode) {
		hdd_err("SoftAp role has not been enabled");
		return;
	}

	hdd_ctx = WLAN_HDD_GET_CTX(ap_adapter);
	hdd_ap_ctx = WLAN_HDD_GET_AP_CTX_PTR(ap_adapter);
	hostapd_state = WLAN_HDD_GET_HOSTAP_STATE_PTR(ap_adapter);
	sap_config = &ap_adapter->sessionCtx.ap.sapConfig;

	mutex_lock(&hdd_ctx->sap_lock);
	if (test_bit(SOFTAP_BSS_STARTED, &ap_adapter->event_flags))
		goto end;

	if (0 != wlan_hdd_cfg80211_update_apies(ap_adapter)) {
		hdd_err("SAP Not able to set AP IEs");
		goto end;
	}

	cds_set_channel_params(hdd_ap_ctx->sapConfig.channel, 0,
			       &hdd_ap_ctx->sapConfig.ch_params);

	qdf_event_reset(&hostapd_state->qdf_event);
	if (wlansap_start_bss(hdd_ap_ctx->sapContext, hdd_hostapd_sap_event_cb,
			      &hdd_ap_ctx->sapConfig,
			      ap_adapter->dev)
			      != QDF_STATUS_SUCCESS)
		goto end;

	hdd_debug("Waiting for SAP to start");
	qdf_status = qdf_wait_for_event_completion(&hostapd_state->qdf_event,
					SME_CMD_TIMEOUT_VALUE);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		hdd_err("SAP Start failed");
		goto end;
	}
	wlansap_reset_sap_config_add_ie(sap_config, eUPDATE_IE_ALL);
	hdd_info("SAP Start Success");
	set_bit(SOFTAP_BSS_STARTED, &ap_adapter->event_flags);
	if (hostapd_state->bssState == BSS_START)
		cds_incr_active_session(ap_adapter->device_mode,
					ap_adapter->sessionId);
	hostapd_state->bCommit = true;
	mutex_unlock(&hdd_ctx->sap_lock);

	return;

end:
	wlansap_reset_sap_config_add_ie(sap_config, eUPDATE_IE_ALL);
	mutex_unlock(&hdd_ctx->sap_lock);
	/* SAP context and beacon cleanup will happen during driver unload
	 * in hdd_stop_adapter
	 */
	hdd_err("SAP restart after SSR failed! Reload WLAN and try SAP again");
}

/**
 * hdd_get_fw_version() - Get FW version
 * @hdd_ctx:     pointer to HDD context.
 * @major_spid:  FW version - major spid.
 * @minor_spid:  FW version - minor spid
 * @ssid:        FW version - ssid
 * @crmid:       FW version - crmid
 *
 * This function is called to get the firmware build version stored
 * as part of the HDD context
 *
 * Return:   None
 */
void hdd_get_fw_version(hdd_context_t *hdd_ctx,
			uint32_t *major_spid, uint32_t *minor_spid,
			uint32_t *siid, uint32_t *crmid)
{
	*major_spid = (hdd_ctx->target_fw_version & 0xf0000000) >> 28;
	*minor_spid = (hdd_ctx->target_fw_version & 0xf000000) >> 24;
	*siid = (hdd_ctx->target_fw_version & 0xf00000) >> 20;
	*crmid = hdd_ctx->target_fw_version & 0x7fff;
}

#ifdef QCA_CONFIG_SMP
/**
 * wlan_hdd_get_cpu() - get cpu_index
 *
 * Return: cpu_index
 */
int wlan_hdd_get_cpu(void)
{
	int cpu_index = get_cpu();

	put_cpu();
	return cpu_index;
}
#endif

/**
 * hdd_get_fwpath() - get framework path
 *
 * This function is used to get the string written by
 * userspace to start the wlan driver
 *
 * Return: string
 */
const char *hdd_get_fwpath(void)
{
	return fwpath.string;
}

/**
 * hdd_init() - Initialize Driver
 *
 * This function initilizes CDS global context with the help of cds_init. This
 * has to be the first function called after probe to get a valid global
 * context.
 *
 * Return: 0 for success, errno on failure
 */
int hdd_init(void)
{
	v_CONTEXT_t p_cds_context = NULL;
	int ret = 0;

	p_cds_context = cds_init();
	if (p_cds_context == NULL) {
		hdd_err("Failed to allocate CDS context");
		ret = -ENOMEM;
		goto err_out;
	}

	wlan_init_bug_report_lock();

#ifdef WLAN_LOGGING_SOCK_SVC_ENABLE
	wlan_logging_sock_init_svc();
#endif

	qdf_timer_init(NULL, &hdd_drv_ops_inactivity_timer,
		hdd_drv_ops_inactivity_handler, NULL,
		QDF_TIMER_TYPE_SW);

	hdd_trace_init();
	hdd_register_debug_callback();

err_out:
	return ret;
}

/**
 * hdd_deinit() - Deinitialize Driver
 *
 * This function frees CDS global context with the help of cds_deinit. This
 * has to be the last function call in remove callback to free the global
 * context.
 */
void hdd_deinit(void)
{

#ifdef WLAN_LOGGING_SOCK_SVC_ENABLE
	wlan_logging_sock_deinit_svc();
#endif

	qdf_timer_free(&hdd_drv_ops_inactivity_timer);
	wlan_destroy_bug_report_lock();
	cds_deinit();
}

#define HDD_WLAN_START_WAIT_TIME (CDS_WMA_TIMEOUT + 5000)

static int wlan_hdd_state_ctrl_param_open(struct inode *inode,
					  struct file *file)
{
	return 0;
}

static ssize_t wlan_hdd_state_ctrl_param_write(struct file *filp,
						const char __user *user_buf,
						size_t count,
						loff_t *f_pos)
{
	char buf[3];
	static const char wlan_off_str[] = "OFF";
	static const char wlan_on_str[] = "ON";
	int ret;
	unsigned long rc;

	if (copy_from_user(buf, user_buf, 3)) {
		pr_err("Failed to read buffer\n");
		return -EINVAL;
	}

	if (strncmp(buf, wlan_off_str, strlen(wlan_off_str)) == 0) {
		pr_debug("Wifi turning off from UI\n");
		goto exit;
	}

	if (strncmp(buf, wlan_on_str, strlen(wlan_on_str)) == 0) {
		pr_info("Wifi Turning On from UI\n");
	}

	if (strncmp(buf, wlan_on_str, strlen(wlan_on_str)) != 0) {
		pr_err("Invalid value received from framework");
		goto exit;
	}

	if (!cds_is_driver_loaded() || cds_is_driver_recovering()) {
		init_completion(&wlan_start_comp);
		rc = wait_for_completion_timeout(&wlan_start_comp,
				msecs_to_jiffies(HDD_WLAN_START_WAIT_TIME));
		if (!rc) {
			hdd_alert("Timed-out waiting in wlan_hdd_state_ctrl_param_write");
			ret = -EINVAL;
			return ret;
		}

		hdd_start_complete(0);
	}

exit:
	return count;
}


const struct file_operations wlan_hdd_state_fops = {
	.owner = THIS_MODULE,
	.open = wlan_hdd_state_ctrl_param_open,
	.write = wlan_hdd_state_ctrl_param_write,
};

static int  wlan_hdd_state_ctrl_param_create(void)
{
	unsigned int wlan_hdd_state_major = 0;
	int ret;
	struct device *dev;

	device = MKDEV(wlan_hdd_state_major, 0);

	ret = alloc_chrdev_region(&device, 0, dev_num, "qcwlanstate");
	if (ret) {
		pr_err("Failed to register qcwlanstate");
		goto dev_alloc_err;
	}
	wlan_hdd_state_major = MAJOR(device);

	class = class_create(THIS_MODULE, WLAN_MODULE_NAME);
	if (IS_ERR(class)) {
		pr_err("wlan_hdd_state class_create error");
		goto class_err;
	}

	dev = device_create(class, NULL, device, NULL, WLAN_MODULE_NAME);
	if (IS_ERR(dev)) {
		pr_err("wlan_hdd_statedevice_create error");
		goto err_class_destroy;
	}

	cdev_init(&wlan_hdd_state_cdev, &wlan_hdd_state_fops);
	ret = cdev_add(&wlan_hdd_state_cdev, device, dev_num);
	if (ret) {
		pr_err("Failed to add cdev error");
		goto cdev_add_err;
	}

	pr_info("wlan_hdd_state %s major(%d) initialized",
		WLAN_MODULE_NAME, wlan_hdd_state_major);

	return 0;

cdev_add_err:
	device_destroy(class, device);
err_class_destroy:
	class_destroy(class);
class_err:
	unregister_chrdev_region(device, dev_num);
dev_alloc_err:
	return -ENODEV;
}

#ifdef MODULE
static void wlan_hdd_state_ctrl_param_destroy(void)
{
	cdev_del(&wlan_hdd_state_cdev);
	device_destroy(class, device);
	class_destroy(class);
	unregister_chrdev_region(device, dev_num);

	pr_info("Device node unregistered");
}
#endif

/**
 * __hdd_module_init - Module init helper
 *
 * Module init helper function used by both module and static driver.
 *
 * Return: 0 for success, errno on failure
 */
static int __hdd_module_init(void)
{
	int ret = 0;

	pr_err("%s: Loading driver v%s (%s)\n",
	       WLAN_MODULE_NAME,
	       g_wlan_driver_version,
	       TIMER_MANAGER_STR MEMORY_DEBUG_STR PANIC_ON_BUG_STR);

	pld_init();

	ret = hdd_init();
	if (ret) {
		pr_err("hdd_init failed %x\n", ret);
		goto err_hdd_init;
	}

	qdf_wake_lock_create(&wlan_wake_lock, "wlan");

	hdd_set_conparam((uint32_t) con_mode);

	ret = wlan_hdd_register_driver();
	if (ret) {
		pr_err("%s: driver load failure, err %d\n", WLAN_MODULE_NAME,
			ret);
		goto out;
	}

	ret = wlan_hdd_state_ctrl_param_create();
	if (ret) {
		pr_err("wlan_hdd_state_create:%x\n", ret);
		goto out;
	}

	pr_info("%s: driver loaded\n", WLAN_MODULE_NAME);

	return 0;
out:
	qdf_wake_lock_destroy(&wlan_wake_lock);
	hdd_deinit();
err_hdd_init:
	pld_deinit();
	return ret;
}

/**
 * wlan_boot_cb_fake() - Fake wlan boot callback
 * @kobj:      object whose directory we're creating the link in.
 * @attr:      yes you are going to read this
 * @buff:      oh I see you reading this
 * @count:     this is fake function you know
 *
 * just return count for the sake of it.
 *
 */
static ssize_t wlan_boot_cb_fake(struct kobject *kobj,
			    struct kobj_attribute *attr,
			    const char *buf,
			    size_t count)
{
	return count;
}

#ifdef MODULE
/**
 * __hdd_module_exit - Module exit helper
 *
 * Module exit helper function used by both module and static driver.
 */
static void __hdd_module_exit(void)
{
	hdd_context_t *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);

	pr_info("%s: Unloading driver v%s\n", WLAN_MODULE_NAME,
		QWLAN_VERSIONSTR);

	if (!hdd_wait_for_recovery_completion())
		return;

	if (hdd_ctx)
		qdf_cancel_delayed_work(&hdd_ctx->iface_idle_work);

	wlan_hdd_unregister_driver();

	qdf_wake_lock_destroy(&wlan_wake_lock);

	hdd_sysfs_destroy_version_interface();
	hdd_deinit();
	pld_deinit();

	wlan_hdd_state_ctrl_param_destroy();
}

/**
 * __hdd_module_init - Module init helper
 *
 * Module init helper function used by both module and static driver.
 *
 * Return: 0 for success, errno on failure
 */
static int hdd_module_init(void)
{
	if (__hdd_module_init()) {
		pr_err("%s: Failed to register handler\n", __func__);
		return -EINVAL;
	}

	return 0;
}

/**
 * hdd_module_exit() - Exit function
 *
 * This is the driver exit point (invoked when module is unloaded using rmmod)
 *
 * Return: None
 */
static void __exit hdd_module_exit(void)
{
	__hdd_module_exit();
}
#else
static void wlan_hdd_boot_fn(struct work_struct *work)
{
	int ret;

	wlan_loader = kzalloc(sizeof(*wlan_loader), GFP_KERNEL);
	if (!wlan_loader)
		goto hdd_init;

	wlan_loader->attr_group = kzalloc(sizeof(*(wlan_loader->attr_group)),
					  GFP_KERNEL);
	if (!wlan_loader->attr_group)
		goto hdd_init;

	wlan_loader->attr_group->attrs = attrs;

	wlan_loader->boot_wlan_obj = kobject_create_and_add("boot_wlan",
							    kernel_kobj);
	if (!wlan_loader->boot_wlan_obj) {
		pr_err("%s: sysfs create and add failed\n", __func__);
		goto hdd_init;
	}

	ret = sysfs_create_group(wlan_loader->boot_wlan_obj,
				 wlan_loader->attr_group);
	if (ret) {
		pr_err("%s: sysfs create group failed %d\n", __func__, ret);
		goto hdd_init;
	}

hdd_init:
	__hdd_module_init();
}

static int __init hdd_module_init(void)
{
	INIT_WORK(&boot_work, wlan_hdd_boot_fn);
	schedule_work(&boot_work);

	return 0;
}
#endif

static int fwpath_changed_handler(const char *kmessage,
				  const struct kernel_param *kp)
{
	return param_set_copystring(kmessage, kp);
}

/**
 * is_con_mode_valid() check con mode is valid or not
 * @mode: global con mode
 *
 * Return: TRUE on success FALSE on failure
 */
static bool is_con_mode_valid(enum tQDF_GLOBAL_CON_MODE mode)
{
	switch (mode) {
	case QDF_GLOBAL_MONITOR_MODE:
	case QDF_GLOBAL_FTM_MODE:
	case QDF_GLOBAL_EPPING_MODE:
	case QDF_GLOBAL_MISSION_MODE:
		return true;
	default:
		return false;
	}
}

/**
 * hdd_get_adpter_mode() - returns adapter mode based on global con mode
 * @mode: global con mode
 *
 * Return: adapter mode
 */
static enum tQDF_ADAPTER_MODE hdd_get_adpter_mode(
					enum tQDF_GLOBAL_CON_MODE mode)
{

	switch (mode) {
	case QDF_GLOBAL_MISSION_MODE:
		return QDF_STA_MODE;
	case QDF_GLOBAL_MONITOR_MODE:
		return QDF_MONITOR_MODE;
	case QDF_GLOBAL_EPPING_MODE:
		return QDF_EPPING_MODE;
	case QDF_GLOBAL_FTM_MODE:
		return QDF_FTM_MODE;
	case QDF_GLOBAL_QVIT_MODE:
		return QDF_QVIT_MODE;
	default:
		return QDF_MAX_NO_OF_MODE;
	}
}

static void hdd_stop_present_mode(hdd_context_t *hdd_ctx,
				  enum tQDF_GLOBAL_CON_MODE curr_mode)
{

	if (hdd_ctx->driver_status == DRIVER_MODULES_CLOSED)
		return;

	switch (curr_mode) {
	case QDF_GLOBAL_MONITOR_MODE:
		hdd_info("Release wakelock for monitor mode!");
		qdf_wake_lock_release(&hdd_ctx->monitor_mode_wakelock,
				      WIFI_POWER_EVENT_WAKELOCK_MONITOR_MODE);
		/* fallthrough */
	case QDF_GLOBAL_MISSION_MODE:
	case QDF_GLOBAL_FTM_MODE:
		hdd_cleanup_actionframe_all_adapters(hdd_ctx);
		hdd_abort_mac_scan_all_adapters(hdd_ctx);
		hdd_cleanup_scan_queue(hdd_ctx, NULL);

		/* re-use the existing session */
		hdd_stop_all_adapters(hdd_ctx, false);
		break;
	default:
		break;
	}
}


static void hdd_cleanup_present_mode(hdd_context_t *hdd_ctx,
				    enum tQDF_GLOBAL_CON_MODE curr_mode)
{
	int driver_status;

	driver_status = hdd_ctx->driver_status;

	switch (curr_mode) {
	case QDF_GLOBAL_MISSION_MODE:
	case QDF_GLOBAL_MONITOR_MODE:
	case QDF_GLOBAL_FTM_MODE:
		hdd_deinit_all_adapters(hdd_ctx, false);
		hdd_close_all_adapters(hdd_ctx, false);
		break;
	case QDF_GLOBAL_EPPING_MODE:
		epping_disable();
		epping_close();
		break;
	default:
		return;
	}
}

static int hdd_register_req_mode(hdd_context_t *hdd_ctx,
				 enum tQDF_GLOBAL_CON_MODE mode)
{
	hdd_adapter_t *adapter;
	int ret = 0;
	bool rtnl_held;
	qdf_device_t qdf_dev = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	QDF_STATUS status;

	if (!qdf_dev) {
		hdd_err("qdf device context is Null return!");
		return -EINVAL;
	}

	rtnl_held = hdd_hold_rtnl_lock();
	switch (mode) {
	case QDF_GLOBAL_MISSION_MODE:
		ret = hdd_open_interfaces(hdd_ctx, rtnl_held);
		if (ret)
			hdd_err("Failed to open interfaces: %d", ret);
		break;
	case QDF_GLOBAL_FTM_MODE:
		adapter = hdd_open_adapter(hdd_ctx, QDF_FTM_MODE, "wlan%d",
					   wlan_hdd_get_intf_addr(
								hdd_ctx,
								QDF_FTM_MODE),
					   NET_NAME_UNKNOWN, rtnl_held);
		if (adapter == NULL)
			ret = -EINVAL;
		break;
	case QDF_GLOBAL_MONITOR_MODE:
		adapter = hdd_open_adapter(hdd_ctx, QDF_MONITOR_MODE, "wlan%d",
					   wlan_hdd_get_intf_addr(
							hdd_ctx,
							QDF_MONITOR_MODE),
					   NET_NAME_UNKNOWN, rtnl_held);
		if (adapter == NULL)
			ret = -EINVAL;
		break;
	case QDF_GLOBAL_EPPING_MODE:
		status = epping_open();
		if (status != QDF_STATUS_SUCCESS) {
			hdd_err("Failed to open in eeping mode: %d", status);
			ret = -EINVAL;
			break;
		}
		ret = epping_enable(qdf_dev->dev);
		if (ret) {
			hdd_err("Failed to enable in epping mode : %d", ret);
			epping_close();
		}
		break;
	default:
		hdd_err("Mode not supported");
		ret = -ENOTSUPP;
		break;
	}
	hdd_release_rtnl_lock();
	rtnl_held = false;
	return ret;
}

/**
 * __con_mode_handler() - Handles module param con_mode change
 * @kmessage: con mode name on which driver to be bring up
 * @kp: The associated kernel parameter
 * @hdd_ctx: Pointer to the global HDD context
 *
 * This function is invoked when user updates con mode using sys entry,
 * to initialize and bring-up driver in that specific mode.
 *
 * Return - 0 on success and failure code on failure
 */
static int __con_mode_handler(const char *kmessage,
			      const struct kernel_param *kp,
			      hdd_context_t *hdd_ctx)
{
	int ret;
	hdd_adapter_t *adapter;
	enum tQDF_GLOBAL_CON_MODE curr_mode;
	enum tQDF_ADAPTER_MODE adapter_mode;

	hdd_info("con_mode handler: %s", kmessage);

	qdf_atomic_set(&hdd_ctx->con_mode_flag, 1);
	mutex_lock(&hdd_init_deinit_lock);
	ret = param_set_int(kmessage, kp);

	if (!(is_con_mode_valid(con_mode))) {
		hdd_err("invlaid con_mode %d", con_mode);
		ret = -EINVAL;
		goto reset_flags;
	}

	curr_mode = hdd_get_conparam();
	if (curr_mode == con_mode) {
		hdd_err("curr mode: %d is same as user triggered mode %d",
		       curr_mode, con_mode);
		ret = 0;
		goto reset_flags;
	}

	/* ensure adapters are stopped */
	hdd_stop_present_mode(hdd_ctx, curr_mode);

	ret = hdd_wlan_stop_modules(hdd_ctx, true);
	if (ret) {
		hdd_err("Stop wlan modules failed");
		goto reset_flags;
	}

	/* Cleanup present mode before switching to new mode */
	hdd_cleanup_present_mode(hdd_ctx, curr_mode);

	hdd_set_conparam(con_mode);

	/* Register for new con_mode & then kick_start modules again */
	ret = hdd_register_req_mode(hdd_ctx, con_mode);
	if (ret) {
		hdd_err("Failed to register for new mode");
		goto reset_flags;
	}

	adapter_mode = hdd_get_adpter_mode(con_mode);
	if (adapter_mode == QDF_MAX_NO_OF_MODE) {
		hdd_err("invalid adapter");
		ret = -EINVAL;
		goto reset_flags;
	}

	adapter = hdd_get_adapter(hdd_ctx, adapter_mode);
	if (!adapter) {
		hdd_err("Failed to get adapter:%d", adapter_mode);
		goto reset_flags;
	}

	ret = hdd_wlan_start_modules(hdd_ctx, adapter, false);
	if (ret) {
		hdd_err("Start wlan modules failed: %d", ret);
		goto reset_flags;
	}

	if (con_mode == QDF_GLOBAL_MONITOR_MODE ||
	    con_mode == QDF_GLOBAL_FTM_MODE) {
		if (hdd_start_adapter(adapter)) {
			hdd_err("Failed to start %s adapter", kmessage);
			ret = -EINVAL;
			goto reset_flags;
		}
	}

	if (con_mode == QDF_GLOBAL_MONITOR_MODE) {
		hdd_info("Acquire wakelock for monitor mode!");
		qdf_wake_lock_acquire(&hdd_ctx->monitor_mode_wakelock,
				      WIFI_POWER_EVENT_WAKELOCK_MONITOR_MODE);
	}

	hdd_info("Mode successfully changed to %s", kmessage);
	ret = 0;

reset_flags:
	mutex_unlock(&hdd_init_deinit_lock);
	qdf_atomic_set(&hdd_ctx->con_mode_flag, 0);
	return ret;
}


static int con_mode_handler(const char *kmessage, const struct kernel_param *kp)
{
	int ret;
	hdd_context_t *hdd_ctx;

	hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret)
		return ret;

	cds_set_load_in_progress(true);
	if (!cds_wait_for_external_threads_completion(__func__)) {
		hdd_warn("External threads are still active, can not change mode");
		ret = -EAGAIN;
		goto err;
	}

	cds_ssr_protect(__func__);
	ret = __con_mode_handler(kmessage, kp, hdd_ctx);
	cds_ssr_unprotect(__func__);

err:
	cds_set_load_in_progress(false);
	return ret;
}

/**
 * hdd_get_conparam() - driver exit point
 *
 * This is the driver exit point (invoked when module is unloaded using rmmod)
 *
 * Return: enum tQDF_GLOBAL_CON_MODE
 */
enum tQDF_GLOBAL_CON_MODE hdd_get_conparam(void)
{
	return (enum tQDF_GLOBAL_CON_MODE) curr_con_mode;
}

void hdd_set_conparam(uint32_t con_param)
{
	curr_con_mode = con_param;
}

/**
 * hdd_clean_up_pre_cac_interface() - Clean up the pre cac interface
 * @hdd_ctx: HDD context
 *
 * Cleans up the pre cac interface, if it exists
 *
 * Return: None
 */
void hdd_clean_up_pre_cac_interface(hdd_context_t *hdd_ctx)
{
	uint8_t session_id;
	QDF_STATUS status;
	struct hdd_adapter_s *precac_adapter;

	status = wlan_sap_get_pre_cac_vdev_id(hdd_ctx->hHal, &session_id);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("failed to get pre cac vdev id");
		return;
	}

	precac_adapter = hdd_get_adapter_by_vdev(hdd_ctx, session_id);
	if (!precac_adapter) {
		hdd_err("invalid pre cac adapater");
		return;
	}

	qdf_create_work(0, &hdd_ctx->sap_pre_cac_work,
			wlan_hdd_sap_pre_cac_failure,
			(void *)precac_adapter);
	qdf_sched_work(0, &hdd_ctx->sap_pre_cac_work);

}

/**
 * hdd_update_ol_config - API to update ol configuration parameters
 * @hdd_ctx: HDD context
 *
 * Return: void
 */
static void hdd_update_ol_config(hdd_context_t *hdd_ctx)
{
	struct ol_config_info cfg;
	struct ol_context *ol_ctx = cds_get_context(QDF_MODULE_ID_BMI);

	if (!ol_ctx)
		return;

	cfg.enable_self_recovery = hdd_ctx->config->enableSelfRecovery;
	cfg.enable_uart_print = hdd_ctx->config->enablefwprint;
	cfg.enable_fw_log = hdd_ctx->config->enable_fw_log;
	cfg.enable_ramdump_collection = hdd_ctx->config->is_ramdump_enabled;
	cfg.enable_lpass_support = hdd_lpass_is_supported(hdd_ctx);

	ol_init_ini_config(ol_ctx, &cfg);
}

#ifdef FEATURE_RUNTIME_PM
/**
 * hdd_populate_runtime_cfg() - populate runtime configuration
 * @hdd_ctx: hdd context
 * @cfg: pointer to the configuration memory being populated
 *
 * Return: void
 */
static void hdd_populate_runtime_cfg(hdd_context_t *hdd_ctx,
				     struct hif_config_info *cfg)
{
	cfg->enable_runtime_pm = hdd_ctx->config->runtime_pm;
	cfg->runtime_pm_delay = hdd_ctx->config->runtime_pm_delay;
}
#else
static void hdd_populate_runtime_cfg(hdd_context_t *hdd_ctx,
				     struct hif_config_info *cfg)
{
}
#endif

/**
 * hdd_update_hif_config - API to update HIF configuration parameters
 * @hdd_ctx: HDD Context
 *
 * Return: void
 */
static void hdd_update_hif_config(hdd_context_t *hdd_ctx)
{
	struct hif_opaque_softc *scn = cds_get_context(QDF_MODULE_ID_HIF);
	struct hif_config_info cfg;

	if (!scn)
		return;

	cfg.enable_self_recovery = hdd_ctx->config->enableSelfRecovery;
	hdd_populate_runtime_cfg(hdd_ctx, &cfg);
	hif_init_ini_config(scn, &cfg);
}

/**
 * hdd_update_config() - Initialize driver per module ini parameters
 * @hdd_ctx: HDD Context
 *
 * API is used to initialize all driver per module configuration parameters
 * Return: 0 for success, errno for failure
 */
int hdd_update_config(hdd_context_t *hdd_ctx)
{
	int ret;

	hdd_update_ol_config(hdd_ctx);
	hdd_update_hif_config(hdd_ctx);
	if (QDF_GLOBAL_FTM_MODE == hdd_get_conparam())
		ret = hdd_update_cds_config_ftm(hdd_ctx);
	else
		ret = hdd_update_cds_config(hdd_ctx);

	return ret;
}

/**
 * wlan_hdd_get_dfs_mode() - get ACS DFS mode
 * @mode : cfg80211 DFS mode
 *
 * Return: return SAP ACS DFS mode else return ACS_DFS_MODE_NONE
 */
enum  sap_acs_dfs_mode wlan_hdd_get_dfs_mode(enum dfs_mode mode)
{
	switch (mode) {
	case DFS_MODE_ENABLE:
		return ACS_DFS_MODE_ENABLE;
	case DFS_MODE_DISABLE:
		return ACS_DFS_MODE_DISABLE;
	case DFS_MODE_DEPRIORITIZE:
		return ACS_DFS_MODE_DEPRIORITIZE;
	default:
		hdd_debug("ACS dfs mode is NONE");
		return ACS_DFS_MODE_NONE;
	}
}

/**
 * hdd_enable_disable_ca_event() - enable/disable channel avoidance event
 * @hddctx: pointer to hdd context
 * @set_value: enable/disable
 *
 * When Host sends vendor command enable, FW will send *ONE* CA ind to
 * Host(even though it is duplicate). When Host send vendor command
 * disable,FW doesn't perform any action. Whenever any change in
 * CA *and* WLAN is in SAP/P2P-GO mode, FW sends CA ind to host.
 *
 * return - 0 on success, appropriate error values on failure.
 */
int hdd_enable_disable_ca_event(hdd_context_t *hddctx, uint8_t set_value)
{
	QDF_STATUS status;

	if (0 != wlan_hdd_validate_context(hddctx))
		return -EAGAIN;

	if (!hddctx->config->goptimize_chan_avoid_event) {
		hdd_warn("goptimize_chan_avoid_event ini param disabled");
		return -EINVAL;
	}

	status = sme_enable_disable_chanavoidind_event(hddctx->hHal, set_value);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("Failed to send chan avoid command to SME");
		return -EINVAL;
	}
	return 0;
}

/**
 * hdd_set_roaming_in_progress() - to set the roaming in progress flag
 * @value: value to set
 *
 * This function will set the passed value to roaming in progress flag.
 *
 * Return: None
 */
void hdd_set_roaming_in_progress(bool value)
{
	hdd_context_t *hdd_ctx;

	hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	if (!hdd_ctx) {
		hdd_err("HDD context is NULL");
		return;
	}

	hdd_ctx->roaming_in_progress = value;
	hdd_debug("Roaming in Progress set to %d", value);
	if (!hdd_ctx->roaming_in_progress) {
		/* Reset scan reject params on successful roam complete */
		hdd_debug("Reset scan reject params");
		hdd_init_scan_reject_params(hdd_ctx);
	}
}

/**
 * hdd_is_roaming_in_progress() - check if roaming is in progress
 * @hdd_ctx - Global HDD context
 *
 * Checks if roaming is in progress on any of the adapters
 *
 * Return: true if roaming is in progress else false
 */
bool hdd_is_roaming_in_progress(hdd_context_t *hdd_ctx)
{
	if (!hdd_ctx) {
		hdd_err("HDD context is NULL");
		return false;
	}

	hdd_debug("roaming_in_progress = %d", hdd_ctx->roaming_in_progress);

	return hdd_ctx->roaming_in_progress;
}

hdd_adapter_t *hdd_get_adapter_by_rand_macaddr(hdd_context_t *hdd_ctx,
						tSirMacAddr mac_addr)
{
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	hdd_adapter_t *adapter;
	QDF_STATUS status;

	status = hdd_get_front_adapter(hdd_ctx, &adapter_node);
	while (adapter_node && status == QDF_STATUS_SUCCESS) {
		adapter = adapter_node->pAdapter;
		if (adapter &&
		    (adapter->device_mode == QDF_STA_MODE ||
		     adapter->device_mode == QDF_P2P_CLIENT_MODE ||
		     adapter->device_mode == QDF_P2P_DEVICE_MODE) &&
		    hdd_check_random_mac(adapter, mac_addr))
			return adapter;
		status = hdd_get_next_adapter(hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}

	return NULL;
}

int hdd_get_rssi_snr_by_bssid(hdd_adapter_t *adapter, const uint8_t *bssid,
			      int8_t *rssi, int8_t *snr)
{
	QDF_STATUS status;
	hdd_wext_state_t *wext_state = WLAN_HDD_GET_WEXT_STATE_PTR(adapter);
	tCsrRoamProfile *profile = &wext_state->roamProfile;

	status = sme_get_rssi_snr_by_bssid(WLAN_HDD_GET_HAL_CTX(adapter),
				profile, bssid, rssi, snr);
	if (QDF_STATUS_SUCCESS != status) {
		hdd_warn("sme_get_rssi_snr_by_bssid failed");
		return -EINVAL;
	}

	return 0;
}

/**
 * hdd_set_limit_off_chan_for_tos() - set limit off-channel command parameters
 * @adapter - HDD adapter
 * @tos - type of service
 * @status - status of the traffic
 *
 * Return: 0 on success and non zero value on failure
 */

int hdd_set_limit_off_chan_for_tos(hdd_adapter_t *adapter, enum tos tos,
		bool is_tos_active)
{
	int ac_bit;
	hdd_context_t *hdd_ctx;
	int ret;
	uint32_t max_off_chan_time = 0;
	QDF_STATUS status;
	tHalHandle hal = WLAN_HDD_GET_HAL_CTX(adapter);

	hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	ret = wlan_hdd_validate_context(hdd_ctx);

	if (ret < 0) {
		hdd_err("failed to set limit off chan params");
		return ret;
	}

	ac_bit = limit_off_chan_tbl[tos][HDD_AC_BIT_INDX];

	if (is_tos_active)
		adapter->active_ac |= ac_bit;
	else
		adapter->active_ac &= ~ac_bit;

	hdd_debug("session id %hu active_ac %0x",
			adapter->sessionId, adapter->active_ac);

	if (adapter->active_ac) {
		if (adapter->active_ac & HDD_AC_VO_BIT) {
			max_off_chan_time =
				limit_off_chan_tbl[TOS_VO][HDD_DWELL_TIME_INDX];
			cds_set_cur_conc_system_pref(CDS_LATENCY);
		} else if (adapter->active_ac & HDD_AC_VI_BIT) {
			max_off_chan_time =
				limit_off_chan_tbl[TOS_VI][HDD_DWELL_TIME_INDX];
			cds_set_cur_conc_system_pref(CDS_LATENCY);
		} else {
			/*ignore this command if only BE/BK is active */
			is_tos_active = false;
			cds_set_cur_conc_system_pref(
					hdd_ctx->config->conc_system_pref);
		}
	} else {
		/* No active tos */
		cds_set_cur_conc_system_pref(hdd_ctx->config->conc_system_pref);
	}

	status = sme_send_limit_off_channel_params(hal, adapter->sessionId,
			is_tos_active, max_off_chan_time,
			hdd_ctx->config->nRestTimeConc, true);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("failed to set limit off chan params");
		ret = -EINVAL;
	}

	return ret;
}

/**
 * hdd_reset_limit_off_chan() - reset limit off-channel command parameters
 * @adapter - HDD adapter
 *
 * Return: 0 on success and non zero value on failure
 */

int hdd_reset_limit_off_chan(hdd_adapter_t *adapter)
{
	hdd_context_t *hdd_ctx;
	int ret;
	QDF_STATUS status;
	tHalHandle hal = WLAN_HDD_GET_HAL_CTX(adapter);

	hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	ret = wlan_hdd_validate_context(hdd_ctx);

	if (ret < 0)
		return ret;

	cds_set_cur_conc_system_pref(hdd_ctx->config->conc_system_pref);

	/* clear the bitmap */
	adapter->active_ac = 0;

	hdd_debug("reset ac_bitmap for session %hu active_ac %0x",
			adapter->sessionId, adapter->active_ac);

	status = sme_send_limit_off_channel_params(hal, adapter->sessionId,
			false, 0, 0, false);
	if (!QDF_IS_STATUS_SUCCESS(status)) {
		hdd_err("failed to reset limit off chan params");
		ret = -EINVAL;
	}

	return ret;
}

void hdd_pld_ipa_uc_shutdown_pipes(void)
{
	hdd_context_t *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);

	if (!hdd_ctx)
		return;

	hdd_ipa_uc_force_pipe_shutdown(hdd_ctx);
}

#ifdef NTH_BEACON_OFFLOAD
/**
 * hdd_set_nth_beacon_offload() - Send the nth beacon offload command to FW
 * @adapter: HDD adapter
 * @value: Value of n, for which the nth beacon will be forwarded by the FW
 *
 * Return: QDF_STATUS_SUCCESS on success and failure status on failure
 */
QDF_STATUS hdd_set_nth_beacon_offload(hdd_adapter_t *adapter, uint16_t value)
{
	int ret;

	ret = sme_cli_set_command(adapter->sessionId,
				  WMI_VDEV_PARAM_NTH_BEACON_TO_HOST,
				  value, VDEV_CMD);
	if (ret) {
		hdd_err("WMI_VDEV_PARAM_NTH_BEACON_TO_HOST %d", ret);
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}
#else
QDF_STATUS hdd_set_nth_beacon_offload(hdd_adapter_t *adapter, uint16_t value)
{
	return QDF_STATUS_SUCCESS;
}
#endif

/**
 * hdd_start_driver_ops_timer() - Starts driver ops inactivity timer
 * @drv_op: Enum indicating driver op
 *
 * Return: none
 */
void hdd_start_driver_ops_timer(int drv_op)
{
	memset(drv_ops_string, 0, MAX_OPS_NAME_STRING_SIZE);
	switch (drv_op) {
	case eHDD_DRV_OP_PROBE:
		memcpy(drv_ops_string, "probe", sizeof("probe"));
		break;
	case eHDD_DRV_OP_REMOVE:
		memcpy(drv_ops_string, "remove", sizeof("remove"));
		break;
	case eHDD_DRV_OP_SHUTDOWN:
		memcpy(drv_ops_string, "shutdown", sizeof("shutdown"));
		break;
	case eHDD_DRV_OP_REINIT:
		memcpy(drv_ops_string, "reinit", sizeof("reinit"));
		break;
	case eHDD_DRV_OP_IFF_UP:
		memcpy(drv_ops_string, "iff_up", sizeof("iff_up"));
		break;
	}

	hdd_drv_ops_task = current;
	qdf_timer_start(&hdd_drv_ops_inactivity_timer,
		HDD_OPS_INACTIVITY_TIMEOUT);
}

/**
 * hdd_stop_driver_ops_timer() - Stops driver ops inactivity timer
 *
 * Return: none
 */
void hdd_stop_driver_ops_timer(void)
{
	qdf_timer_sync_cancel(&hdd_drv_ops_inactivity_timer);
}

/**
 * hdd_drv_ops_inactivity_handler() - Timeout handler for driver ops
 * inactivity timer
 *
 * Return: None
 */
void hdd_drv_ops_inactivity_handler(unsigned long arg)
{
	hdd_err("%s: %d Sec timer expired while in .%s",
		__func__, HDD_OPS_INACTIVITY_TIMEOUT/1000, drv_ops_string);

	if (hdd_drv_ops_task) {
		printk("Call stack for \"%s\"\n", hdd_drv_ops_task->comm);
		qdf_print_thread_trace(hdd_drv_ops_task);
	} else {
		hdd_err("hdd_drv_ops_task is null");
	}

	/* Driver shutdown is stuck, no recovery possible at this point */
	if (0 == qdf_mem_cmp(&drv_ops_string[0], "shutdown",
		sizeof("shutdown")))
		QDF_BUG(0);

	if (cds_is_fw_down()) {
		hdd_err("FW is down");
		return;
	}

	cds_trigger_recovery(false);
}

/* Register the module init/exit functions */
#ifdef MODULE
module_init(hdd_module_init);
module_exit(hdd_module_exit);
#else
device_initcall(hdd_module_init);
#endif

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Qualcomm Atheros, Inc.");
MODULE_DESCRIPTION("WLAN HOST DEVICE DRIVER");

static const struct kernel_param_ops con_mode_ops = {
	.set = con_mode_handler,
	.get = param_get_int,
};

static const struct kernel_param_ops fwpath_ops = {
	.set = fwpath_changed_handler,
	.get = param_get_string,
};

module_param_cb(con_mode, &con_mode_ops, &con_mode,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
module_param_cb(fwpath, &fwpath_ops, &fwpath,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

module_param(enable_dfs_chan_scan, int, S_IRUSR | S_IRGRP | S_IROTH);

module_param(enable_11d, int, S_IRUSR | S_IRGRP | S_IROTH);

module_param(country_code, charp, S_IRUSR | S_IRGRP | S_IROTH);
