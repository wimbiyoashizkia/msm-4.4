/*
 * Copyright (C) 2010 - 2017 Novatek, Inc.
 *
 * $Revision: 31202 $
 * $Date: 2018-07-23 10:56:09 +0800 (周一, 23 七月 2018) $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/input/mt.h>
#include <linux/wakelock.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
/* Huaqin add for TT 1242335 zhangxiude at 2018/9/30 start */
#include <linux/kthread.h>
/* Huaqin add for TT 1242335 zhangxiude at 2018/9/30 end */

#if defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#elif defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>
#endif

#include "nt36xxx.h"
#if 0
/* Huaqin add by zhangxiude for ITO test start */
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/device.h>
/* Huaqin add by zhangxiude for ITO test end */
#endif

//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 start
#if NVT_POWER_SOURCE_CUST_EN

static int nvt_lcm_bias_power_init(struct nvt_ts_data *data)
{
	int ret;
	data->lcm_lab = regulator_get(&data->client->dev, "lcm_lab");
	if (IS_ERR(data->lcm_lab)){
		ret = PTR_ERR(data->lcm_lab);
		NVT_ERR("Regulator get failed lcm_lab ret=%d", ret);
		goto _end;
	}
	if (regulator_count_voltages(data->lcm_lab)>0){
		ret = regulator_set_voltage(data->lcm_lab, LCM_LAB_MIN_UV, LCM_LAB_MAX_UV);
		if (ret){
			NVT_ERR("Regulator set_vtg failed lcm_lab ret=%d", ret);
			goto reg_lcm_lab_put;
		}
	}
	data->lcm_ibb = regulator_get(&data->client->dev, "lcm_ibb");
	if (IS_ERR(data->lcm_ibb)){
		ret = PTR_ERR(data->lcm_ibb);
		NVT_ERR("Regulator get failed lcm_ibb ret=%d", ret);
		goto reg_set_lcm_lab_vtg;
	}
	if (regulator_count_voltages(data->lcm_ibb)>0){
		ret = regulator_set_voltage(data->lcm_ibb, LCM_IBB_MIN_UV, LCM_IBB_MAX_UV);
		if (ret){
			NVT_ERR("Regulator set_vtg failed lcm_lab ret=%d", ret);
			goto reg_lcm_ibb_put;
		}
	}
	return 0;
reg_lcm_ibb_put:
	if (regulator_count_voltages(data->lcm_ibb) > 0){
		regulator_set_voltage(data->lcm_ibb, 0, LCM_IBB_MAX_UV);
	}
reg_set_lcm_lab_vtg:
	data->lcm_ibb = NULL;
	regulator_put(data->lcm_ibb);
reg_lcm_lab_put:
	if (regulator_count_voltages(data->lcm_lab) > 0){
		regulator_set_voltage(data->lcm_lab, 0, LCM_LAB_MAX_UV);
	}
_end:
	data->lcm_lab = NULL;
	regulator_put(data->lcm_lab);
	return ret;
}

static int nvt_lcm_bias_power_deinit(struct nvt_ts_data *data)
{
	if (data-> lcm_ibb != NULL){
		if (regulator_count_voltages(data->lcm_ibb) > 0){
			regulator_set_voltage(data->lcm_ibb, 0, LCM_IBB_MAX_UV);
		}
		regulator_put(data->lcm_ibb);
	}
	if (data-> lcm_lab != NULL){
		if (regulator_count_voltages(data->lcm_lab) > 0){
			regulator_set_voltage(data->lcm_lab, 0, LCM_LAB_MAX_UV);
		}
		regulator_put(data->lcm_lab);
	}
	return 0;

}


static int nvt_lcm_power_source_ctrl(struct nvt_ts_data *data, int enable)
{
	int rc;

	if (data->lcm_lab!= NULL && data->lcm_ibb!= NULL){
		if (enable){
			if (atomic_inc_return(&(data->lcm_lab_power)) == 1) {
				rc = regulator_enable(data->lcm_lab);
				if (rc) {
					atomic_dec(&(data->lcm_lab_power));
					NVT_ERR("Regulator lcm_lab enable failed rc=%d", rc);
				}
			}
			else {
				atomic_dec(&(data->lcm_lab_power));
			}
			if (atomic_inc_return(&(data->lcm_ibb_power)) == 1) {
				rc = regulator_enable(data->lcm_ibb);
				if (rc) {
					atomic_dec(&(data->lcm_ibb_power));
					NVT_ERR("Regulator lcm_ibb enable failed rc=%d", rc);
				}
			}
			else {
				atomic_dec(&(data->lcm_ibb_power));
			}
		}
		else {
			if (atomic_dec_return(&(data->lcm_lab_power)) == 0) {
				rc = regulator_disable(data->lcm_lab);
				if (rc)
				{
					atomic_inc(&(data->lcm_lab_power));
					NVT_ERR("Regulator lcm_lab disable failed rc=%d", rc);
				}
			}
			else{
				atomic_inc(&(data->lcm_lab_power));
			}
			if (atomic_dec_return(&(data->lcm_ibb_power)) == 0) {
				rc = regulator_disable(data->lcm_ibb);
				if (rc)	{
					atomic_inc(&(data->lcm_ibb_power));
					NVT_ERR("Regulator lcm_ibb disable failed rc=%d", rc);
				}
			}
			else{
				atomic_inc(&(data->lcm_ibb_power));
			}
		}
	}
	else
		NVT_ERR("Regulator lcm_ibb or lcm_lab is invalid");
	return 0;
}

#endif
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 end

#if NVT_TOUCH_EXT_PROC
extern int32_t nvt_extra_proc_init(void);
#endif

#if NVT_TOUCH_MP
extern int32_t nvt_mp_proc_init(void);
#endif

struct nvt_ts_data *ts;

static struct workqueue_struct *nvt_wq;

#if BOOT_UPDATE_FIRMWARE
static struct workqueue_struct *nvt_fwu_wq;
extern void Boot_Update_Firmware(struct work_struct *work);
#endif
// Huaqin add for ZQL1820-517. by zhangxiude. at 2018/09/12  start
extern int tp_status_fun(void);
// Huaqin add for ZQL1820-517. by zhangxiude. at 2018/09/12  end
#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
static void nvt_ts_early_suspend(struct early_suspend *h);
static void nvt_ts_late_resume(struct early_suspend *h);
#endif

#if TOUCH_KEY_NUM > 0
const uint16_t touch_key_array[TOUCH_KEY_NUM] = {
	KEY_BACK,
	KEY_HOME,
	KEY_MENU
};
#endif

#if WAKEUP_GESTURE
//Huaqin add for asus gesture by xudongfang at 2018/9/5 start
#define GESTURE_EVENT_C 		KEY_TP_GESTURE_C
#define GESTURE_EVENT_E 		KEY_TP_GESTURE_E
#define GESTURE_EVENT_M			KEY_TP_GESTURE_M
#define GESTURE_EVENT_O			KEY_TP_GESTURE_O
#define GESTURE_EVENT_S 		KEY_TP_GESTURE_S
#define GESTURE_EVENT_V 		KEY_TP_GESTURE_V
#define GESTURE_EVENT_W 		KEY_TP_GESTURE_W
#define GESTURE_EVENT_Z 		KEY_TP_GESTURE_Z
#define GESTURE_EVENT_SWIPE_UP		KEY_TP_GESTURE_SWIPE_UP
#define GESTURE_EVENT_SWIPE_DOWN	KEY_TP_GESTURE_SWIPE_DOWN
#define GESTURE_EVENT_SWIPE_LEFT	KEY_TP_GESTURE_SWIPE_LEFT
#define GESTURE_EVENT_SWIPE_RIGHT	KEY_TP_GESTURE_SWIPE_RIGHT
#define GESTURE_EVENT_DOUBLE_CLICK	KEY_WAKEUP

const uint16_t gesture_key_array[] = {
	GESTURE_EVENT_C,  //GESTURE_WORD_C
	GESTURE_EVENT_W,  //GESTURE_WORD_W
	GESTURE_EVENT_V,  //GESTURE_WORD_V
	GESTURE_EVENT_DOUBLE_CLICK,//GESTURE_DOUBLE_CLICK
	GESTURE_EVENT_Z,  //GESTURE_WORD_Z
	GESTURE_EVENT_M,  //GESTURE_WORD_M
	GESTURE_EVENT_O,  //GESTURE_WORD_O
	GESTURE_EVENT_E,  //GESTURE_WORD_E
	GESTURE_EVENT_S,  //GESTURE_WORD_S
	GESTURE_EVENT_SWIPE_UP,  //GESTURE_SLIDE_UP
	GESTURE_EVENT_SWIPE_DOWN,  //GESTURE_SLIDE_DOWN
	GESTURE_EVENT_SWIPE_LEFT,  //GESTURE_SLIDE_LEFT
	GESTURE_EVENT_SWIPE_RIGHT,  //GESTURE_SLIDE_RIGHT
};
#endif

static uint8_t bTouchIsAwake = 0;

//Huaqin add for gesture by xudongfnag at 20180913 start
#if WAKEUP_GESTURE
#define PAGESIZE 512

static int double_tap_state = 1;
static int letter_c_state = 1;
static int letter_e_state = 1;
static int letter_s_state = 1;
static int letter_v_state = 1;
static int letter_w_state = 1;
static int letter_z_state = 1;
static int letter_m_state =1;
static int letter_o_state =1;
static int up_swipe_state = 1;
static int down_swipe_state = 1;
static int left_swipe_state = 1;
static int right_swipe_state = 1;

#define GESTURE_ATTR(name)\
	static ssize_t name##_enable_read_func(struct file *file, char __user *user_buf, size_t count, loff_t *ppos) {\
		int ret = 0;\
		char page[PAGESIZE];\
		ret = sprintf(page, "%d\n", name##_state);\
		ret = simple_read_from_buffer(user_buf, count, ppos, page, strlen(page));\
		return ret;\
	}\
	static ssize_t name##_enable_write_func(struct file *file, const char __user *user_buf, size_t count, loff_t *ppos) {\
		int ret = 0;\
		char page[PAGESIZE] = {0};\
		ret = copy_from_user(page, user_buf, count);\
		ret = sscanf(page, "%d", &name##_state);\
		return count;\
	}\
	static const struct file_operations name##_enable_proc_fops = {\
		.write = name##_enable_write_func,\
		.read =  name##_enable_read_func,\
		.open = simple_open,\
		.owner = THIS_MODULE,\
	};

	GESTURE_ATTR(double_tap);
	GESTURE_ATTR(letter_c);
	GESTURE_ATTR(letter_e);
	GESTURE_ATTR(letter_s);
	GESTURE_ATTR(letter_v);
	GESTURE_ATTR(letter_w);
	GESTURE_ATTR(letter_z);
	GESTURE_ATTR(letter_m);
	GESTURE_ATTR(letter_o);
	GESTURE_ATTR(up_swipe);
	GESTURE_ATTR(down_swipe);
	GESTURE_ATTR(left_swipe);
	GESTURE_ATTR(right_swipe);

#define CREATE_PROC_NODE(PARENT, NAME, MODE)\
	node = proc_create(#NAME, MODE, PARENT, &NAME##_proc_fops);\
	if (node == NULL) {\
		ret = -ENOMEM;\
		NVT_LOG("[Nvt-ts] : Couldn't create " #NAME " in " #PARENT "\n");\
	}

#define CREATE_GESTURE_NODE(NAME)\
	CREATE_PROC_NODE(touchpanel, NAME##_enable, 0664)

int nvt_gesture_proc_init(void)
{
	int ret = 0;
	struct proc_dir_entry *touchpanel = NULL;
	struct proc_dir_entry *node  = NULL;

	touchpanel = proc_mkdir("touchpanel", NULL);
	if (touchpanel == NULL) {
		ret = -ENOMEM;
		NVT_LOG("[Nvt-ts] : Couldn't create proc/touchpanel \n");
	}

	CREATE_GESTURE_NODE(double_tap);
	CREATE_GESTURE_NODE(letter_c);
	CREATE_GESTURE_NODE(letter_e);
	CREATE_GESTURE_NODE(letter_s);
	CREATE_GESTURE_NODE(letter_v);
	CREATE_GESTURE_NODE(letter_w);
	CREATE_GESTURE_NODE(letter_z);
	CREATE_GESTURE_NODE(letter_m);
	CREATE_GESTURE_NODE(letter_o);
	CREATE_GESTURE_NODE(up_swipe);
	CREATE_GESTURE_NODE(down_swipe);
	CREATE_GESTURE_NODE(left_swipe);
	CREATE_GESTURE_NODE(right_swipe);

	return ret;
}

#define NVT_GESTURE_MODE "tpd_gesture"

static long gesture_mode = 0;
static int allow_gesture = 1;
static int screen_gesture = 0;

static struct kobject *gesture_kobject;

static ssize_t gesture_show(struct kobject *kobj, struct kobj_attribute *attr,
					  char *buf)
{
	return sprintf(buf, "%d\n", allow_gesture);
}

static ssize_t gesture_store(struct kobject *kobj, struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	sscanf(buf, "%du", &allow_gesture);
	return count;
}

static struct kobj_attribute gesture_attribute = __ATTR(dclicknode, 0664, gesture_show, gesture_store);

static ssize_t screengesture_show(struct kobject *kobj, struct kobj_attribute *attr,
					  char *buf)
{
	return sprintf(buf, "%d\n", screen_gesture);
}

static ssize_t screengesture_store(struct kobject *kobj, struct kobj_attribute *attr,
					  char *buf, size_t count)
{
	sscanf(buf, "%du", &screen_gesture);
	return count;
}

static struct kobj_attribute screengesture_attribute = __ATTR(gesture_node, 0664, screengesture_show,
													    screengesture_store);

int create_gesture_node(void)
{
	int error = 0, error2 = 0;

	NVT_LOG("[Nvt-ts] : Gesture Node initialized successfully \n");

	gesture_kobject = kobject_create_and_add("touchpanel", kernel_kobj);
	if (!gesture_kobject)
		return -ENOMEM;

	error = sysfs_create_file(gesture_kobject, &gesture_attribute.attr);
	if (error) {
		NVT_LOG("[Nvt-ts] : failed to create the gesture_node file in /sys/kernel/touchpanel \n");
	}

	 error2 = sysfs_create_file(gesture_kobject, &screengesture_attribute.attr);
	 if (error) {
	 	NVT_LOG("[Nvt-ts] : failed to create the gesture_node file in /sys/kernel/touchpanel \n");
	 }

	return error;
}

void destroy_gesture(void)
{
	kobject_put(gesture_kobject);
}

static ssize_t nvt_gesture_mode_get_proc(struct file *file,
                        char __user *buffer, size_t size, loff_t *ppos)
{
	char ptr[64] = {0};
	unsigned int len = 0;
	unsigned int ret = 0;

	if (gesture_mode == 0) {
		len = sprintf(ptr, "0\n");
	} else {
		len = sprintf(ptr, "1\n");
	}
	ret = simple_read_from_buffer(buffer, size, ppos, ptr, (size_t)len);
	return ret;
}

static ssize_t nvt_gesture_mode_set_proc(struct file *filp,
                        const char __user *buffer, size_t count, loff_t *off)
{
	char msg[20] = {0};
	int ret = 0;
	if (!bTouchIsAwake) {
		NVT_LOG("Touch is already sleep cant modify gesture node\n");
		return count;
	}
	ret = copy_from_user(msg, buffer, count);
	NVT_LOG("msg = %s\n", msg);
	if (ret) {
		return -EFAULT;
	}

	ret = kstrtol(msg, 0, &gesture_mode);
	if (!ret) {
		if (gesture_mode == 0) {
			gesture_mode = 0;
		} else {
			screen_gesture = 1;
			allow_gesture = 1;
			gesture_mode = 0x1FF;
		}
	}
	else {
		NVT_ERR("set gesture mode failed\n");
	}
	NVT_LOG("gesture_mode = 0x%x\n", (unsigned int)gesture_mode);

	return count;
}

static struct proc_dir_entry *nvt_gesture_mode_proc = NULL;
static const struct file_operations gesture_mode_proc_ops = {
	.owner = THIS_MODULE,
	.read = nvt_gesture_mode_get_proc,
	.write = nvt_gesture_mode_set_proc,
};
#endif
//Huaqin add for gesture by xudongfnag at 20180913 end

/*******************************************************
Description:
	Novatek touchscreen i2c read function.

return:
	Executive outcomes. 2---succeed. -5---I/O error
*******************************************************/
int32_t CTP_I2C_READ(struct i2c_client *client, uint16_t address, uint8_t *buf, uint16_t len)
{
	struct i2c_msg msgs[2];
	int32_t ret = -1;
	int32_t retries = 0;

	msgs[0].flags = !I2C_M_RD;
	msgs[0].addr  = address;
	msgs[0].len   = 1;
	msgs[0].buf   = &buf[0];

	msgs[1].flags = I2C_M_RD;
	msgs[1].addr  = address;
	msgs[1].len   = len - 1;
	msgs[1].buf   = &buf[1];

	while (retries < 5) {
		ret = i2c_transfer(client->adapter, msgs, 2);
		if (ret == 2)	break;
		retries++;
	}

	if (unlikely(retries == 5)) {
		NVT_ERR("error, ret=%d\n", ret);
		ret = -EIO;
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen i2c write function.

return:
	Executive outcomes. 1---succeed. -5---I/O error
*******************************************************/
int32_t CTP_I2C_WRITE(struct i2c_client *client, uint16_t address, uint8_t *buf, uint16_t len)
{
	struct i2c_msg msg;
	int32_t ret = -1;
	int32_t retries = 0;

	msg.flags = !I2C_M_RD;
	msg.addr  = address;
	msg.len   = len;
	msg.buf   = buf;

	while (retries < 5) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)	break;
		retries++;
	}

	if (unlikely(retries == 5)) {
		NVT_ERR("error, ret=%d\n", ret);
		ret = -EIO;
	}

	return ret;
}


/*******************************************************
Description:
	Novatek touchscreen reset MCU then into idle mode
    function.

return:
	n.a.
*******************************************************/
void nvt_sw_reset_idle(void)
{
	uint8_t buf[4]={0};

	//---write i2c cmds to reset idle---
	buf[0]=0x00;
	buf[1]=0xA5;
	CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);

	msleep(15);
}

/*******************************************************
Description:
	Novatek touchscreen reset MCU (boot) function.

return:
	n.a.
*******************************************************/
void nvt_bootloader_reset(void)
{
	uint8_t buf[8] = {0};

	//---write i2c cmds to reset---
	buf[0] = 0x00;
	buf[1] = 0x69;
	CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);

	// need 35ms delay after bootloader reset
	msleep(35);
}

/*******************************************************
Description:
	Novatek touchscreen clear FW status function.

return:
	Executive outcomes. 0---succeed. -1---fail.
*******************************************************/
int32_t nvt_clear_fw_status(void)
{
	uint8_t buf[8] = {0};
	int32_t i = 0;
	const int32_t retry = 20;

	for (i = 0; i < retry; i++) {
		//---set xdata index to EVENT BUF ADDR---
		buf[0] = 0xFF;
		buf[1] = (ts->mmap->EVENT_BUF_ADDR >> 16) & 0xFF;
		buf[2] = (ts->mmap->EVENT_BUF_ADDR >> 8) & 0xFF;
		CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 3);

		//---clear fw status---
		buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
		buf[1] = 0x00;
		CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);

		//---read fw status---
		buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
		buf[1] = 0xFF;
		CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 2);

		if (buf[1] == 0x00)
			break;

		msleep(10);
	}

	if (i >= retry) {
		NVT_ERR("failed, i=%d, buf[1]=0x%02X\n", i, buf[1]);
		return -1;
	} else {
		return 0;
	}
}

/*******************************************************
Description:
	Novatek touchscreen check FW status function.

return:
	Executive outcomes. 0---succeed. -1---failed.
*******************************************************/
int32_t nvt_check_fw_status(void)
{
	uint8_t buf[8] = {0};
	int32_t i = 0;
	const int32_t retry = 50;

	for (i = 0; i < retry; i++) {
		//---set xdata index to EVENT BUF ADDR---
		buf[0] = 0xFF;
		buf[1] = (ts->mmap->EVENT_BUF_ADDR >> 16) & 0xFF;
		buf[2] = (ts->mmap->EVENT_BUF_ADDR >> 8) & 0xFF;
		CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 3);

		//---read fw status---
		buf[0] = EVENT_MAP_HANDSHAKING_or_SUB_CMD_BYTE;
		buf[1] = 0x00;
		CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 2);

		if ((buf[1] & 0xF0) == 0xA0)
			break;

		msleep(10);
	}

	if (i >= retry) {
		NVT_ERR("failed, i=%d, buf[1]=0x%02X\n", i, buf[1]);
		return -1;
	} else {
		return 0;
	}
}

/*******************************************************
Description:
	Novatek touchscreen check FW reset state function.

return:
	Executive outcomes. 0---succeed. -1---failed.
*******************************************************/
int32_t nvt_check_fw_reset_state(RST_COMPLETE_STATE check_reset_state)
{
	uint8_t buf[8] = {0};
	int32_t ret = 0;
	int32_t retry = 0;

	while (1) {
		msleep(10);

		//---read reset state---
		buf[0] = EVENT_MAP_RESET_COMPLETE;
		buf[1] = 0x00;
		CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 6);

		if ((buf[1] >= check_reset_state) && (buf[1] <= RESET_STATE_MAX)) {
			ret = 0;
			break;
		}

		retry++;
		if(unlikely(retry > 100)) {
			NVT_ERR("error, retry=%d, buf[1]=0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X\n", retry, buf[1], buf[2], buf[3], buf[4], buf[5]);
			ret = -1;
			break;
		}
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen get novatek project id information
	function.

return:
	Executive outcomes. 0---success. -1---fail.
*******************************************************/
int32_t nvt_read_pid(void)
{
	uint8_t buf[3] = {0};
	int32_t ret = 0;

	//---set xdata index to EVENT BUF ADDR---
	buf[0] = 0xFF;
	buf[1] = (ts->mmap->EVENT_BUF_ADDR >> 16) & 0xFF;
	buf[2] = (ts->mmap->EVENT_BUF_ADDR >> 8) & 0xFF;
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 3);

	//---read project id---
	buf[0] = EVENT_MAP_PROJECTID;
	buf[1] = 0x00;
	buf[2] = 0x00;
	CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 3);

	ts->nvt_pid = (buf[2] << 8) + buf[1];

	NVT_LOG("PID=%04X\n", ts->nvt_pid);

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen get firmware related information
	function.

return:
	Executive outcomes. 0---success. -1---fail.
*******************************************************/
int32_t nvt_get_fw_info(void)
{
	uint8_t buf[64] = {0};
	uint32_t retry_count = 0;
	int32_t ret = 0;

info_retry:
	//---set xdata index to EVENT BUF ADDR---
	buf[0] = 0xFF;
	buf[1] = (ts->mmap->EVENT_BUF_ADDR >> 16) & 0xFF;
	buf[2] = (ts->mmap->EVENT_BUF_ADDR >> 8) & 0xFF;
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 3);

	//---read fw info---
	buf[0] = EVENT_MAP_FWINFO;
	CTP_I2C_READ(ts->client, I2C_FW_Address, buf, 17);
	ts->fw_ver = buf[1];
	ts->x_num = buf[3];
	ts->y_num = buf[4];
	ts->abs_x_max = (uint16_t)((buf[5] << 8) | buf[6]);
	ts->abs_y_max = (uint16_t)((buf[7] << 8) | buf[8]);
	ts->max_button_num = buf[11];

	//---clear x_num, y_num if fw info is broken---
	if ((buf[1] + buf[2]) != 0xFF) {
		NVT_ERR("FW info is broken! fw_ver=0x%02X, ~fw_ver=0x%02X\n", buf[1], buf[2]);
		ts->fw_ver = 0;
		ts->x_num = 18;
		ts->y_num = 32;
		ts->abs_x_max = TOUCH_DEFAULT_MAX_WIDTH;
		ts->abs_y_max = TOUCH_DEFAULT_MAX_HEIGHT;
		ts->max_button_num = TOUCH_KEY_NUM;

		if(retry_count < 3) {
			retry_count++;
			NVT_ERR("retry_count=%d\n", retry_count);
			goto info_retry;
		} else {
			NVT_ERR("Set default fw_ver=%d, x_num=%d, y_num=%d, "
					"abs_x_max=%d, abs_y_max=%d, max_button_num=%d!\n",
					ts->fw_ver, ts->x_num, ts->y_num,
					ts->abs_x_max, ts->abs_y_max, ts->max_button_num);
			ret = -1;
		}
	} else {
		ret = 0;
	}

	//---Get Novatek PID---
	nvt_read_pid();

	return ret;
}

/*******************************************************
  Create Device Node (Proc Entry)
*******************************************************/
#if NVT_TOUCH_PROC
static struct proc_dir_entry *NVT_proc_entry;
#define DEVICE_NAME	"NVTflash"

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTflash read function.

return:
	Executive outcomes. 2---succeed. -5,-14---failed.
*******************************************************/
static ssize_t nvt_flash_read(struct file *file, char __user *buff, size_t count, loff_t *offp)
{
	uint8_t str[68] = {0};
	int32_t ret = -1;
	int32_t retries = 0;
	int8_t i2c_wr = 0;

	if (count > sizeof(str)) {
		NVT_ERR("error count=%zu\n", count);
		return -EFAULT;
	}

	if (copy_from_user(str, buff, count)) {
		NVT_ERR("copy from user error\n");
		return -EFAULT;
	}

	i2c_wr = str[0] >> 7;

	if (i2c_wr == 0) {	//I2C write
		while (retries < 20) {
			ret = CTP_I2C_WRITE(ts->client, (str[0] & 0x7F), &str[2], str[1]);
			if (ret == 1)
				break;
			else
				NVT_ERR("error, retries=%d, ret=%d\n", retries, ret);

			retries++;
		}

		if (unlikely(retries == 20)) {
			NVT_ERR("error, ret = %d\n", ret);
			return -EIO;
		}

		return ret;
	} else if (i2c_wr == 1) {	//I2C read
		while (retries < 20) {
			ret = CTP_I2C_READ(ts->client, (str[0] & 0x7F), &str[2], str[1]);
			if (ret == 2)
				break;
			else
				NVT_ERR("error, retries=%d, ret=%d\n", retries, ret);

			retries++;
		}

		// copy buff to user if i2c transfer
		if (retries < 20) {
			if (copy_to_user(buff, str, count))
				return -EFAULT;
		}

		if (unlikely(retries == 20)) {
			NVT_ERR("error, ret = %d\n", ret);
			return -EIO;
		}

		return ret;
	} else {
		NVT_ERR("Call error, str[0]=%d\n", str[0]);
		return -EFAULT;
	}
}

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTflash open function.

return:
	Executive outcomes. 0---succeed. -12---failed.
*******************************************************/
static int32_t nvt_flash_open(struct inode *inode, struct file *file)
{
	struct nvt_flash_data *dev;

	dev = kmalloc(sizeof(struct nvt_flash_data), GFP_KERNEL);
	if (dev == NULL) {
		NVT_ERR("Failed to allocate memory for nvt flash data\n");
		return -ENOMEM;
	}

	rwlock_init(&dev->lock);
	file->private_data = dev;

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTflash close function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_flash_close(struct inode *inode, struct file *file)
{
	struct nvt_flash_data *dev = file->private_data;

	if (dev)
		kfree(dev);

	return 0;
}

static const struct file_operations nvt_flash_fops = {
	.owner = THIS_MODULE,
	.open = nvt_flash_open,
	.release = nvt_flash_close,
	.read = nvt_flash_read,
};

/*******************************************************
Description:
	Novatek touchscreen /proc/NVTflash initial function.

return:
	Executive outcomes. 0---succeed. -12---failed.
*******************************************************/
static int32_t nvt_flash_proc_init(void)
{
	NVT_proc_entry = proc_create(DEVICE_NAME, 0444, NULL,&nvt_flash_fops);
	if (NVT_proc_entry == NULL) {
		NVT_ERR("Failed!\n");
		return -ENOMEM;
	} else {
		NVT_LOG("Succeeded!\n");
	}

	NVT_LOG("============================================================\n");
	NVT_LOG("Create /proc/NVTflash\n");
	NVT_LOG("============================================================\n");

	return 0;
}
#endif

#define NVT_INFO_PROC_FILE "nvt_info"
static struct proc_dir_entry *nvt_info_proc_entry;
static int32_t firmware_id = 0;

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}


static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}


static void c_stop(struct seq_file *m, void *v)
{
	return;
}

static int32_t c_info_show(struct seq_file *m, void *v)
{
	seq_printf(m, "IC=Novatek NT36672AH module=GX fw_ver= %d\n",ts->fw_ver);
	return 0;
}
const struct seq_operations nvt_info_seq_ops = {
	.start  = c_start,
	.next   = c_next,
	.stop   = c_stop,
	.show   = c_info_show
};
/*******************************************************
Description:
	nvt touchscreen /proc/nvt_fw_version open
	function.

return:
	n.a.
*******************************************************/
static int32_t nvt_info_open(struct inode *inode, struct file *file)
{
/* Huaqin add by limengxia for nvt_info test start */
	if (mutex_lock_interruptible(&ts->lock)) {
		return -ERESTARTSYS;
	}

	NVT_LOG("++\n");

	if (nvt_get_fw_info()) {
		mutex_unlock(&ts->lock);
		return -EAGAIN;
	}

	mutex_unlock(&ts->lock);

	NVT_LOG("--\n");
/* Huaqin add by limengxia for nvt_info test end */
	return seq_open(file, &nvt_info_seq_ops);
}

static const struct file_operations nvt_info_proc_fops = {
	.owner = THIS_MODULE,
	.open = nvt_info_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};
/*******************************************************
Description:
	nvt touchscreen info function proc. file node
	initial function.

return:
	Executive outcomes. 0---succeed. -12---failed.
*******************************************************/
int32_t nvt_tp_info_proc_init(void)
{
	nvt_info_proc_entry = proc_create(NVT_INFO_PROC_FILE, 0777, NULL, &nvt_info_proc_fops);
	if (NULL == nvt_info_proc_entry)
	{
           printk( "[nvt] %s() Couldn't create proc entry!",  __func__);
           return -ENOMEM;
	}
	else
	{
           printk( "[nvt] %s() Create proc entry success!",  __func__);
	}
	return 0;
}

#if 0
/* Huaqin add by zhangxiude for ITO test start */
/**********add ito test mode function  *******************/
int nvt_TestResultLen=0;
static struct platform_device hwinfo_device= {
	.name = HWINFO_NAME,
	.id = -1,
};

static ssize_t ito_test_show(struct device *dev,struct device_attribute *attr,char *buf)
{
	int count;
	nvt_TestResultLen = 0;
	ito_selftest_open();
	count = sprintf(buf, "%d\n", nvt_TestResultLen);
	return count;
}

static ssize_t ito_test_store(struct device *dev,struct device_attribute *attr,const char *buf, size_t count)
{
	return 0;
}

static DEVICE_ATTR(factory_check, 0644, ito_test_show, ito_test_store);

static struct attribute *ito_test_attributes[] ={

	&dev_attr_factory_check.attr,
	NULL
};
static struct attribute_group ito_test_attribute_group = {

.attrs = ito_test_attributes

};
int nvt_test_node_init(struct platform_device *tpinfo_device)
{
	int err=0;
    err = sysfs_create_group(&tpinfo_device->dev.kobj, &ito_test_attribute_group);
    if (0 != err)
    {
        printk( "[nvt-ito] %s() - ERROR: sysfs_create_group() failed.",  __func__);
        sysfs_remove_group(&tpinfo_device->dev.kobj, &ito_test_attribute_group);
        return -EIO;
    }
    else
    {
        printk("[nvt-ito] %s() - sysfs_create_group() succeeded.", __func__);
    }
    return err;
}
/* Huaqin add by zhangxiude for ITO test end */
#endif

#if WAKEUP_GESTURE
#define GESTURE_WORD_C          12
#define GESTURE_WORD_W          13
#define GESTURE_WORD_V          14
#define GESTURE_DOUBLE_CLICK    15
#define GESTURE_WORD_Z          16
#define GESTURE_WORD_M          17
#define GESTURE_WORD_O          18
#define GESTURE_WORD_e          19
#define GESTURE_WORD_S          20
#define GESTURE_SLIDE_UP        21
#define GESTURE_SLIDE_DOWN      22
#define GESTURE_SLIDE_LEFT      23
#define GESTURE_SLIDE_RIGHT     24
/* customized gesture id */
#define DATA_PROTOCOL           30

/* function page definition */
#define FUNCPAGE_GESTURE         1

static struct wake_lock gestrue_wakelock;

/*******************************************************
Description:
	Novatek touchscreen wake up gesture key report function.

return:
	n.a.
*******************************************************/
void nvt_ts_wakeup_gesture_report(uint8_t gesture_id, uint8_t *data)
{
	uint32_t keycode = 0;
	uint8_t func_type = data[2];
	uint8_t func_id = data[3];
	int is_double_tap = 0;

	/* support fw specifal data protocol */
	if ((gesture_id == DATA_PROTOCOL) && (func_type == FUNCPAGE_GESTURE)) {
		gesture_id = func_id;
	} else if (gesture_id > DATA_PROTOCOL) {
		NVT_ERR("gesture_id %d is invalid, func_type=%d, func_id=%d\n", gesture_id, func_type, func_id);
		return;
	}

	switch (gesture_id) {
		case GESTURE_WORD_C:
			if (screen_gesture || letter_c_state) {
				NVT_LOG("Gesture : Word-C.\n");
				keycode = gesture_key_array[0];
			}
			break;
		case GESTURE_WORD_W:
			if (screen_gesture || letter_w_state) {
				NVT_LOG("Gesture : Word-W.\n");
				keycode = gesture_key_array[1];
			}
			break;
		case GESTURE_WORD_V:
			if (screen_gesture || letter_v_state) {
				NVT_LOG("Gesture : Word-V.\n");
				keycode = gesture_key_array[2];
			}
			break;
		case GESTURE_DOUBLE_CLICK:
			if (allow_gesture || double_tap_state) {
				is_double_tap = 1;
				NVT_LOG("Gesture : Double Click.\n");
				keycode = gesture_key_array[3];
			}
			break;
		case GESTURE_WORD_Z:
			if (screen_gesture || letter_z_state) {
				NVT_LOG("Gesture : Word-Z.\n");
				keycode = gesture_key_array[4];
			}
			break;
		case GESTURE_WORD_M:
			if (screen_gesture || letter_m_state) {
				NVT_LOG("Gesture : Word-M.\n");
				keycode = gesture_key_array[5];
			}
			break;
		case GESTURE_WORD_O:
			if (screen_gesture || letter_o_state) {
				NVT_LOG("Gesture : Word-O.\n");
				keycode = gesture_key_array[6];
			}
			break;
		case GESTURE_WORD_e:
			if (screen_gesture || letter_e_state) {
				NVT_LOG("Gesture : Word-e.\n");
				keycode = gesture_key_array[7];
			}
			break;
		case GESTURE_WORD_S:
			if (screen_gesture || letter_s_state) {
				NVT_LOG("Gesture : Word-S.\n");
				keycode = gesture_key_array[8];
			}
			break;
		case GESTURE_SLIDE_UP:
			if (screen_gesture || up_swipe_state) {
				NVT_LOG("Gesture : Slide UP.\n");
				keycode = gesture_key_array[9];
			}
			break;
		case GESTURE_SLIDE_DOWN:
			if (screen_gesture || down_swipe_state) {
				NVT_LOG("Gesture : Slide DOWN.\n");
				keycode = gesture_key_array[10];
			}
			break;
		case GESTURE_SLIDE_LEFT:
			if (screen_gesture || left_swipe_state) {
				NVT_LOG("Gesture : Slide LEFT.\n");
				keycode = gesture_key_array[11];
			}
			break;
		case GESTURE_SLIDE_RIGHT:
			if (screen_gesture || right_swipe_state) {
				NVT_LOG("Gesture : Slide RIGHT.\n");
				keycode = gesture_key_array[12];
			}
			break;
		default:
			NVT_LOG("Still in gesture mode.\n");
			break;
	}

	
	if (keycode > 0 ) {
		if (is_double_tap == 1) {
			input_report_key(ts->input_dev, GESTURE_EVENT_DOUBLE_CLICK, 1);
			input_sync(ts->input_dev);
			input_report_key(ts->input_dev, GESTURE_EVENT_DOUBLE_CLICK, 0);
			input_sync(ts->input_dev);
			is_double_tap = 0;
		} else {
			NVT_LOG("[NVT-ts] : gesture key code = %d\n", keycode);
			input_report_key(ts->input_dev, keycode, 1);
			input_sync(ts->input_dev);
			input_report_key(ts->input_dev, keycode, 0);
			input_sync(ts->input_dev);
		}
	}
}
#endif

/*******************************************************
Description:
	Novatek touchscreen parse device tree function.

return:
	n.a.
*******************************************************/
#ifdef CONFIG_OF
static void nvt_parse_dt(struct device *dev)
{
	struct device_node *np = dev->of_node;

#if NVT_TOUCH_SUPPORT_HW_RST
	ts->reset_gpio = of_get_named_gpio_flags(np, "novatek,reset-gpio", 0, &ts->reset_flags);
	NVT_LOG("novatek,reset-gpio=%d\n", ts->reset_gpio);
#endif
	ts->irq_gpio = of_get_named_gpio_flags(np, "novatek,irq-gpio", 0, &ts->irq_flags);
	NVT_LOG("novatek,irq-gpio=%d\n", ts->irq_gpio);

}
#else
static void nvt_parse_dt(struct device *dev)
{
#if NVT_TOUCH_SUPPORT_HW_RST
	ts->reset_gpio = NVTTOUCH_RST_PIN;
#endif
	ts->irq_gpio = NVTTOUCH_INT_PIN;
}
#endif

/*******************************************************
Description:
	Novatek touchscreen config and request gpio

return:
	Executive outcomes. 0---succeed. not 0---failed.
*******************************************************/
static int nvt_gpio_config(struct nvt_ts_data *ts)
{
	int32_t ret = 0;

#if NVT_TOUCH_SUPPORT_HW_RST
	/* request RST-pin (Output/High) */
	if (gpio_is_valid(ts->reset_gpio)) {
		ret = gpio_request_one(ts->reset_gpio, GPIOF_OUT_INIT_HIGH, "NVT-tp-rst");
		if (ret) {
			NVT_ERR("Failed to request NVT-tp-rst GPIO\n");
			goto err_request_reset_gpio;
		}
	}
#endif

	/* request INT-pin (Input) */
	if (gpio_is_valid(ts->irq_gpio)) {
		ret = gpio_request_one(ts->irq_gpio, GPIOF_IN, "NVT-int");
		if (ret) {
			NVT_ERR("Failed to request NVT-int GPIO\n");
			goto err_request_irq_gpio;
		}
	}

	return ret;

err_request_irq_gpio:
#if NVT_TOUCH_SUPPORT_HW_RST
	gpio_free(ts->reset_gpio);
err_request_reset_gpio:
#endif
	return ret;
}

#define POINT_DATA_LEN 65
/*******************************************************
Description:
	Novatek touchscreen work function.

return:
	n.a.
*******************************************************/
static void nvt_ts_work_func(struct work_struct *work)
{
	int32_t ret = -1;
	uint8_t point_data[POINT_DATA_LEN + 1] = {0};
	uint32_t position = 0;
	uint32_t input_x = 0;
	uint32_t input_y = 0;
	uint32_t input_w = 0;
	uint32_t input_p = 0;
	uint8_t input_id = 0;
#if MT_PROTOCOL_B
	uint8_t press_id[TOUCH_MAX_FINGER_NUM] = {0};
#endif /* MT_PROTOCOL_B */
	int32_t i = 0;
	int32_t finger_cnt = 0;

	mutex_lock(&ts->lock);

	ret = CTP_I2C_READ(ts->client, I2C_FW_Address, point_data, POINT_DATA_LEN + 1);
	if (ret < 0) {
		NVT_ERR("CTP_I2C_READ failed.(%d)\n", ret);
		goto XFER_ERROR;
	}
/*
	//--- dump I2C buf ---
	for (i = 0; i < 10; i++) {
		printk("%02X %02X %02X %02X %02X %02X  ", point_data[1+i*6], point_data[2+i*6], point_data[3+i*6], point_data[4+i*6], point_data[5+i*6], point_data[6+i*6]);
	}
	printk("\n");
*/

// Huaqin add for ZQL1820-517. by zhangxiude. at 2018/09/12  start
	ret = tp_status_fun();
	if (ret) {
		goto XFER_ERROR;
	}
// Huaqin add for ZQL1820-517. by zhengwu.lu. at 2018/09/12  end
#if WAKEUP_GESTURE
	if (bTouchIsAwake == 0) {
		input_id = (uint8_t)(point_data[1] >> 3);
		nvt_ts_wakeup_gesture_report(input_id, point_data);
		enable_irq(ts->client->irq);
		mutex_unlock(&ts->lock);
		return;
	}
#endif

	finger_cnt = 0;

	for (i = 0; i < ts->max_touch_num; i++) {
		position = 1 + 6 * i;
		input_id = (uint8_t)(point_data[position + 0] >> 3);
		if ((input_id == 0) || (input_id > ts->max_touch_num))
			continue;

		if (((point_data[position] & 0x07) == 0x01) || ((point_data[position] & 0x07) == 0x02)) {	//finger down (enter & moving)
			input_x = (uint32_t)(point_data[position + 1] << 4) + (uint32_t) (point_data[position + 3] >> 4);
			input_y = (uint32_t)(point_data[position + 2] << 4) + (uint32_t) (point_data[position + 3] & 0x0F);
			if ((input_x < 0) || (input_y < 0))
				continue;
			if ((input_x > ts->abs_x_max) || (input_y > ts->abs_y_max))
				continue;
			input_w = (uint32_t)(point_data[position + 4]);
			if (input_w == 0)
				input_w = 1;
			if (i < 2) {
				input_p = (uint32_t)(point_data[position + 5]) + (uint32_t)(point_data[i + 63] << 8);
				if (input_p > TOUCH_FORCE_NUM)
					input_p = TOUCH_FORCE_NUM;
			} else {
				input_p = (uint32_t)(point_data[position + 5]);
			}
			if (input_p == 0)
				input_p = 1;

#if MT_PROTOCOL_B
			press_id[input_id - 1] = 1;
			input_mt_slot(ts->input_dev, input_id - 1);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
#else /* MT_PROTOCOL_B */
			input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, input_id - 1);
			input_report_key(ts->input_dev, BTN_TOUCH, 1);
#endif /* MT_PROTOCOL_B */

			input_report_abs(ts->input_dev, ABS_MT_POSITION_X, input_x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, input_y);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, input_w);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, input_p);

#if MT_PROTOCOL_B
#else /* MT_PROTOCOL_B */
			input_mt_sync(ts->input_dev);
#endif /* MT_PROTOCOL_B */

			finger_cnt++;
		}
	}

#if MT_PROTOCOL_B
	for (i = 0; i < ts->max_touch_num; i++) {
		if (press_id[i] != 1) {
			input_mt_slot(ts->input_dev, i);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
		}
	}

	input_report_key(ts->input_dev, BTN_TOUCH, (finger_cnt > 0));
#else /* MT_PROTOCOL_B */
	if (finger_cnt == 0) {
		input_report_key(ts->input_dev, BTN_TOUCH, 0);
		input_mt_sync(ts->input_dev);
	}
#endif /* MT_PROTOCOL_B */

#if TOUCH_KEY_NUM > 0
	if (point_data[61] == 0xF8) {
		for (i = 0; i < ts->max_button_num; i++) {
			input_report_key(ts->input_dev, touch_key_array[i], ((point_data[62] >> i) & 0x01));
		}
	} else {
		for (i = 0; i < ts->max_button_num; i++) {
			input_report_key(ts->input_dev, touch_key_array[i], 0);
		}
	}
#endif

	input_sync(ts->input_dev);

XFER_ERROR:
	enable_irq(ts->client->irq);

	mutex_unlock(&ts->lock);
}

/*******************************************************
Description:
	External interrupt service routine.

return:
	irq execute status.
*******************************************************/
static irqreturn_t nvt_ts_irq_handler(int32_t irq, void *dev_id)
{
	disable_irq_nosync(ts->client->irq);

#if WAKEUP_GESTURE
	if (bTouchIsAwake == 0) {
		wake_lock_timeout(&gestrue_wakelock, msecs_to_jiffies(5000));
	}
#endif

	queue_work(nvt_wq, &ts->nvt_work);

	return IRQ_HANDLED;
}

/*******************************************************
Description:
	Novatek touchscreen check and stop crc reboot loop.

return:
	n.a.
*******************************************************/
void nvt_stop_crc_reboot(void)
{
	uint8_t buf[8] = {0};
	int32_t retry = 0;

	//read dummy buffer to check CRC fail reboot is happening or not

	//---change I2C index to prevent geting 0xFF, but not 0xFC---
	buf[0] = 0xFF;
	buf[1] = 0x01;
	buf[2] = 0xF6;
	CTP_I2C_WRITE(ts->client, I2C_BLDR_Address, buf, 3);

	//---read to check if buf is 0xFC which means IC is in CRC reboot ---
	buf[0] = 0x4E;
	CTP_I2C_READ(ts->client, I2C_BLDR_Address, buf, 4);

	if ((buf[1] == 0xFC) ||
		((buf[1] == 0xFF) && (buf[2] == 0xFF) && (buf[3] == 0xFF))) {

		//IC is in CRC fail reboot loop, needs to be stopped!
		for (retry = 5; retry > 0; retry--) {

			//---write i2c cmds to reset idle : 1st---
			buf[0]=0x00;
			buf[1]=0xA5;
			CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);

			//---write i2c cmds to reset idle : 2rd---
			buf[0]=0x00;
			buf[1]=0xA5;
			CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);
			msleep(1);

			//---clear CRC_ERR_FLAG---
			buf[0] = 0xFF;
			buf[1] = 0x03;
			buf[2] = 0xF1;
			CTP_I2C_WRITE(ts->client, I2C_BLDR_Address, buf, 3);

			buf[0] = 0x35;
			buf[1] = 0xA5;
			CTP_I2C_WRITE(ts->client, I2C_BLDR_Address, buf, 2);

			//---check CRC_ERR_FLAG---
			buf[0] = 0xFF;
			buf[1] = 0x03;
			buf[2] = 0xF1;
			CTP_I2C_WRITE(ts->client, I2C_BLDR_Address, buf, 3);

			buf[0] = 0x35;
			buf[1] = 0x00;
			CTP_I2C_READ(ts->client, I2C_BLDR_Address, buf, 2);

			if (buf[1] == 0xA5)
				break;
		}
		if (retry == 0)
			NVT_ERR("CRC auto reboot is not able to be stopped! buf[1]=0x%02X\n", buf[1]);
	}

	return;
}

/*******************************************************
Description:
	Novatek touchscreen check chip version trim function.

return:
	Executive outcomes. 0---NVT IC. -1---not NVT IC.
*******************************************************/
static int8_t nvt_ts_check_chip_ver_trim(void)
{
	uint8_t buf[8] = {0};
	int32_t retry = 0;
	int32_t list = 0;
	int32_t i = 0;
	int32_t found_nvt_chip = 0;
	int32_t ret = -1;

	nvt_bootloader_reset(); // NOT in retry loop

	//---Check for 5 times---
	for (retry = 5; retry > 0; retry--) {
		nvt_sw_reset_idle();

		buf[0] = 0x00;
		buf[1] = 0x35;
		CTP_I2C_WRITE(ts->client, I2C_HW_Address, buf, 2);
		msleep(10);

		buf[0] = 0xFF;
		buf[1] = 0x01;
		buf[2] = 0xF6;
		CTP_I2C_WRITE(ts->client, I2C_BLDR_Address, buf, 3);

		buf[0] = 0x4E;
		buf[1] = 0x00;
		buf[2] = 0x00;
		buf[3] = 0x00;
		buf[4] = 0x00;
		buf[5] = 0x00;
		buf[6] = 0x00;
		CTP_I2C_READ(ts->client, I2C_BLDR_Address, buf, 7);
		NVT_LOG("buf[1]=0x%02X, buf[2]=0x%02X, buf[3]=0x%02X, buf[4]=0x%02X, buf[5]=0x%02X, buf[6]=0x%02X\n",
			buf[1], buf[2], buf[3], buf[4], buf[5], buf[6]);

		//---Stop CRC check to prevent IC auto reboot---
		if ((buf[1] == 0xFC) ||
			((buf[1] == 0xFF) && (buf[2] == 0xFF) && (buf[3] == 0xFF))) {
			nvt_stop_crc_reboot();
			continue;
		}

		// compare read chip id on supported list
		for (list = 0; list < (sizeof(trim_id_table) / sizeof(struct nvt_ts_trim_id_table)); list++) {
			found_nvt_chip = 0;

			// compare each byte
			for (i = 0; i < NVT_ID_BYTE_MAX; i++) {
				if (trim_id_table[list].mask[i]) {
					if (buf[i + 1] != trim_id_table[list].id[i])
						break;
				}
			}

			if (i == NVT_ID_BYTE_MAX) {
				found_nvt_chip = 1;
			}

			if (found_nvt_chip) {
				NVT_LOG("This is NVT touch IC\n");
				ts->mmap = trim_id_table[list].mmap;
				ts->carrier_system = trim_id_table[list].carrier_system;
				ret = 0;
				goto out;
			} else {
				ts->mmap = NULL;
				ret = -1;
			}
		}

		msleep(10);
	}

out:
	return ret;
}
//Huaqin add for gesture by xudongfnag at 20180913 start
static void nvt_platform_shutdown(struct i2c_client *client)
{
	printk("HQ add for nvt tp shut down\n");
	nvt_lcm_power_source_ctrl(ts, 0);
}
//Huaqin add for gesture by xudongfnag at 20180913 end
/*******************************************************
Description:
	Novatek touchscreen driver probe function.

return:
	Executive outcomes. 0---succeed. negative---failed
*******************************************************/
static int32_t nvt_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int32_t ret = 0, er = 0, er1 = 0;
#if ((TOUCH_KEY_NUM > 0) || WAKEUP_GESTURE)
	int32_t retry = 0;
#endif

	NVT_LOG("start\n");

	ts = kmalloc(sizeof(struct nvt_ts_data), GFP_KERNEL);
	if (ts == NULL) {
		NVT_ERR("failed to allocated memory for nvt ts data\n");
		return -ENOMEM;
	}

	ts->client = client;
	i2c_set_clientdata(client, ts);

	//---parse dts---
	nvt_parse_dt(&client->dev);

//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 start
#if NVT_POWER_SOURCE_CUST_EN
	atomic_set(&(ts->lcm_lab_power), 0);
	atomic_set(&(ts->lcm_ibb_power), 0);
	ret = nvt_lcm_bias_power_init(ts);

	if (ret) {
		NVT_ERR("power resource init error!\n");
		goto err_power_resource_init_fail;
	}

	nvt_lcm_power_source_ctrl(ts, 1);
#endif
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 end

	//---request and config GPIOs---
	ret = nvt_gpio_config(ts);
	if (ret) {
		NVT_ERR("gpio config error!\n");
		goto err_gpio_config_failed;
	}

	//---check i2c func.---
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		NVT_ERR("i2c_check_functionality failed. (no I2C_FUNC_I2C)\n");
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}

	// need 10ms delay after POR(power on reset)
	msleep(10);

	//---check chip version trim---
	ret = nvt_ts_check_chip_ver_trim();
	if (ret) {
		NVT_ERR("chip is not identified\n");
		ret = -EINVAL;
		goto err_chipvertrim_failed;
	}

	mutex_init(&ts->lock);

	mutex_lock(&ts->lock);
	nvt_bootloader_reset();
	nvt_check_fw_reset_state(RESET_STATE_INIT);
	nvt_get_fw_info();
	mutex_unlock(&ts->lock);

	//---create workqueue---
	nvt_wq = create_workqueue("nvt_wq");
	if (!nvt_wq) {
		NVT_ERR("nvt_wq create workqueue failed\n");
		ret = -ENOMEM;
		goto err_create_nvt_wq_failed;
	}
	INIT_WORK(&ts->nvt_work, nvt_ts_work_func);


	//---allocate input device---
	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		NVT_ERR("allocate input device failed\n");
		ret = -ENOMEM;
		goto err_input_dev_alloc_failed;
	}

	ts->max_touch_num = TOUCH_MAX_FINGER_NUM;

#if TOUCH_KEY_NUM > 0
	ts->max_button_num = TOUCH_KEY_NUM;
#endif

	ts->int_trigger_type = INT_TRIGGER_TYPE;


	//---set input device info.---
	ts->input_dev->evbit[0] = BIT_MASK(EV_SYN) | BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS) ;
	ts->input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	ts->input_dev->propbit[0] = BIT(INPUT_PROP_DIRECT);

#if MT_PROTOCOL_B
	input_mt_init_slots(ts->input_dev, ts->max_touch_num, 0);
#endif

	input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE, 0, TOUCH_FORCE_NUM, 0, 0);    //pressure = TOUCH_FORCE_NUM

#if TOUCH_MAX_FINGER_NUM > 1
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);    //area = 255

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, ts->abs_x_max, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, ts->abs_y_max, 0, 0);
#if MT_PROTOCOL_B
	// no need to set ABS_MT_TRACKING_ID, input_mt_init_slots() already set it
#else
	input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID, 0, ts->max_touch_num, 0, 0);
#endif //MT_PROTOCOL_B
#endif //TOUCH_MAX_FINGER_NUM > 1

#if TOUCH_KEY_NUM > 0
	for (retry = 0; retry < ts->max_button_num; retry++) {
		input_set_capability(ts->input_dev, EV_KEY, touch_key_array[retry]);
	}
#endif

#if WAKEUP_GESTURE
	for (retry = 0; retry < (sizeof(gesture_key_array) / sizeof(gesture_key_array[0])); retry++) {
		input_set_capability(ts->input_dev, EV_KEY, gesture_key_array[retry]);
	}
	__set_bit(GESTURE_EVENT_DOUBLE_CLICK, ts->input_dev->keybit);
	__set_bit(GESTURE_EVENT_E, ts->input_dev->keybit);
	__set_bit(GESTURE_EVENT_W, ts->input_dev->keybit);
	__set_bit(GESTURE_EVENT_S, ts->input_dev->keybit);
	__set_bit(GESTURE_EVENT_V, ts->input_dev->keybit);
	__set_bit(GESTURE_EVENT_Z, ts->input_dev->keybit);
	__set_bit(GESTURE_EVENT_C, ts->input_dev->keybit);
	wake_lock_init(&gestrue_wakelock, WAKE_LOCK_SUSPEND, "poll-wake-lock");
#endif

	sprintf(ts->phys, "input/ts");
	ts->input_dev->name = NVT_TS_NAME;
	ts->input_dev->phys = ts->phys;
	ts->input_dev->id.bustype = BUS_I2C;

	//---register input device---
	ret = input_register_device(ts->input_dev);
	if (ret) {
		NVT_ERR("register input device (%s) failed. ret=%d\n", ts->input_dev->name, ret);
		goto err_input_register_device_failed;
	}

	//---set int-pin & request irq---
	client->irq = gpio_to_irq(ts->irq_gpio);
	if (client->irq) {
		NVT_LOG("int_trigger_type=%d\n", ts->int_trigger_type);

#if WAKEUP_GESTURE
		ret = request_irq(client->irq, nvt_ts_irq_handler, ts->int_trigger_type | IRQF_NO_SUSPEND, client->name, ts);
#else
		ret = request_irq(client->irq, nvt_ts_irq_handler, ts->int_trigger_type, client->name, ts);
#endif
		if (ret != 0) {
			NVT_ERR("request irq failed. ret=%d\n", ret);
			goto err_int_request_failed;
		} else {
			disable_irq(client->irq);
			NVT_LOG("request irq %d succeed\n", client->irq);
		}
	}

#if BOOT_UPDATE_FIRMWARE
	nvt_fwu_wq = create_singlethread_workqueue("nvt_fwu_wq");
	if (!nvt_fwu_wq) {
		NVT_ERR("nvt_fwu_wq create workqueue failed\n");
		ret = -ENOMEM;
		goto err_create_nvt_fwu_wq_failed;
	}
	INIT_DELAYED_WORK(&ts->nvt_fwu_work, Boot_Update_Firmware);
	// please make sure boot update start after display reset(RESX) sequence
	/*Huaqin modify for work delay by lanshiming at 2018/11/6 start*/
	queue_delayed_work(nvt_fwu_wq, &ts->nvt_fwu_work, msecs_to_jiffies(10000));
	/*Huaqin modify for work delay by lanshiming at 2018/11/6 end*/
#endif

#if 0
	/* Huaqin add by zhangxiude for ITO test start */
	//--------add ito node
	platform_device_register(&hwinfo_device);
	nvt_test_node_init(&hwinfo_device);
	/* Huaqin add by zhangxiude for ITO test end */
#endif
	firmware_id=ts->fw_ver;
	nvt_tp_info_proc_init();

	//---set device node---
#if NVT_TOUCH_PROC
	ret = nvt_flash_proc_init();
	if (ret != 0) {
		NVT_ERR("nvt flash proc init failed. ret=%d\n", ret);
		goto err_init_NVT_ts;
	}
#endif

#if NVT_TOUCH_EXT_PROC
	ret = nvt_extra_proc_init();
	if (ret != 0) {
		NVT_ERR("nvt extra proc init failed. ret=%d\n", ret);
		goto err_init_NVT_ts;
	}
#endif

#if NVT_TOUCH_MP
	ret = nvt_mp_proc_init();
	if (ret != 0) {
		NVT_ERR("nvt mp proc init failed. ret=%d\n", ret);
		goto err_init_NVT_ts;
	}
#endif
//Huaqin add for gesture by xudongfang at 20180913 start
#if WAKEUP_GESTURE
	er = create_gesture_node();
	er1 = nvt_gesture_proc_init();

	nvt_gesture_mode_proc = proc_create(NVT_GESTURE_MODE, 0644, NULL,
				&gesture_mode_proc_ops);
	if (!nvt_gesture_mode_proc) {
		NVT_ERR("create proc tpd_gesture failed\n");
	}
#endif
//Huaqin add for gesture by xudongfang at 20180913 end

#if defined(CONFIG_FB)
	ts->fb_notif.notifier_call = fb_notifier_callback;
	ret = fb_register_client(&ts->fb_notif);
	if(ret) {
		NVT_ERR("register fb_notifier failed. ret=%d\n", ret);
		goto err_register_fb_notif_failed;
	}
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	ts->early_suspend.suspend = nvt_ts_early_suspend;
	ts->early_suspend.resume = nvt_ts_late_resume;
	ret = register_early_suspend(&ts->early_suspend);
	if(ret) {
		NVT_ERR("register early suspend failed. ret=%d\n", ret);
		goto err_register_early_suspend_failed;
	}
#endif

	bTouchIsAwake = 1;
	NVT_LOG("end\n");

	enable_irq(client->irq);

	return 0;

#if defined(CONFIG_FB)
err_register_fb_notif_failed:
#elif defined(CONFIG_HAS_EARLYSUSPEND)
err_register_early_suspend_failed:
#endif
#if (NVT_TOUCH_PROC || NVT_TOUCH_EXT_PROC || NVT_TOUCH_MP)
err_init_NVT_ts:
#endif
	free_irq(client->irq, ts);
#if BOOT_UPDATE_FIRMWARE
err_create_nvt_fwu_wq_failed:
#endif
err_int_request_failed:
err_input_register_device_failed:
	input_free_device(ts->input_dev);
err_input_dev_alloc_failed:
err_create_nvt_wq_failed:
	mutex_destroy(&ts->lock);
err_chipvertrim_failed:
err_check_functionality_failed:
	gpio_free(ts->irq_gpio);
/* Huaqin add by zhangxiude for ITO test start */
#if NVT_TOUCH_SUPPORT_HW_RST
       gpio_free(ts->reset_gpio);
#endif
/* Huaqin add by zhangxiude for ITO test end */
err_gpio_config_failed:
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 start
	nvt_lcm_power_source_ctrl(ts, 0);
	nvt_lcm_bias_power_deinit(ts);
err_power_resource_init_fail:
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 end
	i2c_set_clientdata(client, NULL);
	kfree(ts);
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen driver release function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_ts_remove(struct i2c_client *client)
{
	//struct nvt_ts_data *ts = i2c_get_clientdata(client);

#if defined(CONFIG_FB)
	if (fb_unregister_client(&ts->fb_notif))
		NVT_ERR("Error occurred while unregistering fb_notifier.\n");
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&ts->early_suspend);
#endif

	mutex_destroy(&ts->lock);

	NVT_LOG("Removing driver...\n");

	free_irq(client->irq, ts);
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 start
	nvt_lcm_bias_power_deinit(ts);
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 end
	input_unregister_device(ts->input_dev);
	i2c_set_clientdata(client, NULL);
	kfree(ts);

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen driver suspend function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_ts_suspend(struct device *dev)
{
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 start
struct nvt_ts_data *data = dev_get_drvdata(dev);
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 end
	uint8_t buf[4] = {0};
#if MT_PROTOCOL_B
	uint32_t i = 0;
#endif

	if (!bTouchIsAwake) {
		NVT_LOG("Touch is already suspend\n");
		return 0;
	}

	mutex_lock(&ts->lock);

	NVT_LOG("start\n");

	bTouchIsAwake = 0;

#if WAKEUP_GESTURE
//Huaqin add for gesture by xudongfang at 20180913 start
if (!allow_gesture && !screen_gesture && !double_tap_state && !letter_c_state &&
	!letter_e_state && !letter_s_state && !letter_v_state && !letter_w_state &&
	!letter_z_state && !letter_m_state && !letter_o_state && !up_swipe_state &&
	!down_swipe_state && !left_swipe_state && !right_swipe_state) {
	disable_irq(ts->client->irq);

	//---write i2c command to enter "deep sleep mode"---
	buf[0] = EVENT_MAP_HOST_CMD;
	buf[1] = 0x11;
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);
	NVT_LOG("Enter normal mode sleep \n");
}
else {
	//---write i2c command to enter "wakeup gesture mode"---
	buf[0] = EVENT_MAP_HOST_CMD;
	buf[1] = 0x13;
#if 0 // Do not set 0xFF first, ToDo
	buf[2] = 0xFF;
	buf[3] = 0xFF;
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 4);
#else
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);
#endif

	enable_irq_wake(ts->client->irq);

	NVT_LOG("Enabled touch wakeup gesture\n");
}
#else // WAKEUP_GESTURE
	disable_irq(ts->client->irq);

	//---write i2c command to enter "deep sleep mode"---
	buf[0] = EVENT_MAP_HOST_CMD;
	buf[1] = 0x11;
	CTP_I2C_WRITE(ts->client, I2C_FW_Address, buf, 2);
#endif // WAKEUP_GESTURE

	/* release all touches */
#if MT_PROTOCOL_B
	for (i = 0; i < ts->max_touch_num; i++) {
		input_mt_slot(ts->input_dev, i);
		input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
	}
#endif
	input_report_key(ts->input_dev, BTN_TOUCH, 0);
#if !MT_PROTOCOL_B
	input_mt_sync(ts->input_dev);
#endif
	input_sync(ts->input_dev);

	msleep(50);

	mutex_unlock(&ts->lock);
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 start
#if NVT_POWER_SOURCE_CUST_EN
	if (!allow_gesture && !screen_gesture && !double_tap_state && !letter_c_state &&
	!letter_e_state && !letter_s_state && !letter_v_state && !letter_w_state &&
	!letter_z_state && !letter_m_state && !letter_o_state && !up_swipe_state &&
	!down_swipe_state && !left_swipe_state && !right_swipe_state) {
	nvt_lcm_power_source_ctrl(data, 0);//disable vsp/vsn
	NVT_LOG("sleep suspend end  disable vsp/vsn\n");
	}
	else{
	NVT_LOG("gesture suspend end not disable vsp/vsn\n");
	}
#endif
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 end

	NVT_LOG("end\n");

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen driver resume function.

return:
	Executive outcomes. 0---succeed.
*******************************************************/
static int32_t nvt_ts_resume(struct device *dev)
{
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 start
	struct nvt_ts_data *data = dev_get_drvdata(dev);
	nvt_lcm_power_source_ctrl(data, 1);//enable vsp/vsn
//Huaqin add for VSN/VSP by xudongfang at 2018/9/5 end
	if (bTouchIsAwake) {
		NVT_LOG("Touch is already resume\n");
		return 0;
	}

	mutex_lock(&ts->lock);

	NVT_LOG("start\n");

	// please make sure display reset(RESX) sequence and mipi dsi cmds sent before this
#if NVT_TOUCH_SUPPORT_HW_RST
	gpio_set_value(ts->reset_gpio, 1);
#endif
	nvt_bootloader_reset();
	nvt_check_fw_reset_state(RESET_STATE_REK);

//Huaqin add for gesture by xudongfang at 20180913 start
if (allow_gesture && screen_gesture) {
	enable_irq(ts->client->irq);
	}
//Huaqin add for gesture by xudongfang at 20180913 end

	bTouchIsAwake = 1;

	mutex_unlock(&ts->lock);

	NVT_LOG("end\n");

	return 0;
}
/* Huaqin add for TT 1242335 zhangxiude at 2018/9/30 start */
int fb_nvt_ts_resume(void *data)
{
	nvt_ts_resume(data);

	return 0;
}
/* Huaqin add for TT 1242335 zhangxiude at 2018/9/30 end */
#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	struct nvt_ts_data *ts =
		container_of(self, struct nvt_ts_data, fb_notif);

	if (evdata && evdata->data && event == FB_EARLY_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_POWERDOWN) {
			nvt_ts_suspend(&ts->client->dev);
		}
	} else if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK) {
			/* Huaqin add for TT 1242335 zhangxiude at 2018/9/30 start */
			kthread_run(fb_nvt_ts_resume,&ts->client->dev,"tp_resume");
			/* Huaqin add for TT 1242335 zhangxiude at 2018/9/30 start */
		}
	}

	return 0;
}
#elif defined(CONFIG_HAS_EARLYSUSPEND)
/*******************************************************
Description:
	Novatek touchscreen driver early suspend function.

return:
	n.a.
*******************************************************/
static void nvt_ts_early_suspend(struct early_suspend *h)
{
	nvt_ts_suspend(ts->client, PMSG_SUSPEND);
}

/*******************************************************
Description:
	Novatek touchscreen driver late resume function.

return:
	n.a.
*******************************************************/
static void nvt_ts_late_resume(struct early_suspend *h)
{
	nvt_ts_resume(ts->client);
}
#endif

#if 0
static const struct dev_pm_ops nvt_ts_dev_pm_ops = {
	.suspend = nvt_ts_suspend,
	.resume  = nvt_ts_resume,
};
#endif

static const struct i2c_device_id nvt_ts_id[] = {
	{ NVT_I2C_NAME, 0 },
	{ }
};

#ifdef CONFIG_OF
static struct of_device_id nvt_match_table[] = {
	{ .compatible = "novatek,NVT-ts",},
	{ },
};
#endif
/*
static struct i2c_board_info __initdata nvt_i2c_boardinfo[] = {
	{
		I2C_BOARD_INFO(NVT_I2C_NAME, I2C_FW_Address),
	},
};
*/

static struct i2c_driver nvt_i2c_driver = {
	.probe		= nvt_ts_probe,
//Huaqin add for shutdown time by xudongfang at 20180913 start
	.shutdown   = nvt_platform_shutdown,
//Huaqin add for shutdown time by xudongfang at 20180913 end
	.remove		= nvt_ts_remove,
//	.suspend	= nvt_ts_suspend,
//	.resume		= nvt_ts_resume,
	.id_table	= nvt_ts_id,
	.driver = {
		.name	= NVT_I2C_NAME,
		.owner	= THIS_MODULE,
#if 0
#ifdef CONFIG_PM
		.pm = &nvt_ts_dev_pm_ops,
#endif
#endif
#ifdef CONFIG_OF
		.of_match_table = nvt_match_table,
#endif
	},
};

/*******************************************************
Description:
	Driver Install function.

return:
	Executive Outcomes. 0---succeed. not 0---failed.
********************************************************/
static int32_t __init nvt_driver_init(void)
{
	int32_t ret = 0;

	NVT_LOG("start\n");
	//---add i2c driver---
	ret = i2c_add_driver(&nvt_i2c_driver);
	if (ret) {
		pr_err("%s: failed to add i2c driver", __func__);
		goto err_driver;
	}

	pr_info("%s: finished\n", __func__);

err_driver:
	return ret;
}

/*******************************************************
Description:
	Driver uninstall function.

return:
	n.a.
********************************************************/
static void __exit nvt_driver_exit(void)
{
	i2c_del_driver(&nvt_i2c_driver);
	destroy_gesture();

	if (nvt_wq)
		destroy_workqueue(nvt_wq);

#if BOOT_UPDATE_FIRMWARE
	if (nvt_fwu_wq)
		destroy_workqueue(nvt_fwu_wq);
#endif
}

//late_initcall(nvt_driver_init);
module_init(nvt_driver_init);
module_exit(nvt_driver_exit);

MODULE_DESCRIPTION("Novatek Touchscreen Driver");
MODULE_LICENSE("GPL");
