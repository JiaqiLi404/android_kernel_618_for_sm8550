# Wi-Fi on 6.18: edits ready to paste.
#   KALAMA_SUPPORT_EDITS / KALAMA_SUPPORT  -> kernel-6.18/k618_kalama.py (SUPPORT_EDITS / SUPPORT; soc tree)
#   TP_EDITS                               -> kernel-6.18/k618_tp_fix.py (EDITS; paths relative to vendor/qcom/opensource)
#   K618_TP_ROWS                           -> kernel-6.18/k618_tp.sh (replace the two rows, PARKED without wlan)
# Every (file, old, new) is checked by check_wifi_patches.py against the real trees.

# ---------------------------------------------------------------------------------------------
# 1. soc: pci-msm on kalama's DT layout
# ---------------------------------------------------------------------------------------------
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

KALAMA_SUPPORT_EDITS = [
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
]

KALAMA_SUPPORT = [
    ("drivers/pci/controller/pci-msm.c",
     "static int msm_pcie_oper_conf(struct pci_bus *bus, u32 devfn, int oper,\n",
     K618_PCIE_ECAM, "before"),
]

# ---------------------------------------------------------------------------------------------
# 2. cnss2 (wlan/platform): legacy IOVA window, Xiaomi's board/regdb names, test log level
# ---------------------------------------------------------------------------------------------
CNSS_NAMES = """
/* k618: the 13 Pro's own board and regulatory files, as its 5.15 cnss2 (Xiaomi's: it picks them
 * with hwid's get_hw_version_platform()) asks for them -- kiwi/bd_m2.elf and kiwi/regdb_xiaomi.bin
 * (5.15 dmesg). firmware_mnt also carries Qualcomm's reference bdwlan.elf / regdb.bin, which differ
 * (sha256). A global unit would boot with cnss2.bdf_name=bd_m2gl.elf.
 */
static char *k618_bdf_name = "bd_m2.elf";
module_param_named(bdf_name, k618_bdf_name, charp, 0644);
MODULE_PARM_DESC(bdf_name, "k618: board data file in the chip's firmware directory");
static char *k618_regdb_name = "regdb_xiaomi.bin";
module_param_named(regdb_name, k618_regdb_name, charp, 0644);
MODULE_PARM_DESC(regdb_name, "k618: regulatory database file in the chip's firmware directory");
"""

TP_EDITS = [
    # cnss2 >= 6.5 reads the IOVA window only from a cnss_pci0_iommu_region_partition node
    # (iommu-addresses); the phone's DT has the 5.15 form, qcom,iommu-dma-addr-pool, in the
    # iommu-group node -> "Invalid SMMU size window" and cnss_pci_probe fails
    ("wlan/platform/cnss2/pci_qcom.c",
     "\tof_node = of_find_node_by_name(pci_dev->dev.of_node,\n"
     "\t\t\t\t       \"cnss_pci0_iommu_region_partition\");\n"
     "\tif (!of_node)\n"
     "\t\treturn -EINVAL;\n",
     "\tof_node = of_find_node_by_name(pci_dev->dev.of_node,\n"
     "\t\t\t\t       \"cnss_pci0_iommu_region_partition\");\n"
     "\tif (!of_node) {\n"
     "\t\t/* k618: kalama's DT (5.15 era) gives the window as qcom,iommu-dma-addr-pool\n"
     "\t\t * in the iommu-group node: read it as the pre-6.5 branch below does\n"
     "\t\t * (5.15 dmesg: smmu_iova_start 0xa0000000, smmu_iova_len 0x10000000)\n"
     "\t\t */\n"
     "\t\tu32 addr_win[2];\n"
     "\t\tint ret = of_property_read_u32_array(iommu_group_node,\n"
     "\t\t\t\t\t\t     \"qcom,iommu-dma-addr-pool\",\n"
     "\t\t\t\t\t\t     addr_win, ARRAY_SIZE(addr_win));\n\n"
     "\t\tif (ret)\n"
     "\t\t\treturn ret;\n"
     "\t\tpci_priv->smmu_iova_start = addr_win[0];\n"
     "\t\tpci_priv->smmu_iova_len = addr_win[1];\n"
     "\t\treturn 0;\n"
     "\t}\n"),
    ("wlan/platform/cnss2/qmi.c",
     "#define HDS_FILE_NAME\t\t\t\"hds.bin\"\n",
     "#define HDS_FILE_NAME\t\t\t\"hds.bin\"\n" + CNSS_NAMES),
    ("wlan/platform/cnss2/qmi.c",
     "#ifndef OPLUS_FEATURE_WIFI_BDF\n"
     "//Modify for: multi projects using different bdf\n"
     "\t\t\tif (plat_priv->chip_info.chip_id & CHIP_ID_GF_MASK)\n"
     "\t\t\t\tsnprintf(filename_tmp, filename_len,\n"
     "\t\t\t\t\t ELF_BDF_FILE_NAME_GF);\n"
     "\t\t\telse\n"
     "\t\t\t\tsnprintf(filename_tmp, filename_len,\n"
     "\t\t\t\t\t ELF_BDF_FILE_NAME);\n",
     "#ifndef OPLUS_FEATURE_WIFI_BDF\n"
     "\t\t\t/* k618: board id 0xff on this phone: Xiaomi's per-project name */\n"
     "\t\t\tsnprintf(filename_tmp, filename_len, \"%s\", k618_bdf_name);\n"),
    ("wlan/platform/cnss2/qmi.c",
     "\t\tsnprintf(filename_tmp, filename_len, REGDB_FILE_NAME);\n",
     "\t\tsnprintf(filename_tmp, filename_len, \"%s\", k618_regdb_name);\n"),
    # TEST BOOTS ONLY: cnss_pr_dbg() to the kernel log as the phone's 5.15 dmesg has it (power-up,
    # enumeration, QMI, BDF steps); without it the 6.18 log shows none of the sequence
    ("wlan/platform/cnss2/debug.c",
     "#define DEFAULT_KERNEL_LOG_LEVEL\t\tINFO_LOG\n",
     "#define DEFAULT_KERNEL_LOG_LEVEL\t\tDEBUG_LOG\t/* k618: test boots */\n"),

    # -----------------------------------------------------------------------------------------
    # 3. qcacld-3.0
    # -----------------------------------------------------------------------------------------
    # OnePlus made the two *_EXT latency items empty without OPLUS_BUG_STABILITY but left their
    # CFG() entries: "too few arguments provided to function-like macro invocation" in every file
    # (the 6.12 attempt's tp-log/wlan.log). Their only users are inside OPLUS_BUG_STABILITY.
    ("wlan/qcacld-3.0/components/mlme/dispatcher/inc/cfg_mlme_fe_wlm.h",
     "\tCFG(CFG_LATENCY_FLAGS_ULTLOW) \\\n"
     "\tCFG(CFG_LATENCY_FLAGS_LOW_EXT) \\\n"
     "\tCFG(CFG_LATENCY_FLAGS_ULTLOW_EXT)\n",
     "\tCFG(CFG_LATENCY_FLAGS_ULTLOW)\n"),
    # qdf_crypto.c (always built) calls crypto_cipher_setkey/encrypt_one, exported in the
    # CRYPTO_INTERNAL namespace; GKI has MODULE_ALLOW_MISSING_NAMESPACE_IMPORTS off, so modpost
    # and the module loader both refuse the module without the import
    ("wlan/qcacld-3.0/core/hdd/src/wlan_hdd_main.c",
     "MODULE_LICENSE(\"Dual BSD/GPL\");\nMODULE_AUTHOR(\"Qualcomm Atheros, Inc.\");\n",
     "MODULE_LICENSE(\"Dual BSD/GPL\");\n"
     "/* k618: qdf_crypto.c uses crypto_cipher_*, exported in this namespace */\n"
     "MODULE_IMPORT_NS(\"CRYPTO_INTERNAL\");\n"
     "MODULE_AUTHOR(\"Qualcomm Atheros, Inc.\");\n"),
    # 6.18: cfg80211_new_sta() / cfg80211_del_sta() take the wireless_dev
    ("wlan/qcacld-3.0/core/hdd/src/wlan_hdd_main.c",
     "\tcfg80211_del_sta(adapter->dev,\n",
     "\tcfg80211_del_sta(adapter->dev->ieee80211_ptr,\n"),
    ("wlan/qcacld-3.0/core/hdd/src/wlan_hdd_hostapd.c",
     "\t\t\t\tcfg80211_new_sta(dev,\n",
     "\t\t\t\tcfg80211_new_sta(dev->ieee80211_ptr,\n"),
    ("wlan/qcacld-3.0/core/hdd/src/wlan_hdd_hostapd.c",
     "\t\t\tcfg80211_del_sta(dev,\n",
     "\t\t\tcfg80211_del_sta(dev->ieee80211_ptr,\n"),
]

CFG80211_WRAPPERS = """/*
 * k618: the cfg80211 ops whose types changed between 6.12 and 6.18 (diff of the two GKI headers):
 * keys and stations take the wireless_dev, wiphy parameters / TX power / antennas a radio index,
 * TX power a link id, remain-on-channel an rx address, link reconfiguration one request struct.
 * kCFI traps an op called through a pointer of another type, so each is a wrapper of the 6.18 type
 * around the release's own (6.12-form) function. Every interface this driver registers has a
 * net_device; a wdev without one is refused.
 */
static int k618_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
\t\t\tint link_id, u8 key_index, bool pairwise,
\t\t\tconst u8 *mac_addr, struct key_params *params)
{
\tif (!wdev->netdev)
\t\treturn -EOPNOTSUPP;
\treturn wlan_hdd_cfg80211_add_key(wiphy, wdev->netdev, link_id,
\t\t\t\t\t key_index, pairwise, mac_addr, params);
}

static int k618_get_key(struct wiphy *wiphy, struct wireless_dev *wdev,
\t\t\tint link_id, u8 key_index, bool pairwise,
\t\t\tconst u8 *mac_addr, void *cookie,
\t\t\tvoid (*callback)(void *cookie, struct key_params *))
{
\tif (!wdev->netdev)
\t\treturn -EOPNOTSUPP;
\treturn wlan_hdd_cfg80211_get_key(wiphy, wdev->netdev, link_id,
\t\t\t\t\t key_index, pairwise, mac_addr,
\t\t\t\t\t cookie, callback);
}

static int k618_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
\t\t\tint link_id, u8 key_index, bool pairwise,
\t\t\tconst u8 *mac_addr)
{
\tif (!wdev->netdev)
\t\treturn -EOPNOTSUPP;
\treturn wlan_hdd_cfg80211_del_key(wiphy, wdev->netdev, link_id,
\t\t\t\t\t key_index, pairwise, mac_addr);
}

static int k618_set_default_mgmt_key(struct wiphy *wiphy,
\t\t\t\t     struct wireless_dev *wdev,
\t\t\t\t     int link_id, u8 key_index)
{
\tif (!wdev->netdev)
\t\treturn -EOPNOTSUPP;
\treturn wlan_hdd_set_default_mgmt_key(wiphy, wdev->netdev, link_id,
\t\t\t\t\t     key_index);
}

static int k618_set_default_beacon_key(struct wiphy *wiphy,
\t\t\t\t       struct wireless_dev *wdev,
\t\t\t\t       int link_id, u8 key_index)
{
\tif (!wdev->netdev)
\t\treturn -EOPNOTSUPP;
\treturn wlan_hdd_cfg80211_set_default_beacon_key(wiphy, wdev->netdev,
\t\t\t\t\t\t\tlink_id, key_index);
}

static int k618_add_station(struct wiphy *wiphy, struct wireless_dev *wdev,
\t\t\t    const u8 *mac, struct station_parameters *params)
{
\tif (!wdev->netdev)
\t\treturn -EOPNOTSUPP;
\treturn wlan_hdd_cfg80211_add_station(wiphy, wdev->netdev, mac, params);
}

static int k618_del_station(struct wiphy *wiphy, struct wireless_dev *wdev,
\t\t\t    struct station_del_parameters *params)
{
\tif (!wdev->netdev)
\t\treturn -EOPNOTSUPP;
\treturn wlan_hdd_cfg80211_del_station(wiphy, wdev->netdev, params);
}

static int k618_change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
\t\t\t       const u8 *mac, struct station_parameters *params)
{
\tif (!wdev->netdev)
\t\treturn -EOPNOTSUPP;
\treturn wlan_hdd_change_station(wiphy, wdev->netdev, mac, params);
}

static int k618_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
\t\t\t    const u8 *mac, struct station_info *sinfo)
{
\tif (!wdev->netdev)
\t\treturn -EOPNOTSUPP;
\treturn wlan_hdd_cfg80211_get_station(wiphy, wdev->netdev, mac, sinfo);
}

static int k618_dump_station(struct wiphy *wiphy, struct wireless_dev *wdev,
\t\t\t     int idx, u8 *mac, struct station_info *sinfo)
{
\tif (!wdev->netdev)
\t\treturn -EOPNOTSUPP;
\treturn wlan_hdd_cfg80211_dump_station(wiphy, wdev->netdev, idx, mac,
\t\t\t\t\t      sinfo);
}

static int k618_set_wiphy_params(struct wiphy *wiphy, int radio_idx,
\t\t\t\t u32 changed)
{
\treturn wlan_hdd_cfg80211_set_wiphy_params(wiphy, changed);
}

static int k618_set_tx_power(struct wiphy *wiphy, struct wireless_dev *wdev,
\t\t\t     int radio_idx, enum nl80211_tx_power_setting type,
\t\t\t     int mbm)
{
\treturn wlan_hdd_cfg80211_set_txpower(wiphy, wdev, type, mbm);
}

static int k618_get_tx_power(struct wiphy *wiphy, struct wireless_dev *wdev,
\t\t\t     int radio_idx, unsigned int link_id, int *dbm)
{
\treturn wlan_hdd_cfg80211_get_txpower(wiphy, wdev, link_id, dbm);
}

static int k618_remain_on_channel(struct wiphy *wiphy,
\t\t\t\t  struct wireless_dev *wdev,
\t\t\t\t  struct ieee80211_channel *chan,
\t\t\t\t  unsigned int duration, u64 *cookie,
\t\t\t\t  const u8 *rx_addr)
{
\treturn wlan_hdd_cfg80211_remain_on_channel(wiphy, wdev, chan, duration,
\t\t\t\t\t\t   cookie);
}

static int k618_set_antenna(struct wiphy *wiphy, int radio_idx,
\t\t\t    u32 tx_ant, u32 rx_ant)
{
\treturn wlan_hdd_cfg80211_set_chainmask(wiphy, tx_ant, rx_ant);
}

static int k618_get_antenna(struct wiphy *wiphy, int radio_idx,
\t\t\t    u32 *tx_ant, u32 *rx_ant)
{
\treturn wlan_hdd_cfg80211_get_chainmask(wiphy, tx_ant, rx_ant);
}

#if defined(WLAN_FEATURE_11BE_MLO) && \\
defined(CFG80211_SETUP_LINK_RECONFIG_SUPPORT)
static int k618_assoc_ml_reconf(struct wiphy *wiphy, struct net_device *dev,
\t\t\t\tstruct cfg80211_ml_reconf_req *req)
{
\treturn wlan_hdd_cfg80211_setup_link_reconfig(wiphy, dev, req->add_links,
\t\t\t\t\t\t     req->rem_links);
}
#endif

"""

_C = "wlan/qcacld-3.0/core/hdd/src/wlan_hdd_cfg80211.c"
TP_EDITS += [
    (_C, "static struct cfg80211_ops wlan_hdd_cfg80211_ops = {\n",
     CFG80211_WRAPPERS + "static struct cfg80211_ops wlan_hdd_cfg80211_ops = {\n"),
] + [
    (_C, f"\t.{op} = {old},\n", f"\t.{op} = k618_{new},\n") for op, old, new in [
        ("change_station", "wlan_hdd_change_station", "change_station"),
        ("add_key", "wlan_hdd_cfg80211_add_key", "add_key"),
        ("get_key", "wlan_hdd_cfg80211_get_key", "get_key"),
        ("del_key", "wlan_hdd_cfg80211_del_key", "del_key"),
        ("set_wiphy_params", "wlan_hdd_cfg80211_set_wiphy_params", "set_wiphy_params"),
        ("set_tx_power", "wlan_hdd_cfg80211_set_txpower", "set_tx_power"),
        ("get_tx_power", "wlan_hdd_cfg80211_get_txpower", "get_tx_power"),
        ("remain_on_channel", "wlan_hdd_cfg80211_remain_on_channel", "remain_on_channel"),
        ("set_default_mgmt_key", "wlan_hdd_set_default_mgmt_key", "set_default_mgmt_key"),
        ("set_default_beacon_key", "wlan_hdd_cfg80211_set_default_beacon_key", "set_default_beacon_key"),
        ("dump_station", "wlan_hdd_cfg80211_dump_station", "dump_station"),
        ("get_station", "wlan_hdd_cfg80211_get_station", "get_station"),
        ("del_station", "wlan_hdd_cfg80211_del_station", "del_station"),
        ("add_station", "wlan_hdd_cfg80211_add_station", "add_station"),
        ("set_antenna", "wlan_hdd_cfg80211_set_chainmask", "set_antenna"),
        ("get_antenna", "wlan_hdd_cfg80211_get_chainmask", "get_antenna"),
    ]
] + [
    (_C, "\t.assoc_ml_reconf =\n\t\twlan_hdd_cfg80211_setup_link_reconfig,\n",
     "\t.assoc_ml_reconf = k618_assoc_ml_reconf,\n"),
]

# ---------------------------------------------------------------------------------------------
# 4. k618_tp.sh rows (replace the two existing rows; PARKED="video eva")
# ---------------------------------------------------------------------------------------------
K618_TP_ROWS = """







	\"wlan_platform|wlan/platform|WLAN_PLATFORM_ROOT=%D CONFIG_CNSS2=m CONFIG_CNSS_UTILS=m CONFIG_CNSS_GENL=m CONFIG_WCNSS_MEM_PRE_ALLOC=m CONFIG_CNSS2_QMI=y CONFIG_CNSS_QMI_SVC=m CONFIG_CNSS_PLAT_IPC_QMI_SVC=m CONFIG_CNSS_OUT_OF_TREE=y CONFIG_PCI_MSM=y|%R|-include %T/k618_oplus_stub.h\"
	# the WLAN host driver, named as the phone's own: Qualcomm's kalama build (Android.mk, several
	# chips, TARGET_MULTI_WLAN) made one module per chip with MODNAME = DEVNAME = the chip, so the
	# phone has qca_cld3_kiwi_v2.ko whose module name is kiwi_v2 (modinfo), whose MULTI_IF_NAME is
	# kiwi_v2 (INI wlan/qca_cld/kiwi_v2/WCNSS_qcom_cfg.ini and MAC wlan/qca_cld/kiwi_v2/wlan_mac.bin,
	# as its 5.15 dmesg loads them), and whose state node is /dev/wlan (WLAN_CTRL_NAME; the Wi-Fi HAL
	# writes ON there and finds the driver in /proc/modules by name). WLAN_PROFILE=kiwi_v2 is the chip
	# profile kalama used (default_defconfig + KIWI v2; IPA offload only on pineapple/SDX, so off -- the
	# phone's 5.15 module imports nothing from IPA either). The CNSS variables are what its
	# default_defconfig keys the PCIe/cnss2 bus on; the GKI config has none of them.
	# CONFIG_BUILD_TAG=n: the tag runs git in the (non-git) tree for every object.
	\"wlan|wlan/qcacld-3.0|WLAN_ROOT=%D WLAN_COMMON_ROOT=../qca-wifi-host-cmn MODNAME=kiwi_v2 DEVNAME=kiwi_v2 WLAN_CTRL_NAME=wlan WLAN_PROFILE=kiwi_v2 CONFIG_QCA_CLD_WLAN=m CONFIG_CNSS2=m CONFIG_CNSS_GENL=m CONFIG_CNSS_UTILS=m CONFIG_WCNSS_MEM_PRE_ALLOC=m CONFIG_CNSS_OUT_OF_TREE=y CONFIG_BUILD_TAG=n|%R|\"
"""
# k618_bootimg.sh: "${RENAME-ucsi_glink=ucsi_qti_glink}" -> "${RENAME-ucsi_glink=ucsi_qti_glink qca_cld3_kiwi_v2=kiwi_v2}"
