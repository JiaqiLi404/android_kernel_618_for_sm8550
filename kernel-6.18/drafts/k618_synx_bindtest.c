// SPDX-License-Identifier: GPL-2.0
/*
 * QEMU-only stand-in for the camera's CSL bind ops (camera 13), so the synx v1 SYNX_BIND path
 * (k618_tp_fix.py SYNX_V1) can be driven end to end without a camera: register_callback keeps what
 * synx hands it and shows the sync object id it was given (last_sync_obj), and writing fire calls
 * synx back the way a CSL fence's signal would. Loaded by k618_synx_v1_qemu.sh.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <synx_api.h>

static synx_callback k618_cb;
static void *k618_cb_data;

static int last_sync_obj = -1;
module_param(last_sync_obj, int, 0444);

static int k618_reg(synx_callback cb_func, void *userdata, s32 sync_obj)
{
	k618_cb = cb_func;
	k618_cb_data = userdata;
	last_sync_obj = sync_obj;
	pr_info("k618_synx_bindtest: register_callback sync_obj %d\n", sync_obj);
	return 0;
}

static int k618_dereg(synx_callback cb_func, void *userdata, s32 sync_obj)
{
	pr_info("k618_synx_bindtest: deregister_callback sync_obj %d\n", sync_obj);
	if (cb_func == k618_cb && userdata == k618_cb_data)
		k618_cb = NULL;
	return 0;
}

static int k618_enable(s32 sync_obj)
{
	return 0;
}

static int k618_signal(s32 sync_obj, u32 status)
{
	pr_info("k618_synx_bindtest: signal sync_obj %d status %u\n", sync_obj, status);
	return 0;
}

static int k618_fire_set(const char *val, const struct kernel_param *kp)
{
	synx_callback cb = k618_cb;

	if (!cb)
		return -ENOENT;
	k618_cb = NULL;
	cb(last_sync_obj, SYNX_STATE_SIGNALED_SUCCESS, k618_cb_data);
	return 0;
}

static const struct kernel_param_ops k618_fire_ops = {
	.set = k618_fire_set,
};
module_param_cb(fire, &k618_fire_ops, NULL, 0200);

static struct synx_register_params k618_params = {
	.name = "k618_synx_bindtest",
	.type = SYNX_TYPE_CSL,
	.ops = {
		.register_callback = k618_reg,
		.deregister_callback = k618_dereg,
		.enable_signaling = k618_enable,
		.signal = k618_signal,
	},
};

static int __init k618_bindtest_init(void)
{
	return synx_register_ops(&k618_params);
}

static void __exit k618_bindtest_exit(void)
{
	synx_deregister_ops(&k618_params);
}

module_init(k618_bindtest_init);
module_exit(k618_bindtest_exit);
MODULE_LICENSE("GPL");
