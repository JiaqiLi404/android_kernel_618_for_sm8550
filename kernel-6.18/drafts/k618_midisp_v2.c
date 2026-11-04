// SPDX-License-Identifier: GPL-2.0
/*
 * k618_midisp_v2 (a.k.a. mi_disp_k618): the Xiaomi display extension this phone's userspace drives
 * brightness and panel modes through, ported from the 12 Pro's published mi_disp (MiCode zeus-s-oss
 * techpack/display-drivers/msm/mi_disp) and trimmed to what fixes brightness / colour / flicker.
 *
 * It is compiled INTO msm_drm (display-drivers/msm/), not loaded as a separate first-stage module:
 * the event device and the display-parameter path both have to send DSI command sets on the live
 * panel under panel_lock, which only code inside msm_drm can do. The old stand-in kernel-6.18/
 * k618_midisp.c (first stage, sysfs only) is retired once this lands -- a second "mi_display" class
 * would collide, so k618_midisp must be dropped from the first-stage list (k618_timg.sh MIDISP=).
 *
 * What it provides, matching the ABI the 13 Pro's own closed msm_drm.ko exposes (confirmed against
 * /vendor_dlkm/lib/modules/msm_drm.ko and the userspace that drives it -- see k618_midisp_notes.md):
 *
 *  (1) the char device /dev/mi_display/disp_feature (class "mi_display", devnode
 *      "mi_display/disp_feature"), with the ioctl ABI version 0x0100 and:
 *        0x00 VERSION, 0x01 SET_FEATURE, 0x02/0x03 SET/GET_DOZE_BRIGHTNESS, 0x04 GET_PANEL_INFO,
 *        0x05 GET_WP_INFO, 0x06 GET_FPS, 0x07/0x08 (DE)REGISTER_EVENT, 0x09 WRITE_DSI_CMD,
 *        0x0A READ_DSI_CMD, 0x0B GET_BRIGHTNESS, plus the 13 Pro / MI-SF extras 0x0D..0x12 answered
 *        sanely. poll()/read() deliver struct disp_event + payload; MiBrightness subscribes to
 *        MI_DISP_EVENT_BRIGHTNESS_CLONE (type 5) and MI-SF to POWER/DOZE/FOD/51_brightness.
 *
 *  (2) the display-parameter path (SET_FEATURE and the disp_param sysfs node): dimming on/off,
 *      HBM on/off and flat mode on/off send the panel's own DT command sets on the active panel.
 *      Those command sets live per timing node under qcom,mdss-dsi-display-timings with the same
 *      blob format as qcom,mdss-dsi-on-command; k618_mi_disp_edits.py adds them to enum
 *      dsi_cmd_set_type + cmd_set_prop_map[]/cmd_set_state_map[] so Qualcomm's own per-mode parser
 *      reads them into priv_info->cmd_sets[], and we send them with dsi_panel_tx_cmd_set().
 *      Feature ids we do not act on (FOD / LHBM / AOD / count / round-corner / MI-SF notify ids)
 *      are accepted and stored so the HAL's setDispParam / flat-mode / MI-SF ioctls stop failing.
 *
 *  (3) panel-on dimming: panel enable fires the POWER event and, panel_on_dimming_delay ms later,
 *      sends dimming-on (stock mi,panel-on-dimming-delay = 120).
 *
 * Brightness, and why there is no double write: on this ROM SDM (libsdmdal, in the composer) writes
 * /sys/class/mi_display/disp-DSI-0/brightness_clone (0..max_brightness_clone). mi_disp turns that
 * into a BRIGHTNESS_CLONE event; the displayfeature HAL's MiBrightness (/odm/lib64/libmiBrightness
 * .so) reads the event, maps clone -> panel DBV with its own curve, and writes /sys/class/backlight/
 * panel0-backlight/brightness -- which is what drives the 51h register. So the clone store here only
 * stores the value and emits the event; it does NOT touch the backlight. SDE's bridge-enable restore
 * (backlight_update_status) re-sends the level MiBrightness last wrote, as stock. A fallback keeps
 * the old k618_midisp behaviour (write the backlight directly) ONLY while no client is subscribed to
 * the clone event -- so a boot where the HAL never comes up is not black, and the fallback disables
 * itself the moment MiBrightness subscribes.
 */
#define pr_fmt(fmt) "k618_midisp: " fmt

#include <linux/backlight.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "dsi_display.h"
#include "dsi_panel.h"
#include "mi_disp_k618.h"

/* ---- the ABI, lifted from the 12 Pro's uapi mi_disp.h (same on the 13 Pro; see notes) ---- */

#define MI_DISP_PRIMARY   0
#define MI_DISP_SECONDARY 1
#define MI_DISP_MAX       2

#define MI_DISP_EVENT_POWER            0
#define MI_DISP_EVENT_BACKLIGHT        1
#define MI_DISP_EVENT_FOD              2
#define MI_DISP_EVENT_DOZE             3
#define MI_DISP_EVENT_FPS              4
#define MI_DISP_EVENT_BRIGHTNESS_CLONE 5
#define MI_DISP_EVENT_51_BRIGHTNESS    6
#define MI_DISP_EVENT_MAX              32   /* 13 Pro has more than the 12 Pro's 7; evbit is sized for it */

#define DISP_FEATURE_DIMMING   0
#define DISP_FEATURE_HBM       1
#define DISP_FEATURE_HBM_FOD   2
#define DISP_FEATURE_FLAT_MODE 6
#define DISP_FEATURE_MAX       48   /* accept-and-store ceiling for ids we do not act on */

#define MI_DISP_FLAG_NONBLOCK 0x0001

#define MI_DISP_FEATURE_VERSION 0x0100   /* major 1, minor 0 */

struct disp_base { __u32 flag; __s32 disp_id; };
struct disp_version { struct disp_base base; __u32 version; };
struct disp_feature_req {
	struct disp_base base;
	__u32 feature_id;
	__s32 feature_val;
	__u32 tx_len;
	__u64 tx_ptr;
	__u32 rx_len;
	__u64 rx_ptr;
};
struct disp_doze_brightness_req { struct disp_base base; __u32 doze_brightness; };
struct disp_brightness_req { struct disp_base base; __u32 brightness; __u32 brightness_clone; };
struct disp_panel_info { struct disp_base base; __u32 info_len; char __user *info; };
struct disp_wp_info { struct disp_base base; __u32 info_len; char __user *info; };
/* the 13 Pro's disp_fps_info is 28 bytes (its GET_FPS ioctl size is 0x1c); extra words are 0 here */
struct disp_fps_info { struct disp_base base; __u32 fps; __u32 reserved[4]; };
struct disp_event_req { struct disp_base base; __u32 type; };
struct disp_event { __s32 disp_id; __u32 type; __u32 length; };
struct disp_event_resp { struct disp_event base; __u8 data[]; };
struct disp_dsi_cmd_req {
	struct disp_base base;
	__u8  tx_state;
	__u32 tx_len;
	__u64 tx_ptr;
	__u8  rx_state;
	__u32 rx_len;
	__u64 rx_ptr;
};

#define MI_DISP_IOCTL_VERSION             _IOR('D', 0x00, struct disp_version)
#define MI_DISP_IOCTL_SET_FEATURE        _IOWR('D', 0x01, struct disp_feature_req)
#define MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS _IOW('D', 0x02, struct disp_doze_brightness_req)
#define MI_DISP_IOCTL_GET_DOZE_BRIGHTNESS _IOR('D', 0x03, struct disp_doze_brightness_req)
#define MI_DISP_IOCTL_GET_PANEL_INFO     _IOWR('D', 0x04, struct disp_panel_info)
#define MI_DISP_IOCTL_GET_WP_INFO        _IOWR('D', 0x05, struct disp_wp_info)
#define MI_DISP_IOCTL_GET_FPS             _IOR('D', 0x06, struct disp_fps_info)
#define MI_DISP_IOCTL_REGISTER_EVENT      _IOW('D', 0x07, struct disp_event_req)
#define MI_DISP_IOCTL_DEREGISTER_EVENT    _IOW('D', 0x08, struct disp_event_req)
#define MI_DISP_IOCTL_WRITE_DSI_CMD       _IOW('D', 0x09, struct disp_dsi_cmd_req)
#define MI_DISP_IOCTL_READ_DSI_CMD       _IOWR('D', 0x0A, struct disp_dsi_cmd_req)
#define MI_DISP_IOCTL_GET_BRIGHTNESS      _IOR('D', 0x0B, struct disp_brightness_req)

#define MI_DISP_IOCTL_NR_MAX 0x13          /* covers the 13 Pro extras up to setDpInRequest (0x12) */

/* ---- parameters, read off the running 5.15 panel ---- */

static char panel_name[128] = "mdss_dsi_m2_38_0c_0a_dsc_cmd";
module_param_string(panel_name, panel_name, sizeof(panel_name), 0444);
static char wp[64] = "0bba0c4d";
module_param_string(wp, wp, sizeof(wp), 0444);
static char build_id[64] = "0xA0";
module_param_string(build_id, build_id, sizeof(build_id), 0444);
static uint max_brightness_clone = 16383;   /* mi,max-brightness-clone = 0x3fff */
module_param(max_brightness_clone, uint, 0444);
static uint fps = 120;
module_param(fps, uint, 0444);
static uint owner_uid = 1000;                /* system:system, as init's rc chowns these files */
module_param(owner_uid, uint, 0444);
static uint owner_gid = 1000;
module_param(owner_gid, uint, 0444);
/* stock mi,panel-on-dimming-delay = 0x78 */
static uint panel_on_dimming_delay = 120;
module_param(panel_on_dimming_delay, uint, 0644);
/* clone -> DBV fallback used ONLY while nobody is subscribed to the clone event */
static char bl_name[32] = "panel0-backlight";
module_param_string(bl_name, bl_name, sizeof(bl_name), 0444);
static uint min_level = 16;                  /* the panel's MinDBV */
module_param(min_level, uint, 0444);

/* ---- state ---- */

struct k618_client {
	struct list_head link;
	wait_queue_head_t event_wait;
	unsigned long evbit[MI_DISP_MAX][BITS_TO_LONGS(MI_DISP_EVENT_MAX)];
	struct list_head event_list;
	struct mutex event_lock;
	u32 event_space;
};

struct k618_pending {
	struct list_head link;
	struct disp_event_resp event;    /* followed in-place by payload bytes */
};

struct k618_mi {
	dev_t dev_id;
	struct cdev cdev;
	struct class *class;
	struct device *pdev;             /* /dev + /sys .../disp_feature */
	struct device *ddev[MI_DISP_MAX]; /* .../disp_feature/disp-DSI-<n> */
	struct dsi_display *disp[MI_DISP_MAX];
	struct list_head client_list;
	spinlock_t client_lock;
	atomic_t clone_subs;             /* clients subscribed to BRIGHTNESS_CLONE */
	struct delayed_work dimming_work[MI_DISP_MAX];
	u32 brightness_clone[MI_DISP_MAX];
	u32 doze_brightness[MI_DISP_MAX];
	int feature_val[MI_DISP_MAX][DISP_FEATURE_MAX];
};

static struct k618_mi *g_mi;

static inline bool disp_ok(int id) { return id >= 0 && id < MI_DISP_MAX; }

/* ---- event delivery ---- */

static void k618_notify(int disp_id, u32 type, u32 len, const void *payload)
{
	struct k618_mi *mi = g_mi;
	struct k618_client *c;
	unsigned long flags;

	if (!mi || !disp_ok(disp_id) || type >= MI_DISP_EVENT_MAX || !len || !payload)
		return;

	spin_lock_irqsave(&mi->client_lock, flags);
	list_for_each_entry(c, &mi->client_list, link) {
		struct k618_pending *n;
		u32 total = sizeof(struct disp_event_resp) + len;

		if (!test_bit(type, c->evbit[disp_id]))
			continue;
		if (c->event_space < sizeof(*n) + len)
			continue;
		n = kzalloc(sizeof(*n) + len, GFP_ATOMIC);
		if (!n)
			continue;
		n->event.base.disp_id = disp_id;
		n->event.base.type = type;
		n->event.base.length = total;      /* header (12) + payload, what read() returns */
		memcpy(n->event.data, payload, len);
		c->event_space -= total;
		list_add_tail(&n->link, &c->event_list);
		wake_up_interruptible(&c->event_wait);
	}
	spin_unlock_irqrestore(&mi->client_lock, flags);
}

static void k618_notify_u32(int disp_id, u32 type, u32 val)
{
	k618_notify(disp_id, type, sizeof(val), &val);
}

/* exported-to-the-rest-of-msm_drm hooks (prototypes in mi_disp_k618.h) */

void k618_mi_disp_event(int disp_id, u32 type, u32 len, u64 val)
{
	k618_notify(disp_id, type, len, &val);
}

/* ---- the display-parameter path ---- */

static int k618_tx(struct dsi_display *d, enum dsi_cmd_set_type type)
{
	struct dsi_panel *p;
	int rc;

	if (!d || !d->panel)
		return -ENODEV;
	p = d->panel;
	mutex_lock(&p->panel_lock);
	if (!p->panel_initialized) {
		mutex_unlock(&p->panel_lock);
		return -ENODEV;
	}
	rc = dsi_panel_tx_cmd_set(p, type, false);
	mutex_unlock(&p->panel_lock);
	return rc;
}

static int k618_set_feature(int disp_id, u32 feature_id, int val)
{
	struct k618_mi *mi = g_mi;
	struct dsi_display *d;
	int rc = 0;

	if (!mi || !disp_ok(disp_id))
		return -EINVAL;
	d = mi->disp[disp_id];
	if (!d)
		return -ENODEV;

	switch (feature_id) {
	case DISP_FEATURE_DIMMING:
		rc = k618_tx(d, val ? DSI_CMD_SET_MI_DIMMINGON : DSI_CMD_SET_MI_DIMMINGOFF);
		break;
	case DISP_FEATURE_HBM:
		rc = k618_tx(d, val ? DSI_CMD_SET_MI_HBM_ON : DSI_CMD_SET_MI_HBM_OFF);
		break;
	case DISP_FEATURE_FLAT_MODE:
		rc = k618_tx(d, val ? DSI_CMD_SET_MI_FLAT_MODE_ON : DSI_CMD_SET_MI_FLAT_MODE_OFF);
		break;
	default:
		/* FOD / LHBM / AOD / count / MI-SF notify ids: accept so the HAL stops failing */
		break;
	}
	if (feature_id < DISP_FEATURE_MAX)
		mi->feature_val[disp_id][feature_id] = val;
	pr_debug("disp%d feature %u = %d (rc %d)\n", disp_id, feature_id, val, rc);
	return rc;
}

/* clone -> DBV fallback, only while MiBrightness is not subscribed (see file header) */
static void k618_clone_fallback(int disp_id, u32 clone)
{
	struct backlight_device *bd;
	unsigned long level;

	if (atomic_read(&g_mi->clone_subs) || !max_brightness_clone)
		return;
	bd = backlight_device_get_by_name(bl_name);
	if (!bd)
		return;
	level = mult_frac((unsigned long)min(clone, max_brightness_clone),
			  bd->props.max_brightness, max_brightness_clone);
	if (clone && level < min_level)
		level = min_t(unsigned long, min_level, bd->props.max_brightness);
	backlight_device_set_brightness(bd, level);
	put_device(&bd->dev);
}

static void k618_set_clone(int disp_id, u32 clone)
{
	if (!disp_ok(disp_id))
		return;
	g_mi->brightness_clone[disp_id] = clone;
	k618_notify_u32(disp_id, MI_DISP_EVENT_BRIGHTNESS_CLONE, clone);
	k618_clone_fallback(disp_id, clone);
}

/* ---- panel-on dimming ---- */

static void k618_dimming_fn(struct work_struct *w)
{
	struct delayed_work *dw = to_delayed_work(w);
	struct k618_mi *mi = g_mi;
	int id;

	for (id = 0; id < MI_DISP_MAX; id++)
		if (dw == &mi->dimming_work[id])
			break;
	if (id < MI_DISP_MAX)
		k618_set_feature(id, DISP_FEATURE_DIMMING, 1);
}

void k618_mi_disp_panel_on(struct dsi_panel *panel)
{
	struct k618_mi *mi = g_mi;
	struct dsi_display *d;
	int id;

	if (!mi || !panel)
		return;
	d = container_of(panel->host, struct dsi_display, host);
	k618_mi_disp_attach(d);   /* bind the display + create disp-DSI-<n> if not yet done */
	id = (d->display_type && !strcmp(d->display_type, "secondary")) ?
		MI_DISP_SECONDARY : MI_DISP_PRIMARY;
	k618_notify_u32(id, MI_DISP_EVENT_POWER, 1 /* SDE_MODE_DPMS_ON */);
	mod_delayed_work(system_wq, &mi->dimming_work[id],
			 msecs_to_jiffies(panel_on_dimming_delay));
}

/* called from the panel's backlight path: pass the 51h level through as an event, like stock */
void k618_mi_disp_bl(struct dsi_panel *panel, u32 bl_lvl)
{
	struct dsi_display *d;
	int id;

	if (!g_mi || !panel)
		return;
	d = container_of(panel->host, struct dsi_display, host);
	id = (d->display_type && !strcmp(d->display_type, "secondary")) ?
		MI_DISP_SECONDARY : MI_DISP_PRIMARY;
	k618_notify_u32(id, MI_DISP_EVENT_51_BRIGHTNESS, bl_lvl);
}

/* ---- char device ---- */

static int k618_open(struct inode *ino, struct file *f)
{
	struct k618_client *c = kzalloc(sizeof(*c), GFP_KERNEL);
	unsigned long flags;

	if (!c)
		return -ENOMEM;
	init_waitqueue_head(&c->event_wait);
	INIT_LIST_HEAD(&c->event_list);
	mutex_init(&c->event_lock);
	c->event_space = 4096;
	spin_lock_irqsave(&g_mi->client_lock, flags);
	list_add_tail(&c->link, &g_mi->client_list);
	spin_unlock_irqrestore(&g_mi->client_lock, flags);
	f->private_data = c;
	return 0;
}

static int k618_release(struct inode *ino, struct file *f)
{
	struct k618_client *c = f->private_data;
	struct k618_pending *e, *t;
	unsigned long flags;
	int id, bit;

	spin_lock_irqsave(&g_mi->client_lock, flags);
	list_del(&c->link);
	list_for_each_entry_safe(e, t, &c->event_list, link) {
		list_del(&e->link);
		kfree(e);
	}
	spin_unlock_irqrestore(&g_mi->client_lock, flags);
	for (id = 0; id < MI_DISP_MAX; id++)
		for_each_set_bit(bit, c->evbit[id], MI_DISP_EVENT_MAX)
			if (bit == MI_DISP_EVENT_BRIGHTNESS_CLONE)
				atomic_dec(&g_mi->clone_subs);
	kfree(c);
	return 0;
}

static __poll_t k618_poll(struct file *f, struct poll_table_struct *wait)
{
	struct k618_client *c = f->private_data;

	poll_wait(f, &c->event_wait, wait);
	return list_empty(&c->event_list) ? 0 : (EPOLLIN | EPOLLRDNORM);
}

static ssize_t k618_read(struct file *f, char __user *buf, size_t count, loff_t *off)
{
	struct k618_client *c = f->private_data;
	int ret;

	ret = mutex_lock_interruptible(&c->event_lock);
	if (ret)
		return ret;
	for (;;) {
		struct k618_pending *e = NULL;
		u32 len;

		spin_lock_irq(&g_mi->client_lock);
		if (!list_empty(&c->event_list)) {
			e = list_first_entry(&c->event_list, struct k618_pending, link);
			c->event_space += e->event.base.length;
			list_del(&e->link);
		}
		spin_unlock_irq(&g_mi->client_lock);

		if (!e) {
			if (ret)
				break;
			if (f->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}
			mutex_unlock(&c->event_lock);
			ret = wait_event_interruptible(c->event_wait,
						       !list_empty(&c->event_list));
			if (ret >= 0)
				ret = mutex_lock_interruptible(&c->event_lock);
			if (ret)
				return ret;
			continue;
		}
		len = e->event.base.length;
		if (len > count - ret) {
			spin_lock_irq(&g_mi->client_lock);
			c->event_space -= len;
			list_add(&e->link, &c->event_list);
			spin_unlock_irq(&g_mi->client_lock);
			wake_up_interruptible(&c->event_wait);
			break;
		}
		if (copy_to_user(buf + ret, &e->event, len)) {
			if (ret == 0)
				ret = -EFAULT;
			spin_lock_irq(&g_mi->client_lock);
			c->event_space -= len;
			list_add(&e->link, &c->event_list);
			spin_unlock_irq(&g_mi->client_lock);
			wake_up_interruptible(&c->event_wait);
			break;
		}
		ret += len;
		kfree(e);
	}
	mutex_unlock(&c->event_lock);
	return ret;
}

static int k618_do_register(struct k618_client *c, void *data, bool on)
{
	struct disp_event_req *r = data;

	if (!disp_ok(r->base.disp_id) || r->type >= MI_DISP_EVENT_MAX)
		return -EINVAL;
	mutex_lock(&c->event_lock);
	if (on && !test_and_set_bit(r->type, c->evbit[r->base.disp_id])) {
		if (r->type == MI_DISP_EVENT_BRIGHTNESS_CLONE)
			atomic_inc(&g_mi->clone_subs);
	} else if (!on && test_and_clear_bit(r->type, c->evbit[r->base.disp_id])) {
		if (r->type == MI_DISP_EVENT_BRIGHTNESS_CLONE)
			atomic_dec(&g_mi->clone_subs);
	}
	mutex_unlock(&c->event_lock);
	return 0;
}

static long k618_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct k618_client *c = f->private_data;
	unsigned int nr = _IOC_NR(cmd), sz = _IOC_SIZE(cmd);
	u8 kdata[64] = {0};
	int disp_id, rc = 0;

	if (nr >= MI_DISP_IOCTL_NR_MAX || sz > sizeof(kdata))
		return -EINVAL;
	if ((cmd & IOC_IN) && sz && copy_from_user(kdata, (void __user *)arg, sz))
		return -EFAULT;
	disp_id = ((struct disp_base *)kdata)->disp_id;

	switch (nr) {
	case _IOC_NR(MI_DISP_IOCTL_VERSION):
		((struct disp_version *)kdata)->version = MI_DISP_FEATURE_VERSION;
		break;
	case _IOC_NR(MI_DISP_IOCTL_REGISTER_EVENT):
		rc = k618_do_register(c, kdata, true);
		break;
	case _IOC_NR(MI_DISP_IOCTL_DEREGISTER_EVENT):
		rc = k618_do_register(c, kdata, false);
		break;
	case _IOC_NR(MI_DISP_IOCTL_SET_FEATURE): {
		struct disp_feature_req *r = (void *)kdata;

		rc = k618_set_feature(r->base.disp_id, r->feature_id, r->feature_val);
		break;
	}
	case _IOC_NR(MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS): {
		struct disp_doze_brightness_req *r = (void *)kdata;

		if (disp_ok(r->base.disp_id)) {
			g_mi->doze_brightness[r->base.disp_id] = r->doze_brightness;
			k618_notify_u32(r->base.disp_id, MI_DISP_EVENT_DOZE, r->doze_brightness);
		}
		break;
	}
	case _IOC_NR(MI_DISP_IOCTL_GET_DOZE_BRIGHTNESS): {
		struct disp_doze_brightness_req *r = (void *)kdata;

		r->doze_brightness = disp_ok(disp_id) ? g_mi->doze_brightness[disp_id] : 0;
		break;
	}
	case _IOC_NR(MI_DISP_IOCTL_GET_BRIGHTNESS): {
		struct disp_brightness_req *r = (void *)kdata;

		r->brightness = 0;
		r->brightness_clone = disp_ok(disp_id) ? g_mi->brightness_clone[disp_id] : 0;
		break;
	}
	case _IOC_NR(MI_DISP_IOCTL_GET_FPS): {
		struct disp_fps_info *r = (void *)kdata;

		r->fps = fps;
		break;
	}
	case _IOC_NR(MI_DISP_IOCTL_GET_PANEL_INFO): {
		struct disp_panel_info *r = (void *)kdata;
		char line[160];
		int n = scnprintf(line, sizeof(line), "panel_name=%s\n", panel_name);

		if (r->info && r->info_len &&
		    copy_to_user(r->info, line, min_t(u32, r->info_len, n + 1)))
			rc = -EFAULT;
		r->info_len = n;
		break;
	}
	case _IOC_NR(MI_DISP_IOCTL_GET_WP_INFO): {
		struct disp_wp_info *r = (void *)kdata;
		char line[80];
		int n = scnprintf(line, sizeof(line), "%s\n", wp);

		if (r->info && r->info_len &&
		    copy_to_user(r->info, line, min_t(u32, r->info_len, n + 1)))
			rc = -EFAULT;
		r->info_len = n;
		break;
	}
	default:
		/* WRITE/READ_DSI_CMD (0x09/0x0A) and the MI-SF extras 0x0D..0x12: accept, no-op */
		break;
	}

	if ((cmd & IOC_OUT) && sz && copy_to_user((void __user *)arg, kdata, sz))
		return -EFAULT;
	return rc;
}

static const struct file_operations k618_fops = {
	.owner          = THIS_MODULE,
	.open           = k618_open,
	.release        = k618_release,
	.unlocked_ioctl = k618_ioctl,
	.compat_ioctl   = k618_ioctl,
	.poll           = k618_poll,
	.read           = k618_read,
	.llseek         = no_llseek,
};

/* ---- the sysfs the composer reads (disp_feature/disp-DSI-0) ---- */

static ssize_t panel_info_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "panel_name=%s\n", panel_name); }
static DEVICE_ATTR_RO(panel_info);
static ssize_t wp_info_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%s\n", wp); }
static DEVICE_ATTR_RO(wp_info);
static ssize_t panel_build_id_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%s\n", build_id); }
static DEVICE_ATTR_RO(panel_build_id);
static ssize_t dynamic_fps_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%u\n", fps); }
static DEVICE_ATTR_RO(dynamic_fps);
static ssize_t max_brightness_clone_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%u\n", max_brightness_clone); }
static DEVICE_ATTR_RO(max_brightness_clone);

static int k618_dev_id(struct device *d)
{
	return (d == g_mi->ddev[MI_DISP_SECONDARY]) ? MI_DISP_SECONDARY : MI_DISP_PRIMARY;
}

static ssize_t brightness_clone_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%u\n", g_mi->brightness_clone[k618_dev_id(d)]); }
static ssize_t brightness_clone_store(struct device *d, struct device_attribute *a,
				      const char *b, size_t n)
{
	u32 v;

	if (kstrtou32(b, 0, &v))
		return -EINVAL;
	k618_set_clone(k618_dev_id(d), v);
	return n;
}
static DEVICE_ATTR_RW(brightness_clone);

static ssize_t doze_brightness_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%u\n", g_mi->doze_brightness[k618_dev_id(d)]); }
static ssize_t doze_brightness_store(struct device *d, struct device_attribute *a,
				     const char *b, size_t n)
{
	u32 v;

	if (kstrtou32(b, 0, &v))
		return -EINVAL;
	g_mi->doze_brightness[k618_dev_id(d)] = v;
	k618_notify_u32(k618_dev_id(d), MI_DISP_EVENT_DOZE, v);
	return n;
}
static DEVICE_ATTR_RW(doze_brightness);

/* disp_param: "<feature_id> <value>", same as the HAL's sysfs path into set_disp_param */
static ssize_t disp_param_show(struct device *d, struct device_attribute *a, char *b)
{
	int id = k618_dev_id(d), i, n = 0;

	for (i = 0; i < DISP_FEATURE_MAX; i++)
		n += sysfs_emit_at(b, n, "feature[%02d]: %d\n", i, g_mi->feature_val[id][i]);
	return n;
}
static ssize_t disp_param_store(struct device *d, struct device_attribute *a,
				const char *b, size_t n)
{
	int id = k618_dev_id(d), fid, val;

	if (sscanf(b, "%d %d", &fid, &val) != 2)
		return -EINVAL;
	k618_set_feature(id, fid, val);
	return n;
}
static DEVICE_ATTR_RW(disp_param);

static struct attribute *k618_attrs[] = {
	&dev_attr_panel_info.attr,
	&dev_attr_wp_info.attr,
	&dev_attr_panel_build_id.attr,
	&dev_attr_dynamic_fps.attr,
	&dev_attr_max_brightness_clone.attr,
	&dev_attr_brightness_clone.attr,
	&dev_attr_doze_brightness.attr,
	&dev_attr_disp_param.attr,
	NULL,
};
ATTRIBUTE_GROUPS(k618);

static void k618_ownership(const struct device *d, kuid_t *uid, kgid_t *gid)
{
	*uid = make_kuid(&init_user_ns, owner_uid);
	*gid = make_kgid(&init_user_ns, owner_gid);
}

/* /dev/mi_display/<name> rather than /dev/<name> */
static char *k618_devnode(const struct device *d, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "mi_display/%s", dev_name(d));
}

/* ---- bind the real display when its panel comes up ---- */

void k618_mi_disp_attach(struct dsi_display *d)
{
	struct k618_mi *mi = g_mi;
	int id;

	if (!mi || !d)
		return;
	id = (d->display_type && !strcmp(d->display_type, "secondary")) ?
		MI_DISP_SECONDARY : MI_DISP_PRIMARY;
	mi->disp[id] = d;
	if (mi->ddev[id])
		return;
	mi->ddev[id] = device_create_with_groups(mi->class, mi->pdev, MKDEV(0, 0), NULL,
						 k618_groups, "disp-DSI-%d", id);
	if (IS_ERR(mi->ddev[id])) {
		pr_err("disp-DSI-%d device failed (%ld)\n", id, PTR_ERR(mi->ddev[id]));
		mi->ddev[id] = NULL;
	}
}

/* ---- init / exit (called from msm_drm module init/exit via the edits) ---- */

int k618_mi_disp_init(void)
{
	struct k618_mi *mi;
	int rc, id;

	mi = kzalloc(sizeof(*mi), GFP_KERNEL);
	if (!mi)
		return -ENOMEM;
	INIT_LIST_HEAD(&mi->client_list);
	spin_lock_init(&mi->client_lock);
	atomic_set(&mi->clone_subs, 0);
	for (id = 0; id < MI_DISP_MAX; id++)
		INIT_DELAYED_WORK(&mi->dimming_work[id], k618_dimming_fn);

	rc = alloc_chrdev_region(&mi->dev_id, 0, 1, "disp_feature");
	if (rc)
		goto err_free;
	cdev_init(&mi->cdev, &k618_fops);
	mi->cdev.owner = THIS_MODULE;
	rc = cdev_add(&mi->cdev, mi->dev_id, 1);
	if (rc)
		goto err_region;

	mi->class = class_create("mi_display");
	if (IS_ERR(mi->class)) {
		rc = PTR_ERR(mi->class);
		goto err_cdev;
	}
	mi->class->devnode = k618_devnode;
	mi->class->get_ownership = k618_ownership;

	/* the node the composer opens: /dev/mi_display/disp_feature, /sys .../disp_feature */
	mi->pdev = device_create(mi->class, NULL, mi->dev_id, NULL, "disp_feature");
	if (IS_ERR(mi->pdev)) {
		rc = PTR_ERR(mi->pdev);
		goto err_class;
	}
	g_mi = mi;

	/* create disp-DSI-0 straight away so the composer's early panel_info read succeeds;
	 * the real display pointer is filled in by k618_mi_disp_attach() when the panel binds
	 */
	k618_mi_disp_attach(NULL);
	mi->ddev[MI_DISP_PRIMARY] =
		device_create_with_groups(mi->class, mi->pdev, MKDEV(0, 0), NULL,
					  k618_groups, "disp-DSI-%d", MI_DISP_PRIMARY);
	if (IS_ERR(mi->ddev[MI_DISP_PRIMARY]))
		mi->ddev[MI_DISP_PRIMARY] = NULL;

	pr_info("mi_display ready: /dev/mi_display/disp_feature, version %#06x, clone max %u\n",
		MI_DISP_FEATURE_VERSION, max_brightness_clone);
	return 0;

err_class:
	class_destroy(mi->class);
err_cdev:
	cdev_del(&mi->cdev);
err_region:
	unregister_chrdev_region(mi->dev_id, 1);
err_free:
	kfree(mi);
	return rc;
}

void k618_mi_disp_exit(void)
{
	struct k618_mi *mi = g_mi;
	int id;

	if (!mi)
		return;
	for (id = 0; id < MI_DISP_MAX; id++) {
		cancel_delayed_work_sync(&mi->dimming_work[id]);
		if (mi->ddev[id])
			device_unregister(mi->ddev[id]);
	}
	device_destroy(mi->class, mi->dev_id);
	class_destroy(mi->class);
	cdev_del(&mi->cdev);
	unregister_chrdev_region(mi->dev_id, 1);
	g_mi = NULL;
	kfree(mi);
}
