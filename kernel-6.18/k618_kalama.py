#!/usr/bin/env python3
"""k618_kalama.py <fuxi tree> <soc tree>: kalama's (SM8550's) SoC drivers -- clock controllers,
TLMM pinctrl, the NoC interconnect, the UFS PHY -- taken from Xiaomi's own 13 Pro tree (MiCode
fuxi-t-oss, 5.15, on Qualcomm's LA.VENDOR.13.2.0.r1-07300-KAILUA.0) into soc-repo, which (as
OnePlus published it for 6.12) has no kalama: the files, their Kconfig entries (and ARCH_KALAMA),
and modules.bzl registrations cloned from the matching sun (SM8750) or waipio module. The DT of the
phone is downstream (qcom,kalama-* compatibles, Xiaomi's numbering in the dt-bindings headers), so
these, not upstream's sm8550 drivers, are what it needs. Idempotent; prints what it did."""

import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import k618_soc_kbuild as kb  # noqa: E402

FILES = [
    "drivers/clk/qcom/gcc-kalama.c",
    "drivers/clk/qcom/camcc-kalama.c",
    "drivers/clk/qcom/dispcc-kalama.c",
    "drivers/clk/qcom/gpucc-kalama.c",
    "drivers/clk/qcom/tcsrcc-kalama.c",
    "drivers/clk/qcom/videocc-kalama.c",
    "drivers/clk/qcom/debugcc-kalama.c",
    "include/dt-bindings/clock/qcom,gcc-kalama.h",
    "include/dt-bindings/clock/qcom,camcc-kalama.h",
    "include/dt-bindings/clock/qcom,dispcc-kalama.h",
    "include/dt-bindings/clock/qcom,gpucc-kalama.h",
    "include/dt-bindings/clock/qcom,tcsrcc-kalama.h",
    "include/dt-bindings/clock/qcom,videocc-kalama.h",
    "drivers/pinctrl/qcom/pinctrl-kalama.c",
    "drivers/interconnect/qcom/kalama.c",
    "include/dt-bindings/interconnect/qcom,kalama.h",
    "drivers/phy/qualcomm/phy-qcom-ufs-qmp-v4-kalama.c",
    "drivers/phy/qualcomm/phy-qcom-ufs-qmp-v4-kalama.h",
]

# (Kconfig file in both trees, option): Xiaomi's entry as it is
KCONFIG = [
    ("drivers/clk/qcom/Kconfig", "SM_GCC_KALAMA"),
    ("drivers/clk/qcom/Kconfig", "SM_CAMCC_KALAMA"),
    ("drivers/clk/qcom/Kconfig", "SM_DISPCC_KALAMA"),
    ("drivers/clk/qcom/Kconfig", "SM_GPUCC_KALAMA"),
    ("drivers/clk/qcom/Kconfig", "SM_TCSRCC_KALAMA"),
    ("drivers/clk/qcom/Kconfig", "SM_VIDEOCC_KALAMA"),
    ("drivers/clk/qcom/Kconfig", "SM_DEBUGCC_KALAMA"),
    ("drivers/pinctrl/qcom/Kconfig", "PINCTRL_KALAMA"),
    ("drivers/interconnect/qcom/Kconfig", "INTERCONNECT_QCOM_KALAMA"),
]
# entries Xiaomi's tree has in another form (one option for all v4 UFS PHYs there)
KCONFIG_NEW = [
    ("arch/arm64/Kconfig.platforms", "ARCH_KALAMA", """config ARCH_KALAMA
	bool "Enable support for Qualcomm Technologies, Inc. Kalama"
	depends on ARCH_QCOM
	help
	  This enables support for Qualcomm Technologies, Inc. Kalama
	  chipset (SM8550). If you do not wish to build a kernel that runs on
	  this chipset or if you are unsure, say 'N' here.
"""),
    ("drivers/phy/qualcomm/Kconfig", "PHY_QCOM_UFS_V4_KALAMA", """config PHY_QCOM_UFS_V4_KALAMA
	tristate "Qualcomm Technologies, Inc. UFS Kalama Phy Driver"
	depends on PHY_QCOM_UFS
	help
	  This configuration option adds support for the UFS
	  PHY present on QCOM Kalama chipsets.
	  This is required to enable UFS on Kalama chipset.
	  To compile this driver as a module, choose M here.
"""),
]

# (new module, template module, [(old, new) in the template's names and sources])
MODULES = [(f"drivers/clk/qcom/{cc}-kalama", f"drivers/clk/qcom/{cc}-sun",
            [(f"{cc}-sun", f"{cc}-kalama"), ("_SUN", "_KALAMA")])
           for cc in ("gcc", "camcc", "dispcc", "gpucc", "tcsrcc", "videocc", "debugcc")] + [
    ("drivers/pinctrl/qcom/pinctrl-kalama", "drivers/pinctrl/qcom/pinctrl-sun",
     [("pinctrl-sun", "pinctrl-kalama"), ("_SUN", "_KALAMA")]),
    ("drivers/interconnect/qcom/qnoc-kalama", "drivers/interconnect/qcom/qnoc-sun",
     [("qnoc-sun", "qnoc-kalama"), ("qcom/sun.c", "qcom/kalama.c"), ("_SUN", "_KALAMA")]),
    ("drivers/phy/qualcomm/phy-qcom-ufs-qmp-v4-kalama", "drivers/phy/qualcomm/phy-qcom-ufs-qmp-v4-waipio",
     [("waipio", "kalama"), ("WAIPIO", "KALAMA")]),
]


# Xiaomi's 5.15 sources -> soc-repo's 6.12 framework: (file pattern, pattern, replacement)
PORT_EDITS = [
    # qcom_cc_really_probe() takes the device
    (r"drivers/clk/qcom/\w+-kalama\.c", r"\bqcom_cc_really_probe\(pdev,", "qcom_cc_really_probe(&pdev->dev,"),
    # the functions are struct pinfunction (same fields)
    (r"drivers/pinctrl/qcom/pinctrl-kalama\.c", r"static const struct msm_function kalama_functions\[\]",
     "static const struct pinfunction kalama_functions[]"),
    # soc-repo's core reads a group's name and pins from .grp only (msm_get_group_name/_pins): fill
    # it too, or every group is nameless and each TLMM pinctrl consumer fails to probe
    (r"drivers/pinctrl/qcom/pinctrl-kalama\.c",
     r"(\t\t\.npins = \(unsigned int\)ARRAY_SIZE\(gpio##id##_pins\),[ \t]*\\\n)(?![ \t]*\.grp)",
     r'\1\t\t.grp = PINCTRL_PINGROUP("gpio" #id, gpio##id##_pins, ARRAY_SIZE(gpio##id##_pins)), \\\n'),
    (r"drivers/pinctrl/qcom/pinctrl-kalama\.c",
     r"(\t\t\.npins = \(unsigned int\)ARRAY_SIZE\(pg_name##_pins\),[ \t]*\\\n)(?![ \t]*\.grp)",
     r"\1\t\t.grp = PINCTRL_PINGROUP(#pg_name, pg_name##_pins, ARRAY_SIZE(pg_name##_pins)), \\\n"),
    # sun.c, soc-repo's newest NoC driver, has the core's sync_state
    (r"drivers/interconnect/qcom/kalama\.c", r"\.sync_state = qcom_icc_rpmh_sync_state,", ".sync_state = icc_sync_state,"),
    # Xiaomi's: a %s without its argument
    (r"drivers/phy/qualcomm/phy-qcom-ufs-qmp-v4-kalama\.c", r'"%s: unsupported submode\.\\n"\);',
     r'"%s: unsupported submode.\\n", __func__);'),
]


KALAMA_RPMH = """
/*
 * kalama (SM8550), as Xiaomi's 5.15 tree has it: the same resources as pineapple's table, but
 * RF_CLK1-4 are clka1-4 undivided and RF_CLK5 is clka5 / 2
 */
static struct clk_hw *kalama_rpmh_clocks[] = {
	[RPMH_CXO_PAD_CLK]      = &clk_rpmh_xo_pad_div2.hw,
	[RPMH_CXO_PAD_CLK_A]    = &clk_rpmh_xo_pad_div2_ao.hw,
	[RPMH_CXO_CLK]          = &pineapple_bi_tcxo.hw,
	[RPMH_CXO_CLK_A]        = &pineapple_bi_tcxo_ao.hw,
	[RPMH_LN_BB_CLK1]	= &clk_rpmh_clk6_a2.hw,
	[RPMH_LN_BB_CLK1_A]	= &clk_rpmh_clk6_a2_ao.hw,
	[RPMH_LN_BB_CLK2]	= &clk_rpmh_clk7_a2.hw,
	[RPMH_LN_BB_CLK2_A]	= &clk_rpmh_clk7_a2_ao.hw,
	[RPMH_LN_BB_CLK3]	= &clk_rpmh_clk8_a2.hw,
	[RPMH_LN_BB_CLK3_A]	= &clk_rpmh_clk8_a2_ao.hw,
	[RPMH_RF_CLK1]		= &clk_rpmh_clk1_a1.hw,
	[RPMH_RF_CLK1_A]	= &clk_rpmh_clk1_a1_ao.hw,
	[RPMH_RF_CLK2]		= &clk_rpmh_clk2_a1.hw,
	[RPMH_RF_CLK2_A]	= &clk_rpmh_clk2_a1_ao.hw,
	[RPMH_RF_CLK3]		= &clk_rpmh_clk3_a1.hw,
	[RPMH_RF_CLK3_A]	= &clk_rpmh_clk3_a1_ao.hw,
	[RPMH_RF_CLK4]		= &clk_rpmh_clk4_a1.hw,
	[RPMH_RF_CLK4_A]	= &clk_rpmh_clk4_a1_ao.hw,
	[RPMH_RF_CLK5]		= &clk_rpmh_clk5_a2.hw,
	[RPMH_RF_CLK5_A]	= &clk_rpmh_clk5_a2_ao.hw,
	[RPMH_IPA_CLK]		= &clk_rpmh_ipa.hw,
};

static const struct clk_rpmh_desc clk_rpmh_kalama = {
	.clks = kalama_rpmh_clocks,
	.num_clks = ARRAY_SIZE(kalama_rpmh_clocks),
};
"""

OLD_KALAMA_PAS_HEAD = """/*
 * kalama, from Xiaomi's 5.15 driver: its qmp_name is load_state here; the modem's DSM setup and
 * free_after_auth_reset of 5.15 have no counterpart in this driver (the modem is not needed to boot)
 */
"""
KALAMA_PAS = """/*
 * kalama, from Xiaomi's 5.15 driver: its qmp_name is load_state here; the modem's DSM setup is
 * k618_dsm_setup (k618_mpss_dsm_setup); 5.15's free_after_auth_reset has no counterpart here
 */
static const struct adsp_data kalama_adsp_resource = {
	.crash_reason_smem = 423,
	.firmware_name = "adsp.mdt",
	.dtb_firmware_name = "adsp_dtb.mdt",
	.pas_id = 1,
	.dtb_pas_id = 0x24,
	.minidump_id = 5,
	.uses_elf64 = true,
	.auto_boot = false,
	.load_state = "adsp",
	.ssr_name = "lpass",
	.sysmon_name = "adsp",
	.ssctl_id = 0x14,
};

static const struct adsp_data kalama_cdsp_resource = {
	.crash_reason_smem = 601,
	.firmware_name = "cdsp.mdt",
	.dtb_firmware_name = "cdsp_dtb.mdt",
	.pas_id = 18,
	.dtb_pas_id = 0x25,
	.minidump_id = 7,
	.uses_elf64 = true,
	.auto_boot = false,
	.load_state = "cdsp",
	.ssr_name = "cdsp",
	.sysmon_name = "cdsp",
	.ssctl_id = 0x17,
};

static const struct adsp_data kalama_mpss_resource = {
	.crash_reason_smem = 421,
	.firmware_name = "modem.mdt",
	.dtb_firmware_name = "modem_dtb.mdt",
	.pas_id = 4,
	.dtb_pas_id = 0x26,
	.minidump_id = 3,
	.uses_elf64 = true,
	.auto_boot = false,
	.load_state = "modem",
	.ssr_name = "mpss",
	.sysmon_name = "modem",
\t.ssctl_id = 0x12,
\t.dma_phys_below_32b = true,
\t.k618_dsm_setup = true,
};

"""

K618_PAS_DSM = """/* Device compatibility. */
static bool k618_dsm_done;

static int k618_mpss_dsm_setup(struct platform_device *pdev)
{
	struct qcom_scm_vmperm dst = { .vmid = QCOM_SCM_VMID_MSS_MSA, .perm = QCOM_SCM_PERM_RW };
	u64 src = BIT(QCOM_SCM_VMID_HLOS);
	struct device_node *node;
	struct resource res;
	int ret;

	if (k618_dsm_done)
		return 0;
	node = of_parse_phandle(pdev->dev.of_node, \"mpss_dsm_mem_reg\", 0);
	if (!node) {
		dev_err(&pdev->dev, \"mpss dsm mem region is missing\\n\");
		return -EINVAL;
	}
	ret = of_address_to_resource(node, 0, &res);
	of_node_put(node);
	if (ret) {
		dev_err(&pdev->dev, \"address to resource failed for mpss dsm mem\\n\");
		return ret;
	}
	ret = qcom_scm_assign_mem(res.start, resource_size(&res), &src, &dst, 1);
	if (ret) {
		dev_err(&pdev->dev, \"hyp assign for mpss dsm mem failed: %d\\n\", ret);
		return ret;
	}
	dev_info(&pdev->dev, \"k618: mpss dsm mem %pR given to the modem\\n\", &res);
	k618_dsm_done = true;
	return 0;
}

"""

K618_PAS_DSM_CALL = """\t/* k618: the modem's DSM memory first, as the phone's 5.15 driver does (K618_PAS_DSM) */
\tif (desc->k618_dsm_setup && !strcmp(fw_name, "modem.mdt")) {
\t\tret = k618_mpss_dsm_setup(pdev);
\t\tif (ret)
\t\t\treturn ret;
\t}

"""

K618_SPMI_SHARED = """/* Device compatibility. */
static void __iomem *k618_ioremap_shared(struct platform_device *pdev, const char *name)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	void __iomem *base;

	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);
	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	return base ? base : IOMEM_ERR_PTR(-ENOMEM);
}

"""

K618_MEMDUMP = """/* k618: what kalama's reusable mem_dump_region is used as (see mem_dump_probe) */
static struct reserved_mem k618_dump_rmem;

/* k618: kalama's dump entries are direct children of the mem_dump node, not groups of them */
static bool k618_mem_dump_flat(const struct device_node *node)
{
	struct device_node *child;

	for_each_available_child_of_node(node, child) {
		if (of_property_present(child, "qcom,dump-size")) {
			of_node_put(child);
			return true;
		}
	}
	return false;
}

/* k618: the size mem_dump_probe lays out: the two tables, then each group of static entries */
static size_t k618_mem_dump_need(const struct device_node *node, bool flat)
{
	struct device_node *child;
	size_t need = ALIGN(sizeof(struct msm_dump_table) * 2, SZ_4K);

	if (flat)
		return need + ALIGN(mem_dump_calc_dump_total_size(node), SZ_4K);
	for_each_available_child_of_node(node, child)
		if (!of_property_read_bool(child, "qcom,dynamic-mem-dump"))
			need += ALIGN(mem_dump_calc_dump_total_size(child), SZ_4K);
	return need;
}

"""

K618_POOL_RESV = """/*
 * k618: kalama's DT (5.15 era) limits a device's IOVAs with qcom,iommu-dma-addr-pool, not with
 * iommu-addresses. Reserve everything outside the pool(s), as 5.15's
 * qcom_iommu_generate_resv_regions() did, so that the DMA layer allocates inside them (the entries
 * are sorted, as in the DT; qcom_iommu_get_fast_iova_range reads the same property).
 */
static void k618_addr_pool_resv_regions(struct device *dev, struct device_node *np,
					struct list_head *head)
{
	struct iommu_resv_region *region;
	const __be32 *p, *end;
	int len, naddr, nsize;
	u64 next = 0;

	p = of_get_property(np, "qcom,iommu-dma-addr-pool", &len);
	naddr = of_n_addr_cells(np);
	nsize = of_n_size_cells(np);
	if (!p || !naddr || !nsize || len % ((naddr + nsize) * sizeof(u32))) {
		dev_err(dev, "unusable qcom,iommu-dma-addr-pool\\n");
		return;
	}
	for (end = p + len / sizeof(u32); p < end; p += naddr + nsize) {
		u64 base = of_read_number(p, naddr), size = of_read_number(p + naddr, nsize);

		if (base > next) {
			region = iommu_alloc_resv_region(next, base - next, 0, IOMMU_RESV_RESERVED,
							 GFP_KERNEL);
			if (!region)
				return;
			list_add_tail(&region->list, head);
		}
		next = base + size;
	}
	if (next) {
		region = iommu_alloc_resv_region(next, -next, 0, IOMMU_RESV_RESERVED, GFP_KERNEL);
		if (region)
			list_add_tail(&region->list, head);
	}
}

"""

K618_ICE_SETUP = """/* Device compatibility. */
#include <linux/tme_hwkm_master.h>

/* Device compatibility. */
unsigned int k618_probe_flush(unsigned int spin_ms);	/* the test probe's, when it is loaded */

static void k618_ice_flush(void)
{
	unsigned int (*f)(unsigned int) = symbol_get(k618_probe_flush);

	if (f) {
		f(200);
		symbol_put(k618_probe_flush);
	}
}

#define K618_HWKM_TPKEY_RECEIVE_CTL	0x101C
#define K618_HWKM_TPKEY_RECEIVE_STATUS	0x1020
#define K618_HWKM_TPKEY_EN		BIT(8)
#define K618_HWKM_TPKEY_VAL		0x18C

static DEFINE_MUTEX(k618_setup_lock);
static bool k618_setup_done;

static int k618_ice_hwkm_setup(struct qcom_ice *ice)
{
	uint32_t (*send)(struct tme_ext_err_info *err_info);
	struct tme_ext_err_info info = {0};
	int status, tries = 0;
	u32 ctl;

	if (!ice->use_hwkm)
		return 0;
	mutex_lock(&k618_setup_lock);
	status = 0;
	if (k618_setup_done)
		goto unlock;
	send = symbol_get(tme_hwkm_master_broadcast_transportkey);
	if (!send) {
		pr_err(\"k618_ice: tmecom-intf is not loaded, key-manager setup not done\\n\");
		status = -EPROBE_DEFER;
		goto unlock;
	}
	ctl = qcom_ice_readl(ice, HWKM_OFFSET(K618_HWKM_TPKEY_RECEIVE_CTL));
	qcom_ice_writel(ice, ctl & ~K618_HWKM_TPKEY_EN, HWKM_OFFSET(K618_HWKM_TPKEY_RECEIVE_CTL));
	wmb();
	qcom_ice_writel(ice, K618_HWKM_TPKEY_VAL, HWKM_OFFSET(K618_HWKM_TPKEY_RECEIVE_CTL));
	wmb();
	/* as the 5.15 driver: the engine may not be ready yet, which it says with -ENODEV/-EAGAIN */
	do {
		status = (int)send(&info);
		if (status != -ENODEV && status != -EAGAIN)
			break;
		usleep_range(8000, 12000);
	} while (++tries < 1000);
	symbol_put(tme_hwkm_master_broadcast_transportkey);
	pr_info(\"k618_ice: key-manager setup %d after %d retries, receive status 0x%x\\n\", status, tries,
		qcom_ice_readl(ice, HWKM_OFFSET(K618_HWKM_TPKEY_RECEIVE_STATUS)));
	ctl = qcom_ice_readl(ice, HWKM_OFFSET(K618_HWKM_TPKEY_RECEIVE_CTL));
	qcom_ice_writel(ice, ctl & ~K618_HWKM_TPKEY_EN, HWKM_OFFSET(K618_HWKM_TPKEY_RECEIVE_CTL));
	wmb();
	if (!status)
		k618_setup_done = true;
unlock:
	mutex_unlock(&k618_setup_lock);
	k618_ice_flush();
	return status;
}

"""

K618_ICE = """/*
 * k618: kalama's devicetree (written for 5.15) gives the inline crypto engine as "ufs_ice" and its
 * key manager as "ufs_ice_hwkm", right behind it; this driver wants one range "ice" with the key
 * manager at QCOM_ICE_HWKM_REG_OFFSET. Join the two when they are laid out that way.
 */
static bool k618_ufs_ice = true;
module_param(k618_ufs_ice, bool, 0444);
MODULE_PARM_DESC(k618_ufs_ice, "use kalama's ufs_ice + ufs_ice_hwkm ranges (5.15-era devicetree)");

static struct resource *k618_ice_legacy_res(struct platform_device *pdev, struct resource *out)
{
	struct resource *ice, *hwkm;

	if (!k618_ufs_ice)
		return NULL;
	ice = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ufs_ice");
	if (!ice)
		return NULL;
	*out = *ice;
	hwkm = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ufs_ice_hwkm");
	if (hwkm && hwkm->start == ice->start + QCOM_ICE_HWKM_REG_OFFSET && hwkm->end > ice->end)
		out->end = hwkm->end;
	else if (hwkm)
		dev_warn(&pdev->dev, "ufs_ice_hwkm %pR does not follow ufs_ice %pR\\n", hwkm, ice);
	out->name = "ice";
	return out;
}

"""


K618_PCIE_ECAM = """/*
 * k618: this release reaches config space through ECAM over the DBI window. canoe's DT gives
 * "dm_core" the whole 256 MB (bus N at DBI + N MB) and a bus-range. kalama's (5.15 era) gives it
 * 0xf1d bytes, no bus-range, bus 1's 1 MB "conf" window right after the DBI and the I/O and memory
 * windows from DBI + 2 MB on -- so pci_ecam_create() got a window too small for one bus (ioremap of
 * 0 bytes: "ECAM ioremap failed"), and the cfg1 iATU region for buses 2-255 would have covered the
 * memory window. Here the ECAM space is DBI .. DBI + 2 MB: bus 0 the root port, bus 1 the WLAN chip
 * (iATU region 0 in CFG-shift mode, as msm_pcie_enable_link programs it). The ECAM blocker stays
 * off on that layout: it blocks DBI + 4 KB .. 1 MB, where kalama keeps its iATU registers ("iatu"
 * = DBI + 0x1000), and it rewrites PARF_SYS_CTRL whole; msm_pcie_oper_conf refuses the other bus-0
 * functions instead.
 */
static struct resource k618_ecam[MAX_RC_NUM];

static bool k618_small_dbi(struct msm_pcie_dev_t *dev)
{
\treturn resource_size(dev->res[MSM_PCIE_RES_DM_CORE].resource) < SZ_1M;
}

static struct resource *k618_ecam_res(struct msm_pcie_dev_t *dev)
{
\tstruct resource *dbi = dev->res[MSM_PCIE_RES_DM_CORE].resource;

\tif (!k618_small_dbi(dev))
\t\treturn dbi;
\tk618_ecam[dev->rc_idx] = DEFINE_RES_MEM_NAMED(dbi->start, SZ_2M, "pcie-ecam");
\treturn &k618_ecam[dev->rc_idx];
}

"""

# kalama in soc-repo's own drivers: (file, anchor, text, where)
SUPPORT = [
    ("drivers/soc/qcom/ice.c", "static struct qcom_ice *qcom_ice_create(struct device *dev,\n", K618_ICE, "before"),
    ("drivers/soc/qcom/memory_dump_v2.c", "#include <linux/of_reserved_mem.h>\n", "#include <linux/cma.h>\n", "after"),
    ("drivers/soc/qcom/memory_dump_v2.c", "static int mem_dump_probe(struct platform_device *pdev)\n{\n",
     K618_MEMDUMP, "before"),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c", "static void arm_smmu_get_resv_regions(struct device *dev,\n",
     K618_POOL_RESV, "before"),
    ("drivers/remoteproc/qcom_q6v5_pas.c", "static const struct of_device_id adsp_of_match[] = {\n", KALAMA_PAS, "before"),
    ("drivers/remoteproc/qcom_q6v5_pas.c", "static int adsp_probe(struct platform_device *pdev)\n{\n", K618_PAS_DSM, "before"),
    ("drivers/remoteproc/qcom_q6v5_pas.c", "\tif (desc->dtb_firmware_name) {\n\t\tdtb_fw_name = desc->dtb_firmware_name;\n",
     K618_PAS_DSM_CALL, "before"),
    ("drivers/spmi/spmi-pmic-arb.c", "static int pmic_arb_get_obsrvr_chnls_v2(struct platform_device *pdev)\n",
     K618_SPMI_SHARED, "before"),
    ("drivers/remoteproc/qcom_q6v5_pas.c",
     "\t{ .compatible = \"qcom,pineapple-adsp-pas\", .data = &pineapple_adsp_resource},\n",
     "\t{ .compatible = \"qcom,kalama-adsp-pas\", .data = &kalama_adsp_resource},\n"
     "\t{ .compatible = \"qcom,kalama-cdsp-pas\", .data = &kalama_cdsp_resource},\n"
     "\t{ .compatible = \"qcom,kalama-modem-pas\", .data = &kalama_mpss_resource},\n", "before"),
    ("drivers/clk/qcom/clk-rpmh.c", "\t.num_clks = ARRAY_SIZE(pineapple_rpmh_clocks),\n};\n", KALAMA_RPMH, "after"),
    ("drivers/clk/qcom/clk-rpmh.c", "\t{ .compatible = \"qcom,pineapple-rpmh-clk\", .data = &clk_rpmh_pineapple},\n",
     "\t{ .compatible = \"qcom,kalama-rpmh-clk\", .data = &clk_rpmh_kalama},\n", "after"),
    # the phone's AOSS node has no generic compatible
    ("drivers/soc/qcom/qcom_aoss.c", "\t{ .compatible = \"qcom,aoss-qmp\", },\n",
     "\t{ .compatible = \"qcom,kalama-aoss-qmp\", },\n", "before"),


    ("drivers/soc/qcom/qti_pmic_glink.c", "\t{ .compatible = \"qcom,qti-pmic-glink\" },\n",
     "\t{ .compatible = \"qcom,pmic-glink\" },\n", "after"),


    ("drivers/leds/leds-qti-flash.c",
     "\t{ .compatible = \"qcom,qti-pm8350c-flash-led\", .data = (void *)4, },\n",
     "\t{ .compatible = \"qcom,pm8350c-flash-led\", .data = (void *)4, },\n", "after"),


    ("drivers/thermal/qcom/cpu_voltage_cooling.c",
     "\t\tif (count == 0) {\n\t\t\tdev_err(dev, \"No cluster available\\n\");\n",
     "\t\t/* k618: the 5.15 layout, one CPU per cluster in one \"qcom,cpus\" list */\n"
     "\t\tcpu_count = count ? 0 : of_count_phandle_with_args(subsys_np, \"qcom,cpus\", NULL);\n"
     "\t\tfor (idx = 0; idx < cpu_count && count < CPU_MAP_CT; idx++) {\n"
     "\t\t\tdev_phandle = of_parse_phandle(subsys_np, \"qcom,cpus\", idx);\n"
     "\t\t\tfor_each_possible_cpu(cpu) {\n"
     "\t\t\t\tcpu_dev = get_cpu_device(cpu);\n"
     "\t\t\t\tif (cpu_dev && cpu_dev->of_node == dev_phandle) {\n"
     "\t\t\t\t\tcpu_map[count++] = cpu;\n"
     "\t\t\t\t\tbreak;\n"
     "\t\t\t\t}\n"
     "\t\t\t}\n"
     "\t\t\tof_node_put(dev_phandle);\n"
     "\t\t}\n"
     "\t\tdev_phandle = NULL;\t/* put already, by the loops above or this one */\n\n", "before"),

    ("drivers/pci/controller/pci-msm.c",
     "static int msm_pcie_oper_conf(struct pci_bus *bus, u32 devfn, int oper,\n",
     K618_PCIE_ECAM, "before"),


    ("drivers/thermal/qcom/qti_cpufreq_cdev.c",
     "\tmutex_unlock(&qti_cpufreq_cdev_lock);\n\n"
     "\tret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, \"thermal-cpu/cdev:online\",\n",
     "\t/* k618: the 5.15 layout, one \"qcom,cpus\" on the node itself, a \"cpu<N>\" device each */\n"
     "\tif (list_empty(&qti_cpufreq_cdev_list)) {\n"
     "\t\tof_for_each_phandle(&it, ret, np, \"qcom,cpus\", NULL, 0) {\n"
     "\t\t\tfor_each_possible_cpu(cpu) {\n"
     "\t\t\t\tcpu_dev = get_cpu_device(cpu);\n"
     "\t\t\t\tif (cpu_dev && cpu_dev->of_node == it.node)\n"
     "\t\t\t\t\tbreak;\n"
     "\t\t\t}\n"
     "\t\t\tif (cpu >= nr_cpu_ids)\n"
     "\t\t\t\tcontinue;\n"
     "\t\t\tcdev_data = devm_kzalloc(dev, sizeof(*cdev_data), GFP_KERNEL);\n"
     "\t\t\tif (!cdev_data) {\n"
     "\t\t\t\tof_node_put(it.node);\n"
     "\t\t\t\tmutex_unlock(&qti_cpufreq_cdev_lock);\n"
     "\t\t\t\treturn -ENOMEM;\n"
     "\t\t\t}\n"
     "\t\t\tcdev_data->cpu = cpu;\n"
     "\t\t\tsnprintf(cdev_data->cdev_name, THERMAL_NAME_LENGTH, \"cpu%d\", cpu);\n"
     "\t\t\tINIT_WORK(&cdev_data->reg_work, cpufreq_cdev_register);\n"
     "\t\t\tlist_add(&cdev_data->node, &qti_cpufreq_cdev_list);\n"
     "\t\t}\n"
     "\t\tret = 0;\n"
     "\t}\n", "before"),
]


# kalama's platform in soc-repo's drivers: (file, old text, new text)
K618_LLCC = """/* Device compatibility. */
static struct regmap *k618_llcc_init_mmio(struct platform_device *pdev, u8 index,
					  const char *name)
{
	struct regmap_config cfg = {
		.reg_bits = 32,
		.reg_stride = 4,
		.val_bits = 32,
		.fast_io = true,
	};
	struct resource *res;
	void __iomem *base;
	resource_size_t stride;
	u32 banks = 1;

	if (of_property_match_string(pdev->dev.of_node, \"reg-names\", \"llcc_base\") < 0)
		return NULL;

	cfg.name = name;
	if (!strcmp(name, \"llcc_broadcast_and_base\"))
		/* this layout has no AND region; the caller takes -EINVAL as \"none\" */
		return ERR_PTR(-EINVAL);
	if (!strcmp(name, \"llcc_broadcast_or_base\")) {
		base = devm_platform_ioremap_resource_byname(pdev, \"llcc_broadcast_base\");
		if (IS_ERR(base))
			return ERR_CAST(base);
		return devm_regmap_init_mmio(&pdev->dev, base, &cfg);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, \"llcc_base\");
	if (!res)
		return ERR_PTR(-EINVAL);
	/* bank 0 is asked for before the count is known: then the whole range is its window */
	if (!IS_ERR_OR_NULL(drv_data) && drv_data->num_banks)
		banks = drv_data->num_banks;
	stride = resource_size(res) / banks;
	if (!stride || (resource_size_t)index * stride >= resource_size(res))
		return ERR_PTR(-EINVAL);
	/* the one range is the device's own resource: map, do not request it again */
	base = devm_ioremap(&pdev->dev, res->start + (resource_size_t)index * stride, stride);
	if (!base)
		return ERR_PTR(-ENOMEM);
	return devm_regmap_init_mmio(&pdev->dev, base, &cfg);
}

"""

SUPPORT_EDITS = [


    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     "\t\tBUG_ON(!smmu_domain->fault_model.non_fatal);\n",
     "\t\t/* k618: no client handler took it -- printed, not a BUG() (k618_kalama.py) */\n"
     "\t\tif (!smmu_domain->fault_model.non_fatal)\n"
     "\t\t\tdev_err_ratelimited(smmu->dev,\n"
     "\t\t\t\t\"k618: unhandled context fault on a bank the tree calls fatal, carrying on\\n\");\n"),
    # the PAS descriptor learns the modem's DSM setup (K618_PAS_DSM): the field, and the descriptor
    # text a tree made before it carries
    ("drivers/remoteproc/qcom_q6v5_pas.c",
     "\tbool dma_phys_below_32b;\n\tbool check_status;\n\tbool early_boot;\n};",
     "\tbool dma_phys_below_32b;\n\tbool check_status;\n\tbool early_boot;\n\tbool k618_dsm_setup;\n};"),
    ("drivers/remoteproc/qcom_q6v5_pas.c", OLD_KALAMA_PAS_HEAD, KALAMA_PAS.split("static const struct adsp_data")[0]),
    ("drivers/remoteproc/qcom_q6v5_pas.c",
     "\t.sysmon_name = \"modem\",\n\t.ssctl_id = 0x12,\n\t.dma_phys_below_32b = true,\n};\n",
     "\t.sysmon_name = \"modem\",\n\t.ssctl_id = 0x12,\n\t.dma_phys_below_32b = true,\n\t.k618_dsm_setup = true,\n};\n"),
    # the two-node SPMI layout (K618_SPMI_SHARED): shared ranges mapped without a request, and the
    # bus a node serves taken from its qcom,bus-id, as the phone's 5.15 driver does
    ("drivers/spmi/spmi-pmic-arb.c", 'devm_platform_ioremap_resource_byname(pdev, "obsrvr")',
     'k618_ioremap_shared(pdev, "obsrvr")'),
    ("drivers/spmi/spmi-pmic-arb.c", 'devm_platform_ioremap_resource_byname(pdev, "chnls")',
     'k618_ioremap_shared(pdev, "chnls")'),
    ("drivers/spmi/spmi-pmic-arb.c", "\tbus->id = bus_index;\n",
     "\tbus->id = bus_index;\n\t{\n\t\t/* k618: one node per bus in the phone's tree, which says which bus */\n"
     "\t\tu32 k618_bus_id;\n\n\t\tif (!of_property_read_u32(node, \"qcom,bus-id\", &k618_bus_id))\n"
     "\t\t\tbus->id = k618_bus_id;\n\t}\n"),
    ("drivers/spmi/spmi-pmic-arb.c", "ret = pmic_arb->ver_ops->init_apid(bus, bus_index);",
     "ret = pmic_arb->ver_ops->init_apid(bus, bus->id);"),
    ("drivers/spmi/spmi-pmic-arb.c", 'dev_set_name(&ctrl->dev, "spmi-%d", bus_index);',
     'dev_set_name(&ctrl->dev, "spmi-%d", bus->id);'),


    ("drivers/soc/qcom/mem_buf/mem-buf-gh.c",
     "\tcount = of_property_count_strings(dev->of_node, \"qcom,msgq-names\");\n\tif (count < 0) {\n",
     "\tcount = of_property_count_strings(dev->of_node, \"qcom,msgq-names\");\n"
     "\t/* k618: the 13 Pro's tree names no queue; its 5.15 driver registered the one trusted_vm queue */\n"
     "\tif (count == -EINVAL)\n\t\tcount = 1;\n\tif (count < 0) {\n"),
    ("drivers/soc/qcom/mem_buf/mem-buf-gh.c",
     "\t\tret = of_property_read_string_index(dev->of_node, \"qcom,msgq-names\", i, &name);\n",
     "\t\tif (!of_property_present(dev->of_node, \"qcom,msgq-names\")) {\n\t\t\tname = \"trusted_vm\";\n"
     "\t\t\tret = 0;\n\t\t} else {\n"
     "\t\t\tret = of_property_read_string_index(dev->of_node, \"qcom,msgq-names\", i, &name);\n\t\t}\n"),
    # The one thing Qualcomm's own 6.18 arm_smmu.ko says that this source does not: how many of the
    # bootloader's stream mappings it took over. Found by disassembling the 18 Pro Max's module
    # against this tree -- everything else in arm_smmu_handoff_cbs() is identical, down to the
    # instruction. It is the only evidence in dmesg that the handover was picked up at all, which
    # matters when a display comes up black.
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     "\tu32 i, smr, s2cr;\n\tu32 index;\n\tstruct arm_smmu_smr\tsmrs;",
     "\tu32 i, smr, s2cr;\n\tu32 index;\n\tint k618_pinned = 0;\n\tstruct arm_smmu_smr\tsmrs;"),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     "\t\t\t\tsmmu->s2crs[i].pinned = true;\n"
     "\t\t\t\tbitmap_set(smmu->context_map, smmu->s2crs[i].cbndx, 1);\n",
     "\t\t\t\tsmmu->s2crs[i].pinned = true;\n"
     "\t\t\t\tbitmap_set(smmu->context_map, smmu->s2crs[i].cbndx, 1);\n"
     "\t\t\t\tk618_pinned++;\n"),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     "\tkfree(handoff_smrs);\n\n\treturn 0;\n}",
     "\tdev_notice(smmu->dev, \"\\tpreserved %d qcom,handoff-smrs boot mapping%s\\n\",\n"
     "\t\t   k618_pinned, k618_pinned > 1 ? \"s\" : \"\");\n"
     "\tkfree(handoff_smrs);\n\n\treturn 0;\n}"),
    # the cache controller's ranges, as this phone's devicetree gives them (K618_LLCC)
    ("drivers/soc/qcom/llcc-qcom.c",
     "static struct regmap *qcom_llcc_init_mmio(struct platform_device *pdev, u8 index,\n"
     "\t\t\t\t\t  const char *name)\n"
     "{\n",
     K618_LLCC +
     "static struct regmap *qcom_llcc_init_mmio(struct platform_device *pdev, u8 index,\n"
     "\t\t\t\t\t  const char *name)\n"
     "{\n"
     "\tstruct regmap *k618 = k618_llcc_init_mmio(pdev, index, name);\n\n"
     "\tif (k618)\n\t\treturn k618;\n"),
    # the 13 Pro's hypervisor has no heap management (its 5.15 set has no such module): the heap
    # manager stays loaded with the feature off -- every one of its exports checks
    # rm_heap_manager_enabled -- instead of failing, which stops first-stage init (mem_buf_dev,
    # qcom_wdt_core and the rest depend on it)
    ("drivers/virt/gunyah/gh_rm_heap_manager.c",
     "out_err:\n\tkfree(rm_heap_manager);\n\treturn ret;\n}\nmodule_init(gh_rm_heap_manager_init);",
     "out_err:\n\tkfree(rm_heap_manager);\n\trm_heap_manager = NULL;\n"
     "\t/* k618: without the hypervisor's heap management the module stays, the feature off */\n"
     "\treturn ret == -ENOMEM ? ret : 0;\n}\nmodule_init(gh_rm_heap_manager_init);"),
    # SM8550's UFS controller has no MCQ and reports LSDBS = 1 ("legacy single doorbell removed"),
    # although the legacy doorbell is all it has: 6.12's core then refuses it ("failed to initialize
    # (legacy doorbell mode not supported)", -22; upstream ea593e0). soc-repo sets the quirk for
    # upstream's compatible only; the phone's node is the downstream "qcom,ufshc"
    ("drivers/ufs/host/ufs-qcom.c",
     "\tif (of_device_is_compatible(hba->dev->of_node, \"qcom,sm8550-ufshc\"))\n",
     "\tif (of_device_is_compatible(hba->dev->of_node, \"qcom,sm8550-ufshc\") ||\n"
     "\t    of_machine_is_compatible(\"qcom,kalama\"))\n"),
    # single doorbell (kalama has no MCQ): 6.12's core does not complete a REQ_POLLED request from the
    # interrupt (ufshcd_clear_polled), so a request sent again after this completion (a retry) would
    # end only at the SCSI timeout; canoe runs MCQ and never gets here
    ("drivers/ufs/host/ufs-qcom.c",
     "\t\t\tif ((host->irq_affinity_support) && atomic_read(&host->hi_pri_en) && rq)\n"
     "\t\t\t\trq->cmd_flags |= REQ_POLLED;\n",
     "\t\t\t/* k618: no REQ_POLLED in single-doorbell mode: the interrupt would never complete it */\n"),
    # kalama's mem_dump_region is a reusable shared-dma-pool (CMA, 52 MiB), which the page allocator
    # owns from boot on; soc-repo uses the region as a carveout and frees what it does not use with
    # free_reserved_page() -- the allocator's own pages a second time (QEMU: "BUG: Bad page state").
    # Take what the dump needs with cma_alloc instead, as 5.15 did with dmam_alloc_coherent, and read
    # kalama's flat layout (the dump entries are direct children of the node)
    ("drivers/soc/qcom/memory_dump_v2.c",
     "\tstruct md_region md_entry;\n\n\tret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));",
     "\tstruct md_region md_entry;\n\tstruct cma *cma = NULL;\n\tbool reusable, flat;\n\tstruct page *pages;\n"
     "\tsize_t need;\n\n\tret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));"),
    ("drivers/soc/qcom/memory_dump_v2.c",
     "\trmem = of_reserved_mem_lookup(rmem_node);\n\tof_node_put(rmem_node);",
     "\trmem = of_reserved_mem_lookup(rmem_node);\n\treusable = of_property_read_bool(rmem_node, \"reusable\");\n"
     "\tof_node_put(rmem_node);"),
    ("drivers/soc/qcom/memory_dump_v2.c",
     "\tused_size = ALIGN(used_size, SZ_4K);\n\tphys_addr = rmem->base;",
     "\tused_size = ALIGN(used_size, SZ_4K);\n"
     "\t/*\n"
     "\t * k618: a reusable region (CMA) belongs to the page allocator: take what the dump needs out\n"
     "\t * of it rather than using it as a carveout (whose rest is freed with free_reserved_page)\n"
     "\t */\n"
     "\tflat = k618_mem_dump_flat(node);\n"
     "\tif (reusable) {\n"
     "\t\tcma = rmem->priv;\n"
     "\t\tneed = k618_mem_dump_need(node, flat);\n"
     "\t\tpages = cma ? cma_alloc(cma, need >> PAGE_SHIFT, 0, false) : NULL;\n"
     "\t\tif (!pages) {\n"
     "\t\t\tdev_err(&pdev->dev, \"no %zu bytes for the dump in %s\\n\", need, rmem->name);\n"
     "\t\t\treturn -ENOMEM;\n"
     "\t\t}\n"
     "\t\tk618_dump_rmem.name = rmem->name;\n"
     "\t\tk618_dump_rmem.base = page_to_phys(pages);\n"
     "\t\tk618_dump_rmem.size = need;\n"
     "\t\trmem = &k618_dump_rmem;\n"
     "\t}\n"
     "\tphys_addr = rmem->base;"),
    ("drivers/soc/qcom/memory_dump_v2.c",
     "\tfor_each_available_child_of_node(node, child_node) {\n"
     "\t\tif (of_property_read_bool(child_node, \"qcom,dynamic-mem-dump\")) {",
     "\tif (flat) {\n"
     "\t\t/* k618: the node itself is the one group of entries */\n"
     "\t\tret = mem_dump_alloc(pdev, (struct device_node *)node, rmem, &used_size);\n"
     "\t\tif (ret)\n"
     "\t\t\tdev_err(&pdev->dev, \"static dump alloc failed\\n\");\n"
     "\t} else for_each_available_child_of_node(node, child_node) {\n"
     "\t\tif (of_property_read_bool(child_node, \"qcom,dynamic-mem-dump\")) {"),
    ("drivers/soc/qcom/memory_dump_v2.c",
     "\tif (free_size > 0)\n\t\tdynamic_mem_dump_free_rmem(rmem->base + used_size, free_size);",
     "\tif (free_size > 0 && cma)\n"
     "\t\t/* k618: a part the layout did not use goes back to the CMA area */\n"
     "\t\tcma_release(cma, phys_to_page(rmem->base + used_size), free_size >> PAGE_SHIFT);\n"
     "\telse if (free_size > 0)\n"
     "\t\tdynamic_mem_dump_free_rmem(rmem->base + used_size, free_size);"),


    ("drivers/pci/controller/pci-msm.c",
     "\tu32 *clk_freq = NULL, *clk_suppressible = NULL;\n",
     "\tu32 *clk_freq = NULL, *clk_suppressible = NULL;\n"
     "\tconst char *freq_prop = \"qcom,pcie-clock-frequency\";\n"),
    ("drivers/pci/controller/pci-msm.c",
     "\tret = of_property_count_elems_of_size(pdev->dev.of_node,\n"
     "\t\t\t\t\t      \"qcom,pcie-clock-frequency\",\n"
     "\t\t\t\t\t      sizeof(*clk_freq));\n"
     "\tif (ret != total_num_clk) {\n",
     "\tret = of_property_count_elems_of_size(pdev->dev.of_node, freq_prop,\n"
     "\t\t\t\t\t      sizeof(*clk_freq));\n"
     "\tif (ret != total_num_clk) {\n"
     "\t\t/* k618: kalama's DT names it clock-frequency */\n"
     "\t\tfreq_prop = \"clock-frequency\";\n"
     "\t\tret = of_property_count_elems_of_size(pdev->dev.of_node, freq_prop,\n"
     "\t\t\t\t\t\t      sizeof(*clk_freq));\n"
     "\t}\n"
     "\tif (ret != total_num_clk) {\n"),
    ("drivers/pci/controller/pci-msm.c",
     "\tret = of_property_read_u32_array(pdev->dev.of_node, \"qcom,pcie-clock-frequency\",\n"
     "\t\t\t\t\t clk_freq, total_num_clk);\n",
     "\tret = of_property_read_u32_array(pdev->dev.of_node, freq_prop,\n"
     "\t\t\t\t\t clk_freq, total_num_clk);\n"),
    # kalama's DT gives some devices (the QUP wrappers) their IOVA range as the legacy
    # qcom,iommu-dma-addr-pool; soc-repo refuses it (WARN, -EINVAL: no fastmap domain, a failed
    # iommu_setup_default_domain). Read it as 5.15 did, and reserve what lies outside it
    ("drivers/iommu/qcom-iommu-util.c",
     "\tif (of_property_present(np, \"qcom,iommu-dma-addr-pool\")) {\n"
     "\t\tWARN(1, \"qcom,iommu-dma-addr-pool is deprecated. Switch to using iommu-addresses.\");\n"
     "\t\treturn -EINVAL;\n"
     "\t}\n\n"
     "\tret = get_iova_range_from_iommu_addresses(dev, &dma_range,\n"
     "\t\t\t\t\t\t  fastmap_max_iova);\n",
     "\tif (of_property_present(np, \"qcom,iommu-dma-addr-pool\"))\n"
     "\t\t/* k618: kalama's DT (5.15 era) gives the range this way: read it as 5.15 did */\n"
     "\t\tret = of_property_walk_each_entry(dev, \"qcom,iommu-dma-addr-pool\",\n"
     "\t\t\t\t\t\t  &get_addr_range_cb_data);\n"
     "\telse\n"
     "\t\tret = get_iova_range_from_iommu_addresses(dev, &dma_range,\n"
     "\t\t\t\t\t\t\t  fastmap_max_iova);\n"),
    ("drivers/iommu/arm/arm-smmu/arm-smmu.c",
     "\t\tif (of_property_present(np, \"qcom,iommu-dma-addr-pool\"))\n"
     "\t\t\tWARN(1, \"%s: qcom,iommu-dma-addr-pool is deprecated. Switch to using iommu-addresses.\",\n"
     "\t\t\t\tdev_name(dev));\n",
     "\t\t/* k618: kalama's legacy pool, handled as 5.15 did */\n"
     "\t\tif (of_property_present(np, \"qcom,iommu-dma-addr-pool\"))\n"
     "\t\t\tk618_addr_pool_resv_regions(dev, np, head);\n"),
    # kalama's UFS node names the inline crypto engine "ufs_ice" and its key manager "ufs_ice_hwkm"
    # (0x1d88000 + 0x8000, then 0x1d90000 + 0x9000); of_qcom_ice_get() looks for "ice" only, so
    # UFS ran without crypto and /data (wrappedkey_v0) could not be unlocked. Join the two into the
    # one range the driver expects (HWKM at base + QCOM_ICE_HWKM_REG_OFFSET) and use HWKM when the
    # key manager's range is there; qcom_ice.k618_ufs_ice=0 turns it off again for an A/B
    ("drivers/soc/qcom/ice.c",
     "\tstruct resource *res;\n\tvoid __iomem *base;\n\n\tif (!dev || !dev->of_node)\n\t\treturn ERR_PTR(-ENODEV);\n",
     "\tstruct resource *res;\n\tstruct resource k618_res;\n\tvoid __iomem *base;\n\n"
     "\tif (!dev || !dev->of_node)\n\t\treturn ERR_PTR(-ENODEV);\n"),
    ("drivers/soc/qcom/ice.c",
     "\tres = platform_get_resource_byname(pdev, IORESOURCE_MEM, \"ice\");\n\tif (res) {\n",
     "\tres = platform_get_resource_byname(pdev, IORESOURCE_MEM, \"ice\");\n"
     "\tif (!res && k618_ice_legacy_res(pdev, &k618_res)) {\n"
     "\t\t/* k618: the joined range spans two of the device's own resources: map, do not request */\n"
     "\t\tbase = devm_ioremap(&pdev->dev, k618_res.start, resource_size(&k618_res));\n"
     "\t\tif (!base)\n\t\t\treturn ERR_PTR(-ENOMEM);\n"
     "\t\treturn qcom_ice_create(&pdev->dev, base);\n"
     "\t}\n"
     "\tif (res) {\n"),
    ("drivers/soc/qcom/ice.c",
     "\tengine->use_hwkm = of_property_read_bool(dev->of_node,\n\t\t\t\t\t\t \"qcom,ice-use-hwkm\");\n",
     "\tengine->use_hwkm = of_property_read_bool(dev->of_node,\n\t\t\t\t\t\t \"qcom,ice-use-hwkm\") ||\n"
     "\t\t\t   /* k618: kalama's DT says so by naming the key manager's range */\n"
     "\t\t\t   (k618_ufs_ice &&\n"
     "\t\t\t    of_property_match_string(dev->of_node, \"reg-names\", \"ufs_ice_hwkm\") >= 0);\n"),


    ("drivers/soc/qcom/llcc-qcom.c",
     "\t{ .compatible = \"qcom,sm8550-llcc\", .data = &sm8550_cfgs },\n",
     "\t/* k618: kalama is SM8550; the phone's devicetree uses the vendor name */\n"
     "\t{ .compatible = \"qcom,kalama-llcc\", .data = &sm8550_cfgs },\n"
     "\t{ .compatible = \"qcom,sm8550-llcc\", .data = &sm8550_cfgs },\n"),
    # the one-time key-manager setup the phone's 5.15 driver does (K618_ICE_SETUP), placed in front
    # of the wrapped-key path and run before its first key
    ("drivers/soc/qcom/ice.c",
     "#if IS_ENABLED(CONFIG_SCSI_UFS_CRYPTO_QTI) || IS_ENABLED(CONFIG_MMC_CRYPTO_QTI)\n"
     "static int qcom_ice_program_wrapped_key(struct qcom_ice *ice,\n",
     "#if IS_ENABLED(CONFIG_SCSI_UFS_CRYPTO_QTI) || IS_ENABLED(CONFIG_MMC_CRYPTO_QTI)\n"
     + K618_ICE_SETUP +
     "static int qcom_ice_program_wrapped_key(struct qcom_ice *ice,\n"),
    ("drivers/soc/qcom/ice.c",
     "\tstruct qtee_shm shm;\n\n\thwkm_slot = translate_hwkm_slot(ice, slot);\n",
     "\tstruct qtee_shm shm;\n\n"
     "\terr = k618_ice_hwkm_setup(ice);\n\tif (err)\n\t\treturn err;\n\n"
     "\thwkm_slot = translate_hwkm_slot(ice, slot);\n"),
    # a refused key is the last thing the log shows before the phone goes down, so it goes to disk
    # with the reason still in it
    ("drivers/soc/qcom/ice.c",
     "\t\tpr_err(\"%s:SCM call Error: 0x%x slot %d\\n\", __func__, err,\n"
     "\t\t       slot);\n\t\treturn err;\n",
     "\t\tpr_err(\"%s:SCM call Error: 0x%x slot %d\\n\", __func__, err,\n"
     "\t\t       slot);\n\t\tk618_ice_flush();\n\t\treturn err;\n"),


    ("drivers/power/supply/qti_battery_charger.c",
     "\tBATT_CHG_CTRL_LIM_MAX,\n\tBATT_TEMP,\n",
     "\tBATT_CHG_CTRL_LIM_MAX,\n"
     "\tBATT_CONSTANT_CURRENT,\t/* k618: in the firmware's numbering (as 5.15) */\n"
     "\tBATT_TEMP,\n"),


    ("drivers/power/supply/qti_battery_charger.c",
     "\t[BATT_CHG_CTRL_START_THR] = POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,\n"
     "\t[BATT_CHG_CTRL_END_THR]   = POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,\n"
     "\t[BATT_CURR_AVG]\t\t= POWER_SUPPLY_PROP_CURRENT_AVG,\n",
     "\t/* k618: no START/END_THRESHOLD, CURRENT_AVG -- this firmware has no such ids */\n"),
    ("drivers/power/supply/qti_battery_charger.c",
     "\tPOWER_SUPPLY_PROP_CURRENT_NOW,\n\tPOWER_SUPPLY_PROP_CURRENT_AVG,\n"
     "\tPOWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,\n\tPOWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,\n"
     "\tPOWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,\n"
     "\tPOWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,\n\tPOWER_SUPPLY_PROP_TEMP,\n",
     "\tPOWER_SUPPLY_PROP_CURRENT_NOW,\n"
     "\tPOWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,\n\tPOWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,\n"
     "\tPOWER_SUPPLY_PROP_TEMP,\n"),


    ("drivers/remoteproc/qcom_q6v5_pas.c",
     "\tret = qcom_scm_pas_shutdown(adsp->pas_id);\n"
     "\tif (ret && adsp->decrypt_shutdown)\n"
     "\t\tret = adsp_shutdown_poll_decrypt(adsp);\n"
     "\n"
     "\tif (ret)\n"
     "\t\tpanic(\"Panicking, remoteproc %s failed to shutdown.\\n\", rproc->name);\n"
     "\n"
     "\tif (adsp->dtb_pas_id) {\n"
     "\t\tret = qcom_scm_pas_shutdown(adsp->dtb_pas_id);\n"
     "\t\tif (ret)\n"
     "\t\t\tpanic(\"Panicking, remoteproc %s dtb failed to shutdown.\\n\", rproc->name);\n"
     "\t}\n",
     "\tret = qcom_scm_pas_shutdown(adsp->pas_id);\n"
     "\tif (ret && adsp->decrypt_shutdown)\n"
     "\t\tret = adsp_shutdown_poll_decrypt(adsp);\n"
     "\telse if (ret)\t/* k618: as 5.15 did for kalama */\n"
     "\t\tret = qcom_scm_pas_shutdown_retry(adsp->pas_id);\n"
     "\n"
     "\tif (ret && qcom_device_shutdown_in_progress)\n"
     "\t\tdev_err(adsp->dev, \"k618: %s failed to shut down (%d); the system is going down\\n\",\n"
     "\t\t\trproc->name, ret);\n"
     "\telse if (ret)\n"
     "\t\tpanic(\"Panicking, remoteproc %s failed to shutdown.\\n\", rproc->name);\n"
     "\n"
     "\tif (adsp->dtb_pas_id) {\n"
     "\t\tret = qcom_scm_pas_shutdown(adsp->dtb_pas_id);\n"
     "\t\tif (ret && qcom_device_shutdown_in_progress)\n"
     "\t\t\tdev_err(adsp->dev, \"k618: %s dtb failed to shut down (%d)\\n\", rproc->name, ret);\n"
     "\t\telse if (ret)\n"
     "\t\t\tpanic(\"Panicking, remoteproc %s dtb failed to shutdown.\\n\", rproc->name);\n"
     "\t}\n"),


    ("drivers/pci/controller/pci-msm.c",
     "\trc_idx = dev->rc_idx;\n\trc = (bus->number == 0);\n",
     "\trc_idx = dev->rc_idx;\n\trc = (bus->number == 0);\n\n"
     "\t/* k618: no ECAM blocker on kalama's layout: on bus 0 only the root port answers */\n"
     "\tif (rc && devfn && k618_small_dbi(dev)) {\n"
     "\t\t*val = ~0;\n"
     "\t\treturn PCIBIOS_DEVICE_NOT_FOUND;\n"
     "\t}\n"),
    ("drivers/pci/controller/pci-msm.c",
     "\t/* Assign ECAM base and limits bus 1 (cfg0) and buses 2-255 (cfg1) */\n",
     "\t/* k618: kalama's layout has room for buses 0 and 1 only (k618_ecam_res) */\n"
     "\tif (k618_small_dbi(dev))\n"
     "\t\tnum_buses = 2;\n\n"
     "\t/* Assign ECAM base and limits bus 1 (cfg0) and buses 2-255 (cfg1) */\n"),
    ("drivers/pci/controller/pci-msm.c",
     "\tmsm_pcie_iatu_setup_ecam_blocker(dev);\n\treturn ret;\n",
     "\tif (!k618_small_dbi(dev))\n\t\tmsm_pcie_iatu_setup_ecam_blocker(dev);\n\treturn ret;\n"),
    ("drivers/pci/controller/pci-msm.c",
     "\tcfg = pci_ecam_create(&dev->pdev->dev,\n"
     "\t\t\t\tdev->res[MSM_PCIE_RES_DM_CORE].resource,\n"
     "\t\t\t\tbus->res, ecam_ops);\n",
     "\tcfg = pci_ecam_create(&dev->pdev->dev, k618_ecam_res(dev),\n"
     "\t\t\t\tbus->res, ecam_ops);\n"),


    ("drivers/phy/qualcomm/phy-qcom-ufs.c",
     "\t\tdev_err_ratelimited(dev, \"%s: fail rx_sym0_mux_clk %d\\n\",\n"
     "\t\t\t__func__, err);\n",
     "\t\t/* k618: as on 5.15, where the same call fails unchecked (see k618_kalama.py) */\n"
     "\t\tdev_dbg(dev, \"%s: fail rx_sym0_mux_clk %d\\n\", __func__, err);\n"),
    ("drivers/phy/qualcomm/phy-qcom-ufs.c",
     "\t\tdev_err_ratelimited(dev, \"%s: fail rx_sym1_mux_clk %d\\n\",\n"
     "\t\t\t__func__, err);\n",
     "\t\tdev_dbg(dev, \"%s: fail rx_sym1_mux_clk %d\\n\", __func__, err);\n"),
    ("drivers/phy/qualcomm/phy-qcom-ufs.c",
     "\t\tdev_err_ratelimited(dev, \"%s: fail tx_sym0_mux_clk %d\\n\",\n"
     "\t\t\t__func__, err);\n",
     "\t\tdev_dbg(dev, \"%s: fail tx_sym0_mux_clk %d\\n\", __func__, err);\n"),


    ("drivers/pinctrl/qcom/pinctrl-msm.c",
     "\t\t/* Claim ownership of pin if egpio capable */\n"
     "\t\tif (egpio_func && val & BIT(g->egpio_present))\n"
     "\t\t\tval |= BIT(g->egpio_enable);\n",
     "\t\t/* Claim ownership of pin if egpio capable -- k618: with or without an egpio\n"
     "\t\t * function, as 5.15's core did (kalama's data names none) */\n"
     "\t\tif (val & BIT(g->egpio_present))\n"
     "\t\t\tval |= BIT(g->egpio_enable);\n"),


    ("drivers/usb/dwc3/dwc3-msm-core.c",
     "static int dwc3_msm_core_init(struct dwc3_msm *mdwc)\n{\n",
     "/* k618: num-hc-interrupters for the USB audio offload's interrupter (see k618_kalama.py) */\n"
     "static void k618_dwc3_hc_interrupters(struct device *dev, struct device_node *dwc3_node)\n"
     "{\n"
     "\tstruct device_node *qmi;\n"
     "\tstruct property *prop;\n"
     "\t__be16 *val;\n"
     "\tu32 intr = 0;\n"
     "\n"
     "\tif (of_property_present(dwc3_node, \"num-hc-interrupters\"))\n"
     "\t\treturn;\n"
     "\tqmi = of_find_compatible_node(NULL, NULL, \"qcom,usb-audio-qmi-dev\");\n"
     "\tif (!qmi)\n"
     "\t\treturn;\n"
     "\tof_property_read_u32(qmi, \"qcom,usb-audio-intr-num\", &intr);\n"
     "\tof_node_put(qmi);\n"
     "\tif (!intr)\n"
     "\t\treturn;\n"
     "\tprop = kzalloc(sizeof(*prop) + sizeof(*val), GFP_KERNEL);\n"
     "\tif (!prop)\n"
     "\t\treturn;\n"
     "\tval = (__be16 *)(prop + 1);\n"
     "\t*val = cpu_to_be16(intr + 1);\n"
     "\tprop->name = \"num-hc-interrupters\";\n"
     "\tprop->length = sizeof(*val);\n"
     "\tprop->value = val;\n"
     "\tif (of_add_property(dwc3_node, prop)) {\n"
     "\t\tkfree(prop);\n"
     "\t\treturn;\n"
     "\t}\n"
     "\tdev_info(dev, \"k618: num-hc-interrupters %u (USB audio offload uses interrupter %u)\\n\",\n"
     "\t\t intr + 1, intr);\n"
     "}\n"
     "\n"
     "static int dwc3_msm_core_init(struct dwc3_msm *mdwc)\n{\n"),
    ("drivers/usb/dwc3/dwc3-msm-core.c",
     "\tret = of_platform_populate(node, NULL, NULL, mdwc->dev);\n"
     "\tif (ret) {\n"
     "\t\tdev_err(mdwc->dev,\n"
     "\t\t\t\t\"failed to add create dwc3 core\\n\");\n",
     "\tk618_dwc3_hc_interrupters(mdwc->dev, dwc3_node);\n"
     "\tret = of_platform_populate(node, NULL, NULL, mdwc->dev);\n"
     "\tif (ret) {\n"
     "\t\tdev_err(mdwc->dev,\n"
     "\t\t\t\t\"failed to add create dwc3 core\\n\");\n"),
]


def support(soc):
    for f, old, new in SUPPORT_EDITS:
        path = os.path.join(soc, f)
        text = open(path, encoding="utf-8").read()
        if new in text:
            continue
        if old not in text:
            print(f"k618_kalama: {f}: text to change not found", file=sys.stderr)
            continue
        open(path, "w", encoding="utf-8", newline="").write(text.replace(old, new))
        print("k618_kalama: kalama in", f)
    for f, anchor, add, where in SUPPORT:
        path = os.path.join(soc, f)
        text = open(path, encoding="utf-8").read()
        if add in text:
            continue
        if anchor not in text:
            print(f"k618_kalama: {f}: anchor not found: {anchor.strip()!r}", file=sys.stderr)
            continue
        i = text.index(anchor)
        at = i + len(anchor) if where == "after" else i
        open(path, "w", encoding="utf-8", newline="").write(text[:at] + add + text[at:])
        print("k618_kalama: kalama in", f)


def port(soc):
    for f in FILES:
        path = os.path.join(soc, f)
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        new = text
        for fpat, pat, rep in PORT_EDITS:
            if re.fullmatch(fpat, f):
                new = re.sub(pat, rep, new)
        if new != text:
            open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(new)
            print("k618_kalama: ported", f)


def kconfig_block(text, opt):
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if line.strip() in (f"config {opt}", f"menuconfig {opt}"):
            j = i + 1
            while j < len(lines) and not lines[j].startswith(("config ", "menuconfig ", "endif", "endmenu",
                                                              "menu ", "if ", "source ", "choice", "comment ")):
                j += 1
            return "\n".join(lines[i:j]).rstrip() + "\n"
    return None


def starlark(m):
    def lst(key, items):
        if not items:
            return ""
        body = "".join(f'            "{x}",\n' for x in items)
        return f"        {key} = [\n            # do not sort\n{body}        ],\n"
    return ("\n    registry.register(\n"
            f'        name = "{m["name"]}",\n'
            f'        out = "{m["out"]}",\n'
            f'        config = "{m["config"]}",\n'
            + lst("srcs", m["srcs"]) + lst("deps", m["deps"]) +
            "    )\n")


def main(argv):
    fuxi, soc = argv[1].rstrip("/"), argv[2].rstrip("/")
    for f in FILES:
        dst = os.path.join(soc, f)
        if os.path.exists(dst):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(os.path.join(fuxi, f), dst)
        print("k618_kalama: copied", f)
    for kc, opt in KCONFIG:
        dst = os.path.join(soc, kc)
        text = open(dst, encoding="utf-8").read()
        if kconfig_block(text, opt):
            continue
        blk = kconfig_block(open(os.path.join(fuxi, kc), encoding="utf-8").read(), opt)
        if not blk:
            print(f"k618_kalama: {opt} not in Xiaomi's {kc}", file=sys.stderr)
            return 1
        open(dst, "a", encoding="utf-8", newline="\n").write("\n# kalama, from Xiaomi's 13 Pro tree (fuxi-t-oss)\n" + blk)
        print(f"k618_kalama: {kc}: {opt}")
    for kc, opt, blk in KCONFIG_NEW:
        dst = os.path.join(soc, kc)
        text = open(dst, encoding="utf-8").read()
        if kconfig_block(text, opt):
            continue
        open(dst, "a", encoding="utf-8", newline="\n").write("\n# kalama (SM8550)\n" + blk)
        print(f"k618_kalama: {kc}: {opt}")
    mods = kb.load_registry(soc)
    for name, tmpl, subs in MODULES:
        if name in mods:
            continue
        t = mods[tmpl]

        def sub(s):
            for a, b in subs:
                s = s.replace(a, b)
            return s
        m = {"name": name, "out": sub(t["out"]), "config": sub(t["config"]),
             "srcs": [sub(s) for s in t["srcs"]], "deps": list(t["deps"])}
        bzl = None
        # the template's own modules.bzl (the registration goes at the end of its register_modules)
        for dp, _dn, fns in os.walk(soc):
            if "modules.bzl" in fns and "/.git" not in dp:
                p = os.path.join(dp, "modules.bzl")
                if f'name = "{tmpl}",' in open(p, encoding="utf-8").read():
                    bzl = p
                    break
        open(bzl, "a", encoding="utf-8", newline="\n").write(starlark(m))
        print(f"k618_kalama: {os.path.relpath(bzl, soc)}: {name} ({m['config']}, like {tmpl})")
    port(soc)
    support(soc)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
