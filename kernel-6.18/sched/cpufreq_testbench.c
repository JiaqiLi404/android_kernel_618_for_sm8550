// SPDX-License-Identifier: GPL-2.0
/*
 * cpufreq_testbench -- a cpufreq driver without hardware, to try governors on.
 *
 * A machine like QEMU's has no cpufreq driver, and without one no governor ever runs. This
 * registers one that changes nothing but its own record of what it was told, for the boot
 * test of the schedutilmium governor (kernel-6.18/k618_sugov_test.c). It does nothing at all
 * unless the kernel is booted with
 *
 *   cpufreq_testbench.enable=1
 *
 * which no phone is: there the real driver is a module that loads later, and this one stays
 * out of its way.
 *
 * CPU 0 .. shared_from-1 get a policy each, the CPUs from shared_from (default 1) on share
 * one, so both of a governor's paths run. Nine steps from 300 MHz to 2 GHz. The fast switch
 * goes by the table entry the cpufreq core resolved last, not by the frequency it is called
 * with -- as Qualcomm's driver does (qcom_cpufreq_hw_fast_switch()), and that is the point: a
 * governor that picks a step of its own after resolving one gets, on such a driver, the
 * step it resolved. What the "hardware" was told is in policyN/testbench_hw_freq, how often
 * in policyN/testbench_switches, and how often the frequency it was called with was not the
 * one of that entry in policyN/testbench_mismatches. fast=0 leaves the fast path out (a
 * governor then switches from its kthread).
 *
 * 6.18 scales the CPUs' load by the frequency against the capacity's reference frequency
 * (capacity_freq_ref), which arch_topology sets only where the device tree gives the CPUs
 * capacities; without one it stays 0, and every switch would warn in topology_set_freq_scale().
 * The table's top is the reference then. em=1 registers an energy model for each policy as
 * well (power rising with the square of the frequency, no step inefficient), so that a boot
 * with a device tree that gives the CPUs different capacities has EAS -- with a governor EAS
 * takes.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/arch_topology.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/energy_model.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

static bool enable;
module_param(enable, bool, 0444);
static unsigned int shared_from = 1;
module_param(shared_from, uint, 0444);
static bool fast = true;
module_param(fast, bool, 0444);
static bool em;
module_param(em, bool, 0444);

static struct cpufreq_frequency_table testbench_table[] = {
	{ .frequency = 300000 },
	{ .frequency = 500000 },
	{ .frequency = 700000 },
	{ .frequency = 900000 },
	{ .frequency = 1100000 },
	{ .frequency = 1300000 },
	{ .frequency = 1500000 },
	{ .frequency = 1700000 },
	{ .frequency = 2000000 },
	{ .frequency = CPUFREQ_TABLE_END },
};

#define TESTBENCH_STEPS	(ARRAY_SIZE(testbench_table) - 1)
#define TESTBENCH_TOP	2000000

struct testbench_policy {
	unsigned int	index;		/* the table entry the "hardware" is at */
	unsigned long	switches;	/* how often it was told one */
	unsigned long	mismatches;	/* how often with another frequency than that entry's */
};

static unsigned int testbench_fast_switch(struct cpufreq_policy *policy, unsigned int target_freq)
{
	struct testbench_policy *tb = policy->driver_data;
	unsigned int index = policy->cached_resolved_idx;

	WRITE_ONCE(tb->index, index);
	WRITE_ONCE(tb->switches, tb->switches + 1);
	if (target_freq != policy->freq_table[index].frequency)
		WRITE_ONCE(tb->mismatches, tb->mismatches + 1);
	return policy->freq_table[index].frequency;
}

static int testbench_target_index(struct cpufreq_policy *policy, unsigned int index)
{
	struct testbench_policy *tb = policy->driver_data;

	WRITE_ONCE(tb->index, index);
	WRITE_ONCE(tb->switches, tb->switches + 1);
	return 0;
}

static unsigned int testbench_get(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);
	struct testbench_policy *tb = policy ? policy->driver_data : NULL;

	return tb ? testbench_table[READ_ONCE(tb->index)].frequency : 0;
}

static int testbench_init(struct cpufreq_policy *policy)
{
	struct testbench_policy *tb = kzalloc(sizeof(*tb), GFP_KERNEL);
	unsigned int cpu;

	if (!tb)
		return -ENOMEM;
	if (policy->cpu >= shared_from)
		for_each_possible_cpu(cpu)
			if (cpu >= shared_from)
				cpumask_set_cpu(cpu, policy->cpus);
	/* (the reference of the frequency scale where no device tree gave one) */
	for_each_cpu(cpu, policy->cpus)
		if (!per_cpu(capacity_freq_ref, cpu))
			per_cpu(capacity_freq_ref, cpu) = TESTBENCH_TOP;
	policy->driver_data = tb;
	policy->freq_table = testbench_table;
	policy->cpuinfo.transition_latency = 1000;
	policy->fast_switch_possible = fast;
	policy->dvfs_possible_from_any_cpu = true;
	return 0;
}

static void testbench_exit(struct cpufreq_policy *policy)
{
	kfree(policy->driver_data);
	policy->driver_data = NULL;
}

/* em=1: the lowest step at or above *@khz, and its power (microwatts: 0.1 W per GHz squared) */
static int testbench_power(struct device *dev, unsigned long *power, unsigned long *khz)
{
	unsigned int i;

	for (i = 0; i < TESTBENCH_STEPS; i++) {
		unsigned long mhz = testbench_table[i].frequency / 1000;

		if (testbench_table[i].frequency >= *khz) {
			*khz = testbench_table[i].frequency;
			*power = mhz * mhz / 10;
			return 0;
		}
	}
	return -EINVAL;
}

static void testbench_register_em(struct cpufreq_policy *policy)
{
	struct em_data_callback cb = EM_DATA_CB(testbench_power);
	int ret;

	if (!em)
		return;
	ret = em_dev_register_perf_domain(get_cpu_device(policy->cpu), TESTBENCH_STEPS, &cb,
					  policy->related_cpus, true);
	pr_info("energy model for CPUs %*pbl: %d\n", cpumask_pr_args(policy->related_cpus), ret);
}

static ssize_t show_testbench_hw_freq(struct cpufreq_policy *policy, char *buf)
{
	struct testbench_policy *tb = policy->driver_data;

	/* (a policy without a CPU online has no record) */
	if (!tb)
		return -ENODEV;
	return sysfs_emit(buf, "%u\n", testbench_table[READ_ONCE(tb->index)].frequency);
}
cpufreq_freq_attr_ro(testbench_hw_freq);

static ssize_t show_testbench_switches(struct cpufreq_policy *policy, char *buf)
{
	struct testbench_policy *tb = policy->driver_data;

	if (!tb)
		return -ENODEV;
	return sysfs_emit(buf, "%lu\n", READ_ONCE(tb->switches));
}
cpufreq_freq_attr_ro(testbench_switches);

static ssize_t show_testbench_mismatches(struct cpufreq_policy *policy, char *buf)
{
	struct testbench_policy *tb = policy->driver_data;

	if (!tb)
		return -ENODEV;
	return sysfs_emit(buf, "%lu\n", READ_ONCE(tb->mismatches));
}
cpufreq_freq_attr_ro(testbench_mismatches);

/*
 * No scaling_available_frequencies here: since 6.13 the cpufreq core creates it itself for every
 * policy with a frequency table, and a driver that lists it too fails to register (duplicate
 * sysfs file, -EEXIST from cpufreq_online()).
 */
static struct freq_attr *testbench_attrs[] = {
	&testbench_hw_freq,
	&testbench_switches,
	&testbench_mismatches,
	NULL,
};

static struct cpufreq_driver testbench_driver = {
	.name		= "testbench",
	.flags		= CPUFREQ_HAVE_GOVERNOR_PER_POLICY,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= testbench_target_index,
	.fast_switch	= testbench_fast_switch,
	.get		= testbench_get,
	.init		= testbench_init,
	.exit		= testbench_exit,
	.register_em	= testbench_register_em,
	.attr		= testbench_attrs,
};

static int __init testbench_register(void)
{
	int ret;

	if (!enable)
		return 0;
	ret = cpufreq_register_driver(&testbench_driver);
	pr_info("a cpufreq driver without hardware: %s (%d)\n", ret ? "not registered" : "registered", ret);
	return 0;
}
late_initcall(testbench_register);
