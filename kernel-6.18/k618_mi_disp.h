/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mi_disp_k618.h: the hooks the rest of msm_drm uses to reach the trimmed mi_disp port in
 * mi_disp_k618.c (drafts/k618_midisp_v2.c). Compiled into msm_drm; no symbol export needed.
 */
#ifndef _MI_DISP_K618_H_
#define _MI_DISP_K618_H_

struct dsi_cmd_desc;
struct dsi_display;
struct dsi_display_mode;
struct dsi_panel;

/* create the char device + class + sysfs once; safe to call more than once */
int  k618_mi_disp_init(void);
void k618_mi_disp_exit(void);

/* bind a DSI display (and create its disp-DSI-<n> sysfs device) when its panel is up */
void k618_mi_disp_attach(struct dsi_display *display);

/* panel enable: fire the POWER event, the reads stock does once the panel is on, panel-on dimming */
void k618_mi_disp_panel_on(struct dsi_panel *panel);

/* the panel's backlight path passes the 51h level through as a 51_brightness event */
void k618_mi_disp_bl(struct dsi_panel *panel, u32 bl_lvl);

/* generic event injection (disp_id, event type, payload length, payload as a u64) */
void k618_mi_disp_event(int disp_id, u32 type, u32 len, u64 val);

/* the brightness chain as the phone's 5.15 msm_drm runs it (k618_mi_disp.c) */
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
/* dsi_panel_set_lp1/lp2(), after the command, panel_lock held: AOD entered */
void k618_mi_disp_post_lp(struct dsi_panel *panel, int rc);

/* the 13 (M3) and 13 Ultra (M1) panels */
/* dsi_panel_get_mode(), after dsi_panel_parse_dsc_params(): the 13 Ultra's DSC rate-control values */
void k618_mi_disp_dsc(struct dsi_panel *panel, struct dsi_display_mode *mode);
/* dsi_panel_post_switch(), panel_lock held, before DSI_CMD_SET_POST_TIMING_SWITCH */
void k618_mi_disp_post_switch(struct dsi_panel *panel);

/* reads from the panel, and the display parameters the bootloader passes */
/* dsi_display_enable(), continuous-splash path (no dsi_panel_enable()): the same reads, no lock held */
void k618_mi_disp_splash(struct dsi_panel *panel);
/* in dsi_display.c: msm_drm.dsi_display0 (idx 0) / dsi_display1 (idx 1) as the bootloader passed them */
const char *k618_dsi_boot_param(int idx);
/* in dsi_display.c: one read; panel_locked = the caller holds panel_lock and the panel is on */
int k618_dsi_display_read(struct dsi_display *display, struct dsi_cmd_desc *cmd, bool panel_locked);
/* in dsi_display.c: dsi_display_cmd_rx() without taking panel_lock */
int k618_dsi_display_cmd_rx_locked(struct dsi_display *display, struct dsi_cmd_desc *cmd);

#endif /* _MI_DISP_K618_H_ */
