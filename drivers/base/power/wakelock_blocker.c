// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017, andip71 <andreasp@gmx.de>
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include "wakelock_blocker.h"

char list_wakelock[LENGTH_LIST_WAKELOCK] = {0};
char list_wakelock_default[LENGTH_LIST_WAKELOCK_DEFAULT] = {0};

extern char list_wakelock_search[LENGTH_LIST_WAKELOCK_SEARCH];
extern bool wakelock_blocker_active;
extern bool wakelock_blocker_debug;

static void build_search_string(char *list1, char *list2)
{
	snprintf(list_wakelock_search, sizeof(list_wakelock_search), ";%s;%s;", list1, list2);

	if (strlen(list_wakelock_search) > 5)
		wakelock_blocker_active = true;
	else
		wakelock_blocker_active = false;
}

static ssize_t wakelock_blocker_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return snprintf(buf, sizeof(list_wakelock), "%s\n", list_wakelock);
}

static ssize_t wakelock_blocker_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t n)
{
	int len = n;

	if (len > LENGTH_LIST_WAKELOCK)
		return -EINVAL;

	sscanf(buf, "%s", list_wakelock);
	build_search_string(list_wakelock_default, list_wakelock);

	return n;
}

static ssize_t wakelock_blocker_default_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return snprintf(buf, sizeof(list_wakelock_default), "%s\n", list_wakelock_default);
}

static ssize_t wakelock_blocker_default_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t n)
{
	int len = n;

	if (len > LENGTH_LIST_WAKELOCK_DEFAULT)
		return -EINVAL;

	sscanf(buf, "%s", list_wakelock_default);
	build_search_string(list_wakelock_default, list_wakelock);

	return n;
}

static ssize_t debug_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "Debug status: %d\n\nUser list: %s\nDefault list: %s\nSearch list: %s\nActive: %d\n",
					wakelock_blocker_debug, list_wakelock, list_wakelock_default, list_wakelock_search, wakelock_blocker_active);
}

static ssize_t debug_store(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	unsigned int ret = -EINVAL;
	unsigned int val;

	ret = sscanf(buf, "%d", &val);

	if (ret != 1)
		return -EINVAL;

	if (val == 1)
		wakelock_blocker_debug = true;
	else
		wakelock_blocker_debug = false;

	return count;
}


static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, sizeof(WAKELOCK_BLOCKER_VERSION), "%s\n", WAKELOCK_BLOCKER_VERSION);
}

static DEVICE_ATTR(wakelock_blocker, 0644,
				    wakelock_blocker_show, wakelock_blocker_store);
static DEVICE_ATTR(wakelock_blocker_default, 0644,
				    wakelock_blocker_default_show, wakelock_blocker_default_store);
static DEVICE_ATTR(debug, 0664,
				    debug_show, debug_store);
static DEVICE_ATTR(version, 0664,
				    version_show, NULL);

static struct attribute *wakelock_blocker_attributes[] = {
	&dev_attr_wakelock_blocker.attr,
	&dev_attr_wakelock_blocker_default.attr,
	&dev_attr_debug.attr,
	&dev_attr_version.attr,
	NULL,
};

static struct attribute_group wakelock_blocker_control_group = {
	.attrs = wakelock_blocker_attributes,
};

static struct miscdevice wakelock_blocker_control_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "wakelock_blocker",
};

static int wakelock_blocker_init(void)
{
	misc_register(&wakelock_blocker_control_device);
	if (sysfs_create_group(&wakelock_blocker_control_device.this_device->kobj,
				&wakelock_blocker_control_group) < 0) {
		pr_debug("wakelock blocker: failed to create sys fs object.\n");
		return 0;
	}

	scnprintf(list_wakelock_default, sizeof(LIST_WAKELOCK_DEFAULT), "%s", LIST_WAKELOCK_DEFAULT);
	build_search_string(list_wakelock_default, list_wakelock);

	pr_debug("wakelock blocker: driver version %s started\n", WAKELOCK_BLOCKER_VERSION);

	return 0;
}


static void wakelock_blocker_exit(void)
{
	sysfs_remove_group(&wakelock_blocker_control_device.this_device->kobj,
		&wakelock_blocker_control_group);

	pr_debug("wakelock blocker: driver stopped\n");
}

module_init(wakelock_blocker_init);
module_exit(wakelock_blocker_exit);
