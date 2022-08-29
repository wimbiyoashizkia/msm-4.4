/*
 * Copyright (C) 2005-2008 Google, Inc.
 * Copyright (C) 2013 Paul Reioux.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/powersuspend.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

#define MAJOR_VERSION	1
#define SUB_MINOR_VERSION 1
#define MINOR_VERSION	8

struct workqueue_struct *suspend_work_queue;

static DEFINE_MUTEX(power_suspend_lock);
static LIST_HEAD(power_suspend_handlers);
static DEFINE_SPINLOCK(state_lock);

struct work_struct power_suspend_work;
struct work_struct power_resume_work;

static void power_suspend(struct work_struct *work);
static void power_resume(struct work_struct *work);

static int state;
static int mode;

void register_power_suspend(struct power_suspend *handler)
{
	struct list_head *pos;

	mutex_lock(&power_suspend_lock);
	list_for_each(pos, &power_suspend_handlers) {
		struct power_suspend *p;
		p = list_entry(pos, struct power_suspend, link);
	}
	list_add_tail(&handler->link, pos);
	mutex_unlock(&power_suspend_lock);
}
EXPORT_SYMBOL(register_power_suspend);

void unregister_power_suspend(struct power_suspend *handler)
{
	mutex_lock(&power_suspend_lock);
	list_del(&handler->link);
	mutex_unlock(&power_suspend_lock);
}
EXPORT_SYMBOL(unregister_power_suspend);

static void power_suspend(struct work_struct *work)
{
	struct power_suspend *pos;
	unsigned long irqflags;
	int abort = 0;

	pr_info("Powersuspend : Entering suspend...\n");

	mutex_lock(&power_suspend_lock);
	spin_lock_irqsave(&state_lock, irqflags);
	if (state == POWER_SUSPEND_INACTIVE)
		abort = 1;
	spin_unlock_irqrestore(&state_lock, irqflags);

	if (abort)
		goto abort_suspend;

	pr_info("Powersuspend : Suspending...\n");

	list_for_each_entry(pos, &power_suspend_handlers, link) {
		if (pos->suspend != NULL) {
			pos->suspend(pos);
		}
	}

	pr_info("Powersuspend : Suspend completed.\n");

abort_suspend:
	mutex_unlock(&power_suspend_lock);
}

static void power_resume(struct work_struct *work)
{
	struct power_suspend *pos;
	unsigned long irqflags;
	int abort = 0;

	pr_info("Powersuspend : Entering resume...\n");

	mutex_lock(&power_suspend_lock);
	spin_lock_irqsave(&state_lock, irqflags);
	if (state == POWER_SUSPEND_ACTIVE)
		abort = 1;
	spin_unlock_irqrestore(&state_lock, irqflags);

	if (abort)
		goto abort_resume;

	pr_info("Powersuspend : Resuming...\n");

	list_for_each_entry_reverse(pos, &power_suspend_handlers, link) {
		if (pos->resume != NULL) {
			pos->resume(pos);
		}
	}

	pr_info("Powersuspend : Resume completed.\n");

abort_resume:
	mutex_unlock(&power_suspend_lock);
}

bool power_suspended = false;

void set_power_suspend_state(int new_state)
{
	unsigned long irqflags;

	spin_lock_irqsave(&state_lock, irqflags);
	if (state == POWER_SUSPEND_INACTIVE && new_state == POWER_SUSPEND_ACTIVE) {
		pr_info("Powersuspend : State activated.\n");
		state = new_state;
		queue_work(suspend_work_queue, &power_suspend_work);
	} else if (state == POWER_SUSPEND_ACTIVE && new_state == POWER_SUSPEND_INACTIVE) {
		pr_info("Powersuspend : State deactivated.\n");
		state = new_state;
		queue_work(suspend_work_queue, &power_resume_work);
	}
	spin_unlock_irqrestore(&state_lock, irqflags);
}

void set_power_suspend_state_autosleep_hook(int new_state)
{
	pr_info("Powersuspend : Autosleep resquests %s.\n", new_state == POWER_SUSPEND_ACTIVE ? "sleep" : "wakeup");
	if (mode == POWER_SUSPEND_AUTOSLEEP || mode == POWER_SUSPEND_HYBRID)
		set_power_suspend_state(new_state);
}
EXPORT_SYMBOL(set_power_suspend_state_autosleep_hook);

void set_power_suspend_state_panel_hook(int new_state)
{
	pr_info("Powersuspend : Panel resquests %s.\n", new_state == POWER_SUSPEND_ACTIVE ? "sleep" : "wakeup");
	if (mode == POWER_SUSPEND_PANEL || mode == POWER_SUSPEND_HYBRID)
		set_power_suspend_state(new_state);
}
EXPORT_SYMBOL(set_power_suspend_state_panel_hook);

/* -------------------- sysfs interface -------------------- */
static ssize_t power_suspend_state_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", state);
}

static ssize_t power_suspend_state_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int new_state = 0;

	if (mode != POWER_SUSPEND_USERSPACE)
		return -EINVAL;

	sscanf(buf, "%d\n", &new_state);

	pr_info("Powersuspend : Userspace resquests %s.\n", new_state == POWER_SUSPEND_ACTIVE ? "sleep" : "wakeup");
	if (new_state == POWER_SUSPEND_ACTIVE || new_state == POWER_SUSPEND_INACTIVE)
		set_power_suspend_state(new_state);

	return count;
}

static struct kobj_attribute power_suspend_state_attribute =
	__ATTR(power_suspend_state, 0664,
		power_suspend_state_show,
		power_suspend_state_store);

static ssize_t power_suspend_mode_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", mode);
}

static ssize_t power_suspend_mode_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	int data = 0;

	sscanf(buf, "%d\n", &data);

	switch (data) {
		case POWER_SUSPEND_AUTOSLEEP:
		case POWER_SUSPEND_PANEL:
		case POWER_SUSPEND_USERSPACE:
		case POWER_SUSPEND_HYBRID:
			mode = data;
			return count;
		default:
			return -EINVAL;
	}
}

static struct kobj_attribute power_suspend_mode_attribute =
	__ATTR(power_suspend_mode, 0664,
		power_suspend_mode_show,
		power_suspend_mode_store);

static ssize_t power_suspend_version_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "version: %d.%d.%d\n", MAJOR_VERSION, MINOR_VERSION, SUB_MINOR_VERSION);
}

static struct kobj_attribute power_suspend_version_attribute =
	__ATTR(power_suspend_version, 0444,
		power_suspend_version_show,
		NULL);

static struct attribute *power_suspend_attrs[] =
{
	&power_suspend_state_attribute.attr,
	&power_suspend_mode_attribute.attr,
	&power_suspend_version_attribute.attr,
	NULL,
};

static struct attribute_group power_suspend_attr_group =
{
	.attrs = power_suspend_attrs,
};

static struct kobject *power_suspend_kobj;

/* -------------------- sysfs interface -------------------- */
static int power_suspend_init(void)
{

	int sysfs_result;

	power_suspend_kobj = kobject_create_and_add("power_suspend", kernel_kobj);
	if (!power_suspend_kobj) {
		pr_err("%s kobject create failed!\n", __FUNCTION__);
		return -ENOMEM;
	}

	sysfs_result = sysfs_create_group(power_suspend_kobj, &power_suspend_attr_group);
	if (sysfs_result) {
		pr_info("%s group create failed!\n", __FUNCTION__);
		kobject_put(power_suspend_kobj);
		return -ENOMEM;
	}

	suspend_work_queue = create_singlethread_workqueue("p-suspend");

	if (suspend_work_queue == NULL) {
		return -ENOMEM;
	}

	mode = POWER_SUSPEND_HYBRID;

	INIT_WORK(&power_suspend_work, power_suspend);
	INIT_WORK(&power_resume_work, power_resume);

	return 0;
}

static void power_suspend_exit(void)
{
	flush_work(&power_suspend_work);
	flush_work(&power_resume_work);

	if (power_suspend_kobj != NULL)
		kobject_put(power_suspend_kobj);

	destroy_workqueue(suspend_work_queue);
} 

subsys_initcall(power_suspend_init);
module_exit(power_suspend_exit);

MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_DESCRIPTION("power_suspend - A replacement kernel PM driver for"
					"Android's deprecated early_suspend/late_resume PM driver!");
MODULE_LICENSE("GPL v2");
