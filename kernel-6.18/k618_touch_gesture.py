#!/usr/bin/env python3
# k618_touch_gesture.py <dir>: stage 1 of the touch features -- gesture wake (double tap, single
# tap), palm, and the screen-off suspend they hang on -- for the 12 Pro's fts_spi + xiaomi touch
# drivers as k618_touch_fix.py leaves them. Run it right after k618_touch_fix.py on the same tree
# (k618_touch.sh: `python3 -I $KD/k618_touch_gesture.py $D`), or append EDITS to that file's list.
#
# Every change is taken from the phone's own 5.15 modules (vendor_dlkm fts_touch_spi.ko /
# xiaomi_touch.ko, symbols kept, disassembled with llvm-objdump; the function names in the
# comments are theirs). Nothing here writes the touch IC's flash: the commands are the run-time
# ones the stock driver sends (A2 03 gesture mask, A0 scan mode, C0 14 palm), and the safety part
# at the end refuses every flash / CX-save / lockdown path the 12 Pro's tree still has.
#
# Build side (k618_touch.sh): KCFLAGS needs -DCONFIG_QCOM_PANEL_EVENT_NOTIFIER_MODULE=1, or the soc
# header's panel_event_notifier_register() is an inline stub returning ERR_PTR(-EOPNOTSUPP) -- which
# is what every image up to now built (fts_touch_spi.ko does not even import the symbol).
import os
import sys

EDITS = [
    # ------------------------------------------------------------------ build-wide switch
    # k618_stock: the phone's own FOD / single-tap / palm rules (default). 0 = the 12 Pro driver's
    # rules, kept for an A/B: fts_touch_spi.k618_stock=0 on the command line or in the parameter
    # file before the screen goes off.
    ("fts_spi/fts.c",
     "#if defined(CONFIG_DRM)\nstatic struct drm_panel *active_panel;\n#endif\n",
     "#if defined(CONFIG_DRM)\nstatic struct drm_panel *active_panel;\n#endif\n"
     "/* k618: 1 = the phone's own 5.15 driver's FOD / single-tap / palm rules, 0 = the 12 Pro's */\n"
     "static bool k618_stock = true;\n"
     "module_param(k618_stock, bool, 0644);\n"),

    # ------------------------------------------------------------------ 1. panel events
    # The 12 Pro code registers in fts_init_sensing() with `if (!cookie)` -- but the stub returns
    # ERR_PTR(-EOPNOTSUPP), and the real function returns ERR_PTR(-EEXIST) the second time
    # (fts_init_sensing() runs again after every firmware-update request), both "success" to it.
    # Stock does it in fts_register_panel_notifier_work: PRIMARY tag, client PRIMARY_TOUCH, the DT
    # "panel" phandles, retried every 5 s (1250 jiffies) up to 5 times.
    ("fts_spi/fts.c",
     "static int fts_init_sensing(struct fts_ts_info *info)\n{\n\tint error = 0;\n"
     "#if defined(CONFIG_DRM)\n"
     "\tif (active_panel) {\n"
     "\t\tinfo->notifier_cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,\n"
     "\t\t\t\tPANEL_EVENT_NOTIFIER_CLIENT_PRIMARY_TOUCH, active_panel,\n"
     "\t\t\t\t&fts_drm_panel_notifier_callback, (void *)info);\n"
     "\t\tif (!info->notifier_cookie) {\n"
     "\t\t\tpr_err(\"Failed to register for panel events\\n\");\n"
     "\t\t\treturn ERROR_BUS_R;\n"
     "\t\t}\n"
     "\t}\n"
     "#endif\n",
     "#if defined(CONFIG_DRM)\n"
     "/*\n"
     " * k618: screen on/off for the touch driver (stock fts_register_panel_notifier_work). Without it\n"
     " * the driver never suspends: no gesture mode, no double / single tap, the IC scanning at full\n"
     " * rate with the screen off. Registers once and keeps the cookie only when it is a real one.\n"
     " */\n"
     "static int fts_ts_check_panel(struct device_node *np);\n"
     "static void k618_fts_panel_work(struct work_struct *work);\n"
     "static DECLARE_DELAYED_WORK(k618_panel_dw, k618_fts_panel_work);\n"
     "static int k618_panel_tries;\n"
     "\n"
     "static void k618_fts_panel_register(struct fts_ts_info *info)\n"
     "{\n"
     "\tvoid *cookie;\n"
     "\n"
     "\tif (info->notifier_cookie)\n"
     "\t\treturn;\n"
     "\tif (!active_panel && info->dev && info->dev->of_node)\n"
     "\t\tfts_ts_check_panel(info->dev->of_node);\n"
     "\tif (!active_panel) {\n"
     "\t\tif (k618_panel_tries++ < 5) {\n"
     "\t\t\tlogError(1, \"%s k618: no panel yet, panel events retried in 5 s\\n\", tag);\n"
     "\t\t\tschedule_delayed_work(&k618_panel_dw, msecs_to_jiffies(5000));\n"
     "\t\t} else {\n"
     "\t\t\tlogError(1, \"%s k618: no panel, no panel events\\n\", tag);\n"
     "\t\t}\n"
     "\t\treturn;\n"
     "\t}\n"
     "\tcookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,\n"
     "\t\t\tPANEL_EVENT_NOTIFIER_CLIENT_PRIMARY_TOUCH, active_panel,\n"
     "\t\t\t&fts_drm_panel_notifier_callback, (void *)info);\n"
     "\tif (IS_ERR_OR_NULL(cookie)) {\n"
     "\t\tlogError(1, \"%s k618: panel events not registered (%ld)\\n\", tag, PTR_ERR(cookie));\n"
     "\t\treturn;\n"
     "\t}\n"
     "\tinfo->notifier_cookie = cookie;\n"
     "\tlogError(1, \"%s k618: panel events registered\\n\", tag);\n"
     "}\n"
     "\n"
     "static void k618_fts_panel_work(struct work_struct *work)\n"
     "{\n"
     "\tif (fts_info)\n"
     "\t\tk618_fts_panel_register(fts_info);\n"
     "}\n"
     "#endif\n"
     "\n"
     "static int fts_init_sensing(struct fts_ts_info *info)\n{\n\tint error = 0;\n"
     "#if defined(CONFIG_DRM)\n"
     "\tk618_fts_panel_register(info);\n"
     "#endif\n"),
    ("fts_spi/fts.c",
     "#if defined(CONFIG_DRM)\n"
     "\tif (active_panel && info->notifier_cookie)\n"
     "\t\tpanel_event_notifier_unregister(info->notifier_cookie);\n"
     "#endif\n"
     "\t/* unregister the device */\n",
     "#if defined(CONFIG_DRM)\n"
     "\tcancel_delayed_work_sync(&k618_panel_dw);\n"
     "\tif (active_panel && info->notifier_cookie)\n"
     "\t\tpanel_event_notifier_unregister(info->notifier_cookie);\n"
     "\tinfo->notifier_cookie = NULL;\n"
     "#endif\n"
     "\t/* unregister the device */\n"),

    # The callback as stock fts_drm_panel_notifier_callback has it: msm_drm sends every change
    # twice (early_trigger 1 before the commit, 0 after); stock suspends on the late BLANK /
    # BLANK_LP and resumes on the early UNBLANK, flushes before it checks sensor_sleep, and drops
    # FPS changes unlogged (MIUI switches the refresh rate many times a minute).
    ("fts_spi/fts.c",
     "\tlogError(1, \"%s %s: Notification type:%d, early_trigger:%d\", tag,\n"
     "\t\t\t __func__,\n"
     "\t\t\tnotification->notif_type,\n"
     "\t\t\tnotification->notif_data.early_trigger);\n"
     "\tswitch (notification->notif_type) {\n"
     "\tcase DRM_PANEL_EVENT_UNBLANK:\n"
     "\t\tif (!info->sensor_sleep)\n"
     "\t\t\treturn;\n"
     "\t\tlogError(1, \"%s %s: FB_BLANK_UNBLANK\\n\", tag,\n"
     "\t\t\t __func__);\n"
     "\t\tflush_workqueue(info->event_wq);\n"
     "\t\tqueue_work(info->event_wq, &info->resume_work);\n"
     "\t\tbreak;\n"
     "\tcase DRM_PANEL_EVENT_BLANK:\n"
     "\tcase DRM_PANEL_EVENT_BLANK_LP:\n"
     "\t\tif (info->sensor_sleep)\n"
     "\t\t\treturn;\n"
     "\t\tlogError(1, \"%s %s: FB_BLANK %s\\n\", tag,\n"
     "\t\t\t __func__, notification->notif_type == DRM_PANEL_EVENT_BLANK_LP ? \"POWER DOWN\" : \"LP\");\n"
     "\t\tflush_workqueue(info->event_wq);\n"
     "\t\tqueue_work(info->event_wq, &info->suspend_work);\n"
     "\t\tbreak;\n"
     "\tcase DRM_PANEL_EVENT_FPS_CHANGE:\n"
     "\t\tlogError(1, \"%s %s: shashank:Received fps change old fps:%d new fps:%d\\n\",\n"
     "\t\t\t\ttag, __func__,\n"
     "\t\t\t\tnotification->notif_data.old_fps,\n"
     "\t\t\t\tnotification->notif_data.new_fps);\n"
     "\t\tbreak;\n",
     "\t/* k618: as stock -- fps changes are not ours, and not logged */\n"
     "\tif (notification->notif_type == DRM_PANEL_EVENT_FPS_CHANGE)\n"
     "\t\treturn;\n"
     "\tlogError(1, \"%s %s: Notification type:%d, early_trigger:%d\\n\", tag,\n"
     "\t\t\t __func__,\n"
     "\t\t\tnotification->notif_type,\n"
     "\t\t\tnotification->notif_data.early_trigger);\n"
     "\tswitch (notification->notif_type) {\n"
     "\tcase DRM_PANEL_EVENT_UNBLANK:\n"
     "\t\t/* k618: stock resumes on the early unblank, so touch is up with the panel */\n"
     "\t\tif (!notification->notif_data.early_trigger)\n"
     "\t\t\treturn;\n"
     "\t\tlogError(1, \"%s %s: FB_BLANK_UNBLANK\\n\", tag,\n"
     "\t\t\t __func__);\n"
     "\t\tflush_workqueue(info->event_wq);\n"
     "\t\tif (!info->sensor_sleep)\n"
     "\t\t\treturn;\n"
     "\t\tqueue_work(info->event_wq, &info->resume_work);\n"
     "\t\tbreak;\n"
     "\tcase DRM_PANEL_EVENT_BLANK:\n"
     "\tcase DRM_PANEL_EVENT_BLANK_LP:\n"
     "\t\t/* k618: stock suspends after the commit */\n"
     "\t\tif (notification->notif_data.early_trigger)\n"
     "\t\t\treturn;\n"
     "\t\tlogError(1, \"%s %s: FB_BLANK %s\\n\", tag,\n"
     "\t\t\t __func__, notification->notif_type == DRM_PANEL_EVENT_BLANK_LP ? \"LP\" : \"POWER DOWN\");\n"
     "\t\tflush_workqueue(info->event_wq);\n"
     "\t\tif (info->sensor_sleep)\n"
     "\t\t\treturn;\n"
     "\t\tqueue_work(info->event_wq, &info->suspend_work);\n"
     "\t\tbreak;\n"),

    # ------------------------------------------------------------------ 2. which gestures
    # The HAL's Touch_Fod_Enable values on this ROM are 0 (off), 1 (authenticating), 2 (enrolling)
    # and 3 (fingerprint on, keyguard not up: stock turns it into 1 at screen off); stock treats
    # only 1 and 2 as "fingerprint enabled" (fts_enter_pointer_event_handler, the gesture handler,
    # fts_mode_handler). The 12 Pro's rule (anything but 0 / -1 / 100) also counts 3.
    ("fts_spi/fts.c",
     "\tif (fts_info->fod_status != 0 && fts_info->fod_status != -1 && fts_info->fod_status != 100)\n"
     "\t\treturn true;\n",
     "\tif (k618_stock)\t/* k618: stock -- 1 authenticating, 2 enrolling */\n"
     "\t\treturn fts_info->fod_status == 1 || fts_info->fod_status == 2;\n"
     "\tif (fts_info->fod_status != 0 && fts_info->fod_status != -1 && fts_info->fod_status != 100)\n"
     "\t\treturn true;\n"),
    # The gesture mask for screen off (stock fts_mode_handler / fts_switch_mode_work): nonui 2
    # (phone in a pocket) leaves only double tap; single tap (bit 1) needs AOD or the FOD icon and
    # nonui 0; FOD long press (bit 0) needs fingerprint 1 or 2.
    ("fts_spi/fts.c",
     "\tu8 tmp_value = 0;\n\n"
     "\tif (fts_info->aod_status && !fts_info->nonui_status)\n"
     "\t\ttmp_value |= FOD_SINGLETAP_EVENT;\n",
     "\tu8 tmp_value = 0;\n\n"
     "\tif (k618_stock) {\n"
     "\t\tint nonui = fts_info->nonui_status;\n"
     "\n"
     "\t\tif (nonui == 2)\n"
     "\t\t\treturn 0;\n"
     "\t\tif (fts_info->aod_status && !nonui)\n"
     "\t\t\ttmp_value |= FOD_SINGLETAP_EVENT;\n"
     "\t\tif (fts_info->fod_status == 1 || fts_info->fod_status == 2) {\n"
     "\t\t\ttmp_value |= FOD_LONGPRESS_EVENT;\n"
     "\t\t\tif (fts_info->fod_icon_status && !nonui)\n"
     "\t\t\t\ttmp_value |= FOD_SINGLETAP_EVENT;\n"
     "\t\t}\n"
     "\t\treturn tmp_value;\n"
     "\t}\n"
     "\tif (fts_info->aod_status && !fts_info->nonui_status)\n"
     "\t\ttmp_value |= FOD_SINGLETAP_EVENT;\n"),
    # Single tap: stock reports KEY_GOTO and then notify_gesture_single_tap() (a POLLPRI on
    # /sys/class/touch/touch_dev/gesture_single_tap_state).
    ("fts_spi/fts.c",
     "\t\t\tinput_report_key(info->input_dev, KEY_GOTO, 0);\n"
     "\t\t\tinput_sync(info->input_dev);\n"
     "\t\t\tinfo->sleep_finger = 0;\n",
     "\t\t\tinput_report_key(info->input_dev, KEY_GOTO, 0);\n"
     "\t\t\tinput_sync(info->input_dev);\n"
     "\t\t\tnotify_gesture_single_tap();\t/* k618: stock */\n"
     "\t\t\tinfo->sleep_finger = 0;\n"),


    ("fts_spi/fts.c",
     "\tfts_info->fod_status = value;\n"
     "\tif (fts_info->fod_status == 2) {\n"
     "\t\tmutex_lock(&fts_info->fod_mutex);\n"
     "\t\tres = fts_write(gesture_cmd, ARRAY_SIZE(gesture_cmd));\n",
     "\tif (k618_stock) {\n"
     "\t\tpm_stay_awake(fts_info->dev);\n"
     "\t\tif (fts_info->fod_pressed) {\n"
     "\t\t\tif (fts_info->fod_status == value)\n"
     "\t\t\t\tlogError(1, \"%s %s has already set and process:%d\\n\", tag, __func__, value);\n"
     "\t\t\telse\n"
     "\t\t\t\tfts_info->fod_status = value;\n"
     "\t\t\tpm_relax(fts_info->dev);\n"
     "\t\t\treturn 0;\n"
     "\t\t}\n"
     "\t\tfts_info->fod_status = value;\n"
     "\t\tif (value == 1 || value == 2) {\n"
     "\t\t\tmutex_lock(&fts_info->fod_mutex);\n"
     "\t\t\tif (fts_info->gesture_enabled == 1)\n"
     "\t\t\t\tgesture_cmd[2] = 0x20;\n"
     "\t\t\tif (!fts_info->resume_bit) {\n"
     "\t\t\t\tsetScanMode(SCAN_MODE_LOW_POWER, 0);\n"
     "\t\t\t\tmdelay(15);\n"
     "\t\t\t\tfts_enableInterrupt();\n"
     "\t\t\t}\n"
     "\t\t\tres = fts_write_dma_safe(gesture_cmd, ARRAY_SIZE(gesture_cmd));\n"
     "\t\t\tif (res < OK)\n"
     "\t\t\t\tlogError(1, \"%s %s: enter gesture and longpress failed! ERROR %08X recovery in senseOff...\\n\",\n"
     "\t\t\t\t\ttag, __func__, res);\n"
     "\t\t\telse\n"
     "\t\t\t\tlogError(1, \"%s %s send gesture and longpress cmd success\\n\", tag, __func__);\n"
     "\t\t\tmsleep(12);\n"
     "\t\t\tmutex_unlock(&fts_info->fod_mutex);\n"
     "\t\t}\n"
     "\t\tpm_relax(fts_info->dev);\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\tfts_info->fod_status = value;\n"
     "\tif (fts_info->fod_status == 2) {\n"
     "\t\tmutex_lock(&fts_info->fod_mutex);\n"
     "\t\tres = fts_write_dma_safe(gesture_cmd, ARRAY_SIZE(gesture_cmd));\n"),

    # Double tap / nonui switched while the screen is off: stock fts_switch_mode_work skips while a
    # finger is on the sensor, turns sensing off when no gesture is left (or nonui is 2), and
    # otherwise re-enters low-power scan with the interrupt on before sending the mask. The 12 Pro
    # version only sends the mask (and leaves the IC in whatever scan mode it was).
    ("fts_spi/fts.c",
     "\tif (info->resume_bit) {\n"
     "\t\tlogError(1, \"%s %s touch in resume mode, don't need to set gesture cmds\\n\", tag, __func__);\n"
     "\t\treturn;\n"
     "\t}\n"
     "\tif (info->gesture_enabled)\n"
     "\t\tgesture_cmd[2] = 0x20;\n",
     "\tif (info->resume_bit) {\n"
     "\t\tlogError(1, \"%s %s touch in resume mode, don't need to set gesture cmds\\n\", tag, __func__);\n"
     "\t\treturn;\n"
     "\t}\n"
     "\tif (k618_stock) {\n"
     "\t\tif (info->fod_pressed) {\n"
     "\t\t\tlogError(0, \"%s %s: fod pressed, skip set cmd, gesture_type: %d, gesture_enable: %d\\n\",\n"
     "\t\t\t\t tag, __func__, gesture_type, info->gesture_enabled);\n"
     "\t\t\treturn;\n"
     "\t\t}\n"
     "\t\tpm_stay_awake(info->dev);\n"
     "\t\tif ((!gesture_type && !info->gesture_enabled) || info->nonui_status == 2) {\n"
     "\t\t\tlogError(0, \"%s %s: Sense OFF! \\n\", tag, __func__);\n"
     "\t\t\tsetScanMode(SCAN_MODE_ACTIVE, 0x00);\n"
     "\t\t\tmdelay(WAIT_AFTER_SENSEOFF);\n"
     "\t\t} else {\n"
     "\t\t\tlogError(0, \"%s %s: enter low power mode! gesture_type: %d, gesture_enable: %d\\n\",\n"
     "\t\t\t\t tag, __func__, gesture_type, info->gesture_enabled);\n"
     "\t\t\tsetScanMode(SCAN_MODE_LOW_POWER, 0);\n"
     "\t\t\tmdelay(15);\n"
     "\t\t\tfts_enableInterrupt();\n"
     "\t\t\tif (info->gesture_enabled)\n"
     "\t\t\t\tgesture_cmd[2] = 0x20;\n"
     "\t\t\tres = fts_write_dma_safe(gesture_cmd, ARRAY_SIZE(gesture_cmd));\n"
     "\t\t\tif (res < OK)\n"
     "\t\t\t\tlogError(1, \"%s %s: send gesture cmd error! ERROR %08X recovery in senseOff...\\n\",\n"
     "\t\t\t\t\t tag, __func__, res);\n"
     "\t\t}\n"
     "\t\tpm_relax(info->dev);\n"
     "\t\treturn;\n"
     "\t}\n"
     "\tif (info->gesture_enabled)\n"
     "\t\tgesture_cmd[2] = 0x20;\n"),

    # ------------------------------------------------------------------ 3. suspend / resume
    # Stock fts_suspend_work: palm detection is switched off on the IC but the switch is kept, so
    # resume turns it back on (the 12 Pro clears it and palm stays off after every screen-off);
    # fod_status 3 becomes 1 (the lock screen will want the fingerprint); fod_pressed is cleared.
    ("fts_spi/fts.c",
     "\t\tupdate_palm_sensor_value(0);\n"
     "\t\tfts_palm_sensor_cmd(0);\n"
     "\t\tinfo->palm_sensor_switch = false;\n"
     "\t}\n"
     "#endif\n"
     "\tfts_disableInterrupt();\n"
     "\tinfo->resume_bit = 0;\n"
     "\tfts_mode_handler(info, 0);\n"
     "\trelease_all_touches(info);\n",
     "\t\tupdate_palm_sensor_value(0);\n"
     "\t\tfts_palm_sensor_cmd(0);\n"
     "\t\tif (k618_stock)\n"
     "\t\t\tinfo->palm_sensor_changed = false;\t/* k618: stock -- resume turns it on again */\n"
     "\t\telse\n"
     "\t\t\tinfo->palm_sensor_switch = false;\n"
     "\t}\n"
     "#endif\n"
     "\tfts_disableInterrupt();\n"
     "\tinfo->resume_bit = 0;\n"
     "\tif (k618_stock && info->fod_status == 3)\n"
     "\t\tinfo->fod_status = 1;\t/* k618: stock */\n"
     "\tfts_mode_handler(info, 0);\n"
     "\trelease_all_touches(info);\n"
     "\tinfo->fod_pressed = false;\t/* k618: stock */\n"),
    # Stock fts_resume_work ends with nonui (pocket) cleared: the screen is on again.
    ("fts_spi/fts.c",
     "\t\tfts_enable_thp_cmd(1);\n"
     "\t}\n"
     "#endif\n"
     "\n"
     "\txiaomi_touch_set_suspend_state(XIAOMI_TOUCH_RESUME);\n",
     "\t\tfts_enable_thp_cmd(1);\n"
     "\t}\n"
     "#endif\n"
     "\n"
     "\tif (k618_stock)\n"
     "\t\tinfo->nonui_status = 0;\t/* k618: stock */\n"
     "\txiaomi_touch_set_suspend_state(XIAOMI_TOUCH_RESUME);\n"),

    # ------------------------------------------------------------------ 4. THP stays off here
    # The 12 Pro's raw-data switch (sysfs enable_touch_raw) flashes a THP firmware (TOUCH_THP_FW)
    # or sends its own C0 15 command; this IC runs THP firmware natively and is switched with
    # C0 21 (stock fts_enable_thp_onoff). Until the THP stage it refuses, which also keeps that
    # flashProcedure() path out of the module (the old body becomes an unused static function).
    ("fts_spi/fts.c",
     "int fts_enable_touch_raw(bool en)\n{\n#ifdef TOUCH_THP_FW\n",
     "/* k618: THP is not switched on in this stage (see k618_touch_gesture.py) */\n"
     "int fts_enable_touch_raw(bool en)\n"
     "{\n"
     "\tlogError(1, \"%s k618: enable_touch_raw %d refused, THP stays off\\n\", tag, en);\n"
     "\treturn -EOPNOTSUPP;\n"
     "}\n"
     "\n"
     "static int __maybe_unused k618_zeus_enable_touch_raw(bool en)\n{\n#ifdef TOUCH_THP_FW\n"),

    # ------------------------------------------------------------------ 5. xiaomi_touch
    ("xiaomi/xiaomi_touch.h",
     "\tint fod_pressed;\t\t/* k618: what fod_press_status reads */\n",
     "\tint fod_pressed;\t\t/* k618: what fod_press_status reads */\n"
     "\tint palm_onoff;\t\t\t/* k618: the last palm_sensor request (palm_sensor_data reads it) */\n"),
    ("xiaomi/xiaomi_touch.h",
     "extern int update_fod_press_status(int value);\n",
     "extern int update_fod_press_status(int value);\n\n"
     "/* k618: stock -- a POLLPRI on gesture_single_tap_state after a single tap */\n"
     "extern int notify_gesture_single_tap(void);\n"),
    # palm_sensor (the sensor HAL's switch), stock palm_sensor_store: remember the request, pass it
    # to the driver, notify palm_sensor_data (where a touch daemon would pick it up).
    ("xiaomi/xiaomi_touch.c",
     "\tif (pdata->touch_data[0]->palm_sensor_write)\n"
     "\t\tpdata->touch_data[0]->palm_sensor_write(!!input);\n"
     "\telse {\n"
     "\t\tpr_err(\"%s has not implement\\n\", __func__);\n"
     "\t}\n"
     "\tpr_info(\"%s value:%d\\n\", __func__, !!input);\n",
     "\tpdata->palm_onoff = input;\t/* k618: stock */\n"
     "\tif (pdata->touch_data[0]->palm_sensor_write)\n"
     "\t\tpdata->touch_data[0]->palm_sensor_write(!!input);\n"
     "\telse {\n"
     "\t\tpr_err(\"%s has not implement\\n\", __func__);\n"
     "\t}\n"
     "\tpr_info(\"%s value:%d\\n\", __func__, !!input);\n"
     "\tsysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, \"palm_sensor_data\");\n"),
    ("xiaomi/xiaomi_touch.c",
     "EXPORT_SYMBOL_GPL(update_fod_press_status);\n",
     "EXPORT_SYMBOL_GPL(update_fod_press_status);\n"
     "\n"
     "/*\n"
     " * k618: stock notify_gesture_single_tap(): gesture_single_tap_state always reads 1, the poll\n"
     " * wake-up is the event. Stock takes the device mutex around sysfs_notify(); it is left out here\n"
     " * -- the ioctl path holds that mutex across calls into fts that can wait for the IRQ thread\n"
     " * this is called from.\n"
     " */\n"
     "int notify_gesture_single_tap(void)\n"
     "{\n"
     "\tif (!touch_pdata || !xiaomi_touch_dev.dev)\n"
     "\t\treturn -ENODEV;\n"
     "\tsysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, \"gesture_single_tap_state\");\n"
     "\treturn 0;\n"
     "}\n"
     "EXPORT_SYMBOL_GPL(notify_gesture_single_tap);\n"
     "\n"
     "static ssize_t gesture_single_tap_value_show(struct device *dev,\n"
     "\t\tstruct device_attribute *attr, char *buf)\n"
     "{\n"
     "\treturn snprintf(buf, PAGE_SIZE, \"%d\\n\", 1);\n"
     "}\n"
     "\n"
     "/*\n"
     " * k618: stock palm_sensor_data. Read: the palm detection asked for through palm_sensor. Write:\n"
     " * a detection result (from a touch daemon) -- the same as update_palm_sensor_value() from the\n"
     " * IC: stored and notified on palm_sensor.\n"
     " */\n"
     "static ssize_t palm_sensor_data_show(struct device *dev,\n"
     "\t\tstruct device_attribute *attr, char *buf)\n"
     "{\n"
     "\tstruct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);\n"
     "\n"
     "\tpdata->palm_changed = false;\n"
     "\treturn snprintf(buf, PAGE_SIZE, \"%d\\n\", pdata->palm_onoff);\n"
     "}\n"
     "\n"
     "static ssize_t palm_sensor_data_store(struct device *dev,\n"
     "\t\tstruct device_attribute *attr, const char *buf, size_t count)\n"
     "{\n"
     "\tint input = 0;\n"
     "\n"
     "\tif (sscanf(buf, \"%d\", &input) < 1)\n"
     "\t\treturn -EINVAL;\n"
     "\tupdate_palm_sensor_value(input);\n"
     "\treturn count;\n"
     "}\n"),
    ("xiaomi/xiaomi_touch.c",
     "static DEVICE_ATTR(suspend_state, 0644, xiaomi_touch_suspend_state, NULL);\n",
     "static DEVICE_ATTR(suspend_state, 0644, xiaomi_touch_suspend_state, NULL);\n"
     "\n"
     "/* k618: stock modes */\n"
     "static DEVICE_ATTR(gesture_single_tap_state, 0664, gesture_single_tap_value_show, NULL);\n"
     "static DEVICE_ATTR(palm_sensor_data, 0664, palm_sensor_data_show, palm_sensor_data_store);\n"),
    ("xiaomi/xiaomi_touch.c",
     "\t&dev_attr_fod_press_status.attr,\n\tNULL,\n};\n",
     "\t&dev_attr_fod_press_status.attr,\n"
     "\t&dev_attr_gesture_single_tap_state.attr,\n"
     "\t&dev_attr_palm_sensor_data.attr,\n"
     "\tNULL,\n};\n"),
    # Bounds the 12 Pro's code does not check (the HAL's ioctl and sysfs writers reach them): a
    # SET_LONG_VALUE length below 0 or above 253 copied past buf / thp_cmd_buf, and more than 256
    # numbers written to touch_thp_cmd ran past its stack array.
    ("xiaomi/xiaomi_touch.c",
     "\t\tif (touch_data->setModeLongValue && buf[2] <= MAX_BUF_SIZE)\n",
     "\t\tif (touch_data->setModeLongValue && buf[2] >= 0 && buf[2] <= MAX_BUF_SIZE - 3)\t/* k618 */\n"),
    ("xiaomi/xiaomi_touch.c",
     "\t} else if (user_cmd == SET_LONG_VALUE) {\n",
     "\t} else if (user_cmd == SET_LONG_VALUE && buf[2] >= 0 && buf[2] <= MAX_BUF_SIZE - 4) {\t/* k618 */\n"),
    ("xiaomi/xiaomi_touch.c",
     "\t\t\tif (new_data) {\n\t\t\t\ti++;\n\t\t\t\tnew_data = false;\n\t\t\t}\n",
     "\t\t\tif (new_data) {\n\t\t\t\ti++;\n\t\t\t\tnew_data = false;\n"
     "\t\t\t\tif (i >= MAX_BUF_SIZE)\t/* k618 */\n\t\t\t\t\tbreak;\n\t\t\t}\n"),

    # ------------------------------------------------------------------ 6. flash safety
    # Paths in the 12 Pro's tree that can still write the IC's flash, all reachable from userspace
    # as root: the fts "fwupdate" sysfs store and /proc/fts/driver_test (flashProcedure,
    # flash_unlock / flash_erase_*), the self-test's panel init (production_test_initialization:
    # SPECIAL_PANEL_INIT + SYS_CMD_SAVE_FLASH of the CX data), hover auto-tune (SAVE_FLASH) and the
    # lockdown store (writeLockDownInfo). k618_touch_fix.py only cut fts_fw_update(); these refuse
    # at their entry instead. Reading (lockdown, self-test frames) is untouched.
    ("fts_spi/fts_lib/ftsFlash.c",
     "int flashProcedure(const char *path, int force, int keep_cx)\n{\n\tFirmware fw;\n\tint res;\n\n",
     "int flashProcedure(const char *path, int force, int keep_cx)\n{\n\tFirmware fw;\n\tint res;\n\n"
     "\t/* k618: the IC's flash is not this kernel's to write */\n"
     "\tlogError(1, \"%s k618: flash write refused (%s)\\n\", tag, path ? path : \"-\");\n"
     "\treturn ERROR_OP_NOT_ALLOW;\n"),
    ("fts_spi/fts_lib/ftsFlash.c",
     "int flash_unlock(void)\n{\n",
     "int flash_unlock(void)\n{\n"
     "\tlogError(1, \"%s k618: flash unlock refused\\n\", tag);\n"
     "\treturn ERROR_OP_NOT_ALLOW;\n"
     "}\n\n"
     "static int __maybe_unused k618_flash_unlock(void)\n{\n"),
    ("fts_spi/fts_lib/ftsFlash.c",
     "int flash_erase_unlock(void)\n{\n",
     "int flash_erase_unlock(void)\n{\n"
     "\tlogError(1, \"%s k618: flash erase unlock refused\\n\", tag);\n"
     "\treturn ERROR_OP_NOT_ALLOW;\n"
     "}\n\n"
     "static int __maybe_unused k618_flash_erase_unlock(void)\n{\n"),
    ("fts_spi/fts_lib/ftsFlash.c",
     "int flash_erase_page_by_page(ErasePage keep_cx, Firmware *fw)\n{\n",
     "int flash_erase_page_by_page(ErasePage keep_cx, Firmware *fw)\n{\n"
     "\tlogError(1, \"%s k618: flash erase refused\\n\", tag);\n"
     "\treturn ERROR_OP_NOT_ALLOW;\n"
     "}\n\n"
     "static int __maybe_unused k618_flash_erase_page_by_page(ErasePage keep_cx, Firmware *fw)\n{\n"),
    ("fts_spi/fts_lib/ftsTest.c",
     "int production_test_initialization(u8 type)\n{\n\tint res;\n",
     "int production_test_initialization(u8 type)\n{\n\tint res;\n\n"
     "\t/* k618: panel init writes the CX calibration to flash -- refused */\n"
     "\tlogError(1, \"%s k618: panel init %d refused\\n\", tag, type);\n"
     "\treturn ERROR_OP_NOT_ALLOW;\n"),
    ("fts_spi/fts_lib/ftsCore.c",
     "int writeSysCmd(u8 sys_cmd, u8 *sett, int size)\n{\n\tu8 *cmd = NULL;\n\tint ret;\n\n",
     "int writeSysCmd(u8 sys_cmd, u8 *sett, int size)\n{\n\tu8 *cmd = NULL;\n\tint ret;\n\n"
     "\t/* k618: no save-to-flash, no panel init (both write the IC's flash) */\n"
     "\tif (sys_cmd == SYS_CMD_SAVE_FLASH || (sys_cmd == SYS_CMD_SPECIAL && size > 0 && sett &&\n"
     "\t    (sett[0] == SPECIAL_PANEL_INIT || sett[0] == SPECIAL_FULL_PANEL_INIT))) {\n"
     "\t\tlogError(1, \"%s k618: system command %02X refused\\n\", tag, sys_cmd);\n"
     "\t\treturn ERROR_OP_NOT_ALLOW;\n"
     "\t}\n"),
    ("fts_spi/fts_lib/ftsCore.c",
     "int writeLockDownInfo(u8 *data, int size, u8 lock_id)\n{\n",
     "int writeLockDownInfo(u8 *data, int size, u8 lock_id)\n{\n"
     "\tlogError(1, \"%s k618: lockdown write refused\\n\", tag);\n"
     "\treturn ERROR_OP_NOT_ALLOW;\n"
     "}\n\n"
     "static int __maybe_unused k618_writeLockDownInfo(u8 *data, int size, u8 lock_id)\n{\n"),


    ("fts_spi/fts.c",
     "\t\t\t\t\tinfo->fod_pressed = true;\n"
     "\t\t\t\t\tinput_report_key(info->input_dev, BTN_INFO, 1);\n"
     "\t\t\t\t\tinput_sync(info->input_dev);\n",
     "\t\t\t\t\tinfo->fod_pressed = true;\n"
     "\t\t\t\t\tinput_report_key(info->input_dev, BTN_INFO, 1);\n"
     "\t\t\t\t\tinput_sync(info->input_dev);\n"
     "\t\t\t\t\tupdate_fod_press_status(1);\t/* k618: stock -- what mfp-daemon polls */\n"),
    ("fts_spi/fts.c",
     "\tinput_sync(info->input_dev);\n"
     "\tinput_report_key(info->input_dev, BTN_INFO, 0);\n"
     "\tinput_sync(info->input_dev);\n"
     "#ifdef CONFIG_FTS_BOOST\n",
     "\tinput_sync(info->input_dev);\n"
     "\tinput_report_key(info->input_dev, BTN_INFO, 0);\n"
     "\tupdate_fod_press_status(0);\t/* k618: stock */\n"
     "\tinput_sync(info->input_dev);\n"
     "#ifdef CONFIG_FTS_BOOST\n"),


    ("fts_spi/fts.c",
     "static void fts_resume_work(struct work_struct *work)\n{\n\tstruct fts_ts_info *info;\n",
     "static void fts_resume_work(struct work_struct *work)\n{\n\tstruct fts_ts_info *info;\n"
     "\tbool k618_did_reset = false;\t/* k618 */\n"),
    ("fts_spi/fts.c",
     "\tfts_system_reset();\n\trelease_all_touches(info);\n"
     "#ifndef CONFIG_FACTORY_BUILD\n#ifdef FTS_FOD_AREA_REPORT\n\t}\n#endif\n#endif\n"
     "\tfts_mode_handler(info, 0);\n\tk618_fts_thp_off();\n",
     "\tfts_system_reset();\n\trelease_all_touches(info);\n\tk618_did_reset = true;\n"
     "#ifndef CONFIG_FACTORY_BUILD\n#ifdef FTS_FOD_AREA_REPORT\n\t}\n#endif\n#endif\n"
     "\tfts_mode_handler(info, 0);\n"
     "\tif (k618_did_reset)\t/* k618: THP comes back only with a reset */\n"
     "\t\tk618_fts_thp_off();\n"),
]


def main(argv):
    root = argv[1]
    done = 0
    for rel, old, new in EDITS:
        path = os.path.join(root, rel)
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        if new in text and (old not in text or old in new):
            continue
        if old not in text:
            print(f"k618_touch_gesture: {rel}: text to change not found:\n{old[:200]}", file=sys.stderr)
            return 1
        if text.count(old) != 1:
            print(f"k618_touch_gesture: {rel}: text to change found {text.count(old)} times", file=sys.stderr)
            return 1
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text.replace(old, new))
        done += 1
    print(f"k618_touch_gesture: {done} of {len(EDITS)} edits applied")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
