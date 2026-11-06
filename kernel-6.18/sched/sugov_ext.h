/* SPDX-License-Identifier: GPL-2.0 */
/*
 * schedutilmium (cpufreq_sugov_ext.c) as the rest of the scheduler sees it. Declarations only, so
 * that a unit including this one keeps its types -- and the CRCs of its exports -- as they are.
 */
#ifndef _KERNEL_SCHED_SUGOV_EXT_H
#define _KERNEL_SCHED_SUGOV_EXT_H

#include <linux/types.h>

struct cpufreq_policy;

#ifdef CONFIG_CPU_FREQ_GOV_SUGOV_EXT
/* is @policy run by schedutilmium? EAS takes it as it takes schedutil (sugov_is_governor()) */
bool sugov_ext_is_governor(struct cpufreq_policy *policy);
#else
static inline bool sugov_ext_is_governor(struct cpufreq_policy *policy)
{
	return false;
}
#endif

#endif /* _KERNEL_SCHED_SUGOV_EXT_H */
