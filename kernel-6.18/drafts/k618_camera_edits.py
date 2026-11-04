# Proposed additions to kernel-6.18/k618_tp_fix.py EDITS (camera-kernel, 6.12 release on 6.18,
# phone = Xiaomi 13 Pro nuwa, 5.15 devicetree + ROM CamX). Order matters only within a file.
CAMERA_EDITS = [
    # --- 1. CPAS SmartQoS. The phone's qcom,cam-cpas@ac13000 has enable-smart-qos with only
    # rt-wr-priority-min = 3 / -max = 6 (5.15's 8-entry SmartQoS). 6.12 also demands seven tuning
    # properties and fails the whole camera bind on the first ("Invalid clamp Qos priority rc -22").
    # With the values below 6.12's 16-entry algorithm (cam_cpas_calculate_smart_qos) writes exactly what
    # 5.15 wrote: clamp threshold is always 7 (least == most == 7, and the "most stressed" branch needs
    # bw * 100 > total * 100, never true), so entries 15..7 = clamp = max -> PRIORITYLUT_HIGH stays
    # 0x66666666 (its static value, which 5.15 never touched) and entry 7 = max (5.15 forced it);
    # entries 6..1 = min + round(pos * bw / max_bw) capped at max (slope 100% = 5.15's ramp); entry 0 = min.
    ("camera-kernel/drivers/cam_cpas/cam_cpas_soc.c",
     "\t\tif (rc) {\n\t\t\tCAM_ERR(CAM_CPAS, \"Invalid clamp Qos priority rc %d\", rc);\n"
     "\t\t\tgoto cleanup_clients;\n\t\t}\n",
     "\t\tif (rc)\t/* k618: not in the 5.15 DT; = max, see k618_tp_fix.py */\n"
     "\t\t\tvalue = soc_private->smart_qos_info->rt_wr_priority_max;\n"),
    ("camera-kernel/drivers/cam_cpas/cam_cpas_soc.c",
     "\t\tif (rc) {\n\t\t\tCAM_ERR(CAM_CPAS, \"Invalid slope factor rc %d\", rc);\n"
     "\t\t\tgoto cleanup_clients;\n\t\t}\n",
     "\t\tif (rc)\t/* k618: 5.15 DT -- 100% = 5.15's ramp */\n"
     "\t\t\tvalue = CAM_CPAS_MAX_SLOPE_FACTOR;\n"),
    ("camera-kernel/drivers/cam_cpas/cam_cpas_soc.c",
     "\t\tif (rc) {\n\t\t\tCAM_ERR(CAM_CPAS, \"Invalid leaststressed clamp threshold rc %d\", rc);\n"
     "\t\t\tgoto cleanup_clients;\n\t\t}\n",
     "\t\tif (rc)\t/* k618: 5.15 DT -- entries 15..7 at max */\n"
     "\t\t\tvalue = 7;\n"),
    ("camera-kernel/drivers/cam_cpas/cam_cpas_soc.c",
     "\t\tif (rc) {\n\t\t\tCAM_ERR(CAM_CPAS, \"Invalid moststressed clamp threshold rc %d\", rc);\n"
     "\t\t\tgoto cleanup_clients;\n\t\t}\n",
     "\t\tif (rc)\t/* k618: 5.15 DT */\n"
     "\t\t\tvalue = 7;\n"),
    ("camera-kernel/drivers/cam_cpas/cam_cpas_soc.c",
     "\t\tif (rc) {\n\t\t\tCAM_ERR(CAM_CPAS, \"Invalid highstress indicator threshold rc %d\", rc);\n"
     "\t\t\tgoto cleanup_clients;\n\t\t}\n",
     "\t\tif (rc)\t/* k618: 5.15 DT */\n"
     "\t\t\tvalue = CAM_CPAS_MAX_STRESS_INDICATOR;\n"),
    ("camera-kernel/drivers/cam_cpas/cam_cpas_soc.c",
     "\t\tif (rc) {\n\t\t\tCAM_ERR(CAM_CPAS, \"Invalid lowstress indicator threshold rc %d\", rc);\n"
     "\t\t\tgoto cleanup_clients;\n\t\t}\n",
     "\t\tif (rc)\t/* k618: 5.15 DT (high - low != 0: no division by zero) */\n"
     "\t\t\tvalue = 0;\n"),
    ("camera-kernel/drivers/cam_cpas/cam_cpas_soc.c",
     "\t\tif (rc) {\n\t\t\tCAM_ERR(CAM_CPAS, \"Invalid bw ratio scale factor rc %d\", rc);\n"
     "\t\t\tgoto cleanup_clients;\n\t\t}\n",
     "\t\tif (rc)\t/* k618: 5.15 DT (multiplied by least - most = 0) */\n"
     "\t\t\tvalue = 1;\n"),
    # --- 2. the four rt-wr-niu level-1 nodes name only priority-lut-low-offset (0x7630, 0x7830,
    # 0x7a30, 0x7c30); 6.12 also wants the HIGH register and returns -22 from the node-tree parse.
    # On Titan 780 it is the next word: cpastop_v780_100.h lists 0x7634/0x7834/0x7a34/0x7c34 as
    # NOC_RT_n_NIU_PRIORITYLUT_HIGH.
    ("camera-kernel/drivers/cam_cpas/cam_cpas_soc.c",
     "\t\t\t\tif (rc) {\n\t\t\t\t\tCAM_ERR(CAM_CPAS, \"Invalid priority high offset rc %d\", rc);\n"
     "\t\t\t\t\treturn rc;\n\t\t\t\t}\n",
     "\t\t\t\tif (rc) {\n"
     "\t\t\t\t\t/* k618: 5.15 DT names only LOW; Titan 780 has HIGH at LOW + 4 */\n"
     "\t\t\t\t\tcurr_node_ptr->pri_lut_high_offset =\n"
     "\t\t\t\t\t\tcurr_node_ptr->pri_lut_low_offset + 0x4;\n"
     "\t\t\t\t\trc = 0;\n\t\t\t\t}\n"),
    # (cosmetic) 6.18 warns "Read of boolean property 'interconnect-names' with a value"
    ("camera-kernel/drivers/cam_cpas/cam_cpas_soc.c",
     "soc_private->bus_icc_based = of_property_read_bool(of_node, \"interconnect-names\");",
     "soc_private->bus_icc_based = of_property_present(of_node, \"interconnect-names\");"),
    # --- 3. interconnect paths. On >= 6.5 cam_icc_get_path() refuses anything but a lookup by
    # name with of_icc_get(dev, name), which searches the device's own node ("Must use path names to
    # get icc path handle" -> NULL -> CPAS "failed in ahb setup"). The phone's DT has no
    # cam-icc-path-names: the AHB path is in the CPAS node ("cam_ahb"), but each AXI port's
    # interconnects/interconnect-names ("cam_hf_0", "cam_sf_0", "cam_sf_icp") sit in its own
    # qcom,axi-port-mnoc child, and 5.15 reached them with icc_get(src, dst), which 6.18 no longer
    # has. The caller already passes that node (dev_node, unused until now): look the name up there.
    ("camera-kernel/drivers/cam_utils/cam_soc_icc.c",
     "int cam_soc_bus_client_register(struct platform_device *pdev,\n"
     "\tstruct device_node *dev_node, void **client,\n"
     "\tstruct cam_soc_bus_client_common_data *common_data, bool use_path_name)\n"
     "{\n\tstruct cam_soc_bus_client *bus_client = NULL;",
     "static int __cam_soc_bus_client_register(struct platform_device *pdev,\n"
     "\tstruct device_node *dev_node, void **client,\n"
     "\tstruct cam_soc_bus_client_common_data *common_data, bool use_path_name)\n"
     "{\n\tstruct cam_soc_bus_client *bus_client = NULL;"),
    ("camera-kernel/drivers/cam_utils/cam_soc_icc.c",
     "void cam_soc_bus_client_unregister(void **client)\n{",
     "/*\n"
     " * k618: 5.15 devicetrees keep an AXI port's interconnects in its qcom,axi-port-mnoc child and\n"
     " * have no cam-icc-path-names; >= 6.5 can only get a path by name from the device's own node.\n"
     " * Point the lookup at the node that carries the names (the CPAS node itself for AHB).\n"
     " */\n"
     "int cam_soc_bus_client_register(struct platform_device *pdev,\n"
     "\tstruct device_node *dev_node, void **client,\n"
     "\tstruct cam_soc_bus_client_common_data *common_data, bool use_path_name)\n"
     "{\n"
     "\tstruct device_node *np = pdev->dev.of_node;\n"
     "\tint rc;\n\n"
     "\tif (!use_path_name && dev_node)\n"
     "\t\tpdev->dev.of_node = dev_node;\n"
     "\trc = __cam_soc_bus_client_register(pdev, dev_node, client, common_data, true);\n"
     "\tpdev->dev.of_node = np;\n"
     "\treturn rc;\n"
     "}\n\n"
     "void cam_soc_bus_client_unregister(void **client)\n{"),
    # --- 4. user-space interface: the ROM's CamX (odm/lib64/hw/camera.qcom.so) and the phone's own
    # camera.ko (Xiaomi "camera-kernel-nuwa") use VIDIOC_CAM_CONTROL = 0xc02056c0, i.e. a 32-byte
    # struct cam_control: 8 bytes after .handle that the 5.15 driver never reads (it takes op_code at
    # 0, size at 4, handle at 0x10). Ours is 0xc01856c0 -> every camera ioctl is an unknown command.
    # (cam_sync's CAM_PRIVATE_IOCTL_CMD stays 0xc01856c0 on both sides.)
    ("camera-kernel/include/uapi/camera/media/cam_defs.h",
     "struct cam_control {\n\t__u32        op_code;\n\t__u32        size;\n"
     "\t__u32        handle_type;\n\t__u32        reserved;\n\t__u64        handle;\n};",
     "struct cam_control {\n\t__u32        op_code;\n\t__u32        size;\n"
     "\t__u32        handle_type;\n\t__u32        reserved;\n\t__u64        handle;\n"
     "\t__u64        k618_xiaomi_ext;\t/* k618: Xiaomi's 32-byte layout, never read */\n};"),
    # 5.15's ISP query-cap payload is 216 bytes (the phone's camera.ko copies 0xd8 both ways). 6.12
    # added CAM_ISP_HW_MC_TFE and grew dev_caps[] to 9 entries (240 bytes), and
    # cam_ife_mgr_get_hw_caps() rejects any other size with -EFAULT. Only 4 entries are ever filled
    # (IFE, IFE-lite, CSID, CSID-lite); MC_TFE is never reported on this IFE-based chip.
    ("camera-kernel/include/uapi/camera/media/cam_isp.h",
     "#define CAM_ISP_HW_MAX                          9",
     "#define CAM_ISP_HW_MAX                          8\t/* k618: 5.15 value, 216-byte query cap */"),
    # --- 5. ICP: 6.12 needs a CAM_SMMU_REGION_DEVICE (global counter, synx, LLCC registers) in the
    # ICP context bank before it will allocate the HFI memory; the phone's msm_cam_smmu_icp node has
    # only shared / fw_uncached / io / qdss, so every ICP open fails ("Unable to get device memory
    # info") and no IPE/BPS request can run. The 5.15 driver and firmware have no such region.
    ("camera-kernel/drivers/cam_icp/icp_hw/icp_hw_mgr/cam_icp_hw_mgr.c",
     "\tif (rc) {\n\t\tCAM_ERR(CAM_ICP, \"[%s] Unable to get device memory info, rc: %d\",\n"
     "\t\t\thw_mgr->hw_mgr_name, rc);\n\t\treturn rc;\n\t}\n",
     "\tif (rc) {\n"
     "\t\t/* k618: 5.15 DT: no device region for the ICP context bank, as in 5.15 */\n"
     "\t\tCAM_WARN(CAM_ICP, \"[%s] no device memory region (rc: %d), running without it\",\n"
     "\t\t\thw_mgr->hw_mgr_name, rc);\n"
     "\t\tmemset(&hw_mgr->hfi_mem.device, 0, sizeof(hw_mgr->hfi_mem.device));\n"
     "\t\treturn 0;\n\t}\n"),
    ("camera-kernel/drivers/cam_icp/icp_hw/icp_hw_mgr/cam_icp_hw_mgr.c",
     "static inline void cam_icp_free_device_mem(struct cam_icp_hw_mgr *hw_mgr)\n{\n",
     "static inline void cam_icp_free_device_mem(struct cam_icp_hw_mgr *hw_mgr)\n{\n"
     "\tif (!hw_mgr->hfi_mem.device.iova_len)\t/* k618: nothing was mapped */\n"
     "\t\treturn;\n\n"),
    ("camera-kernel/drivers/cam_icp/icp_hw/icp_hw_mgr/cam_icp_hw_mgr.c",
     "\tuint32_t num_regions = ICP_NUM_MEM_REGIONS_FOR_GLOBAL_CNTR;\n\tsize_t payload_size;\n\n",
     "\tuint32_t num_regions = ICP_NUM_MEM_REGIONS_FOR_GLOBAL_CNTR;\n\tsize_t payload_size;\n\n"
     "\t/* k618: no device region: nothing to describe, and the 5.15 firmware does not\n"
     "\t * know HFI_PROP_SYS_MEM_REGIONS\n\t */\n"
     "\tif (!hw_mgr->hfi_mem.device.iova_len)\n\t\treturn 0;\n\n"),
    # --- 6. (noise only) since 6.15 a DMA-API domain carries the IOVA cookie and refuses a fault
    # handler: iommu_set_fault_handler() WARNs once per context bank (5 stack traces at boot) and
    # installs nothing. Skip the call; camera page faults are then reported by the SMMU driver only.
    ("camera-kernel/drivers/cam_smmu/cam_smmu_api.c",
     "\tif (cb->io_support && cb->domain) {\n\t\tiommu_set_fault_handler(cb->domain,",
     "\tif (cb->io_support && cb->domain &&\n"
     "\t\tcb->domain->cookie_type == IOMMU_COOKIE_NONE) {\t/* k618: 6.15+ */\n"
     "\t\tiommu_set_fault_handler(cb->domain,"),
    # --- 7. (defensive) 6.12 refuses to bind ICP unless CPAS hw-caps register 0xac13008 has the ICP
    # bit 0x1 as well as IPE0 0x800 / BPS 0x400; the 5.15 driver never checked the ICP bit and used
    # the other two only to enable IPE/BPS. If kalama's register lacks bit 0 the whole camera bind
    # fails with -ENODEV, so warn instead (the phone's 5.15 behaviour).
    ("camera-kernel/drivers/cam_icp/icp_hw/icp_hw_mgr/cam_icp_hw_mgr.c",
     "\t\t\t\thw_mgr->hw_mgr_name, cam_caps[i], icp_dev_mask[i]);\n\t\t\treturn -ENODEV;\n\t\t}\n",
     "\t\t\t\thw_mgr->hw_mgr_name, cam_caps[i], icp_dev_mask[i]);\n"
     "\t\t\t/* k618: 5.15 never failed on this; carry on */\n\t\t}\n"),
]
