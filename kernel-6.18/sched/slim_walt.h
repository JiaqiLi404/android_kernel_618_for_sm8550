/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_SCHED_SLIM_WALT_H
#define _KERNEL_SCHED_SLIM_WALT_H

#include <linux/types.h>

/*
 * The tick is sampled while somebody holds a reference. Both may be called with the CPU hotplug
 * lock held (a governor starts and stops in CPU hotplug): the tick hook itself is put in and taken
 * out by a work item.
 */
void slim_walt_get(void);
void slim_walt_put(void);

/*
 * How busy @cpu was in its last complete window, 0 to 1024 of its time; 0 also when that
 * is not known. @now is the scheduler's clock.
 */
unsigned long slim_walt_load(int cpu, u64 now);

#endif /* _KERNEL_SCHED_SLIM_WALT_H */
