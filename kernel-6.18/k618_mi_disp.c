// SPDX-License-Identifier: GPL-2.0
/*
 * k618_mi_disp (built into msm_drm as mi_disp_k618.c): the Xiaomi display extension this phone's userspace drives
 * brightness and panel modes through, ported from the 12 Pro's published mi_disp (MiCode zeus-s-oss
 * techpack/display-drivers/msm/mi_disp) and trimmed to what fixes brightness / colour / flicker.
 *
 * It is compiled INTO msm_drm (display-drivers/msm/), not loaded as a separate first-stage module:
 * the event device and the display-parameter path both have to send DSI command sets on the live
 * panel under panel_lock, which only code inside msm_drm can do. kernel-6.18/k618_midisp.c (first
 * stage, sysfs only) must not be loaded with it -- a second "mi_display" class would collide, so
 * k618_midisp stays out of the first-stage list (k618_timg.sh MIDISP=).
 *
 * What it provides, matching the ABI the 13 Pro's own closed msm_drm.ko exposes (confirmed against
 * /vendor_dlkm/lib/modules/msm_drm.ko and the userspace that drives it):
 *
 *  (1) the char device /dev/mi_display/disp_feature (class "mi_display", devnode
 *      "mi_display/disp_feature"), with the ioctl ABI version 0x0100 and:
 *        0x00 VERSION, 0x01 SET_FEATURE, 0x02/0x03 SET/GET_DOZE_BRIGHTNESS, 0x04 GET_PANEL_INFO,
 *        0x05 GET_WP_INFO, 0x06 GET_FPS, 0x07/0x08 (DE)REGISTER_EVENT, 0x09 WRITE_DSI_CMD,
 *        0x0A READ_DSI_CMD, 0x0B GET_BRIGHTNESS, plus the 13 Pro / MI-SF extras 0x0D..0x12 answered
 *        sanely. poll()/read() deliver struct disp_event + payload; MiBrightness subscribes to
 *        MI_DISP_EVENT_BRIGHTNESS_CLONE (type 5) and MI-SF to POWER/DOZE/FOD/51_brightness.
 *
 *  (2) the display-parameter path (SET_FEATURE and the disp_param sysfs node): flat mode on/off,
 *      dimming on/off (dropped where the DT says mi,disable-ic-dimming-flag, as stock) and HBM on/off
 *      send the panel's own DT command sets on the active panel; HBM and the 13 Pro's features 22/23
 *      (the HBM-range levels MiBrightness writes) as the stock module does them -- see (4).
 *      Those command sets live per timing node under qcom,mdss-dsi-display-timings with the same
 *      blob format as qcom,mdss-dsi-on-command; k618_tp_fix.py adds them to enum
 *      dsi_cmd_set_type + cmd_set_prop_map[]/cmd_set_state_map[] so Qualcomm's own per-mode parser
 *      reads them into priv_info->cmd_sets[], and we send them with dsi_panel_tx_cmd_set().
 *      Feature ids we do not act on (count / round-corner / MI-SF notify ids) are accepted and
 *      stored so the HAL's setDispParam / flat-mode / MI-SF ioctls stop failing.
 *
 *  (3) panel-on dimming: panel enable fires the POWER event and, panel_on_dimming_delay ms later,
 *      sends dimming-on (stock mi,panel-on-dimming-delay = 120) -- unless IC dimming is disabled.
 *
 *  (4) the brightness chain of the phone's own 5.15 msm_drm for this panel (reverse-engineered
 *      from that module): the 1920 Hz PWM switch (16-pulse emission at DBV <=
 *      mi,panel-1920pwm-dbv-threshold, 3-pulse above it) and the dead zone around it before every
 *      51h write; the DT's "-command-update" byte patching of the em-cycle, timing-switch, qsync,
 *      HBM and nolp commands for the current level and refresh rate. Without the switch, leaving
 *      DOZE with nolp puts the panel in 16 pulses and later timing switches write 3-pulse BDh bytes
 *      beside nolp's 65h -- a mix stock never makes (a white flash and flicker above a level).
 *      msm_drm.em_cycle=0 / msm_drm.ic_dimming= for A/B.
 *
 *  (5) the fingerprint area and AOD (reverse-engineered from the same module):
 *      SET_LOCAL_HBM (0x0E) takes mfp-daemon's code, picks the local-HBM command for it (the
 *      16-pulse variant when the base level is in 16-pulse mode, the HLPM one from AOD), sends it
 *      and tells the fingerprint HAL through the FOD event when the area is lit; the doze levels
 *      (SET_DOZE_BRIGHTNESS, the doze_brightness file, feature 3) send doze-hbm / doze-lbm once the
 *      panel is in LP1/LP2 (k618_mi_disp_post_lp), nolp then takes that level's dbv, and AOD_TO_
 *      NORMAL (16) brings the panel out for the fingerprint animation. msm_drm.fod=0 /
 *      msm_drm.doze_cmd=0 store the requests only.
 *
 *  (6) the 13 (M3) and 13 Ultra (M1) panels, from the same 5.15 msm_drm.ko (the 0x... numbers
 *      below are its .text offsets), keyed on mi,panel-id; of the 13 Pro (M2) paths only
 *      AOD_TO_NORMAL changes (it follows stock on all three):
 *      - M1 DSC: three rate-control values of its 10 bpc / 8 bpp RGB modes (k618_mi_disp_dsc);
 *      - M1 DBI-BWG and skip-source command sets by DBV band and panel temperature, before every
 *        51h write, at timing switch, at the 1 Hz post switch and after the fingerprint area
 *        goes off in the lowest band;
 *      - M1 26h/2Fh (the flat-mode gamma choice, oriented by the panel build id) and BAh (40/30/24
 *        Hz) in the timing-switch, post-switch, flat-mode and doze commands; the build id is DAh,
 *        read once after the first panel on (msm_drm.m1_build_id= sets it instead);
 *      - M1 local-HBM calibration: 87h alpha by level, DFh, D0h gray levels from SMEM item 498;
 *      - M3 DC dimming (mi,dc-feature-type = "crc_skip_backlight"): no 51h below the threshold
 *        while the HAL has DC on;
 *      - HBM: M3 on M2's path without the em-cycle switch, M1 sends nothing;
 *      - AOD_TO_NORMAL for both values as stock, with the M1/M2 timing switch after it below
 *        120 Hz, and the M1 timing switch after every nolp;
 *      - flat mode on only for value 1, as stock (any other value is off).
 *
 * Brightness, and why there is no double write: on this ROM SDM (libsdmdal, in the composer) writes
 * /sys/class/mi_display/disp-DSI-0/brightness_clone (0..max_brightness_clone). mi_disp turns that
 * into a BRIGHTNESS_CLONE event; the displayfeature HAL's MiBrightness (/odm/lib64/libmiBrightness
 * .so) reads the event, maps clone -> panel DBV with its own curve, and writes /sys/class/backlight/
 * panel0-backlight/brightness -- which is what drives the 51h register. So the clone store here only
 * stores the value and emits the event; it does NOT touch the backlight. SDE's bridge-enable restore
 * (backlight_update_status) re-sends the level MiBrightness last wrote, as stock. A fallback keeps
 * k618_midisp's behaviour (write the backlight directly) ONLY while no client is subscribed to
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
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "dsi_display.h"
#include "dsi_panel.h"
#include "mi_disp_k618.h"

/* ---- the ABI, lifted from the 12 Pro's uapi mi_disp.h (same on the 13 Pro) ---- */

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

/* fingerprint and AOD (uapi mi_disp.h values) */
#define DISP_FEATURE_DOZE_BRIGHTNESS    3
#define DISP_FEATURE_LOCAL_HBM          9
#define DISP_FEATURE_SENSOR_LUX         10
#define DISP_FEATURE_LOW_BRIGHTNESS_FOD 11
#define DISP_FEATURE_FP_STATUS          12
#define DISP_FEATURE_AOD_TO_NORMAL      16
/* the 13 (M3) and 13 Ultra (M1) panels (stock feature name table .rodata+0x899e8) */
#define DISP_FEATURE_DC                 8
#define DISP_FEATURE_DBI                20
/* set_disp_param DISP_FEATURE_DC notifies event type 8 (stock 0x18cc74) */
#define MI_DISP_EVENT_DC 8
/* the fp_status (feature 12) value that keeps the panel out of AOD (stock 0x18e388) */
#define K618_FP_AUTH_STOP 4

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
/* the 13 Ultra's and the 13's panels ("\0\0M1\0B\2\0" and "\0\0M3\08\f\0") */
#define K618_M1_PANEL_ID 0x00004D3100420200ULL
#define K618_M3_PANEL_ID 0x00004D3300380C00ULL

/* 1: switch the 1920 Hz PWM like stock; 0: never send em-cycle commands */
static bool em_cycle = true;
module_param(em_cycle, bool, 0644);
/* -1: follow the panel's mi,disable-ic-dimming-flag (stock); 0: never send dimming on/off; 1: always */
static int ic_dimming = -1;
module_param(ic_dimming, int, 0644);
/* 1: light the fingerprint area (local HBM) as stock; 0: store the request only */
static bool fod = true;
module_param(fod, bool, 0644);
/* 1: send the AOD brightness commands as stock; 0: store the level only */
static bool doze_cmd = true;
module_param(doze_cmd, bool, 0644);
/* 1: the 13 Ultra's DBI-BWG / skip-source sets by level and temperature (stock vrr_set_by_dbv) */
static bool m1_dbi = true;
module_param(m1_dbi, bool, 0644);
/* 1: the 26h/2Fh/BAh rewrites in the 13 Ultra's timing-switch, flat-mode and doze commands */
static bool m1_gamma = true;
module_param(m1_gamma, bool, 0644);
/* 1: the 13 Ultra's local-HBM calibration (87h, DFh, D0h); 0: those commands go as parsed */
static bool m1_lhbm_cal = true;
module_param(m1_lhbm_cal, bool, 0644);
/* 1: the 13 Ultra's DSC rate-control values (applied while the modes are parsed, at boot) */
static bool m1_dsc = true;
module_param(m1_dsc, bool, 0444);
/* -1: the 13 Ultra's build id is read from the panel (DAh); 0..255: this value is used */
static int m1_build_id = -1;
module_param(m1_build_id, int, 0644);
/* 1: no 51h below the DC threshold while DC is on (DT crc_skip_backlight: the 13) */
static bool dc_skip = true;
module_param(dc_skip, bool, 0644);

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
	struct work_struct build_id_work[MI_DISP_MAX];	/* the 13 Ultra's build id read */
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

/* ---- brightness: the 1920 Hz PWM switch, command-update patching, HBM ---- */

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

#define K618_M1_LHBM_SMEM	498	/* SMEM item with the local-HBM gray levels (bootloader) */
#define K618_M1_LHBM_HALF	26	/* bytes per half: flat mode off, then on */
#define K618_M1_LHBM_LEN	(2 * K618_M1_LHBM_HALF)
#define K618_M1_BA_LEN		9	/* BAh payload of the 40/30/24 Hz timing switch */

/*
 * the 13 Ultra's per-timing values (stock mi_dsi_panel_parse_sub_timing 0x186b4c and
 * mi_dsi_panel_parse_2F26reg_gamma_config 0x1874f0)
 */
struct k618_m1_timing {
	bool valid;
	u8 ddic_mode;		/* mi,mdss-dsi-ddic-mode: 0 normal, 1 idle, 2 auto, 3 qsync, 4 diff, 5 test */
	u32 fps;		/* qcom,mdss-dsi-panel-framerate */
	u32 sf;			/* mi,mdss-dsi-sf-framerate; the frame rate when there is no ddic mode */
	u32 dmin;		/* mi,mdss-dsi-ddic-min-framerate; likewise */
	u8 cfg2f[2];		/* mi,mdss-flat-status-control-gamma-2F-cfg <a b> */
	u8 cfg26[2];		/* mi,mdss-flat-status-control-gamma-26-cfg <a b> */
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

	/* AOD and fingerprint */
	u32 doze_hbm_dbv;	/* mi,doze-hbm-dbv-level (stock panel+0x9f0; M2: 0xf5) */
	u32 doze_lbm_dbv;	/* mi,doze-lbm-dbv-level (stock panel+0x9f4; M2: 0x14) */
	u32 ui_ready_frames;	/* mi,local-hbm-ui-ready-delay-num-frame (stock panel+0xa00; 2) */
	u32 panel_state;	/* K618_PANEL_STATE_*: which doze level is on the panel (panel+0xc04) */
	u32 doze_level;		/* the DOZE_* the HAL asked for last (panel+0x9b0) */
	bool in_lp;		/* LP1/LP2 sent and no nolp since: doze commands may go out */
	bool lhbm_on;		/* a local-HBM state is lit: no 51h (stock is_backlight_set_skip) */

	/* the 13 (M3) and 13 Ultra (M1) panels; the DT-driven parts apply to any panel with the DT */
	bool is_m1;
	bool is_m3;
	bool direct;		/* this write is one stock sends past is_backlight_set_skip() */
	bool need_fod_anim;	/* mi,need-fod-animal-in-normal-enabled (stock panel+0xa04) */
	u32 normal_to_aod_51;	/* mi,normal-to-aod-doze-51reg-update-value (stock panel+0x9f8) */
	bool dc_enabled;	/* mi,dc-feature-enabled (stock panel+0xa86) */
	u32 dc_type;		/* mi,dc-feature-type: 1 lut_compatible_backlight, 2 crc_skip_backlight */
	u32 dc_thr;		/* mi,mdss-dsi-panel-dc-threshold, 440 when absent (stock panel+0xa8c) */
	u32 last_fps;		/* the rate the last timing switch left the panel at (panel+0xbdc) */
	struct k618_m1_timing m1t[K618_TIMINGS];	/* by cell-index, M1 only */
	bool m1_build_read;	/* mi,panel-build-id-read-needed */
	int m1_build_id;	/* DAh as read after panel on (stock panel+0x6cc); -1 = not read */
	u8 m1_build_tries;
	u16 m1_doze_done;	/* timings whose doze 26h/2Fh were cleared (stock priv_info+0x13e0) */
	enum dsi_cmd_set_type m1_skip;	/* skip-source set last sent (stock panel+0xc58) */
	enum dsi_cmd_set_type m1_dbi;	/* DBI-BWG set last sent (stock panel+0xc5c) */
	enum dsi_cmd_set_type m1_dbi_shown;	/* the last one logged */
	u8 m1_lhbm[K618_M1_LHBM_LEN];	/* SMEM item 498 (stock panel+0xa48) */
	bool m1_lhbm_valid;	/* stock panel+0xa7c */
	int m1_smem_rc;
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

/* stock mi_dsi_panel_parse_config() 0x180b34: DC dimming, read only with a known type */
static void k618_dc_parse(struct k618_bl *b, struct device_node *np)
{
	const char *t = NULL;

	b->dc_enabled = of_property_read_bool(np, "mi,dc-feature-enabled");
	if (!b->dc_enabled || of_property_read_string(np, "mi,dc-feature-type", &t))
		return;
	if (!strcmp(t, "lut_compatible_backlight"))
		b->dc_type = 1;
	else if (!strcmp(t, "crc_skip_backlight"))
		b->dc_type = 2;
	else
		return;
	if (of_property_read_u32(np, "mi,mdss-dsi-panel-dc-threshold", &b->dc_thr))
		b->dc_thr = 440;
}

/*
 * M1 local-HBM gray levels the bootloader leaves in SMEM item 498 (stock mi_dsi_panel_parse_config
 * 0x1807c0): up to 52 bytes of big-endian u16, 26 bytes for flat mode off then 26 for on; used
 * when every u16 it holds is nonzero (0x180860, 0x1808d8). Stock calls qcom_smem_get() directly;
 * here it is looked up at run time (smem.ko, first-stage ramdisk), so msm_drm keeps no link-time
 * dependency or symbol CRC on it and a missing smem.ko only leaves the gray levels as parsed.
 */
static void k618_m1_smem(struct k618_bl *b)
{
	typeof(&qcom_smem_get) get;
	size_t size = 0, n, i;
	const u8 *src;

	get = symbol_get(qcom_smem_get);
	if (!get) {
		b->m1_smem_rc = -EPROBE_DEFER;	/* tried again at the next local-HBM command */
		pr_info("lhbm calibration: qcom_smem_get not available yet\n");
		return;
	}
	src = get(QCOM_SMEM_HOST_ANY, K618_M1_LHBM_SMEM, &size);
	if (IS_ERR_OR_NULL(src)) {
		b->m1_smem_rc = src ? PTR_ERR(src) : -ENOENT;
		symbol_put(qcom_smem_get);
		pr_info("lhbm calibration: smem item %d: %d\n", K618_M1_LHBM_SMEM, b->m1_smem_rc);
		return;
	}
	b->m1_smem_rc = 0;
	n = min_t(size_t, size, K618_M1_LHBM_LEN);
	memcpy(b->m1_lhbm, src, n);
	symbol_put(qcom_smem_get);
	b->m1_lhbm_valid = false;
	for (i = 0; i + 1 < n; i += 2) {
		b->m1_lhbm_valid = ((b->m1_lhbm[i] << 8) | b->m1_lhbm[i + 1]) != 0;
		if (!b->m1_lhbm_valid)
			break;
	}
	pr_info("lhbm calibration: %zu bytes in smem item %d, %s\n", size, K618_M1_LHBM_SMEM,
		b->m1_lhbm_valid ? "used" : "not used (a zero value)");
}

/* one timing node's sub-timing and 26h/2Fh values (stock 0x186b4c, 0x1874f0) */
static void k618_m1_parse_timing(struct k618_m1_timing *t, struct device_node *np)
{
	static const char * const modes[] = { "normal", "idle", "auto", "qsync", "diff", "test" };
	const char *mode = NULL;
	u32 v[2];
	int i;

	t->valid = true;
	of_property_read_u32(np, "qcom,mdss-dsi-panel-framerate", &t->fps);
	if (of_property_read_string(np, "mi,mdss-dsi-ddic-mode", &mode)) {
		/* no mode: normal, and the sf and minimum rates are the frame rate (0x186ca8) */
		t->ddic_mode = 0;
		t->sf = t->dmin = t->fps;
	} else {
		/* "normal" or an unknown string: normal, the two rates not read (0x186c80) */
		i = match_string(modes, ARRAY_SIZE(modes), mode);
		t->ddic_mode = i > 0 ? i : 0;
		if (i > 0) {
			of_property_read_u32(np, "mi,mdss-dsi-sf-framerate", &t->sf);
			of_property_read_u32(np, "mi,mdss-dsi-ddic-min-framerate", &t->dmin);
		}
	}
	if (!of_property_read_u32_array(np, "mi,mdss-flat-status-control-gamma-2F-cfg", v, 2)) {
		t->cfg2f[0] = v[0];
		t->cfg2f[1] = v[1];
	}
	if (!of_property_read_u32_array(np, "mi,mdss-flat-status-control-gamma-26-cfg", v, 2)) {
		t->cfg26[0] = v[0];
		t->cfg26[1] = v[1];
	}
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
	b->m1_skip = b->m1_dbi = b->m1_dbi_shown = DSI_CMD_SET_MAX;
	b->m1_build_id = -1;
	if (!np)
		return;

	b->dt_no_ic_dimming = of_property_read_bool(np, "mi,disable-ic-dimming-flag");
	of_property_read_u32(np, "mi,doze-hbm-dbv-level", &b->doze_hbm_dbv);
	of_property_read_u32(np, "mi,doze-lbm-dbv-level", &b->doze_lbm_dbv);
	of_property_read_u32(np, "mi,local-hbm-ui-ready-delay-num-frame", &b->ui_ready_frames);

	of_property_read_u64(np, "mi,panel-id", &panel_id);
	b->is_m2 = panel_id == K618_M2_PANEL_ID;
	b->is_m1 = panel_id == K618_M1_PANEL_ID;
	b->is_m3 = panel_id == K618_M3_PANEL_ID;
	b->need_fod_anim = of_property_read_bool(np, "mi,need-fod-animal-in-normal-enabled");
	of_property_read_u32(np, "mi,normal-to-aod-doze-51reg-update-value", &b->normal_to_aod_51);
	k618_dc_parse(b, np);
	if (b->is_m1) {
		b->m1_build_read = of_property_read_bool(np, "mi,panel-build-id-read-needed");
		k618_m1_smem(b);
	}
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
		if (b->is_m1)
			k618_m1_parse_timing(&b->m1t[cell], child);
	}
	of_node_put(timings);

	pr_info("%s: panel id %016llx, M2 %d, em-cycle %d (threshold %#x, build id %#x), %d timings, DT IC dimming %s\n",
		panel->name, panel_id, b->is_m2, b->em_capable, b->thr, bid, pos,
		b->dt_no_ic_dimming ? "disabled" : "allowed");
	if (b->is_m1)
		pr_info("%s: 13 Ultra panel, build id read %s, lhbm calibration %s\n", panel->name,
			b->m1_build_read ? "after panel on" : "not needed",
			b->m1_lhbm_valid ? "from smem" : "as parsed");
	if (b->dc_enabled)
		pr_info("%s: DC dimming type %u, threshold %u\n", panel->name, b->dc_type, b->dc_thr);
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

/* ---- the 13 (M3) and 13 Ultra (M1) panels ---- */

/* the "-command-update" table `prop` of the active timing, read on the spot (absent = no entries) */
static bool k618_upd_now(struct dsi_panel *panel, const char *prop, struct k618_upd_set *set)
{
	struct device_node *timings, *child, *np = NULL;
	u32 want, cell, pos = 0;

	set->count = 0;
	if (!panel->cur_mode || !panel->cur_mode->priv_info || !panel->panel_of_node)
		return false;
	timings = of_get_child_by_name(panel->panel_of_node, "qcom,mdss-dsi-display-timings");
	if (!timings)
		return false;
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
		return false;
	k618_upd_parse(set, np, prop);
	of_node_put(np);
	return set->count > 0;
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
	struct k618_upd_set set;
	int i;

	if (!k618_upd_now(panel, update_prop, &set))
		return;
	for (i = 0; i < set.count; i++)
		if (set.e[i].reg == reg && set.e[i].len == len) {
			k618_patch(panel, type, &set.e[i], val, len);
			break;
		}
}

/*
 * M1 local-HBM 87h alpha by level, levels 0..0x146 (stock .rodata+0x7b254, 327 x u32 at file
 * offset 0x9f314 of the 5.15 msm_drm.ko); above 0x146 the alpha is 0xfff (0x19066c)
 */
static const u16 k618_m1_alpha[0x147] = {
	0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x2fe, 0x31d, 0x338, 0x359,
	0x36f, 0x388, 0x3ab, 0x3c7, 0x3e9, 0x403, 0x421, 0x442, 0x468, 0x487, 0x4a5, 0x4c9,
	0x4e9, 0x4fe, 0x510, 0x52a, 0x538, 0x54c, 0x567, 0x575, 0x58f, 0x5a2, 0x5b6, 0x5ca,
	0x5de, 0x5f0, 0x602, 0x61b, 0x62d, 0x640, 0x64d, 0x65d, 0x66e, 0x67f, 0x691, 0x6a1,
	0x6ae, 0x6c0, 0x6cc, 0x6dc, 0x6ec, 0x6fa, 0x70f, 0x71b, 0x727, 0x738, 0x744, 0x754,
	0x762, 0x771, 0x77f, 0x78d, 0x79c, 0x7ab, 0x7b3, 0x7c7, 0x7d4, 0x7e3, 0x7ee, 0x7fc,
	0x80c, 0x816, 0x822, 0x835, 0x842, 0x84d, 0x855, 0x866, 0x875, 0x87f, 0x88d, 0x89a,
	0x8a6, 0x8b2, 0x8bb, 0x8ca, 0x8d5, 0x8e4, 0x8e9, 0x8f7, 0x906, 0x90d, 0x91e, 0x926,
	0x92e, 0x93d, 0x943, 0x952, 0x95d, 0x969, 0x96f, 0x97a, 0x987, 0x98d, 0x9a0, 0x9a8,
	0x9ad, 0x9ba, 0x9c2, 0x9cd, 0x9da, 0x9e0, 0x9ec, 0x9f8, 0xa03, 0xa0a, 0xa17, 0xa1f,
	0xa27, 0xa37, 0xa3b, 0xa44, 0xa4f, 0xa59, 0xa63, 0xa6e, 0xa79, 0xa81, 0xa8a, 0xa95,
	0xaa1, 0xaaa, 0xab3, 0xaba, 0xac5, 0xacb, 0xad2, 0xadf, 0xae7, 0xaf1, 0xaf7, 0xb03,
	0xb0c, 0xb17, 0xb1d, 0xb21, 0xb2b, 0xb37, 0xb44, 0xb48, 0xb4d, 0xb58, 0xb63, 0xb6d,
	0xb72, 0xb78, 0xb83, 0xb88, 0xb93, 0xb98, 0xb9f, 0xba8, 0xbaf, 0xbb9, 0xbbf, 0xbca,
	0xbd1, 0xbd9, 0xbe4, 0xbec, 0xbf0, 0xbfa, 0xbff, 0xc07, 0xc10, 0xc1a, 0xc20, 0xc27,
	0xc2e, 0xc36, 0xc3e, 0xc42, 0xc4c, 0xc57, 0xc5c, 0xc63, 0xc69, 0xc72, 0xc79, 0xc7e,
	0xc89, 0xc93, 0xc9c, 0xca1, 0xcaa, 0xcb0, 0xcb9, 0xcbe, 0xcc4, 0xccd, 0xcd8, 0xce2,
	0xced, 0xcf3, 0xcf8, 0xcff, 0xd04, 0xd0f, 0xd15, 0xd1b, 0xd22, 0xd29, 0xd36, 0xd39,
	0xd41, 0xd4a, 0xd52, 0xd5a, 0xd64, 0xd67, 0xd6c, 0xd6e, 0xd7c, 0xd7d, 0xd85, 0xd8b,
	0xd90, 0xd9b, 0xd9d, 0xda8, 0xdab, 0xdb1, 0xdb7, 0xdb9, 0xdc2, 0xdc5, 0xdcb, 0xdd0,
	0xddb, 0xde0, 0xde4, 0xded, 0xdf2, 0xdf9, 0xe00, 0xe04, 0xe07, 0xe12, 0xe17, 0xe1f,
	0xe24, 0xe28, 0xe34, 0xe39, 0xe3e, 0xe45, 0xe46, 0xe4d, 0xe54, 0xe57, 0xe62, 0xe67,
	0xe73, 0xe74, 0xe7a, 0xe84, 0xe86, 0xe8c, 0xe92, 0xe97, 0xe9e, 0xea4, 0xeac, 0xeb3,
	0xeb6, 0xebb, 0xec2, 0xec9, 0xece, 0xed6, 0xeda, 0xedf, 0xee6, 0xef6, 0xef8, 0xefe,
	0xf07, 0xf0e, 0xf12, 0xf17, 0xf20, 0xf23, 0xf2c, 0xf32, 0xf38, 0xf3c, 0xf43, 0xf4d,
	0xf51, 0xf55, 0xf5f, 0xf65, 0xf6c, 0xf6e, 0xf72, 0xf84, 0xf84, 0xf92, 0xf94, 0xf99,
	0xfa3, 0xfa5, 0xfb1, 0xfb3, 0xfba, 0xfc2, 0xfca, 0xfcc, 0xfd4, 0xfda, 0xfe1, 0xfe7,
	0xfe9, 0xff5, 0xffb,
};

/*
 * M1 BAh of the 40, 30 and 24 Hz timing-switch commands, by the rate the panel comes from
 * (stock .rodata+0x89901 / 0x89913 / 0x89925, chosen at 0x185b80)
 */
static const u8 k618_m1_ba40[2][K618_M1_BA_LEN] = {
	{ 0x91, 0x01, 0x01, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00 },	/* from 90 or 120 Hz */
	{ 0x91, 0x02, 0x02, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00 },	/* from any other rate */
};
static const u8 k618_m1_ba30[2][K618_M1_BA_LEN] = {
	{ 0x91, 0x01, 0x01, 0x00, 0x01, 0x03, 0x03, 0x00, 0x00 },	/* from 90 or 120 Hz */
	{ 0x91, 0x03, 0x03, 0x00, 0x01, 0x03, 0x03, 0x00, 0x00 },	/* from any other rate */
};
static const u8 k618_m1_ba24[3][K618_M1_BA_LEN] = {
	{ 0x91, 0x03, 0x01, 0x00, 0x11, 0x04, 0x04, 0x00, 0x05 },	/* from 90 or 120 Hz */
	{ 0x91, 0x03, 0x03, 0x00, 0x11, 0x04, 0x04, 0x00, 0x05 },	/* from 60 Hz */
	{ 0x91, 0x04, 0x04, 0x00, 0x01, 0x04, 0x04, 0x00, 0x00 },	/* from any other rate */
};

/* M1 DBV bands (stock vrr_set_by_dbv 0x18f548..0x18f648): the lowest level of each */
#define K618_M1_BANDS 10
static const u32 k618_m1_band_floor[K618_M1_BANDS - 1] = {
	0x7ff, 0x333, 0x1c2, 0x147, 0x12b, 0x110, 0xf5, 0xdc, 0x84,
};
static const enum dsi_cmd_set_type k618_m1_dbi_set[K618_M1_BANDS] = {
	DSI_CMD_SET_MI_DBI_BWG_500_1300NIT, DSI_CMD_SET_MI_DBI_BWG_200_500NIT,
	DSI_CMD_SET_MI_DBI_BWG_110_200NIT, DSI_CMD_SET_MI_DBI_BWG_80_110NIT,
	DSI_CMD_SET_MI_DBI_BWG_72_79NIT, DSI_CMD_SET_MI_DBI_BWG_65_72NIT,
	DSI_CMD_SET_MI_DBI_BWG_60_65NIT, DSI_CMD_SET_MI_DBI_BWG_53_60NIT,
	DSI_CMD_SET_MI_DBI_BWG_30_53NIT, DSI_CMD_SET_MI_DBI_BWG_0_30NIT,
};
static const enum dsi_cmd_set_type k618_m1_skip_set[K618_M1_BANDS] = {
	DSI_CMD_SET_MI_SKIP_SOURCE_500_1300NIT, DSI_CMD_SET_MI_SKIP_SOURCE_200_500NIT,
	DSI_CMD_SET_MI_SKIP_SOURCE_110_200NIT, DSI_CMD_SET_MI_SKIP_SOURCE_80_110NIT,
	DSI_CMD_SET_MI_SKIP_SOURCE_72_79NIT, DSI_CMD_SET_MI_SKIP_SOURCE_65_72NIT,
	DSI_CMD_SET_MI_SKIP_SOURCE_60_65NIT, DSI_CMD_SET_MI_SKIP_SOURCE_53_60NIT,
	DSI_CMD_SET_MI_SKIP_SOURCE_30_53NIT, DSI_CMD_SET_MI_SKIP_SOURCE_0_30NIT,
};

/*
 * the build id the 26h/2Fh and DFh choices follow: the module parameter if set, else DAh as read
 * after panel on; 0 until then -- stock's panel+0x6cc when its read fails, which takes the same
 * branch as every id other than 0x40/0x50
 */
static u32 k618_m1_bid(const struct k618_bl *b)
{
	if (m1_build_id >= 0 && m1_build_id <= 0xff)
		return m1_build_id;
	return b->m1_build_id < 0 ? 0 : b->m1_build_id;
}

/* the active timing's M1 values, or NULL */
static const struct k618_m1_timing *k618_m1_t(const struct k618_bl *b, struct dsi_panel *p)
{
	u32 cell;

	if (!p->cur_mode)
		return NULL;
	cell = p->cur_mode->mode_idx;
	return cell < K618_TIMINGS && b->m1t[cell].valid ? &b->m1t[cell] : NULL;
}

/*
 * stock dsi_panel_switch_locked() tail (0x163c40): the rate the panel is left at -- the frame rate
 * in normal mode, the minimum rate in idle mode, the sf rate in auto/qsync mode, unchanged in the
 * diff/test modes
 */
static void k618_last_fps(struct k618_bl *b, struct dsi_panel *p)
{
	const struct k618_m1_timing *t = k618_m1_t(b, p);

	if (!p->cur_mode)
		return;
	if (!t || t->ddic_mode == 0)
		b->last_fps = p->cur_mode->timing.refresh_rate;
	else if (t->ddic_mode == 1)
		b->last_fps = t->dmin;
	else if (t->ddic_mode <= 3)
		b->last_fps = t->sf;
}

/* stock mi_dsi_panel_lgs_cie_by_temperature() (0x18f7c8): the 0-30 nit set by temperature */
static enum dsi_cmd_set_type k618_m1_dbi_temp(u32 bl, int t)
{
	if (t >= 32100) {
		if (bl > 0x7a)
			return DSI_CMD_SET_MI_DBI_BWG_45;
		if (bl > 0x4b)
			return DSI_CMD_SET_MI_DBI_BWG_46;
		if (bl > 0x27)
			return DSI_CMD_SET_MI_DBI_BWG_47;
		if (bl > 0x18)
			return DSI_CMD_SET_MI_DBI_BWG_48;
	} else if (t >= 28100) {
		if (bl > 0x7a)
			return DSI_CMD_SET_MI_DBI_BWG_41;
		if (bl > 0x4b)
			return DSI_CMD_SET_MI_DBI_BWG_42;
		if (bl > 0x27)
			return DSI_CMD_SET_MI_DBI_BWG_43;
		if (bl > 0x18)
			return DSI_CMD_SET_MI_DBI_BWG_44;
	}
	return DSI_CMD_SET_MI_DBI_BWG_0_30NIT;
}

/*
 * stock mi_dsi_panel_vrr_set_by_dbv() (0x18f4f4), M1, panel_lock held: the skip-source set and the
 * DBI-BWG set of the level's band, each sent only when it differs from the one last sent; the
 * 0-30 nit band's DBI set by the temperature in feature 20. Level 0 clears both (nothing sent).
 */
static void k618_m1_dbi(struct k618_bl *b, struct dsi_panel *p, u32 bl)
{
	enum dsi_cmd_set_type dbi, skip;
	int i, rc;

	if (!m1_dbi || !p->panel_initialized)
		return;
	if (!bl) {
		b->m1_skip = b->m1_dbi = DSI_CMD_SET_MAX;
		return;
	}
	for (i = 0; i < K618_M1_BANDS - 1 && bl < k618_m1_band_floor[i]; i++)
		;
	skip = k618_m1_skip_set[i];
	dbi = k618_m1_dbi_set[i];
	if (i == K618_M1_BANDS - 1 && g_mi)
		dbi = k618_m1_dbi_temp(bl, g_mi->feature_val[k618_panel_id(p)][DISP_FEATURE_DBI]);
	if (skip != b->m1_skip) {
		rc = dsi_panel_tx_cmd_set(p, skip, false);
		if (rc) {
			pr_err("%s: skip-source set %d at %u: %d\n", p->name, skip, bl, rc);
			return;
		}
		b->m1_skip = skip;
	}
	if (dbi != b->m1_dbi) {
		rc = dsi_panel_tx_cmd_set(p, dbi, false);
		if (rc) {
			pr_err("%s: DBI set %d at %u: %d\n", p->name, dbi, bl, rc);
			return;
		}
		b->m1_dbi = dbi;
		/* logged when the set changes, not for the resends after a timing switch */
		if (dbi != b->m1_dbi_shown) {
			b->m1_dbi_shown = dbi;
			pr_info("%s: DBI set %d, skip-source set %d, level %u\n", p->name, dbi,
				skip, bl);
		}
	}
}

/* the M1 commands mi_dsi_update_timing_switch_and_flat_mode_cmd() (0x185990) rewrites */
enum k618_m1_cmd {
	K618_M1_TIMING_SWITCH,
	K618_M1_POST_SWITCH,
	K618_M1_FLAT_ON,
	K618_M1_FLAT_OFF,
	K618_M1_DOZE_HBM,
	K618_M1_DOZE_LBM,
};

static const struct {
	enum dsi_cmd_set_type type;
	const char *upd;
} k618_m1_cmd_map[] = {
	[K618_M1_TIMING_SWITCH] = { DSI_CMD_SET_TIMING_SWITCH,
				    "qcom,mdss-dsi-timing-switch-command-update" },
	[K618_M1_POST_SWITCH]	= { DSI_CMD_SET_POST_TIMING_SWITCH,
				    "qcom,mdss-dsi-post-mode-switch-on-command-update" },
	[K618_M1_FLAT_ON]	= { DSI_CMD_SET_MI_FLAT_MODE_ON,
				    "mi,mdss-dsi-flat-mode-on-command-update" },
	[K618_M1_FLAT_OFF]	= { DSI_CMD_SET_MI_FLAT_MODE_OFF,
				    "mi,mdss-dsi-flat-mode-off-command-update" },
	[K618_M1_DOZE_HBM]	= { DSI_CMD_SET_MI_DOZE_HBM, "mi,mdss-dsi-doze-hbm-command-update" },
	[K618_M1_DOZE_LBM]	= { DSI_CMD_SET_MI_DOZE_LBM, "mi,mdss-dsi-doze-lbm-command-update" },
};

/*
 * stock mi_dsi_update_timing_switch_and_flat_mode_cmd() (0x185990) for M1: every 2Fh and 26h
 * (1 byte) entry of the command's update table gets the timing's cfg value a or b -- on build
 * 0x40/0x50 b with flat mode on and a with it off, on any other build the other way round
 * (0x185c30); flat mode is the HAL's state for the timing and post switch, on for flat-on and off
 * for flat-off and the doze commands; on build 0x40/0x50 the doze commands get 0/0 instead
 * (0x185c74). The timing switch also gets BAh (9 bytes) when the new mode runs at 40, 30 or 24 Hz,
 * by the rate it comes from (0x185b80). Stock's peak-HDR override (2Fh 0, 26h 7 outside 90 Hz) is
 * not here: this port has no peak-HDR mode.
 */
static void k618_m1_gamma(struct k618_bl *b, struct dsi_panel *p, enum k618_m1_cmd which)
{
	const struct k618_m1_timing *t = k618_m1_t(b, p);
	enum dsi_cmd_set_type type = k618_m1_cmd_map[which].type;
	const struct k618_upd_set *s;
	struct k618_upd_set set;
	const u8 *ba = NULL;
	u32 cur, last = b->last_fps, bid = k618_m1_bid(b);
	bool flat, b45 = (bid | 0x10) == 0x50;
	u8 v2f, v26;
	int i, idx;

	if (!m1_gamma || !t)
		return;
	if (which == K618_M1_FLAT_ON)
		flat = true;
	else if (which == K618_M1_TIMING_SWITCH || which == K618_M1_POST_SWITCH)
		flat = g_mi && g_mi->feature_val[k618_panel_id(p)][DISP_FEATURE_FLAT_MODE];
	else
		flat = false;
	idx = b45 ? flat : !flat;
	v2f = t->cfg2f[idx];
	v26 = t->cfg26[idx];
	if (b45 && (which == K618_M1_DOZE_HBM || which == K618_M1_DOZE_LBM))
		v2f = v26 = 0;

	if (which == K618_M1_TIMING_SWITCH) {
		/* the new mode's rate: sf rate in auto mode, minimum in idle, frame rate in normal */
		cur = t->ddic_mode == 2 ? t->sf : t->ddic_mode == 1 ? t->dmin :
		      t->ddic_mode == 0 ? p->cur_mode->timing.refresh_rate : 0;
		if (cur == 40)
			ba = k618_m1_ba40[(last == 90 || last == 120) ? 0 : 1];
		else if (cur == 30)
			ba = k618_m1_ba30[(last == 90 || last == 120) ? 0 : 1];
		else if (cur == 24)
			ba = k618_m1_ba24[(last == 90 || last == 120) ? 0 : last == 60 ? 1 : 2];
		s = k618_upd_get(b, p, K618_UPD_TIMING_SWITCH);
		if (!s)
			return;
		set = *s;
	} else if (!k618_upd_now(p, k618_m1_cmd_map[which].upd, &set)) {
		return;
	}
	for (i = 0; i < set.count; i++) {
		const struct k618_upd *e = &set.e[i];

		if (e->reg == 0x2f && e->len == 1)
			k618_patch(p, type, e, &v2f, 1);
		else if (e->reg == 0x26 && e->len == 1)
			k618_patch(p, type, e, &v26, 1);
		else if (e->reg == 0xba && e->len == K618_M1_BA_LEN && ba)
			k618_patch(p, type, e, ba, K618_M1_BA_LEN);
	}
}

/*
 * stock dsi_display_post_enable() (0x154bd4): on build 0x40/0x50 the doze commands of the mode the
 * panel is enabled in get 26h = 2Fh = 0, once per mode (priv_info+0x13e0)
 */
static void k618_m1_doze_gamma(struct k618_bl *b, struct dsi_panel *p)
{
	u32 cell;

	if (!b->is_m1 || !m1_gamma || (k618_m1_bid(b) | 0x10) != 0x50 || !p->cur_mode)
		return;
	cell = p->cur_mode->mode_idx;
	if (cell >= K618_TIMINGS || (b->m1_doze_done & BIT(cell)))
		return;
	k618_m1_gamma(b, p, K618_M1_DOZE_HBM);
	k618_m1_gamma(b, p, K618_M1_DOZE_LBM);
	b->m1_doze_done |= BIT(cell);
	pr_info("%s: doze 26h/2Fh cleared for timing %u (build id %#x)\n", p->name, cell,
		k618_m1_bid(b));
}

/* stock is_dc_on_skip_backlight() (0x18610c) */
static bool k618_dc_skip(const struct k618_bl *b, int id, u32 bl)
{
	return dc_skip && b->dc_enabled && b->dc_type == 2 && g_mi &&
	       g_mi->feature_val[id][DISP_FEATURE_DC] && bl && bl < b->dc_thr;
}

/* stock is_backlight_set_skip() first test (0x186168): the fingerprint area is lit */
static bool k618_fod_lit(const struct k618_bl *b, int id)
{
	int lhbm;

	if (b->lhbm_on)
		return true;
	if (!g_mi)
		return false;
	lhbm = g_mi->feature_val[id][DISP_FEATURE_LOCAL_HBM];
	return g_mi->feature_val[id][DISP_FEATURE_HBM_FOD] == 1 || (lhbm >= 1 && lhbm <= 7);
}

/* dsi_panel_switch_locked() where stock calls it from the nolp and AOD paths; panel_lock held */
static void k618_switch_locked(struct dsi_panel *p, const char *why)
{
	int rc;

	k618_mi_disp_timing_switch(p);
	rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_TIMING_SWITCH, false);
	if (rc)
		pr_err("%s: timing switch after %s: %d\n", p->name, why, rc);
	else
		pr_info("%s: timing switch sent again after %s (%u Hz)\n", p->name, why,
			p->cur_mode ? p->cur_mode->timing.refresh_rate : 0);
}

/*
 * stock sde_dsc_populate_dsc_config(dsc, scr, panel_id) (0x117bf4): for M1 (and two other Xiaomi
 * panels) the rate-control ranges come from tables of their own (.rodata+0x71070, 0x71214,
 * 0x713b8) instead of Qualcomm's (0x71142, 0x712e6, 0x7148a). In the row the 13 Ultra's modes use
 * (DSC 1.2, RGB 4:4:4, 10 bpc, 8 bpp: ratio index 4) they differ in three values:
 * range_min_qp[9] 7 -> 8, range_min_qp[13] 13 -> 12, range_bpg_offset[11] -12 -> -10;
 * range_max_qp, the buffer thresholds and the init parameters are the same. Applied after
 * dsi_panel_parse_dsc_params(), before the PPS is built from the config, and only where the
 * standard values are found. Stock's other M1 change there, first_line_bpg_offset = 13, is what
 * the 6.18 formula already gives for the panel's slice height of 20.
 */
void k618_mi_disp_dsc(struct dsi_panel *panel, struct dsi_display_mode *mode)
{
	struct drm_dsc_rc_range_parameters *r;
	struct drm_dsc_config *c;
	u64 id = 0;
	int n = 0;

	if (!m1_dsc || !panel || !panel->panel_of_node || !mode || !mode->priv_info ||
	    !mode->priv_info->dsc_enabled)
		return;
	if (of_property_read_u64(panel->panel_of_node, "mi,panel-id", &id) ||
	    id != K618_M1_PANEL_ID)
		return;
	c = &mode->priv_info->dsc.config;
	if (c->dsc_version_major != 1 || c->dsc_version_minor != 2 ||
	    c->bits_per_component != 10 || (c->bits_per_pixel >> 4) != 8 ||
	    c->native_420 || c->native_422)
		return;
	r = c->rc_range_params;
	if (r[9].range_min_qp == 7) {
		r[9].range_min_qp = 8;
		n++;
	}
	if (r[13].range_min_qp == 13) {
		r[13].range_min_qp = 12;
		n++;
	}
	if (r[11].range_bpg_offset == (u8)-12) {
		r[11].range_bpg_offset = (u8)-10;
		n++;
	}
	pr_info("%s: timing %u: %d DSC rate-control values set as stock\n", panel->name,
		mode->mode_idx, n);
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
	if (!b->is_m2) {
		int id = k618_panel_id(panel);

		/*
		 * stock is_backlight_set_skip() (0x186248): with DC on and a crc_skip_backlight
		 * panel, no 51h below the threshold -- unless the fingerprint area is lit (stock
		 * writes it then) or the write is one stock sends with dsi_panel_update_backlight()
		 * directly (features 22/23 and its own resends)
		 */
		if (!b->direct && !k618_fod_lit(b, id) && k618_dc_skip(b, id, bl)) {
			pr_debug("%s: 51h %u not sent, DC on (threshold %u)\n", panel->name, bl,
				 b->dc_thr);
			return false;
		}
		/* stock dsi_panel_update_backlight() 0x15a0f0: the 13 Ultra's sets go before 51h */
		if (b->is_m1)
			k618_m1_dbi(b, panel, bl);
		return true;
	}
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

/*
 * dsi_panel_switch(), before DSI_CMD_SET_TIMING_SWITCH (and k618_switch_locked()). Stock
 * dsi_panel_switch_locked() (0x163a60): on M1 the command's 26h/2Fh/BAh for the new mode, then the
 * DBI set sent again unless the mode's sf rate is 1 (0x163ae0); on M2 the em-cycle bytes; after
 * the command, the rate the panel is left at (0x163c40) -- set here, the command follows at once.
 */
void k618_mi_disp_timing_switch(struct dsi_panel *panel)
{
	struct k618_bl *b = k618_bl_get(panel);
	const struct k618_m1_timing *t;

	if (!b)
		return;
	if (b->is_m1) {
		k618_m1_gamma(b, panel, K618_M1_TIMING_SWITCH);
		t = k618_m1_t(b, panel);
		if (!t || t->sf != 1) {
			b->m1_dbi = DSI_CMD_SET_MAX;
			k618_m1_dbi(b, panel, b->last_bl);
		}
	}
	k618_em_update(b, panel, K618_UPD_TIMING_SWITCH, b->last_bl);
	k618_last_fps(b, panel);
}

/*
 * stock dsi_panel_post_switch() (0x163d3c): after a switch that left the panel at 1 Hz, the
 * post-switch command's 26h/2Fh and the DBI set again (the tables are the 13 Ultra's)
 */
void k618_mi_disp_post_switch(struct dsi_panel *panel)
{
	struct k618_bl *b = k618_bl_get(panel);

	if (!b || !b->is_m1 || b->last_fps != 1)
		return;
	k618_m1_gamma(b, panel, K618_M1_POST_SWITCH);
	b->m1_dbi = DSI_CMD_SET_MAX;
	k618_m1_dbi(b, panel, b->last_bl);
}

void k618_mi_disp_qsync(struct dsi_panel *panel, bool on)
{
	struct k618_bl *b = k618_bl_get(panel);

	if (!b || !b->is_m2)
		return;
	b->qsync_on = on;	/* before the rewrite, as stock: qsync on = no 0x80, "auto" table */
	k618_em_update(b, panel, on ? K618_UPD_QSYNC_ON : K618_UPD_QSYNC_OFF, b->last_bl);
}

/* ---- AOD: the doze levels ---- */

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
 * stock mi_dsi_panel_set_doze_brightness() (0x187efc), panel_lock held, panel in AOD (b->in_lp):
 * doze-hbm (32) or doze-lbm (33) for the level asked for, unless that level is on the panel
 * already ("has been set, skip") or the fingerprint HBM is lit (stock keeps the level and sends
 * nothing then). DOZE_TO_NORMAL sends nothing: nolp brings the panel back. Each command writes
 * 51h twice: the normal-mode level before 53h = 24 (AOD on) -- 07 0a in the DT, which stock
 * zeroes first (mi_dsi_update_51_mipi_cmd(panel, 32/33, 0) for M2 and M3), so no flash of it on the way
 * in -- and then the AOD level itself, fixed per command (HBM 07 fe, LBM 00 f6).
 */
static int k618_doze_send(struct k618_bl *b, struct dsi_panel *p)
{
	static const u8 dbv_0[2] = { 0, 0 };
	enum dsi_cmd_set_type type;
	const char *upd;
	u32 state;
	int rc;

	/*
	 * every panel. Stock 0x188044 clears the first 51h on M2 and M3 only; the 13 Ultra's
	 * doze update tables have no 51h entry, so its commands go as parsed either way
	 */
	if (!doze_cmd || !p->panel_initialized)
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
	if (b->is_m2 || b->is_m3)
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

/*
 * nolp = the ROM's DOZE -> ON. Its DT bytes: 53 20, 51 00 00, then 65 04 + BD 21 8F + the 16-pulse
 * table -- the panel comes out in 16-pulse mode at level 0. Stock patches the 51h with the doze
 * level's dbv and sets the 16-pulse state (mi_dsi_panel_set_nolp_locked): the framework writes the
 * real level next. With no doze level on the panel (doze_cmd=0, or the HAL never asked for one)
 * the level is the last one written: what the framework wrote during DOZE is on the panel.
 */
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

	if (!b)
		return;
	if (!rc) {
		from_doze = k618_in_doze(b);
		b->in_lp = false;
		b->panel_state = K618_PANEL_STATE_ON;
		if (b->is_m2) {
			b->em_16p = true;
			/*
			 * not in stock (its doze levels, 0x14 and 0xf5, are below the threshold): if
			 * the level on the panel is above it, leave 16-pulse now rather than at the
			 * next level change. Out of a doze level the panel is at that level's dbv,
			 * below it.
			 */
			if (!from_doze && b->last_bl > b->thr)
				k618_em_switch(b, panel, b->last_bl);
		}
	}
	/*
	 * stock dsi_display_set_power() (0x14b310): on the 13 Ultra the timing switch follows every
	 * nolp, whatever nolp returned
	 */
	if (b->is_m1)
		k618_switch_locked(panel, "nolp");
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
 * stock set_disp_param DISP_FEATURE_HBM (0x18c6b8):
 *   M2 and M3 (0x18c6cc / 0x18c6dc):
 *     on:  M2 em-cycle for 0x7ff (= 3 pulses); 53h of hbm-on = 0x28 on M2 (stock writes 0x38
 *          instead while the fingerprint area is lit; not done here), 0 on M3
 *          (mi_dsi_update_hbm_cmd_53reg 0x189a54 -- M3's tables have no 53h entry); send it
 *          (M2: 51h 07 ff, 63h 30, 49h 00);
 *     off: 51h = 0x7ff in hbm-off (the 51h its update table names first: M2's second 51h
 *          packet, M3's only one), 53h = 0x20 on M2, 0 on M3; send it;
 *     both: the remembered level becomes 0x7ff -- the HAL writes the real one next.
 *   M1 (0x18ce98): nothing is sent; the state is kept for the local-HBM level.
 *   Other panels: hbm-on / hbm-off as parsed.
 */
static int k618_hbm(struct dsi_display *d, int val)
{
	static const u8 dbv_7ff[2] = { 0x07, 0xff };
	static const u8 on53 = 0x28, off53 = 0x20, zero53;
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
	if (b && b->is_m1) {
		b->hbm_on = val == 1;
		rc = 0;
		goto out;
	}
	if (!b || !(b->is_m2 || b->is_m3)) {
		rc = dsi_panel_tx_cmd_set(p, val == 1 ? DSI_CMD_SET_MI_HBM_ON : DSI_CMD_SET_MI_HBM_OFF,
					  false);
		goto out;
	}
	if (val == 1) {
		if (b->is_m2) {
			k618_em_switch(b, p, 0x7ff);
			k618_patch_reg(b, p, K618_UPD_HBM_ON, 0x53, &on53, 1, false);
		} else {
			k618_patch_reg(b, p, K618_UPD_HBM_ON, 0x53, &zero53, 1, false);
		}
		rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_HBM_ON, false);
		b->hbm_on = true;
	} else {
		k618_patch_reg(b, p, K618_UPD_HBM_OFF, 0x51, dbv_7ff, 2, true);
		k618_patch_reg(b, p, K618_UPD_HBM_OFF, 0x53, b->is_m2 ? &off53 : &zero53, 1, false);
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
		if (b) {
			b->last_bl = val;
			/* stock sends these with dsi_panel_update_backlight(), past
			 * is_backlight_set_skip() */
			b->direct = true;
		}
		rc = dsi_panel_set_backlight(p, val);
		if (b)
			b->direct = false;
	}
	mutex_unlock(&p->panel_lock);
	return rc;
}

/*
 * flat mode on/off (stock set_disp_param DISP_FEATURE_FLAT_MODE, 0x18cd58 / 0x18cf38; 1 = on, any
 * other value off): on the 13 Ultra the command's 26h/2Fh for the new state go in first (0x18cf54)
 */
static int k618_flat(struct dsi_display *d, int val)
{
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
	if (b && b->is_m1)
		k618_m1_gamma(b, p, val == 1 ? K618_M1_FLAT_ON : K618_M1_FLAT_OFF);
	rc = dsi_panel_tx_cmd_set(p, val == 1 ? DSI_CMD_SET_MI_FLAT_MODE_ON :
				  DSI_CMD_SET_MI_FLAT_MODE_OFF, false);
out:
	mutex_unlock(&p->panel_lock);
	return rc;
}

/*
 * DC dimming on/off (stock set_disp_param case 8, 0x18c814): the state is kept for the 51h skip in
 * k618_mi_disp_bl_pre(), also with the panel off (0x18c614, -ENODEV then and nothing else);
 * stock's DC on/off command sets (42/43) are in none of these DTs. Event type 8 carries the value
 * -- sent here on the panels that declare mi,dc-feature-enabled.
 */
static int k618_dc(struct dsi_display *d, int disp_id, int val)
{
	struct dsi_panel *p = d->panel;
	struct k618_bl *b;
	bool dc = false;
	int rc = 0;

	g_mi->feature_val[disp_id][DISP_FEATURE_DC] = val;
	if (!p)
		return -ENODEV;
	mutex_lock(&p->panel_lock);
	if (!p->panel_initialized) {
		rc = -ENODEV;
	} else {
		b = k618_bl_get(p);
		if (b && b->dc_enabled) {
			dc = true;
			pr_info("%s: DC %s (type %u, threshold %u)\n", p->name, val ? "on" : "off",
				b->dc_type, b->dc_thr);
		}
	}
	mutex_unlock(&p->panel_lock);
	if (dc)
		k618_notify_u32(disp_id, MI_DISP_EVENT_DC, val);
	return rc;
}

/*
 * the panel temperature MiBrightness reports (stock set_disp_param case 20, 0x18c9bc / 0x18cc7c):
 * it picks the 13 Ultra's 0-30 nit DBI set, so at a level in that band (1..0x83), outside
 * LP1/LP2, the level goes out again and the set follows
 */
static int k618_dbi(struct dsi_display *d, int disp_id, int val)
{
	struct dsi_panel *p = d->panel;
	struct k618_bl *b;
	int rc = 0;

	g_mi->feature_val[disp_id][DISP_FEATURE_DBI] = val;
	if (!p)
		return -ENODEV;
	mutex_lock(&p->panel_lock);
	if (!p->panel_initialized) {
		rc = -ENODEV;
		goto out;
	}
	b = k618_bl_get(p);
	if (b && b->is_m1 && b->last_bl >= 1 && b->last_bl <= 0x83 &&
	    p->power_mode != SDE_MODE_DPMS_LP1 && p->power_mode != SDE_MODE_DPMS_LP2) {
		b->direct = true;
		rc = dsi_panel_set_backlight(p, b->last_bl);
		b->direct = false;
	}
out:
	mutex_unlock(&p->panel_lock);
	return rc;
}

/* ---- the fingerprint area: local HBM ---- */

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
 * stock set_lhbm_fod_locked(): the level each local-HBM command is set up for (its 51h and
 * mi_dsi_panel_update_lhbm_param's input): 1000 nit in HBM bl_max (M3: the last level, 0x18d570);
 * in LP1/LP2 the doze level's dbv -- 1000 nit the LBM one only for an LBM request, 110 nit the HBM
 * one only for an HBM request (0x18e220, 0x18ddec, 0x18df10, 0x18e034); else the last level
 */
static u32 k618_lhbm_bl(const struct k618_bl *b, struct dsi_panel *p, int state)
{
	bool lp = p->panel_initialized &&
		  (p->power_mode == SDE_MODE_DPMS_LP1 || p->power_mode == SDE_MODE_DPMS_LP2);

	switch (state) {
	case LOCAL_HBM_NORMAL_WHITE_1000NIT:
		if (b->hbm_on)
			return b->is_m3 ? b->last_bl : p->bl_config.bl_max_level;
		fallthrough;
	case LOCAL_HBM_HLPM_WHITE_1000NIT:
		if (lp)
			return b->doze_level == DOZE_BRIGHTNESS_LBM ? b->doze_lbm_dbv : b->doze_hbm_dbv;
		return b->last_bl;
	case LOCAL_HBM_NORMAL_WHITE_110NIT:
	case LOCAL_HBM_HLPM_WHITE_110NIT:
		if (lp)
			return b->doze_level == DOZE_BRIGHTNESS_HBM ? b->doze_hbm_dbv : b->doze_lbm_dbv;
		return b->last_bl;
	default:
		return b->last_bl;
	}
}

/*
 * stock mi_dsi_panel_update_lhbm_param() (0x19050c), M1: before a local-HBM command goes out,
 * every entry of its update table:
 *   87h, 2 bytes: the alpha for the level -- k618_m1_alpha[bl] up to 0x146, else 0xfff;
 *   DFh, 2 bytes: max(bl, 0x147) << 2;
 *   DFh, 1 byte:  0x20 or 0x22 -- build 0x40/0x50: 0x22 with flat mode on, 0x20 off; any other
 *                 build the other way round (0x19092c);
 *   D0h, 6 bytes: the SMEM gray levels (when valid) of the half for the flat state, at the
 *                 command's offset (1000 nit 0, 110 nit 18, green 24), 1000 nit and green scaled by
 *                 480/420 (475/420 with flat mode on). The green command's value is 2 bytes and
 *                 stock patches D0h only when the entry is that long; this DT's is 6, so its D0h
 *                 goes as parsed, as on stock.
 * The 750/500 nit and 16-pulse commands are not used here (stock: "unsupport cmd" for 16-pulse).
 */
static void k618_m1_lhbm(struct k618_bl *b, struct dsi_panel *p, int state,
			 enum dsi_cmd_set_type type)
{
	static const struct {
		enum dsi_cmd_set_type type;
		const char *upd;
		u8 off;		/* where its gray levels start in each SMEM half */
		bool calc;	/* scaled by 480/420 (475/420 with flat mode on) */
		bool green;	/* one value, in D0h's second pair */
	} m[] = {
		{ DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT,
		  "mi,mdss-dsi-local-hbm-normal-white-1000nit-command-update", 0, true, false },
		{ DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT,
		  "mi,mdss-dsi-local-hbm-hlpm-white-1000nit-command-update", 0, true, false },
		{ DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT,
		  "mi,mdss-dsi-local-hbm-normal-white-110nit-command-update", 18, false, false },
		{ DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT,
		  "mi,mdss-dsi-local-hbm-hlpm-white-110nit-command-update", 18, false, false },
		{ DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT,
		  "mi,mdss-dsi-local-hbm-normal-green-500nit-command-update", 24, true, true },
	};
	struct k618_upd_set set;
	u8 alpha[2], df2[2], df1, d0[6] = { 0 };
	u32 bl, a, v, f;
	bool flat, b45;
	int i, j, dlen;

	if (!b->is_m1 || !m1_lhbm_cal)
		return;
	for (i = 0; i < ARRAY_SIZE(m) && m[i].type != type; i++)
		;
	if (i == ARRAY_SIZE(m))
		return;
	if (b->m1_smem_rc == -EPROBE_DEFER)
		k618_m1_smem(b);
	bl = k618_lhbm_bl(b, p, state);
	flat = g_mi && g_mi->feature_val[k618_panel_id(p)][DISP_FEATURE_FLAT_MODE];
	b45 = (k618_m1_bid(b) | 0x10) == 0x50;

	a = bl <= 0x146 ? k618_m1_alpha[bl] : 0xfff;
	alpha[0] = (a >> 8) & 0xff;
	alpha[1] = a & 0xff;
	v = (bl > 0x146 ? bl : 0x147) << 2;
	df2[0] = (v >> 8) & 0xff;
	df2[1] = v & 0xff;
	df1 = b45 ? (flat ? 0x22 : 0x20) : (flat ? 0x20 : 0x22);

	dlen = m[i].green ? 2 : 6;
	if (b->m1_lhbm_valid) {
		u8 *dst = m[i].green ? d0 + 2 : d0;

		memcpy(dst, b->m1_lhbm + (flat ? K618_M1_LHBM_HALF : 0) + m[i].off, dlen);
		if (m[i].calc) {
			f = flat ? 475 : 480;
			for (j = 0; j < dlen; j += 2) {
				v = ((dst[j] << 8) | dst[j + 1]) * f / 420;
				dst[j] = (v >> 8) & 0xff;
				dst[j + 1] = v & 0xff;
			}
		}
	}
	if (!k618_upd_now(p, m[i].upd, &set))
		return;
	for (j = 0; j < set.count; j++) {
		const struct k618_upd *e = &set.e[j];

		if (e->reg == 0x87)
			k618_patch(p, type, e, alpha, 2);
		else if (e->reg == 0xdf && e->len == 2)
			k618_patch(p, type, e, df2, 2);
		else if (e->reg == 0xdf && e->len == 1)
			k618_patch(p, type, e, &df1, 1);
		else if (e->reg == 0xd0 && e->len == dlen && b->m1_lhbm_valid)
			k618_patch(p, type, e, d0, 6);
	}
	pr_info("%s: lhbm set %d for level %u: alpha %#x, DFh %#x, D0h %s\n", p->name, type, bl, a,
		df1, b->m1_lhbm_valid && !m[i].green ? "from smem" : "as parsed");
}

/*
 * stock set_lhbm_fod_locked() 0x18e198, after off-to-normal / off-to-hbm: at a last level in the
 * 0-30 nit band (up to 0x83) the 13 Ultra's DBI set and 51h go out again
 */
static void k618_m1_lhbm_off(struct k618_bl *b, struct dsi_panel *p)
{
	if (!b->is_m1 || b->last_bl > 0x83)
		return;
	b->m1_dbi = DSI_CMD_SET_MAX;
	b->direct = true;
	dsi_panel_set_backlight(p, b->last_bl);
	b->direct = false;
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
	/*
	 * every panel. The 13's M3 and the 13 Ultra's M1 have the normal / HLPM / off command
	 * sets and no 16-pulse ones, so em_16p (M2 only) stays false for them. The panel-specific
	 * parts of stock set_lhbm_fod_locked: M3's 1000 nit level in HBM (0x18d570), M1's
	 * calibration (k618_m1_lhbm) and DBI resend after the area goes off (0x18e198), the last
	 * level in the plain off-to-normal command on the panels other than M2 (0x18ddd4).
	 */
	if (!fod)
		goto out;
	aod = k618_in_doze(b);

	switch (state) {
	case LOCAL_HBM_NORMAL_WHITE_1000NIT:
		/*
		 * stock: bl_max in HBM (M3: the last level), the doze level's dbv in AOD, else the
		 * last level -- into the 3-pulse command's 51h; the 16-pulse twin has no
		 * "-command-update" of its own in this DT, so it goes as parsed
		 */
		type = k618_lhbm_cmd(state, b->em_16p);
		if (type == DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT) {
			lvl = b->hbm_on ? (b->is_m3 ? b->last_bl : p->bl_config.bl_max_level) :
					  (aod ? k618_doze_dbv(b) : b->last_bl);
			k618_dbv2(dbv, lvl);
			k618_patch_cmd_now(p, type,
					   "mi,mdss-dsi-local-hbm-normal-white-1000nit-command-update",
					   0x51, dbv, 2);
		}
		k618_m1_lhbm(b, p, state, type);
		rc = dsi_panel_tx_cmd_set(p, type, false);
		break;
	case LOCAL_HBM_NORMAL_WHITE_110NIT:
	case LOCAL_HBM_NORMAL_GREEN_500NIT:
		type = k618_lhbm_cmd(state, b->em_16p);
		k618_m1_lhbm(b, p, state, type);
		rc = dsi_panel_tx_cmd_set(p, type, false);
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
		k618_m1_lhbm(b, p, state, type);
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
			if (state == LOCAL_HBM_OFF_TO_NORMAL)
				k618_m1_lhbm_off(b, p);
			break;
		}
		/*
		 * off-to-normal (61, or its 16-pulse twin 62, each with a "-command-update" of its
		 * own): on M2 the composer's next backlight write restores the level after a plain
		 * one; on the other panels the plain one carries the last level (0x18ddd4); the
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
		} else if (!b->is_m2) {
			k618_dbv2(dbv, b->last_bl);
			k618_patch_cmd_now(p, type, "mi,mdss-dsi-local-hbm-off-to-normal-command-update",
					   0x51, dbv, 2);
		}
		rc = dsi_panel_tx_cmd_set(p, type, false);
		if (state == LOCAL_HBM_OFF_TO_NORMAL)
			k618_m1_lhbm_off(b, p);
		break;
	default:
		pr_debug("local hbm state %d: nothing for this panel\n", state);
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
 * AOD_TO_NORMAL (16), as stock mi_dsi_panel_aod_to_normal_optimize_locked() (0x18e258) runs it on
 * the panels with mi,need-fod-animal-in-normal-enabled, never while the fingerprint area is lit:
 *  1: the fingerprint animation layer showed up in AOD -- the panel comes out ahead of the
 *     framework with doze-lbm-nolp (35) / doze-hbm-nolp (34), by the doze level the HAL asked
 *     for, so the area lights before the screen; then, below 120 Hz, the 13 Pro and 13 Ultra
 *     send their timing switch again (0x18e4ec);
 *  0: back to AOD from there (0x18e37c) -- not while the panel is in a doze level, fully on, or
 *     the fingerprint HAL reports AUTH_STOP: doze-hbm (32) / doze-lbm (33) with its first 51h =
 *     mi,normal-to-aod-doze-51reg-update-value on M2/M3 (the doze level's dbv on other panels).
 * -EAGAIN where stock returns -11, -EINVAL with the panel off.
 */
static int k618_aod_to_normal(struct dsi_display *d, bool enable)
{
	struct dsi_panel *p = d->panel;
	enum dsi_cmd_set_type type = DSI_CMD_SET_MAX;
	u32 lvl = 0, state = K618_PANEL_STATE_ON;
	const char *upd = NULL;
	struct k618_bl *b;
	int id, lhbm, rc = 0;
	u8 dbv[2];

	if (!p)
		return -ENODEV;
	mutex_lock(&p->panel_lock);
	b = k618_bl_get(p);
	if (!b || !p->panel_initialized) {
		rc = -EINVAL;
		goto out;
	}
	if (!fod || !b->need_fod_anim)
		goto out;
	id = k618_panel_id(p);
	lhbm = g_mi ? g_mi->feature_val[id][DISP_FEATURE_LOCAL_HBM] : 0;
	if (g_mi && (g_mi->feature_val[id][DISP_FEATURE_HBM_FOD] == 1 || (lhbm >= 1 && lhbm <= 7))) {
		pr_info("%s: aod to normal %d not done, fingerprint area lit\n", p->name, enable);
		goto out;
	}
	if (enable) {
		if (b->panel_state == K618_PANEL_STATE_ON) {
			rc = -EAGAIN;
			goto out;
		}
		if (b->doze_level == DOZE_BRIGHTNESS_LBM)
			type = DSI_CMD_SET_MI_DOZE_LBM_NOLP;
		else if (b->doze_level == DOZE_BRIGHTNESS_HBM)
			type = DSI_CMD_SET_MI_DOZE_HBM_NOLP;
		else
			goto out;
		rc = dsi_panel_tx_cmd_set(p, type, false);
		if (!rc) {
			b->panel_state = K618_PANEL_STATE_ON;
			if (b->is_m2)
				b->em_16p = true;	/* stock 0x18e458 */
		}
		pr_info("%s: aod to normal: %d\n", p->name, rc);
		if (p->cur_mode && p->cur_mode->timing.refresh_rate != 120 && (b->is_m1 || b->is_m2))
			k618_switch_locked(p, "aod to normal");
		goto out;
	}
	if (b->panel_state != K618_PANEL_STATE_ON || p->power_mode == SDE_MODE_DPMS_ON ||
	    (g_mi && g_mi->feature_val[id][DISP_FEATURE_FP_STATUS] == K618_FP_AUTH_STOP)) {
		rc = -EAGAIN;
		goto out;
	}
	if (!doze_cmd)
		goto out;
	if (b->doze_level == DOZE_BRIGHTNESS_HBM) {
		type = DSI_CMD_SET_MI_DOZE_HBM;
		upd = "mi,mdss-dsi-doze-hbm-command-update";
		lvl = (b->is_m2 || b->is_m3) ? b->normal_to_aod_51 : b->doze_hbm_dbv;
		state = K618_PANEL_STATE_DOZE_HIGH;
	} else if (b->doze_level == DOZE_BRIGHTNESS_LBM) {
		type = DSI_CMD_SET_MI_DOZE_LBM;
		upd = "mi,mdss-dsi-doze-lbm-command-update";
		lvl = (b->is_m2 || b->is_m3) ? b->normal_to_aod_51 : b->doze_lbm_dbv;
		state = K618_PANEL_STATE_DOZE_LOW;
	} else {
		goto out;
	}
	k618_dbv2(dbv, lvl);
	k618_patch_cmd_now(p, type, upd, 0x51, dbv, 2);
	rc = dsi_panel_tx_cmd_set(p, type, false);
	b->panel_state = state;	/* stock sets it whatever the send returned (0x18e60c) */
	pr_info("%s: back to aod, doze level %u, first 51h %#x: %d\n", p->name, b->doze_level, lvl,
		rc);
out:
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
		rc = k618_flat(d, val);
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
		rc = k618_aod_to_normal(d, val == 1);
		break;
	case DISP_FEATURE_DC:
		rc = k618_dc(d, disp_id, val);
		break;
	case DISP_FEATURE_DBI:
		rc = k618_dbi(d, disp_id, val);
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

/*
 * the 13 Ultra's build id (stock mi_dsi_display_read_panel_build_id 0x183188, read in
 * dsi_display_enable() under the display lock): DAh through the panel's
 * mi,mdss-dsi-panel-build-id-read-command, once. dsi_display_cmd_receive() takes the display lock,
 * which the panel-on hook runs under, so this work item reads it once enable has returned.
 */
static void k618_build_id_fn(struct work_struct *w)
{
	struct k618_mi *mi = g_mi;
	struct dsi_display *d;
	struct dsi_panel *p;
	struct k618_bl *b;
	const void *cmd;
	u32 rlen = 1;
	u8 val[4] = { 0 };
	int id, len = 0, rc;

	if (!mi)
		return;
	for (id = 0; id < MI_DISP_MAX; id++)
		if (w == &mi->build_id_work[id])
			break;
	if (id == MI_DISP_MAX)
		return;
	d = mi->disp[id];
	p = d ? d->panel : NULL;
	if (!p || !p->panel_of_node)
		return;
	cmd = of_get_property(p->panel_of_node, "mi,mdss-dsi-panel-build-id-read-command", &len);
	of_property_read_u32(p->panel_of_node, "mi,panel-build-id-read-length", &rlen);
	if (!cmd || len < 8 || !rlen || rlen > sizeof(val))
		return;
	rc = dsi_display_cmd_receive(d, cmd, len, val, rlen, NULL);

	mutex_lock(&p->panel_lock);
	b = k618_bl_get(p);
	if (b && b->is_m1 && b->m1_build_id < 0) {
		if (rc > 0) {
			b->m1_build_id = val[0];
			pr_info("%s: build id %#x\n", p->name, val[0]);
			if (p->panel_initialized)
				k618_m1_doze_gamma(b, p);
		} else {
			pr_warn("%s: build id read, try %u: %d\n", p->name, b->m1_build_tries, rc);
		}
	}
	mutex_unlock(&p->panel_lock);
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
	if (b && b->is_m1) {
		/* stock dsi_panel_enable() 0x163f44: both caches cleared, the next 51h sends both sets */
		b->m1_skip = b->m1_dbi = DSI_CMD_SET_MAX;
		k618_m1_doze_gamma(b, panel);
		if (b->m1_build_read && b->m1_build_id < 0 && m1_build_id < 0 &&
		    b->m1_build_tries < 3) {
			b->m1_build_tries++;
			schedule_work(&mi->build_id_work[id]);
		}
	}
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

/*
 * the rest of what the stand-in (k618_midisp) answered, with the values the phone's own
 * kernel returns: read by displayfeature / the composer at start-up (cell_id, flatmode_check,
 * hw_vsync_info), written by the HAL (disp_count), and the panel's debug nodes (answered as
 * nothing, writes dropped)
 */
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
	for (id = 0; id < MI_DISP_MAX; id++) {
		INIT_DELAYED_WORK(&mi->dimming_work[id], k618_dimming_fn);
		INIT_WORK(&mi->build_id_work[id], k618_build_id_fn);
	}

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
		cancel_work_sync(&mi->build_id_work[id]);
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
