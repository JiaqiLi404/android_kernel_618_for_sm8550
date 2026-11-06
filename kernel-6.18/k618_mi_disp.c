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
 *        read once after the first panel on (msm_drm.build_id= sets it instead);
 *      - M1 local-HBM calibration: 87h alpha by level, DFh, D0h gray levels from SMEM item 498;
 *      - M3 DC dimming (mi,dc-feature-type = "crc_skip_backlight"): no 51h below the threshold
 *        while the HAL has DC on;
 *      - HBM: M3 on M2's path without the em-cycle switch, M1 sends nothing;
 *      - AOD_TO_NORMAL for both values as stock, with the M1/M2 timing switch after it below
 *        120 Hz, and the M1 timing switch after every nolp;
 *      - flat mode on only for value 1, as stock (any other value is off).
 *
 *  (7) from the same .ko:
 *      - the 13 Pro's second panel, 42 02 0b (mi,local-hbm-alpha-ctrl-aa-area): the 87h alpha of
 *        its local-HBM commands by level (stock aa_alpha_set), the direct 51h writes around them,
 *        dimming on again after the fingerprint area goes off, and the HBM state they read;
 *      - the 13 Ultra's peak HDR mode (feature 27): B0h..B8h read once after panel on plus the
 *        DT's peak values, the peak gamma command, 51h 0xfff, 26h/2Fh, no 51h while it is on;
 *      - per panel from the panel and the bootloader instead of fixed values: the panel name
 *        (msm_drm.dsi_display0), the white point (msm_drm.oled_wp / sec_oled_wp, else read from
 *        the panel), the build id (DAh after panel on wherever mi,panel-build-id-read-needed; it
 *        decides the 13 Pro's em-cycle switching), mi,max-brightness-clone and
 *        mi,panel-on-dimming-delay; SET_LOCAL_HBM only on mi,local-hbm-enabled panels.
 *
 *  (8) /sys/hwconf/hw_mon and hw_mon_info_no_clear: the display usage statistics as JSON (stock
 *      hwconf in the same .ko; see the section above k618_hw_init()). The ROM's other two names
 *      from that .ko are no sysfs nodes: "dc_backlight" is disp feature 18, which stock answers
 *      with "TODO" and nothing else (stored and accepted here, the default case of
 *      k618_set_feature()); "ddic_mode" is a key of the DSI connector's "mi_mode_info" blob
 *      property (with "mi_layer_info"), which is not ported (SDE connector properties and the
 *      mode matching behind them).
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
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <video/mipi_display.h>	/* MIPI_DSI_DCS_READ */

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
#define DISP_FEATURE_PEAK_HDR_MODE      27	/* the 13 Ultra's peak HDR (stock set_disp_param 0x18cab8) */
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
/*
 * -1: the build id is read from the panel (DAh) on the panels with mi,panel-build-id-read-needed
 * (the 13 Pro's 38 0c, the 13 Ultra); 0..255: this value is used instead. It decides the 13 Pro's
 * em-cycle switching and the 13 Ultra's 26h/2Fh and DFh choices; panel_build_id shows it.
 */
static int build_id = -1;
module_param(build_id, int, 0444);
/* 1: the 13 Ultra's peak HDR mode (feature 27) as stock; 0: the request is stored only */
static bool m1_peak = true;
module_param(m1_peak, bool, 0644);
/* 1: the 42 02 0b panel's local-HBM 87h alpha and direct 51h writes as stock; 0: as parsed */
static bool lhbm_alpha = true;
module_param(lhbm_alpha, bool, 0644);
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

/* empty: from msm_drm.dsi_display0/1, else the live panel's node name (k618_panel_name()) */
static char panel_name[128];
module_param_string(panel_name, panel_name, sizeof(panel_name), 0444);
/*
 * the white point under stock's parameter names, as the bootloader passes it per unit (8 hex
 * digits, msm_drm.oled_wp=...); empty: read from the panel (k618_wp())
 */
static char oled_wp[64];
module_param_string(oled_wp, oled_wp, sizeof(oled_wp), 0444);
static char sec_oled_wp[64];
module_param_string(sec_oled_wp, sec_oled_wp, sizeof(sec_oled_wp), 0444);
/* 0: the panel's mi,max-brightness-clone (0xfff without it, as stock); else this value */
static uint max_brightness_clone;
module_param(max_brightness_clone, uint, 0444);
static uint fps = 120;
module_param(fps, uint, 0444);
static uint owner_uid = 1000;                /* system:system, as init's rc chowns these files */
module_param(owner_uid, uint, 0444);
static uint owner_gid = 1000;
module_param(owner_gid, uint, 0444);
/* -1: the panel's mi,panel-on-dimming-delay (0 without it, as stock; 0x78 on all four); else ms */
static int panel_on_dimming_delay = -1;
module_param(panel_on_dimming_delay, int, 0644);
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

/*
 * ---- /sys/hwconf/hw_mon: the display usage statistics (the phone's 5.15 msm_drm "hwconf") ----
 *
 * Stock keeps a tree of named groups of key/value strings (hwconf_init(), register/add/
 * update_hw_monitor_info(); only msm_drm itself fills it) and prints it as JSON. The one group is
 * the primary panel's, "display", with the keys of mi_dsi_panel_count_init() in its order; the
 * values are fed by mi_dsi_panel_state_count() from the panel paths. Readers: PowerKeeper's
 * thermal IPA (the <N>fps_times keys: seconds at each refresh rate), miui-services' TrackUtil and
 * PowerInsight. Reading hw_mon zeroes the error counters (stock mi_dsi_panel_clean_data()); the
 * 18 Pro Max's msm_drm added hw_mon_info_no_clear, the same text without that.
 *
 * Fed here, as stock does, from the hooks this file already has: panel_active (seconds lit:
 * enable / nolp to disable / LP1 / LP2), the refresh-rate seconds (enable, nolp, timing switch;
 * AOD and off count for none), kernel_boottime, kernel_days, panel_id (from the panel name).
 * Left at stock's "0": the backlight averages, HBM, power-on cost, refresh count and the error
 * counters (ESD, TE lost, underrun, overflow, ping-pong timeout, long commit, cmdq timeout,
 * tp idle), which need hooks in SDE / DSI paths this port does not have.
 */
#define K618_HW_FPS_N 11
static int k618_panel_id(struct dsi_panel *panel);
static const char * const k618_hw_keys[] = {
	"panel_active", "panel_refresh", "kernel_boottime", "kernel_days", "bl_level_avg",
	"bl_level_high", "bl_level_low", "hbm_duration", "hbm_times", "tpidle_times",
	"1fps_times", "10fps_times", "24fps_times", "30fps_times", "40fps_times", "48fps_times",
	"50fps_times", "60fps_times", "90fps_times", "120fps_times", "144fps_times",
	"panel_id", "poweron_cost_avg", "esd_times", "te_lost_times", "underrun_times",
	"overflow_times", "pingpong_timeout_times", "commit_long_times", "cmdq_timeout_times",
};
#define K618_HW_KEYS ARRAY_SIZE(k618_hw_keys)
#define K618_HW_KEY_FPS 10	/* index of "1fps_times" */
static const u32 k618_hw_rates[K618_HW_FPS_N] = { 1, 10, 24, 30, 40, 48, 50, 60, 90, 120, 144 };

static struct {
	struct kobject *kobj;
	struct mutex lock;
	char *buf;			/* the printed text, PAGE_SIZE */
	u64 val[K618_HW_KEYS];		/* "%llu" values, 0 = stock's initial "0" */
	bool lit;			/* stock panel+0xd98 */
	unsigned long lit_since;	/* jiffies */
	u64 lit_jiffies;		/* stock panel+0xdb8 */
	int fps_idx;			/* K618_HW_FPS_N = none (off, AOD, a rate stock has no key for) */
	bool fps_started;
	unsigned long fps_since;
	u64 fps_jiffies[K618_HW_FPS_N];	/* stock panel+0xdf8 */
	time64_t days_since;		/* real time the day count runs from (stock panel+0xda8) */
	u64 days;			/* stock panel+0xdb0 */
} k618_hw = { .fps_idx = K618_HW_FPS_N };

/*
 * stock mi_dsi_panel_count_init(): " 0a " 1, " 0b " 2, " 0c " 3, " 0d " 4, first match wins, 1
 * when none matches (the 13 Pro's "xiaomi 38 0c 0a ..." is 1, its "42 02 0b" panel 2)
 */
static void k618_hw_panel_id(struct dsi_panel *panel)
{
	static const char * const ids[] = { " 0a ", " 0b ", " 0c ", " 0d " };
	int i;

	if (!k618_hw.buf || !panel || !panel->name || k618_panel_id(panel) != MI_DISP_PRIMARY)
		return;
	mutex_lock(&k618_hw.lock);
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		if (strstr(panel->name, ids[i]))
			break;
	k618_hw.val[21] = i < ARRAY_SIZE(ids) ? i + 1 : 1;
	mutex_unlock(&k618_hw.lock);
}

static u64 k618_hw_secs(u64 j)
{
	return div_u64(j, HZ);
}

/* stock mi_dsi_panel_fps_count(): the time at the previous rate goes to that rate's key */
static void k618_hw_fps_locked(u32 fps)
{
	unsigned long now = jiffies;
	int i, idx = K618_HW_FPS_N, old = k618_hw.fps_idx;

	for (i = 0; i < K618_HW_FPS_N; i++)
		if (k618_hw_rates[i] == fps)
			idx = i;
	if (!k618_hw.fps_started) {
		k618_hw.fps_started = true;
	} else if (idx != old && old < K618_HW_FPS_N) {
		if (time_after(now, k618_hw.fps_since))
			k618_hw.fps_jiffies[old] += now - k618_hw.fps_since;
		k618_hw.val[K618_HW_KEY_FPS + old] = k618_hw_secs(k618_hw.fps_jiffies[old]);
	} else if (idx == old) {
		return;
	}
	k618_hw.fps_since = now;
	k618_hw.fps_idx = idx;
}

/* stock mi_dsi_panel_active_count(): lit time, boot time and whole days, published at the end */
static void k618_hw_active_locked(bool on)
{
	unsigned long now = jiffies;
	time64_t real, d;

	if (on) {
		if (!k618_hw.lit) {
			k618_hw.lit = true;
			k618_hw.lit_since = now;
		}
		return;
	}
	if (!k618_hw.lit)
		return;
	k618_hw.lit = false;
	if (time_after(now, k618_hw.lit_since))
		k618_hw.lit_jiffies += now - k618_hw.lit_since;
	k618_hw.val[0] = k618_hw_secs(k618_hw.lit_jiffies);
	k618_hw.val[2] = div_u64(ktime_get_boottime_ns(), NSEC_PER_SEC);
	real = ktime_get_real_seconds();
	d = real - k618_hw.days_since;
	if (!k618_hw.days_since || d <= 0 || d > 315360000) {
		k618_hw.days_since = real;
	} else if (d >= 86401) {
		k618_hw.days += div_u64(d, 86400);
		k618_hw.days_since = real - (d % 86400);
	}
	k618_hw.val[3] = k618_hw.days;
}

/* stock mi_dsi_panel_state_count() events ACTIVE (0) and FPS (3), primary panel only */
static void k618_hw_state(struct dsi_panel *panel, bool lit)
{
	if (!k618_hw.buf || !panel || k618_panel_id(panel) != MI_DISP_PRIMARY)
		return;
	mutex_lock(&k618_hw.lock);
	k618_hw_active_locked(lit);
	k618_hw_fps_locked(lit && panel->cur_mode ? panel->cur_mode->timing.refresh_rate : 0);
	mutex_unlock(&k618_hw.lock);
}

static void k618_hw_fps(struct dsi_panel *panel)
{
	if (!k618_hw.buf || !panel || !panel->cur_mode || k618_panel_id(panel) != MI_DISP_PRIMARY)
		return;
	mutex_lock(&k618_hw.lock);
	if (k618_hw.lit)
		k618_hw_fps_locked(panel->cur_mode->timing.refresh_rate);
	mutex_unlock(&k618_hw.lock);
}

/* stock hw_item_print() for the root -> "display" -> keys tree; hw_mon_show() adds a newline */
static ssize_t k618_hw_print(char *out, bool clear)
{
	int i, n;

	mutex_lock(&k618_hw.lock);
	n = scnprintf(k618_hw.buf, PAGE_SIZE, "{\n%8s\"%s\" : {\n", "", "display");
	for (i = 0; i < K618_HW_KEYS; i++)
		n += scnprintf(k618_hw.buf + n, PAGE_SIZE - n, "%16s\"%s\" : \"%llu%s\n", "",
			       k618_hw_keys[i], k618_hw.val[i], i < K618_HW_KEYS - 1 ? "\"," : "\"");
	n += scnprintf(k618_hw.buf + n, PAGE_SIZE - n, "%8s}\n}\n", "");
	if (clear) {
		/*
		 * stock mi_dsi_panel_clean_data(): only the seven error counters (esd_times ..
		 * cmdq_timeout_times) start again; the times are totals since boot, which
		 * PowerKeeper takes differences of
		 */
		for (i = 23; i < K618_HW_KEYS; i++)
			k618_hw.val[i] = 0;
	}
	n = scnprintf(out, PAGE_SIZE, "%s\n", k618_hw.buf);
	mutex_unlock(&k618_hw.lock);
	return n;
}

static ssize_t hw_mon_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return k618_hw_print(buf, true);
}

static ssize_t hw_mon_info_no_clear_show(struct kobject *k, struct kobj_attribute *a, char *buf)
{
	return k618_hw_print(buf, false);
}

/* stock: writes are accepted and ignored */
static ssize_t k618_hw_store(struct kobject *k, struct kobj_attribute *a, const char *buf,
			     size_t n)
{
	return n;
}

static struct kobj_attribute k618_hw_mon_attr =
	__ATTR(hw_mon, 0644, hw_mon_show, k618_hw_store);
static struct kobj_attribute k618_hw_mon_nc_attr =
	__ATTR(hw_mon_info_no_clear, 0644, hw_mon_info_no_clear_show, k618_hw_store);
static struct attribute *k618_hw_attrs[] = {
	&k618_hw_mon_attr.attr,
	&k618_hw_mon_nc_attr.attr,
	NULL,
};
static const struct attribute_group k618_hw_group = { .attrs = k618_hw_attrs };

/* stock hwconf_init(): /sys/hwconf with its attribute group (the debugfs "hwconf_check" is left out) */
static void k618_hw_init(void)
{
	mutex_init(&k618_hw.lock);
	k618_hw.buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!k618_hw.buf)
		return;
	k618_hw.kobj = kobject_create_and_add("hwconf", NULL);
	if (!k618_hw.kobj || sysfs_create_group(k618_hw.kobj, &k618_hw_group)) {
		pr_err("hwconf: /sys/hwconf not created\n");
		kobject_put(k618_hw.kobj);
		k618_hw.kobj = NULL;
		kfree(k618_hw.buf);
		k618_hw.buf = NULL;
	}
}

static void k618_hw_exit(void)
{
	if (k618_hw.kobj) {
		sysfs_remove_group(k618_hw.kobj, &k618_hw_group);
		kobject_put(k618_hw.kobj);
		k618_hw.kobj = NULL;
	}
	kfree(k618_hw.buf);
	k618_hw.buf = NULL;
}

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

#define K618_UPD_ENTRIES 12	/* M2: up to 4 triples a command; the 13 Ultra peak gamma: 9 */
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
#define K618_PEAK_POINTS	75	/* the 13 Ultra's peak gamma: 25 points per colour */
#define K618_PEAK_LEN		(2 * K618_PEAK_POINTS)	/* B0h..B8h payloads: 18, 18, 14 bytes, 3 times */

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
	bool bid_needed;	/* mi,panel-build-id-read-needed (stock panel+0x97d), cleared once read */
	int bid;		/* DAh as read after panel on (stock panel+0x6cc); -1 = not read */
	int bid_failed;		/* reads of it that failed (k618_bid: the 38 0c's fallback) */
	bool em_tables;	/* M2 with the 1920 Hz threshold and the three em-cycle tables */
	u16 m1_doze_done;	/* timings whose doze 26h/2Fh were cleared (stock priv_info+0x13e0) */
	enum dsi_cmd_set_type m1_skip;	/* skip-source set last sent (stock panel+0xc58) */
	enum dsi_cmd_set_type m1_dbi;	/* DBI-BWG set last sent (stock panel+0xc5c) */
	enum dsi_cmd_set_type m1_dbi_shown;	/* the last one logged */
	u8 m1_lhbm[K618_M1_LHBM_LEN];	/* SMEM item 498 (stock panel+0xa48) */
	bool m1_lhbm_valid;	/* stock panel+0xa7c */
	int m1_smem_rc;

	/* local HBM of the 42 02 0b, panel-on dimming, the 13 Ultra's peak HDR */
	bool lhbm_enabled;	/* mi,local-hbm-enabled (stock panel+0x9fd) */
	bool lhbm_alpha;	/* mi,local-hbm-alpha-ctrl-aa-area (stock panel+0xa09): the 42 02 0b */
	bool lhbm_dimming;	/* the area went off, dimming on with the next level (stock 0xbe8 = 2) */
	u32 dimming_delay;	/* mi,panel-on-dimming-delay (stock panel+0xbe4) */
	bool peak_read;	/* the 13 Ultra's B0h..B8h read, b->peak made (stock panel+0xd90) */
	bool peak_sent;	/* the peak gamma command sent since panel on (stock panel+0xd91) */
	bool peak_on;		/* peak HDR on (stock feature_val[27], panel+0x958) */
	u8 peak[K618_PEAK_LEN];	/* B0h..B8h as read plus the DT's peak values (stock panel+0xc64) */
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

/*
 * the build id: msm_drm.build_id if set, else DAh as read after panel on, 0 until then. One
 * difference from stock: on the 13 Pro's 38 0c panel a read that FAILED gives 0xA0, the id this
 * phone's panel answered on 5.15 -- so a 6.18 DSI read that does
 * not work cannot take its 1920 Hz PWM switching away (stock: 0, i.e. no switching). A read that
 * works decides, as on stock.
 */
static u32 k618_bid(const struct k618_bl *b)
{
	if (build_id >= 0 && build_id <= 0xff)
		return build_id;
	if (b->bid < 0 && b->is_m2 && b->bid_failed)
		return 0xa0;
	return b->bid < 0 ? 0 : b->bid;
}

/*
 * stock dsi_panel_send_em_cycle_setting() / mi_dsi_update_em_cycle_cmd(): no em-cycle switching
 * for build id 0 (not read yet, panel+0x6cc as at power-on), 0x50, 0xb0 or 0xfb. Stock reads the
 * id before the first level is written (dsi_display_enable); so does this port (panel on).
 */
static void k618_em_eval(struct k618_bl *b)
{
	u32 bid = k618_bid(b);

	b->em_capable = b->em_tables && bid && bid != 0x50 && bid != 0xb0 && bid != 0xfb;
}

/* the dimming delay: the parameter if set, else the panel's mi,panel-on-dimming-delay */
static u32 k618_dimming_delay(const struct k618_bl *b)
{
	if (panel_on_dimming_delay >= 0)
		return panel_on_dimming_delay;
	return b ? b->dimming_delay : 0;
}

/* the panel-level properties and every timing's update tables, from the panel's DT node */
static void k618_bl_parse(struct k618_bl *b, struct dsi_panel *panel)
{
	struct device_node *np = panel->panel_of_node, *timings, *child;
	u64 panel_id = 0;
	int i, pos = 0;

	b->panel = panel;
	b->parsed = true;
	b->panel_state = K618_PANEL_STATE_ON;
	b->m1_skip = b->m1_dbi = b->m1_dbi_shown = DSI_CMD_SET_MAX;
	b->bid = -1;
	if (!np)
		return;

	b->dt_no_ic_dimming = of_property_read_bool(np, "mi,disable-ic-dimming-flag");
	of_property_read_u32(np, "mi,doze-hbm-dbv-level", &b->doze_hbm_dbv);
	of_property_read_u32(np, "mi,doze-lbm-dbv-level", &b->doze_lbm_dbv);
	of_property_read_u32(np, "mi,local-hbm-ui-ready-delay-num-frame", &b->ui_ready_frames);
	/* stock mi_dsi_panel_parse_config() 0x180498, 0x180648, 0x180d18 */
	b->lhbm_enabled = of_property_read_bool(np, "mi,local-hbm-enabled");
	b->lhbm_alpha = of_property_read_bool(np, "mi,local-hbm-alpha-ctrl-aa-area");
	of_property_read_u32(np, "mi,panel-on-dimming-delay", &b->dimming_delay);

	of_property_read_u64(np, "mi,panel-id", &panel_id);
	b->is_m2 = panel_id == K618_M2_PANEL_ID;
	b->is_m1 = panel_id == K618_M1_PANEL_ID;
	b->is_m3 = panel_id == K618_M3_PANEL_ID;
	b->need_fod_anim = of_property_read_bool(np, "mi,need-fod-animal-in-normal-enabled");
	of_property_read_u32(np, "mi,normal-to-aod-doze-51reg-update-value", &b->normal_to_aod_51);
	k618_dc_parse(b, np);
	/* stock parse_config 0x18037c; read after panel on (k618_read_build_id()) */
	b->bid_needed = of_property_read_bool(np, "mi,panel-build-id-read-needed");
	if (b->is_m1)
		k618_m1_smem(b);
	b->em_tables = b->is_m2 &&
		!of_property_read_u32(np, "mi,panel-1920pwm-dbv-threshold", &b->thr) && b->thr >= 3 &&
		k618_table(np, "mi,panel-em-cycle-3pulse-gamma-paramerer", b->table_3p) &&
		k618_table(np, "mi,panel-em-cycle-16pulse-gamma-paramerer", b->table_16p) &&
		k618_table(np, "mi,panel-em-cycle-gamma-paramerer-auto", b->table_auto);
	k618_em_eval(b);

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

	pr_info("%s: panel id %016llx, M2 %d, em-cycle tables %d (threshold %#x), build id %s, %d timings, DT IC dimming %s\n",
		panel->name, panel_id, b->is_m2, b->em_tables, b->thr,
		b->bid_needed ? "read after panel on" : "not read", pos,
		b->dt_no_ic_dimming ? "disabled" : "allowed");
	if (b->is_m1)
		pr_info("%s: 13 Ultra panel, lhbm calibration %s\n", panel->name,
			b->m1_lhbm_valid ? "from smem" : "as parsed");
	if (b->lhbm_alpha)
		pr_info("%s: local-HBM alpha control of the AA area\n", panel->name);
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

/* the active timing's DT node -- a reference the caller puts -- or NULL */
static struct device_node *k618_timing_node(struct dsi_panel *panel)
{
	struct device_node *timings, *child, *np = NULL;
	u32 want, cell, pos = 0;

	if (!panel->cur_mode || !panel->cur_mode->priv_info || !panel->panel_of_node)
		return NULL;
	timings = of_get_child_by_name(panel->panel_of_node, "qcom,mdss-dsi-display-timings");
	if (!timings)
		return NULL;
	want = panel->cur_mode->mode_idx;	/* = the timing's cell-index, as k618_bl_parse() */
	for_each_child_of_node(timings, child) {
		cell = pos++;
		of_property_read_u32(child, "cell-index", &cell);
		if (cell == want) {
			np = child;	/* the loop's reference, the caller's now */
			break;
		}
	}
	of_node_put(timings);
	return np;
}

/* the "-command-update" table `prop` of the active timing, read on the spot (absent = no entries) */
static bool k618_upd_now(struct dsi_panel *panel, const char *prop, struct k618_upd_set *set)
{
	struct device_node *np = k618_timing_node(panel);

	set->count = 0;
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
	return k618_bid(b);
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
 * by the rate it comes from (0x185b80). With peak HDR on (feature 27), 2Fh 0 and 26h 7 in every
 * one of these commands outside 90 Hz (0x185cb8).
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
	if (b->peak_on && p->cur_mode->timing.refresh_rate != 90) {
		v2f = 0;
		v26 = 7;
	}

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

/* ---- reads from the panel, and the 13 Ultra's peak HDR mode ---- */

/* a DCS read (06h) of reg, len bytes, low-power as stock (MIPI_DSI_MODE_LPM, 0x18b2b8); panel_lock held */
static int k618_dcs_read(struct dsi_panel *p, u8 reg, u8 *buf, u32 len)
{
	struct dsi_display *d = container_of(p->host, struct dsi_display, host);
	struct dsi_cmd_desc cmd = {};
	u8 tx = reg;

	cmd.msg.type = MIPI_DSI_DCS_READ;
	cmd.msg.tx_buf = &tx;
	cmd.msg.tx_len = 1;
	cmd.msg.rx_buf = buf;
	cmd.msg.rx_len = len;
	cmd.msg.flags = MIPI_DSI_MSG_UNICAST_COMMAND | MIPI_DSI_MSG_USE_LPM;
	cmd.last_command = true;
	return k618_dsi_display_read(d, &cmd, true);
}

/*
 * a read command of the panel node in the DT blob format (06 01 00 01 00 00 01 <reg>), low-power
 * when its "-state" is dsi_lp_mode, as stock mi_dsi_display_cmd_read_locked() takes it;
 * locked: panel_lock held by the caller (panel on), else the display and panel locks are taken
 */
static int k618_dt_read(struct dsi_panel *p, const char *prop, u8 *buf, u32 len, bool locked)
{
	struct dsi_display *d = container_of(p->host, struct dsi_display, host);
	struct dsi_cmd_desc cmd = {};
	const char *state = NULL;
	char sprop[96];
	const u8 *blob;
	int blen = 0;
	u8 tx[16];
	u32 dlen;

	if (!p->panel_of_node)
		return -ENODEV;
	blob = of_get_property(p->panel_of_node, prop, &blen);
	if (!blob || blen < 8)
		return -ENOENT;
	dlen = (blob[5] << 8) | blob[6];
	if (!dlen || dlen > sizeof(tx) || 7 + dlen > (u32)blen)
		return -EINVAL;
	memcpy(tx, blob + 7, dlen);
	snprintf(sprop, sizeof(sprop), "%s-state", prop);
	of_property_read_string(p->panel_of_node, sprop, &state);
	cmd.msg.type = blob[0];
	cmd.last_command = blob[1] == 1;
	cmd.msg.channel = blob[2];
	cmd.msg.flags = blob[3] | MIPI_DSI_MSG_UNICAST_COMMAND;
	if (state && !strcmp(state, "dsi_lp_mode"))
		cmd.msg.flags |= MIPI_DSI_MSG_USE_LPM;
	cmd.msg.tx_buf = tx;
	cmd.msg.tx_len = dlen;
	cmd.msg.rx_buf = buf;
	cmd.msg.rx_len = len;
	return k618_dsi_display_read(d, &cmd, locked);
}

/* the 13 Ultra's peak gamma registers B0h..B8h (stock .rodata+0x898f8): lengths, offsets in b->peak */
static const u8 k618_peak_len[9] = { 18, 18, 14, 18, 18, 14, 18, 18, 14 };
static const u8 k618_peak_off[9] = { 0, 18, 36, 50, 68, 86, 100, 118, 136 };

/*
 * stock mi_dsi_panel_read_peak_hdr_gamma() (0x18b23c), the 13 Ultra, once per boot after panel on
 * (dsi_display_enable 0x1544dc, and dsi_display_post_enable 0x154c78 while not done), panel_lock
 * held: the read-pre command (125), B0h..B8h read low-power (75 big-endian points), each point plus
 * the active timing's mi,mdss-dsi-panel-peak-hdr-gamma-value (0x18b45c; the same 75 values in all
 * nine timings), stored for the peak gamma command; then read-pre-disable (127). Stock sends 127
 * only after a complete read and leaves the panel in the read set-up otherwise; here it goes out
 * either way.
 */
static void k618_m1_peak_read(struct k618_bl *b, struct dsi_panel *p)
{
	u32 val[K618_PEAK_POINTS];
	u8 raw[K618_PEAK_LEN];
	struct device_node *np;
	int i, rc;

	if (!b->is_m1 || !m1_peak || b->peak_read || !p->panel_initialized)
		return;
	np = k618_timing_node(p);
	if (!np)
		return;
	rc = of_property_read_u32_array(np, "mi,mdss-dsi-panel-peak-hdr-gamma-value", val,
					K618_PEAK_POINTS);
	of_node_put(np);
	if (rc) {
		pr_info("%s: no peak hdr gamma values in this timing: %d\n", p->name, rc);
		return;
	}
	rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE, false);
	if (rc) {
		pr_err("%s: peak gamma read-pre: %d\n", p->name, rc);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(k618_peak_len); i++) {
		rc = k618_dcs_read(p, 0xb0 + i, raw + k618_peak_off[i], k618_peak_len[i]);
		if (rc != k618_peak_len[i]) {
			pr_err("%s: peak gamma read %02xh: %d\n", p->name, 0xb0 + i, rc);
			break;
		}
	}
	if (i == ARRAY_SIZE(k618_peak_len)) {
		for (i = 0; i < K618_PEAK_POINTS; i++) {
			u32 v = ((raw[2 * i] << 8) | raw[2 * i + 1]) + val[i];

			b->peak[2 * i] = (v >> 8) & 0xff;
			b->peak[2 * i + 1] = v & 0xff;
		}
		b->peak_read = true;
	}
	rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE_DISABLE, false);
	pr_info("%s: peak gamma %s, read-pre-disable %d\n", p->name,
		b->peak_read ? "read" : "not read", rc);
}

/*
 * stock mi_dsi_panel_cal_send_peak_hdr_gamma() (0x18bfb8): the B0h..B8h entries of the peak gamma
 * command's update table get b->peak (18 or 14 bytes each), then read-pre (125) and the peak gamma
 * command (126) go out; sent counts as done whatever it returned (0x18c1bc), until panel off
 */
static int k618_m1_peak_send(struct k618_bl *b, struct dsi_panel *p)
{
	struct k618_upd_set set;
	int i, r, rc;

	k618_upd_now(p, "mi,mdss-dsi-peak-gamma-command-update", &set);
	for (i = 0; i < set.count; i++) {
		const struct k618_upd *e = &set.e[i];

		if (e->reg < 0xb0 || e->reg > 0xb8)
			continue;
		r = e->reg - 0xb0;
		k618_patch(p, DSI_CMD_SET_MI_PEAK_GAMMA, e, b->peak + k618_peak_off[r], k618_peak_len[r]);
	}
	dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_PEAK_GAMMA_READ_PRE, false);
	rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_PEAK_GAMMA, false);
	b->peak_sent = true;
	return rc;
}

/* peak HDR off: the flat-off command with the normal 26h/2Fh (stock 0x18c88c, 0x186444) */
static void k618_m1_peak_off(struct k618_bl *b, struct dsi_panel *p, const char *why)
{
	int rc;

	b->peak_on = false;
	if (g_mi)
		g_mi->feature_val[k618_panel_id(p)][DISP_FEATURE_PEAK_HDR_MODE] = 0;
	k618_m1_gamma(b, p, K618_M1_FLAT_OFF);
	rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_FLAT_MODE_OFF, false);
	pr_info("%s: peak hdr off (%s): %d\n", p->name, why, rc);
}

/*
 * feature 27, stock set_disp_param (0x18cab8), the 13 Ultra: on, the level goes to 0xfff first
 * (dsi_panel_update_backlight(): 51h and the DBI set; the remembered level stays the HAL's); the
 * peak gamma command once per panel on; the value kept; then the flat-off command with the
 * 26h/2Fh for the new state (0 and 7 outside 90 Hz while on) -- its result is the feature's. While
 * on, levels are not written (k618_mi_disp_bl_pre()); level 0, a local-HBM request and panel off
 * end it. Stock writes the peak gamma even when the read failed (an all-zero table); here on is
 * refused then (-EIO). Other panels keep the value only.
 */
static int k618_peak(struct dsi_display *d, int disp_id, int val)
{
	struct dsi_panel *p = d->panel;
	struct k618_bl *b;
	u32 keep;
	int rc = 0;

	if (!p)
		return -ENODEV;
	mutex_lock(&p->panel_lock);
	if (!p->panel_initialized) {
		rc = -ENODEV;
		goto out;
	}
	b = k618_bl_get(p);
	if (!b || !b->is_m1 || !m1_peak)
		goto out;
	if (!b->peak_sent && !b->peak_read)
		k618_m1_peak_read(b, p);	/* not read after panel on: once more now */
	if (val && !b->peak_read) {
		pr_err("%s: peak hdr on refused: B0h..B8h were not read\n", p->name);
		rc = -EIO;
		goto out;
	}
	if (val) {
		keep = b->last_bl;
		b->direct = true;
		dsi_panel_set_backlight(p, 0xfff);
		b->direct = false;
		b->last_bl = keep;
	}
	if (!b->peak_sent && b->peak_read) {
		rc = k618_m1_peak_send(b, p);
		if (rc)
			pr_err("%s: peak gamma: %d\n", p->name, rc);
	}
	b->peak_on = val != 0;
	if (g_mi)
		g_mi->feature_val[disp_id][DISP_FEATURE_PEAK_HDR_MODE] = val;
	k618_m1_gamma(b, p, K618_M1_FLAT_OFF);
	rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_FLAT_MODE_OFF, false);
	pr_info("%s: peak hdr %d (%u Hz): %d\n", p->name, val,
		p->cur_mode ? p->cur_mode->timing.refresh_rate : 0, rc);
out:
	mutex_unlock(&p->panel_lock);
	return rc;
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
		/*
		 * stock is_backlight_set_skip() (0x186398), the 13 Ultra in peak HDR: no 51h for a
		 * level (the panel stays at 0xfff); level 0 ends peak HDR and goes out
		 */
		if (b->is_m1 && b->peak_on && !b->direct && !k618_fod_lit(b, id)) {
			if (bl) {
				pr_debug("%s: 51h %u not sent, peak HDR on\n", panel->name, bl);
				return false;
			}
			k618_m1_peak_off(b, panel, "level 0");
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
	k618_hw_fps(panel);	/* stock dsi_display_set_mode(): the new mode's rate */
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

	if (!rc)
		k618_hw_state(panel, false);	/* stock dsi_panel_set_lp1/lp2(): not lit, no rate */
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

	if (!rc)
		k618_hw_state(panel, true);	/* stock dsi_panel_set_nolp(): lit, the mode's rate */
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

	k618_hw_state(panel, false);	/* stock dsi_panel_disable(): not lit, no rate */
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
	b->lhbm_dimming = false;
	/*
	 * stock dsi_panel_disable() 0x1641cc / 0x1642a0: peak HDR off, its gamma sent again after
	 * the next panel on. Stock also sends the flat-off command first; this hook runs after the
	 * panel is off, so that is left out (the next 26h/2Fh rewrite recomputes it).
	 */
	b->peak_on = false;
	b->peak_sent = false;
	if (g_mi) {
		g_mi->feature_val[k618_panel_id(panel)][DISP_FEATURE_HBM] = 0;
		g_mi->feature_val[k618_panel_id(panel)][DISP_FEATURE_PEAK_HDR_MODE] = 0;
	}
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
 *   Other panels: hbm-on / hbm-off as parsed, the state kept for the local-HBM paths (stock
 *   keeps feature_val[HBM] on every panel; set_lhbm_fod_locked reads it, 0x18d4e0 / 0x18d544).
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
		if (b)
			b->hbm_on = val == 1;
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
 * stock .rodata+0x77254 aa_alpha_set (file offset 0x9b314 of the 5.15 msm_drm.ko): the 87h alpha of
 * the 42 02 0b panel's local-HBM commands by level. 4096 u32 there; every one from 0x7ff on is
 * 0xfff, so levels 0..0x7ff are kept here.
 */
static const u16 k618_aa_alpha[0x800] = {
	0x000, 0x071, 0x0af, 0x0d2, 0x0f0, 0x10a, 0x121, 0x135, 0x14d, 0x15b, 0x16c, 0x17a,
	0x188, 0x196, 0x1a4, 0x1b2, 0x1bd, 0x1cb, 0x1d6, 0x1e2, 0x1f1, 0x1fe, 0x209, 0x214,
	0x224, 0x22c, 0x232, 0x23c, 0x246, 0x24f, 0x258, 0x261, 0x26a, 0x273, 0x27b, 0x282,
	0x28a, 0x294, 0x29c, 0x2a4, 0x2ac, 0x2b4, 0x2bb, 0x2c5, 0x2ca, 0x2d6, 0x2dd, 0x2e4,
	0x2ed, 0x2f6, 0x2fd, 0x302, 0x309, 0x30d, 0x314, 0x31d, 0x323, 0x326, 0x32a, 0x330,
	0x337, 0x33d, 0x343, 0x349, 0x34f, 0x355, 0x35b, 0x361, 0x367, 0x36d, 0x372, 0x378,
	0x37c, 0x383, 0x387, 0x38c, 0x394, 0x399, 0x39f, 0x3a4, 0x3aa, 0x3af, 0x3b4, 0x3b9,
	0x3bf, 0x3c4, 0x3c9, 0x3ce, 0x3d3, 0x3d8, 0x3dd, 0x3e2, 0x3e7, 0x3f0, 0x3f7, 0x3fa,
	0x401, 0x407, 0x40a, 0x40f, 0x416, 0x418, 0x41f, 0x424, 0x426, 0x42d, 0x432, 0x436,
	0x439, 0x43f, 0x444, 0x448, 0x44b, 0x44f, 0x457, 0x45c, 0x45e, 0x463, 0x467, 0x46d,
	0x46f, 0x476, 0x47a, 0x47c, 0x484, 0x485, 0x48d, 0x48f, 0x495, 0x495, 0x49d, 0x49f,
	0x4a3, 0x4a9, 0x4ad, 0x4af, 0x4b7, 0x4b9, 0x4bd, 0x4c3, 0x4c5, 0x4c7, 0x4cb, 0x4d1,
	0x4d5, 0x4db, 0x4df, 0x4e0, 0x4e6, 0x4e8, 0x4ec, 0x4f0, 0x4f5, 0x4f9, 0x4fd, 0x4ff,
	0x504, 0x50a, 0x50c, 0x50f, 0x515, 0x519, 0x51a, 0x51e, 0x522, 0x527, 0x52b, 0x52e,
	0x52e, 0x535, 0x537, 0x53a, 0x53e, 0x545, 0x547, 0x548, 0x54e, 0x553, 0x555, 0x558,
	0x55a, 0x561, 0x561, 0x564, 0x567, 0x56d, 0x56e, 0x573, 0x575, 0x57a, 0x57d, 0x57f,
	0x584, 0x589, 0x58b, 0x58e, 0x591, 0x592, 0x596, 0x597, 0x59c, 0x59f, 0x5a1, 0x5a2,
	0x5a7, 0x5aa, 0x5ab, 0x5af, 0x5b2, 0x5b7, 0x5b8, 0x5bb, 0x5be, 0x5c0, 0x5c1, 0x5c4,
	0x5c9, 0x5ca, 0x5cb, 0x5d4, 0x5d5, 0x5da, 0x5dd, 0x5e0, 0x5e1, 0x5e4, 0x5e6, 0x5e7,
	0x5ec, 0x5eb, 0x5f2, 0x5f1, 0x5f4, 0x5f5, 0x5f9, 0x5fa, 0x5ff, 0x602, 0x607, 0x606,
	0x609, 0x60c, 0x60f, 0x612, 0x613, 0x618, 0x61d, 0x61f, 0x620, 0x623, 0x626, 0x629,
	0x62e, 0x62f, 0x634, 0x634, 0x637, 0x63a, 0x63d, 0x63c, 0x642, 0x645, 0x648, 0x649,
	0x64e, 0x64e, 0x653, 0x658, 0x659, 0x65b, 0x65e, 0x661, 0x662, 0x66c, 0x66f, 0x670,
	0x673, 0x677, 0x678, 0x67b, 0x67f, 0x680, 0x683, 0x685, 0x688, 0x68b, 0x691, 0x690,
	0x695, 0x695, 0x698, 0x69b, 0x69d, 0x69c, 0x69f, 0x6a3, 0x6a4, 0x6aa, 0x6ad, 0x6ac,
	0x6b6, 0x6b9, 0x6bb, 0x6bc, 0x6be, 0x6c1, 0x6c4, 0x6c6, 0x6cb, 0x6cb, 0x6cc, 0x6d2,
	0x6d5, 0x6d9, 0x6da, 0x6dd, 0x6dd, 0x6e2, 0x6e2, 0x6e5, 0x6e7, 0x6ea, 0x6ec, 0x6ed,
	0x6f1, 0x6f4, 0x6fc, 0x6fd, 0x6ff, 0x701, 0x700, 0x700, 0x703, 0x707, 0x708, 0x708,
	0x709, 0x70b, 0x711, 0x710, 0x714, 0x713, 0x713, 0x716, 0x71a, 0x71c, 0x71d, 0x71d,
	0x71e, 0x722, 0x722, 0x725, 0x727, 0x729, 0x72c, 0x726, 0x729, 0x72b, 0x72d, 0x730,
	0x732, 0x734, 0x737, 0x739, 0x73b, 0x73e, 0x740, 0x73a, 0x73d, 0x73f, 0x741, 0x744,
	0x746, 0x748, 0x74a, 0x74d, 0x74f, 0x751, 0x754, 0x74e, 0x758, 0x75a, 0x75d, 0x75f,
	0x759, 0x75c, 0x75e, 0x760, 0x762, 0x765, 0x767, 0x769, 0x76b, 0x76e, 0x770, 0x772,
	0x76c, 0x76e, 0x771, 0x773, 0x775, 0x777, 0x77a, 0x77c, 0x77e, 0x780, 0x782, 0x785,
	0x787, 0x789, 0x78b, 0x78d, 0x788, 0x78a, 0x78c, 0x78e, 0x790, 0x792, 0x795, 0x797,
	0x799, 0x79b, 0x79d, 0x79f, 0x7a1, 0x7a4, 0x7a6, 0x7a8, 0x7a2, 0x7a4, 0x7a6, 0x7a8,
	0x7ab, 0x7ad, 0x7af, 0x7b1, 0x7b3, 0x7b5, 0x7b7, 0x7b9, 0x7bb, 0x7b6, 0x7c0, 0x7c2,
	0x7c4, 0x7c6, 0x7c0, 0x7ca, 0x7c4, 0x7ce, 0x7c8, 0x7d2, 0x7d5, 0x7cf, 0x7d1, 0x7d3,
	0x7d5, 0x7d7, 0x7d9, 0x7db, 0x7dd, 0x7df, 0x7e1, 0x7db, 0x7e5, 0x7df, 0x7e1, 0x7e3,
	0x7e5, 0x7e7, 0x7e9, 0x7eb, 0x7ed, 0x7f0, 0x7f2, 0x7f4, 0x7f6, 0x7f8, 0x7fa, 0x7fc,
	0x7fe, 0x800, 0x802, 0x804, 0x806, 0x800, 0x80a, 0x804, 0x806, 0x808, 0x812, 0x814,
	0x80e, 0x80f, 0x811, 0x813, 0x815, 0x817, 0x819, 0x81b, 0x81d, 0x81f, 0x821, 0x823,
	0x825, 0x827, 0x829, 0x82b, 0x82d, 0x82f, 0x831, 0x833, 0x835, 0x837, 0x839, 0x83a,
	0x83c, 0x83e, 0x838, 0x842, 0x844, 0x846, 0x840, 0x84a, 0x844, 0x84e, 0x850, 0x851,
	0x853, 0x855, 0x857, 0x859, 0x85b, 0x85d, 0x85f, 0x861, 0x863, 0x864, 0x866, 0x868,
	0x86a, 0x86c, 0x86e, 0x870, 0x872, 0x873, 0x875, 0x877, 0x879, 0x87b, 0x87d, 0x87f,
	0x881, 0x882, 0x884, 0x886, 0x888, 0x88a, 0x88c, 0x88d, 0x88f, 0x891, 0x893, 0x895,
	0x897, 0x899, 0x89a, 0x89c, 0x89e, 0x8a0, 0x8a2, 0x8a4, 0x8a5, 0x8a7, 0x8a9, 0x8ab,
	0x8ad, 0x8ae, 0x8b8, 0x8ba, 0x8bc, 0x8b6, 0x8bf, 0x8c1, 0x8c3, 0x8c5, 0x8c7, 0x8c8,
	0x8ca, 0x8cc, 0x8ce, 0x8d0, 0x8d1, 0x8d3, 0x8d5, 0x8d7, 0x8d9, 0x8e2, 0x8dc, 0x8de,
	0x8e0, 0x8e1, 0x8e3, 0x8e5, 0x8e7, 0x8e9, 0x8ea, 0x8ec, 0x8ee, 0x8f0, 0x8f1, 0x8f3,
	0x8f5, 0x8f7, 0x900, 0x902, 0x904, 0x906, 0x907, 0x909, 0x90b, 0x90d, 0x90e, 0x910,
	0x912, 0x914, 0x915, 0x917, 0x919, 0x91b, 0x91c, 0x91e, 0x920, 0x921, 0x923, 0x925,
	0x927, 0x928, 0x92a, 0x92c, 0x92d, 0x937, 0x931, 0x933, 0x93c, 0x93e, 0x938, 0x939,
	0x93b, 0x93d, 0x93f, 0x948, 0x94a, 0x944, 0x94d, 0x94f, 0x951, 0x952, 0x954, 0x956,
	0x957, 0x959, 0x95b, 0x95d, 0x95e, 0x960, 0x962, 0x963, 0x96d, 0x967, 0x970, 0x972,
	0x96c, 0x96d, 0x96f, 0x979, 0x97a, 0x97c, 0x97e, 0x977, 0x981, 0x983, 0x984, 0x986,
	0x988, 0x989, 0x98b, 0x98d, 0x98e, 0x990, 0x991, 0x99b, 0x995, 0x996, 0x998, 0x99a,
	0x99b, 0x9a5, 0x99f, 0x9a8, 0x9a2, 0x9a3, 0x9ad, 0x9af, 0x9b0, 0x9b2, 0x9ac, 0x9b5,
	0x9b7, 0x9b8, 0x9ba, 0x9bc, 0x9bd, 0x9bf, 0x9c1, 0x9c2, 0x9c4, 0x9c5, 0x9c7, 0x9c9,
	0x9d2, 0x9cc, 0x9d5, 0x9d7, 0x9d1, 0x9d2, 0x9dc, 0x9dd, 0x9df, 0x9e1, 0x9e2, 0x9e4,
	0x9e5, 0x9e7, 0x9e9, 0x9ea, 0x9ec, 0x9ed, 0x9ef, 0x9f1, 0x9f2, 0x9f4, 0x9f5, 0x9f7,
	0xa01, 0xa02, 0xa04, 0xa05, 0x9ff, 0xa08, 0xa0a, 0xa0c, 0xa0d, 0xa0f, 0xa10, 0xa12,
	0xa13, 0xa1d, 0xa16, 0xa18, 0xa1a, 0xa1b, 0xa25, 0xa1e, 0xa28, 0xa21, 0xa2b, 0xa2d,
	0xa2e, 0xa30, 0xa31, 0xa33, 0xa34, 0xa36, 0xa37, 0xa39, 0xa3a, 0xa3c, 0xa3e, 0xa3f,
	0xa41, 0xa42, 0xa4c, 0xa45, 0xa4f, 0xa50, 0xa52, 0xa53, 0xa55, 0xa56, 0xa58, 0xa59,
	0xa5b, 0xa5d, 0xa5e, 0xa60, 0xa61, 0xa63, 0xa6c, 0xa66, 0xa6c, 0xa71, 0xa6c, 0xa71,
	0xa72, 0xa74, 0xa78, 0xa7a, 0xa7b, 0xa7d, 0xa7e, 0xa80, 0xa81, 0xa83, 0xa8a, 0xa86,
	0xa8d, 0xa89, 0xa8d, 0xa8f, 0xa8d, 0xa8f, 0xa90, 0xa92, 0xa93, 0xa95, 0xa96, 0xa98,
	0xa96, 0xa9b, 0xa9c, 0xa9b, 0xa9f, 0xa9e, 0xa9f, 0xaa1, 0xa9f, 0xaa1, 0xaa5, 0xaa4,
	0xaa5, 0xaaa, 0xaa8, 0xaaa, 0xaab, 0xaac, 0xaab, 0xaaf, 0xaab, 0xaaf, 0xab1, 0xab2,
	0xab1, 0xab5, 0xab4, 0xab5, 0xaba, 0xab8, 0xaba, 0xab8, 0xabf, 0xabe, 0xabf, 0xac4,
	0xac5, 0xac4, 0xac8, 0xac4, 0xac8, 0xaca, 0xac8, 0xac9, 0xace, 0xacc, 0xace, 0xacf,
	0xad1, 0xacf, 0xad4, 0xad5, 0xad3, 0xad8, 0xad6, 0xad8, 0xad9, 0xad8, 0xad9, 0xade,
	0xadc, 0xadd, 0xae2, 0xae3, 0xae2, 0xae6, 0xae2, 0xae6, 0xae7, 0xae6, 0xae7, 0xae9,
	0xae7, 0xaec, 0xaea, 0xaee, 0xaf0, 0xaee, 0xaf3, 0xaf1, 0xaef, 0xaf4, 0xaf5, 0xaf4,
	0xaf8, 0xafa, 0xaf8, 0xafc, 0xafb, 0xaf9, 0xafe, 0xaff, 0xb00, 0xb02, 0xb00, 0xb02,
	0xb03, 0xb04, 0xb03, 0xb04, 0xb06, 0xb07, 0xb08, 0xb0d, 0xb0b, 0xb0d, 0xb0e, 0xb0f,
	0xb11, 0xb0f, 0xb11, 0xb12, 0xb13, 0xb15, 0xb13, 0xb15, 0xb19, 0xb14, 0xb1c, 0xb1d,
	0xb1b, 0xb1d, 0xb1e, 0xb20, 0xb27, 0xb25, 0xb2a, 0xb2b, 0xb2a, 0xb28, 0xb2c, 0xb2b,
	0xb2c, 0xb30, 0xb32, 0xb30, 0xb2f, 0xb33, 0xb31, 0xb36, 0xb37, 0xb35, 0xb3a, 0xb35,
	0xb39, 0xb3b, 0xb3c, 0xb3b, 0xb3f, 0xb3d, 0xb3f, 0xb40, 0xb44, 0xb40, 0xb44, 0xb45,
	0xb44, 0xb48, 0xb4a, 0xb4b, 0xb49, 0xb4e, 0xb49, 0xb4d, 0xb52, 0xb4d, 0xb51, 0xb53,
	0xb51, 0xb55, 0xb57, 0xb55, 0xb54, 0xb56, 0xb57, 0xb58, 0xb5a, 0xb5b, 0xb5c, 0xb5e,
	0xb5f, 0xb60, 0xb62, 0xb63, 0xb64, 0xb66, 0xb67, 0xb68, 0xb6a, 0xb6b, 0xb6c, 0xb6e,
	0xb67, 0xb70, 0xb6a, 0xb73, 0xb74, 0xb6e, 0xb6f, 0xb70, 0xb72, 0xb7b, 0xb74, 0xb76,
	0xb77, 0xb78, 0xb7a, 0xb7b, 0xb7c, 0xb7e, 0xb7f, 0xb80, 0xb82, 0xb83, 0xb84, 0xb86,
	0xb87, 0xb88, 0xb89, 0xb8b, 0xb8c, 0xb85, 0xb8f, 0xb90, 0xb89, 0xb93, 0xb94, 0xb95,
	0xb97, 0xb98, 0xb91, 0xba2, 0xba4, 0xba5, 0xba6, 0xbb0, 0xba9, 0xbaa, 0xbac, 0xbad,
	0xbae, 0xbb7, 0xbb1, 0xbb2, 0xbb3, 0xbb5, 0xbb6, 0xbb7, 0xbb9, 0xbba, 0xbbb, 0xbbc,
	0xbbe, 0xbbf, 0xbc0, 0xbc2, 0xbc3, 0xbc4, 0xbc5, 0xbc7, 0xbc8, 0xbc9, 0xbcb, 0xbcc,
	0xbcd, 0xbce, 0xbc8, 0xbd1, 0xbd2, 0xbd3, 0xbcd, 0xbce, 0xbcf, 0xbd1, 0xbd2, 0xbd3,
	0xbd4, 0xbd6, 0xbd7, 0xbd8, 0xbda, 0xbcb, 0xbcc, 0xbcd, 0xbcf, 0xbd0, 0xbd1, 0xbd2,
	0xbd4, 0xbd5, 0xbd6, 0xbd7, 0xbd9, 0xbe2, 0xbdb, 0xbdc, 0xbde, 0xbdf, 0xbe0, 0xbe2,
	0xbe3, 0xbdc, 0xbe5, 0xbe7, 0xbe8, 0xbe1, 0xbea, 0xbe4, 0xbe5, 0xbee, 0xbef, 0xbe9,
	0xbf2, 0xbeb, 0xbf4, 0xbee, 0xbf7, 0xbf0, 0xbf1, 0xbf3, 0xbf4, 0xbfd, 0xbf6, 0xbf8,
	0xc01, 0xbfa, 0xbfb, 0xbfd, 0xbfe, 0xbff, 0xc00, 0xc02, 0xc03, 0xc04, 0xc05, 0xc06,
	0xc08, 0xc09, 0xc0a, 0xc0b, 0xc15, 0xc0e, 0xc0f, 0xc10, 0xc12, 0xc13, 0xc14, 0xc0d,
	0xc0f, 0xc18, 0xc19, 0xc1a, 0xc13, 0xc1d, 0xc16, 0xc1f, 0xc20, 0xc1a, 0xc1b, 0xc1c,
	0xc1d, 0xc26, 0xc20, 0xc21, 0xc22, 0xc23, 0xc25, 0xc26, 0xc27, 0xc28, 0xc29, 0xc2b,
	0xc2c, 0xc2d, 0xc2e, 0xc30, 0xc31, 0xc32, 0xc33, 0xc34, 0xc36, 0xc37, 0xc40, 0xc41,
	0xc3a, 0xc3c, 0xc3d, 0xc46, 0xc3f, 0xc40, 0xc42, 0xc43, 0xc4c, 0xc45, 0xc47, 0xc48,
	0xc49, 0xc4a, 0xc4b, 0xc4d, 0xc4e, 0xc4f, 0xc50, 0xc51, 0xc53, 0xc54, 0xc55, 0xc56,
	0xc57, 0xc59, 0xc5a, 0xc5b, 0xc5c, 0xc5d, 0xc5f, 0xc60, 0xc61, 0xc62, 0xc63, 0xc64,
	0xc66, 0xc67, 0xc68, 0xc69, 0xc6a, 0xc6c, 0xc6d, 0xc6e, 0xc6f, 0xc70, 0xc72, 0xc73,
	0xc74, 0xc75, 0xc76, 0xc77, 0xc79, 0xc7a, 0xc7b, 0xc7c, 0xc75, 0xc7f, 0xc80, 0xc81,
	0xc82, 0xc7b, 0xc84, 0xc86, 0xc87, 0xc80, 0xc89, 0xc8a, 0xc84, 0xc8d, 0xc86, 0xc87,
	0xc90, 0xc91, 0xc93, 0xc94, 0xc95, 0xc96, 0xc97, 0xc98, 0xc9a, 0xc9b, 0xc94, 0xc9d,
	0xc96, 0xc9f, 0xca1, 0xc9a, 0xca3, 0xc9c, 0xca5, 0xca6, 0xca8, 0xca1, 0xcaa, 0xca3,
	0xca4, 0xcad, 0xca7, 0xca8, 0xcb1, 0xcaa, 0xcb3, 0xcb4, 0xcad, 0xcaf, 0xcb8, 0xcb1,
	0xcb2, 0xcb3, 0xcb4, 0xcb6, 0xcb7, 0xcb8, 0xcb9, 0xcc2, 0xcbb, 0xcbc, 0xcbe, 0xcbf,
	0xcc0, 0xcc9, 0xcc2, 0xcc3, 0xcc5, 0xcc6, 0xcc7, 0xcd0, 0xcd1, 0xcca, 0xccb, 0xccd,
	0xcd6, 0xcd7, 0xcd0, 0xcd9, 0xcd2, 0xcdb, 0xcd5, 0xcde, 0xcdf, 0xcd8, 0xce1, 0xce2,
	0xce3, 0xcdd, 0xce6, 0xce7, 0xce8, 0xce9, 0xcea, 0xceb, 0xcec, 0xcee, 0xce7, 0xcf0,
	0xcf1, 0xcf2, 0xcf3, 0xcf4, 0xcf6, 0xcf7, 0xcf8, 0xcf9, 0xcfa, 0xcfb, 0xcf4, 0xcf5,
	0xcf7, 0xcf8, 0xcf9, 0xd02, 0xd03, 0xd04, 0xd05, 0xd06, 0xd08, 0xd09, 0xd0a, 0xd0b,
	0xd0c, 0xd0d, 0xd0e, 0xd0f, 0xd11, 0xd12, 0xd13, 0xd14, 0xd15, 0xd16, 0xd17, 0xd18,
	0xd1a, 0xd1b, 0xd1c, 0xd1d, 0xd1e, 0xd1f, 0xd20, 0xd21, 0xd22, 0xd24, 0xd25, 0xd26,
	0xd27, 0xd28, 0xd29, 0xd2a, 0xd2b, 0xd2c, 0xd2e, 0xd2f, 0xd30, 0xd31, 0xd32, 0xd33,
	0xd34, 0xd35, 0xd36, 0xd38, 0xd39, 0xd3a, 0xd3b, 0xd3c, 0xd3d, 0xd3e, 0xd3f, 0xd40,
	0xd41, 0xd43, 0xd44, 0xd45, 0xd46, 0xd47, 0xd48, 0xd49, 0xd4a, 0xd4b, 0xd4c, 0xd4e,
	0xd4f, 0xd50, 0xd51, 0xd52, 0xd4b, 0xd54, 0xd55, 0xd4e, 0xd57, 0xd58, 0xd52, 0xd5b,
	0xd5c, 0xd55, 0xd5e, 0xd5f, 0xd58, 0xd61, 0xd62, 0xd63, 0xd64, 0xd66, 0xd67, 0xd68,
	0xd69, 0xd6a, 0xd73, 0xd74, 0xd6d, 0xd6e, 0xd6f, 0xd70, 0xd72, 0xd7b, 0xd74, 0xd75,
	0xd76, 0xd77, 0xd78, 0xd79, 0xd82, 0xd7b, 0xd84, 0xd7d, 0xd7f, 0xd80, 0xd81, 0xd82,
	0xd8b, 0xd84, 0xd8d, 0xd8e, 0xd87, 0xd88, 0xd91, 0xd8a, 0xd93, 0xd94, 0xd8e, 0xd97,
	0xd98, 0xd91, 0xd92, 0xd9b, 0xd9c, 0xd95, 0xd96, 0xd9f, 0xda0, 0xda1, 0xda2, 0xda3,
	0xda5, 0xda6, 0xd9f, 0xda8, 0xda9, 0xdaa, 0xdab, 0xdac, 0xdad, 0xdae, 0xdaf, 0xdb0,
	0xdb1, 0xdb2, 0xdb3, 0xdb5, 0xdb6, 0xdb7, 0xdb8, 0xdb9, 0xdba, 0xdbb, 0xdbc, 0xdbd,
	0xdbe, 0xdbf, 0xdc0, 0xdc1, 0xdc2, 0xdc3, 0xdc4, 0xdc5, 0xdc7, 0xdc8, 0xdc9, 0xdca,
	0xdcb, 0xdcc, 0xdcd, 0xdce, 0xdcf, 0xdd0, 0xdd1, 0xdd2, 0xddb, 0xdd4, 0xdd5, 0xdd6,
	0xdd7, 0xdd8, 0xdd9, 0xddb, 0xddc, 0xddd, 0xdde, 0xddf, 0xde0, 0xde1, 0xde2, 0xde3,
	0xde4, 0xde5, 0xdee, 0xdef, 0xde8, 0xdf1, 0xdea, 0xdeb, 0xdf4, 0xdf5, 0xdee, 0xdf7,
	0xdf8, 0xdf2, 0xdfb, 0xdfc, 0xdf5, 0xdfe, 0xdff, 0xe00, 0xe01, 0xe02, 0xe03, 0xe04,
	0xe05, 0xe06, 0xe07, 0xe08, 0xe09, 0xe0a, 0xe0b, 0xe0c, 0xe0d, 0xe0e, 0xe0f, 0xe10,
	0xe11, 0xe12, 0xe13, 0xe14, 0xe16, 0xe17, 0xe18, 0xe19, 0xe1a, 0xe1b, 0xe1c, 0xe1d,
	0xe1e, 0xe1f, 0xe20, 0xe21, 0xe22, 0xe23, 0xe24, 0xe25, 0xe26, 0xe2f, 0xe28, 0xe29,
	0xe32, 0xe2b, 0xe2c, 0xe2d, 0xe2e, 0xe2f, 0xe38, 0xe39, 0xe32, 0xe33, 0xe3c, 0xe3d,
	0xe36, 0xe3f, 0xe38, 0xe41, 0xe42, 0xe43, 0xe3c, 0xe45, 0xe47, 0xe48, 0xe49, 0xe42,
	0xe4b, 0xe4c, 0xe4d, 0xe4e, 0xe4f, 0xe50, 0xe49, 0xe52, 0xe4b, 0xe54, 0xe55, 0xe4e,
	0xe4f, 0xe50, 0xe59, 0xe5a, 0xe5b, 0xe5c, 0xe55, 0xe5e, 0xe5f, 0xe60, 0xe61, 0xe6a,
	0xe63, 0xe64, 0xe6d, 0xe66, 0xe67, 0xe68, 0xe69, 0xe6a, 0xe6b, 0xe6c, 0xe6d, 0xe6e,
	0xe6f, 0xe70, 0xe79, 0xe72, 0xe73, 0xe7c, 0xe75, 0xe7e, 0xe7f, 0xe80, 0xe81, 0xe82,
	0xe83, 0xe84, 0xe85, 0xe7e, 0xe87, 0xe88, 0xe89, 0xe8a, 0xe8b, 0xe8c, 0xe8d, 0xe8e,
	0xe8f, 0xe90, 0xe91, 0xe92, 0xe93, 0xe94, 0xe95, 0xe96, 0xe97, 0xe98, 0xe99, 0xe9a,
	0xe9b, 0xe9c, 0xe9d, 0xe9e, 0xe9f, 0xea0, 0xea1, 0xea2, 0xea3, 0xeac, 0xea5, 0xea6,
	0xea7, 0xea8, 0xea9, 0xeb2, 0xeab, 0xeac, 0xeb5, 0xeae, 0xeaf, 0xeb8, 0xeb9, 0xeba,
	0xebb, 0xeb4, 0xebd, 0xebe, 0xebf, 0xec0, 0xec1, 0xec2, 0xebb, 0xec4, 0xec5, 0xec6,
	0xec7, 0xec7, 0xec8, 0xec9, 0xeca, 0xecb, 0xecc, 0xecd, 0xece, 0xecf, 0xed0, 0xed1,
	0xed2, 0xed3, 0xed4, 0xed5, 0xede, 0xed7, 0xed8, 0xee1, 0xeda, 0xedb, 0xee4, 0xee5,
	0xede, 0xee7, 0xee0, 0xee9, 0xeea, 0xee3, 0xeec, 0xeed, 0xeee, 0xeef, 0xee8, 0xef1,
	0xef2, 0xeeb, 0xef4, 0xeed, 0xef6, 0xef7, 0xef0, 0xef8, 0xef9, 0xefa, 0xefb, 0xefc,
	0xefd, 0xefe, 0xeff, 0xf00, 0xf01, 0xf0a, 0xf0b, 0xf04, 0xf0d, 0xf06, 0xf07, 0xf10,
	0xf11, 0xf0a, 0xf13, 0xf0c, 0xf0d, 0xf16, 0xf17, 0xf18, 0xf19, 0xf1a, 0xf13, 0xf1c,
	0xf1c, 0xf1d, 0xf1e, 0xf1f, 0xf20, 0xf21, 0xf22, 0xf23, 0xf24, 0xf25, 0xf26, 0xf27,
	0xf28, 0xf29, 0xf2a, 0xf2b, 0xf2c, 0xf2d, 0xf36, 0xf2f, 0xf30, 0xf39, 0xf32, 0xf33,
	0xf3b, 0xf34, 0xf3d, 0xf3e, 0xf3f, 0xf38, 0xf41, 0xf42, 0xf43, 0xf44, 0xf45, 0xf46,
	0xf47, 0xf48, 0xf49, 0xf4a, 0xf4b, 0xf4c, 0xf4d, 0xf4e, 0xf4f, 0xf4f, 0xf50, 0xf51,
	0xf52, 0xf53, 0xf4c, 0xf55, 0xf56, 0xf57, 0xf58, 0xf59, 0xf5a, 0xf5b, 0xf5c, 0xf5d,
	0xf5e, 0xf5f, 0xf60, 0xf69, 0xf69, 0xf6a, 0xf6b, 0xf6c, 0xf6d, 0xf6e, 0xf6f, 0xf68,
	0xf71, 0xf72, 0xf73, 0xf74, 0xf75, 0xf76, 0xf77, 0xf78, 0xf79, 0xf79, 0xf7a, 0xf7b,
	0xf7c, 0xf7d, 0xf7e, 0xf7f, 0xf80, 0xf81, 0xf82, 0xf83, 0xf8c, 0xf85, 0xf86, 0xf8f,
	0xf88, 0xf90, 0xf89, 0xf8a, 0xf93, 0xf94, 0xf95, 0xf96, 0xf97, 0xf90, 0xf91, 0xf9a,
	0xf9b, 0xf9c, 0xf9d, 0xf9e, 0xf9e, 0xf9f, 0xfa0, 0xfa1, 0xfa2, 0xfa3, 0xfa4, 0xfa5,
	0xfa6, 0xfa7, 0xfa8, 0xfa9, 0xfaa, 0xfab, 0xfb3, 0xfac, 0xfad, 0xfae, 0xfaf, 0xfb0,
	0xfb9, 0xfba, 0xfbb, 0xfb4, 0xfbd, 0xfbe, 0xfbf, 0xfb7, 0xfc0, 0xfc1, 0xfc2, 0xfc3,
	0xfc4, 0xfc5, 0xfc6, 0xfc7, 0xfc8, 0xfc9, 0xfca, 0xfcb, 0xfcb, 0xfcc, 0xfcd, 0xfce,
	0xfcf, 0xfd0, 0xfd1, 0xfda, 0xfd3, 0xfd4, 0xfd5, 0xfd6, 0xfd7, 0xfd7, 0xfd8, 0xfe1,
	0xfda, 0xfdb, 0xfdc, 0xfdd, 0xfde, 0xfdf, 0xfe0, 0xfe9, 0xfea, 0xfea, 0xfeb, 0xfec,
	0xfed, 0xfee, 0xfef, 0xff0, 0xff1, 0xff2, 0xff3, 0xff4, 0xff4, 0xff5, 0xff6, 0xff7,
	0xff8, 0xff9, 0xffa, 0xffb, 0xffc, 0xffd, 0xffe, 0xfff,
};

/*
 * stock mi_dsi_update_lhbm_cmd_87reg() (0x186468) on a panel with mi,local-hbm-alpha-ctrl-aa-area
 * that is neither N11 nor M11 (the 42 02 0b): the first 87h entry of the command's update table
 * gets (alpha >> 8) & 0xf, alpha & 0xff, alpha = aa_alpha_set[level] -- the level being
 * k618_lhbm_bl()'s (1000 nit at bl_max in HBM, the doze level's dbv in LP1/LP2, else the last
 * level; green the last level: 0x18d6b0). Its commands carry 51h 3fff: the area at full drive,
 * the rest of the screen scaled back to the level by this alpha. Stock stops (BUG) above 0xfff;
 * the alpha is 0xfff there.
 */
static void k618_lhbm_alpha(struct k618_bl *b, struct dsi_panel *p, int state,
			    enum dsi_cmd_set_type type)
{
	static const struct {
		enum dsi_cmd_set_type type;
		const char *upd;
	} m[] = {
		{ DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT,
		  "mi,mdss-dsi-local-hbm-normal-white-1000nit-command-update" },
		{ DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT,
		  "mi,mdss-dsi-local-hbm-hlpm-white-1000nit-command-update" },
		{ DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT,
		  "mi,mdss-dsi-local-hbm-normal-white-110nit-command-update" },
		{ DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT,
		  "mi,mdss-dsi-local-hbm-hlpm-white-110nit-command-update" },
		{ DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT,
		  "mi,mdss-dsi-local-hbm-normal-green-500nit-command-update" },
	};
	struct k618_upd_set set;
	u32 bl, a;
	u8 v[2];
	int i, j;

	for (i = 0; i < ARRAY_SIZE(m) && m[i].type != type; i++)
		;
	if (i == ARRAY_SIZE(m) || !k618_upd_now(p, m[i].upd, &set))
		return;
	bl = k618_lhbm_bl(b, p, state);
	a = bl < ARRAY_SIZE(k618_aa_alpha) ? k618_aa_alpha[bl] : 0xfff;
	v[0] = (a >> 8) & 0xf;
	v[1] = a & 0xff;
	for (j = 0; j < set.count; j++) {
		if (set.e[j].reg != 0x87)
			continue;
		if (k618_patch(p, type, &set.e[j], v, 2))
			pr_warn_ratelimited("%s: lhbm set %d: 87h entry (%u bytes) not patched\n",
					    p->name, type, set.e[j].len);
		break;
	}
	pr_info("%s: lhbm set %d for level %u: alpha %#x\n", p->name, type, bl, a);
}

/*
 * stock mi_dsi_update_backlight_in_aod() (0x18819c): 51h straight to the panel as
 * dsi_panel_update_backlight() writes it (the bytes swapped with qcom,mdss-dsi-bl-inverted-dbv)
 */
static void k618_bl_direct(struct dsi_panel *p, u32 lvl)
{
	u16 v = lvl;
	int rc;

	if (p->bl_config.bl_inverted_dbv)
		v = ((lvl & 0xff) << 8) | ((lvl >> 8) & 0xff);
	rc = mipi_dsi_dcs_set_display_brightness(&p->mipi_device, v);
	if (rc < 0)
		pr_err("%s: 51h %u: %d\n", p->name, lvl, rc);
}

/* the same with the doze level's dbv: LBM's for a DOZE_BRIGHTNESS_LBM request, HBM's for HBM, else none */
static void k618_bl_direct_doze(struct k618_bl *b, struct dsi_panel *p)
{
	if (b->doze_level == DOZE_BRIGHTNESS_LBM)
		k618_bl_direct(p, b->doze_lbm_dbv);
	else if (b->doze_level == DOZE_BRIGHTNESS_HBM)
		k618_bl_direct(p, b->doze_hbm_dbv);
}

/*
 * stock mi_dsi_panel_update_last_bl_level() (0x186754): a level after the fingerprint area went off
 * (panel+0xbe8 = 2) starts the dimming-on work, mi,panel-on-dimming-delay ms later
 * (mi_dsi_panel_tigger_dimming_delayed_work 0x184fe4) -- the local-HBM commands leave 53h at 0x20.
 * Stock does it on every panel; here on the 42 02 0b.
 */
static void k618_lhbm_dimming(struct k618_bl *b, struct dsi_panel *p)
{
	b->lhbm_dimming = false;
	if (!g_mi || k618_no_ic_dimming(b))
		return;
	mod_delayed_work(system_wq, &g_mi->dimming_work[k618_panel_id(p)],
			 msecs_to_jiffies(k618_dimming_delay(b)));
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
 * (and off-to-normal from AOD) patched. The 42 02 0b sets the alpha flag: k618_lhbm_alpha(), its
 * direct 51h writes and the dimming after the area goes off.
 */
static int k618_lhbm(struct dsi_display *d, int state)
{
	struct dsi_panel *p = d->panel;
	enum dsi_cmd_set_type type;
	struct k618_bl *b;
	u8 dbv[2];
	u32 lvl;
	bool aod, lit, alpha = false;
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
	/* stock set_disp_param case 9 (0x18c868): a local-HBM request ends the 13 Ultra's peak HDR */
	if (b->is_m1 && b->peak_on)
		k618_m1_peak_off(b, p, "local hbm");
	if (!fod)
		goto out;
	aod = k618_in_doze(b);
	alpha = lhbm_alpha && b->lhbm_alpha;

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
		if (alpha)
			k618_lhbm_alpha(b, p, state, type);
		rc = dsi_panel_tx_cmd_set(p, type, false);
		break;
	case LOCAL_HBM_NORMAL_WHITE_110NIT:
	case LOCAL_HBM_NORMAL_GREEN_500NIT:
		type = k618_lhbm_cmd(state, b->em_16p);
		k618_m1_lhbm(b, p, state, type);
		if (alpha)
			k618_lhbm_alpha(b, p, state, type);
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
		if (alpha) {
			k618_lhbm_alpha(b, p, state, type);
			k618_bl_direct_doze(b, p);	/* stock 0x18dfdc / 0x18e088 */
		}
		rc = dsi_panel_tx_cmd_set(p, type, false);
		b->panel_state = K618_PANEL_STATE_ON;
		break;
	case LOCAL_HBM_OFF_TO_NORMAL:
	case LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT:
	case LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE:
		/* the 42 02 0b: stock mi_dsi_update_backlight_in_aod() first (0x18d4dc, 0x18d890, 0x18d928) */
		if (alpha) {
			if (state == LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT)
				k618_bl_direct_doze(b, p);
			else
				k618_bl_direct(p, b->last_bl);
		}
		if (b->hbm_on) {
			/* back to the global HBM: off-to-hbm (63) with 51h = the last level */
			k618_dbv2(dbv, b->last_bl);
			k618_patch_cmd_now(p, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM,
					   "mi,mdss-dsi-local-hbm-off-to-hbm-command-update",
					   0x51, dbv, 2);
			rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM, false);
			if (state == LOCAL_HBM_OFF_TO_NORMAL)
				k618_m1_lhbm_off(b, p);
			else if (alpha)
				b->lhbm_dimming = true;	/* stock 0xbe8 = 2 for the backlight variants */
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
		if (alpha) {
			/* stock 0x18e194: off-to-normal with the last level; the others with the next one */
			if (state == LOCAL_HBM_OFF_TO_NORMAL && b->last_bl)
				k618_lhbm_dimming(b, p);
			else
				b->lhbm_dimming = true;
		}
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
	/* stock mi_disp_set_local_hbm() (0x17f8a8): nothing for a panel without mi,local-hbm-enabled */
	if (k618_bls[disp_id].parsed && !k618_bls[disp_id].lhbm_enabled) {
		pr_debug("disp%d: not a local-HBM panel, request %d dropped\n", disp_id, mfp_code);
		return 0;
	}
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
	case DISP_FEATURE_PEAK_HDR_MODE:
		rc = k618_peak(d, disp_id, val);
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

/*
 * the brightness clone maximum (max_brightness_clone, which MiBrightness reads): stock
 * mi,max-brightness-clone of the panel, 0xfff without it (mi_dsi_panel_parse_config 0x180f18);
 * 0x3fff on all four panels here
 */
static u32 k618_max_clone(int id)
{
	struct dsi_display *d = g_mi && disp_ok(id) ? g_mi->disp[id] : NULL;
	u32 v = 0xfff;

	if (max_brightness_clone)
		return max_brightness_clone;
	if (d && d->panel && d->panel->panel_of_node)
		of_property_read_u32(d->panel->panel_of_node, "mi,max-brightness-clone", &v);
	return v;
}

/* clone -> DBV fallback, only while MiBrightness is not subscribed (see file header) */
static void k618_clone_fallback(int disp_id, u32 clone)
{
	u32 max = k618_max_clone(disp_id);
	struct backlight_device *bd;
	unsigned long level;

	if (atomic_read(&g_mi->clone_subs) || !max)
		return;
	bd = backlight_device_get_by_name(bl_name);
	if (!bd)
		return;
	level = mult_frac((unsigned long)min(clone, max), bd->props.max_brightness, max);
	if (clone && level < min_level)
		level = min_t(unsigned long, min_level, bd->props.max_brightness);
	backlight_device_set_brightness(bd, level);
	put_device(&bd->dev);
}

static void k618_set_clone(int disp_id, u32 clone)
{
	if (!disp_ok(disp_id))
		return;
	/*
	 * stock mi_dsi_panel_set_brightness_clone() (0x1888d4): at most the maximum; the thermal
	 * limit (panel+0x9a8) only without mi,thermal-dimming-flag, which all four panels set
	 */
	clone = min(clone, k618_max_clone(disp_id));
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
 * stock mi_dsi_display_read_panel_build_id() (0x183188), after panel on (dsi_display_enable 0x154604,
 * and 0x154484 in its continuous-splash path) while mi,panel-build-id-read-needed: DAh through
 * mi,mdss-dsi-panel-build-id-read-command, mi,panel-build-id-read-length bytes. 0x40 on a panel
 * other than the 13 Ultra is replaced by the sub id (0x1832a0): sub-write-1, the
 * mi,mdss-dsi-panel-build-id-sub-read-command read (the 38 0c: 1Dh), sub-write-2. A failed read is
 * tried again at the next panel on, as stock. panel_lock held, the panel on.
 */
static void k618_read_build_id(struct k618_bl *b, struct dsi_panel *p)
{
	u8 v[4] = { 0 };
	u32 len = 1;
	int rc;

	if (!b->bid_needed || build_id >= 0)
		return;
	of_property_read_u32(p->panel_of_node, "mi,panel-build-id-read-length", &len);
	if (!len || len > sizeof(v))
		len = 1;
	rc = k618_dt_read(p, "mi,mdss-dsi-panel-build-id-read-command", v, len, true);
	if (rc <= 0) {
		if (!b->bid_failed++ && b->is_m2) {
			k618_em_eval(b);
			pr_warn("%s: build id read: %d -- 0xa0 used until a read works, em-cycle %d\n",
				p->name, rc, b->em_capable);
		} else {
			pr_warn("%s: build id read: %d\n", p->name, rc);
		}
		return;
	}
	if (v[0] == 0x40 && !b->is_m1) {
		v[0] = 0;
		rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_PANEL_BUILD_ID_SUB_WRITE_1, false);
		if (rc) {
			pr_err("%s: build id sub-write-1: %d\n", p->name, rc);
			return;
		}
		rc = k618_dt_read(p, "mi,mdss-dsi-panel-build-id-sub-read-command", v, len, true);
		if (rc <= 0)
			pr_err("%s: sub panel id read: %d\n", p->name, rc);
		rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_PANEL_BUILD_ID_SUB_WRITE_2, false);
		if (rc) {
			pr_err("%s: build id sub-write-2: %d\n", p->name, rc);
			return;
		}
	}
	b->bid = v[0];
	b->bid_needed = false;
	k618_em_eval(b);
	pr_info("%s: build id %#x, em-cycle %d\n", p->name, b->bid, b->em_capable);
}

/* what stock reads from the panel once it is on; panel_lock held */
static void k618_first_reads(struct k618_bl *b, struct dsi_panel *p)
{
	if (!p->panel_initialized)
		return;
	k618_read_build_id(b, p);
	k618_m1_peak_read(b, p);
}

/*
 * panel enable (dsi_panel_enable(), panel_lock held, the panel just on): POWER event; the reads
 * stock does after it (build id, the 13 Ultra's peak gamma); panel-on dimming unless the panel
 * disables IC dimming
 */
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
	k618_hw_panel_id(panel);
	k618_hw_state(panel, true);	/* stock dsi_panel_enable(): lit, the mode's rate */
	id = k618_panel_id(panel);
	k618_notify_u32(id, MI_DISP_EVENT_POWER, 1 /* SDE_MODE_DPMS_ON */);
	b = k618_bl_get(panel);   /* dsi_panel_enable() holds panel_lock */
	if (b) {
		k618_first_reads(b, panel);
		if (b->is_m1) {
			/* stock dsi_panel_enable() 0x163f44: both caches cleared, the next 51h sends both */
			b->m1_skip = b->m1_dbi = DSI_CMD_SET_MAX;
			k618_m1_doze_gamma(b, panel);
		}
	}
	if (b && k618_no_ic_dimming(b))
		return;
	mod_delayed_work(system_wq, &mi->dimming_work[id],
			 msecs_to_jiffies(k618_dimming_delay(b)));
}

/*
 * dsi_display_enable(), continuous-splash path (the panel on from the bootloader, no
 * dsi_panel_enable()): stock reads the build id and the 13 Ultra's peak gamma there too
 * (0x154484, 0x1544dc). The caller holds no lock.
 */
void k618_mi_disp_splash(struct dsi_panel *panel)
{
	struct k618_bl *b;

	if (!g_mi || !panel || !panel->host)
		return;
	k618_mi_disp_attach(container_of(panel->host, struct dsi_display, host));
	k618_hw_panel_id(panel);
	k618_hw_state(panel, true);	/* lit from the bootloader on */
	mutex_lock(&panel->panel_lock);
	b = k618_bl_get(panel);
	if (b) {
		k618_first_reads(b, panel);
		if (b->is_m1)
			k618_m1_doze_gamma(b, panel);
	}
	mutex_unlock(&panel->panel_lock);
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
		/* the 42 02 0b: the first level after the fingerprint area went off (k618_lhbm_dimming) */
		if (b->lhbm_dimming && b->last_bl && !k618_fod_lit(b, k618_panel_id(panel)))
			k618_lhbm_dimming(b, panel);
	}
	k618_notify_u32(k618_panel_id(panel), MI_DISP_EVENT_51_BRIGHTNESS, bl_lvl);
}

/* ---- what userspace reads about the panel ---- */

/*
 * panel_info / GET_PANEL_INFO: stock mi_dsi_display_read_panel_info() (0x18308c) takes
 * msm_drm.dsi_display0/1 as the bootloader passed it ("qcom,mdss_dsi_m2_38_0c_0a_dsc_cmd:") after
 * its last ',' and up to its last ':' (mi_dsi_display_get_cmdline_panel_info 0x149920), else the
 * display's name after its last ','; here the bound panel's node name stands in for the latter.
 * The 13's "qcom,global,mdss_dsi_m3_38_0c_0a_dsc_cmd" gives mdss_dsi_m3_38_0c_0a_dsc_cmd either way.
 */
static const char *k618_panel_name(int id, char *buf, size_t len)
{
	struct dsi_display *d = g_mi && disp_ok(id) ? g_mi->disp[id] : NULL;
	const char *s, *c;
	char *e;

	if (panel_name[0])
		return panel_name;
	if (!disp_ok(id))
		return "null";
	s = k618_dsi_boot_param(id);
	c = s ? strrchr(s, ',') : NULL;
	if (c && c[1]) {
		strscpy(buf, c + 1, len);
		e = strrchr(buf, ':');
		if (e)
			*e = '\0';
		return buf;
	}
	if (d && d->panel && d->panel->panel_of_node) {
		s = d->panel->panel_of_node->full_name;
		c = strrchr(s, ',');
		strscpy(buf, c && c[1] ? c + 1 : s, len);
		return buf;
	}
	return "null";
}

/*
 * wp_info / GET_WP_INFO: stock mi_dsi_display_read_wp_info() (0x183614) -- msm_drm.oled_wp for the
 * primary display, sec_oled_wp for the secondary, as the bootloader passed them; without one, a
 * read from the panel: mi,mdss-dsi-panel-wp-read-command, mi,mdss-dsi-panel-wp-read-length bytes,
 * hex from mi,mdss-dsi-panel-wp-read-index on, kept in the parameter for the next call (the 38 0c
 * and the 13: A1h, 4 bytes; the 13 Ultra: A3h, 8 bytes; the 42 02 0b has no read command). Stock's
 * optional wp-read-offset command is in none of these DTs.
 */
static int k618_wp(int id, char *out, size_t size)
{
	struct dsi_display *d = g_mi && disp_ok(id) ? g_mi->disp[id] : NULL;
	char *s = id == MI_DISP_SECONDARY ? sec_oled_wp : oled_wp;
	u32 len = 0, idx = 0, i;
	size_t n = 0;
	u8 buf[16];
	int rc;

	if (s[0])
		return scnprintf(out, size, "%s\n", s);
	if (!d || !d->panel || !d->panel->panel_of_node)
		return -EINVAL;
	of_property_read_u32(d->panel->panel_of_node, "mi,mdss-dsi-panel-wp-read-length", &len);
	of_property_read_u32(d->panel->panel_of_node, "mi,mdss-dsi-panel-wp-read-index", &idx);
	if (!len || len > sizeof(buf) || idx >= len)
		return -EINVAL;
	rc = k618_dt_read(d->panel, "mi,mdss-dsi-panel-wp-read-command", buf, len, false);
	if (rc <= 0) {
		pr_warn("disp%d: wp read: %d\n", id, rc);
		return rc ? rc : -EIO;
	}
	for (i = idx; i < len && n + 3 <= sizeof(oled_wp); i++)
		n += scnprintf(s + n, sizeof(oled_wp) - n, "%02x", buf[i]);
	return scnprintf(out, size, "%s\n", s);
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
		char line[160], name[128];
		int n = scnprintf(line, sizeof(line), "panel_name=%s\n",
				  k618_panel_name(disp_id, name, sizeof(name)));

		if (r->info && r->info_len &&
		    copy_to_user(r->info, line, min_t(u32, r->info_len, n + 1)))
			rc = -EFAULT;
		r->info_len = n;
		break;
	}
	case _IOC_NR(MI_DISP_IOCTL_GET_WP_INFO): {
		struct disp_wp_info *r = (void *)kdata;
		char line[80];
		int n = k618_wp(disp_id, line, sizeof(line));

		if (n < 0) {
			rc = n;
			r->info_len = 0;
			break;
		}
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

static int k618_dev_id(struct device *d);

static ssize_t panel_info_show(struct device *d, struct device_attribute *a, char *b)
{
	char name[128];

	return sysfs_emit(b, "panel_name=%s\n", k618_panel_name(k618_dev_id(d), name, sizeof(name)));
}
static DEVICE_ATTR_RO(panel_info);
static ssize_t wp_info_show(struct device *d, struct device_attribute *a, char *b)
{
	return k618_wp(k618_dev_id(d), b, PAGE_SIZE);
}
static DEVICE_ATTR_RO(wp_info);
/* stock mi_dsi_display_read_panel_build_id_info() (0x183584): "Unsupported" while it is 0 */
static ssize_t panel_build_id_show(struct device *d, struct device_attribute *a, char *b)
{
	int id = k618_dev_id(d);
	u32 bid = build_id >= 0 ? (build_id & 0xff) : 0;

	if (disp_ok(id) && k618_bls[id].parsed)
		bid = k618_bid(&k618_bls[id]);
	return bid ? sysfs_emit(b, "0x%02X\n", bid) : sysfs_emit(b, "Unsupported\n");
}
static DEVICE_ATTR_RO(panel_build_id);
static ssize_t dynamic_fps_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%u\n", fps); }
static DEVICE_ATTR_RO(dynamic_fps);
static ssize_t max_brightness_clone_show(struct device *d, struct device_attribute *a, char *b)
{ return sysfs_emit(b, "%u\n", k618_max_clone(k618_dev_id(d))); }
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

	k618_hw_init();	/* stock mi_disp_feature_init() -> hwconf_init() */
	pr_info("mi_display ready: /dev/mi_display/disp_feature, version %#06x\n",
		MI_DISP_FEATURE_VERSION);
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
	k618_hw_exit();
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
