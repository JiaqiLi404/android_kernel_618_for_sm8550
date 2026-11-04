// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */

/* ---- enums (uapi mi_disp.h values; add near the DISP_FEATURE_* in k618_mi_disp.c) ---- */

#define DISP_FEATURE_DOZE_BRIGHTNESS 3
#define DISP_FEATURE_LOCAL_HBM       9
#define DISP_FEATURE_SENSOR_LUX      10
#define DISP_FEATURE_LOW_BRIGHTNESS_FOD 11
#define DISP_FEATURE_FP_STATUS       12
#define DISP_FEATURE_AOD_TO_NORMAL   16
/* DISP_FEATURE_DIMMING 0, HBM 1, HBM_FOD 2, FLAT_MODE 6 already in k618_mi_disp.c */

/* enum local_hbm_state (uapi) */
#define LOCAL_HBM_OFF_TO_NORMAL                    0
#define LOCAL_HBM_NORMAL_WHITE_1000NIT             1
#define LOCAL_HBM_NORMAL_WHITE_750NIT              2
#define LOCAL_HBM_NORMAL_WHITE_500NIT              3
#define LOCAL_HBM_NORMAL_WHITE_110NIT              4
#define LOCAL_HBM_NORMAL_GREEN_500NIT              5
#define LOCAL_HBM_HLPM_WHITE_1000NIT               6
#define LOCAL_HBM_HLPM_WHITE_110NIT                7
#define LOCAL_HBM_OFF_TO_HLPM                      8
#define LOCAL_HBM_OFF_TO_LLPM                      9
#define LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT          10
#define LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT_RESTORE  11

/* enum fingerprint_status (uapi) */
#define FINGERPRINT_NONE 0
#define ENROLL_START     1
#define AUTH_START       3
#define AUTH_STOP        4
#define HEART_RATE_START 5

/* enum doze_brightness_state (uapi) */
#define DOZE_TO_NORMAL      0
#define DOZE_BRIGHTNESS_HBM 1
#define DOZE_BRIGHTNESS_LBM 2

/* enum mi_panel_state (stock panel+0xc04) */
#define K618_PANEL_STATE_ON        1
#define K618_PANEL_STATE_DOZE_HIGH 2
#define K618_PANEL_STATE_DOZE_LOW  3

/* the FOD event (type 2) payload bits (uapi fod_ui_ready_state) */
#define K618_FOD_UI_NONE              0
#define K618_FOD_LOW_BRIGHTNESS_CAP   0x4   /* FOD_LOW_BRIGHTNESS_CAPTURE */
#define K618_FOD_UI_READY             0x8   /* LOCAL_HBM_UI_READY */

/* the mfp codes the SET_LOCAL_HBM ioctl carries (stock .rodata+0x89280 name table) */
#define K618_MFP_FINGER_UP  0
#define K618_MFP_AUTH_STOP  1
#define K618_MFP_WHITE_1000 2
#define K618_MFP_WHITE_110  3
#define K618_MFP_GREEN_500  4

/* Device compatibility. */
static bool fod = true;
module_param(fod, bool, 0644);
/* 1: send doze-hbm/lbm commands as stock; 0: store only */
static bool doze_cmd = true;
module_param(doze_cmd, bool, 0644);

/*
 * The Xiaomi command sets FOD/doze send. Types come from k618_fod_disp_edits.py's MI_CMD_SETS
 * additions (enum dsi_cmd_set_type + cmd_set_prop_map[]/cmd_set_state_map[]), so Qualcomm's own
 * per-mode parser reads them into priv_info->cmd_sets[] per timing. Their DT names:
 *   NORMAL_WHITE_1000NIT   mi,mdss-dsi-local-hbm-normal-white-1000nit-command           (47)
 *   NORMAL_WHITE_1000NIT_16P ...-em-cycle-16pulse-command                               (48)
 *   HLPM_WHITE_1000NIT     mi,mdss-dsi-local-hbm-hlpm-white-1000nit-command             (49)
 *   NORMAL_WHITE_110NIT    mi,mdss-dsi-local-hbm-normal-white-110nit-command            (56)
 *   NORMAL_WHITE_110NIT_16P ...-em-cycle-16pulse-command                                (57)
 *   HLPM_WHITE_110NIT      mi,mdss-dsi-local-hbm-hlpm-white-110nit-command              (58)
 *   NORMAL_GREEN_500NIT    mi,mdss-dsi-local-hbm-normal-green-500nit-command            (59)
 *   NORMAL_GREEN_500NIT_16P ...-em-cycle-16pulse-command                                (60)
 *   OFF_TO_NORMAL          mi,mdss-dsi-local-hbm-off-to-normal-command                  (61)
 *   OFF_TO_NORMAL_16P      ...-off-to-normal-em-cycle-16pulse-command                   (62)
 *   OFF_TO_HBM             mi,mdss-dsi-local-hbm-off-to-hbm-command                     (63)
 *   DOZE_HBM / DOZE_LBM    mi,mdss-dsi-doze-hbm-command / -doze-lbm-command             (32/33)
 *   DOZE_HBM_NOLP/LBM_NOLP mi,mdss-dsi-doze-hbm-nolp-command / -lbm-nolp-command        (34/35)
 */

/* ---- (1) patch one register of a command now, from its "-command-update" property ---- */

/*
 * stock mi_dsi_update_51_mipi_cmd(): for command `type`, read its "-command-update" triples of the
 * active timing, find the packet whose first byte is `reg`, overwrite `len` bytes after it. Here we
 * parse the property on the spot (k618_upd_parse) and apply with k618_patch(), rather than keeping a
 * per-timing table as the hot brightness path does: FOD/doze are rare.
 */
static void k618_patch_cmd_now(struct dsi_panel *panel, enum dsi_cmd_set_type type,
			       const char *update_prop, u8 reg, const u8 *val, u8 len)
{
	struct device_node *np;
	struct k618_upd_set set;
	int i;

	struct device_node *timings, *child;
	u32 want, cell, pos = 0;

	if (!panel->cur_mode || !panel->cur_mode->priv_info || !panel->panel_of_node)
		return;
	/* the "-command-update" property lives on the active timing node, beside the command itself */
	timings = of_get_child_by_name(panel->panel_of_node, "qcom,mdss-dsi-display-timings");
	if (!timings)
		return;
	want = panel->cur_mode->mode_idx;	/* = the timing's cell-index (dsi_panel_get_mode) */
	np = NULL;
	for_each_child_of_node(timings, child) {
		cell = pos++;
		of_property_read_u32(child, "cell-index", &cell);
		if (cell == want) {
			np = child;	/* for_each_child_of_node took a ref; put it below */
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
			break;	/* stock patches the first 0x51 packet only */
		}
}

/* bl high byte first, 2 bytes -- what the 51h packet wants */
static void k618_dbv2(u8 out[2], u32 bl)
{
	out[0] = (bl >> 8) & 0xff;
	out[1] = bl & 0xff;
}

/* ---- (2) send the local-HBM command set for one state (M2) ---- */

/* the command type for a nit level, 16-pulse variant when the base emission is 16-pulse */
static enum dsi_cmd_set_type k618_lhbm_nit_cmd(int state, bool em16)
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

/* the doze dbv for the current doze level (stock doze_hbm/lbm_dbv_level, panel+0x9f0/0x9f4) */
static u32 k618_doze_dbv(struct k618_bl *b)
{
	if (b->doze_level == DOZE_BRIGHTNESS_LBM && b->doze_lbm_dbv)
		return b->doze_lbm_dbv;
	return b->doze_hbm_dbv ? b->doze_hbm_dbv : b->last_bl;
}

/*
 * stock mi_dsi_panel_set_lhbm_fod_locked() for M2 (panel id 1). panel_lock held. Returns the tx rc.
 * No alpha(87h)/DF/white-param(66h) runtime patch: M2 does not set mi,local-hbm-alpha-ctrl-aa-area /
 * -ctrl-df-reg / the *-update-flags, so those stock helpers early-return for it -- the DT command is
 * complete as parsed, and only the off/hbm path patches 51h.
 */
static int k618_lhbm(struct dsi_display *d, int state)
{
	struct dsi_panel *p = d->panel;
	struct k618_bl *b;
	enum dsi_cmd_set_type type;
	bool aod;
	u8 dbv[2];
	u32 lvl;
	int rc = 0;

	if (!p)
		return -ENODEV;
	mutex_lock(&p->panel_lock);
	if (!p->panel_initialized) {
		rc = -ENODEV;
		goto out;
	}
	b = k618_bl_get(p);
	if (!b) {
		rc = -ENODEV;
		goto out;
	}
	if (!fod || !b->is_m2) {
		/* store-only / non-M2: nothing to send (feature_val is stored by the caller) */
		goto out;
	}
	aod = b->panel_state == K618_PANEL_STATE_DOZE_HIGH ||
	      b->panel_state == K618_PANEL_STATE_DOZE_LOW;

	switch (state) {
	case LOCAL_HBM_NORMAL_WHITE_1000NIT:
		/* stock: in HBM use bl_max, in doze the doze dbv, else last_bl; patch 47's 51h if present */
		lvl = b->hbm_on ? p->bl_config.bl_max_level : (aod ? k618_doze_dbv(b) : b->last_bl);
		k618_dbv2(dbv, lvl);
		type = k618_lhbm_nit_cmd(state, b->em_16p);
		k618_patch_cmd_now(p, type, "mi,mdss-dsi-local-hbm-normal-white-1000nit-command-update",
				   0x51, dbv, 2);
		rc = dsi_panel_tx_cmd_set(p, type, false);
		break;
	case LOCAL_HBM_NORMAL_WHITE_110NIT:
	case LOCAL_HBM_NORMAL_GREEN_500NIT:
		type = k618_lhbm_nit_cmd(state, b->em_16p);
		rc = dsi_panel_tx_cmd_set(p, type, false);
		break;
	case LOCAL_HBM_HLPM_WHITE_1000NIT:
	case LOCAL_HBM_HLPM_WHITE_110NIT:
		/* H-doze: patch the HLPM command's 51h with the doze dbv */
		k618_dbv2(dbv, k618_doze_dbv(b));
		type = k618_lhbm_nit_cmd(state, false);
		k618_patch_cmd_now(p, type,
				   state == LOCAL_HBM_HLPM_WHITE_1000NIT ?
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
			/* off-to-hbm: patch 51h = last_bl, send 63 */
			k618_dbv2(dbv, b->last_bl);
			k618_patch_cmd_now(p, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM,
					   "mi,mdss-dsi-local-hbm-off-to-hbm-command-update",
					   0x51, dbv, 2);
			rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM, false);
		} else {
			/*
			 * off-to-normal (61): 3-pulse variant unless the base is 16-pulse. Its DT has no
			 * 51h slot (brightness is restored by the composer's next backlight write), but
			 * the BACKLIGHT variants in doze carry the dbv in their own update, kept for when
			 * the panel adds one; harmless if absent.
			 */
			type = (b->em_16p) ? DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_16P
					   : DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL;
			if (state != LOCAL_HBM_OFF_TO_NORMAL) {
				lvl = aod ? min(b->last_bl ? b->last_bl : k618_doze_dbv(b),
						k618_doze_dbv(b)) : b->last_bl;
				k618_dbv2(dbv, lvl);
				k618_patch_cmd_now(p, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL,
						   "mi,mdss-dsi-local-hbm-off-to-normal-command-update",
						   0x51, dbv, 2);
			}
			rc = dsi_panel_tx_cmd_set(p, type, false);
		}
		b->hbm_on = false;
		break;
	default:
		pr_debug("lhbm state %d not handled on M2\n", state);
		break;
	}
	if (rc)
		pr_err("%s: lhbm state %d failed: %d\n", p->name, state, rc);
	else
		pr_info("%s: lhbm state %d (em16 %d, aod %d)\n", p->name, state, b->em_16p, aod);
out:
	mutex_unlock(&p->panel_lock);
	return rc;
}

/* ---- (3) the FOD UI-ready event the HAL waits for (stock type 2) ---- */

static void k618_fod_event(int disp_id, bool down, bool low_brightness)
{
	struct k618_bl *b = &k618_bls[disp_id];
	u32 ui = K618_FOD_UI_NONE;

	if (down) {
		ui = K618_FOD_UI_READY;
		if (low_brightness)
			ui |= K618_FOD_LOW_BRIGHTNESS_CAP;
		/* stock waits lhbm_ui_ready_delay_frame (2) frames for the HBM to settle before notify */
		if (b->ui_ready_frames && b->panel && b->panel->cur_mode) {
			u32 fps = b->panel->cur_mode->timing.refresh_rate;
			u32 us = fps ? 1000000U / fps * b->ui_ready_frames : 0;

			if (us)
				usleep_range(us, us + 1000);
		}
	}
	k618_notify_u32(disp_id, MI_DISP_EVENT_FOD, ui);
}

/* ---- (4) SET_LOCAL_HBM ioctl (0x0E): mfp code 0..4 -> local_hbm_state, send, notify ---- */

static int k618_set_local_hbm(int disp_id, int mfp_code)
{
	struct k618_mi *mi = g_mi;
	struct dsi_display *d;
	struct k618_bl *b;
	bool aod, doze, low;
	int state, rc;

	if (!mi || !disp_ok(disp_id))
		return -EINVAL;
	d = mi->disp[disp_id];
	if (!d || !d->panel)
		return -ENODEV;
	b = &k618_bls[disp_id];
	aod = b->panel_state == K618_PANEL_STATE_DOZE_HIGH ||
	      b->panel_state == K618_PANEL_STATE_DOZE_LOW;
	doze = aod;
	/* low-brightness FOD: the HAL set LOW_BRIGHTNESS_FOD and a dim scene (stock reads sensor lux) */
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
		state = doze ? LOCAL_HBM_HLPM_WHITE_1000NIT : LOCAL_HBM_NORMAL_WHITE_1000NIT;
		break;
	case K618_MFP_WHITE_110:
		state = doze ? LOCAL_HBM_HLPM_WHITE_110NIT : LOCAL_HBM_NORMAL_WHITE_110NIT;
		break;
	case K618_MFP_GREEN_500:
		state = LOCAL_HBM_NORMAL_GREEN_500NIT;
		break;
	default:
		return -EINVAL;
	}

	mi->feature_val[disp_id][DISP_FEATURE_LOCAL_HBM] = state;

	/* stock: finger-up/auth-stop notify UP before the command; the lit states notify DOWN after */
	if (mfp_code <= K618_MFP_AUTH_STOP)
		k618_fod_event(disp_id, false, false);
	rc = k618_lhbm(d, state);
	if (!rc && mfp_code >= K618_MFP_WHITE_1000)
		k618_fod_event(disp_id, true, low);
	return rc;
}

/* ---- (5) SET_DOZE_BRIGHTNESS: send doze-hbm(32)/lbm(33), patch 51h, track panel_state ---- */

static int k618_doze(struct dsi_display *d, u32 level)
{
	struct dsi_panel *p = d->panel;
	struct k618_bl *b;
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
	/* stock skips while a FOD/HBM is lit (would fight the LHBM); store and leave */
	if (b->hbm_on) {
		b->doze_level = level;
		goto out;
	}
	if (!doze_cmd || !b->is_m2) {
		b->doze_level = level;
		goto out;
	}
	if (b->doze_level == level) {	/* "has been set, skip" */
		goto out;
	}
	switch (level) {
	case DOZE_BRIGHTNESS_HBM:
		/* doze-hbm (32): its DT 51h is fixed per timing; patch anyway if an update slot exists */
		rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_DOZE_HBM, false);
		b->panel_state = K618_PANEL_STATE_DOZE_HIGH;
		break;
	case DOZE_BRIGHTNESS_LBM:
		rc = dsi_panel_tx_cmd_set(p, DSI_CMD_SET_MI_DOZE_LBM, false);
		b->panel_state = K618_PANEL_STATE_DOZE_LOW;
		break;
	case DOZE_TO_NORMAL:
	default:
		/* stock sends nothing here: the ON transition goes through nolp (set_power ON) */
		break;
	}
	if (rc)
		pr_err("%s: doze level %u failed: %d\n", p->name, level, rc);
	else
		pr_info("%s: doze level %u\n", p->name, level);
	b->doze_level = level;
out:
	mutex_unlock(&p->panel_lock);
	return rc;
}

/* ---- (6) the feature path, extended ---- */

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
		/* the HAL's disp_param path into LHBM; val is a local_hbm_state directly */
		rc = k618_lhbm(d, val);
		break;
	case DISP_FEATURE_AOD_TO_NORMAL:
		/* FOD animation: smooth AOD->normal via doze-hbm-nolp(34)/lbm-nolp(35) */
		if (fod && val) {
			struct k618_bl *b = NULL;

			if (d->panel) {
				mutex_lock(&d->panel->panel_lock);
				b = k618_bl_get(d->panel);
				if (b && b->is_m2 && d->panel->panel_initialized)
					rc = dsi_panel_tx_cmd_set(d->panel,
						b->doze_level == DOZE_BRIGHTNESS_LBM ?
						DSI_CMD_SET_MI_DOZE_LBM_NOLP : DSI_CMD_SET_MI_DOZE_HBM_NOLP,
						false);
				if (b)
					b->panel_state = K618_PANEL_STATE_ON;
				mutex_unlock(&d->panel->panel_lock);
			}
		}
		break;
	case DISP_FEATURE_DOZE_BRIGHTNESS:
		rc = k618_doze(d, val);
		break;
	case DISP_FEATURE_FP_STATUS:
	case DISP_FEATURE_HBM_FOD:
	case DISP_FEATURE_SENSOR_LUX:
	case DISP_FEATURE_LOW_BRIGHTNESS_FOD:
		/* stored below; used by k618_set_local_hbm() to pick the LHBM target. No command. */
		break;
	default:
		/* calibration / crc / dc / round-corner / MI-SF notify ids: accept so the HAL proceeds */
		break;
	}
	if (feature_id < DISP_FEATURE_MAX)
		mi->feature_val[disp_id][feature_id] = val;
	pr_debug("disp%d feature %u = %d (rc %d)\n", disp_id, feature_id, val, rc);
	return rc;
}

/*
 * ================= snippets to merge elsewhere in k618_mi_disp.c =================
 *
 * (a) struct k618_bl -- add four fields:
 *         u32 panel_state;     // K618_PANEL_STATE_* (stock panel+0xc04)
 *         u32 doze_level;      // current DOZE_BRIGHTNESS_* (stock panel+0x9b0)
 *         u32 doze_hbm_dbv;    // DT mi,doze-hbm-dbv-level (stock panel+0x9f0, M2 = 0xf5)
 *         u32 doze_lbm_dbv;    // DT mi,doze-lbm-dbv-level (stock panel+0x9f4, M2 = 0x14)
 *         u32 ui_ready_frames; // DT mi,local-hbm-ui-ready-delay-num-frame (stock panel+0xa00, = 2)
 *
 * (b) k618_bl_parse() -- after the em-cycle reads, add:
 *         b->panel_state = K618_PANEL_STATE_ON;
 *         of_property_read_u32(np, "mi,doze-hbm-dbv-level", &b->doze_hbm_dbv);
 *         of_property_read_u32(np, "mi,doze-lbm-dbv-level", &b->doze_lbm_dbv);
 *         of_property_read_u32(np, "mi,local-hbm-ui-ready-delay-num-frame", &b->ui_ready_frames);
 *
 * (c) k618_ioctl() -- SET_LOCAL_HBM (0x0E) and SET_DOZE_BRIGHTNESS (0x02). The 0x0E case replaces
 *     the current "no-op default" for that nr; 0x02 replaces the store-only block:
 *
 *       case _IOC_NR(MI_DISP_IOCTL_SET_LOCAL_HBM): {       // 0x0E, struct disp_feature_req-like
 *               struct disp_feature_req *r = (void *)kdata;  // [+4] carries the mfp code
 *               rc = k618_set_local_hbm(r->base.disp_id, r->feature_val);
 *               break;
 *       }
 *       case _IOC_NR(MI_DISP_IOCTL_SET_DOZE_BRIGHTNESS): { // 0x02
 *               struct disp_doze_brightness_req *r = (void *)kdata;
 *               if (disp_ok(r->base.disp_id)) {
 *                       k618_doze(g_mi->disp[r->base.disp_id], r->doze_brightness);
 *                       g_mi->doze_brightness[r->base.disp_id] = r->doze_brightness;
 *                       k618_notify_u32(r->base.disp_id, MI_DISP_EVENT_DOZE, r->doze_brightness);
 *               }
 *               break;
 *       }
 *     and add the macro near the other ioctls:
 *       #define MI_DISP_IOCTL_SET_LOCAL_HBM _IOW('D', 0x0E, struct disp_feature_req)
 *
 * (d) doze_brightness_store() -- call k618_doze() before storing, as the ioctl does.
 *
 * (e) k618_mi_disp_pre_nolp() -- doze-dbv fix (replaces the current last_bl patch):
 *       if a doze level is active (b->panel_state is DOZE_HIGH/LOW), the nolp 51h must be the doze
 *       dbv (stock mi_dsi_panel_set_nolp_locked), not last_bl:
 *           u32 lvl = (b->panel_state == K618_PANEL_STATE_DOZE_HIGH ||
 *                      b->panel_state == K618_PANEL_STATE_DOZE_LOW) ? k618_doze_dbv(b) : b->last_bl;
 *           if (!lvl) return;  // don't write 51h = 0
 *           k618_dbv2(dbv, lvl); k618_patch_reg(b, panel, K618_UPD_NOLP, 0x51, dbv, 2, true);
 *       and in k618_mi_disp_post_nolp(): b->panel_state = K618_PANEL_STATE_ON;
 */
