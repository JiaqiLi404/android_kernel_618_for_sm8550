// SPDX-License-Identifier: GPL-2.0
/*
 * k618_badmod: a module whose init() fails, to prove k618_insmod on a run where nothing else does.
 *
 * Put in a list after k618_insmod (QEMU runs only -- the phone's batches have real failures of
 * their own), it is the one load that is certain to fail: insmod gets err=-EINVAL, k618_insmod says
 * "NOT LOADED .../k618_badmod.ko: err -22 (tolerated 1 ...)", and init walks on to the next module
 * instead of exiting. err= asks for another error; err=0 loads and proves nothing.
 */
#include <linux/module.h>

static int err = -EINVAL;
module_param(err, int, 0444);
/*
 * fault=1 goes one worse: the init() reads through a null pointer, as btpower and qcedev did on the
 * phone. Loaded by init that ends the boot (the fault is pid 1's); loaded by k618_load's child it
 * kills the child, and the batch goes on -- which is the thing to prove in QEMU before trusting it.
 */
static bool fault;
module_param(fault, bool, 0444);

static int __init k618_badmod_init(void)
{
	if (fault) {
		pr_emerg("k618_badmod: reading through a null pointer now\n");
		pr_emerg("k618_badmod: %d\n", *(volatile int *)16);
		return 0;
	}
	pr_emerg("k618_badmod: init returning %d\n", err);
	return err;
}
module_init(k618_badmod_init);

MODULE_DESCRIPTION("6.12 bring-up: a module that fails to load, on purpose");
MODULE_LICENSE("GPL");
