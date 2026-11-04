// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#include <linux/backlight.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

static char panel_name[128] = "mdss_dsi_m2_38_0c_0a_dsc_cmd";
module_param_string(panel_name, panel_name, sizeof(panel_name), 0444);
static char wp[64] = "0bba0c4d";
module_param_string(wp, wp, sizeof(wp), 0444);
static char build_id[64] = "0xA0";
module_param_string(build_id, build_id, sizeof(build_id), 0444);
static uint max_brightness_clone = 16383;
module_param(max_brightness_clone, uint, 0444);
static uint fps = 120;
module_param(fps, uint, 0444);
/* system:system, which is what init's rc gives these files */
static uint owner_uid = 1000;
module_param(owner_uid, uint, 0444);
static uint owner_gid = 1000;
module_param(owner_gid, uint, 0444);

/* what is read: the panel's name, its white point and its build id */
static ssize_t panel_info_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "panel_name=%s\n", panel_name);
}
static DEVICE_ATTR(panel_info, 0444, panel_info_show, NULL);

static ssize_t wp_info_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n", wp);
}
static DEVICE_ATTR(wp_info, 0444, wp_info_show, NULL);

static ssize_t panel_build_id_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%s\n", build_id);
}
static DEVICE_ATTR(panel_build_id, 0444, panel_build_id_show, NULL);

/* empty on this phone too */
static ssize_t cell_id_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "\n");
}
static DEVICE_ATTR(cell_id, 0444, cell_id_show, NULL);

static ssize_t dynamic_fps_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", fps);
}
static DEVICE_ATTR(dynamic_fps, 0444, dynamic_fps_show, NULL);

static ssize_t flatmode_check_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "1\n");
}
static DEVICE_ATTR(flatmode_check, 0444, flatmode_check_show, NULL);

/* the phone answers one line of the real timing; the period here follows fps */
static ssize_t hw_vsync_info_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "fps: %u.000 vsync_period_ns:%u panel_mode:dsi_cmd panel_type:primary\n",
			  fps, fps ? 1000000000U / fps : 0);
}
static DEVICE_ATTR(hw_vsync_info, 0444, hw_vsync_info_show, NULL);

/*
 * the phone's own counters, in the order and shape it prints them (a reader of this file looks for
 * its own name=value line, so the whole set is answered, with nothing counted here)
 */
static ssize_t disp_count_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf,
			  "panel_active=0\npanel_kickoff_count=0\nkernel_boottime=0\n"
			  "kernel_rtctime=0\nkernel_days=0\nbl_duration=0\nbl_level_integral=0\n"
			  "bl_highlevel_duration=0\nbl_lowlevel_duration=0\nhbm_duration=0\n"
			  "hbm_times=0\nfps1_times=0\nfps10_times=0\nfps24_times=0\nfps30_times=0\n"
			  "fps40_times=0\nfps48_times=0\nfps50_times=0\nfps60_times=0\nfps90_times=0\n"
			  "fps120_times=0\nfps144_times=0\npanel_id=1\npoweron_cost_avg=0\n"
			  "esd_times=0\nte_lost_times=0\nunderrun_times=0\noverflow_times=0\n"
			  "pingpong_timeout_times=0\ncommit_long_times=0\ncmdq_timeout_times=0\n"
			  "tpidle_times=0\nrecord_end=0\n");
}
static ssize_t disp_count_store(struct device *dev, struct device_attribute *a,
				const char *buf, size_t n)
{
	return n;
}
static DEVICE_ATTR(disp_count, 0664, disp_count_show, disp_count_store);

static ssize_t max_brightness_clone_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", max_brightness_clone);
}
static ssize_t max_brightness_clone_store(struct device *dev, struct device_attribute *a,
					  const char *buf, size_t n)
{
	return n;
}
static DEVICE_ATTR(max_brightness_clone, 0664, max_brightness_clone_show,
		   max_brightness_clone_store);

/* Device compatibility. */
static char bl_name[32] = "panel0-backlight";
module_param_string(bl_name, bl_name, sizeof(bl_name), 0444);
static uint min_level = 16;
module_param(min_level, uint, 0444);
static uint brightness_clone;
static ssize_t brightness_clone_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", brightness_clone);
}
static ssize_t brightness_clone_store(struct device *dev, struct device_attribute *a,
				      const char *buf, size_t n)
{
	struct backlight_device *bd;
	unsigned long level;
	int was, rc;
	uint v;

	if (kstrtouint(buf, 0, &v))
		return -EINVAL;
	brightness_clone = v;
	if (!max_brightness_clone)
		return n;
	bd = backlight_device_get_by_name(bl_name);
	if (!bd) {
		pr_emerg_ratelimited("k618_midisp: brightness_clone %u, no %s yet\n", v, bl_name);
		return n;
	}
	level = mult_frac((unsigned long)min(v, max_brightness_clone), bd->props.max_brightness,
			  max_brightness_clone);
	if (v && level < min_level)
		level = min_t(unsigned long, min_level, bd->props.max_brightness);
	was = bd->props.brightness;
	rc = backlight_device_set_brightness(bd, level);
	put_device(&bd->dev);
	pr_emerg_ratelimited("k618_midisp: brightness_clone %u: %s %d -> %lu (%d)\n", v, bl_name,
			     was, level, rc);
	return n;
}
static DEVICE_ATTR(brightness_clone, 0664, brightness_clone_show, brightness_clone_store);

static uint doze_brightness;
static ssize_t doze_brightness_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "%u\n", doze_brightness);
}
static ssize_t doze_brightness_store(struct device *dev, struct device_attribute *a,
				     const char *buf, size_t n)
{
	uint v;

	if (kstrtouint(buf, 0, &v))
		return -EINVAL;
	doze_brightness = v;
	return n;
}
static DEVICE_ATTR(doze_brightness, 0664, doze_brightness_show, doze_brightness_store);

/* the HAL writes feature commands here; they are logged and accepted, nothing acts on them */
static ssize_t disp_param_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "\t\tfeature name[feature id]: feature vaule\n"
			       "\t\t\t     dimming[00]: 0\n"
			       "\t\t\t\t hbm[01]: 0\n"
			       "\t\t\t     hbm_fod[02]: 0\n");
}
static ssize_t disp_param_store(struct device *dev, struct device_attribute *a,
				const char *buf, size_t n)
{
	pr_emerg("k618_midisp: disp_param '%.64s'\n", buf);
	return n;
}
static DEVICE_ATTR(disp_param, 0664, disp_param_show, disp_param_store);

/* the panel's debug registers: answered as nothing, writes dropped */
static ssize_t zero_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "0\n");
}
static ssize_t drop_store(struct device *dev, struct device_attribute *a,
			  const char *buf, size_t n)
{
	return n;
}
static DEVICE_ATTR(mipi_rw, 0664, zero_show, drop_store);
static DEVICE_ATTR(pps_rw, 0644, zero_show, drop_store);
static DEVICE_ATTR(dsi_phy_rw, 0644, zero_show, drop_store);
static DEVICE_ATTR(dsi_porch_rw, 0644, zero_show, drop_store);

static struct attribute *k618_midisp_attrs[] = {
	&dev_attr_panel_info.attr,
	&dev_attr_wp_info.attr,
	&dev_attr_panel_build_id.attr,
	&dev_attr_cell_id.attr,
	&dev_attr_dynamic_fps.attr,
	&dev_attr_flatmode_check.attr,
	&dev_attr_hw_vsync_info.attr,
	&dev_attr_disp_count.attr,
	&dev_attr_max_brightness_clone.attr,
	&dev_attr_brightness_clone.attr,
	&dev_attr_doze_brightness.attr,
	&dev_attr_disp_param.attr,
	&dev_attr_mipi_rw.attr,
	&dev_attr_pps_rw.attr,
	&dev_attr_dsi_phy_rw.attr,
	&dev_attr_dsi_porch_rw.attr,
	NULL,
};
ATTRIBUTE_GROUPS(k618_midisp);

static void k618_midisp_ownership(const struct device *dev, kuid_t *uid, kgid_t *gid)
{
	*uid = make_kuid(&init_user_ns, owner_uid);
	*gid = make_kgid(&init_user_ns, owner_gid);
}

static struct class k618_midisp_class = {
	.name = "mi_display",
	.get_ownership = k618_midisp_ownership,
};
static struct device *k618_midisp_parent, *k618_midisp_dev;

static int __init k618_midisp_init(void)
{
	int ret;

	ret = class_register(&k618_midisp_class);
	if (ret) {
		pr_err("k618_midisp: no class (%d): the phone's own driver may already have it\n", ret);
		return ret;
	}
	/* the parent the file's SELinux type is matched on, with no attributes of its own */
	k618_midisp_parent = device_create(&k618_midisp_class, NULL, MKDEV(0, 0), NULL,
					   "disp_feature");
	if (IS_ERR(k618_midisp_parent)) {
		ret = PTR_ERR(k618_midisp_parent);
		class_unregister(&k618_midisp_class);
		pr_err("k618_midisp: no parent device (%d)\n", ret);
		return ret;
	}
	k618_midisp_dev = device_create_with_groups(&k618_midisp_class, k618_midisp_parent,
						    MKDEV(0, 0), NULL, k618_midisp_groups,
						    "disp-DSI-0");
	if (IS_ERR(k618_midisp_dev)) {
		ret = PTR_ERR(k618_midisp_dev);
		device_unregister(k618_midisp_parent);
		class_unregister(&k618_midisp_class);
		pr_err("k618_midisp: no device (%d)\n", ret);
		return ret;
	}
	pr_emerg("k618_midisp: %s/disp_feature/disp-DSI-0 with panel_name '%s', wp '%s', build id '%s', files %u:%u\n",
		 "/sys/devices/virtual/mi_display", panel_name, wp, build_id, owner_uid, owner_gid);
	return 0;
}
module_init(k618_midisp_init);

static void __exit k618_midisp_exit(void)
{
	device_unregister(k618_midisp_dev);
	device_unregister(k618_midisp_parent);
	class_unregister(&k618_midisp_class);
}
module_exit(k618_midisp_exit);

MODULE_DESCRIPTION("k618: the mi_display sysfs interface this phone's display stack reads, and its brightness");
MODULE_LICENSE("GPL");
