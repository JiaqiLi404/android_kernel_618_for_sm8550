// SPDX-License-Identifier: GPL-2.0-only
/*
 * k618_mi_power.c, built under the stock name mi_power: /sys/class/power_debug/temp_aware as the
 * 13 Pro's own 5.15 mi_power.ko provides it.
 *
 * What the file is: a small value that user-space services share. PowerKeeper's low-temperature
 * endurance handler writes a mode there (it names this path for Qualcomm phones and
 * /sys/class/thermal/thermal_message/temp_aware for others); mi_thermald reads it to pick its
 * thermal configuration (the *-newcharger scene configs refer to it); CameraMind and libcameraopt
 * read it as well. The vendor's init.qcom.rc gives the file to system:system, and the vendor
 * file_contexts / system_ext genfscon label it sysfs_poweraware. 6.18 had no such file, so all of
 * those reads and writes failed.
 *
 * Stock behaviour (mi_power.ko: temp_aware_show 0x1cb0, temp_aware_store 0x1cf0, the value one
 * byte at .data+0x420):
 *   - the value starts at 10;
 *   - a write is parsed with kstrtoull(buf, 10): not a number is -EINVAL; 1..9 is stored; any
 *     other number is accepted (the write returns its length) and changes nothing;
 *   - a read prints the value with "%d\n".
 * Nothing in the kernel reads the value; only these two handlers touch it.
 *
 * Left out of the stock module, each for a reason:
 *   - power_mode, wakeup_device_list, subsystem_sleep_state: no reader or writer on this ROM;
 *   - debug_suspend: PowerKeeper writes it only from its developer debug-label settings; on stock
 *     it turns on a dump of registered regulator / clock / wakeup state at suspend, fed by other
 *     stock modules through exports this kernel's modules do not call;
 *   - the files of mi_power_device/qcom_sleep_stats: read by PowerKeeper's statistics collector and
 *     PowerInsight; on stock the numbers come from Xiaomi's modified soc_sleep_stats registering
 *     its data with mi_power. This kernel's soc_sleep_stats keeps the same numbers in debugfs.
 */
#define pr_fmt(fmt) "mi_power: " fmt

#include <linux/device/class.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sysfs.h>

static u8 temp_aware_mode = 10;		/* stock .data+0x420 */

static ssize_t temp_aware_show(const struct class *class, const struct class_attribute *attr,
			       char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(temp_aware_mode));
}

static ssize_t temp_aware_store(const struct class *class, const struct class_attribute *attr,
				const char *buf, size_t count)
{
	unsigned long long val;

	if (kstrtoull(buf, 10, &val))
		return -EINVAL;
	if (val >= 1 && val <= 9)
		WRITE_ONCE(temp_aware_mode, (u8)val);
	return count;
}
static CLASS_ATTR_RW(temp_aware);

static struct attribute *power_debug_attrs[] = {
	&class_attr_temp_aware.attr,
	NULL,
};
ATTRIBUTE_GROUPS(power_debug);

static struct class power_debug_class = {
	.name = "power_debug",
	.class_groups = power_debug_groups,
};

static int __init mi_power_init(void)
{
	int ret = class_register(&power_debug_class);

	if (ret)
		pr_err("class power_debug: %d\n", ret);
	else
		pr_info("/sys/class/power_debug/temp_aware (stock value 10)\n");
	return ret;
}

static void __exit mi_power_exit(void)
{
	class_unregister(&power_debug_class);
}

module_init(mi_power_init);
module_exit(mi_power_exit);
MODULE_DESCRIPTION("power_debug class: temp_aware, as the stock mi_power module");
MODULE_LICENSE("GPL");
