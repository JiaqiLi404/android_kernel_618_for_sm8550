// SPDX-License-Identifier: GPL-2.0
/*
 * k618_suspendtest: QEMU only, never in a phone image. Asks for a system suspend (suspend-to-idle)
 * at delay_ms after it loads and prints what pm_suspend() answered -- to check k618_probe's suspend
 * watch on a boot that has no user space to ask for one: with suspend_veto=1 the probe must log the
 * attempt, write its log and refuse it (pm_suspend returns -EPERM), and the boot must go on.
 */
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/workqueue.h>

static unsigned int delay_ms = 1000;
module_param(delay_ms, uint, 0444);

static void k618_suspendtest_work(struct work_struct *w)
{
	int ret;

	pr_emerg("k618_suspendtest: asking for suspend-to-idle\n");
	ret = pm_suspend(PM_SUSPEND_TO_IDLE);
	pr_emerg("k618_suspendtest: pm_suspend returned %d\n", ret);
}
static DECLARE_DELAYED_WORK(k618_suspendtest_dw, k618_suspendtest_work);

static int __init k618_suspendtest_init(void)
{
	queue_delayed_work(system_unbound_wq, &k618_suspendtest_dw, msecs_to_jiffies(delay_ms));
	return 0;
}
module_init(k618_suspendtest_init);

static void __exit k618_suspendtest_exit(void)
{
	cancel_delayed_work_sync(&k618_suspendtest_dw);
}
module_exit(k618_suspendtest_exit);

MODULE_DESCRIPTION("6.18 bring-up, QEMU only: ask for a suspend to check the probe's suspend watch");
MODULE_LICENSE("GPL");
