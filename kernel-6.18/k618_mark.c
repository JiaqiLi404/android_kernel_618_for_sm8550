// SPDX-License-Identifier: GPL-2.0
/*
 * k618_mark: a module that only waits. Copies of it (k618_mark01, k618_mark02, ...) put between
 * the first-stage modules of a test image turn the time a failed boot takes into a progress bar:
 * the phone keeps no log, but how long its screen stays black says how many marks were passed
 * (k618_bootimg.sh, MARKS=<modules between marks>).
 */
#include <linux/delay.h>
#include <linux/module.h>

static unsigned int ms = 5000;
module_param(ms, uint, 0444);

static int __init k618_mark_init(void)
{
	pr_info("%s: waiting %u ms\n", KBUILD_MODNAME, ms);
	/*
	 * an hrtimer, not msleep(): above 4096 jiffies the timer wheel rounds a timeout up by up to 512
	 * jiffies (2 s at HZ=250), which a 20 s mark showed in QEMU
	 */
	usleep_range((unsigned long)ms * USEC_PER_MSEC, (unsigned long)ms * USEC_PER_MSEC + 1000);
	return 0;
}
module_init(k618_mark_init);

MODULE_DESCRIPTION("6.12 bring-up: a wait between the first-stage modules");
MODULE_LICENSE("GPL");
