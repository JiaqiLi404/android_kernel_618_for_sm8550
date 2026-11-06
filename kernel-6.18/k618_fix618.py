#!/usr/bin/env python3
"""k618_fix618.py <soc dir>: the 6.12 -> 6.18 API changes in the vendor sources.

Everywhere:
  - MODULE_IMPORT_NS(X) / EXPORT_SYMBOL_NS[_GPL](sym, X) take the namespace as a string since 6.13
  - struct platform_driver's .remove_new is .remove again since 6.11 (both return void)
  - nth_page() is gone in 6.18: the memmap is contiguous (SPARSEMEM_VMEMMAP), page + n
  - struct dma_buf_ops has no cache_sgt_mapping, struct dma_heap_ops no get_pool_size (an
    Android addition) in 6.18
  - pinctrl's PIN_CONFIG_OUTPUT is PIN_CONFIG_LEVEL, from_timer() is timer_container_of()

In one file each (FILE_EDITS, INSERTS): blk_crypto_key's raw[] is bytes[] since 6.15; cfs_rq
counts nr_queued since 6.14; 6.18's iommu-pages hands out zeroed memory by address and size; the
IOMMU core frees a failed probe's fwspec itself (and no longer exports iommu_fwspec_free()), and
probes all of an SMMU's devices from inside its iommu_device_register() (arm-smmu: upstream's
order, ready before registering); the
UFS core exports ufshcd_dme_rmw() and keeps enum ufs_trace_str_t in its own header; UCSI's
sync_control reads CCI and MESSAGE_IN itself; no NR_BOUNCE; OnePlus' memalloc_boost_*() (its own
kernel's) dropped.

Prints what it changed; idempotent.
"""

import os
import re
import sys

EDITS = [
    (re.compile(r"\bMODULE_IMPORT_NS\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)"), r'MODULE_IMPORT_NS("\1")'),
    (re.compile(r"\b(EXPORT_SYMBOL_NS(?:_GPL)?)\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)"), r'\1(\2, "\3")'),
    (re.compile(r"\.remove_new(\s*=)"), r".remove\1"),
    (re.compile(r"\bnth_page\(\s*([^,()]+(?:\([^()]*\))?)\s*,\s*"), r"__k618_nth_page(\1, "),
    (re.compile(r"^\s*\.cache_sgt_mapping\s*=\s*true,\n", re.M), ""),
    (re.compile(r"^\s*\.get_pool_size\s*=\s*\w+,\n", re.M), ""),
    (re.compile(r"\bPIN_CONFIG_OUTPUT\b"), "PIN_CONFIG_LEVEL"),
    (re.compile(r"\bfrom_timer\("), "timer_container_of("),
    (re.compile(r"\bdel_timer_sync\("), "timer_delete_sync("),
    (re.compile(r"\bdel_singleshot_timer_sync\("), "timer_delete_sync("),
    (re.compile(r"\bdel_timer\("), "timer_delete("),
    # the cpufreq core adds the frequency table's attributes itself (and exports them no more)
    (re.compile(r"^\t&cpufreq_freq_attr_scaling_(?:available|boost)_freqs,\n", re.M), ""),
    # console_stop() / console_start() are console_suspend() / console_resume() since 6.14
    (re.compile(r"\bconsole_stop\("), "console_suspend("),
    (re.compile(r"\bconsole_start\("), "console_resume("),
    (re.compile(r"\bSPI_CS_CNT_MAX\b"), "SPI_DEVICE_CS_CNT_MAX"),
    (re.compile(r"\bmsi_lock_descs\("), "__msi_lock_descs("),
    (re.compile(r"\bmsi_unlock_descs\("), "__msi_unlock_descs("),
    # ufshcd_config_pwr_mode() takes a policy; 6.12's changed the mode only if it differed
    (re.compile(r"\bufshcd_config_pwr_mode\(\s*([^,()]+),\s*([^,()]+)\)"),
     r"ufshcd_config_pwr_mode(\1, \2, UFSHCD_PMC_POLICY_DONT_FORCE)"),
]
NTH = "#define __k618_nth_page(page, n) ((page) + (n))\n"

# 6.18's platform irqchip drivers are probed with their platform device (and the parent's node)
IRQCHIP_MATCH = re.compile(r"IRQCHIP_MATCH\(\s*\"[^\"]+\"\s*,\s*(\w+)\s*\)")


HRT_INIT = re.compile(r"\bhrtimer_init\(\s*([^,]+?)\s*,\s*([^,]+?)\s*,\s*([^)]+?)\s*\);")


def hrtimer_setup_conv(text, rel):
    """hrtimer_init() is gone in 6.18: hrtimer_setup() takes the callback too. The callback is the one
    assigned to the same timer (deleted when it comes right after the init); a timer whose callback
    is set elsewhere, under another name, gets the file's only hrtimer callback"""
    cbs = re.findall(r"\benum hrtimer_restart (\w+)\(struct hrtimer \*", text)
    while True:
        m = HRT_INIT.search(text)
        if not m:
            return text
        t, clk, mode = m.group(1), m.group(2), m.group(3)
        lhs = re.escape(t[1:].strip()) + r"\s*\." if t.startswith("&") else re.escape(t) + r"\s*->"
        assign = re.compile(r"^[ \t]*" + lhs + r"function\s*=\s*(\w+)\s*;[ \t]*\n", re.M)
        a = assign.search(text, m.end()) or assign.search(text)
        if a:
            fn = a.group(1)
        elif len(set(cbs)) == 1:
            fn = cbs[0]
        else:
            print(f"k618_fix618: {rel}: no callback for hrtimer_init({t}, ...)", file=sys.stderr)
            fn = "NULL"
        new = f"hrtimer_setup({t}, {fn}, {clk}, {mode});"
        if a and a.start() > m.end() and text[m.end():a.start()].count("\n") <= 3:
            text = text[:m.start()] + new + text[m.end():a.start()] + text[a.end():]
        else:
            text = text[:m.start()] + new + text[m.end():]


def gpio_set_int(text):
    """gpio_chip's set() and set_multiple() return int in 6.18"""
    for fn in sorted(set(re.findall(r"\.set(?:_multiple)?\s*=\s*(\w+)\s*[,;]", text))):
        m = re.search(rf"^static void {fn}\((struct gpio_chip \*[^)]*)\)\n\{{\n", text, re.M)
        if not m:
            continue
        end = text.index("\n}\n", m.end())
        body = re.sub(r"\breturn;", "return 0;", text[m.end():end])
        if not re.search(r"\n\treturn [^;]+;\s*$", body):
            body += "\n\n\treturn 0;"
        text = text[:m.start()] + f"static int {fn}({m.group(1)})\n{{\n" + body + text[end:]
    return text


def psy_cfg_fwnode(text):
    """struct power_supply_config has a fwnode instead of an of_node"""
    for v in set(re.findall(r"struct power_supply_config\s+(\w+)\s*(?:=|;)", text)):
        text = re.sub(rf"\b{v}\.of_node\s*=\s*([^;]+);", rf"{v}.fwnode = of_fwnode_handle(\1);", text)
    return text


IDA_SIMPLE = """/* 6.18 has no ida_simple_*(): the same with ida_alloc_*() */
static inline int __k618_ida_simple_get(struct ida *ida, unsigned int start, unsigned int end, gfp_t gfp)
{
	return end ? ida_alloc_range(ida, start, end - 1, gfp) : ida_alloc_min(ida, start, gfp);
}
"""


def irqchip_probe(text):
    for fn in sorted(set(IRQCHIP_MATCH.findall(text))):
        text = re.sub(rf"(static int (?:__init )?){fn}\(struct device_node \*(\w+),\s*struct device_node \*(\w+)\)\n\{{\n",
                      lambda m: f"{m.group(1)}{fn}(struct platform_device *pdev, struct device_node *{m.group(3)})\n"
                                f"{{\n\tstruct device_node *{m.group(2)} = pdev->dev.of_node;\n", text)
    return text

FILE_EDITS = [
    ("drivers/soc/qcom/ice.c", re.compile(r"key->raw,"), "key->bytes,"),
    ("drivers/ufs/host/ufshcd-crypto-qti.c", re.compile(r"key->raw\b"), "key->bytes"),
    ("drivers/mmc/host/cqhci-crypto.c", re.compile(r"key->raw\b"), "key->bytes"),
    # 6.18 exports a sections_per_block of its own (the same value): this one is the driver's
    ("drivers/soc/qcom/mem-offline.c", re.compile(r"\bsections_per_block\b"), "mem_offline_sections_per_block"),
    # boost is the core's business in 6.18 (it finds the CPUFREQ_BOOST_FREQ entries itself): the
    # driver gives set_boost, as upstream's does, instead of enabling it
    ("drivers/cpufreq/qcom-cpufreq-hw.c",
     re.compile(r"\tif \(policy_has_boost_freq\(policy\)\) \{\n\t\tret = cpufreq_enable_boost_support\(\);\n"
                r"\t\tif \(ret\)\n\t\t\tdev_warn\(cpu_dev, \"failed to enable boost: %d\\n\", ret\);\n\t\}\n\n"), ""),
    ("drivers/cpufreq/qcom-cpufreq-hw.c", re.compile(r"(\t\.boost_enabled\t= true,\n)(?!\t\.set_boost)"),
     r"\1\t.set_boost\t= cpufreq_boost_set_sw,\n"),
    # the CMA best-fit hook passes the area's vendor data as it is (unsigned long, not bool)
    ("drivers/soc/qcom/mem-hooks.c", re.compile(r"(unsigned long \*bitmap_no,\s*)bool best_fit\)"),
     r"\1unsigned long best_fit)"),
    ("drivers/soc/qcom/minidump_log.c", re.compile(r"\b(my_q|cfs)->nr_running\b"), r"\1->nr_queued"),
    ("drivers/soc/qcom/minidump_memory.c", re.compile(r"global_zone_page_state\(NR_BOUNCE\)"), "0UL"),
    ("drivers/soc/qcom/minidump_memory.c", re.compile(r"global_node_page_state\(NR_WRITEBACK_TEMP\)"), "0UL"),
    # page->flags is a memdesc_flags_t in 6.18
    ("drivers/soc/qcom/minidump_memory.c", re.compile(r"\bpage->flags &"), "page->flags.f &"),
    ("drivers/soc/qcom/minidump_memory.c", re.compile(r"\bget_dmabuf_debugfs_data\("), "__k618_get_dmabuf_debugfs_data("),
    # gpio_chip's .set returns int in 6.18
    ("drivers/pinctrl/qcom/pinctrl-msm.c",
     re.compile(r"static void msm_gpio_set\((struct gpio_chip \*chip, unsigned offset, int value\)\n\{.*?"
                r"\n\traw_spin_unlock_irqrestore\(&pctrl->lock, flags\);\n)\}", re.S),
     r"static int msm_gpio_set(\1\n\treturn 0;\n}"),
    # 6.18's iommu_domain_ops has no enable_nesting (VFIO's type1 nesting went), its iommu_ops no
    # pgsize_bitmap: a new domain gets all SMMUs' page sizes from here, as 6.12's core gave it
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     re.compile(r"^\t+\.enable_nesting\s*=\s*arm_smmu_enable_nesting,\n", re.M), ""),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c", re.compile(r"^\t+\.pgsize_bitmap\s*=\s*-1UL,.*\n", re.M), ""),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     re.compile(r"arm_smmu_ops\.iommu_ops\.pgsize_bitmap"), "k618_arm_smmu_pgsize_bitmap"),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     re.compile(r"(\tsmmu_domain->secure_vmid = VMID_INVAL;\n)(\n\treturn &smmu_domain->domain;\n)"),
     r"\1\tsmmu_domain->domain.pgsize_bitmap = k618_arm_smmu_pgsize_bitmap;\n\2"),
    # OnePlus turns its modem crash-reason code on for all remoteproc drivers here (it calls
    # OnePlus' mdmreason_set()): off, Qualcomm's code
    ("drivers/remoteproc/qcom_common.h",
     re.compile(r"#ifndef  ?OPLUS_FEATURE_MODEM_MINIDUMP\n#define OPLUS_FEATURE_MODEM_MINIDUMP\n#endif\n\n"), ""),
    # OnePlus' debug hook without even a commented guard
    ("drivers/ufs/host/ufs-qcom.c",
     re.compile(r"(?<!CONFIG_OPLUS_UFS_DRIVER\)\n)\tufs_oplus_init_sdev\(sdev\);\n"),
     "#if IS_ENABLED(CONFIG_OPLUS_UFS_DRIVER)\n\tufs_oplus_init_sdev(sdev);\n#endif\n"),
    # struct ufs_hba lost res[] (kept in struct ufs_qcom_host, see UFS_RES) and ufs_stats its last
    # interrupt's time and status; the core has no 5 ms delay after VCC off any more, the only
    # thing Toshiba's entry asked for
    ("drivers/ufs/host/ufs-qcom.c", re.compile(r"\bhba->res\b"), "ufs_qcom_res(hba)"),
    ("drivers/ufs/host/ufs-qcom.c",
     re.compile(r"\tdev_err\(hba->dev, \"last intr at %lld us, last intr status=0x%x\\n\",\n"
                r"\t\tktime_to_us\(hba->ufs_stats\.last_intr_ts\),\n\t\thba->ufs_stats\.last_intr_status\);\n"), ""),
    ("drivers/ufs/host/ufs-qcom.c",
     re.compile(r"\t\{ \.wmanufacturerid = UFS_VENDOR_TOSHIBA,\n\t  \.model = UFS_ANY_MODEL,\n"
                r"\t  \.quirk = UFS_DEVICE_QUIRK_DELAY_AFTER_LPM \},\n"), ""),
    ("drivers/ufs/host/ufs-qcom.c", re.compile(r"&hba->scsi_block_reqs_cnt\b"), "ufs_qcom_block_reqs_cnt(hba)"),
    # the core has UFS_DEVICE_QUIRK_PA_HIBER8TIME itself now (another bit) and adds the 100 us
    # itself: Qualcomm's own bit and its second +100 us go
    ("drivers/ufs/host/ufs-qcom.h",
     re.compile(r"/\*\n \* Some ufs devices may need more time to be in hibern8 before exiting\.\n"
                r" \* Enable this quirk to give it an additional 100us\.\n \*/\n"
                r"#define UFS_DEVICE_QUIRK_PA_HIBER8TIME +\(1 << 15\)\n\n"), ""),
    ("drivers/ufs/host/ufs-qcom.c",
     re.compile(r"\tif \(hba->dev_quirks & UFS_DEVICE_QUIRK_PA_HIBER8TIME\)\n\t\tufs_qcom_override_pa_h8time\(hba\);\n\n"), ""),
    # 6.18 negotiates the power mode in .negotiate_pwr_mode and only tells .pwr_change_notify the
    # result (UFS_PWR_618); .program_key is gone -- with CONFIG_SCSI_UFS_CRYPTO_QTI (ours) the
    # crypto profile, keys included, is ufshcd-crypto-qti's own
    ("drivers/ufs/host/ufs-qcom.c", re.compile(r"\t\.pwr_change_notify\t= ufs_qcom_pwr_change_notify,\n"),
     "\t.negotiate_pwr_mode\t= ufs_qcom_negotiate_pwr_mode_618,\n"
     "\t.pwr_change_notify\t= ufs_qcom_pwr_change_notify_618,\n"),
    ("drivers/ufs/host/ufs-qcom.c", re.compile(r"^\t\.program_key\t\t= ufs_qcom_ice_program_key,\n", re.M), ""),
    ("drivers/dma-buf/heaps/qcom_system_heap.c",
     re.compile(r"^\t*memalloc_boost_(?:save|restore)\(\);\n", re.M), ""),
    # the SCMI core header that comes in with memlat_vendor (k618_fuxi_mods.py imports both from
    # Xiaomi's 5.15 tree) is the 5.15 one: its struct scmi_protocol has six fields where this
    # kernel's (drivers/firmware/arm_scmi/protocols.h) has ten -- the same ten in 6.12 and 6.18.
    # The SCMI core is the kernel's own and built in, so a vendor protocol module built against the
    # short header hands scmi_protocol_register() a struct four fields too small and the core reads
    # vendor_id past its end: on 6.12 memlat_vendor was refused with -EINVAL ("missing vendor_id for
    # protocol 0x80") and memlat then deferred for ever. The four are added in the kernel's order;
    # k618_fuxi_mods.py fills in memlat_vendor's own vendor_id, as the vendor protocols beside it
    # (pmu, c1dcvs, plh, qcom_scmi_vendor) already do.
    ("drivers/firmware/arm_scmi/common.h",
     re.compile(r"(struct scmi_protocol \{\n(?:\t[^\n]*\n)+?\tconst struct scmi_protocol_events\t\*events;\n)\};"),
     "\\1\tunsigned int\t\t\t\tsupported_version;\n\tchar\t\t\t\t\t*vendor_id;\n"
     "\tchar\t\t\t\t\t*sub_vendor_id;\n\tu32\t\t\t\t\timpl_ver;\n};"),

    # ---- the second-stage modules' own interface changes (none of these files is in the first
    # ---- stage, so they first showed when the whole module set was built)
    # a connector's .mode_valid takes the mode as const since 6.15
    ("drivers/gpu/drm/bridge/lt9611uxc.c",
     re.compile(r"(static enum drm_mode_status lt9611uxc_connector_mode_valid\(\n"
                r"\tstruct drm_connector \*connector, )struct drm_display_mode \*drm_mode\)"),
     r"\1const struct drm_display_mode *drm_mode)"),
    # ...and drm_bridge_funcs.attach is told which encoder it is attached to (6.16)
    ("drivers/gpu/drm/bridge/lt9611uxc.c",
     re.compile(r"static int lt9611uxc_bridge_attach\(struct drm_bridge \*bridge, "
                r"enum drm_bridge_attach_flags flags\)"),
     "static int lt9611uxc_bridge_attach(struct drm_bridge *bridge, struct drm_encoder *encoder,\n"
     "\t\t\t\t   enum drm_bridge_attach_flags flags)"),
    # i3c_master_controller_ops.i2c_xfers lost the const on its messages
    ("drivers/i3c/master/i3c-master-msm-geni.c",
     re.compile(r"(geni_i3c_master_i2c_xfers\(struct i2c_dev_desc \*dev, )const (struct i2c_msg \*msgs, int num\))"),
     r"\1\2"),
    # __calc_vm_flag_bits() is calc_vm_flag_bits() since 6.17 (same arguments)
    ("drivers/input/misc/qcom-hv-haptics.c",
     re.compile(r"\b__calc_vm_flag_bits\("), "calc_vm_flag_bits("),


    ("drivers/slimbus/core.c",
     re.compile(r"static int (slim_match_dev|of_slim_match_dev)\(struct device \*dev, void \*data\)"),
     r"static int \1(struct device *dev, const void *data)"),
    ("drivers/slimbus/core.c",
     re.compile(r"\tstruct slim_eaddr \*e_addr = data;"), "\tconst struct slim_eaddr *e_addr = data;"),
    ("drivers/slimbus/core.c",
     re.compile(r"\tstruct device_node \*np = data;"), "\tconst struct device_node *np = data;"),
    ("drivers/slimbus/core.c",
     re.compile(r"return slim_eaddr_equal\(&sbdev->e_addr, e_addr\);"),
     "return slim_eaddr_equal(&sbdev->e_addr, (struct slim_eaddr *)e_addr);"),
    # driver_override moved out of struct platform_device into struct device, where it is a small
    # struct with its own lock and may only be set through the helper -- which is what the kernel's
    # own copy of this driver now does (6.18 drivers/slimbus/qcom-ngd-ctrl.c:1542)
    ("drivers/slimbus/qcom-ngd-ctrl.c",
     re.compile(r"driver_set_override\(&ngd->pdev->dev,\n\s*&ngd->pdev->driver_override,\n"
                r"\s*QCOM_SLIM_NGD_DRV_NAME,\n\s*strlen\(QCOM_SLIM_NGD_DRV_NAME\)\)"),
     "device_set_driver_override(&ngd->pdev->dev, QCOM_SLIM_NGD_DRV_NAME)"),
    # a bin_attribute's read/write take it as const, and an attribute group's bin_attrs is a
    # const array of const pointers (6.15/6.16)
    ("drivers/soc/qcom/qti_battery_debug.c",
     re.compile(r"(static ssize_t qbg_blob_(?:read|write)\(struct file \*filp, struct kobject \*kobj,\n"
                r"\s*)struct bin_attribute \*attr"), r"\1const struct bin_attribute *attr"),
    ("drivers/soc/qcom/qti_battery_debug.c",
     re.compile(r"static struct bin_attribute \*battery_dbg_bin_attrs\[\] = \{"),
     "static const struct bin_attribute *const battery_dbg_bin_attrs[] = {"),
    # hdmi_codec_ops' no_capture_mute now sits in hdmi_codec_pdata, and get_dai_id takes the
    # driver's own data as a third argument
    ("sound/soc/codecs/hdmi-codec.c",
     re.compile(r"!hcp->hcd\.ops->no_capture_mute"), "!hcp->hcd.no_capture_mute"),
    ("sound/soc/codecs/hdmi-codec.c",
     re.compile(r"hcp->hcd\.ops->get_dai_id\(component, endpoint\)"),
     "hcp->hcd.ops->get_dai_id(component, endpoint, hcp->hcd.data)"),
    # iommu_domain_alloc(bus) is iommu_paging_domain_alloc(dev) since 6.12/6.13, and it returns
    # an error pointer instead of NULL
    ("sound/usb/usb_audio_qmi_svc.c",
     re.compile(r"(\t\w+->domain = )iommu_domain_alloc\(pdev->dev\.bus\);"),
     r"\1iommu_paging_domain_alloc(&pdev->dev);\n"
     r"\tif (IS_ERR(uaudio_qdev->domain))\n\t\tuaudio_qdev->domain = NULL;"),
    # pci-msm: of_pci_parse_bus_range() is private to the PCI core in 6.18 (drivers/pci/of.c), so
    # the bus-range property is read here instead (K618_BUS_RANGE), and struct pci_dev's aer_stats
    # is private too -- the driver only reads the core's AER counters for its own sysfs, so those
    # go through a NULL pointer, which every one of its users already checks for
    ("drivers/pci/controller/pci-msm.c",
     re.compile(r"if \(!of_pci_parse_bus_range\(dev->pdev->dev\.of_node, &res\)\)"),
     "if (!k618_of_pci_bus_range(dev->pdev->dev.of_node, &res))"),
    ("drivers/pci/controller/pci-msm.c",
     re.compile(r"(\t\t\tpcie_dev->aer_stats = )pcidev->aer_stats;"),
     r"\1NULL;\t/* 6.18: the core keeps its AER counters to itself */"),
    ("drivers/pci/controller/pci-msm.c",
     re.compile(r"\tstruct aer_stats \*aer_stats = pdev->aer_stats;"),
     "\tstruct aer_stats *aer_stats = NULL;\t/* 6.18: private to the PCI core */"),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     re.compile(r"(out_free:\n)\tiommu_fwspec_free\(dev\);\n"), r"\1"),


    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     re.compile(r"driver_find_device_by_fwnode\(&arm_smmu_driver\.driver,\s*fwnode\)"),
     "bus_find_device_by_fwnode(&platform_bus_type, fwnode)"),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     re.compile(r"(\terr = iommu_device_sysfs_add\(&smmu->iommu, smmu->dev, NULL,\n"
                r"(?:.*\n)*?\t\tdev_err\(dev, \"Failed to register iommu\\n\"\);\n"
                r"\t\tgoto remove_iommu_sysfs_node;\n\t\}\n\n)"
                r"(\tplatform_set_drvdata\(pdev, smmu\);\n\n"
                r"\t/\* Check for RMRs and install bypass SMRs if any \*/\n"
                r"\tarm_smmu_rmr_install_bypass_smr\(smmu\);\n\n"
                r"\tarm_smmu_device_reset\(smmu\);\n"
                r"\tarm_smmu_test_smr_masks\(smmu\);\n"
                r"\tarm_smmu_interrupt_selftest\(smmu\);\n\n)"),
     r"\t/* ready before iommu_device_register() probes the devices behind it (6.18) */\n\2\1"),
    # lazy_attach=1 (a module parameter, INSERTS): devices no driver has matched yet are left out
    # when the SMMU registers and attached when their driver probes, as on 6.12
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     re.compile(r"(static struct iommu_device \*arm_smmu_probe_device\(struct device \*dev\)\n\{\n"
                r"(?:\t.*\n)*?\tint i, ret;\n\n)(\tif \(using_legacy_binding\) \{)"),
     r"\1\tif (lazy_attach && !dev->driver && !dev->can_match)\n\t\treturn ERR_PTR(-ENODEV);\n\n\2"),
    ("drivers/iommu/io-pgtable-fast.c",
     re.compile(r"__iommu_alloc_pages\(\(GFP_KERNEL \| __GFP_ZERO\), 0\)"), "__k618_iommu_alloc_page(GFP_KERNEL)"),
    ("drivers/iommu/io-pgtable-fast.c",
     re.compile(r"__iommu_free_pages\(([^,()]+), 0\)"), r"__k618_iommu_free_page(\1)"),
    ("drivers/iommu/qcom-io-pgtable-alloc.c",
     re.compile(r"__iommu_alloc_pages\(([^,()]+), 0\)"), r"__k618_iommu_alloc_page(\1)"),
    ("drivers/iommu/qcom-io-pgtable-alloc.c",
     re.compile(r"__iommu_free_pages\(([^,()]+), 0\)"), r"__k618_iommu_free_page(\1)"),
    ("drivers/ufs/host/ufs-qcom.h",
     re.compile(r"/\*\*\n \* ufshcd_dme_rmw - .*?\n}\n\n", re.S), ""),
    ("drivers/usb/typec/ucsi/ucsi_qti_glink.c",
     re.compile(r"\.sync_control = ucsi_qti_sync_control,"), ".sync_control = ucsi_qti_sync_control_618,"),
    # 6.18.32: dwc3_ep has no regs any more; DWC3_DEPCMDPAR0-2 and DWC3_DEPCMD take the endpoint
    # number and carry DWC3_DEP_BASE themselves. The same addresses: dep->regs was
    # dwc->regs + DWC3_DEP_BASE(dep->number)
    ("drivers/usb/dwc3/dwc3-msm-core.c",
     re.compile(r"dwc3_msm_ep_(writel|readl)\(dep->regs, (DWC3_DEPCMDPAR[0-2]|DWC3_DEPCMD)([,)])"),
     r"dwc3_msm_ep_\1(dep->dwc->regs, \2(dep->number)\3"),
    ("drivers/usb/dwc3/dwc3-msm-core.c",
     re.compile(r"DWC3_DEP_BASE\(dep->number\) \+ DWC3_DEPCMD\)"), "DWC3_DEPCMD(dep->number))"),
]

IOMMU_PAGE = """/* 6.18's iommu-pages: zeroed memory by its address, the size in bytes */
static struct page *__k618_iommu_alloc_page(gfp_t gfp)
{
	void *virt = iommu_alloc_pages_sz(gfp, PAGE_SIZE);

	return virt ? virt_to_page(virt) : NULL;
}

static void __k618_iommu_free_page(struct page *page)
{
	if (page)
		iommu_free_pages(page_address(page));
}
"""

UCSI_SYNC = """/*
 * 6.18's UCSI core leaves reading CCI and MESSAGE_IN after a blocking command to .sync_control
 * (as ucsi_sync_control_common() does for the drivers that wait on the core's completion; this
 * one waits for the PMIC's acknowledgement of the write)
 */
static int ucsi_qti_sync_control_618(struct ucsi *ucsi, u64 command, u32 *cci,
				     void *data, size_t size)
{
	int ret = ucsi_qti_sync_control(ucsi, command);

	if (!ret && cci)
		ret = ucsi_qti_read_cci(ucsi, cci);
	if (!ret && data && cci && (*cci & UCSI_CCI_COMMAND_COMPLETE))
		ret = ucsi_qti_read_message_in(ucsi, data, size);
	return ret;
}

"""

DMABUF_ITER = """/* 6.18 walks all dma-bufs with dma_buf_iter_begin() / _next() (6.12's Android kernel had this) */
static int __k618_get_dmabuf_debugfs_data(int (*fn)(const struct dma_buf *, void *), void *priv)
{
	struct dma_buf *dmabuf;
	int ret = 0;

	for (dmabuf = dma_buf_iter_begin(); dmabuf; dmabuf = dma_buf_iter_next(dmabuf)) {
		ret = fn(dmabuf, priv);
		if (ret) {
			dma_buf_put(dmabuf);
			break;
		}
	}
	return ret;
}
"""

SMMU_PGSIZE = """/*
 * 6.18's iommu_ops has no pgsize_bitmap: the page sizes of all SMMUs, which 6.12's core gave a
 * new domain (attaching it restricts them to its own SMMU's)
 */
static unsigned long k618_arm_smmu_pgsize_bitmap = -1UL;

"""

SMMU_LAZY = """
/*
 * 6.18's iommu_device_register() probes and attaches every device behind the SMMU at once, drivers
 * or not; lazy_attach=1 leaves out those no driver has matched yet, which are attached when their
 * driver probes instead, as 6.12 did
 */
static bool lazy_attach;
module_param(lazy_attach, bool, 0444);
"""

UFS_RES = """/*
 * 6.18's struct ufs_hba has no res[] (Qualcomm's driver was its only user): the register regions
 * are kept in struct ufs_qcom_host instead, as 6.12's core defined them
 */
enum ufshcd_res {
	RES_UFS,
	RES_MCQ,
	RES_MCQ_SQD,
	RES_MCQ_SQIS,
	RES_MCQ_CQD,
	RES_MCQ_CQIS,
	RES_MCQ_VS,
	RES_MAX,
};

struct ufshcd_res_info {
	const char *name;
	struct resource *resource;
	void __iomem *base;
};

#define ufs_qcom_res(hba) (((struct ufs_qcom_host *)ufshcd_get_variant(hba))->res)
/* nor the count of scsi_block_requests() made: only this driver still makes them */
#define ufs_qcom_block_reqs_cnt(hba) (&((struct ufs_qcom_host *)ufshcd_get_variant(hba))->scsi_block_reqs_cnt)

"""
UFS_RES_MEMBER = ("\t/* 6.18: what struct ufs_hba no longer has, see enum ufshcd_res */\n"
                  "\tstruct ufshcd_res_info res[RES_MAX];\n\tatomic_t scsi_block_reqs_cnt;\n")

UFS_PWR_618 = """/*
 * 6.18 asks for the power mode in .negotiate_pwr_mode and tells .pwr_change_notify the result:
 * the negotiation of Qualcomm's PRE_CHANGE, which (done again there on the agreed values) leaves
 * them as they are
 */
static int ufs_qcom_negotiate_pwr_mode_618(struct ufs_hba *hba,
					   const struct ufs_pa_layer_attr *desired,
					   struct ufs_pa_layer_attr *final)
{
	struct ufs_qcom_host *host = ufshcd_get_variant(hba);
	struct ufs_pa_layer_attr dev_max = *desired;

	return ufs_qcom_get_pwr_dev_param(&host->host_pwr_cap, &dev_max, final);
}

static int ufs_qcom_pwr_change_notify_618(struct ufs_hba *hba,
					  enum ufs_notify_change_status status,
					  struct ufs_pa_layer_attr *final)
{
	struct ufs_pa_layer_attr dev_max = *final;

	return ufs_qcom_pwr_change_notify(hba, status, &dev_max, final);
}

"""

# (path ending, anchor, text, where): text put after (or before) the anchor, once; anchor None:
# after the first block of #include lines; a pattern: at the end of its match
K618_BUS_RANGE = """/*
 * 6.18 keeps of_pci_parse_bus_range() inside the PCI core (drivers/pci/of.c), so the one property
 * this driver wants of it is read here: "bus-range" is two cells, first and last bus number.
 */
static int k618_of_pci_bus_range(struct device_node *node, struct resource *res)
{
	u32 bus_range[2];

	if (of_property_read_u32_array(node, "bus-range", bus_range, 2))
		return -EINVAL;
	res->flags = IORESOURCE_BUS;
	res->start = bus_range[0];
	res->end = bus_range[1];
	return 0;
}

"""

INSERTS = [
    ("drivers/pci/controller/pci-msm.c", True, K618_BUS_RANGE, "after"),
    ("drivers/ufs/host/ufs-qcom.h", "struct ufs_qcom_host {\n", UFS_RES, "before"),
    ("drivers/ufs/host/ufs-qcom.h", re.compile(r"^struct ufs_qcom_host \{\n(?:.*\n)*?(?=^\};\n)", re.M),
     UFS_RES_MEMBER, "after"),
    ("drivers/ufs/host/ufs-qcom.c", re.compile(r"^static int ufs_qcom_pwr_change_notify\(struct ufs_hba \*hba,\n"
                                               r"(?:.*\n)*?^\}\n\n", re.M), UFS_PWR_618, "after"),
    ("drivers/ufs/host/ufs-qcom-trace.h", "#include <linux/tracepoint.h>\n",
     "/* 6.18 keeps enum ufs_trace_str_t in the UFS core's own header */\n"
     "#include \"drivers/ufs/core/ufs_trace_types.h\"\n", "after"),
    ("drivers/iommu/io-pgtable-fast.c", None, IOMMU_PAGE, "after"),
    ("drivers/iommu/qcom-io-pgtable-alloc.c", None, IOMMU_PAGE, "after"),
    ("drivers/usb/typec/ucsi/ucsi_qti_glink.c", "static const struct ucsi_operations ucsi_qti_ops = {\n",
     UCSI_SYNC, "before"),
    ("drivers/soc/qcom/minidump_memory.c", None, DMABUF_ITER, "after"),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     "static struct iommu_domain *arm_smmu_domain_alloc_paging(struct device *dev)\n", SMMU_PGSIZE, "before"),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c", "static struct platform_driver arm_smmu_driver;\n", SMMU_LAZY, "after"),
    # 6.18 exports dma_heap_add() and its kin in a namespace of their own
    ("drivers/dma-buf/heaps/qcom_dma_heap.c", "MODULE_LICENSE(\"GPL\");\n",
     "MODULE_IMPORT_NS(\"DMA_BUF_HEAP\");\n", "after"),
]

# OnePlus marks its changes with commented-out guards (//#ifdef OPLUS_FEATURE_X ... //#endif) that
# its build ignores. Where they only wrap OnePlus' additions they are made real again (the option
# is off, so the code is Qualcomm's); not everywhere: some wrap lines that replaced Qualcomm's
# (left commented out), others code moved into a header
RESTORE_GUARDS = [
    ("drivers/ufs/host/ufs-qcom.c", "CONFIG_OPLUS_UFS_DRIVER"),
    # the modem log buffer size comes from a OnePlus module that was not published (md_buffersize)
    ("drivers/soc/qcom/memshare/msm_memshare.c", "OPLUS_FEATURE_MDLOG_BUFFERSIZE"),
]
CMT_IF = re.compile(r"^(\s*)//\s*(#\s*if(?:n?def)?\b.*)$")
CMT_ELSE = re.compile(r"^\s*//\s*#\s*(?:else|elif)\b")
CMT_ENDIF = re.compile(r"^(\s*)//\s*(#\s*endif\b.*)$")


def restore_guards(text, rel, opt):
    out, depth = [], 0
    for line in text.split("\n"):
        m = CMT_IF.match(line)
        if m and (depth or opt in m.group(2)):
            out.append(m.group(1) + m.group(2))
            depth += 1
            continue
        if depth:
            if CMT_ELSE.match(line):
                print(f"k618_fix618: {rel}: a commented #else in OnePlus' guard, left alone", file=sys.stderr)
                return text
            m = CMT_ENDIF.match(line)
            if m:
                out.append(m.group(1) + m.group(2))
                depth -= 1
                continue
        out.append(line)
    if depth:
        print(f"k618_fix618: {rel}: OnePlus' guard not closed, left alone", file=sys.stderr)
        return text
    return "\n".join(out)


def after_last_include(text, add):
    incs = list(re.finditer(r"^#include [<\"][^\n]+\n", text, re.M))
    at = incs[-1].end() if incs else 0
    return text[:at] + add + text[at:]


def after_first_includes(text, add):
    """after the first run of #include lines (blank lines and comments between them allowed):
    a late #include may sit inside an #if, or after the code that needs what is added"""
    first = re.search(r"^#include [<\"]", text, re.M)
    if not first:
        return add + text
    at = pos = first.start()
    for line in text[first.start():].split("\n"):
        s = line.strip()
        if s.startswith("#include"):
            at = pos + len(line) + 1
        elif s and not s.startswith(("//", "/*", "*")):
            break
        pos += len(line) + 1
    return text[:at] + add + text[at:]


def fix_file(path, rel):
    text = open(path, encoding="utf-8", errors="surrogateescape").read()
    new = text
    for pat, rep in EDITS:
        new = pat.sub(rep, new)
    if "IRQCHIP_MATCH(" in new:
        new = irqchip_probe(new)
    if "hrtimer_init(" in new:
        new = hrtimer_setup_conv(new, rel)
    if "struct gpio_chip" in new or "gpio_chip." in new or "->chip.set" in new:
        new = gpio_set_int(new)
    if "struct power_supply_config" in new:
        new = psy_cfg_fwnode(new)
    if "ida_simple_get(" in new or "ida_simple_remove(" in new:
        new = re.sub(r"\bida_simple_remove\(", "ida_free(", new)
        new = re.sub(r"\bida_simple_get\(", "__k618_ida_simple_get(", new)
        if "__k618_ida_simple_get(" in new and IDA_SIMPLE not in new:
            new = after_first_includes(new, IDA_SIMPLE)
    if "__k618_nth_page(" in new:
        # after the FIRST run of #include lines, not the last one: a file whose later #include sits
        # below the code (drivers/scsi/sg.c has one at line 1648) would get the helper after its
        # own first use, and clang then reads it as an implicit function call. A copy left in the
        # wrong place by an earlier run of this script is moved (the kernel files soc-repo copies in
        # are not restored by git, so they keep what the last run wrote).
        if NTH in new and new.index(NTH) > new.index("__k618_nth_page("):
            new = new.replace("/* 6.18 has no nth_page(): the memmap is contiguous */\n" + NTH, "")
            new = new.replace(NTH, "")
        if NTH not in new:
            new = after_first_includes(new, "/* 6.18 has no nth_page(): the memmap is contiguous */\n" + NTH)
    for end, opt in RESTORE_GUARDS:
        if rel.endswith(end):
            new = restore_guards(new, rel, opt)
    for end, pat, rep in FILE_EDITS:
        if rel.endswith(end):
            new = pat.sub(rep, new)
    for end, anchor, add, where in INSERTS:
        if not rel.endswith(end) or add in new:
            continue
        if anchor is None:
            new = after_first_includes(new, add)
        elif anchor is True:
            # after every #include of the file: for an addition that uses what a late include
            # declares (pci-msm.c includes linux/of.h well below the first run)
            new = after_last_include(new, add)
        elif isinstance(anchor, re.Pattern):
            m = anchor.search(new)
            if m:
                new = new[:m.end()] + add + new[m.end():]
            else:
                print(f"k618_fix618: {rel}: pattern not found: {anchor.pattern[:40]!r}", file=sys.stderr)
        elif anchor in new:
            i = new.index(anchor)
            at = i + len(anchor) if where == "after" else i
            new = new[:at] + add + new[at:]
        else:
            print(f"k618_fix618: {rel}: anchor not found: {anchor.strip()!r}", file=sys.stderr)
    if new != text:
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
        return True
    return False


def main(argv):
    soc = argv[1].rstrip("/")
    changed = []
    for dp, _dn, fns in os.walk(soc):
        if "/.git" in dp or dp.endswith("/gen"):
            continue
        for fn in fns:
            p = os.path.join(dp, fn)
            # (soc-repo has links into trees that are not part of it: left alone)
            if fn.endswith((".c", ".h")) and os.path.isfile(p) and not os.path.islink(p):
                rel = os.path.relpath(p, soc)
                if fix_file(p, rel):
                    changed.append(rel)
    print(f"k618_fix618: {len(changed)} files changed")
    for c in changed[:40]:
        print("  ", c)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
