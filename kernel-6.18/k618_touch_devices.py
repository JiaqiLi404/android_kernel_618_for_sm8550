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
