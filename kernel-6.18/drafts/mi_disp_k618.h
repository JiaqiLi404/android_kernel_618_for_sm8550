/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mi_disp_k618.h: the hooks the rest of msm_drm uses to reach the trimmed mi_disp port in
 * mi_disp_k618.c (drafts/k618_midisp_v2.c). Compiled into msm_drm; no symbol export needed.
 */
#ifndef _MI_DISP_K618_H_
#define _MI_DISP_K618_H_

struct dsi_display;
struct dsi_panel;

/* create the char device + class + sysfs once; safe to call more than once */
int  k618_mi_disp_init(void);
void k618_mi_disp_exit(void);

/* bind a DSI display (and create its disp-DSI-<n> sysfs device) when its panel is up */
void k618_mi_disp_attach(struct dsi_display *display);

/* panel enable: fire the POWER event and schedule panel-on dimming */
void k618_mi_disp_panel_on(struct dsi_panel *panel);

/* the panel's backlight path passes the 51h level through as a 51_brightness event */
void k618_mi_disp_bl(struct dsi_panel *panel, u32 bl_lvl);

/* generic event injection (disp_id, event type, payload length, payload as a u64) */
void k618_mi_disp_event(int disp_id, u32 type, u32 len, u64 val);

#endif /* _MI_DISP_K618_H_ */
