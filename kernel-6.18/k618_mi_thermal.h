/* SPDX-License-Identifier: GPL-2.0 */
/* Device compatibility. */
#ifndef _K618_MI_THERMAL_H
#define _K618_MI_THERMAL_H

#include <linux/types.h>

struct drm_panel;

/* ---- (A) panel_event_notifier: the subset we use, names as in the vendor header ---- */

enum panel_event_notifier_tag {
	PANEL_EVENT_NOTIFICATION_NONE,
	PANEL_EVENT_NOTIFICATION_PRIMARY,
	PANEL_EVENT_NOTIFICATION_SECONDARY,
	PANEL_EVENT_NOTIFICATION_MAX
};

/*
 * The client handle is a plain index into panel_event_notifier_entries[], sized
 * PANEL_EVENT_NOTIFIER_CLIENT_MAX, and a second register with the same index returns -EEXIST. The
 * vendor enum in this soc tree carries OPLUS additions, so the numbers differ from the 5.15 one the
 * stock module was built against; we therefore do not name the slots at all. The module asks for
 * K618_PANEL_CLIENT (3, the slot the stock module used: "mov w1, #0x3" at 0x504 of its
 * screen_state_check) and, on -EEXIST, walks up to K618_PANEL_CLIENT_MAX for a free one.
 */
#define K618_PANEL_CLIENT	3
#define K618_PANEL_CLIENT_MAX	21	/* PANEL_EVENT_NOTIFIER_CLIENT_MAX in this soc tree */

enum panel_event_notification_type {
	DRM_PANEL_EVENT_NONE,
	DRM_PANEL_EVENT_BLANK,		/* 1: screen off */
	DRM_PANEL_EVENT_UNBLANK,	/* 2: screen on */
	DRM_PANEL_EVENT_BLANK_LP,	/* 3: low power / AOD */
	DRM_PANEL_EVENT_FPS_CHANGE,
};

struct panel_event_notification_data {
	u32 old_fps;
	u32 new_fps;
	bool early_trigger;
	/* layout only: as the soc header with CONFIG_QCOM_PANEL_EVENT_NOTIFIER (see above) */
	int *lcd_ctl_blank;
	u32 data;
};

struct panel_event_notification {
	enum panel_event_notification_type notif_type;
	struct panel_event_notification_data notif_data;
	/* struct drm_panel *panel; -- unused here */
};

typedef void (*panel_event_notifier_handler)(enum panel_event_notifier_tag tag,
					     struct panel_event_notification *notification,
					     void *pvt_data);

void *panel_event_notifier_register(enum panel_event_notifier_tag tag,
				    int client_handle,
				    struct drm_panel *panel,
				    panel_event_notifier_handler notif_handler,
				    void *pvt_data);
void panel_event_notifier_unregister(void *cookie);

/* ---- (B) Xiaomi's charging driver, when it is there ---- */

/*
 * Stock mi_thermal_interface.ko calls this from thermal_board_sensor_temp_store (0xbb4) as
 *   charger_notifier_call_chain(0, board_temp_millicelsius / 1000)
 * -- x0 = 0 (the event id) and w1 = the temperature in whole degrees C (the 0x51eb851f / asr 37
 * divide-by-1000 at 0xb98..0xbb0). The second argument being a 32-bit value in w1, not a pointer, is
 * why it is declared int here; the event id 0 is named below. Both are taken from the call site
 * only -- the real prototype lives in Xiaomi's qti_battery_charger, which we do not have yet, so
 * the charging-driver port must match this or change it here. See k618_mi_thermal_notes.md.
 */
#define MI_CHARGER_EVENT_BOARD_TEMP	0

/* reached only through symbol_get() (see (B) above): never referenced directly */
extern int charger_notifier_call_chain(unsigned long val, int v);

#endif /* _K618_MI_THERMAL_H */
