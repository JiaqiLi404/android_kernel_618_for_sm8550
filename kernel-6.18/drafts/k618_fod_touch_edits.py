#!/usr/bin/env python3
'Device support and compatibility helpers.'

EDITS = [
    # --- xiaomi_touch.h: pdata field + exported prototype ---
    ("xiaomi/xiaomi_touch.h",
     "\tint prox_value;\n"
     "\tbool prox_changed;\n"
     "\tconst char *name;\n",
     "\tint prox_value;\n"
     "\tbool prox_changed;\n"
     "\tint fod_pressed;\t\t/* k618: FOD press state for /sys/.../fod_press_status */\n"
     "\tconst char *name;\n"),
    ("xiaomi/xiaomi_touch.h",
     "extern int update_prox_sensor_value(int value);\n",
     "extern int update_prox_sensor_value(int value);\n\n"
     "/* k618: FTS reports the FOD press here; mfp-daemon polls the fod_press_status node */\n"
     "extern int update_fod_press_status(int value);\n"),

    # --- xiaomi_touch.c: the exported updater + the sysfs node ---
    # the updater goes right after update_palm_sensor_value()'s EXPORT; it stores the value and
    # sysfs_notify()s "fod_press_status" (POLLPRI), exactly what the HAL's poll(fd,-1) waits for.
    # No heavy lock: the FTS event handler runs in thread/process context and only flips an int.
    ("xiaomi/xiaomi_touch.c",
     "EXPORT_SYMBOL_GPL(update_palm_sensor_value);\n",
     "EXPORT_SYMBOL_GPL(update_palm_sensor_value);\n"
     "\n"
     "/* k618: FOD press state the fingerprint HAL (mfp-daemon) polls on fod_press_status */\n"
     "int update_fod_press_status(int value)\n"
     "{\n"
     "\tif (!touch_pdata)\n"
     "\t\treturn -ENODEV;\n"
     "\tif (value != touch_pdata->fod_pressed) {\n"
     "\t\ttouch_pdata->fod_pressed = value;\n"
     "\t\tpr_info(\"%s value:%d\\n\", __func__, value);\n"
     "\t\tsysfs_notify(&xiaomi_touch_dev.dev->kobj, NULL, \"fod_press_status\");\n"
     "\t}\n"
     "\treturn 0;\n"
     "}\n"
     "EXPORT_SYMBOL_GPL(update_fod_press_status);\n"),
    # the node itself (read returns '0'/'1'; the HAL only reads, so RO), declared next to suspend_state
    ("xiaomi/xiaomi_touch.c",
     "static DEVICE_ATTR(suspend_state, 0644, xiaomi_touch_suspend_state, NULL);\n",
     "/* k618: FOD press state; read '0'/'1', sysfs_notify()d by update_fod_press_status() */\n"
     "static ssize_t fod_press_status_show(struct device *dev,\n"
     "\t\tstruct device_attribute *attr, char *buf)\n"
     "{\n"
     "\tstruct xiaomi_touch_pdata *pdata = dev_get_drvdata(dev);\n"
     "\n"
     "\treturn snprintf(buf, PAGE_SIZE, \"%d\\n\", pdata ? pdata->fod_pressed : 0);\n"
     "}\n"
     "static DEVICE_ATTR(fod_press_status, 0644, fod_press_status_show, NULL);\n"
     "\n"
     "static DEVICE_ATTR(suspend_state, 0644, xiaomi_touch_suspend_state, NULL);\n"),
    # add it to the attribute group so it appears under touch_dev/
    ("xiaomi/xiaomi_touch.c",
     "\t&dev_attr_suspend_state.attr,\n"
     "\tNULL,\n"
     "};\n",
     "\t&dev_attr_suspend_state.attr,\n"
     "\t&dev_attr_fod_press_status.attr,\n"
     "\tNULL,\n"
     "};\n"),

    # --- fts_spi/fts.c: report the press/release to xiaomi_touch at all three FOD points ---
    # 1. FOD press (enter/motion): report after the log, unconditionally (the HAL needs it even when
    #    board->support_fod skips the BTN_INFO key report)
    ("fts_spi/fts.c",
     "\t\t\t\tif (!info->board->support_fod) {\n"
     "\t\t\t\t\tinput_report_key(info->input_dev, BTN_INFO, 1);\n"
     "\t\t\t\t}\n"
     "\t\t\t\tlogError(1,\t\"%s  %s :  FOD Press :%d, fod_id:%08x\\n\", tag, __func__, touchId, info->fod_id);\n",
     "\t\t\t\tif (!info->board->support_fod) {\n"
     "\t\t\t\t\tinput_report_key(info->input_dev, BTN_INFO, 1);\n"
     "\t\t\t\t}\n"
     "\t\t\t\tupdate_fod_press_status(1);\t/* k618: wake mfp-daemon's fod_press_status poll */\n"
     "\t\t\t\tlogError(1,\t\"%s  %s :  FOD Press :%d, fod_id:%08x\\n\", tag, __func__, touchId, info->fod_id);\n"),
    # 2. FOD release (enter/motion else-branch)
    ("fts_spi/fts.c",
     "\t\t} else if (__test_and_clear_bit(touchId, &info->fod_id)) {\n"
     "\t\t\tinput_report_abs(info->input_dev, ABS_MT_WIDTH_MINOR, 0);\n"
     "\t\t\tinput_report_key(info->input_dev, BTN_INFO, 0);\n"
     "\t\t\tinfo->fod_x = 0;\n",
     "\t\t} else if (__test_and_clear_bit(touchId, &info->fod_id)) {\n"
     "\t\t\tinput_report_abs(info->input_dev, ABS_MT_WIDTH_MINOR, 0);\n"
     "\t\t\tinput_report_key(info->input_dev, BTN_INFO, 0);\n"
     "\t\t\tupdate_fod_press_status(0);\t/* k618 */\n"
     "\t\t\tinfo->fod_x = 0;\n"),
    # 3. FOD release (leave handler, the 0xb5 path cleared here)
    ("fts_spi/fts.c",
     "\tif (__test_and_clear_bit(touchId, &info->fod_id)) {\n"
     "\t\tinput_report_abs(info->input_dev, ABS_MT_WIDTH_MINOR, 0);\n"
     "\t\tinput_report_key(info->input_dev, BTN_INFO, 0);\n"
     "\t\tinfo->fod_coordinate_update = false;\n",
     "\tif (__test_and_clear_bit(touchId, &info->fod_id)) {\n"
     "\t\tinput_report_abs(info->input_dev, ABS_MT_WIDTH_MINOR, 0);\n"
     "\t\tinput_report_key(info->input_dev, BTN_INFO, 0);\n"
     "\t\tupdate_fod_press_status(0);\t/* k618 */\n"
     "\t\tinfo->fod_coordinate_update = false;\n"),
]


def apply(text, old, new):
    """k618_touch_fix.py's rule: skip if already applied, else require exactly one match."""
    if new in text and (old not in text or old in new):
        return text, "already"
    n = text.count(old)
    if n != 1:
        return text, "found %d times" % n
    return text.replace(old, new), "applied"


def main(argv):
    import os
    import sys
    if len(argv) < 2:
        print("usage: k618_fod_touch_edits.py <touch src dir> (the dir k618_touch.sh stages to)",
              file=sys.stderr)
        return 2
    root = argv[1].rstrip("/")
    done = 0
    for rel, old, new in EDITS:
        path = os.path.join(root, rel)
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        text2, what = apply(text, old, new)
        if what.startswith("found"):
            print(f"k618_fod_touch_edits: {rel}: text to change {what}", file=sys.stderr)
            return 1
        if what == "applied":
            open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text2)
            done += 1
        print(f"k618_fod_touch_edits: {rel}: {what}")
    print(f"k618_fod_touch_edits: {done} of {len(EDITS)} edits applied")
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main(sys.argv))
