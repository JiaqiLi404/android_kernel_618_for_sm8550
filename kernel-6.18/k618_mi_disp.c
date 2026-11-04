// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#define pr_fmt(fmt) "k618_midisp: " fmt

#include <linux/backlight.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
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

/* the 13 Pro's mi_disp: set_disp_param notifies event type 7 after an HBM change (stock 0x18cecc) */
#define MI_DISP_EVENT_HBM 7
/* the 13 Pro's feature ids beyond the 12 Pro's 0..21 (stock feature name table .rodata+0x899e8) */
#define DISP_FEATURE_HBM_BACKLIGHT 22	/* "hbm_backlight_level": MiBrightness::writeDBVCmd() */
#define DISP_FEATURE_BACKLIGHT     23	/* "backlight" */

/* Device compatibility. */
#define DISP_FEATURE_DOZE_BRIGHTNESS    3
#define DISP_FEATURE_LOCAL_HBM          9
#define DISP_FEATURE_SENSOR_LUX         10
#define DISP_FEATURE_LOW_BRIGHTNESS_FOD 11
#define DISP_FEATURE_FP_STATUS          12
#define DISP_FEATURE_AOD_TO_NORMAL      16

/* enum local_hbm_state */
#define LOCAL_HBM_OFF_TO_NORMAL                    0
#define LOCAL_HBM_NORMAL_WHITE_1000NIT             1
#define LOCAL_HBM_NORMAL_WHITE_110NIT              4
#define LOCAL_HBM_NORMAL_GREEN_500NIT              5
#define LOCAL_HBM_HLPM_WHITE_1000NIT               6
#define LOCAL_HBM_HLPM_WHITE_110NIT                7
#define LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT          10
#define LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE  11

/* enum doze_brightness_state */
#define DOZE_TO_NORMAL      0
#define DOZE_BRIGHTNESS_HBM 1
#define DOZE_BRIGHTNESS_LBM 2

/* enum mi_panel_state (stock panel+0xc04) */
#define K618_PANEL_STATE_ON        1
#define K618_PANEL_STATE_DOZE_HIGH 2
#define K618_PANEL_STATE_DOZE_LOW  3

/* the FOD event's payload (fod_ui_ready_state) */
#define K618_FOD_UI_NONE            0
#define K618_FOD_LOW_BRIGHTNESS_CAP 0x4	/* FOD_LOW_BRIGHTNESS_CAPTURE */
#define K618_FOD_UI_READY           0x8	/* LOCAL_HBM_UI_READY */

/* what SET_LOCAL_HBM carries: mfp-daemon's codes (stock name table .rodata+0x89280) */
#define K618_MFP_FINGER_UP  0
#define K618_MFP_AUTH_STOP  1
#define K618_MFP_WHITE_1000 2
#define K618_MFP_WHITE_110  3
#define K618_MFP_GREEN_500  4

/* mi,panel-id of this phone's panel, as the stock compares it (8 bytes "\0\0M2\08\f\0", big endian) */
#define K618_M2_PANEL_ID 0x00004D3200380C00ULL

/* Device compatibility. */
static bool em_cycle = true;
module_param(em_cycle, bool, 0644);
/* -1: follow the panel's mi,disable-ic-dimming-flag (stock); 0: never send dimming on/off; 1: always */
static int ic_dimming = -1;
module_param(ic_dimming, int, 0644);
/* Device compatibility. */
static bool fod = true;
module_param(fod, bool, 0644);
/* Device compatibility. */
static bool doze_cmd = true;
module_param(doze_cmd, bool, 0644);

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
/* SET_LOCAL_HBM's 12 bytes: the mfp code is the word after disp_base (stock reads [req+8]) */
struct disp_local_hbm_req { struct disp_base base; __u32 local_hbm_value; };
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
#define MI_DISP_IOCTL_SET_LOCAL_HBM       _IOW('D', 0x0E, struct disp_local_hbm_req)

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

/* non-static in dsi_panel.c for us (k618_tp_fix.py); dsi_panel.h declares it for OnePlus only */
int dsi_panel_tx_cmd_set(struct dsi_panel *panel, enum dsi_cmd_set_type type,
			 bool do_peripheral_flush);

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

/* Device compatibility. */

/* the stock "-command-update" tables this port uses, one per command (cmd_set_update_map of the .ko) */
enum k618_upd_kind {
	K618_UPD_TIMING_SWITCH,
	K618_UPD_EM_ON,
	K618_UPD_EM_OFF,
	K618_UPD_HBM_ON,
	K618_UPD_HBM_OFF,
	K618_UPD_NOLP,
	K618_UPD_QSYNC_ON,
	K618_UPD_QSYNC_OFF,
	K618_UPD_MAX
};

static const struct {
	const char *prop;
	enum dsi_cmd_set_type type;
} k618_upd_map[K618_UPD_MAX] = {
	[K618_UPD_TIMING_SWITCH] = { "qcom,mdss-dsi-timing-switch-command-update",
				     DSI_CMD_SET_TIMING_SWITCH },
	[K618_UPD_EM_ON]  = { "mi,mdss-dsi-panel-em-cycle-16pulse-on-command-update",
			      DSI_CMD_SET_MI_EM_CYCLE_16PULSE_ON },
	[K618_UPD_EM_OFF] = { "mi,mdss-dsi-panel-em-cycle-16pulse-off-command-update",
			      DSI_CMD_SET_MI_EM_CYCLE_16PULSE_OFF },
	[K618_UPD_HBM_ON]  = { "mi,mdss-dsi-hbm-on-command-update", DSI_CMD_SET_MI_HBM_ON },
	[K618_UPD_HBM_OFF] = { "mi,mdss-dsi-hbm-off-command-update", DSI_CMD_SET_MI_HBM_OFF },
	[K618_UPD_NOLP] = { "qcom,mdss-dsi-nolp-command-update", DSI_CMD_SET_NOLP },
	/* stock types 24/25 use update slots 32/33, the "-em-cycle-update" ones, not 31 (min fps) */
	[K618_UPD_QSYNC_ON]  = { "qcom,mdss-dsi-qsync-on-commands-em-cycle-update",
				 DSI_CMD_SET_QSYNC_ON },
	[K618_UPD_QSYNC_OFF] = { "qcom,mdss-dsi-qsync-off-commands-em-cycle-update",
				 DSI_CMD_SET_QSYNC_OFF },
};

#define K618_UPD_ENTRIES 8	/* the M2 timings carry at most 4 triples per command */
#define K618_TIMINGS     16	/* cell-index range; this panel has 11 timing nodes (0..10) */
#define K618_EM_TABLE    14	/* BDh pulse table bytes (stock requires exactly 14) */

/* one <register index length> triple: packet idx of the command must start with reg; len bytes follow */
struct k618_upd {
	u8 reg;
	u8 len;
	u16 idx;
};

struct k618_upd_set {
	u8 count;
	struct k618_upd e[K618_UPD_ENTRIES];
};

struct k618_bl {
	struct dsi_panel *panel;	/* the panel the tables below were read for */
	bool parsed;
	bool is_m2;			/* mi,panel-id == M2: dead zone, HBM-at-0, qsync rules */
	bool em_capable;		/* + threshold, three tables, build id: em-cycle switching */
	bool dt_no_ic_dimming;		/* mi,disable-ic-dimming-flag (k618_no_ic_dimming()) */
	u32 thr;			/* mi,panel-1920pwm-dbv-threshold (stock panel+0xc18) */
	u8 table_3p[K618_EM_TABLE];	/* mi,panel-em-cycle-3pulse-gamma-paramerer   (panel+0xc1c) */
	u8 table_16p[K618_EM_TABLE];	/* mi,panel-em-cycle-16pulse-gamma-paramerer  (panel+0xc2a) */
	u8 table_auto[K618_EM_TABLE];	/* mi,panel-em-cycle-gamma-paramerer-auto     (panel+0xc38) */
	struct k618_upd_set upd[K618_TIMINGS][K618_UPD_MAX];	/* by the timing's cell-index */

	/* runtime, panel_lock held */
	u32 last_bl;	/* stock mi_cfg.last_bl_level (panel+0x990): the level asked for last, 0 = off */
	u32 req_bl;	/* the level of the write in progress, before the dead zone */
	bool pre_done;	/* k618_mi_disp_bl_pre() ran for the write in progress */
	bool em_16p;	/* stock panel+0xc14: the panel is in 16-pulse (1920 Hz) emission */
	bool qsync_on;	/* stock panel+0xea0 */
	bool hbm_on;	/* stock feature_val[DISP_FEATURE_HBM] (panel+0x8f0) */

	/* Device compatibility. */
	u32 doze_hbm_dbv;	/* mi,doze-hbm-dbv-level (stock panel+0x9f0; M2: 0xf5) */
	u32 doze_lbm_dbv;	/* mi,doze-lbm-dbv-level (stock panel+0x9f4; M2: 0x14) */
	u32 ui_ready_frames;	/* mi,local-hbm-ui-ready-delay-num-frame (stock panel+0xa00; 2) */
	u32 panel_state;	/* K618_PANEL_STATE_*: which doze level is on the panel (panel+0xc04) */
	u32 doze_level;		/* the DOZE_* the HAL asked for last (panel+0x9b0) */
	bool in_lp;		/* LP1/LP2 sent and no nolp since: doze commands may go out */
	bool lhbm_on;		/* a local-HBM state is lit: no 51h (stock is_backlight_set_skip) */
};

static struct k618_bl k618_bls[MI_DISP_MAX];

static int k618_panel_id(struct dsi_panel *panel)
{
	struct dsi_display *d = container_of(panel->host, struct dsi_display, host);

	return (d->display_type && !strcmp(d->display_type, "secondary")) ?
		MI_DISP_SECONDARY : MI_DISP_PRIMARY;
}

/* one "-command-update" property of a timing node into *s (absent = no entries) */
static void k618_upd_parse(struct k618_upd_set *s, struct device_node *np, const char *prop)
{
	u32 v[K618_UPD_ENTRIES * 3];
	int n = of_property_count_u32_elems(np, prop);
	int i;

	s->count = 0;
	if (n <= 0)
		return;
	if (n % 3 || n > (int)ARRAY_SIZE(v) || of_property_read_u32_array(np, prop, v, n)) {
		pr_warn("%pOFn: %s: %d cells, ignored\n", np, prop, n);
		return;
	}
	for (i = 0; i < n; i += 3) {
		if (v[i] > 0xff || !v[i + 2] || v[i + 2] > 0xff || v[i + 1] > 0xffff)
			continue;
		s->e[s->count].reg = v[i];
		s->e[s->count].idx = v[i + 1];
		s->e[s->count].len = v[i + 2];
		s->count++;
	}
}

static bool k618_table(struct device_node *np, const char *prop, u8 *buf)
{
	return of_property_count_u8_elems(np, prop) == K618_EM_TABLE &&
	       !of_property_read_u8_array(np, prop, buf, K618_EM_TABLE);
}

/* the panel-level properties and every timing's update tables, from the panel's DT node */
static void k618_bl_parse(struct k618_bl *b, struct dsi_panel *panel)
{
	struct device_node *np = panel->panel_of_node, *timings, *child;
	u64 panel_id = 0;
	u8 bid = 0;
	int i, pos = 0;

	b->panel = panel;
	b->parsed = true;
	b->panel_state = K618_PANEL_STATE_ON;
	if (!np)
		return;

	b->dt_no_ic_dimming = of_property_read_bool(np, "mi,disable-ic-dimming-flag");
	of_property_read_u32(np, "mi,doze-hbm-dbv-level", &b->doze_hbm_dbv);
	of_property_read_u32(np, "mi,doze-lbm-dbv-level", &b->doze_lbm_dbv);
	of_property_read_u32(np, "mi,local-hbm-ui-ready-delay-num-frame", &b->ui_ready_frames);

	of_property_read_u64(np, "mi,panel-id", &panel_id);
	b->is_m2 = panel_id == K618_M2_PANEL_ID;
	/*
	 * stock reads the build id from the panel (DAh, mi_dsi_display_read_panel_build_id) and skips
	 * the em-cycle logic for 0, 0x50, 0xb0 and 0xfb; this phone's panel answered 0xA0 on 5.15
	 */
	if (kstrtou8(build_id, 0, &bid))
		bid = 0;
	b->em_capable = b->is_m2 &&
		!of_property_read_u32(np, "mi,panel-1920pwm-dbv-threshold", &b->thr) && b->thr >= 3 &&
		k618_table(np, "mi,panel-em-cycle-3pulse-gamma-paramerer", b->table_3p) &&
		k618_table(np, "mi,panel-em-cycle-16pulse-gamma-paramerer", b->table_16p) &&
		k618_table(np, "mi,panel-em-cycle-gamma-paramerer-auto", b->table_auto) &&
		bid && bid != 0x50 && bid != 0xb0 && bid != 0xfb;

	timings = of_get_child_by_name(np, "qcom,mdss-dsi-display-timings");
	for_each_child_of_node(timings, child) {
		u32 cell = pos++;

		of_property_read_u32(child, "cell-index", &cell);
		if (cell >= K618_TIMINGS)
			continue;
		for (i = 0; i < K618_UPD_MAX; i++)
			k618_upd_parse(&b->upd[cell][i], child, k618_upd_map[i].prop);
	}
	of_node_put(timings);

	pr_info("%s: M2 %d, em-cycle %d (threshold %#x, build id %#x), %d timings, DT IC dimming %s\n",
		panel->name, b->is_m2, b->em_capable, b->thr, bid, pos,
		b->dt_no_ic_dimming ? "disabled" : "allowed");
}

/* the ic_dimming parameter (0644) wins over the DT flag, so an A/B needs no reboot */
static bool k618_no_ic_dimming(const struct k618_bl *b)
{
	return ic_dimming >= 0 ? !ic_dimming : b->dt_no_ic_dimming;
}

/* the state for this panel, read from the DT on first use; panel_lock held */
static struct k618_bl *k618_bl_get(struct dsi_panel *panel)
{
	struct k618_bl *b;

	if (!panel || !panel->host)
		return NULL;
	b = &k618_bls[k618_panel_id(panel)];
	if (!b->parsed || b->panel != panel) {
		memset(b, 0, sizeof(*b));
		k618_bl_parse(b, panel);
	}
	return b;
}

static const struct k618_upd_set *k618_upd_get(struct k618_bl *b, struct dsi_panel *panel,
						 enum k618_upd_kind kind)
{
	u32 idx;

	if (!panel->cur_mode || !panel->cur_mode->priv_info)
		return NULL;
	idx = panel->cur_mode->mode_idx;	/* = the timing's cell-index (dsi_panel_get_mode) */
	return idx < K618_TIMINGS ? &b->upd[idx][kind] : NULL;
}

/*
 * stock mi_dsi_panel_update_cmd_set(): write len bytes after the register byte of one packet of the
 * current mode's command set. The parsed packets are private kzalloc'ed copies
 * (dsi_panel_create_cmd_packets), so the change stays until the next one -- as on stock.
 */
static int k618_patch(struct dsi_panel *panel, enum dsi_cmd_set_type type,
		      const struct k618_upd *u, const u8 *val, u8 len)
{
	struct dsi_panel_cmd_set *set = &panel->cur_mode->priv_info->cmd_sets[type];
	struct dsi_cmd_desc *cmd;
	u8 *buf;

	if (u->len != len || !set->cmds || u->idx >= set->count)
		return -EINVAL;
	cmd = &set->cmds[u->idx];
	buf = (u8 *)cmd->msg.tx_buf;
	if (!buf || buf[0] != u->reg || cmd->msg.tx_len < (size_t)len + 1) {
		pr_warn_ratelimited("%s: set %d packet %u is not %02xh/%u, not patched\n",
				    panel->name, type, u->idx, u->reg, len);
		return -EINVAL;
	}
	memcpy(buf + 1, val, len);
	return 0;
}

/* the entries of one table with this register and length; first_only = stock mi_dsi_update_51_mipi_cmd */
static void k618_patch_reg(struct k618_bl *b, struct dsi_panel *panel, enum k618_upd_kind kind,
			   u8 reg, const u8 *val, u8 len, bool first_only)
{
	const struct k618_upd_set *s = k618_upd_get(b, panel, kind);
	int i;

	if (!s)
		return;
	for (i = 0; i < s->count; i++) {
		if (s->e[i].reg != reg || s->e[i].len != len)
			continue;
		k618_patch(panel, k618_upd_map[kind].type, &s->e[i], val, len);
		if (first_only)
			break;
	}
}

/*
 * stock mi_dsi_update_em_cycle_cmd(): rewrite one command for level bl --
 *   BDh, 2 bytes:  21 xx, xx = 0x02 above the threshold (3 pulses) or 0x0f at/below it (16 pulses),
 *                  | 0x80 unless the mode is 120 Hz or qsync is on;
 *   BDh, 14 bytes: the "auto" table at 120 Hz or with qsync on, else the 3- or 16-pulse table;
 *   51h, 2 bytes:  bl, high byte first.
 * A level of 0 changes nothing, except in the qsync-off command (stock: type != 25 && bl == 0).
 * Stock compares the 21 xx byte against a literal 0x148 and the table against the DT threshold;
 * on this panel both are 0x148.
 */
static void k618_em_update(struct k618_bl *b, struct dsi_panel *panel, enum k618_upd_kind kind,
			   u32 bl)
{
	const struct k618_upd_set *s = k618_upd_get(b, panel, kind);
	enum dsi_cmd_set_type type = k618_upd_map[kind].type;
	u8 dbv[2] = { (bl >> 8) & 0xff, bl & 0xff };
	u8 cfg[2] = { 0x21, bl > b->thr ? 0x02 : 0x0f };
	bool fps120;
	const u8 *table;
	int i;

	if (!b->em_capable || !s || !s->count || (!bl && kind != K618_UPD_QSYNC_OFF))
		return;
	fps120 = panel->cur_mode->timing.refresh_rate == 120;
	if (!b->qsync_on && !fps120)
		cfg[1] |= 0x80;
	table = (b->qsync_on || fps120) ? b->table_auto :
		(bl > b->thr ? b->table_3p : b->table_16p);

	for (i = 0; i < s->count; i++) {
		const struct k618_upd *u = &s->e[i];

		if (u->reg == 0x51 && u->len == 2)
			k618_patch(panel, type, u, dbv, 2);
		else if (u->reg == 0xbd && u->len == 2)
			k618_patch(panel, type, u, cfg, 2);
		else if (u->reg == 0xbd && u->len == K618_EM_TABLE)
			k618_patch(panel, type, u, table, K618_EM_TABLE);
	}
}

/*
 * stock dsi_panel_send_em_cycle_setting(): 16 pulses at or below the threshold, 3 above; the first
 * nonzero level after power-on (last_bl == 0) always sends one of the two; at 120 Hz the
 * timing-switch command follows. The state flips even when the send fails (stock: off clears it
 * before sending, on sets it after).
 */
static int k618_em_switch(struct k618_bl *b, struct dsi_panel *panel, u32 bl)
{
	enum k618_upd_kind kind;
	bool want16;
	int rc;

	if (!b->em_capable || !em_cycle || !bl || !panel->cur_mode)
		return 0;
	want16 = bl <= b->thr;
	if (!b->last_bl)
		b->em_16p = !want16;
	if (want16 == b->em_16p)
		return 0;

	kind = want16 ? K618_UPD_EM_ON : K618_UPD_EM_OFF;
	k618_em_update(b, panel, kind, bl);
	rc = dsi_panel_tx_cmd_set(panel, k618_upd_map[kind].type, false);
	b->em_16p = want16;
	if (rc) {
		pr_err("%s: em-cycle 16 pulse %s at %u failed: %d\n", panel->name,
		       want16 ? "on" : "off", bl, rc);
		return rc;
	}
	pr_info("%s: em-cycle 16 pulse %s at %u, %u Hz\n", panel->name, want16 ? "on" : "off", bl,
		panel->cur_mode->timing.refresh_rate);

	if (panel->cur_mode->timing.refresh_rate == 120) {
		k618_em_update(b, panel, K618_UPD_TIMING_SWITCH, bl);
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_TIMING_SWITCH, false);
		if (rc)
			pr_err("%s: timing switch after em-cycle failed: %d\n", panel->name, rc);
	}
	return rc;
}

/* stock dsi_panel_update_backlight(): HBM is turned off with 51h = 0 when the level goes to 0 (M2) */
static void k618_hbm_off_at_zero(struct k618_bl *b, struct dsi_panel *panel)
{
	static const u8 zero[2];

	k618_patch_reg(b, panel, K618_UPD_HBM_OFF, 0x51, zero, 2, true);
	if (dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_OFF, false))
		pr_err("%s: hbm off at level 0 failed\n", panel->name);
	b->hbm_on = false;
	if (g_mi)
		g_mi->feature_val[k618_panel_id(panel)][DISP_FEATURE_HBM] = 0;
}

/* ---- hooks dsi_panel.c calls (panel_lock held) ---- */

bool k618_mi_disp_bl_pre(struct dsi_panel *panel, u32 *bl_lvl)
{
	struct k618_bl *b;
	u32 bl;

	if (!g_mi || !bl_lvl)
		return true;
	b = k618_bl_get(panel);
	if (!b)
		return true;
	bl = *bl_lvl;
	b->req_bl = bl;
	b->pre_done = true;
	if (!b->is_m2)
		return true;
	/*
	 * stock is_backlight_set_skip() (0x186150): no 51h while the fingerprint area is lit --
	 * the level is still kept (k618_mi_disp_bl), and the off command or the next write puts it
	 * back
	 */
	if (b->lhbm_on || g_mi->feature_val[k618_panel_id(panel)][DISP_FEATURE_HBM_FOD])
		return false;

	if (!bl && b->hbm_on)
		k618_hbm_off_at_zero(b, panel);
	else if (b->thr >= 3 && bl > b->thr && bl <= b->thr + 3)
		bl = b->thr + 4;		/* 0x149..0x14b -> 0x14c */
	else if (b->thr >= 3 && bl + 2 >= b->thr && bl <= b->thr)
		bl = b->thr - 3;		/* 0x146..0x148 -> 0x145 */

	k618_em_switch(b, panel, bl);
	*bl_lvl = bl;
	/* stock: no 51h while qsync is on; the qsync-off command carries the level (k618_mi_disp_qsync) */
	return !(b->qsync_on && bl);
}

void k618_mi_disp_timing_switch(struct dsi_panel *panel)
{
	struct k618_bl *b = k618_bl_get(panel);

	if (b)
		k618_em_update(b, panel, K618_UPD_TIMING_SWITCH, b->last_bl);
}

void k618_mi_disp_qsync(struct dsi_panel *panel, bool on)
{
	struct k618_bl *b = k618_bl_get(panel);

	if (!b || !b->is_m2)
		return;
	b->qsync_on = on;	/* before the rewrite, as stock: qsync on = no 0x80, "auto" table */
	k618_em_update(b, panel, on ? K618_UPD_QSYNC_ON : K618_UPD_QSYNC_OFF, b->last_bl);
}

/* Device compatibility. */

/* bl, high byte first: a 51h packet's two bytes */
static void k618_dbv2(u8 out[2], u32 bl)
{
	out[0] = (bl >> 8) & 0xff;
	out[1] = bl & 0xff;
}

static bool k618_in_doze(const struct k618_bl *b)
{
	return b->panel_state == K618_PANEL_STATE_DOZE_HIGH ||
	       b->panel_state == K618_PANEL_STATE_DOZE_LOW;
}

/* the dbv of the doze level on the panel (stock doze_hbm/lbm_dbv_level) */
static u32 k618_doze_dbv(const struct k618_bl *b)
{
	if (b->panel_state == K618_PANEL_STATE_DOZE_LOW && b->doze_lbm_dbv)
		return b->doze_lbm_dbv;
	return b->doze_hbm_dbv ? b->doze_hbm_dbv : b->last_bl;
}

/*
 * stock mi_dsi_update_51_mipi_cmd(): in command `type` of the active timing, the first packet that
 * `update_prop` (that command's "-command-update") names for register `reg` gets `len` new bytes.
 * Read on the spot (the brightness path keeps tables; doze and fingerprint commands are rare). A
 * command without the property is left alone, as stock (it finds no entry and returns -EINVAL).
 */
static void k618_patch_cmd_now(struct dsi_panel *panel, enum dsi_cmd_set_type type,
			       const char *update_prop, u8 reg, const u8 *val, u8 len)
{
	struct device_node *timings, *child, *np = NULL;
	struct k618_upd_set set;
	u32 want, cell, pos = 0;
	int i;

	if (!panel->cur_mode || !panel->cur_mode->priv_info || !panel->panel_of_node)
		return;
	timings = of_get_child_by_name(panel->panel_of_node, "qcom,mdss-dsi-display-timings");
	if (!timings)
		return;
	want = panel->cur_mode->mode_idx;	/* = the timing's cell-index, as k618_bl_parse() */
	for_each_child_of_node(timings, child) {
		cell = pos++;
		of_property_read_u32(child, "cell-index", &cell);
		if (cell == want) {
			np = child;	/* the loop's reference, put below */
			break;
		}
	}
	of_node_put(timings);
	if (!np)
		return;
	k618_upd_parse(&set, np, update_prop);
	of_node_put(np);
	for (i = 0; i < set.count; i++)
		if (set.e[i].reg == reg && set.e[i].len == len) {
			k618_patch(panel, type, &set.e[i], val, len);
			break;
		}
}

/*
 * stock mi_dsi_panel_set_doze_brightness() (0x187efc), panel_lock held, panel in AOD (b->in_lp):
 * doze-hbm (32) or doze-lbm (33) for the level asked for, unless that level is on the panel
 * already ("has been set, skip") or the fingerprint HBM is lit (stock keeps the level and sends
 * nothing then). DOZE_TO_NORMAL sends nothing: nolp brings the panel back. Each command writes
 * 51h twice: the normal-mode level before 53h = 24 (AOD on) -- 07 0a in the DT, which stock
 * zeroes first (mi_dsi_update_51_mipi_cmd(panel, 32/33, 0) for M2), so no flash of it on the way
 * in -- and then the AOD level itself, fixed per command (HBM 07 fe, LBM 00 f6).
 */
static int k618_doze_send(struct k618_bl *b, struct dsi_panel *p)
{
	static const u8 dbv_0[2] = { 0, 0 };
	enum dsi_cmd_set_type type;
	const char *upd;
	u32 state;
	int rc;

	if (!doze_cmd || !b->is_m2 || !p->panel_initialized)
		return 0;
	if (g_mi && g_mi->feature_val[k618_panel_id(p)][DISP_FEATURE_HBM_FOD])
		return 0;
	if (b->doze_level == DOZE_BRIGHTNESS_HBM) {
		type = DSI_CMD_SET_MI_DOZE_HBM;
		upd = "mi,mdss-dsi-doze-hbm-command-update";
		state = K618_PANEL_STATE_DOZE_HIGH;
	} else if (b->doze_level == DOZE_BRIGHTNESS_LBM) {
		type = DSI_CMD_SET_MI_DOZE_LBM;
		upd = "mi,mdss-dsi-doze-lbm-command-update";
		state = K618_PANEL_STATE_DOZE_LOW;
	} else {
		return 0;
	}
	if (b->panel_state == state)
		return 0;
	k618_patch_cmd_now(p, type, upd, 0x51, dbv_0, 2);
	rc = dsi_panel_tx_cmd_set(p, type, false);
	if (rc) {
		pr_err("%s: doze level %u: %d\n", p->name, b->doze_level, rc);
		return rc;
	}
	b->panel_state = state;
	pr_info("%s: doze level %u\n", p->name, b->doze_level);
	return 0;
}

/*
 * dsi_panel_set_lp1/lp2(), after the command, panel_lock held: the panel is in AOD, and the level
 * the HAL asked for goes out now. Stock queues SET_DOZE_BRIGHTNESS until then and wakes it from the
 * end of dsi_panel_set_lp1(); sent to a lit panel, a doze command would put it in doze mode.
 */
void k618_mi_disp_post_lp(struct dsi_panel *panel, int rc)
{
	struct k618_bl *b = k618_bl_get(panel);

	if (!b || rc)
		return;
	b->in_lp = true;
	k618_doze_send(b, panel);
}

/* Device compatibility. */
void k618_mi_disp_pre_nolp(struct dsi_panel *panel)
{
	struct k618_bl *b = k618_bl_get(panel);
	u32 lvl;
	u8 dbv[2];

	if (!b || !b->is_m2)
		return;
	lvl = k618_in_doze(b) ? k618_doze_dbv(b) : b->last_bl;
	if (!lvl)
		return;
	k618_dbv2(dbv, lvl);
	k618_patch_reg(b, panel, K618_UPD_NOLP, 0x51, dbv, 2, true);
}

void k618_mi_disp_post_nolp(struct dsi_panel *panel, int rc)
{
	struct k618_bl *b = k618_bl_get(panel);
	bool from_doze;

	if (!b || rc)
		return;
	from_doze = k618_in_doze(b);
	b->in_lp = false;
	b->panel_state = K618_PANEL_STATE_ON;
	if (!b->is_m2)
		return;
	b->em_16p = true;
	/*
	 * not in stock (its doze levels, 0x14 and 0xf5, are below the threshold): if the level on the
	 * panel is above it, leave 16-pulse now rather than at the next level change. Out of a doze
	 * level the panel is at that level's dbv, below it.
	 */
	if (!from_doze && b->last_bl > b->thr)
		k618_em_switch(b, panel, b->last_bl);
}

/* stock dsi_panel_disable(): em state, 0-size, HBM, qsync... cleared; last level 0 = "first" next time */
void k618_mi_disp_panel_off(struct dsi_panel *panel)
{
	struct k618_bl *b = k618_bl_get(panel);

	if (!b)
		return;
	b->em_16p = false;
	b->qsync_on = false;
	b->hbm_on = false;
	b->last_bl = 0;
	b->pre_done = false;
	b->in_lp = false;	/* the doze level asked for stays: the next AOD sends it again */
	b->panel_state = K618_PANEL_STATE_ON;
	b->lhbm_on = false;
	if (g_mi)
		g_mi->feature_val[k618_panel_id(panel)][DISP_FEATURE_HBM] = 0;
}

/* ---- the feature path's brightness parts (they take panel_lock) ---- */

/* stock set_disp_param DISP_FEATURE_DIMMING: dropped when the panel disables IC dimming */
static int k618_dimming(struct dsi_display *d, int on)
{
	struct dsi_panel *p = d->panel;
	struct k618_bl *b;
	int rc;

	if (!p)
		return -ENODEV;
	mutex_lock(&p->panel_lock);
	b = k618_bl_get(p);
	if (!p->panel_initialized)
		rc = -ENODEV;
	else if (b && k618_no_ic_dimming(b))
		rc = -EPERM;
	else
		rc = dsi_panel_tx_cmd_set(p, on ? DSI_CMD_SET_MI_DIMMINGON : DSI_CMD_SET_MI_DIMMINGOFF,
					  false);
	mutex_unlock(&p->panel_lock);
	return rc;
}

/*
 * stock set_disp_param DISP_FEATURE_HBM on M2 (0x18c6b8):
 *   on:  em-cycle for 0x7ff (= 3 pulses), 53h = 0x28 in hbm-on, send it (51h 07 ff, 63h 30, 49h 00);
 *   off: 51h = 0x7ff in hbm-off (its second 51h), 53h = 0x20, send it;
 *   both: the remembered level becomes 0x7ff -- the HAL writes the real one next.
 */
static int k618_hbm(struct dsi_display *d, int val)
{
	static const u8 dbv_7ff[2] = { 0x07, 0xff };
	static const u8 on53 = 0x28, off53 = 0x20;
	struct dsi_panel *p = d->panel;
	struct k618_bl *b;
	int rc;

	if (!p)
		return -ENODEV;
	mutex_lock(&p->panel_lock);
	if (!p->panel_initialized) {
		rc = -ENODEV;
		goto out;
	}
	b = k618_bl_get(p);
	if (!b || !b->is_m2) {
		rc = dsi_panel_tx_cmd_set(p, val == 1 ? DSI_CMD_SET_MI_HBM_ON : DSI_CMD_SET_MI_HBM_OFF,
					  false);
		goto out;
	}
	if (val == 1) {
		k618_em_switch(b, p, 0x7ff);
		k618_patch_reg(b, p, K618_UPD_HBM_ON, 0x53, &on53, 1, false);
		rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_HBM_ON, false);
		b->hbm_on = true;
	} else {
		k618_patch_reg(b, p, K618_UPD_HBM_OFF, 0x51, dbv_7ff, 2, true);
		k618_patch_reg(b, p, K618_UPD_HBM_OFF, 0x53, &off53, 1, false);
		rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_HBM_OFF, false);
		b->hbm_on = false;
	}
	b->last_bl = 0x7ff;
out:
	mutex_unlock(&p->panel_lock);
	return rc;
}

/*
 * stock set_disp_param features 22/23 (0x18ca44): remember the level, then the backlight path
 * (dsi_panel_update_backlight: dead zone, em-cycle, 51h). Here dsi_panel_set_backlight() carries
 * the same hooks; the backlight device's own value is left alone, as on stock.
 */
static int k618_set_level(struct dsi_display *d, int val)
{
	struct dsi_panel *p = d->panel;
	struct k618_bl *b;
	int rc;

	if (!p)
		return -ENODEV;
	if (val < 0 || val > 0xffff)
		return -EINVAL;
	mutex_lock(&p->panel_lock);
	if (!p->panel_initialized) {
		rc = -ENODEV;
	} else {
		b = k618_bl_get(p);
		if (b)
			b->last_bl = val;
		rc = dsi_panel_set_backlight(p, val);
	}
	mutex_unlock(&p->panel_lock);
	return rc;
}

/* Device compatibility. */

/* the command for a lit state; the 16-pulse variant when the base level is in 16-pulse mode */
static enum dsi_cmd_set_type k618_lhbm_cmd(int state, bool em16)
{
	switch (state) {
	case LOCAL_HBM_NORMAL_WHITE_1000NIT:
		return em16 ? DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_16P
			    : DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT;
	case LOCAL_HBM_NORMAL_WHITE_110NIT:
		return em16 ? DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT_16P
			    : DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT;
	case LOCAL_HBM_NORMAL_GREEN_500NIT:
		return em16 ? DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT_16P
			    : DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT;
	case LOCAL_HBM_HLPM_WHITE_1000NIT:
		return DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT;
	case LOCAL_HBM_HLPM_WHITE_110NIT:
		return DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT;
	default:
		return DSI_CMD_SET_MAX;
	}
}

/*
 * stock mi_dsi_panel_set_lhbm_fod_locked() (0x18d3c0) for M2. The panel sets none of the alpha /
 * DFh / white-point update flags, so stock's runtime rewrites of 87h, DFh and 66h all return early
 * for it: the DT commands go out as parsed, with only the 51h of 1000 nit, HLPM and off-to-hbm
 * (and off-to-normal from AOD) patched.
 */
static int k618_lhbm(struct dsi_display *d, int state)
{
	struct dsi_panel *p = d->panel;
	enum dsi_cmd_set_type type;
	struct k618_bl *b;
	u8 dbv[2];
	u32 lvl;
	bool aod, lit;
	int rc = 0;

	if (!p)
		return -ENODEV;
	mutex_lock(&p->panel_lock);
	if (!p->panel_initialized) {
		rc = -ENODEV;
		goto out;
	}
	b = k618_bl_get(p);
	if (!b)
		goto out;
	lit = state != LOCAL_HBM_OFF_TO_NORMAL && state != LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT &&
	      state != LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE;
	if (!lit)
		b->lhbm_on = false;	/* whatever happens below: never leave 51h writes blocked */
	if (!fod || !b->is_m2)
		goto out;
	aod = k618_in_doze(b);

	switch (state) {
	case LOCAL_HBM_NORMAL_WHITE_1000NIT:
		/*
		 * stock: bl_max in HBM, the doze level's dbv in AOD, else the last level -- into the
		 * 3-pulse command's 51h; the 16-pulse twin has no "-command-update" of its own in this
		 * DT, so it goes as parsed
		 */
		type = k618_lhbm_cmd(state, b->em_16p);
		if (type == DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT) {
			lvl = b->hbm_on ? p->bl_config.bl_max_level :
					  (aod ? k618_doze_dbv(b) : b->last_bl);
			k618_dbv2(dbv, lvl);
			k618_patch_cmd_now(p, type,
					   "mi,mdss-dsi-local-hbm-normal-white-1000nit-command-update",
					   0x51, dbv, 2);
		}
		rc = dsi_panel_tx_cmd_set(p, type, false);
		break;
	case LOCAL_HBM_NORMAL_WHITE_110NIT:
	case LOCAL_HBM_NORMAL_GREEN_500NIT:
		rc = dsi_panel_tx_cmd_set(p, k618_lhbm_cmd(state, b->em_16p), false);
		break;
	case LOCAL_HBM_HLPM_WHITE_1000NIT:
	case LOCAL_HBM_HLPM_WHITE_110NIT:
		/* from AOD: the HLPM command's 51h = the doze level's dbv; the panel is on after it */
		k618_dbv2(dbv, k618_doze_dbv(b));
		type = k618_lhbm_cmd(state, false);
		k618_patch_cmd_now(p, type, state == LOCAL_HBM_HLPM_WHITE_1000NIT ?
				   "mi,mdss-dsi-local-hbm-hlpm-white-1000nit-command-update" :
				   "mi,mdss-dsi-local-hbm-hlpm-white-110nit-command-update",
				   0x51, dbv, 2);
		rc = dsi_panel_tx_cmd_set(p, type, false);
		b->panel_state = K618_PANEL_STATE_ON;
		break;
	case LOCAL_HBM_OFF_TO_NORMAL:
	case LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT:
	case LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE:
		if (b->hbm_on) {
			/* back to the global HBM: off-to-hbm (63) with 51h = the last level */
			k618_dbv2(dbv, b->last_bl);
			k618_patch_cmd_now(p, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM,
					   "mi,mdss-dsi-local-hbm-off-to-hbm-command-update",
					   0x51, dbv, 2);
			rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM, false);
			break;
		}
		/*
		 * off-to-normal (61, or its 16-pulse twin 62, each with a "-command-update" of its
		 * own): the composer's next backlight write restores the level after a plain one; the
		 * backlight variants (from AOD) patch the level into the command that goes out
		 */
		type = b->em_16p ? DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_16P
				 : DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL;
		if (state != LOCAL_HBM_OFF_TO_NORMAL) {
			lvl = aod ? min(b->last_bl ? b->last_bl : k618_doze_dbv(b), k618_doze_dbv(b)) :
				    b->last_bl;
			k618_dbv2(dbv, lvl);
			k618_patch_cmd_now(p, type, b->em_16p ?
				"mi,mdss-dsi-local-hbm-off-to-normal-em-cycle-16pulse-command-update" :
				"mi,mdss-dsi-local-hbm-off-to-normal-command-update", 0x51, dbv, 2);
		}
		rc = dsi_panel_tx_cmd_set(p, type, false);
		break;
	default:
		pr_debug("local hbm state %d: nothing for M2\n", state);
		lit = false;
		break;
	}
	if (rc) {
		pr_err("%s: local hbm state %d: %d\n", p->name, state, rc);
	} else {
		b->lhbm_on = lit;
		pr_info("%s: local hbm state %d (16-pulse %d, aod %d)\n", p->name, state, b->em_16p, aod);
	}
out:
	mutex_unlock(&p->panel_lock);
	return rc;
}

/* the FOD event (type 2) the fingerprint HAL waits for before it captures */
static void k618_fod_event(int disp_id, bool down, bool low_brightness)
{
	struct k618_bl *b = &k618_bls[disp_id];
	u32 ui = K618_FOD_UI_NONE;

	if (down) {
		ui = K618_FOD_UI_READY;
		if (low_brightness)
			ui |= K618_FOD_LOW_BRIGHTNESS_CAP;
		/* stock lets the HBM settle for mi,local-hbm-ui-ready-delay-num-frame frames first */
		if (b->ui_ready_frames && b->panel && b->panel->cur_mode) {
			u32 fps_now = b->panel->cur_mode->timing.refresh_rate;
			u32 us = fps_now ? 1000000U / fps_now * b->ui_ready_frames : 0;

			if (us)
				usleep_range(us, us + 1000);
		}
	}
	k618_notify_u32(disp_id, MI_DISP_EVENT_FOD, ui);
}

/*
 * SET_LOCAL_HBM (0x0E): stock queues the mfp code for its FOD thread (mi_disp_lhbm_fod_thread_fn,
 * 0x17f11c), which maps it to a local_hbm_state and sends it through set_disp_param. Done in line
 * here: panel_lock serialises, and a press is not a hot path.
 */
static int k618_set_local_hbm(int disp_id, int mfp_code)
{
	struct k618_mi *mi = g_mi;
	struct dsi_display *d;
	bool aod, low;
	int state, rc;

	if (!mi || !disp_ok(disp_id))
		return -EINVAL;
	d = mi->disp[disp_id];
	if (!d || !d->panel)
		return -ENODEV;
	aod = k618_in_doze(&k618_bls[disp_id]);
	/* the dim capture: the HAL set LOW_BRIGHTNESS_FOD and asks for 110 nit */
	low = mi->feature_val[disp_id][DISP_FEATURE_LOW_BRIGHTNESS_FOD] &&
	      mfp_code == K618_MFP_WHITE_110;

	switch (mfp_code) {
	case K618_MFP_FINGER_UP:
		state = aod ? LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT : LOCAL_HBM_OFF_TO_NORMAL;
		break;
	case K618_MFP_AUTH_STOP:
		state = aod ? LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE : LOCAL_HBM_OFF_TO_NORMAL;
		break;
	case K618_MFP_WHITE_1000:
		state = aod ? LOCAL_HBM_HLPM_WHITE_1000NIT : LOCAL_HBM_NORMAL_WHITE_1000NIT;
		break;
	case K618_MFP_WHITE_110:
		state = aod ? LOCAL_HBM_HLPM_WHITE_110NIT : LOCAL_HBM_NORMAL_WHITE_110NIT;
		break;
	case K618_MFP_GREEN_500:
		state = LOCAL_HBM_NORMAL_GREEN_500NIT;
		break;
	default:
		return -EINVAL;
	}
	mi->feature_val[disp_id][DISP_FEATURE_LOCAL_HBM] = state;

	/* stock: finger up / auth stop say UP before the command, the lit states DOWN after it */
	if (mfp_code <= K618_MFP_AUTH_STOP)
		k618_fod_event(disp_id, false, false);
	rc = k618_lhbm(d, state);
	if (!rc && mfp_code >= K618_MFP_WHITE_1000)
		k618_fod_event(disp_id, true, low);
	return rc;
}

/* SET_DOZE_BRIGHTNESS and the doze_brightness file: remember the level, send it if in AOD now */
static int k618_doze(struct dsi_display *d, u32 level)
{
	struct dsi_panel *p;
	struct k618_bl *b;
	int rc = 0;

	if (!d || !d->panel)
		return -ENODEV;
	p = d->panel;
	mutex_lock(&p->panel_lock);
	b = k618_bl_get(p);
	if (b) {
		b->doze_level = level;
		if (b->in_lp)
			rc = k618_doze_send(b, p);
	}
	mutex_unlock(&p->panel_lock);
	return rc;
}

/*
 * AOD_TO_NORMAL (16): the fingerprint animation layer showed up in AOD -- stock
 * (mi_dsi_panel_aod_to_normal_optimize_locked, 0x18e258) brings the panel out ahead of the
 * framework with doze-hbm-nolp (34) / doze-lbm-nolp (35), so the area lights before the screen
 */
static int k618_aod_to_normal(struct dsi_display *d)
{
	struct dsi_panel *p = d->panel;
	struct k618_bl *b;
	int rc = 0;

	if (!p)
		return -ENODEV;
	mutex_lock(&p->panel_lock);
	b = k618_bl_get(p);
	if (b && fod && b->is_m2 && p->panel_initialized && b->in_lp && k618_in_doze(b)) {
		rc = dsi_panel_tx_cmd_set(p, b->panel_state == K618_PANEL_STATE_DOZE_LOW ?
					  DSI_CMD_SET_MI_DOZE_LBM_NOLP : DSI_CMD_SET_MI_DOZE_HBM_NOLP,
					  false);
		if (!rc)
			b->panel_state = K618_PANEL_STATE_ON;
		pr_info("%s: aod to normal: %d\n", p->name, rc);
	}
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
		rc = k618_dimming(d, val);
		if (rc == -EPERM) {
			/* stock prints "disable_ic_dimming is 1" and neither sends nor stores it */
			pr_debug("disp%d dimming %d dropped (IC dimming disabled)\n", disp_id, val);
			return 0;
		}
		break;
	case DISP_FEATURE_HBM:
		rc = k618_hbm(d, val);
		k618_notify_u32(disp_id, MI_DISP_EVENT_HBM, val);
		break;
	case DISP_FEATURE_FLAT_MODE:
		rc = k618_tx(d, val ? DSI_CMD_SET_MI_FLAT_MODE_ON : DSI_CMD_SET_MI_FLAT_MODE_OFF);
		break;
	case DISP_FEATURE_HBM_BACKLIGHT:
	case DISP_FEATURE_BACKLIGHT:
		rc = k618_set_level(d, val);
		break;
	case DISP_FEATURE_LOCAL_HBM:
		/* the HAL's set_disp_param path: a local_hbm_state itself */
		rc = k618_lhbm(d, val);
		break;
	case DISP_FEATURE_DOZE_BRIGHTNESS:
		rc = k618_doze(d, val);
		break;
	case DISP_FEATURE_AOD_TO_NORMAL:
		if (val)
			rc = k618_aod_to_normal(d);
		break;
	default:
		/*
		 * FP_STATUS, HBM_FOD, SENSOR_LUX, LOW_BRIGHTNESS_FOD (stored: the local HBM and doze
		 * paths read them), count / round-corner / MI-SF notify ids: accepted, so the HAL
		 * stops failing
		 */
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

/* panel enable: POWER event; panel-on dimming unless the panel disables IC dimming */
void k618_mi_disp_panel_on(struct dsi_panel *panel)
{
	struct k618_mi *mi = g_mi;
	struct k618_bl *b;
	struct dsi_display *d;
	int id;

	if (!mi || !panel || !panel->host)
		return;
	d = container_of(panel->host, struct dsi_display, host);
	k618_mi_disp_attach(d);   /* bind the display + create disp-DSI-<n> if not yet done */
	id = k618_panel_id(panel);
	k618_notify_u32(id, MI_DISP_EVENT_POWER, 1 /* SDE_MODE_DPMS_ON */);
	b = k618_bl_get(panel);   /* dsi_panel_enable() holds panel_lock */
	if (b && k618_no_ic_dimming(b))
		return;
	mod_delayed_work(system_wq, &mi->dimming_work[id],
			 msecs_to_jiffies(panel_on_dimming_delay));
}

/*
 * called after the 51h write (or in its place when qsync skips it) with
 * the level that went to the panel; remembers the level that was asked for (stock
 * mi_dsi_panel_update_last_bl_level) and passes the written one on as the 51_brightness event
 * (stock: the level after the dead zone, 0x15a16c)
 */
void k618_mi_disp_bl(struct dsi_panel *panel, u32 bl_lvl)
{
	struct k618_bl *b;

	if (!g_mi || !panel || !panel->host)
		return;
	b = k618_bl_get(panel);
	if (b) {
		b->last_bl = b->pre_done ? b->req_bl : bl_lvl;
		b->pre_done = false;
	}
	k618_notify_u32(k618_panel_id(panel), MI_DISP_EVENT_51_BRIGHTNESS, bl_lvl);
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
			k618_doze(g_mi->disp[r->base.disp_id], r->doze_brightness);
			g_mi->doze_brightness[r->base.disp_id] = r->doze_brightness;
			k618_notify_u32(r->base.disp_id, MI_DISP_EVENT_DOZE, r->doze_brightness);
		}
		break;
	}
	case _IOC_NR(MI_DISP_IOCTL_SET_LOCAL_HBM): {
		struct disp_local_hbm_req *r = (void *)kdata;

		rc = k618_set_local_hbm(r->base.disp_id, r->local_hbm_value);
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
		/* WRITE/READ_DSI_CMD (0x09/0x0A) and the MI-SF extras 0x0D, 0x0F..0x12: accept, no-op */
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
	.llseek         = noop_llseek,	/* no_llseek is gone (6.12) */
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
	k618_doze(g_mi->disp[k618_dev_id(d)], v);
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

/* Device compatibility. */
static ssize_t cell_id_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "\n"); }
static DEVICE_ATTR_RO(cell_id);
static ssize_t flatmode_check_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "1\n"); }
static DEVICE_ATTR_RO(flatmode_check);
static ssize_t hw_vsync_info_show(struct device *d, struct device_attribute *a, char *b)
{
	return sysfs_emit(b, "fps: %u.000 vsync_period_ns:%u panel_mode:dsi_cmd panel_type:primary\n",
			  fps, fps ? 1000000000U / fps : 0);
}
static DEVICE_ATTR_RO(hw_vsync_info);
static ssize_t disp_count_show(struct device *d, struct device_attribute *a, char *b)
{
	return sysfs_emit(b,
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
static ssize_t disp_count_store(struct device *d, struct device_attribute *a, const char *b, size_t n)
{ return n; }
static DEVICE_ATTR(disp_count, 0664, disp_count_show, disp_count_store);
static ssize_t k618_zero_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "0\n"); }
static ssize_t k618_drop_store(struct device *d, struct device_attribute *a, const char *b, size_t n)
{ return n; }
static DEVICE_ATTR(mipi_rw, 0664, k618_zero_show, k618_drop_store);
static DEVICE_ATTR(pps_rw, 0644, k618_zero_show, k618_drop_store);
static DEVICE_ATTR(dsi_phy_rw, 0644, k618_zero_show, k618_drop_store);
static DEVICE_ATTR(dsi_porch_rw, 0644, k618_zero_show, k618_drop_store);

static struct attribute *k618_attrs[] = {
	&dev_attr_panel_info.attr,
	&dev_attr_wp_info.attr,
	&dev_attr_panel_build_id.attr,
	&dev_attr_dynamic_fps.attr,
	&dev_attr_max_brightness_clone.attr,
	&dev_attr_brightness_clone.attr,
	&dev_attr_doze_brightness.attr,
	&dev_attr_disp_param.attr,
	&dev_attr_cell_id.attr,
	&dev_attr_flatmode_check.attr,
	&dev_attr_hw_vsync_info.attr,
	&dev_attr_disp_count.attr,
	&dev_attr_mipi_rw.attr,
	&dev_attr_pps_rw.attr,
	&dev_attr_dsi_phy_rw.attr,
	&dev_attr_dsi_porch_rw.attr,
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

	/* every DSI display node's probe calls this, and a deferred probe calls it again */
	if (g_mi)
		return 0;
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
