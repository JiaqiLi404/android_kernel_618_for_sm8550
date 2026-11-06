#!/usr/bin/env python3
# k618_touch_m3.py <dir>: the 13's (fuxi) touch rules on the fingerprint path, after
# k618_touch_fix.py, k618_touch_gesture.py, drafts/k618_touch_thp.py and k618_touch_devices.py.
#
# The three phones' stock touch modules are three builds of one Xiaomi source tree,
# vendor/xiaomi/proprietary/touch/touchfeature_v2/touch_driver/<variant>: the 13 Pro's
# fts_touch_spi.ko / xiaomi_touch.ko are m2, the 13 Ultra's m1 (same functions as m2) and the
# 13's m3 (fuxi OS3.0.309.0 vendor_dlkm). The earlier scripts follow m2. What the 13's m3 build
# does differently where the fingerprint press is concerned (m3 .text addresses):
#   - mode 41 from the touchfeature HAL (fts_set_cur_value 0x22958; m2: "don't support") keeps a
#     fingerprint auth state; release_all_touches (0x1a1a8) leaves fod_press_status alone while it
#     is set ("means wrong fingerprint, skip") instead of always reporting the release.
#   - panel BLANK / BLANK_LP while already asleep turns fod_status 3 into 1 (0x20708), and the
#     suspend is queued with sleep_finger cleared (0x20840).
#   - an interrupt while the system is suspended goes to the sleep work with the interrupt
#     disabled (fts_event_handler 0x1daec, fts_disableInterruptNoSync); fts_switch_mode_work
#     waits for the system resume (0x22288) before it sends the gesture mask, and fts_pm_resume
#     wakes every waiter (complete_all, 0x2a784).
# And what m3 shares with m2 / m1 but our driver did not do (also only on the 13 here, the other
# two phones stay as they are):
#   - the finger-down on the awake path is reported by the IC's long-press gesture; the enter
#     handler reports it itself only without fts,support-fod (0x1c520), and all three nodes have
#     it. k618_touch_fix.py reported it at the first touch in the area.
#   - the long-press gesture is ignored with nonui 2, the phone in a pocket (0x1d648).
#   - suspend and resume works wait for the system resume first (0x20d5c, 0x20ae0) and resume
#     waits 12 ms after the mode commands (0x20b90).
# For all three phones, as all three stock builds do: the leave handler reports the
# release when the last touch goes up (m3 0x1c888, m2 / m1 alike; fts_touch_spi.k618_fod_up=0
# leaves it out). After a screen-off press the IC sends the FOD release (0xb5) and this is the
# only place it reaches fod_press_status; without it the state stayed 1 after such a press, the
# next press changed nothing, so mfp-daemon's poll was not woken -- which would explain a first
# screen-off attempt that fails and a second that works on the 13 Pro; not verified on the phone yet.
# And (m3 probe 0x20238..0x202e4, resume
# 0x20cc4..0x20cd0; m2 0x20e24..0x20ed0, 0x21804): the THP-off command (C0 21 01 between scan off
# and on, k618_fts_thp_off) goes only to an IC whose node has fts,support-thp-fw. The 13's has
# not (firmware st_fts_m3, no THP), so it no longer gets a command its firmware was never sent;
# the 13 Pro and 13 Ultra have it, nothing changes there.
#
# Which phone: the touch node's fts,default-fw-name. The stock variant directories and the
# firmware names go together (m1 st_fts_m1.ftb on the 13 Ultra, m2 st_fts_m2.ftb on the 13 Pro,
# m3 st_fts_m3.ftb on the 13), so the name says which build's rules the IC was made for;
# fts,support-thp-fw only says whether its firmware has THP (and decides the THP-off command).
# fts_touch_spi.k618_m3 = -1 (default: from the firmware name), 0 (never), 1 (always), read
# where it is used, so it can be changed at run time. Nothing here writes the IC's flash; the
# only command difference is the THP-off one no longer sent to the 13.
import os
import sys

EDITS = [
    # ------------------------------------------------------------------ switches and state
    ("fts_spi/fts.c",
     "static bool k618_stock = true;\nmodule_param(k618_stock, bool, 0644);\n",
     "static bool k618_stock = true;\nmodule_param(k618_stock, bool, 0644);\n"
     "/*\n"
     " * k618 m3: 1 = the 13's stock rules on the fingerprint path (stock touch_driver/m3, see\n"
     " * k618_touch_m3.py), 0 = the 13 Pro's (m2); -1 = from the node's firmware name, st_fts_m3*.\n"
     " */\n"
     "static int k618_m3 = -1;\n"
     "module_param(k618_m3, int, 0644);\n"
     "/*\n"
     " * k618: the last touch up reports the fingerprint release (fod_press_status 0), as all three\n"
     " * stock builds do (m3 0x1c888, m2 / m1 alike); 0 = no release is reported there\n"
     " */\n"
     "static bool k618_fod_up = true;\n"
     "module_param(k618_fod_up, bool, 0644);\n"
     "static bool k618_dt_m3;\t\t/* fts,default-fw-name is st_fts_m3* (parse_dt) */\n"
     "static bool k618_dt_thp_fw;\t/* fts,support-thp-fw: the IC firmware has THP (parse_dt) */\n"
     "static int k618_fp_authstate;\t/* m3 mode 41: nonzero while the last fingerprint did not match */\n"
     "#define K618_M3_FP_AUTHSTATE\t41\n"
     "static bool k618_m3_on(void)\n"
     "{\n"
     "\treturn k618_m3 < 0 ? k618_dt_m3 : k618_m3 != 0;\n"
     "}\n"),
    # parse_dt: default-fw-name is read before support-fod
    ("fts_spi/fts.c",
     "\tbdata->support_fod =\n\t    of_property_read_bool(np, \"fts,support-fod\");\n",
     "\tbdata->support_fod =\n\t    of_property_read_bool(np, \"fts,support-fod\");\n"
     "\tk618_dt_thp_fw = of_property_read_bool(np, \"fts,support-thp-fw\");\t/* k618 m3 */\n"
     "\tk618_dt_m3 = bdata->default_fw_name &&\n"
     "\t\t     !strncmp(bdata->default_fw_name, \"st_fts_m3\", 9) &&\n"
     "\t\t     (bdata->default_fw_name[9] < '0' || bdata->default_fw_name[9] > '9');\n"
     "\tlogError(1, \"%s k618: firmware %s: %s fingerprint rules, THP-off command %s\\n\", tag,\n"
     "\t\t bdata->default_fw_name ? bdata->default_fw_name : \"-\", k618_dt_m3 ? \"m3\" : \"m2\",\n"
     "\t\t k618_dt_thp_fw ? \"as stock\" : \"never (no fts,support-thp-fw)\");\n"),

    # ------------------------------------------------------------------ THP off: THP firmware only
    ("fts_spi/fts.c",
     "\tu8 scan_on[3] = { FTS_CMD_SCAN_MODE, SCAN_MODE_ACTIVE, 0x01 };\n\tint res = 0;\n\n",
     "\tu8 scan_on[3] = { FTS_CMD_SCAN_MODE, SCAN_MODE_ACTIVE, 0x01 };\n\tint res = 0;\n\n"
     "\tif (!k618_dt_thp_fw)\n"
     "\t\treturn;\t/* k618 m3: stock sends it only to THP firmware (fts,support-thp-fw) */\n"),

    # ------------------------------------------------------------------ mode 41 and release_all_touches
    ("fts_spi/fts.c",
     "static int fts_set_cur_value(int mode, int value)\n{\n"
     "\tlogError(1, \"%s %s, mode:%d,value:%d\\n\", tag, __func__, mode, value);\n",
     "static int fts_set_cur_value(int mode, int value)\n{\n"
     "\tlogError(1, \"%s %s, mode:%d,value:%d\\n\", tag, __func__, mode, value);\n"
     "\t/* k618 m3: stock m3 keeps the fingerprint auth state for release_all_touches() */\n"
     "\tif (mode == K618_M3_FP_AUTHSTATE && fts_info && value >= 0 && k618_m3_on()) {\n"
     "\t\tk618_fp_authstate = value != 0;\n"
     "\t\treturn 0;\n"
     "\t}\n"),
    ("fts_spi/fts.c",
     "\tinput_report_key(info->input_dev, BTN_INFO, 0);\n"
     "\tupdate_fod_press_status(0);\t/* k618: stock */\n"
     "\tinput_sync(info->input_dev);\n"
     "#ifdef CONFIG_FTS_BOOST\n",
     "\tinput_report_key(info->input_dev, BTN_INFO, 0);\n"
     "\tif (!k618_m3_on()) {\n"
     "\t\tupdate_fod_press_status(0);\t/* k618: stock */\n"
     "\t} else if (k618_fp_authstate) {\n"
     "\t\t/* k618 m3: stock m3 reports no release while the last match failed (mode 41) */\n"
     "\t\tlogError(1, \"%s  %s fingerprint_authstate: %d means wrong fingerprint, skip\\n\",\n"
     "\t\t\t tag, __func__, k618_fp_authstate);\n"
     "\t} else {\n"
     "\t\tupdate_fod_press_status(0);\n"
     "\t\tlogError(1, \"%s  %s fingerprint_authstate: %d update_fod_press_status\\n\",\n"
     "\t\t\t tag, __func__, k618_fp_authstate);\n"
     "\t}\n"
     "\tinput_sync(info->input_dev);\n"
     "#ifdef CONFIG_FTS_BOOST\n"),

    # ------------------------------------------------------------------ press / release reporting
    # awake: with fts,support-fod the long-press gesture reports the press (stock m1/m2/m3)
    ("fts_spi/fts.c",
     "\t\t\t\tif (!info->board->support_fod) {\n"
     "\t\t\t\t\tinput_report_key(info->input_dev, BTN_INFO, 1);\n"
     "\t\t\t\t}\n"
     "\t\t\t\tupdate_fod_press_status(1);\t/* k618: wakes mfp-daemon's poll */\n",
     "\t\t\t\tif (!info->board->support_fod) {\n"
     "\t\t\t\t\tinput_report_key(info->input_dev, BTN_INFO, 1);\n"
     "\t\t\t\t}\n"
     "\t\t\t\t/* k618 m3: stock leaves the press to the long-press gesture with support-fod */\n"
     "\t\t\t\tif (!k618_m3_on() || !info->board->support_fod)\n"
     "\t\t\t\t\tupdate_fod_press_status(1);\t/* k618: wakes mfp-daemon's poll */\n"),
    # the last touch up: where the FOD release (0xb5) after a screen-off press ends
    ("fts_spi/fts.c",
     "\t\tinfo->fod_pressed = false;\n"
     "\t\tinput_report_key(info->input_dev, BTN_INFO, 0);\n"
     "\n"
     "#ifdef FTS_XIAOMI_TOUCHFEATURE\n"
     "\t\twake_up(&info->wait_queue);\n"
     "#endif\n"
     "\t\tinfo->touch_skip = 0;\n",
     "\t\tinfo->fod_pressed = false;\n"
     "\t\tinput_report_key(info->input_dev, BTN_INFO, 0);\n"
     "\t\tif (k618_fod_up)\n"
     "\t\t\tupdate_fod_press_status(0);\t/* k618: stock m1/m2/m3, the release of a screen-off press */\n"
     "\n"
     "#ifdef FTS_XIAOMI_TOUCHFEATURE\n"
     "\t\twake_up(&info->wait_queue);\n"
     "#endif\n"
     "\t\tinfo->touch_skip = 0;\n"),
    # long press in a pocket (nonui 2): stock m3 takes it for no gesture
    ("fts_spi/fts.c",
     "\t\tif (event[2] == GEST_ID_LONG_PRESS) {\n"
     "\t\t\tif (!fts_fingerprint_is_enable()) {\n",
     "\t\tif (event[2] == GEST_ID_LONG_PRESS) {\n"
     "\t\t\tif (k618_m3_on() && info->nonui_status > 1) {\t/* k618 m3: stock */\n"
     "\t\t\t\tlogError(0, \"%s %s:  No valid GestureID! \\n\", tag, __func__);\n"
     "\t\t\t\tgoto gesture_done;\n"
     "\t\t\t}\n"
     "\t\t\tif (!fts_fingerprint_is_enable()) {\n"),

    # ------------------------------------------------------------------ screen off / on
    ("fts_spi/fts.c",
     "\t\tflush_workqueue(info->event_wq);\n"
     "\t\tif (info->sensor_sleep)\n"
     "\t\t\treturn;\n"
     "\t\tqueue_work(info->event_wq, &info->suspend_work);\n",
     "\t\tflush_workqueue(info->event_wq);\n"
     "\t\tif (info->sensor_sleep) {\n"
     "\t\t\tif (k618_m3_on() && info->fod_status == 3)\n"
     "\t\t\t\tinfo->fod_status = 1;\t/* k618 m3: stock, e.g. BLANK_LP after BLANK */\n"
     "\t\t\treturn;\n"
     "\t\t}\n"
     "\t\tif (k618_m3_on())\n"
     "\t\t\tinfo->sleep_finger = 0;\t/* k618 m3: stock */\n"
     "\t\tqueue_work(info->event_wq, &info->suspend_work);\n"),
    # the works wait for the system resume as stock does
    ("fts_spi/fts.c",
     "static void fts_ts_sleep_work(struct work_struct *work)\n{\n",
     "/*\n"
     " * k618 m3: the 13's stock suspend, resume and gesture-switch works wait for the system resume\n"
     " * (fts_pm_resume) before they use the bus, and give up after 500 ms as stock does.\n"
     " */\n"
     "static bool k618_m3_pm_wait_failed(struct fts_ts_info *info)\n"
     "{\n"
     "\tif (!info->tp_pm_suspend)\n"
     "\t\treturn false;\n"
     "\tpm_wakeup_event(info->dev, 0);\n"
     "\tif (!wait_for_completion_timeout(&info->pm_resume_completion, msecs_to_jiffies(500))) {\n"
     "\t\tlogError(1, \"%s pm_resume_completion timeout, i2c is closed\", tag);\n"
     "\t\treturn true;\n"
     "\t}\n"
     "\tlogError(1, \"%s pm_resume_completion be completed\", tag);\n"
     "\treturn false;\n"
     "}\n"
     "\n"
     "static void fts_ts_sleep_work(struct work_struct *work)\n{\n"),
    ("fts_spi/fts.c",
     "\tinfo = container_of(work, struct fts_ts_info, resume_work);\n",
     "\tinfo = container_of(work, struct fts_ts_info, resume_work);\n"
     "\tif (k618_m3_on() && k618_m3_pm_wait_failed(info))\n"
     "\t\treturn;\t/* k618 m3 */\n"),
    ("fts_spi/fts.c",
     "\tfts_mode_handler(info, 0);\n"
     "\tif (k618_did_reset)\t/* k618: THP comes back only with a reset */\n"
     "\t\tk618_fts_thp_off();\n",
     "\tfts_mode_handler(info, 0);\n"
     "\tif (k618_did_reset)\t/* k618: THP comes back only with a reset */\n"
     "\t\tk618_fts_thp_off();\n"
     "\tif (k618_m3_on())\n"
     "\t\tmsleep(12);\t/* k618 m3: stock waits after the mode commands */\n"),
    ("fts_spi/fts.c",
     "\tinfo = container_of(work, struct fts_ts_info, suspend_work);\n",
     "\tinfo = container_of(work, struct fts_ts_info, suspend_work);\n"
     "\tif (k618_m3_on() && k618_m3_pm_wait_failed(info))\n"
     "\t\treturn;\t/* k618 m3 */\n"),
    ("fts_spi/fts.c",
     "\t\t\treturn;\n"
     "\t\t}\n"
     "\t\tpm_stay_awake(info->dev);\n"
     "\t\tif ((!gesture_type && !info->gesture_enabled) || info->nonui_status == 2) {\n",
     "\t\t\treturn;\n"
     "\t\t}\n"
     "\t\tif (k618_m3_on() && k618_m3_pm_wait_failed(info))\n"
     "\t\t\treturn;\t/* k618 m3: stock waits for the system resume here */\n"
     "\t\tpm_stay_awake(info->dev);\n"
     "\t\tif ((!gesture_type && !info->gesture_enabled) || info->nonui_status == 2) {\n"),
    ("fts_spi/fts.c",
     "\tinfo->tp_pm_suspend = false;\n\tcomplete(&info->pm_resume_completion);\n",
     "\tinfo->tp_pm_suspend = false;\n"
     "\tif (k618_m3_on())\n"
     "\t\tcomplete_all(&info->pm_resume_completion);\t/* k618 m3: stock, several works wait */\n"
     "\telse\n"
     "\t\tcomplete(&info->pm_resume_completion);\n"),
    # interrupt while the system is suspended: stock m3 masks it until the sleep work has read
    # the FIFO (the level interrupt would otherwise fire again and again until then)
    ("fts_spi/fts.c",
     "\t\t\tpm_stay_awake(info->dev);\n"
     "\t\t\tqueue_work(info->irq_wq, &info->sleep_work);\n"
     "\t\t}\n",
     "\t\t\tpm_stay_awake(info->dev);\n"
     "\t\t\tqueue_work(info->irq_wq, &info->sleep_work);\n"
     "\t\t\tif (k618_m3_on())\n"
     "\t\t\t\tfts_disableInterruptNoSync();\t/* k618 m3: the sleep work enables it again */\n"
     "\t\t}\n"),
]


def main(root):
    done = 0
    for path, old, new in EDITS:
        p = os.path.join(root, path)
        s = open(p, encoding="utf-8", errors="surrogateescape").read()
        if new in s:
            continue
        if s.count(old) != 1:
            sys.exit(f"k618_touch_m3: {path}: anchor found {s.count(old)} times: {old[:70]!r}")
        s = s.replace(old, new)
        open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
        done += 1
    print(f"k618_touch_m3: {done} of {len(EDITS)} edits applied")


if __name__ == "__main__":
    main(sys.argv[1])
