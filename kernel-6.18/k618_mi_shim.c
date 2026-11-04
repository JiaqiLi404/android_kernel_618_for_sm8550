// SPDX-License-Identifier: GPL-2.0
/*
 * k618_mi_shim: what the 18 Pro Max's (madrid's) own 6.18 builds of Xiaomi's modules import from a
 * madrid module this phone does not load.
 *
 * madrid's WALT (sched-walt.ko) ends its schedule hook with a call to mi_dflt_schedule_hook(),
 * exported by mist.ko, Xiaomi's "soc performance tuner": a per-CPU check that, when mist has a
 * frequency request pending for this CPU, hands it to the DCVS callback mist registered. mist is
 * not loaded here -- it tunes madrid's own Xiaomi-modified memlat and bwmon, which call into it,
 * while this phone runs Qualcomm's (which do not), and it needs two exports this kernel does not
 * have (get_ddr_freq, clk_debug_mc_cc_get) and a different qcom_dcvs_update_votes. With no request
 * ever pending, the call does nothing, which is what this does: the function takes no arguments
 * (mist's reads the CPU from the current task) and the caller ignores its return.
 */
#include <linux/export.h>
#include <linux/module.h>

/* mist's own declaration; madrid publishes no header for it */
void mi_dflt_schedule_hook(void);

void mi_dflt_schedule_hook(void)
{
}
EXPORT_SYMBOL(mi_dflt_schedule_hook);

MODULE_DESCRIPTION("k618: the madrid exports Xiaomi's modules need from modules not loaded here");
MODULE_LICENSE("GPL");
