// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#define pr_fmt(fmt) "k618_mithermal: " fmt

#include <linux/cleanup.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#include <drm/drm_panel.h>

#include "k618_mi_thermal.h"

/* stock's buffer for the string-valued attributes (char[0x80] in its .bss) */
#define K618_STR_LEN 128

/*
 * cooling=0: do not register brightness0-clone. The thermal class comes from that cooling device, so
 * with cooling=0 we register a throwaway one, read the class off it and unregister it again -- that
 * way the sysfs interface still comes up and an A/B of "with and without the backlight cooling
 * device" costs nothing.
 */
static bool cooling = true;
module_param(cooling, bool, 0444);
/* brightness0-clone's max_state. Stock: 200 (and thermald-devices.conf's def_target:200). */
static unsigned int clone_levels = 200;
module_param(clone_levels, uint, 0444);
/* 0: never call the charging driver from board_sensor_temp, even when the symbol is there */
static bool charger_notify = true;
module_param(charger_notify, bool, 0644);
/* charger_notifier_call_chain() of whichever module exports it, if one does (symbol_get at load) */
static int (*k618_charger_notify_fn)(unsigned long val, int v);
/* how many times screen_state_check retries 5 s apart before giving up. Stock: 10 (.data+0x10). */
static int panel_retries = 10;
module_param(panel_retries, int, 0444);

/* ---------------------------------------------------------------- the stored values */

static int temp_state;				/* .bss+0x148 */
static int switch_mode = -1;			/* sconfig,      .data+0x488 = -1 */
static int balance_mode;			/* .bss+0x34c */
static int modem_limit;				/* .bss+0x350 */
static int wifi_limit;				/* .bss+0x354 */
static int flash_state;				/* .bss+0x358 */
static int torch_level;				/* .bss+0x35c */
static int torch_real_level = -1;		/* .data+0x48c = -1 */
static int thermal_max_brightness;		/* .bss+0x360 */
static int voice_limit;				/* .bss+0x364 */
static int market_download_limit;		/* .bss+0x368 */
static int board_sensor_temp_comp_default;	/* .bss+0x36c */
static int poor_modem_limit;			/* .bss+0x370 */
static int cpu_nolimit_temp_default;		/* .bss+0x374 */
static int modem_rate;				/* .bss+0x378 */
static int modem_level;				/* .bss+0x37c */
static int charger_mode = -1;			/* charger_temp, .data+0x490 = -1 */
static int powersave_mode = -1;			/* .data+0x4f0 = -1 */
static int power_level = -1;			/* .data+0x4f4 = -1 */

static char boost_buf[K618_STR_LEN];		/* .bss+0x14c */
static char board_sensor_temp[K618_STR_LEN];	/* .bss+0x1cc */
static char board_sensor_second_temp[K618_STR_LEN];	/* .bss+0x24c */
static char board_sensor_other_temp[K618_STR_LEN];	/* .bss+0x2cc */
static char ambient_sensor_temp[K618_STR_LEN];	/* .bss+0x380 */

static const char *board_sensor;		/* DT thermal-message/board-sensor, .bss+0x0 */

static int screen_light;			/* .bss+0x100 */
static int screen_state;			/* .bss+0x104 (u8 there) */
static int screen_last_status;			/* .bss+0x108 */
static int usb_online;				/* inside stock's usb_state, .bss+0xe8 */

static struct device *k618_thermal_dev;		/* /sys/class/thermal/thermal_message */
static struct device *k618_powersave_dev;	/* /sys/class/thermal/power_save */
static struct thermal_cooling_device *k618_clone_cdev;

/* ---------------------------------------------------------------- (2) cpu_limits / freq_qos */

struct k618_cpufreq_dev {
	struct list_head node;
	struct cpufreq_policy *policy;
	struct freq_qos_request qos_req;
	unsigned int cpu;		/* policy->cpu -- the N mi_thermald writes as "cpuN" */
	unsigned int max_level;		/* last index of freq_table */
	unsigned int cur_level;
	unsigned int *freq_table;	/* kHz, descending: [0] is the highest */
};

static LIST_HEAD(k618_cpufreq_list);
static DEFINE_MUTEX(k618_cpufreq_lock);

/* stock cpu_limits_set_level (0x3a0): first level whose frequency is <= freq, then update the QoS */
static void k618_cpu_set_limit(unsigned int cpu, unsigned int freq)
{
	struct k618_cpufreq_dev *cd;
	unsigned int level;

	guard(mutex)(&k618_cpufreq_lock);

	list_for_each_entry(cd, &k618_cpufreq_list, node) {
		if (cd->cpu != cpu)
			continue;

		for (level = 0; level <= cd->max_level; level++) {
			if (cd->freq_table[level] > freq)
				continue;
			if (cd->cur_level != level) {
				cd->cur_level = level;
				freq_qos_update_request(&cd->qos_req,
							cd->freq_table[level]);
			}
			return;
		}
		/* below the cluster's floor: stock leaves the limit as it is, and so do we */
		pr_debug("cpu%u: %u kHz is under the lowest level (%u kHz), ignored\n",
			 cpu, freq, cd->freq_table[cd->max_level]);
		return;
	}
	pr_debug("cpu%u: no cpufreq policy of that number\n", cpu);
}

static void k618_cpufreq_free(struct k618_cpufreq_dev *cd)
{
	freq_qos_remove_request(&cd->qos_req);
	cpufreq_cpu_put(cd->policy);
	kfree(cd->freq_table);
	kfree(cd);
}

/* one entry per cpufreq policy, with a descending frequency table (stock cpu_thermal_init) */
static int k618_cpufreq_add(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *pos;
	struct k618_cpufreq_dev *cd;
	unsigned int count = 0, i, prev;
	int ret;

	cpufreq_for_each_valid_entry(pos, policy->freq_table)
		count++;
	if (!count)
		return -EINVAL;

	cd = kzalloc(sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	cd->freq_table = kcalloc(count, sizeof(*cd->freq_table), GFP_KERNEL);
	if (!cd->freq_table) {
		kfree(cd);
		return -ENOMEM;
	}

	cd->policy = policy;
	cd->cpu = policy->cpu;
	cd->max_level = count - 1;

	/* descending, by repeated max-below-the-previous -- stock's own sort (0x1d4..0x238) */
	prev = UINT_MAX;
	for (i = 0; i < count; i++) {
		unsigned int best = 0;

		cpufreq_for_each_valid_entry(pos, policy->freq_table)
			if (pos->frequency < prev && pos->frequency > best)
				best = pos->frequency;
		if (!best) {
			/* duplicate entries in the table: stock warns and keeps the 0 */
			pr_warn("cpu%u: fewer than %u distinct frequencies\n", cd->cpu, count);
			cd->max_level = i ? i - 1 : 0;
			break;
		}
		cd->freq_table[i] = best;
		prev = best;
	}

	/* no limit to start with, i.e. the highest frequency (stock passes table[0] too) */
	ret = freq_qos_add_request(&policy->constraints, &cd->qos_req,
				   FREQ_QOS_MAX, cd->freq_table[0]);
	if (ret < 0) {
		pr_err("cpu%u: cannot add the freq constraint (%d)\n", cd->cpu, ret);
		kfree(cd->freq_table);
		kfree(cd);
		return ret;
	}

	scoped_guard(mutex, &k618_cpufreq_lock)
		list_add(&cd->node, &k618_cpufreq_list);

	pr_info("cpu%u: %u levels, %u..%u kHz\n", cd->cpu, cd->max_level + 1,
		cd->freq_table[cd->max_level], cd->freq_table[0]);
	return 0;
}

static void k618_cpufreq_init(void)
{
	struct k618_cpufreq_dev *cd;
	struct cpufreq_policy *policy;
	int cpu;
	bool have;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			/* stock stopped the whole loop here; we skip this CPU instead */
			pr_debug("no cpufreq policy for cpu%d\n", cpu);
			continue;
		}
		if (!policy->freq_table) {
			pr_warn("cpu%d: policy has no frequency table\n", cpu);
			cpufreq_cpu_put(policy);
			continue;
		}

		/* one entry per policy, keyed by policy->cpu (what mi_thermald writes) */
		have = false;
		scoped_guard(mutex, &k618_cpufreq_lock)
			list_for_each_entry(cd, &k618_cpufreq_list, node)
				if (cd->cpu == policy->cpu) {
					have = true;
					break;
				}
		if (have || k618_cpufreq_add(policy))
			cpufreq_cpu_put(policy);
	}
}

static void k618_cpufreq_exit(void)
{
	struct k618_cpufreq_dev *cd, *tmp;

	scoped_guard(mutex, &k618_cpufreq_lock)
		list_for_each_entry_safe(cd, tmp, &k618_cpufreq_list, node) {
			list_del(&cd->node);
			k618_cpufreq_free(cd);
		}
}

static ssize_t cpu_limits_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct k618_cpufreq_dev *cd;
	int len = 0;

	/* stock returns 0 bytes here; nothing reads it, so say something useful instead */
	guard(mutex)(&k618_cpufreq_lock);
	list_for_each_entry(cd, &k618_cpufreq_list, node)
		len += sysfs_emit_at(buf, len, "cpu%u %u\n", cd->cpu,
				     cd->freq_table[cd->cur_level]);
	return len;
}

static ssize_t cpu_limits_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int cpu, freq;

	if (sscanf(buf, "cpu%u %u", &cpu, &freq) != 2) {
		pr_err("cpu_limits: cannot parse \"%.32s\"\n", buf);
		return -EINVAL;
	}
	k618_cpu_set_limit(cpu, freq);
	return count;
}
static DEVICE_ATTR(cpu_limits, 0664, cpu_limits_show, cpu_limits_store);

/* ---------------------------------------------------------------- (7) the stored attributes */

#define K618_INT_ATTR(name, var, mode)						\
static ssize_t name##_show(struct device *dev,					\
			   struct device_attribute *attr, char *buf)		\
{										\
	return sysfs_emit(buf, "%d\n", var);					\
}										\
static ssize_t name##_store(struct device *dev, struct device_attribute *attr,	\
			    const char *buf, size_t count)			\
{										\
	int val;								\
										\
	if (kstrtoint(buf, 10, &val))						\
		return -EINVAL;							\
	var = val;								\
	return count;								\
}										\
static DEVICE_ATTR(name, mode, name##_show, name##_store)

#define K618_STR_ATTR(name, var, mode)						\
static ssize_t name##_show(struct device *dev,					\
			   struct device_attribute *attr, char *buf)		\
{										\
	return sysfs_emit(buf, "%s", var);					\
}										\
static ssize_t name##_store(struct device *dev, struct device_attribute *attr,	\
			    const char *buf, size_t count)			\
{										\
	strscpy(var, buf, sizeof(var));						\
	return count;								\
}										\
static DEVICE_ATTR(name, mode, name##_show, name##_store)

K618_INT_ATTR(temp_state, temp_state, 0664);
K618_INT_ATTR(sconfig, switch_mode, 0664);
K618_INT_ATTR(balance_mode, balance_mode, 0664);
K618_INT_ATTR(modem_limit, modem_limit, 0664);
K618_INT_ATTR(wifi_limit, wifi_limit, 0664);
K618_INT_ATTR(flash_state, flash_state, 0664);
K618_INT_ATTR(torch_level, torch_level, 0664);
K618_INT_ATTR(torch_real_level, torch_real_level, 0664);
K618_INT_ATTR(thermal_max_brightness, thermal_max_brightness, 0664);
K618_INT_ATTR(voice_limit, voice_limit, 0664);
K618_INT_ATTR(market_download_limit, market_download_limit, 0664);
K618_INT_ATTR(board_sensor_temp_comp, board_sensor_temp_comp_default, 0664);
K618_INT_ATTR(poor_modem_limit, poor_modem_limit, 0664);
K618_INT_ATTR(cpu_nolimit_temp, cpu_nolimit_temp_default, 0664);
K618_INT_ATTR(modem_rate, modem_rate, 0664);
K618_INT_ATTR(modem_level, modem_level, 0664);
K618_INT_ATTR(charger_temp, charger_mode, 0664);
K618_INT_ATTR(powersave_mode, powersave_mode, 0664);
K618_INT_ATTR(power_level, power_level, 0664);

/* boost is the one attribute stock gives 0644 (its dev_attr mode word at .data+0x1b0) */
K618_STR_ATTR(boost, boost_buf, 0644);
K618_STR_ATTR(board_sensor_second_temp, board_sensor_second_temp, 0664);
K618_STR_ATTR(board_sensor_other_temp, board_sensor_other_temp, 0664);
K618_STR_ATTR(ambient_sensor_temp, ambient_sensor_temp, 0664);

/* (6) board_sensor_temp: store the string, and hand whole degrees to the charging driver */
static ssize_t board_sensor_temp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s", board_sensor_temp);
}

static ssize_t board_sensor_temp_store(struct device *dev, struct device_attribute *attr,
				       const char *buf, size_t count)
{
	int millicelsius, ret;

	strscpy(board_sensor_temp, buf, sizeof(board_sensor_temp));

	if (charger_notify && k618_charger_notify_fn &&
	    !kstrtoint(buf, 10, &millicelsius)) {
		/* tenths of a degree, as stock (0x51eb851f, asr #37 at 0xb98..0xbb0: / 100) */
		ret = k618_charger_notify_fn(MI_CHARGER_EVENT_BOARD_TEMP, millicelsius / 100);
		if (ret)
			pr_err("board_sensor_temp: charger_notifier_call_chain: %d\n", ret);
	}
	return count;
}
static DEVICE_ATTR(board_sensor_temp, 0664, board_sensor_temp_show, board_sensor_temp_store);

/*
 * Read-only: the DT sensor name, the constant ambient sensor name, and the two states the kernel
 * owns. Stock gives all four mode 0664 with a NULL store, i.e. a write returns -EIO but an O_RDWR
 * open still succeeds -- which is why they are not DEVICE_ATTR_RO (0444) here: mi_thermald opens
 * several of these files O_RDWR whether it writes them or not.
 */
static ssize_t board_sensor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s", board_sensor ? board_sensor : "invalid");
}
static DEVICE_ATTR(board_sensor, 0664, board_sensor_show, NULL);

static ssize_t ambient_sensor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s", "ABT-SENSOR");
}
static DEVICE_ATTR(ambient_sensor, 0664, ambient_sensor_show, NULL);

static ssize_t screen_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", screen_state);
}
static DEVICE_ATTR(screen_state, 0664, screen_state_show, NULL);

static ssize_t usb_online_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", usb_online);
}
static DEVICE_ATTR(usb_online, 0664, usb_online_show, NULL);

/* the 27 files of thermal_message, in stock's own order (its attrs array at .data+0x48) */
static struct attribute *k618_thermal_message_attrs[] = {
	&dev_attr_temp_state.attr,
	&dev_attr_cpu_limits.attr,
	&dev_attr_sconfig.attr,
	&dev_attr_screen_state.attr,
	&dev_attr_boost.attr,
	&dev_attr_board_sensor.attr,
	&dev_attr_board_sensor_temp.attr,
	&dev_attr_board_sensor_second_temp.attr,
	&dev_attr_board_sensor_other_temp.attr,
	&dev_attr_balance_mode.attr,
	&dev_attr_modem_limit.attr,
	&dev_attr_wifi_limit.attr,
	&dev_attr_flash_state.attr,
	&dev_attr_torch_level.attr,
	&dev_attr_torch_real_level.attr,
	&dev_attr_thermal_max_brightness.attr,
	&dev_attr_voice_limit.attr,
	&dev_attr_market_download_limit.attr,
	&dev_attr_board_sensor_temp_comp.attr,
	&dev_attr_poor_modem_limit.attr,
	&dev_attr_cpu_nolimit_temp.attr,
	&dev_attr_modem_rate.attr,
	&dev_attr_modem_level.attr,
	&dev_attr_ambient_sensor.attr,
	&dev_attr_ambient_sensor_temp.attr,
	&dev_attr_charger_temp.attr,
	&dev_attr_usb_online.attr,
	NULL,
};
static const struct attribute_group k618_thermal_message_group = {
	.attrs = k618_thermal_message_attrs,
};
static const struct attribute_group *k618_thermal_message_groups[] = {
	&k618_thermal_message_group, NULL,
};

/* and the 2 of power_save (.data+0x498) */
static struct attribute *k618_power_save_attrs[] = {
	&dev_attr_powersave_mode.attr,
	&dev_attr_power_level.attr,
	NULL,
};
static const struct attribute_group k618_power_save_group = {
	.attrs = k618_power_save_attrs,
};
static const struct attribute_group *k618_power_save_groups[] = {
	&k618_power_save_group, NULL,
};

/* ---------------------------------------------------------------- (4) screen_state */

static struct workqueue_struct *k618_panel_wq;
static struct delayed_work k618_panel_dw;
static struct drm_panel *k618_prim_panel;
static void *k618_panel_cookie;
static int k618_panel_left;		/* stock's screen_state_check.retry_count, .data+0x10 = 10 */

/*
 * Stock screen_state_for_thermal_callback (0x6e0). screen_light's bit 1 is never set by anything
 * here (stock keeps the "& 2" from shared code that also handles a secondary panel), so screen_light
 * is 0 or 1 and screen_state is just "is the primary panel unblanked".
 */
static void k618_panel_event(enum panel_event_notifier_tag tag,
			     struct panel_event_notification *notification,
			     void *pvt_data)
{
	int state;

	if (!notification) {
		pr_err("panel event with no notification\n");
		return;
	}
	if (notification->notif_data.early_trigger)
		return;

	if (tag == PANEL_EVENT_NOTIFICATION_PRIMARY) {
		switch (notification->notif_type) {
		case DRM_PANEL_EVENT_UNBLANK:
			screen_light |= 1;
			break;
		case DRM_PANEL_EVENT_BLANK:
		case DRM_PANEL_EVENT_BLANK_LP:
			screen_light &= ~1;
			break;
		default:
			return;
		}
	}

	state = !!screen_light;
	screen_state = state;
	if (screen_last_status != state) {
		if (k618_thermal_dev)
			sysfs_notify(&k618_thermal_dev->kobj, NULL, "screen_state");
		screen_last_status = state;
	}
}

/*
 * the registered panel when none of thermal-screen's "panel" list is -- that list names the
 * 13 Pro's 38 0c 0a panel and a reference panel, not its second panel (42 02 0b), so on phones
 * with that panel screen_state never registered. Only the active panel is registered with drm.
 */
static struct drm_panel *k618_registered_panel(void)
{
	struct device_node *np;
	struct drm_panel *panel;

	for_each_node_with_property(np, "qcom,mdss-dsi-panel-name") {
		panel = of_drm_find_panel(np);
		if (!IS_ERR(panel)) {
			of_node_put(np);
			return panel;
		}
	}
	return NULL;
}

/* stock screen_state_check (0x448): find the panel, register, retry every 5 s while it is not up */
static void k618_panel_check(struct work_struct *work)
{
	struct device_node *np, *panel_np;
	struct drm_panel *panel;
	int count, i, client;

	np = of_find_node_by_name(NULL, "thermal-screen");
	if (!np) {
		pr_err("no DT node \"thermal-screen\"\n");
		k618_prim_panel = k618_registered_panel();
		if (!k618_prim_panel)
			goto retry;
		goto found;
	}

	count = of_count_phandle_with_args(np, "panel", NULL);
	for (i = 0; i < count; i++) {
		panel_np = of_parse_phandle(np, "panel", i);
		if (!panel_np)
			continue;
		panel = of_drm_find_panel(panel_np);
		of_node_put(panel_np);
		if (!IS_ERR(panel)) {
			k618_prim_panel = panel;
			break;
		}
		k618_prim_panel = NULL;
		if (PTR_ERR(panel) == -EPROBE_DEFER)
			pr_debug("panel %d of thermal-screen is not probed yet\n", i);
	}
	of_node_put(np);

	if (!k618_prim_panel) {
		k618_prim_panel = k618_registered_panel();
		if (k618_prim_panel)
			pr_info("screen_state: panel not in thermal-screen's list, the registered one taken\n");
	}
	if (!k618_prim_panel)
		goto retry;
found:
	if (k618_panel_cookie)
		return;

	/*
	 * The client handle is an index into the notifier's own array and a used slot gives -EEXIST,
	 * so start at the slot stock used and walk up if something else in this build took it
	 * (qti_amoled_ecm takes 2 and qti_battery_charger 7 in our soc tree).
	 */
	for (client = K618_PANEL_CLIENT; client < K618_PANEL_CLIENT_MAX; client++) {
		k618_panel_cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
								  client, k618_prim_panel,
								  k618_panel_event, NULL);
		if (!IS_ERR(k618_panel_cookie)) {
			pr_info("screen_state: panel events on client slot %d\n", client);
			return;
		}
		if (PTR_ERR(k618_panel_cookie) != -EEXIST)
			break;
	}
	pr_err("screen_state: cannot register for panel events (%ld) -- it will stay 0\n",
	       PTR_ERR(k618_panel_cookie));
	k618_panel_cookie = NULL;
	return;

retry:
	if (k618_panel_left > 0) {
		k618_panel_left--;
		queue_delayed_work(k618_panel_wq, &k618_panel_dw, msecs_to_jiffies(5000));
	} else {
		pr_err("screen_state: no primary panel, giving up -- it will stay 0\n");
	}
}

/* ---------------------------------------------------------------- (5) usb_online */

static struct work_struct k618_usb_work;
static struct power_supply *k618_usb_psy;

static void k618_usb_online_work(struct work_struct *work)
{
	union power_supply_propval val = { 0 };
	int ret;

	if (!k618_usb_psy) {
		k618_usb_psy = power_supply_get_by_name("usb");
		if (!k618_usb_psy)
			return;
	}

	ret = power_supply_get_property(k618_usb_psy, POWER_SUPPLY_PROP_ONLINE, &val);
	if (ret) {
		pr_err("usb online read error: %d\n", ret);
		return;
	}

	usb_online = val.intval;
	if (k618_thermal_dev)
		sysfs_notify(&k618_thermal_dev->kobj, NULL, "usb_online");
}

static int k618_usb_online_cb(struct notifier_block *nb, unsigned long val, void *v)
{
	struct power_supply *psy = v;

	if (psy && psy->desc && !strcmp(psy->desc->name, "usb"))
		queue_work(system_wq, &k618_usb_work);
	return NOTIFY_OK;
}

static struct notifier_block k618_usb_nb = {
	.notifier_call = k618_usb_online_cb,
};

/* ---------------------------------------------------------------- (3) brightness0-clone */

static unsigned long k618_clone_state;

static int k618_clone_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = clone_levels;
	return 0;
}

static int k618_clone_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = k618_clone_state;
	return 0;
}

static int k618_clone_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	if (state > clone_levels)
		return -EINVAL;
	if (state == k618_clone_state)
		return 0;

	k618_clone_state = state;
	/*
	 * Hold and publish only. On this ROM the dimming itself is the composer's: libsdmdal finds
	 * this node by scanning the cooling devices' "type" files for "brightness0-clone" and caps
	 * its own brightness_clone accordingly. A kernel-side clamp would have to sit in
	 * k618_mi_disp, which owns brightness_clone -- see k618_mi_thermal_notes.md.
	 */
	sysfs_notify(&cdev->device.kobj, NULL, "cur_state");
	return 0;
}

static const struct thermal_cooling_device_ops k618_clone_ops = {
	.get_max_state = k618_clone_get_max_state,
	.get_cur_state = k618_clone_get_cur_state,
	.set_cur_state = k618_clone_set_cur_state,
};

/* ---------------------------------------------------------------- init / exit */

/* stock of_parse_thermal_message: the DT node "thermal-message", string property "board-sensor" */
static void k618_parse_board_sensor(void)
{
	struct device_node *np;

	np = of_find_node_by_name(NULL, "thermal-message");
	if (!np || of_property_read_string(np, "board-sensor", &board_sensor)) {
		pr_err("cannot parse the thermal-message node's board-sensor\n");
		board_sensor = NULL;
	} else {
		pr_info("board sensor: %s\n", board_sensor);
	}
	of_node_put(np);
}

static int __init k618_mi_thermal_init(void)
{
	struct thermal_cooling_device *probe_cdev;
	const struct class *thermal_class;
	struct device_node *np;
	int ret;

	/*
	 * The thermal class, taken off a cooling device of our own, because 6.18's thermal_class is
	 * static and a second class_register("thermal") would collide -- and the class name has to be
	 * "thermal" or the nodes get no SELinux label (see the header comment).
	 */
	probe_cdev = thermal_cooling_device_register(cooling ? "brightness0-clone"
							     : "k618-thermal-probe",
						    NULL, &k618_clone_ops);
	if (IS_ERR(probe_cdev)) {
		ret = PTR_ERR(probe_cdev);
		pr_err("cannot register a cooling device (%d) -- is thermal_sys up?\n", ret);
		return ret;
	}
	thermal_class = probe_cdev->device.class;
	if (!thermal_class) {
		pr_err("the cooling device has no class\n");
		thermal_cooling_device_unregister(probe_cdev);
		return -ENODEV;
	}
	if (cooling) {
		k618_clone_cdev = probe_cdev;
		k618_clone_state = clone_levels;	/* stock sits at cur = max = 200 */
	} else {
		thermal_cooling_device_unregister(probe_cdev);
	}

	k618_thermal_dev = device_create_with_groups(thermal_class, NULL, 0, NULL,
						     k618_thermal_message_groups,
						     "thermal_message");
	if (IS_ERR(k618_thermal_dev)) {
		ret = PTR_ERR(k618_thermal_dev);
		pr_err("cannot create thermal_message (%d)\n", ret);
		k618_thermal_dev = NULL;
		goto err_cdev;
	}

	/*
	 * power_save. Stock looks for a DT node of that name and only logs when it is missing -- it
	 * creates the device either way (0x478: the error branch falls back into the create), so a DT
	 * without the node still gets powersave_mode and power_level, which is what init.target.rc
	 * chowns to system and miui-services reads.
	 */
	np = of_find_node_by_name(NULL, "power_save");
	if (!np)
		pr_info("no DT node \"power_save\" -- creating the node anyway, as stock does\n");
	of_node_put(np);

	k618_powersave_dev = device_create_with_groups(thermal_class, NULL, 0, NULL,
						       k618_power_save_groups, "power_save");
	if (IS_ERR(k618_powersave_dev)) {
		ret = PTR_ERR(k618_powersave_dev);
		pr_err("cannot create power_save (%d)\n", ret);
		k618_powersave_dev = NULL;
		goto err_dev;
	}

	k618_parse_board_sensor();
	k618_cpufreq_init();

	/* screen_state: the panel is not up this early, so look for it from a delayed work */
	k618_panel_left = panel_retries;
	k618_panel_wq = alloc_ordered_workqueue("k618_screen_state", WQ_MEM_RECLAIM);
	if (k618_panel_wq) {
		INIT_DELAYED_WORK(&k618_panel_dw, k618_panel_check);
		queue_delayed_work(k618_panel_wq, &k618_panel_dw, msecs_to_jiffies(5000));
	} else {
		pr_err("no workqueue for screen_state\n");
	}

	INIT_WORK(&k618_usb_work, k618_usb_online_work);
	ret = power_supply_reg_notifier(&k618_usb_nb);
	if (ret)
		pr_err("usb online notifier registration error: %d\n", ret);

	k618_charger_notify_fn = symbol_get(charger_notifier_call_chain);
	pr_info("thermal_message (%zu files) and power_save (%zu files) are up%s\n",
		ARRAY_SIZE(k618_thermal_message_attrs) - 1,
		ARRAY_SIZE(k618_power_save_attrs) - 1,
		k618_charger_notify_fn ? "" : ", without a charging driver to notify");
	return 0;

err_dev:
	device_unregister(k618_thermal_dev);
	k618_thermal_dev = NULL;
err_cdev:
	if (k618_clone_cdev) {
		thermal_cooling_device_unregister(k618_clone_cdev);
		k618_clone_cdev = NULL;
	}
	return ret;
}

static void __exit k618_mi_thermal_exit(void)
{
	if (k618_charger_notify_fn)
		symbol_put(charger_notifier_call_chain);
	if (k618_panel_cookie)
		panel_event_notifier_unregister(k618_panel_cookie);
	if (k618_panel_wq) {
		cancel_delayed_work_sync(&k618_panel_dw);
		destroy_workqueue(k618_panel_wq);
	}

	power_supply_unreg_notifier(&k618_usb_nb);
	cancel_work_sync(&k618_usb_work);
	if (k618_usb_psy)
		power_supply_put(k618_usb_psy);

	k618_cpufreq_exit();

	/* the devices before the cooling device: they hold the class the cooling device gave us */
	if (k618_powersave_dev)
		device_unregister(k618_powersave_dev);
	if (k618_thermal_dev)
		device_unregister(k618_thermal_dev);
	if (k618_clone_cdev)
		thermal_cooling_device_unregister(k618_clone_cdev);
}

module_init(k618_mi_thermal_init);
module_exit(k618_mi_thermal_exit);

MODULE_DESCRIPTION("k618: the Xiaomi thermal interface (thermal_message, power_save, brightness0-clone)");
MODULE_LICENSE("GPL v2");
