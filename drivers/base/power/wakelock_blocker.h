// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017, andip71 <andreasp@gmx.de>
 */

#define WAKELOCK_BLOCKER_VERSION	"1.1.0"

#define LIST_WAKELOCK_DEFAULT				"qcom_rx_wakelock;wlan;NETLINK;[timerfd];wlan_pno_wl;stk_input_wakelock;hal_bluetooth_lock;IPCRTR_lpass_rx;alarmtimer;radio-interface;bluetooth_timer;poll-wake-lock;pil-a512_zap;event3;event1;event6;pil-modem;vbus-soc:usb_nop_phy;qcril_pre_client_init;a800000.ssusb;800f000.qcom,spmi:qcom,pm660@0:qpnp,fg;tftp_server_wakelock;vdev_set_key;vdev_start;wlan_wow_wl;wlan_ap_assoc_lost_wl;usb;battery;800f000.qcom,spmi:qcom,pm660@0:qcom,qpnp-smb2;event0;800f000.qcom,spmi:qcom,pm660l@3:analog-codec@f000;qcril;video3;ipc000000a4_1385_NasModemEndPoi;ipc000000a5_982_NasModemEndPoi;ipc000000b5_1932_NasModemEndPoi;IPCRTR_mpss_rx;mpss_IPCRTR;soc:gpio_keys;CHG_PLCY_HVDCP_WL;CHG_PLCY_HVDCP2_WL;CHG_PLCY_MAIN_WL;CHG_PLCY_STD_PD_WL;CHG_PLCY_PPS_WL;SensorService_wakelock;ipc000000e2_2029_NasModemEndPoi;deleted;1344;1338"

#define LENGTH_LIST_WAKELOCK				4096
#define LENGTH_LIST_WAKELOCK_DEFAULT		512
#define LENGTH_LIST_WAKELOCK_SEARCH		LENGTH_LIST_WAKELOCK + LENGTH_LIST_WAKELOCK_DEFAULT + 5
