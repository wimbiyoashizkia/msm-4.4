/* drivers/cpufreq/qcom-cpufreq.c
 *
 * MSM architecture cpufreq driver
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2007-2017, The Linux Foundation. All rights reserved.
 * Author: Mike A. Chan <mikechan@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/suspend.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/of_device.h>
#include <linux/cpu_cooling.h>
#include <trace/events/power.h>

static DEFINE_MUTEX(l2bw_lock);

static struct thermal_cooling_device *cdev[NR_CPUS];
static struct clk *cpu_clk[NR_CPUS];
static struct clk *l2_clk;
static DEFINE_PER_CPU(struct cpufreq_frequency_table *, freq_table);
static bool hotplug_ready;

struct cpufreq_suspend_t {
	struct mutex suspend_mutex;
	int device_suspended;
};

static DEFINE_PER_CPU(struct cpufreq_suspend_t, suspend_data);

struct clk_update_data {
	struct mutex update_lock;
	ktime_t last_update;
};

static struct clk_update_data cpu_update_data[NR_CPUS];

static int set_cpu_freq(struct cpufreq_policy *policy, unsigned int new_freq,
			unsigned int index)
{
	int ret = 0;
	struct cpufreq_freqs freqs;
	unsigned long rate;

	freqs.old = policy->cur;
	freqs.new = new_freq;
	freqs.cpu = policy->cpu;

	trace_cpu_frequency_switch_start(freqs.old, freqs.new, policy->cpu);
	cpufreq_freq_transition_begin(policy, &freqs);

	rate = new_freq * 1000;
	rate = clk_round_rate(cpu_clk[policy->cpu], rate);
	ret = clk_set_rate(cpu_clk[policy->cpu], rate);
	cpufreq_freq_transition_end(policy, &freqs, ret);
	if (!ret)
		trace_cpu_frequency_switch_end(policy->cpu);

	return ret;
}

static int msm_cpufreq_target(struct cpufreq_policy *policy,
				unsigned int target_freq,
				unsigned int relation)
{
	int ret = 0;
	int index;
	struct cpufreq_frequency_table *table;
	struct clk_update_data *udata;
	s64 delta_us;

	mutex_lock(&per_cpu(suspend_data, policy->cpu).suspend_mutex);

	udata = &cpu_update_data[cpumask_first(policy->cpus)];
	mutex_lock(&udata->update_lock);

	if (target_freq == policy->cur)
		goto done;

	if (per_cpu(suspend_data, policy->cpu).device_suspended) {
		pr_debug("cpufreq: cpu%d scheduling frequency change "
				"in suspend.\n", policy->cpu);
		ret = -EFAULT;
		goto done;
	}

	table = cpufreq_frequency_get_table(policy->cpu);
	if (!table) {
		pr_err("cpufreq: Failed to get frequency table for CPU%u\n",
		       policy->cpu);
		ret = -ENODEV;
		goto done;
	}
	if (cpufreq_frequency_table_target(policy, table, target_freq, relation,
			&index)) {
		pr_err("cpufreq: invalid target_freq: %d\n", target_freq);
		ret = -EINVAL;
		goto done;
	}

	pr_debug("CPU[%d] target %d relation %d (%d-%d) selected %d\n",
		policy->cpu, target_freq, relation,
		policy->min, policy->max, table[index].frequency);

	/* The old rate needs time to settle before it can be changed again */
	delta_us = ktime_us_delta(ktime_get_boottime(), udata->last_update);
	if (delta_us < 10000)
		usleep_range(10000 - delta_us, 11000 - delta_us);
	udata->last_update = ktime_get_boottime();

	ret = set_cpu_freq(policy, table[index].frequency,
			   table[index].driver_data);
done:
	mutex_unlock(&udata->update_lock);
	mutex_unlock(&per_cpu(suspend_data, policy->cpu).suspend_mutex);
	return ret;
}

static int msm_cpufreq_verify(struct cpufreq_policy *policy)
{
	cpufreq_verify_within_limits(policy, policy->cpuinfo.min_freq,
			policy->cpuinfo.max_freq);
	return 0;
}

static unsigned int msm_cpufreq_get_freq(unsigned int cpu)
{
	return clk_get_rate(cpu_clk[cpu]) / 1000;
}

static int msm_cpufreq_init(struct cpufreq_policy *policy)
{
	int cur_freq;
	int index;
	int ret = 0;
	struct cpufreq_frequency_table *table =
			per_cpu(freq_table, policy->cpu);
	int cpu;

	/*
	 * In some SoC, some cores are clocked by same source, and their
	 * frequencies can not be changed independently. Find all other
	 * CPUs that share same clock, and mark them as controlled by
	 * same policy.
	 */
	for_each_possible_cpu(cpu)
		if (cpu_clk[cpu] == cpu_clk[policy->cpu])
			cpumask_set_cpu(cpu, policy->cpus);

	ret = cpufreq_table_validate_and_show(policy, table);
	if (ret) {
		pr_err("cpufreq: failed to get policy min/max\n");
		return ret;
	}

	cur_freq = clk_get_rate(cpu_clk[policy->cpu])/1000;

	if (cpufreq_frequency_table_target(policy, table, cur_freq,
	    CPUFREQ_RELATION_H, &index) &&
	    cpufreq_frequency_table_target(policy, table, cur_freq,
	    CPUFREQ_RELATION_L, &index)) {
		pr_info("cpufreq: cpu%d at invalid freq: %d\n",
				policy->cpu, cur_freq);
		return -EINVAL;
	}
	/*
	 * Call set_cpu_freq unconditionally so that when cpu is set to
	 * online, frequency limit will always be updated.
	 */
	ret = set_cpu_freq(policy, table[index].frequency,
			   table[index].driver_data);
	if (ret)
		return ret;
	pr_debug("cpufreq: cpu%d init at %d switching to %d\n",
			policy->cpu, cur_freq, table[index].frequency);
	policy->cur = table[index].frequency;

	return 0;
}

static int msm_cpufreq_cpu_callback(struct notifier_block *nfb,
		unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	int rc;

	/* Fail hotplug until this driver can get CPU clocks */
	if (!hotplug_ready)
		return NOTIFY_BAD;

	switch (action & ~CPU_TASKS_FROZEN) {

	case CPU_DYING:
		clk_disable(cpu_clk[cpu]);
		clk_disable(l2_clk);
		break;
	/*
	 * Scale down clock/power of CPU that is dead and scale it back up
	 * before the CPU is brought up.
	 */
	case CPU_DEAD:
		clk_unprepare(cpu_clk[cpu]);
		clk_unprepare(l2_clk);
		break;
	case CPU_UP_CANCELED:
		clk_unprepare(cpu_clk[cpu]);
		clk_unprepare(l2_clk);
		break;
	case CPU_UP_PREPARE:
		rc = clk_prepare(l2_clk);
		if (rc < 0)
			return NOTIFY_BAD;
		rc = clk_prepare(cpu_clk[cpu]);
		if (rc < 0) {
			clk_unprepare(l2_clk);
			return NOTIFY_BAD;
		}
		break;

	case CPU_STARTING:
		rc = clk_enable(l2_clk);
		if (rc < 0)
			return NOTIFY_BAD;
		rc = clk_enable(cpu_clk[cpu]);
		if (rc) {
			clk_disable(l2_clk);
			return NOTIFY_BAD;
		}
		break;

	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block __refdata msm_cpufreq_cpu_notifier = {
	.notifier_call = msm_cpufreq_cpu_callback,
};

static int msm_cpufreq_suspend(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		mutex_lock(&per_cpu(suspend_data, cpu).suspend_mutex);
		per_cpu(suspend_data, cpu).device_suspended = 1;
		mutex_unlock(&per_cpu(suspend_data, cpu).suspend_mutex);
	}

	return NOTIFY_DONE;
}

static int msm_cpufreq_resume(void)
{
	int cpu, ret;
	struct cpufreq_policy policy;

	for_each_possible_cpu(cpu) {
		per_cpu(suspend_data, cpu).device_suspended = 0;
	}

	/*
	 * Freq request might be rejected during suspend, resulting
	 * in policy->cur violating min/max constraint.
	 * Correct the frequency as soon as possible.
	 */
	get_online_cpus();
	for_each_online_cpu(cpu) {
		ret = cpufreq_get_policy(&policy, cpu);
		if (ret)
			continue;
		if (policy.cur <= policy.max && policy.cur >= policy.min)
			continue;
		ret = cpufreq_update_policy(cpu);
		if (ret)
			pr_info("cpufreq: Current frequency violates policy min/max for CPU%d\n",
			       cpu);
		else
			pr_info("cpufreq: Frequency violation fixed for CPU%d\n",
				cpu);
	}
	put_online_cpus();

	return NOTIFY_DONE;
}

static int msm_cpufreq_pm_event(struct notifier_block *this,
				unsigned long event, void *ptr)
{
	switch (event) {
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		return msm_cpufreq_resume();
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		return msm_cpufreq_suspend();
	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block msm_cpufreq_pm_notifier = {
	.notifier_call = msm_cpufreq_pm_event,
};

static struct freq_attr *msm_freq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static void msm_cpufreq_ready(struct cpufreq_policy *policy)
{
	struct device_node *np, *lmh_node;
	unsigned int cpu = 0;

	if (cdev[policy->cpu])
		return;

	np = of_cpu_device_node_get(policy->cpu);
	if (WARN_ON(!np))
		return;

	/*
	 * For now, just loading the cooling device;
	 * thermal DT code takes care of matching them.
	 */
	if (of_find_property(np, "#cooling-cells", NULL)) {
		lmh_node = of_parse_phandle(np, "qcom,lmh-dcvs", 0);
		if (lmh_node) {
			of_node_put(lmh_node);
			goto ready_exit;
		}

		for_each_cpu(cpu, policy->related_cpus) {
			cpumask_t cpu_mask  = CPU_MASK_NONE;

			of_node_put(np);
			np = of_cpu_device_node_get(cpu);
			if (WARN_ON(!np))
				return;

			cpumask_set_cpu(cpu, &cpu_mask);
			cdev[cpu] = of_cpufreq_cooling_register(np, &cpu_mask);
			if (IS_ERR(cdev[cpu])) {
				pr_err(
				"running cpufreq for CPU%d without cooling dev: %ld\n",
				cpu, PTR_ERR(cdev[cpu]));
				cdev[cpu] = NULL;
			}
		}
	}

ready_exit:
	of_node_put(np);
}

static struct cpufreq_driver msm_cpufreq_driver = {
	/* lps calculations are handled here. */
	.flags		= CPUFREQ_STICKY | CPUFREQ_CONST_LOOPS,
	.init		= msm_cpufreq_init,
	.verify		= msm_cpufreq_verify,
	.target		= msm_cpufreq_target,
	.get		= msm_cpufreq_get_freq,
	.name		= "msm",
	.attr		= msm_freq_attr,
	.ready		= msm_cpufreq_ready,
};

static struct cpufreq_frequency_table *cpufreq_parse_dt(struct device *dev,
						char *tbl_name, int cpu)
{
	int ret, nf, i, j;
	u32 *data;
	struct cpufreq_frequency_table *ftbl;

	/* Parse list of usable CPU frequencies. */
	if (!of_find_property(dev->of_node, tbl_name, &nf))
		return ERR_PTR(-EINVAL);
	nf /= sizeof(*data);

	if (nf == 0)
		return ERR_PTR(-EINVAL);

	data = devm_kzalloc(dev, nf * sizeof(*data), GFP_KERNEL);
	if (!data)
		return ERR_PTR(-ENOMEM);

	ret = of_property_read_u32_array(dev->of_node, tbl_name, data, nf);
	if (ret)
		return ERR_PTR(ret);

	ftbl = devm_kzalloc(dev, (nf + 1) * sizeof(*ftbl), GFP_KERNEL);
	if (!ftbl)
		return ERR_PTR(-ENOMEM);

	j = 0;
	for (i = 0; i < nf; i++) {
		unsigned long f;

		f = clk_round_rate(cpu_clk[cpu], data[i] * 1000);
		if (IS_ERR_VALUE(f))
			break;
		f /= 1000;

		/*
		 * Don't repeat frequencies if they round up to the same clock
		 * frequency.
		 *
		 */
		if (j > 0 && f <= ftbl[j - 1].frequency)
			continue;

		ftbl[j].driver_data = j;
		ftbl[j].frequency = f;
		j++;
	}

	ftbl[j].driver_data = j;
	ftbl[j].frequency = CPUFREQ_TABLE_END;

	devm_kfree(dev, data);

	return ftbl;
}

static int __init msm_cpufreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	char clk_name[] = "cpu??_clk";
	char tbl_name[] = "qcom,cpufreq-table-??";
	struct clk *c;
	int cpu;
	struct cpufreq_frequency_table *ftbl;

	l2_clk = devm_clk_get(dev, "l2_clk");
	if (IS_ERR(l2_clk))
		l2_clk = NULL;

	for_each_possible_cpu(cpu) {
		snprintf(clk_name, sizeof(clk_name), "cpu%d_clk", cpu);
		c = devm_clk_get(dev, clk_name);
		if (cpu == 0 && IS_ERR(c))
			return PTR_ERR(c);
		else if (IS_ERR(c))
			c = cpu_clk[cpu-1];
		cpu_clk[cpu] = c;
	}
	hotplug_ready = true;

	/* Use per-policy governor tunable for some targets */
	if (of_property_read_bool(dev->of_node, "qcom,governor-per-policy"))
		msm_cpufreq_driver.flags |= CPUFREQ_HAVE_GOVERNOR_PER_POLICY;

	/* Parse commong cpufreq table for all CPUs */
	ftbl = cpufreq_parse_dt(dev, "qcom,cpufreq-table", 0);
	if (!IS_ERR(ftbl)) {
		for_each_possible_cpu(cpu)
			per_cpu(freq_table, cpu) = ftbl;
		return 0;
	}

	/*
	 * No common table. Parse individual tables for each unique
	 * CPU clock.
	 */
	for_each_possible_cpu(cpu) {
		snprintf(tbl_name, sizeof(tbl_name),
			 "qcom,cpufreq-table-%d", cpu);
		ftbl = cpufreq_parse_dt(dev, tbl_name, cpu);

		/* CPU0 must contain freq table */
		if (cpu == 0 && IS_ERR(ftbl)) {
			dev_err(dev, "Failed to parse CPU0's freq table\n");
			return PTR_ERR(ftbl);
		}
		if (cpu == 0) {
			per_cpu(freq_table, cpu) = ftbl;
			continue;
		}

		if (cpu_clk[cpu] != cpu_clk[cpu - 1] && IS_ERR(ftbl)) {
			dev_err(dev, "Failed to parse CPU%d's freq table\n",
				cpu);
			return PTR_ERR(ftbl);
		}

		/* Use previous CPU's table if it shares same clock */
		if (cpu_clk[cpu] == cpu_clk[cpu - 1]) {
			if (!IS_ERR(ftbl)) {
				dev_warn(dev, "Conflicting tables for CPU%d\n",
					 cpu);
				devm_kfree(dev, ftbl);
			}
			ftbl = per_cpu(freq_table, cpu - 1);
		}
		per_cpu(freq_table, cpu) = ftbl;
	}

	return 0;
}

static struct of_device_id match_table[] = {
	{ .compatible = "qcom,msm-cpufreq" },
	{}
};

static struct platform_driver msm_cpufreq_plat_driver = {
	.driver = {
		.name = "msm-cpufreq",
		.of_match_table = match_table,
		.owner = THIS_MODULE,
	},
};

static int __init msm_cpufreq_register(void)
{
	int cpu, rc;

	for_each_possible_cpu(cpu) {
		mutex_init(&(per_cpu(suspend_data, cpu).suspend_mutex));
		per_cpu(suspend_data, cpu).device_suspended = 0;
		mutex_init(&cpu_update_data[cpu].update_lock);
	}

	rc = platform_driver_probe(&msm_cpufreq_plat_driver,
				   msm_cpufreq_probe);
	if (rc < 0) {
		/* Unblock hotplug if msm-cpufreq probe fails */
		unregister_hotcpu_notifier(&msm_cpufreq_cpu_notifier);
		for_each_possible_cpu(cpu)
			mutex_destroy(&(per_cpu(suspend_data, cpu).
					suspend_mutex));
		return rc;
	}

	register_pm_notifier(&msm_cpufreq_pm_notifier);
	return cpufreq_register_driver(&msm_cpufreq_driver);
}

subsys_initcall(msm_cpufreq_register);

static int __init msm_cpufreq_early_register(void)
{
	return register_hotcpu_notifier(&msm_cpufreq_cpu_notifier);
}
core_initcall(msm_cpufreq_early_register);
