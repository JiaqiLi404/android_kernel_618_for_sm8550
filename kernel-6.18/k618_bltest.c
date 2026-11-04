// SPDX-License-Identifier: GPL-2.0
/*
 * k618_bltest: QEMU only, never in a phone image. Registers a backlight device named and sized like
 * the phone display driver's (panel0-backlight, raw, maximum 4095, starting at the maximum as
 * sde_backlight_setup starts it), whose update_status prints what it is asked to show; then writes
 * what the display HAL writes -- numbers into /sys/class/mi_display/disp-DSI-0/brightness_clone --
 * to check k618_midisp's way from there to the backlight: the scaling (16383 -> 4095), the smallest
 * non-zero value, a value past the top, a write while the panel is off (kept, not shown), and the
 * level coming back when the panel does (what the display driver's bridge enable does).
 */
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/workqueue.h>

static unsigned int delay_ms = 1000;
module_param(delay_ms, uint, 0444);

static struct backlight_device *k618_bltest_bd;

static int k618_bltest_update(struct backlight_device *bd)
{
	pr_emerg("k618_bltest: update_status brightness %d power %d -> shown %d\n",
		 bd->props.brightness, bd->props.power, backlight_get_brightness(bd));
	return 0;
}

static const struct backlight_ops k618_bltest_ops = {
	.update_status = k618_bltest_update,
};

static void k618_bltest_write(const char *val)
{
	struct file *f = filp_open("/sys/class/mi_display/disp-DSI-0/brightness_clone", O_WRONLY, 0);
	loff_t pos = 0;
	ssize_t n;

	if (IS_ERR(f)) {
		pr_emerg("k618_bltest: brightness_clone: open %ld\n", PTR_ERR(f));
		return;
	}
	n = kernel_write(f, val, strlen(val), &pos);
	filp_close(f, NULL);
	pr_emerg("k618_bltest: wrote %s -> %zd, backlight %d\n", val, n,
		 k618_bltest_bd->props.brightness);
}

static void k618_bltest_work(struct work_struct *w)
{
	struct backlight_device *bd = k618_bltest_bd;

	k618_bltest_write("822");	/* Device compatibility. */
	k618_bltest_write("16383");	/* the top: 4095 */
	k618_bltest_write("1");		/* the smallest non-zero: the panel's floor, 16 */
	k618_bltest_write("15");	/* Device compatibility. */
	k618_bltest_write("400");	/* above the floor: 99 */
	k618_bltest_write("0");		/* 0 */
	k618_bltest_write("99999");	/* past the top: 4095 */
	/* the panel goes off as sde_connector_helper_bridge_disable leaves it ... */
	bd->props.power = FB_BLANK_POWERDOWN;
	bd->props.state |= BL_CORE_FBBLANK;
	backlight_update_status(bd);
	k618_bltest_write("4096");	/* kept (1023), shown 0 */
	/* ... and comes back as sde_connector_helper_bridge_enable brings it: 1023 shown */
	bd->props.power = FB_BLANK_UNBLANK;
	bd->props.state &= ~BL_CORE_FBBLANK;
	backlight_update_status(bd);
	pr_emerg("k618_bltest: done, backlight %d\n", bd->props.brightness);
}
static DECLARE_DELAYED_WORK(k618_bltest_dw, k618_bltest_work);

static int __init k618_bltest_init(void)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 4095,
		.brightness = 4095,
		.power = FB_BLANK_UNBLANK,
	};

	k618_bltest_bd = backlight_device_register("panel0-backlight", NULL, NULL,
						   &k618_bltest_ops, &props);
	if (IS_ERR(k618_bltest_bd))
		return PTR_ERR(k618_bltest_bd);
	queue_delayed_work(system_unbound_wq, &k618_bltest_dw, msecs_to_jiffies(delay_ms));
	return 0;
}
module_init(k618_bltest_init);

static void __exit k618_bltest_exit(void)
{
	cancel_delayed_work_sync(&k618_bltest_dw);
	backlight_device_unregister(k618_bltest_bd);
}
module_exit(k618_bltest_exit);

MODULE_DESCRIPTION("6.18 bring-up, QEMU only: a stand-in panel backlight to check k618_midisp's brightness");
MODULE_LICENSE("GPL");
/* filp_open: the GKI keeps it for filesystems; a QEMU-only test may say it means to use it */
MODULE_IMPORT_NS("ANDROID_GKI_VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
