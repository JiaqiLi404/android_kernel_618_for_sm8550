#!/usr/bin/env python3
'Device support and compatibility helpers.'

import os
import re
import sys

# the files replaced, as they are in Xiaomi's tree
FILES = [
    "drivers/power/supply/qti_battery_charger.c",
    "drivers/input/misc/qcom-hv-haptics.c",
]

CHG = "drivers/power/supply/qti_battery_charger.c"
HAP = "drivers/input/misc/qcom-hv-haptics.c"

# 5.15 -> 6.x, as a regular expression over one file: (file, pattern, replacement, why)
RE_EDITS = [
    # A class attribute's show() and store() take the class and the attribute as const since 6.4
    # (commit 7b884b7f24b4 and its kin). 134 signatures in the charger, 5 in the haptics driver;
    # soc-repo's own 6.12 copies of both files are already written this way. The embedded
    # struct class (battery_class / hap_class) stays as it is: class_register() takes it as const.
    (CHG, re.compile(r"(?<!const )\bstruct class \*c,"), "const struct class *c,"),
    (CHG, re.compile(r"(?<!const )\bstruct class_attribute \*attr"),
     "const struct class_attribute *attr"),
    (HAP, re.compile(r"(?<!const )\bstruct class \*c,"), "const struct class *c,"),
    (HAP, re.compile(r"(?<!const )\bstruct class_attribute \*attr"),
     "const struct class_attribute *attr"),
]

# 5.15 -> 6.x, literal: (file, old, new)
EDITS = [
    # Qualcomm's glink transport is soc-repo's own qti_pmic_glink; the kernel has a pmic_glink.h of
    # its own now (the USB/battery glink client in drivers/soc/qcom), and that is what this path
    # resolves to -- a struct of the same name and none of these calls. The declarations in the two
    # headers are otherwise identical. k618_fuxi_mods.py makes the same change in spmi-glink-debug.
    (CHG, "#include <linux/soc/qcom/pmic_glink.h>", "#include <linux/soc/qcom/qti_pmic_glink.h>"),
    # power_supply_desc.usb_types is a bitmask since 6.11 (and num_usb_types is gone): the same ten
    # types, in the form soc-repo's own copy of this driver already has
    (CHG,
     """static enum power_supply_usb_type usb_psy_supported_types[] = {
\tPOWER_SUPPLY_USB_TYPE_UNKNOWN,
\tPOWER_SUPPLY_USB_TYPE_SDP,
\tPOWER_SUPPLY_USB_TYPE_DCP,
\tPOWER_SUPPLY_USB_TYPE_CDP,
\tPOWER_SUPPLY_USB_TYPE_ACA,
\tPOWER_SUPPLY_USB_TYPE_C,
\tPOWER_SUPPLY_USB_TYPE_PD,
\tPOWER_SUPPLY_USB_TYPE_PD_DRP,
\tPOWER_SUPPLY_USB_TYPE_PD_PPS,
\tPOWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID,
};

""", ""),
    (CHG,
     "\t.usb_types\t\t= usb_psy_supported_types,\n"
     "\t.num_usb_types\t\t= ARRAY_SIZE(usb_psy_supported_types),\n",
     "\t.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_SDP)     |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_DCP)     |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_CDP)     |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_ACA)     |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_C)       |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_PD)      |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_PD_DRP)  |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_PD_PPS)  |\n"
     "\t\t     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),\n"),
    # struct thermal_zone_device is private to the thermal core since 6.4; the zone's own data
    # comes back through a call
    (CHG, "\tpsy = tzd->devdata;\n", "\tpsy = thermal_zone_device_priv(tzd);\n"),
    # a zone with no trip points has a registration call of its own since 6.4, and it comes back
    # disabled -- the power-supply core enables the zone it registers itself the same way
    # (psy_register_thermal()). batt_psy_desc asks for no_thermal, so this zone, whose get_temp
    # reads Xiaomi's XM_PROP_THERMAL_TEMP, is the battery's only one. Its name is the supply's, as
    # before ("battery"), which is what the ROM's thermal service looks for.
    (CHG,
     "\tpsy->tzd = thermal_zone_device_register(psy->desc->name,\n"
     "\t\t\t\t\t0, 0, psy, &psy_tzd_ops, NULL, 0, 0);\n",
     "\tpsy->tzd = thermal_tripless_zone_device_register(psy->desc->name,\n"
     "\t\t\t\t\tpsy, &psy_tzd_ops, NULL);\n"
     "\tif (IS_ERR(psy->tzd)) {\n"
     "\t\tpr_err(\"Failed to register battery thermal zone, rc=%ld\\n\",\n"
     "\t\t\t\tPTR_ERR(psy->tzd));\n"
     "\t\tpsy->tzd = NULL;\n"
     "\t} else if (thermal_zone_device_enable(psy->tzd)) {\n"
     "\t\tpr_err(\"Failed to enable battery thermal zone\\n\");\n"
     "\t}\n"),
    # a device type's .uevent takes the device as const since 6.2 (commit 31b05212360a). The body
    # needs nothing else: container_of() casts the qualifier away, platform_get_drvdata() takes a
    # const device of its own, and the attribute show() calls it makes now take a const class.
    (CHG, "static int add_xiaomi_uevent(struct device *dev, struct kobj_uevent_env *env)",
     "static int add_xiaomi_uevent(const struct device *dev, struct kobj_uevent_env *env)"),
    # a platform driver's .remove returns void since 6.10: the value this one returns in the middle
    # (and at the end) goes with the signature, which VOID_REMOVE below rewrites
    (CHG,
     "\trc = pmic_glink_unregister_client(bcdev->client);\n"
     "\tif (rc < 0) {\n"
     "\t\tpr_err(\"Error unregistering from pmic_glink, rc=%d\\n\", rc);\n"
     "\t\treturn rc;\n"
     "\t}\n",
     "\trc = pmic_glink_unregister_client(bcdev->client);\n"
     "\tif (rc < 0)\n"
     "\t\tpr_err(\"Error unregistering from pmic_glink, rc=%d\\n\", rc);\n"),
    # vmalloc() comes from a header of its own since 6.x -- 5.15 got it through another include
    # here (the FIFO sample buffer, haptics_load_custom_effect())
    (HAP, "#include <linux/uaccess.h>\n", "#include <linux/uaccess.h>\n#include <linux/vmalloc.h>\n"),
    # hrtimer_init() is gone in 6.18: hrtimer_setup() takes the callback, and struct hrtimer's
    # function member is __private now. k618_fix618.py converts the other timer in this file
    # (chip->hbst_off_timer) by itself, but not this one: it looks for the callback in an
    # assignment to the timer it was given, and Xiaomi writes the timer in parentheses here --
    # hrtimer_init(&(g_mhaptic.delay_off_timer), ...) against g_mhaptic.delay_off_timer.function --
    # so it finds none, and with two callbacks in the file it cannot guess (it would pass NULL and
    # leave the assignment to the private member behind). Converted here instead.
    (HAP,
     "\thrtimer_init(&(g_mhaptic.delay_off_timer), CLOCK_MONOTONIC, HRTIMER_MODE_REL);\n"
     "\tg_mhaptic.delay_off_timer.function = haptics_disable_fifo_delay_timer;\n",
     "\thrtimer_setup(&g_mhaptic.delay_off_timer, haptics_disable_fifo_delay_timer,\n"
     "\t\t\tCLOCK_MONOTONIC, HRTIMER_MODE_REL);\n"),
]

# text put before an anchor: (file, anchor, text)
INSERTS = [
    # Xiaomi's build defines these for this phone in drivers/input/Makefile, from TARGET_PRODUCT
    # (nuwa): the custom effect's full scale, the wireless-backcharge check that pauses the motor,
    # and the f0 sanity check. soc-repo's Kbuild is generated from modules.bzl, which carries no
    # per-module CPPFLAGS, so they are defined here -- #ifndef, so that a Kconfig option of the
    # same name would still win.
    (HAP, "#define CREATE_TRACE_POINTS\n",
     """/*
 * k618: what Xiaomi's drivers/input/Makefile defines for this phone (TARGET_PRODUCT nuwa):
 * the custom (FIFO) effect's 8000 mV full scale, the wireless-reverse-charge check that
 * holds the motor off, and QCOM_HAPTIC_F0_PROTECT, the boot-time f0 sanity check that reads
 * qcom,lra-f0-cal-count/default/max/min from the devicetree (this phone's node has all four).
 */
#ifndef CONFIG_TARGET_PRODUCT_NUWA
#define CONFIG_TARGET_PRODUCT_NUWA 1
#endif
#ifndef QCOM_HAPTIC_F0_PROTECT
#define QCOM_HAPTIC_F0_PROTECT 1
#endif

"""),
]

# a platform driver's .remove returns void since 6.10: (file, function) -- the signature and the
# `return 0;` the body ends with are both rewritten (as k618_fuxi_mods.py does)
VOID_REMOVE = [
    (CHG, "battery_chg_remove"),
    (HAP, "haptics_remove"),
]

# an option of Xiaomi's that soc-repo's Kconfig has no entry for: (Kconfig file, option).
# MI_WIRELESS is the 13 Pro's 50 W wireless charging -- the reverse-charge, tx_mac, rx_cep and
# wls_fw_state attributes among others, which the ROM's own module has as well (its qti_battery_
# charger.ko has all of their names in it). k618_overrides.bzl has to turn it on; see the notes.
KCONFIG = [("drivers/power/supply/Kconfig", "MI_WIRELESS")]


def void_remove(text, func, rel):
    """static int <func>(struct platform_device *) -> static void, without its final return"""
    sig = f"static int {func}(struct platform_device *pdev)"
    if f"static void {func}(" in text:
        return text
    if sig not in text:
        print(f"k618_mi_chg_hap: {rel}: no {sig}", file=sys.stderr)
        return text
    start = text.index(sig)
    end = text.index("\n}\n", start) + 3		# the function's closing brace at column 0
    body = text[start:end].replace(sig, f"static void {func}(struct platform_device *pdev)")
    body = re.sub(r"\n\treturn 0;\n\}\n$", "\n}\n", body)
    if "return" in body.rsplit("\n}", 2)[0][-40:]:
        print(f"k618_mi_chg_hap: {rel}: {func} still returns a value", file=sys.stderr)
    return text[:start] + body + text[end:]


def port(fuxi, rel):
    """Xiaomi's file with the edits for it applied, or None if one of them does not fit"""
    src = os.path.join(fuxi, rel)
    if not os.path.exists(src):
        print(f"k618_mi_chg_hap: no {rel} in Xiaomi's tree", file=sys.stderr)
        return None
    text = open(src, encoding="utf-8", errors="surrogateescape").read()
    ok = True
    for f, old, new in EDITS:
        if f != rel:
            continue
        if old not in text:
            print(f"k618_mi_chg_hap: {rel}: text to change not found: {old[:60]!r}", file=sys.stderr)
            ok = False
            continue
        text = text.replace(old, new)
    for f, pat, new in RE_EDITS:
        if f != rel:
            continue
        text, n = pat.subn(new, text)
        if not n:
            print(f"k618_mi_chg_hap: {rel}: no match for {pat.pattern!r}", file=sys.stderr)
            ok = False
    for f, anchor, add in INSERTS:
        if f != rel:
            continue
        if anchor not in text:
            print(f"k618_mi_chg_hap: {rel}: anchor not found: {anchor.strip()!r}", file=sys.stderr)
            ok = False
            continue
        i = text.index(anchor)
        text = text[:i] + add + text[i:]
    for f, func in VOID_REMOVE:
        if f == rel:
            text = void_remove(text, func, rel)
    return text if ok else None


def kconfig_block(text, opt):
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line.strip() in (f"config {opt}", f"menuconfig {opt}"):
            j = i + 1
            while j < len(lines) and not lines[j].startswith(("config ", "menuconfig ", "endif",
                                                              "endmenu", "menu ", "if ", "source ",
                                                              "choice", "comment ")):
                j += 1
            return "\n".join(lines[i:j]).rstrip() + "\n"
    return None


def main(argv):
    if len(argv) < 3:
        print(__doc__.split("\n")[0], file=sys.stderr)
        return 2
    if os.environ.get("K618_MI_CHG_HAP") != "1":
        print("k618_mi_chg_hap: experimental adapter; XM property numbers differ from the target "
              "firmware -- K618_MI_CHG_HAP=1 explicitly enables it", file=sys.stderr)
        return 1
    fuxi, soc = argv[1].rstrip("/"), argv[2].rstrip("/")
    changed = 0
    for rel in FILES:
        text = port(fuxi, rel)
        if text is None:
            print(f"k618_mi_chg_hap: {rel} left alone", file=sys.stderr)
            return 1
        dst = os.path.join(soc, rel)
        if os.path.exists(dst) and \
                open(dst, encoding="utf-8", errors="surrogateescape").read() == text:
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)
        print(f"k618_mi_chg_hap: {rel}: Xiaomi's driver, ported "
              f"({len(text.splitlines())} lines)")
        changed += 1
    for kc, opt in KCONFIG:
        dst = os.path.join(soc, kc)
        if kconfig_block(open(dst, encoding="utf-8").read(), opt):
            continue
        blk = kconfig_block(open(os.path.join(fuxi, kc), encoding="utf-8",
                                 errors="replace").read(), opt)
        if not blk:
            print(f"k618_mi_chg_hap: {opt} is not in Xiaomi's {kc}", file=sys.stderr)
            return 1
        with open(dst, "a", encoding="utf-8", newline="\n") as f:
            f.write(f"\n# from Xiaomi's 13 Pro tree (fuxi-t-oss)\n{blk}")
        print(f"k618_mi_chg_hap: {kc}: {opt}")
        changed += 1
    print(f"k618_mi_chg_hap: {changed} files changed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
