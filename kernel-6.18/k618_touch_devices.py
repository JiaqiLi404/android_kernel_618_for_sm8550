#!/usr/bin/env python3
# k618_touch_devices.py <dir>: the touch driver for the 13 and the 13 Ultra too, after
# k618_touch_fix.py, k618_touch_gesture.py and drafts/k618_touch_thp.py.
#
# The three phones' device trees (the Xiaomi SM8550 dtbo carries all of them) have the same touch
# node, "st,spi" (ST FingerTipS), with their own parameters. Two of those are more than numbers:
#   - THP: only the 13 Pro's and 13 Ultra's nodes have fts,support-thp; drafts/k618_touch_thp.py
#     follows it (k618_thp -1).
#   - The 13 Ultra's node has a second interrupt pin, fts,irq-gpio,new, and pin states
#     pmx_ts_active_new / pmx_ts_suspend_new. Its stock 5.15 fts_probe (13 Ultra
#     OS3.0.308.0.WMACNXM, fts_touch_spi.ko 0x1ed84..0x1f8b8) reads the board id with
#     get_hw_id_value() and keeps the old pin only for 0, 1, 0x10000, 0x100000, 0x100001 and
#     0x110000 -- early boards; every other board uses the new pin. The board id is what the
#     bootloader passes as hwid.hwid_value= (589824 on a 13 Pro); it is read here from /chosen
#     bootargs, so the driver needs no other module.
import os
import sys

EDITS = [
    # declared up here: fts_pinctrl_init() comes before parse_dt() in the file
    ("fts_spi/fts.c",
     "static bool k618_stock = true;\nmodule_param(k618_stock, bool, 0644);\n",
     "static bool k618_stock = true;\nmodule_param(k618_stock, bool, 0644);\n"
     "static bool k618_irq_new;\t/* k618 devices: the 13 Ultra's second interrupt pin is in use */\n"),
    ("fts_spi/fts.c",
     "static int parse_dt(struct device *dev, struct fts_hw_platform_data *bdata)\n{\n",
     "/* k618 devices: the board id (hwid.hwid_value= on the command line), see k618_touch_devices.py */\n"
     "static u32 k618_hwid_value(void)\n"
     "{\n"
     "\tstruct device_node *ch = of_find_node_by_path(\"/chosen\");\n"
     "\tconst char *args = NULL, *p;\n"
     "\tu32 v = 0;\n\n"
     "\tif (ch && !of_property_read_string(ch, \"bootargs\", &args) && args &&\n"
     "\t    (p = strstr(args, \"hwid.hwid_value=\")))\n"
     "\t\tv = simple_strtoul(p + strlen(\"hwid.hwid_value=\"), NULL, 0);\n"
     "\tof_node_put(ch);\n"
     "\treturn v;\n"
     "}\n\n"
     "/* the 13 Ultra's stock fts_probe: the old interrupt pin only on these early boards */\n"
     "static bool k618_hwid_early(u32 hw)\n"
     "{\n"
     "\tif ((int)hw >= 0x100000)\n"
     "\t\treturn hw - 0x100000 < 2 || hw == 0x110000;\n"
     "\treturn hw < 2 || hw == 0x10000;\n"
     "}\n\n"
     "static int parse_dt(struct device *dev, struct fts_hw_platform_data *bdata)\n{\n"),
    ("fts_spi/fts.c",
     "\tbdata->irq_gpio = of_get_named_gpio(np, \"fts,irq-gpio\", 0);\n",
     "\tbdata->irq_gpio = of_get_named_gpio(np, \"fts,irq-gpio\", 0);\n"
     "\tif (of_find_property(np, \"fts,irq-gpio,new\", NULL)) {\n"
     "\t\tu32 hw = k618_hwid_value();\n\n"
     "\t\t/* k618 devices: the 13 Ultra -- its second pin, as its stock driver picks it */\n"
     "\t\tk618_irq_new = !k618_hwid_early(hw);\n"
     "\t\tif (k618_irq_new)\n"
     "\t\t\tbdata->irq_gpio = of_get_named_gpio(np, \"fts,irq-gpio,new\", 0);\n"
     "\t\tlogError(1, \"%s k618: hw_id_value %x: %s interrupt pin, gpio %d\\n\", tag, hw,\n"
     "\t\t\t k618_irq_new ? \"new\" : \"old\", bdata->irq_gpio);\n"
     "\t}\n"),
    # pin states: the new pin has its own pair
    ("fts_spi/fts.c",
     "\t    = pinctrl_lookup_state(info->ts_pinctrl, PINCTRL_STATE_ACTIVE);\n",
     "\t    = pinctrl_lookup_state(info->ts_pinctrl,\n"
     "\t\t\t\t   k618_irq_new ? \"pmx_ts_active_new\" : PINCTRL_STATE_ACTIVE);\n"),
    ("fts_spi/fts.c",
     "\t    = pinctrl_lookup_state(info->ts_pinctrl, PINCTRL_STATE_SUSPEND);\n",
     "\t    = pinctrl_lookup_state(info->ts_pinctrl,\n"
     "\t\t\t\t   k618_irq_new ? \"pmx_ts_suspend_new\" : PINCTRL_STATE_SUSPEND);\n"),
    # the interrupt: the node's interrupts names the old pin, so the new one is mapped here
    ("fts_spi/fts.c",
     "\t\tparse_dt(&client->dev, info->board);\n\t}\n",
     "\t\tparse_dt(&client->dev, info->board);\n\t}\n"
     "\tif (k618_irq_new && info->board && gpio_is_valid(info->board->irq_gpio))\n"
     "\t\tclient->irq = gpio_to_irq(info->board->irq_gpio);\t/* k618 devices: the new pin's */\n"),
    # the 13 Ultra's xiaomi_touch.ko (OS3.0.308.0, 0x59dc..0x5c14) has two more THP nodes,
    # touch_thp_breakline_mode and touch_thp_breakline_result: an int each (pdata+0x1020 /
    # +0x1024), "%d" both ways, sysfs_notify on store -- what K618_INT_NODE (drafts/k618_touch_thp.py)
    # does. Values passed between the THP algorithm and the HAL; the IC is not touched. Present
    # on all three phones (the others' userspace never opens them).
    ("xiaomi/xiaomi_touch.h",
     "\tint thp_testmode, thp_testresult, thp_preset_point;\n",
     "\tint thp_testmode, thp_testresult, thp_preset_point;\n"
     "\tint thp_breakline_mode, thp_breakline_result;\t/* k618 devices: the 13 Ultra's nodes */\n"),
    ("xiaomi/xiaomi_touch.c",
     "K618_INT_NODE(touch_sensor_ctrl, sensor_ctrl)\n",
     "K618_INT_NODE(touch_sensor_ctrl, sensor_ctrl)\n"
     "K618_INT_NODE(touch_thp_breakline_mode, thp_breakline_mode)\t/* k618 devices */\n"
     "K618_INT_NODE(touch_thp_breakline_result, thp_breakline_result)\n"),
    ("xiaomi/xiaomi_touch.c",
     "static DEVICE_ATTR(touch_sensor_ctrl, 0664, touch_sensor_ctrl_show, touch_sensor_ctrl_store);\n",
     "static DEVICE_ATTR(touch_sensor_ctrl, 0664, touch_sensor_ctrl_show, touch_sensor_ctrl_store);\n"
     "static DEVICE_ATTR(touch_thp_breakline_mode, 0664, touch_thp_breakline_mode_show,\n"
     "\t\t   touch_thp_breakline_mode_store);\t/* k618 devices */\n"
     "static DEVICE_ATTR(touch_thp_breakline_result, 0664, touch_thp_breakline_result_show,\n"
     "\t\t   touch_thp_breakline_result_store);\n"),
    ("xiaomi/xiaomi_touch.c",
     "\t&dev_attr_touch_sensor_ctrl.attr,\n",
     "\t&dev_attr_touch_sensor_ctrl.attr,\n"
     "\t&dev_attr_touch_thp_breakline_mode.attr,\t/* k618 devices */\n"
     "\t&dev_attr_touch_thp_breakline_result.attr,\n"),
    # the panel for panel events. The touch node's "panel" list names the 13 Pro's 38 0c 0a
    # panel and a reference panel, not its second panel (42 02 0b, which some 13 Pro carry), so on
    # those phones no listed panel is registered and touch got no screen on / off / AOD events.
    # Only the active panel is registered with drm: take the first registered one among the DSI
    # panel nodes when none of the list is.
    ("fts_spi/fts.c",
     "static int fts_ts_check_panel(struct device_node *np)\n{\n",
     "/* k618 devices: the registered panel, when none of the node's list is (see fts_ts_check_panel) */\n"
     "static struct drm_panel *k618_registered_panel(void)\n"
     "{\n"
     "\tstruct device_node *pn;\n"
     "\tstruct drm_panel *p;\n"
     "\n"
     "\tfor_each_node_with_property(pn, \"qcom,mdss-dsi-panel-name\") {\n"
     "\t\tp = of_drm_find_panel(pn);\n"
     "\t\tif (!IS_ERR(p)) {\n"
     "\t\t\tof_node_put(pn);\n"
     "\t\t\treturn p;\n"
     "\t\t}\n"
     "\t}\n"
     "\treturn NULL;\n"
     "}\n"
     "\n"
     "static int fts_ts_check_panel(struct device_node *np)\n{\n"),
    ("fts_spi/fts.c",
     "\t\t} else {\n\t\t\tactive_panel = NULL;\n\t\t}\n\t}\n\n\treturn PTR_ERR(panel);\n}\n",
     "\t\t} else {\n\t\t\tactive_panel = NULL;\n\t\t}\n\t}\n\n"
     "\tactive_panel = k618_registered_panel();\t/* k618 devices */\n"
     "\tif (active_panel) {\n"
     "\t\tlogError(1, \"%s k618: panel not in the node's list, the registered one taken\\n\", tag);\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\treturn PTR_ERR(panel);\n}\n"),
]


def main(root):
    for path, old, new in EDITS:
        p = os.path.join(root, path)
        s = open(p, encoding="utf-8", errors="surrogateescape").read()
        if new in s:
            print(f"k618_touch_devices: {path}: already applied")
            continue
        if s.count(old) != 1:
            sys.exit(f"k618_touch_devices: {path}: anchor found {s.count(old)} times: {old[:60]!r}")
        s = s.replace(old, new)
        open(p, "w", encoding="utf-8", errors="surrogateescape").write(s)
    print(f"k618_touch_devices: {len(EDITS)} edits")


if __name__ == "__main__":
    main(sys.argv[1])
