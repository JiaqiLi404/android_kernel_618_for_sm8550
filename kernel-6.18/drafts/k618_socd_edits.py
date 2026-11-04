

# ---------------------------------------------------------------- option A: just the log level
SOCD_QUIET = [
    ("drivers/thermal/qcom/bcl_soc.c",
     "\tif (IS_ERR(bcl_perph->tz_dev)) {\n"
     "\t\tpr_err(\"soc TZ register failed. err:%ld\\n\",\n"
     "\t\t\t\tPTR_ERR(bcl_perph->tz_dev));\n",
     "\tif (IS_ERR(bcl_perph->tz_dev)) {\n"
     "\t\t/* k618: -ENODEV here is the device tree, not a regression -- nuwa's dtbo gives\n"
     "\t\t * the socd zone no thermal-sensors, so no kernel ever matches bcl-soc to it and\n"
     "\t\t * stock 5.15 prints this same err:-19. Not an error.\n"
     "\t\t */\n"
     "\t\tpr_info(\"soc TZ register skipped (no DT sensor for socd). err:%ld\\n\",\n"
     "\t\t\t\tPTR_ERR(bcl_perph->tz_dev));\n"),
]

# ---------------------------------------------------------------- option B: a standalone socd zone
SOCD_ZONE = [
    # B.1 remember that the zone is ours, so remove() unregisters it (devm does not own this one)
    ("drivers/thermal/qcom/bcl_soc.c",
     "static struct bcl_device *bcl_perph;\n",
     "static struct bcl_device *bcl_perph;\n"
     "/* k618: true when tz_dev is the standalone socd zone we registered, not a devm/DT one */\n"
     "static bool bcl_soc_own_tz;\n"),

    # B.2 tear it down again
    ("drivers/thermal/qcom/bcl_soc.c",
     "static void bcl_soc_remove(struct platform_device *pdev)\n"
     "{\n"
     "\tpower_supply_unreg_notifier(&bcl_perph->psy_nb);\n"
     "\tflush_work(&bcl_perph->soc_eval_work);\n"
     "}\n",
     "static void bcl_soc_remove(struct platform_device *pdev)\n"
     "{\n"
     "\tpower_supply_unreg_notifier(&bcl_perph->psy_nb);\n"
     "\tflush_work(&bcl_perph->soc_eval_work);\n"
     "\tif (bcl_soc_own_tz) {\n"
     "\t\t/* k618: the standalone socd zone is not devm managed */\n"
     "\t\tstruct thermal_zone_device *tz = bcl_perph->tz_dev;\n"
     "\n"
     "\t\tbcl_soc_own_tz = false;\n"
     "\t\tbcl_perph->tz_dev = NULL;\n"
     "\t\tthermal_zone_device_unregister(tz);\n"
     "\t}\n"
     "}\n"),

    # B.3 the fallback itself. Trip 90 and polling-delay-passive 100 are nuwa's own dtbo values
    # (dtbo44 socd-trip temperature = 0x5a, polling-delay-passive = 0x64, polling-delay = 0);
    # THERMAL_TRIP_PASSIVE matches the DT's type = "passive". No cooling-maps are bound, so this
    # zone reports and does not throttle -- see option C.
    ("drivers/thermal/qcom/bcl_soc.c",
     "\tif (IS_ERR(bcl_perph->tz_dev)) {\n"
     "\t\tpr_err(\"soc TZ register failed. err:%ld\\n\",\n"
     "\t\t\t\tPTR_ERR(bcl_perph->tz_dev));\n"
     "\t\tret = PTR_ERR(bcl_perph->tz_dev);\n"
     "\t\tbcl_perph->tz_dev = NULL;\n"
     "\t\tgoto bcl_soc_probe_exit;\n"
     "\t}\n",
     "\tif (PTR_ERR_OR_ZERO(bcl_perph->tz_dev) == -ENODEV) {\n"
     "\t\t/* k618: nuwa's dtbo gives the socd zone no thermal-sensors, so no kernel --\n"
     "\t\t * 5.15 included -- ever matches bcl-soc to it and every one of them returns\n"
     "\t\t * -ENODEV here. 5.15 still had a (dead, temp = -EINVAL) socd zone object\n"
     "\t\t * because its of_parse_thermal_zones() built every zone up front; 6.x only\n"
     "\t\t * builds a zone when a sensor registers one, so build it ourselves with the\n"
     "\t\t * dtbo's own numbers. Reports only: no cooling device is bound, so this does\n"
     "\t\t * not add the throttle stock never had (drafts/k618_socd_edits.py).\n"
     "\t\t */\n"
     "\t\tstatic struct thermal_trip socd_trip = {\n"
     "\t\t\t.temperature = 90,\n"
     "\t\t\t.hysteresis = 0,\n"
     "\t\t\t.type = THERMAL_TRIP_PASSIVE,\n"
     "\t\t};\n"
     "\n"
     "\t\tbcl_perph->tz_dev = thermal_zone_device_register_with_trips(\"socd\",\n"
     "\t\t\t\t&socd_trip, 1, bcl_perph, &bcl_perph->ops, NULL, 100, 0);\n"
     "\t\tif (!IS_ERR(bcl_perph->tz_dev)) {\n"
     "\t\t\tbcl_soc_own_tz = true;\n"
     "\t\t\tret = thermal_zone_device_enable(bcl_perph->tz_dev);\n"
     "\t\t\tif (ret) {\n"
     "\t\t\t\tpr_err(\"socd TZ enable failed. err:%d\\n\", ret);\n"
     "\t\t\t\tgoto bcl_soc_probe_exit;\n"
     "\t\t\t}\n"
     "\t\t\tpr_info(\"socd TZ built in the driver (the DT names no sensor for it)\\n\");\n"
     "\t\t}\n"
     "\t}\n"
     "\tif (IS_ERR(bcl_perph->tz_dev)) {\n"
     "\t\tpr_err(\"soc TZ register failed. err:%ld\\n\",\n"
     "\t\t\t\tPTR_ERR(bcl_perph->tz_dev));\n"
     "\t\tret = PTR_ERR(bcl_perph->tz_dev);\n"
     "\t\tbcl_perph->tz_dev = NULL;\n"
     "\t\tgoto bcl_soc_probe_exit;\n"
     "\t}\n"),
]

# ================================= NOTES =================================
#
# Why B.3 keys on -ENODEV and not on IS_ERR: any other error (-EPROBE_DEFER from a sensor that is
# not up, -EINVAL from a malformed zone) still has to go the old way, because then the DT *does*
# name a sensor for socd and building our own zone would give two zones of the same name.
#
# Why the devm/non-devm mix is safe: devm_thermal_of_zone_register() registers nothing on the error
# path, so there is no devm action to undo; the standalone zone is ours alone and B.2 drops it.
# bcl_soc_remove() is also the probe error path (`goto bcl_soc_probe_exit`), where own_tz is false
# unless the enable failed -- and in that one case unregistering is exactly right.
#
# APIs used, all exported in $O/Module.symvers and CONFIG_TRIM_UNUSED_KSYMS is not set:
#   thermal_zone_device_register_with_trips  0xc59146dc  EXPORT_SYMBOL_GPL
#   thermal_zone_device_enable               0x5294c3ae  EXPORT_SYMBOL_GPL
#   thermal_zone_device_unregister           0x4f597230  EXPORT_SYMBOL_GPL
# struct thermal_trip is public ($W/common/include/linux/thermal.h): temperature, hysteresis, type,
# flags, priv. bcl_soc.c already includes <linux/thermal.h>, so no include edit is needed.
#
# NOT compile-checked. These touch the soc tree, which is a full soc-repo build (not a single-module
# M= build), and the brief says no whole-tree build -- so they are drafts only. Compile them with the
# next soc build. Expected log change, option B:
#   before  bcl_soc:bcl_soc_probe soc TZ register failed. err:-19
#   after   bcl_soc:bcl_soc_probe socd TZ built in the driver (the DT names no sensor for it)
# and `grep -l socd /sys/class/thermal/thermal_zone*/type` then finds one, whose `temp` reads
# 100 - battery capacity (i.e. 90 at 10% battery). With option A, only the log line changes.
