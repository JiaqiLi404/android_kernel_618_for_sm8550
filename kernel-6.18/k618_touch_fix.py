#!/usr/bin/env python3
# k618_touch_fix.py <dir>: the changes k618_touch.sh makes to the 12 Pro's touch drivers (MiCode
# zeus-s-oss drivers/input/touchscreen/{fts_spi,xiaomi}) before building them for this phone on
# 6.18. Each entry is (file, old text, new text); one whose new text is already there is skipped.
import os
import sys

EDITS = [
    # Never write the touch IC. The 12 Pro's driver updates the IC firmware at probe when the file's
    # version differs; the file it would take is this phone's own st_fts_m2.ftb, but the phone's IC
    # runs Xiaomi's THP firmware and the 12 Pro's driver knows nothing of it -- and a test kernel
    # writes no flash it does not have to. Firmware stays as the phone's own kernel leaves it.
    ("fts_spi/fts.h",
     "#define FW_UPDATE_ON_PROBE\n",
     "/* k618: no firmware update at probe -- this IC's firmware is the phone's own kernel's business */\n"
     "/*#define FW_UPDATE_ON_PROBE*/\n"),


    ("fts_spi/fts.c",
     "\tlogError(1, \"%s Fw Auto Update is starting... \\n\", tag);\n",
     "\tlogError(1, \"%s Fw Auto Update is starting... \\n\", tag);\n"
     "\t/* k618: no flash, no CX recovery, no panel init -- the IC's flash is not ours to write */\n"
     "\tlogError(1, \"%s k618: firmware update and panel init skipped\\n\", tag);\n"
     "\tfts_system_reset();\n"
     "\terror = fts_init_sensing(info);\n"
     "\tif (error < OK)\n"
     "\t\tlogError(1, \"%s Cannot initialize the hardware device ERROR %08X\\n\", tag, error);\n"
     "\treturn error;\n"),
    # --- 5.10 -> 6.18 interface drift ---
    # <stdarg.h> is the kernel's own since 5.15
] + [(f"fts_spi/fts_lib/{f}", "#include <stdarg.h>\n", "#include <linux/stdarg.h>\n")
     for f in ("ftsCompensation.c", "ftsTime.c", "ftsFrame.c", "ftsTool.c", "ftsTest.c", "ftsIO.c",
               "ftsFlash.c")] + [
    # no_llseek is gone (6.12); a misc device with no seek is what noop_llseek is for
    ("xiaomi/xiaomi_touch.c", "\t.llseek\t= no_llseek,\n", "\t.llseek\t= noop_llseek,\n"),
    # definitions with no prototype (-Wstrict-prototypes is an error now); the header has (void)
    ("xiaomi/xiaomi_touch.c", "struct class *get_xiaomi_touch_class()\n",
     "struct class *get_xiaomi_touch_class(void)\n"),
    ("xiaomi/xiaomi_touch.c", "struct device *get_xiaomi_touch_dev()\n",
     "struct device *get_xiaomi_touch_dev(void)\n"),
    ("xiaomi/xiaomi_touch.c", "int update_touch_rawdata()\n", "int update_touch_rawdata(void)\n"),
    ("xiaomi/xiaomi_touch.c", "kernel base:%lld, phy base:%lld\\n", "kernel base:%lu, phy base:%lu\\n"),
    # class_create() lost its owner argument (6.4)
    ("xiaomi/xiaomi_touch.c", "class_create(THIS_MODULE, \"touch\")", "class_create(\"touch\")"),
    # platform_driver.remove returns void (6.11)
    ("xiaomi/xiaomi_touch.c", "static int xiaomi_touch_remove(struct platform_device *pdev)",
     "static void xiaomi_touch_remove(struct platform_device *pdev)"),
    ("xiaomi/xiaomi_touch.c",
     "\n\treturn 0;\n}\n\nstatic struct platform_driver xiaomi_touch_device_driver",
     "\n}\n\nstatic struct platform_driver xiaomi_touch_device_driver"),
    # strlcpy is gone (6.8); strscpy copies the same bytes (only its return value differs, unused)
    ("fts_spi/fts_lib/ftsTool.c", "strlcpy(", "strscpy("),
    ("fts_spi/fts_lib/ftsTest.c", "strlcpy(", "strscpy("),
    ("fts_spi/fts_lib/ftsIO.c", "struct device *getDev()\n", "struct device *getDev(void)\n"),
    ("fts_spi/fts_lib/ftsIO.c", "struct spi_device *getClient()\n",
     "struct spi_device *getClient(void)\n"),
    # spi_transfer.delay_usecs became .delay (value + unit, 5.11)
] + [("fts_spi/fts_lib/ftsIO.c", f"transfer[{i}].delay_usecs = SPI_DELAY_CS;",
      f"transfer[{i}].delay.value = SPI_DELAY_CS;\n\ttransfer[{i}].delay.unit = SPI_DELAY_UNIT_USECS;")
     for i in range(3)] + [
    # spi_device.master is spi_device.controller (6.x)
    ("fts_spi/fts_lib/ftsIO.c", "((struct spi_device *)client)->master->flags",
     "((struct spi_device *)client)->controller->flags"),
    # vmalloc() came in through another header on 5.10
    ("fts_spi/fts.c", "#include <linux/slab.h>\n", "#include <linux/slab.h>\n#include <linux/vmalloc.h>\n"),
    # "%100s" into a char[100] leaves no room for the NUL (the compiler's fortify check is an error)
    ("fts_spi/fts.c", "sscanf(buf, \"%100s %d %d\", path, &mode[0], &mode[1]);",
     "sscanf(buf, \"%99s %d %d\", path, &mode[0], &mode[1]);"),
    ("fts_spi/fts_proc.c", "sscanf(p, \"%100s\", path);", "sscanf(p, \"%99s\", path);"),
    # 13 bytes allocated for "{ XXXXXXXX }\n" and its NUL (14), and copy_to_user unchecked
    ("fts_spi/fts_proc.c",
     "\t\t\tdriver_test_buff = (u8 *) kmalloc(13 * sizeof(u8), GFP_KERNEL);\n"
     "\t\t\tcnt = snprintf(driver_test_buff, PAGE_SIZE, \"{ %08X }\\n\", ERROR_OP_NOT_ALLOW);\n"
     "\t\t\tcopy_to_user(buf, driver_test_buff, cnt);\n",
     "\t\t\tdriver_test_buff = (u8 *) kmalloc(32, GFP_KERNEL);\n"
     "\t\t\tif (!driver_test_buff)\n"
     "\t\t\t\treturn -ENOMEM;\n"
     "\t\t\tcnt = snprintf(driver_test_buff, 32, \"{ %08X }\\n\", ERROR_OP_NOT_ALLOW);\n"
     "\t\t\tif (copy_to_user(buf, driver_test_buff, cnt))\n"
     "\t\t\t\tpr_err(\"fts: copy_to_user failed\\n\");\n"),
    ("fts_spi/fts.c", "\tstatic expert_mode = false;\n", "\tstatic bool expert_mode = false;\n"),
    ("fts_spi/fts.c", "#include <linux/of_gpio.h>\n", "#include <linux/of_gpio.h>\n#include <linux/pinctrl/consumer.h>\n"),
    # of_get_named_gpio_flags() is gone (6.x); the flags were never asked for
    ("fts_spi/fts.c", "of_get_named_gpio_flags(np, \"fts,irq-gpio\", 0, NULL);",
     "of_get_named_gpio(np, \"fts,irq-gpio\", 0);"),
    ("fts_spi/fts.c",
     "of_get_named_gpio_flags(np,\n\t\t\t\t\t\t\t    \"fts,reset-gpio\", 0,\n\t\t\t\t\t\t\t    NULL);",
     "of_get_named_gpio(np, \"fts,reset-gpio\", 0);"),
    # spi_driver.remove returns void (6.x); the I2C variant beside it is not built
    ("fts_spi/fts.c", "static int fts_remove(struct spi_device *client)\n{\n#endif",
     "static void fts_remove(struct spi_device *client)\n{\n#endif"),
    ("fts_spi/fts.c", "\t/* free all */\n\tkfree(info);\n\n\treturn OK;\n}", "\t/* free all */\n\tkfree(info);\n}"),
    # --- this phone's IC ---
    # It runs Xiaomi's THP firmware (fts,support-thp): in THP mode it hands raw frames to a
    # userspace algorithm instead of reporting touches, and the 12 Pro's driver has no THP. The
    # phone's own driver turns THP off with three commands (its fts_enable_thp_onoff(0),
    # disassembled): scan off, custom command 0x21 = 1, scan on. They set the IC's running state
    # only -- nothing goes to its flash. Sent after the IC is set up at probe and after the reset
    # every resume does; a failure is logged, never a reason to fail the probe.
    ("fts_spi/fts.c",
     "static int fts_init_sensing(struct fts_ts_info *info)\n{\n\tint error = 0;\n",
     "/* k618: THP off -- the IC reports touches itself (see k618_touch_fix.py) */\n"
     "static void k618_fts_thp_off(void)\n"
     "{\n"
     "\tu8 scan_off[3] = { FTS_CMD_SCAN_MODE, SCAN_MODE_ACTIVE, 0x00 };\n"
     "\tu8 thp_off[3] = { FTS_CMD_CUSTOM, 0x21, 0x01 };\n"
     "\tu8 scan_on[3] = { FTS_CMD_SCAN_MODE, SCAN_MODE_ACTIVE, 0x01 };\n"
     "\tint res = 0;\n"
     "\n"
     "\tres |= fts_write_dma_safe(scan_off, 3);\n"
     "\tmdelay(WAIT_AFTER_SENSEOFF);\n"
     "\tres |= fts_write_dma_safe(thp_off, 3);\n"
     "\tres |= fts_write_dma_safe(scan_on, 3);\n"
     "\tlogError(1, \"%s k618: THP off sent (%08X)\\n\", tag, res);\n"
     "}\n"
     "\n"
     "static int fts_init_sensing(struct fts_ts_info *info)\n{\n\tint error = 0;\n"),
    ("fts_spi/fts.c",
     "\terror |= fts_mode_handler(info, 0);\n#ifdef FTS_FOD_AREA_REPORT\n"
     "\terror |= setScanMode(SCAN_MODE_ACTIVE, 0x00);\n",
     "\terror |= fts_mode_handler(info, 0);\n\tk618_fts_thp_off();\n#ifdef FTS_FOD_AREA_REPORT\n"
     "\terror |= setScanMode(SCAN_MODE_ACTIVE, 0x00);\n"),
    ("fts_spi/fts.c",
     "#endif\n#endif\n\tfts_mode_handler(info, 0);\n#ifdef CONFIG_FTS_POWERSUPPLY_CB\n",
     "#endif\n#endif\n\tfts_mode_handler(info, 0);\n\tk618_fts_thp_off();\n#ifdef CONFIG_FTS_POWERSUPPLY_CB\n"),


    ("xiaomi/xiaomi_touch.h",
     "\tint prox_value;\n\tbool prox_changed;\n\tconst char *name;\n",
     "\tint prox_value;\n\tbool prox_changed;\n"
     "\tint fod_pressed;\t\t/* k618: what fod_press_status reads */\n"
     "\tconst char *name;\n"),
    ("xiaomi/xiaomi_touch.h",
     "extern int update_prox_sensor_value(int value);\n",
     "extern int update_prox_sensor_value(int value);\n\n"
     "/* k618: fts reports the fingerprint press here; mfp-daemon polls fod_press_status */\n"
     "extern int update_fod_press_status(int value);\n"),
    ("xiaomi/xiaomi_touch.c",
     "EXPORT_SYMBOL_GPL(update_palm_sensor_value);\n",
     "EXPORT_SYMBOL_GPL(update_palm_sensor_value);\n"
     "\n"
     "/* k618: the fingerprint press, for mfp-daemon's poll on fod_press_status */\n"
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
    ("xiaomi/xiaomi_touch.c",
     "static DEVICE_ATTR(suspend_state, 0644, xiaomi_touch_suspend_state, NULL);\n",
     "/* k618: '1' while a finger is on the sensor area, '0' after (update_fod_press_status()) */\n"
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
    ("xiaomi/xiaomi_touch.c",
     "\t&dev_attr_suspend_state.attr,\n\tNULL,\n};\n",
     "\t&dev_attr_suspend_state.attr,\n\t&dev_attr_fod_press_status.attr,\n\tNULL,\n};\n"),
    # press: reported whether or not board->support_fod skips the BTN_INFO key
    ("fts_spi/fts.c",
     "\t\t\t\tif (!info->board->support_fod) {\n"
     "\t\t\t\t\tinput_report_key(info->input_dev, BTN_INFO, 1);\n"
     "\t\t\t\t}\n"
     "\t\t\t\tlogError(1,\t\"%s  %s :  FOD Press :%d, fod_id:%08x\\n\", tag, __func__, touchId, info->fod_id);\n",
     "\t\t\t\tif (!info->board->support_fod) {\n"
     "\t\t\t\t\tinput_report_key(info->input_dev, BTN_INFO, 1);\n"
     "\t\t\t\t}\n"
     "\t\t\t\tupdate_fod_press_status(1);\t/* k618: wakes mfp-daemon's poll */\n"
     "\t\t\t\tlogError(1,\t\"%s  %s :  FOD Press :%d, fod_id:%08x\\n\", tag, __func__, touchId, info->fod_id);\n"),
    # release, in the enter/motion handler and in the leave handler
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


def main(argv):
    root = argv[1]
    done = 0
    for rel, old, new in EDITS:
        path = os.path.join(root, rel)
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        if new in text and (old not in text or old in new):
            continue
        if old not in text:
            print(f"k618_touch_fix: {rel}: text to change not found", file=sys.stderr)
            return 1
        # every occurrence: an interface rename applies wherever the old name is used
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text.replace(old, new))
        done += 1
    print(f"k618_touch_fix: {done} of {len(EDITS)} edits applied")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
