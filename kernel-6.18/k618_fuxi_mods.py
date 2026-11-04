#!/usr/bin/env python3
"""k618_fuxi_mods.py <fuxi tree> <soc tree>: the second-stage modules that only Xiaomi's 13 Pro
tree has, imported into soc-repo.

The phone loads a number of Qualcomm vendor modules that the 6.12 soc release no longer carries --
the sleep and memory-latency statistics, the thermal limits driver, the PWM and tri-colour LED
drivers, MHI's controller and its device drivers, the fingerprint and QDSS bridges, and so on. Their
sources are in Xiaomi's 5.15 release (MiCode fuxi-t-oss, on Qualcomm's LA.VENDOR.13.2.0), so they
come from there: the files as they are, their Kconfig entry, and a modules.bzl registration so
k618_soc_kbuild.py builds them like any other soc module. k618_overrides.bzl turns the options on.

Three of the phone's modules cannot be imported: Xiaomi published only a Makefile for perfmgr
(drivers/mihw/perfmgr) and only one file of mi_cnss_statistic, and stm_p_basic's source is the
kernel's own p_basic.c, which soc's STM copy leaves out (it is imported from the kernel tree here).

Idempotent; prints what it did."""

import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import k618_soc_kbuild as kb  # noqa: E402

# (module, config, directory, sources, soc registry names it depends on)
MODS = [
    # power and sleep statistics, as the phone's second stage loads them
    # (not qcom_stats: this needs nothing from it, and the two cannot be loaded together -- see
    # CONFIG_QCOM_STATS in k618_overrides.bzl)
    ("soc_sleep_stats", "CONFIG_QCOM_SOC_SLEEP_STATS", "drivers/soc/qcom",
     ["soc_sleep_stats.c"], ["drivers/soc/qcom/smem"]),
    ("subsystem_sleep_stats", "CONFIG_QCOM_SUBSYSTEM_SLEEP_STATS", "drivers/soc/qcom",
     ["subsystem_sleep_stats.c"], ["drivers/soc/qcom/smem"]),
    # NOT imported: rimps_log -- its option depends on QCOM_RIMPS, the RIMPS mailbox driver, which
    # this release does not have either (the phone's qcom_rimps.ko is on the first stage's own list
    # of what we cannot build yet)
    ("core_hang_detect", "CONFIG_MSM_CORE_HANG_DETECT", "drivers/soc/qcom",
     ["core_hang_detect.c"], []),
    ("usb_bam", "CONFIG_USB_BAM", "drivers/soc/qcom", ["usb_bam.c"], []),
    # NOT imported: qdss_bridge -- it reaches inside struct usb_qdss_ch, which soc-repo's own 6.12
    # usb_qdss.h keeps opaque (the USB gadget function owns it now)
    ("qbt_handler", "CONFIG_MSM_QBT_HANDLER", "drivers/soc/qcom", ["qbt_handler.c"], []),
    # the memory-latency governor's SCMI client and the protocol it speaks
    # NOT imported: memlat_scmi -- on 5.15 it was a separate module that called into memlat's
    # cpucp_memlat_init(); in the 6.12 driver that function is static inside memlat.c, so the SCMI
    # client is part of memlat.ko (which we already build) and there is nothing left to split out
    ("memlat_vendor", "CONFIG_QTI_SCMI_MEMLAT_PROTOCOL", "drivers/firmware/arm_scmi",
     ["memlat_vendor.c"], []),


    ("qcom_rimps", "CONFIG_QCOM_RIMPS", "drivers/soc/qcom", ["qcom_rimps.c"], []),
    # thermal
    ("msm_lmh_dcvs", "CONFIG_QTI_THERMAL_LIMITS_DCVS", "drivers/thermal/qcom",
     ["msm_lmh_dcvs.c"], []),
    # the notification light and the PWM provider under it (the phone's DT has qcom,pwm-lpg twice
    # and qcom,tri-led; its own modules.dep has the same one-way dependency between the two).
    # 6.11 moved a PWM provider to a framework-allocated chip -- the EDITS below. The LPG driver's
    # one call into the PBS driver falls to the stub in soc's qpnp-pbs.h with CONFIG_QPNP_PBS off,
    # which is how the phone's own module is built too (its modules.dep line is empty)
    ("pwm-qti-lpg", "CONFIG_PWM_QTI_LPG", "drivers/pwm", ["pwm-qti-lpg.c"], []),
    ("leds-qti-tri-led", "CONFIG_LEDS_QTI_TRI_LED", "drivers/leds",
     ["leds-qti-tri-led.c"], []),
    # the rest of what the phone's list names
    # NOT imported: usb_f_diag -- soc-repo has no Kconfig in drivers/usb/gadget/function, so the
    # option never sticks through olddefconfig and usbdiag.h takes its no-driver branch
    ("spmi-glink-debug", "CONFIG_SPMI_QTI_GLINK_DEBUG", "drivers/spmi",
     ["spmi-glink-debug.c"], ["drivers/soc/qcom/qti_pmic_glink"]),
    ("xlogchar", "CONFIG_XLOGCHAR", "drivers/char", ["xlogchar.c"], []),
    # the trust management engine's mailbox client: the phone's hwkm.ko uses it once, before the
    # first wrapped key is programmed into the inline crypto engine (see ice.c's k618 step in
    # k618_kalama.py). The 6.12 soc release has no copy; the phone loads tmecom-intf.ko
    ("tmecom-intf", "CONFIG_MSM_TMECOM_QMP", "drivers/soc/qcom/tmecom",
     ["tmecom.c", "tme_hwkm_master.c", "tmecom.h", "tme_hwkm_master_intf.h"], []),
    # NOT imported: coresight-hwevent -- it registers with CORESIGHT_DEV_TYPE_NONE, a device type
    # 6.12's coresight core no longer has (every device is now a source, link, sink or helper)
    ("goodix_fod", "CONFIG_FINGERPRINT_GOODIX_FOD", "drivers/input/fingerprint/goodix_fod",
     ["gf_spi.c", "gf_spi.h", "platform.c", "netlink.c"], []),
    # NOT imported: qcom_ipc_lite -- soc-repo has no Kconfig under drivers/media/platform/msm/synx,
    # so CONFIG_MSM_GLOBAL_SYNX cannot be given one that Kconfig would read
    # NOT imported: ipa_fmwk (the 5.15 IPA framework shim). Its header pulls in the whole 5.15 IPA
    # interface -- include/linux/ipa.h, uapi/linux/msm_ipa.h and eight more -- which, sitting in
    # soc-repo's include/, would shadow the dataipa techpack's own copies for every techpack build.
    # The 6.12 data path is dataipa's (ipam.ko and friends), which does not use this shim.
]

# already in soc-repo, only missing from its registry: the 6.12 sources, no import, no API drift
SOC_MODS = [
    ("cpu_voltage_cooling", "CONFIG_QTI_CPU_VOLTAGE_COOLING_DEVICE", "drivers/thermal/qcom",
     ["cpu_voltage_cooling.c"], []),
    ("policy_engine", "CONFIG_QTI_POLICY_ENGINE_SENSOR", "drivers/thermal/qcom",
     ["policy_engine.c"], []),
    ("sdpm_clk", "CONFIG_QTI_SDPM_CLOCK_MONITOR", "drivers/thermal/qcom", ["sdpm_clk.c"], []),
    # MHI: the controller for the modem/WLAN link and the device drivers on it
    # NOT registered: mhi_cntrl_qcom, mhi_dev_netdev, mhi_dev_dtr. soc-repo carries these files but
    # does not build them for canoe, and they are written against the 5.15 MHI stack: struct
    # mhi_controller has no session_id any more, mhi_poll() is gone, and the device-id table needs
    # the new mod_devicetable form. The phone needs them for the modem link, so they are a port of
    # their own (the 6.12 MHI host API), not drift to patch over.
    # NOT registered: icc-test -- soc-repo's debugfs-test.c still calls icc_get(), which upstream
    # replaced with of_icc_get()/icc_get_by_index()
]

# headers of the driver's own that nothing it includes names (a soc-repo header pulls them in)
EXTRA_HDRS = [("include/linux/usb", "usb_qdss.h"),
              ("include/linux", "tme_hwkm_master.h"), ("include/linux", "tme_hwkm_master_defs.h")]

# a platform driver's .remove returns void since 6.10: (file, function) -- the signature and the
# `return 0;` the body ends with are both rewritten
VOID_REMOVE = [
    ("drivers/soc/qcom/soc_sleep_stats.c", "soc_sleep_stats_remove"),
    ("drivers/soc/qcom/subsystem_sleep_stats.c", "subsystem_stats_remove"),
    ("drivers/soc/qcom/usb_bam.c", "usb_bam_remove"),
    ("drivers/thermal/qcom/cpu_voltage_cooling.c", "cc_cooling_remove"),
    ("drivers/thermal/qcom/sdpm_clk.c", "sdpm_clk_device_remove"),
    ("drivers/thermal/qcom/policy_engine.c", "pe_sens_device_remove"),
    ("drivers/input/fingerprint/goodix_fod/gf_spi.c", "gf_remove"),
    ("drivers/soc/qcom/core_hang_detect.c", "msm_hang_detect_remove"),
    ("drivers/spmi/spmi-glink-debug.c", "spmi_glink_remove"),
    ("drivers/soc/qcom/qbt_handler.c", "qbt_remove"),
    ("drivers/pwm/pwm-qti-lpg.c", "qpnp_lpg_remove"),
    ("drivers/leds/leds-qti-tri-led.c", "qpnp_tri_led_remove"),
    ("drivers/soc/qcom/tmecom/tmecom.c", "tmecom_remove"),
    ("drivers/soc/qcom/qcom_rimps.c", "qcom_rimps_remove"),
]

# Kconfig.msm does not source every directory's Kconfig; an imported option in one of those would
# never reach the configuration (drivers/pwm is the only one here)
KCONFIG_SOURCES = ["drivers/pwm/Kconfig", "drivers/soc/qcom/tmecom/Kconfig"]	# (drivers/leds and drivers/char it already reads)

# 5.15 -> 6.12 drift in the imported drivers: (file, old, new, why)
EDITS = [


    ("drivers/soc/qcom/tmecom/tme_hwkm_master.c",
     "EXPORT_SYMBOL(tme_hwkm_master_broadcast_transportkey);",
     "EXPORT_SYMBOL_GPL(tme_hwkm_master_broadcast_transportkey);"),


    ("drivers/firmware/arm_scmi/memlat_vendor.c",
     "\t.id = SCMI_PROTOCOL_MEMLAT,\n",
     "\t.id = SCMI_PROTOCOL_MEMLAT,\n\t.vendor_id = \"Qualcomm\",\n"),
    # class_create() lost its module argument in 6.4
    ("drivers/char/xlogchar.c",
     'class_create(THIS_MODULE, "xlog")', 'class_create("xlog")'),
    ("drivers/soc/qcom/subsystem_sleep_stats.c",
     "class_create(THIS_MODULE, STATS_DEVICE_NAME)", "class_create(STATS_DEVICE_NAME)"),
    # The 6.12 tree's own qcom_stats is the successor of these two and exports the same ddr_stats_*
    # symbols, so the second of them to be insmod'ed fails ("exports duplicate symbol") -- fatal in
    # the first stage. This phone's devicetree has qcom,rpmh-sleep-stats and no qcom,rpmh-stats, and
    # the ROM ships soc_sleep_stats.ko and no qcom_stats.ko, so qcom_stats is the one left out
    # (CONFIG_QCOM_STATS in k618_overrides.bzl). Its only user here is sys_pm_vx, which takes just
    # has_system_slept() and has_subsystem_slept() from it -- both exported by subsystem_sleep_stats
    # too, with the 5.15 signature that takes no out-parameter. That is the form the phone's own
    # sys_pm_vx.ko is built against, so aosd_enhancement stays false as it does there.
    ("drivers/soc/qcom/sys_pm_vx.c",
     "#include <soc/qcom/qcom_stats.h>",
     "#include <soc/qcom/subsystem_sleep_stats.h>"),
    ("drivers/soc/qcom/sys_pm_vx.c",
     "\tbool aosd_enhancement;\n", "\tbool aosd_enhancement = false;\n"),
    ("drivers/soc/qcom/sys_pm_vx.c",
     "has_system_slept(&aosd_enhancement)", "has_system_slept()"),
    # 6.11 took the PWM chip away from the driver: the framework allocates it (and the device in
    # it is now embedded, not a pointer), the driver reaches its own data through
    # pwmchip_get_drvdata() and the parent device through pwmchip_parent(), and .base is gone
    ("drivers/pwm/pwm-qti-lpg.c",
     "\tstruct pwm_chip\t\tpwm_chip;\n", "\tstruct pwm_chip\t\t*pwm_chip;\n"),
    ("drivers/pwm/pwm-qti-lpg.c",
     "\tstruct qpnp_lpg_chip *chip = container_of(pwm_chip,\n"
     "\t\t\tstruct qpnp_lpg_chip, pwm_chip);\n",
     "\tstruct qpnp_lpg_chip *chip = pwmchip_get_drvdata(pwm_chip);\n"),
    # (the chip's device is embedded in it now, so neither spelling of "the chip's device" is a
    # pointer any more -- both go through pwmchip_parent())
    ("drivers/pwm/pwm-qti-lpg.c", "pwm->chip->dev", "pwmchip_parent(pwm->chip)"),
    ("drivers/pwm/pwm-qti-lpg.c", "pwm_chip->dev", "pwmchip_parent(pwm_chip)"),
    ("drivers/pwm/pwm-qti-lpg.c",
     "pwm = &chip->pwm_chip.pwms[lpg_idx];", "pwm = &chip->pwm_chip->pwms[lpg_idx];"),
    ("drivers/pwm/pwm-qti-lpg.c",
     "\tchip->pwm_chip.dev = chip->dev;\n"
     "\tchip->pwm_chip.base = -1;\n"
     "\tchip->pwm_chip.npwm = chip->num_lpgs;\n"
     "\tchip->pwm_chip.ops = &qpnp_lpg_pwm_ops;\n"
     "\n\trc = pwmchip_add(&chip->pwm_chip);\n",
     "\tchip->pwm_chip = devm_pwmchip_alloc(chip->dev, chip->num_lpgs, 0);\n"
     "\tif (IS_ERR(chip->pwm_chip)) {\n"
     "\t\trc = PTR_ERR(chip->pwm_chip);\n"
     "\t\tgoto err_out;\n"
     "\t}\n"
     "\tchip->pwm_chip->ops = &qpnp_lpg_pwm_ops;\n"
     "\tpwmchip_set_drvdata(chip->pwm_chip, chip);\n"
     "\n\trc = pwmchip_add(chip->pwm_chip);\n"),
    ("drivers/pwm/pwm-qti-lpg.c",
     "\tpwmchip_remove(&chip->pwm_chip);\n", "\tpwmchip_remove(chip->pwm_chip);\n"),
    # pwm_ops lost .owner in 6.7: pwmchip_add() takes THIS_MODULE itself
    ("drivers/pwm/pwm-qti-lpg.c",
     "\t.apply = qpnp_lpg_pwm_apply,\n\t.owner = THIS_MODULE,\n",
     "\t.apply = qpnp_lpg_pwm_apply,\n"),
    # a PWM consumer: the apply call says it may sleep since 6.8, and the lookup takes a fwnode
    # since 6.5 (devm_of_pwm_get went with the old one)
    ("drivers/leds/leds-qti-tri-led.c",
     "pwm_apply_state(led->pwm_dev, &pstate)", "pwm_apply_might_sleep(led->pwm_dev, &pstate)"),
    ("drivers/leds/leds-qti-tri-led.c",
     "devm_of_pwm_get(chip->dev, child_node, NULL)",
     "devm_fwnode_pwm_get(chip->dev, of_fwnode_handle(child_node), NULL)"),
    # strlcpy went away in 6.8; strscpy is the one to use
    ("drivers/char/xlogchar.c", "strlcpy(", "strscpy("),
    # netlink_kernel_cfg lost cb_mutex (the netlink core keeps its own lock now)
    ("drivers/input/fingerprint/goodix_fod/netlink.c",
     "\tnetlink_cfg.cb_mutex = NULL;\n",
     "\t/* k618: netlink_kernel_cfg has no cb_mutex since 6.x */\n"),
    ("drivers/input/fingerprint/goodix_fod/gf_spi.c",
     "class_create(THIS_MODULE, CLASS_NAME)", "class_create(CLASS_NAME)"),
    ("drivers/soc/qcom/qbt_handler.c",
     "class_create(THIS_MODULE,\n\t\t\t\t\t   drvdata->qbt_node)",
     "class_create(drvdata->qbt_node)"),
    # a return value in the middle of a .remove that now returns void
    ("drivers/soc/qcom/subsystem_sleep_stats.c",
     "\tstats_data = platform_get_drvdata(pdev);\n\tif (!stats_data)\n\t\treturn 0;\n",
     "\tstats_data = platform_get_drvdata(pdev);\n\tif (!stats_data)\n\t\treturn;\n"),
    # the legacy GPIO wrappers are gone; gpio_set_value / gpio_get_value are what stayed
    ("drivers/soc/qcom/qbt_handler.c", "__gpio_set_value(", "gpio_set_value("),
    ("drivers/soc/qcom/qbt_handler.c", "__gpio_get_value(", "gpio_get_value("),
    # of_get_named_gpio_flags() and enum of_gpio_flags went away with the legacy OF GPIO API; the
    # descriptor interface gives the same "active low" answer
    ("drivers/soc/qcom/qbt_handler.c",
     "\tenum of_gpio_flags flags;\n", "\tstruct gpio_desc *fd_desc;\n"),
    ("drivers/soc/qcom/qbt_handler.c",
     "\tgpio = of_get_named_gpio_flags(pdev->dev.of_node,\n"
     "\t\t\t\t\"qcom,finger-detect-gpio\", 0, &flags);\n",
     "\tgpio = of_get_named_gpio(pdev->dev.of_node, \"qcom,finger-detect-gpio\", 0);\n"
     "\tfd_desc = gpio_is_valid(gpio) ? gpio_to_desc(gpio) : NULL;\n"),
    ("drivers/soc/qcom/qbt_handler.c",
     "\tdrvdata->fd_gpio.active_low = flags & OF_GPIO_ACTIVE_LOW;\n",
     "\tdrvdata->fd_gpio.active_low = fd_desc && gpiod_is_active_low(fd_desc);\n"),
    # struct bus_type lost dev_root in 6.4: bus_get_dev_root() gives the same device
    ("drivers/soc/qcom/core_hang_detect.c",
     "\t\t\t&cpu_subsys.dev_root->kobj, \"%s_%s\",",
     "\t\t\tk618_cpu_subsys_kobj(), \"%s_%s\","),
    # a thermal zone's .get_trend takes the trip table since 6.4
    ("drivers/thermal/qcom/policy_engine.c",
     "static int pe_sensor_get_trend(struct thermal_zone_device *tz, int trip, enum thermal_trend *trend)",
     "static int pe_sensor_get_trend(struct thermal_zone_device *tz, const struct thermal_trip *trip,\n"
     "\t\t\t       enum thermal_trend *trend)"),
    # includes 6.12 no longer gets through another header
    ("drivers/thermal/qcom/msm_lmh_dcvs.c",
     "#include <linux/thermal.h>", "#include <linux/cpu.h>\n#include <linux/thermal.h>"),
    ("drivers/input/fingerprint/goodix_fod/platform.c",
     "#include <linux/delay.h>", "#include <linux/delay.h>\n#include <linux/pinctrl/consumer.h>"),
    ("drivers/input/fingerprint/goodix_fod/gf_spi.c",
     "#include <linux/delay.h>", "#include <linux/delay.h>\n#include <linux/pinctrl/consumer.h>"),
    # asm/unaligned.h is linux/unaligned.h since 6.12 (soc's own SCMI header, which the memory
    # latency protocol is the first module here to pull in)
    ("drivers/firmware/arm_scmi/common.h",
     "#include <asm/unaligned.h>", "#include <linux/unaligned.h>"),
    # pmic_glink_write() is Qualcomm's own glink transport, soc-repo's qti_pmic_glink; the kernel's
    # own pmic_glink.h (which this path now resolves to) has no such call and a struct of the same
    # name, so it is replaced rather than added
    ("drivers/spmi/spmi-glink-debug.c",
     "#include <linux/soc/qcom/pmic_glink.h>",
     "#include <linux/soc/qcom/qti_pmic_glink.h>"),
]

# text added to a file, before an anchor: (file, anchor, text)
INSERTS = [
    ("drivers/soc/qcom/core_hang_detect.c", "static int msm_hang_detect_probe(",
     """/*
 * k618: struct bus_type has no dev_root since 6.4; bus_get_dev_root() takes a reference to the
 * same device, which this module holds for as long as it is loaded.
 */
static struct kobject *k618_cpu_subsys_kobj(void)
{
	static struct device *root;

	if (!root)
		root = bus_get_dev_root(&cpu_subsys);
	return root ? &root->kobj : NULL;
}

"""),
]


# from the kernel's own tree instead: soc's copy of the STM framework leaves this protocol out
FROM_KERNEL = [
    ("stm_p_basic", "CONFIG_STM_PROTO_BASIC", "drivers/hwtracing/stm", ["p_basic.c"],
     ["drivers/hwtracing/stm/stm_core"]),
]

HDR = re.compile(r'^#include "([\w./-]+)"', re.M)
SYSHDR = re.compile(r'^#include <([\w./-]+)>', re.M)
# where soc-repo keeps the headers a vendor module's <...> include resolves to, in order
INC_DIRS = ["include", "include/uapi"]
KERNEL_INC = ""				# set in main(): the kernel tree, whose own headers win


def copy_with_headers(src_root, dst_root, rel_dir, names, done):
    """the named files, and the same-directory headers they include, recursively"""
    todo = list(names)
    while todo:
        name = todo.pop()
        rel = os.path.join(rel_dir, name)
        if rel in done:
            continue
        s, d = os.path.join(src_root, rel), os.path.join(dst_root, rel)
        if not os.path.exists(s):
            print(f"k618_fuxi_mods: no {rel} in the source tree", file=sys.stderr)
            continue
        os.makedirs(os.path.dirname(d), exist_ok=True)
        if os.path.exists(d):
            # never replace a file soc-repo has of its own: its own code includes it, and the 5.15
            # copy of a shared header (coresight-common.h) does not match the 6.12 drivers around it
            if open(d, "rb").read() != open(s, "rb").read():
                print(f"k618_fuxi_mods: {rel} is already in soc-repo and differs -- left alone",
                      file=sys.stderr)
        else:
            shutil.copyfile(s, d)
            print(f"k618_fuxi_mods: copied {rel}")
        done.add(rel)
        if name.endswith((".c", ".h")):
            text = open(s, encoding="utf-8", errors="replace").read()
            todo += [h for h in HDR.findall(text)
                     if os.path.exists(os.path.join(src_root, rel_dir, h))]
            # the driver's own public headers: a <...> include that only the source tree has.
            # soc-repo's include/ comes before the kernel's own (k618_soc_kbuild.py's NOSTDINC),
            # so a header of the kernel's must never be copied over -- only ones it lacks
            for h in SYSHDR.findall(text):
                for inc in INC_DIRS:
                    src = os.path.join(src_root, inc, h)
                    if not os.path.exists(src):
                        continue
                    if any(os.path.exists(os.path.join(d, inc, h)) for d in (dst_root, KERNEL_INC)):
                        break
                    copy_with_headers(src_root, dst_root, inc, [h], done)
                    break


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


def starlark(out, cfg, rel_dir, srcs, deps):
    def lst(key, items):
        if not items:
            return ""
        body = "".join(f'            "{x}",\n' for x in items)
        return f"        {key} = [\n            # do not sort\n{body}        ],\n"
    name = f"{rel_dir}/{out}"
    return ("\n    registry.register(\n"
            f'        name = "{name}",\n'
            f'        out = "{out}.ko",\n'
            f'        config = "{cfg}",\n'
            + lst("srcs", [f"{rel_dir}/{s}" for s in srcs]) + lst("deps", deps) +
            "    )\n")


def register(soc, out, cfg, rel_dir, srcs, deps, have):
    name = f"{rel_dir}/{out}"
    if name in have:
        return False
    bzl = os.path.join(soc, rel_dir, "modules.bzl")
    if not os.path.exists(bzl):
        os.makedirs(os.path.dirname(bzl), exist_ok=True)
        open(bzl, "w", encoding="utf-8", newline="\n").write(
            "# generated by kernel-6.12/k618_fuxi_mods.py\ndef register_modules(registry):\n")
    text = open(bzl, encoding="utf-8").read()
    if f'name = "{name}",' in text:
        return False
    open(bzl, "a", encoding="utf-8", newline="\n").write(starlark(out, cfg, rel_dir, srcs, deps))
    print(f"k618_fuxi_mods: {rel_dir}/modules.bzl: {out} ({cfg})")
    return True


import k618_cpucp_scmi as _cs  # noqa: E402
MODS += _cs.MODS
SOC_MODS += _cs.SOC_MODS
EDITS += _cs.EDITS
INSERTS += _cs.INSERTS


def main(argv):
    global KERNEL_INC
    fuxi, soc = argv[1].rstrip("/"), argv[2].rstrip("/")
    kernel = argv[3].rstrip("/") if len(argv) > 3 else None
    KERNEL_INC = kernel or soc		# a header the kernel itself has is left to the kernel
    have = set(kb.load_registry(soc))
    done = set()
    added = 0
    for src_root, table in ((fuxi, MODS), (kernel, FROM_KERNEL), (None, SOC_MODS)):
        for out, cfg, rel_dir, srcs, deps in table:
            if src_root:
                copy_with_headers(src_root, soc, rel_dir, srcs, done)
            elif not all(os.path.exists(os.path.join(soc, rel_dir, s)) for s in srcs):
                print(f"k618_fuxi_mods: {out}: soc-repo does not have its sources", file=sys.stderr)
                continue
            # the option's own Kconfig text, so `olddefconfig` knows it (and its dependencies)
            kc_src, kc_dst = (os.path.join(src_root or fuxi, rel_dir, "Kconfig"),
                              os.path.join(soc, rel_dir, "Kconfig"))
            if os.path.exists(kc_src):
                dst_text = open(kc_dst, encoding="utf-8").read() if os.path.exists(kc_dst) else ""
                if not kconfig_block(dst_text, cfg[len("CONFIG_"):]):
                    blk = kconfig_block(open(kc_src, encoding="utf-8", errors="replace").read(),
                                        cfg[len("CONFIG_"):])
                    if blk:
                        with open(kc_dst, "a", encoding="utf-8", newline="\n") as f:
                            f.write(f"\n# from Xiaomi's 13 Pro tree (5.15)\n{blk}")
                        print(f"k618_fuxi_mods: {rel_dir}/Kconfig: {cfg}")
                    else:
                        print(f"k618_fuxi_mods: {cfg} has no Kconfig entry in {rel_dir}",
                              file=sys.stderr)
            added += register(soc, out, cfg, rel_dir, srcs, deps, have)
    for inc, h in EXTRA_HDRS:
        copy_with_headers(fuxi, soc, inc, [h], done)
    kmsm = os.path.join(soc, "Kconfig.msm")
    text = open(kmsm, encoding="utf-8").read()
    for src in KCONFIG_SOURCES:
        if f'$(KCONFIG_EXT_PREFIX){src}"' in text:
            continue
        # the same form the file's own lines use, so flatten_kconfig.sh finds it
        text += ('\n# k618: this directory has an option of ours in it\n'
                 f'source "$(KCONFIG_EXT_PREFIX){src}"\n')
        print(f"k618_fuxi_mods: Kconfig.msm sources {src}")
    open(kmsm, "w", encoding="utf-8", newline="").write(text)
    for rel, func in VOID_REMOVE:
        path = os.path.join(soc, rel)
        if not os.path.exists(path):
            print(f"k618_fuxi_mods: no {rel} for {func}", file=sys.stderr)
            continue
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        sig = f"static int {func}(struct platform_device *pdev)"
        if f"static void {func}(" in text:
            continue
        if sig not in text:
            print(f"k618_fuxi_mods: {rel}: no {sig}", file=sys.stderr)
            continue
        start = text.index(sig)
        end = text.index("\n}\n", start) + 3		# the function's closing brace at column 0
        body = text[start:end].replace(sig, f"static void {func}(struct platform_device *pdev)")
        body = re.sub(r"\n\treturn 0;\n\}\n$", "\n}\n", body)
        if "return 0;" in body.rsplit("\n}", 2)[0][-40:]:
            print(f"k618_fuxi_mods: {rel}: {func} still returns a value", file=sys.stderr)
        open(path, "w", encoding="utf-8", errors="surrogateescape",
             newline="").write(text[:start] + body + text[end:])
        print(f"k618_fuxi_mods: 6.12 in {rel} ({func} returns void)")
    for rel, old, new in EDITS:
        path = os.path.join(soc, rel)
        if not os.path.exists(path):
            print(f"k618_fuxi_mods: no {rel} to edit", file=sys.stderr)
            continue
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        # whether the edit has been made already cannot be read off `old` alone: an edit that keeps
        # its own anchor (adding an include before the one it names) leaves `old` in place, and one
        # whose `new` is a substring of `old` (or empty) leaves `new` findable before it is made
        if old in new:
            if new in text:
                continue
        elif new and new in text and old not in text:
            continue
        if old not in text:
            print(f"k618_fuxi_mods: {rel}: text to change not found", file=sys.stderr)
            continue
        open(path, "w", encoding="utf-8", errors="surrogateescape",
             newline="").write(text.replace(old, new))
        print(f"k618_fuxi_mods: 6.12 in {rel}")
    for rel, anchor, add in INSERTS:
        path = os.path.join(soc, rel)
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        if add in text or anchor not in text:
            continue
        open(path, "w", encoding="utf-8", errors="surrogateescape",
             newline="").write(text.replace(anchor, add + anchor, 1))
        print(f"k618_fuxi_mods: 6.12 in {rel}")
    print(f"k618_fuxi_mods: {added} modules registered, {len(done)} files in place")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
