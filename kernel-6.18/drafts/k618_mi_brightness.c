// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */

/* ===================== for k618_mi_disp.h (the hooks dsi_panel.c calls) ===================== */
#ifndef K618_MI_BRIGHTNESS_HOOKS
#define K618_MI_BRIGHTNESS_HOOKS
/* dsi_panel_set_backlight(), DCS, before the 51h write: em-cycle switch + dead zone; false = skip 51h */
bool k618_mi_disp_bl_pre(struct dsi_panel *panel, u32 *bl_lvl);
/* dsi_panel_switch(), before DSI_CMD_SET_TIMING_SWITCH: em bytes of the new mode for the current level */
void k618_mi_disp_timing_switch(struct dsi_panel *panel);
/* dsi_panel_send_qsync_on/off_dcs(), before the command */
void k618_mi_disp_qsync(struct dsi_panel *panel, bool on);
/* dsi_panel_set_nolp(), around DSI_CMD_SET_NOLP */
void k618_mi_disp_pre_nolp(struct dsi_panel *panel);
void k618_mi_disp_post_nolp(struct dsi_panel *panel, int rc);
/* dsi_panel_disable(), after the panel is marked off */
void k618_mi_disp_panel_off(struct dsi_panel *panel);
#endif

/* ===================== near the top of k618_mi_disp.c ===================== */

#include <linux/of.h>

/* the 13 Pro's mi_disp: set_disp_param notifies event type 7 after an HBM change (stock 0x18cecc) */
#define MI_DISP_EVENT_HBM 7
/* the 13 Pro's feature ids beyond the 12 Pro's 0..21 (stock feature name table .rodata+0x899e8) */
#define DISP_FEATURE_HBM_BACKLIGHT 22	/* "hbm_backlight_level": MiBrightness::writeDBVCmd() */
#define DISP_FEATURE_BACKLIGHT     23	/* "backlight" */

/* mi,panel-id of this phone's panel, as the stock compares it (8 bytes "\0\0M2\08\f\0", big endian) */
#define K618_M2_PANEL_ID 0x00004D3200380C00ULL

/* Device compatibility. */
static bool em_cycle = true;
module_param(em_cycle, bool, 0644);
/* -1: follow the panel's mi,disable-ic-dimming-flag (stock); 0: never send dimming on/off; 1: always */
static int ic_dimming = -1;
module_param(ic_dimming, int, 0644);

/* ===================== brightness state ===================== */

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
	if (!np)
		return;

	b->dt_no_ic_dimming = of_property_read_bool(np, "mi,disable-ic-dimming-flag");

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

/* ===================== hooks (dsi_panel.c, panel_lock held) ===================== */

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

/*
 * nolp = the ROM's DOZE -> ON. Its DT bytes: 53 20, 51 00 00, then 65 04 + BD 21 8F + the 16-pulse
 * table -- the panel comes out in 16-pulse mode at level 0. Stock patches the 51h with the doze level
 * and sets the 16-pulse state (mi_dsi_panel_set_nolp_locked); here the level is the last one written
 * (6.18 sends no doze commands, so what the framework wrote during DOZE is on the panel).
 */
void k618_mi_disp_pre_nolp(struct dsi_panel *panel)
{
	struct k618_bl *b = k618_bl_get(panel);
	u8 dbv[2];

	if (!b || !b->is_m2 || !b->last_bl)
		return;
	dbv[0] = (b->last_bl >> 8) & 0xff;
	dbv[1] = b->last_bl & 0xff;
	k618_patch_reg(b, panel, K618_UPD_NOLP, 0x51, dbv, 2, true);
}

void k618_mi_disp_post_nolp(struct dsi_panel *panel, int rc)
{
	struct k618_bl *b = k618_bl_get(panel);

	if (!b || !b->is_m2 || rc)
		return;
	b->em_16p = true;
	/*
	 * not in stock (its doze levels, 0x14 and 0xf5, are below the threshold): if the level on the
	 * panel is above it, leave 16-pulse now rather than at the next level change
	 */
	if (b->last_bl > b->thr)
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
	if (g_mi)
		g_mi->feature_val[k618_panel_id(panel)][DISP_FEATURE_HBM] = 0;
}

/* ===================== the feature path (takes panel_lock) ===================== */

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

/* ===================== replacements in k618_mi_disp.c ===================== */

/* replaces k618_set_feature() */
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
	default:
		/* FOD / LHBM / AOD / count / MI-SF notify ids: accept so the HAL stops failing */
		break;
	}
	if (feature_id < DISP_FEATURE_MAX)
		mi->feature_val[disp_id][feature_id] = val;
	pr_debug("disp%d feature %u = %d (rc %d)\n", disp_id, feature_id, val, rc);
	return rc;
}

/* replaces k618_mi_disp_panel_on(): no panel-on dimming where the panel disables IC dimming */
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
 * replaces k618_mi_disp_bl(): called after the 51h write (or in its place when qsync skips it) with
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
