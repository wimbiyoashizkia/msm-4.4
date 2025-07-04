// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/sched/sysctl.h>

static unsigned int input_boost_freq_lp __read_mostly =
	CONFIG_INPUT_BOOST_FREQ_LP;
static unsigned int input_boost_freq_hp __read_mostly =
	CONFIG_INPUT_BOOST_FREQ_PERF;
static unsigned int max_boost_freq_lp __read_mostly =
	CONFIG_MAX_BOOST_FREQ_LP;
static unsigned int max_boost_freq_hp __read_mostly =
	CONFIG_MAX_BOOST_FREQ_PERF;

static unsigned short input_boost_duration __read_mostly =
	CONFIG_INPUT_BOOST_DURATION_MS;
static unsigned short wake_boost_duration __read_mostly =
	CONFIG_WAKE_BOOST_DURATION_MS;

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static unsigned short dynamic_stune_boost;
static int boost_slot;
static bool stune_boost_active;
#endif

module_param(input_boost_freq_lp, uint, 0644);
module_param(input_boost_freq_hp, uint, 0644);
module_param(max_boost_freq_lp, uint, 0644);
module_param(max_boost_freq_hp, uint, 0644);

module_param(input_boost_duration, short, 0644);
module_param(wake_boost_duration, short, 0644);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
module_param(dynamic_stune_boost, short, 0644);
#endif

enum {
	SCREEN_OFF,
	INPUT_BOOST,
	MAX_BOOST
};

struct boost_drv {
	struct delayed_work input_unboost;
	struct delayed_work max_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block fb_notif;
	wait_queue_head_t boost_waitq;
	atomic_long_t max_boost_expires;
	unsigned long state;
	unsigned long last_input_jiffies;
};

static void input_unboost_worker(struct work_struct *work);
static void max_unboost_worker(struct work_struct *work);

static struct boost_drv boost_drv_g __read_mostly = {
	.input_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.input_unboost,
						    input_unboost_worker, 0),
	.max_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.max_unboost,
						  max_unboost_worker, 0),
	.boost_waitq = __WAIT_QUEUE_HEAD_INITIALIZER(boost_drv_g.boost_waitq)
};

static unsigned int get_input_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = input_boost_freq_lp;
	else
		freq = input_boost_freq_hp;

	return min(freq, policy->max);
}

static unsigned int get_max_boost_freq(struct cpufreq_policy *policy)
{
	unsigned int freq;

	if (cpumask_test_cpu(policy->cpu, cpu_lp_mask))
		freq = max_boost_freq_lp;
	else
		freq = max_boost_freq_hp;

	return min(freq, policy->max);
}

static void update_online_cpu_policy(void)
{
	unsigned int cpu;

	get_online_cpus();
	for_each_possible_cpu(cpu) {
		if (cpu_online(cpu)) {
			if (cpumask_intersects(cpumask_of(cpu), cpu_lp_mask))
				cpufreq_update_policy(cpu);
			if (cpumask_intersects(cpumask_of(cpu), cpu_perf_mask))
				cpufreq_update_policy(cpu);
		}
	}
	put_online_cpus();
}

bool cpu_input_boost_within_input(unsigned long timeout_ms)
{
	struct boost_drv *b = &boost_drv_g;

	return time_before(jiffies, b->last_input_jiffies +
			   msecs_to_jiffies(timeout_ms));
}

static void __cpu_input_boost_kick(struct boost_drv *b)
{
	unsigned int ret;

	if (test_bit(SCREEN_OFF, &b->state))
		return;

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	ret = do_stune_boost("top-app", dynamic_stune_boost, &boost_slot);
	if (!ret)
		stune_boost_active = true;
#endif

	set_bit(INPUT_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->input_unboost,
			      msecs_to_jiffies(input_boost_duration)))
		wake_up(&b->boost_waitq);
}

void cpu_input_boost_kick(void)
{
	struct boost_drv *b = &boost_drv_g;

	__cpu_input_boost_kick(b);
}

static void __cpu_input_boost_kick_max(struct boost_drv *b,
				       unsigned int duration_ms)
{
	unsigned long boost_jiffies = msecs_to_jiffies(duration_ms);
	unsigned long curr_expires, new_expires;

	if (test_bit(SCREEN_OFF, &b->state))
		return;

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	do_stune_boost("top-app", dynamic_stune_boost, &boost_slot);
#endif

	do {
		curr_expires = atomic_long_read(&b->max_boost_expires);
		new_expires = jiffies + boost_jiffies;

		/* Skip this boost if there's a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic_long_cmpxchg(&b->max_boost_expires, curr_expires,
				     new_expires) != curr_expires);

	set_bit(MAX_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->max_unboost,
			      boost_jiffies))
		wake_up(&b->boost_waitq);
}

void cpu_input_boost_kick_max(unsigned int duration_ms)
{
	struct boost_drv *b = &boost_drv_g;

	sysctl_sched_energy_aware = 0;

	__cpu_input_boost_kick_max(b, duration_ms);
}

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), input_unboost);

	clear_bit(INPUT_BOOST, &b->state);
	wake_up(&b->boost_waitq);
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
#endif
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), max_unboost);

	clear_bit(MAX_BOOST, &b->state);
	wake_up(&b->boost_waitq);
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
#endif

	sysctl_sched_energy_aware = 1;
}

static int cpu_boost_thread(void *data)
{
	static const struct sched_param sched_max_rt_prio = {
		.sched_priority = MAX_RT_PRIO - 1
	};
	struct boost_drv *b = data;
	unsigned long old_state = 0;

	sched_setscheduler_nocheck(current, SCHED_FIFO, &sched_max_rt_prio);

	while (1) {
		bool should_stop = false;
		unsigned long curr_state;

		wait_event(b->boost_waitq,
			(curr_state = READ_ONCE(b->state)) != old_state ||
			(should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		old_state = curr_state;
		update_online_cpu_policy();
	}

	return 0;
}

static int cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	/* Unboost when the screen is off */
	if (test_bit(SCREEN_OFF, &b->state)) {
		policy->min = policy->cpuinfo.min_freq;
		return NOTIFY_OK;
	}

	/* Boost CPU to max frequency for max boost */
	if (test_bit(MAX_BOOST, &b->state)) {
		policy->min = get_max_boost_freq(policy);
		return NOTIFY_OK;
	}

	/*
	 * Boost to policy->max if the boost frequency is higher. When
	 * unboosting, set policy->min to the absolute min freq for the CPU.
	 */
	if (test_bit(INPUT_BOOST, &b->state))
		policy->min = get_input_boost_freq(policy);
	else
		policy->min = policy->cpuinfo.min_freq;

	return NOTIFY_OK;
}

static int fb_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), fb_notif);
	int *blank = ((struct fb_event *)data)->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	if (*blank == FB_BLANK_UNBLANK) {
		clear_bit(SCREEN_OFF, &b->state);
		__cpu_input_boost_kick_max(b, wake_boost_duration);
	} else {
		set_bit(SCREEN_OFF, &b->state);
		wake_up(&b->boost_waitq);
	}

	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
					unsigned int type, unsigned int code,
					int value)
{
	struct boost_drv *b = handle->handler->private;

	__cpu_input_boost_kick(b);

	b->last_input_jiffies = jiffies;
}

static int cpu_input_boost_input_connect(struct input_handler *handler,
					 struct input_dev *dev,
					 const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
#endif
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b = &boost_drv_g;
	struct task_struct *thread;
	int ret;

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		return ret;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->last_input_jiffies = jiffies;
	b->fb_notif.notifier_call = fb_notifier_cb;
	b->fb_notif.priority = INT_MAX;
	ret = fb_register_client(&b->fb_notif);
	if (ret) {
		pr_err("Failed to register fb notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	thread = kthread_run(cpu_boost_thread, b, "cpu_boostd");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start CPU boost thread, err: %d\n", ret);
		goto unregister_fb_notif;
	}

	return 0;

unregister_fb_notif:
	fb_unregister_client(&b->fb_notif);
unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	return ret;
}
subsys_initcall(cpu_input_boost_init);
