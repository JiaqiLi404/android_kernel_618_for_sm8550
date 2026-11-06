#!/usr/bin/env python3
"""k618_tp_fix.py <techpack root>: the edits Qualcomm's techpack repos need to build through their
own `make M=` path (Kbuild) instead of Kleaf's DDK.

Qualcomm builds these repos with Kleaf: the module's sources come from its `*_modules.bzl`, and the
Kbuild files are the older Android.mk path, which is not what they test. Where the two disagree the
.bzl is authoritative, and the Kbuild is missing a source or a flag. Each edit below says which.

Prints what it changed; idempotent."""

import os
import sys

# (file, old text, new text, why)
EDITS = [
    # synx_modules.bzl lists synx/synx_debugfs_util.c among synx-driver's sources; the Kbuild does
    # not, and synx_debugfs.c calls into it (modpost: synx_debugfs_util_print_hash_table undefined)
    ("synx-kernel/msm/Kbuild",
     "synx-driver-objs := synx/synx.o synx/synx_global.o synx/synx_util.o synx/synx_debugfs.o \\\n",
     "synx-driver-objs := synx/synx.o synx/synx_global.o synx/synx_util.o synx/synx_debugfs.o \\\n"
     "\t\t\tsynx/synx_debugfs_util.o \\\n"),


    ("synx-kernel/msm/synx/synx.c",
     "\trc = synx_global_mem_init();\n"
     "\tif (rc) {\n"
     "\t\tdprintk(SYNX_ERR, \"shared mem init failed, err=%d\\n\", rc);\n"
     "\t\tgoto err;\n"
     "\t}\n",
     "\trc = synx_global_mem_init();\n"
     "\tif (rc) {\n"
     "\t\t/* k618: no IPCLite on this phone -- local synx objects only (see k618_tp_fix.py) */\n"
     "\t\tdprintk(SYNX_WARN, \"no global shared memory (err=%d): local synx only\\n\", rc);\n"
     "\t\trc = 0;\n"
     "\t}\n"),
    # define_modules.bzl lists dsp/fastrpc_sysfs.c among frpc-adsprpc's sources, the Kbuild does not
    # (modpost: fastrpc_sysfs_domain_create undefined)
    ("dsp-kernel/Kbuild",
     "frpc-adsprpc-y := dsp/fastrpc.o\t\\\n\t\t\t dsp/fastrpc_rpmsg.o \\\n",
     "frpc-adsprpc-y := dsp/fastrpc.o\t\\\n\t\t\t dsp/fastrpc_rpmsg.o \\\n\t\t\t dsp/fastrpc_sysfs.o \\\n"),
    # this release dropped cdsp-loader's build rule but kept its source, and the phone's module
    # list still has it; the rule is the one its 5.15 Kbuild had
    ("dsp-kernel/Kbuild",
     "obj-m := frpc-adsprpc.o\n",
     "obj-m := frpc-adsprpc.o\nobj-m += cdsp-loader.o\ncdsp-loader-y := dsp/cdsp-loader.o\n"),
    # cdsp_loader_remove returns void since 6.10 and the file knows that, but its early return was
    # left as "return 0"
    ("dsp-kernel/dsp/cdsp-loader.c",
     "\tif (!priv)\n\t\treturn 0;\n",
     "\tif (!priv)\n"
     "#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)\n\t\treturn;\n"
     "#else\n\t\treturn 0;\n#endif\n"),
    # the NFC Kbuild carries C preprocessor directives, which make reads as comments: the continued
    # line joins a comment and the space-indented line after it becomes a stray recipe ("missing
    # separator"). Write the monitor object into the list as make wants it -- i2c_drv.c calls into it
    # whatever the option says (its guard there is commented out), and its source is under nfc/
    ("../../nxp/opensource/driver/Kbuild",
     "\t\tnfc/i2c_drv.o\\\n#ifdef CONFIG_NXP_NFC_VBAT_MONITOR\n"
     "               nfc_vbat_monitor.o\n#endif\n",
     "\t\tnfc/i2c_drv.o \\\n\t\tnfc/nfc_vbat_monitor.o\n"),
    # the same file includes a header of OnePlus' own boot driver and never uses anything from it
    ("../../nxp/opensource/driver/nfc/i2c_drv.c",
     "#include <soc/oplus/boot/boot_mode.h>\n",
     "/* k618: OnePlus' own boot driver's header, and nothing here uses it */\n"),
    # the 5.15 video driver on 6.12: struct dma_buf_map became struct iosys_map in 5.18, and the
    # mapping call now says explicitly that the caller does not hold the dma-buf's lock
    ("video-driver-515/driver/vidc/inc/msm_vidc_internal.h",
     "\tstruct dma_buf_map          dmabuf_map;",
     "\tstruct iosys_map            dmabuf_map;"),
    ("video-driver-515/driver/vidc/src/msm_vidc_memory.c",
     "dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL)",
     "dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL)"),
    # ... and the AV1 profile, level and tier enums it carried are the kernel's own since 6.5, with
    # the same names and the same values, so the vendor copies only clash
    ("video-driver-515/include/uapi/vidc/media/v4l2_vidc_extensions.h",
     "enum v4l2_mpeg_video_av1_profile {\n"
     "\tV4L2_MPEG_VIDEO_AV1_PROFILE_MAIN            = 0,\n"
     "\tV4L2_MPEG_VIDEO_AV1_PROFILE_HIGH            = 1,\n"
     "\tV4L2_MPEG_VIDEO_AV1_PROFILE_PROFESSIONAL    = 2,\n"
     "};\n",
     "/* k618: enum v4l2_mpeg_video_av1_profile is the kernel's own since 6.5 */\n"),
    ("video-driver-515/include/uapi/vidc/media/v4l2_vidc_extensions.h",
     "enum v4l2_mpeg_video_av1_level {\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_2_0  = 0,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_2_1  = 1,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_2_2  = 2,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_2_3  = 3,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_3_0  = 4,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_3_1  = 5,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_3_2  = 6,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_3_3  = 7,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_4_0  = 8,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_4_1  = 9,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_4_2  = 10,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_4_3  = 11,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_5_0  = 12,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_5_1  = 13,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_5_2  = 14,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_5_3  = 15,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_6_0  = 16,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_6_1  = 17,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_6_2  = 18,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_6_3  = 19,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_7_0  = 20,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_7_1  = 21,\n"
     "\tV4L2_MPEG_VIDEO_AV1_LEVEL_7_2  = 22,\n\tV4L2_MPEG_VIDEO_AV1_LEVEL_7_3  = 23,\n};\n",
     "/* k618: enum v4l2_mpeg_video_av1_level is the kernel's own since 6.5 */\n"),
    # (the AV1 *tier* enum stays: the kernel has an HEVC tier, not an AV1 one)
    # Qualcomm's own 6.12 release answers these three for us, so take their answer:
    # 1) the system-cache hint: their 6.12 video driver writes `|= 0UL /*TODO: define ...*/`, and
    #    soc's qcom-dma-mapping.h defines DMA_ATTR_SYS_CACHE as (0UL) when the kernel has no such
    #    thing -- 6.12's IOMMU core takes no per-mapping bus-attribute override any more
    ("video-driver-515/driver/vidc/src/msm_vidc_memory.c",
     "\t\t\tDMA_ATTR_IOMMU_USE_UPSTREAM_HINT;",
     "\t\t\t0UL /* k618: no such attribute since 6.2, as in the 6.12 driver */;"),
    # 2) the best-fit IOVA allocator and the IOVA reservation: in their 6.12 camera driver
    #    cam_smmu_util_iommu_custom() is an empty function for >= 5.15, the old body kept only in
    #    the #else -- the allocator policy went with Qualcomm's fork of the IOMMU-DMA layer
    ("video-driver-515/driver/vidc/src/msm_vidc_dt.c",
     "\tiommu_dma_enable_best_fit_algo(cb->dev);",
     "\t/* k618: dropped from 5.15 on, as in the 6.12 drivers (the IOMMU-DMA layer has no such\n"
     "\t * allocator policy any more); the default IOVA allocator does the job\n"
     "\t */"),
    # 3) vb2_queue's buffer count is read through an accessor since 6.9; their 6.12 driver uses it
    ("video-driver-515/driver/vidc/src/msm_vidc_debug.c",
     "inst->bufq[i].vb2q->num_buffers);", "vb2_get_num_buffers(inst->bufq[i].vb2q));"),
    # and the ordinary drift around them
    ("video-driver-515/driver/vidc/src/msm_vidc_probe.c",
     "static int msm_vidc_remove(struct platform_device *pdev)",
     "static void msm_vidc_remove(struct platform_device *pdev)"),
    ("video-driver-515/driver/vidc/src/msm_vidc_probe.c",
     "\tif (of_device_is_compatible(pdev->dev.of_node, \"qcom,msm-vidc\")) {\n"
     "\t\treturn msm_vidc_remove_video_device(pdev);\n"
     "\t} else if (of_device_is_compatible(pdev->dev.of_node,\n"
     "\t\t\t\t\"qcom,msm-vidc,context-bank\")) {\n"
     "\t\treturn msm_vidc_remove_context_bank(pdev);\n"
     "\t}\n\n\t/* How did we end up here? */\n\tWARN_ON(1);\n\treturn -EINVAL;\n",
     "\tif (of_device_is_compatible(pdev->dev.of_node, \"qcom,msm-vidc\")) {\n"
     "\t\tmsm_vidc_remove_video_device(pdev);\n\t\treturn;\n"
     "\t} else if (of_device_is_compatible(pdev->dev.of_node,\n"
     "\t\t\t\t\"qcom,msm-vidc,context-bank\")) {\n"
     "\t\tmsm_vidc_remove_context_bank(pdev);\n\t\treturn;\n"
     "\t}\n\n\t/* How did we end up here? */\n\tWARN_ON(1);\n"),
    ("video-driver-515/driver/vidc/src/msm_vidc_dt.c",
     "\tbus = cb->dev->bus;", "\tbus = (struct bus_type *)cb->dev->bus;\t/* k618: const since 6.4 */"),
    ("video-driver-515/driver/vidc/src/msm_vidc_debug.c",
     "struct dentry* msm_vidc_debugfs_init_drv()",
     "struct dentry* msm_vidc_debugfs_init_drv(void)"),
    ("video-driver-515/driver/vidc/src/msm_vidc_memory.c",
     "#include <linux/dma-buf.h>", "#include <linux/dma-buf.h>\n#include <linux/vmalloc.h>"),
    # smcinvoke's compat layer both registers its qseecom entry points with soc's qseecom_proxy
    # (provide_qseecom_kernel_fun_ops, which is what the proxy forwards through) and exports them
    # under their own names -- and the proxy exports those names too, so whichever of the two is
    # insmod'ed second fails with "exports duplicate symbol", which in the first stage is fatal.
    # The phone's own modules show which way round it goes: its qseecom_proxy.ko exports
    # qseecom_start_app, _shutdown_app and _send_command, and its smcinvoke_dlkm.ko exports none of
    # them. So the registration stays and the four exports go.
    ("securemsm-kernel/smcinvoke/compat/smcinvoke_kernel.c",
     "EXPORT_SYMBOL_GPL(qseecom_start_app);",
     "/* k618: qseecom_proxy exports this one; this file registers the implementation */"),
    ("securemsm-kernel/smcinvoke/compat/smcinvoke_kernel.c",
     "EXPORT_SYMBOL_GPL(qseecom_shutdown_app);",
     "/* k618: qseecom_proxy exports this one */"),
    ("securemsm-kernel/smcinvoke/compat/smcinvoke_kernel.c",
     "EXPORT_SYMBOL_GPL(qseecom_send_command);",
     "/* k618: qseecom_proxy exports this one */"),
    ("securemsm-kernel/smcinvoke/compat/smcinvoke_kernel.c",
     "EXPORT_SYMBOL_GPL(qseecom_process_listener_from_smcinvoke);",
     "/* k618: qseecom_proxy exports this one */"),
    # the dma-buf exports live in a namespace a module has to name (as soc's own heaps do)
    ("video-driver-515/driver/vidc/src/msm_vidc_probe.c",
     "MODULE_SOFTDEP(\"pre: subsys-pil-tz msm-mmrm\");",
     "MODULE_IMPORT_NS(\"DMA_BUF\");\nMODULE_SOFTDEP(\"pre: subsys-pil-tz msm-mmrm\");"),
    # 6.12 does not reach vmalloc.h through the headers this file includes
    ("video-driver-515/driver/vidc/src/msm_vidc_driver.c",
     "#include <linux/iommu.h>", "#include <linux/iommu.h>\n#include <linux/vmalloc.h>"),
    # linux/dma-iommu.h went away in 6.2; what this file uses of it is in iommu.h and dma-mapping.h
    ("video-driver-515/driver/vidc/src/msm_vidc_dt.c",
     "#include <linux/dma-iommu.h>\n",
     "/* k618: linux/dma-iommu.h is gone since 6.2; iommu.h and dma-mapping.h have what is used */\n"),
    # the 5.15 EVA driver on 6.12: the same three changes as everywhere else
    ("eva-kernel-515/msm/eva/cvp.c",
     "class_create(THIS_MODULE, CLASS_NAME)", "class_create(CLASS_NAME)"),
    ("eva-kernel-515/msm/eva/cvp.c",
     "static int msm_cvp_remove(struct platform_device *pdev)",
     "static void msm_cvp_remove(struct platform_device *pdev)"),
    ("eva-kernel-515/msm/eva/msm_cvp_core.c", "strlcpy(", "strscpy("),


    ("video-driver-515/driver/vidc/src/msm_vidc_dt.c",
     "\tkres = platform_get_resource(pdev, IORESOURCE_IRQ, 0);\n\tdt->irq = kres ? kres->start : -1;\n",
     "\tdt->irq = platform_get_irq(pdev, 0);\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_res_parse.c",
     "\tkres = platform_get_resource(pdev, IORESOURCE_IRQ, 0);\n\tres->irq = kres ? kres->start : -1;\n",
     "\tres->irq = platform_get_irq(pdev, 0);\n"),
    # the three value-returns left in that .remove (and the variable they returned)
    ("eva-kernel-515/msm/eva/cvp.c",
     "msm_cvp_remove(struct platform_device *pdev)\n{\n\tint rc = 0;\n\tstruct msm_cvp_core *core;\n",
     "msm_cvp_remove(struct platform_device *pdev)\n{\n\tstruct msm_cvp_core *core;\n"),
    ("eva-kernel-515/msm/eva/cvp.c",
     "\t\tdprintk(CVP_ERR, \"%s invalid input %pK\", __func__, pdev);\n\t\treturn -EINVAL;\n",
     "\t\tdprintk(CVP_ERR, \"%s invalid input %pK\", __func__, pdev);\n\t\treturn;\n"),
    ("eva-kernel-515/msm/eva/cvp.c",
     "\t\tdprintk(CVP_ERR, \"%s invalid core\", __func__);\n\t\treturn -EINVAL;\n",
     "\t\tdprintk(CVP_ERR, \"%s invalid core\", __func__);\n\t\treturn;\n"),
    ("eva-kernel-515/msm/eva/cvp.c",
     "\tmutex_destroy(&core->clk_lock);\n\tkfree(core);\n\treturn rc;\n}",
     "\tmutex_destroy(&core->clk_lock);\n\tkfree(core);\n}"),
    # iommu_map() takes the flags for its own page-table allocations since 6.3
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\t\t\t\t\tIOMMU_READ | IOMMU_WRITE);\n",
     "\t\t\t\t\tIOMMU_READ | IOMMU_WRITE, GFP_KERNEL);\n"),
    # 6.12 looks an interconnect path up by name, not by master/slave id -- the same call
    # Qualcomm's own 6.12 EVA release makes. A node with no interconnects/interconnect-names (this
    # phone's cvp@ab00000 has the 5.15-era qcom,bus-master / qcom,bus-slave children instead) gets
    # a NULL path, which the interconnect core documents as "skip setting constraints":
    # icc_set_bw() and icc_put() take NULL and do nothing, so EVA then runs without bus scaling
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\t\tbus->client = icc_get(&device->res->pdev->dev,\n"
     "\t\t\t\tbus->master, bus->slave);\n"
     "\t\tif (IS_ERR_OR_NULL(bus->client)) {\n"
     "\t\t\trc = PTR_ERR(bus->client) ?: -EBADHANDLE;\n",
     "\t\tbus->client = of_icc_get(&device->res->pdev->dev, bus->name);\n"
     "\t\tif (!bus->client)\n"
     "\t\t\tdprintk(CVP_WARN, \"bus %s: no interconnect path in the node, \"\n"
     "\t\t\t\t\t\"running without bus scaling\\n\", bus->name);\n"
     "\t\tif (IS_ERR(bus->client)) {\n"
     "\t\t\trc = PTR_ERR(bus->client);\n"),
    # a bus with no path has nothing to vote for, and both vote paths give up on an error
    ("eva-kernel-515/msm/eva/msm_cvp_clocks.c",
     "\tif (!bus->client)\n\t\treturn -EINVAL;\n",
     "\tif (!bus->client)\t\t/* k618: no interconnect path, nothing to vote */\n\t\treturn 0;\n"),
    # struct device's bus is const since 6.4, and this one is only read
    ("eva-kernel-515/msm/eva/msm_cvp_res_parse.c",
     "\tstruct bus_type *bus;\n", "\tconst struct bus_type *bus;\n"),
    # synx: the driver we build is this release's synx-driver, whose API takes a session pointer
    # from synx_initialize() and imports a fence through synx_import_params.indv -- the form
    # Qualcomm's own 6.12 EVA uses. (Their IS_ERR_OR_NULL takes the address of the pointer, which
    # is never NULL; written here on the pointer itself, which is what the call can return.)
    ("eva-kernel-515/msm/eva/msm_cvp_internal.h",
     "\tstruct synx_session synx_session_id;",
     "\tstruct synx_session *synx_session_id;"),
    ("eva-kernel-515/msm/eva/msm_cvp_synx.c",
     "\tstruct synx_initialization_params params;\n"
     "\n\tparams.name = \"cvp-kernel-client\";\n"
     "\tif (synx_initialize(&inst->synx_session_id, &params)) {\n",
     "\tstruct synx_initialization_params params = { 0 };\n"
     "\n\tparams.name = \"cvp-kernel-client\";\n"
     "\tparams.id = SYNX_CLIENT_EVA_CTX0;\n"
     "\tinst->synx_session_id = synx_initialize(&params);\n"
     "\tif (IS_ERR_OR_NULL(inst->synx_session_id)) {\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_synx.c",
     "\tdprintk(CVP_WARN, \"inst %x fence q mode %d, ssid %d\\n\",\n"
     "\t\t\thash32_ptr(inst->session), q->mode, ssid.client_id);\n",
     "\tdprintk(CVP_WARN, \"inst %x fence q mode %d, ssid %pK\\n\",\n"
     "\t\t\thash32_ptr(inst->session), q->mode, ssid);\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_synx.c",
     "\tstruct synx_import_params params;\n", "\tstruct synx_import_params params = { 0 };\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_synx.c",
     "\t\t\tparams.h_synx = h_synx;\n"
     "\t\t\tparams.secure_key = fs[i].secure_key;\n"
     "\t\t\tparams.new_h_synx = &fc->synx[i];\n",
     "\t\t\tparams.type = SYNX_IMPORT_INDV_PARAMS;\n"
     "\t\t\tparams.indv.fence = &h_synx;\n"
     "\t\t\tparams.indv.flags = SYNX_IMPORT_SYNX_FENCE\n"
     "\t\t\t\t\t| SYNX_IMPORT_LOCAL_FENCE;\n"
     "\t\t\tparams.indv.new_h_synx = &fc->synx[i];\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_synx.c",
     "static int cvp_wait_synx(struct synx_session ssid,",
     "static int cvp_wait_synx(struct synx_session *ssid,"),
    ("eva-kernel-515/msm/eva/msm_cvp_synx.c",
     "static int cvp_signal_synx(struct synx_session ssid,",
     "static int cvp_signal_synx(struct synx_session *ssid,"),
    # do_exit() is not a module's to call (and is no longer exported); both of these sit at the end
    # of a kthread that returns right after -- which is how a kthread ends, and what their own 6.12
    # EVA does with the same two functions
    ("eva-kernel-515/msm/eva/msm_cvp.c",
     "\tcvp_put_inst(inst);\n\tdo_exit(rc);\n\treturn rc;\n",
     "\tcvp_put_inst(inst);\n\t/* k618: a kthread ends by returning */\n\treturn rc;\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_dsp.c",
     "\tdprintk(CVP_DBG, \"dsp thread exit\\n\");\n\tdo_exit(rc);\n\treturn rc;\n",
     "\tdprintk(CVP_DBG, \"dsp thread exit\\n\");\n"
     "\t/* k618: a kthread ends by returning */\n\treturn rc;\n"),
    # the dma-buf exports live in a namespace a module has to name
    ("eva-kernel-515/msm/eva/cvp.c",
     "MODULE_LICENSE(\"GPL v2\");",
     "MODULE_IMPORT_NS(\"DMA_BUF\");\nMODULE_LICENSE(\"GPL v2\");"),
    # the fd lookup: 6.12 dropped the _rcu name (files_lookup_fd_raw() under rcu_read_lock() is
    # it) and get_file_rcu_many() went with the refcount rework in 6.7
    ("eva-kernel-515/msm/eva/msm_cvp_buf.c",
     "#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0))\n"
     "\tfile = fcheck_files(files, fd);\n"
     "#else\n"
     "\tfile = files_lookup_fd_rcu(files, fd);\n"
     "#endif\n",
     "\tfile = files_lookup_fd_raw(files, fd);\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_buf.c",
     "\t\telse if (!get_file_rcu_many(file, refs))\n\t\t\tgoto loop;\n",
     "\t\t/*\n"
     "\t\t * k618: this is what get_file_rcu_many() was before 6.7. The 6.12 get_file_rcu()\n"
     "\t\t * cannot stand in: it reloads the pointer from the slot it is handed and loops\n"
     "\t\t * until it can take a reference, and a local slot never changes.\n"
     "\t\t */\n"
     "\t\telse if (!atomic_long_add_unless(&file->f_count, refs, 0))\n\t\t\tgoto loop;\n"),
    # hyp_assign_phys() is Qualcomm's own 5.15 call; their 6.12 EVA uses the kernel's
    # qcom_scm_assign_mem() with the same two VM lists, which is what soc's qcom-scm provides
    ("eva-kernel-515/msm/eva/msm_cvp_dsp.c",
     "#include <soc/qcom/secure_buffer.h>",
     "#include <linux/firmware/qcom/qcom_scm.h>\n#include <soc/qcom/secure_buffer.h>"),
    ("eva-kernel-515/msm/eva/msm_cvp_dsp.c",
     "\tif (!me->hyp_assigned) {\n"
     "\t\trc = hyp_assign_phys(addr, size, hlosVM, HLOS_VM_NUM, dspVM,\n"
     "\t\t\tdspVMperm, DSP_VM_NUM);\n",
     "\tuint64_t hlosVMid = BIT(VMID_HLOS);\n"
     "\tstruct qcom_scm_vmperm dspVMlist[DSP_VM_NUM] = {\n"
     "\t\t{VMID_HLOS, PERM_READ | PERM_WRITE | PERM_EXEC},\n"
     "\t\t{VMID_CDSP_Q6, PERM_READ | PERM_WRITE | PERM_EXEC}\n"
     "\t};\n"
     "\n"
     "\tif (!me->hyp_assigned) {\n"
     "\t\trc = qcom_scm_assign_mem(addr, size, &hlosVMid, dspVMlist,\n"
     "\t\t\tDSP_VM_NUM);\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_dsp.c",
     "\tif (me->hyp_assigned) {\n"
     "\t\trc = hyp_assign_phys(me->addr, me->size, dspVM, DSP_VM_NUM,\n"
     "\t\t\t\thlosVM, hlosVMperm, HLOS_VM_NUM);\n",
     "\tuint64_t dspVMids = BIT(VMID_HLOS) | BIT(VMID_CDSP_Q6);\n"
     "\tstruct qcom_scm_vmperm hlosVMlist[HLOS_VM_NUM] = {\n"
     "\t\t{VMID_HLOS, PERM_READ | PERM_WRITE | PERM_EXEC},\n"
     "\t};\n"
     "\n"
     "\tif (me->hyp_assigned) {\n"
     "\t\trc = qcom_scm_assign_mem(me->addr, me->size, &dspVMids,\n"
     "\t\t\t\thlosVMlist, HLOS_VM_NUM);\n"),
    # hfi_adapter.h takes the HFI_TX_FLAGS_* values from hfi_packet.h, but includes it only under
    # CONFIG_MDSS_HFI_ADAPTER while using them unguarded -- so the header does not compile for a
    # chipset without the newer HFI stack (kalama). Guard the enum as its includes are; every user of
    # it is in the msm/*/hfi files, which are built under the same option
    ("display-drivers/msm/hfi/hfi_adapter.h",
     "enum hfi_host_flags {\n"
     "\tHFI_HOST_FLAGS_NONE = HFI_TX_FLAGS_NONE,\n",
     "#if IS_ENABLED(CONFIG_MDSS_HFI_ADAPTER)\n"
     "enum hfi_host_flags {\n"
     "\tHFI_HOST_FLAGS_NONE = HFI_TX_FLAGS_NONE,\n"),
    ("display-drivers/msm/hfi/hfi_adapter.h",
     "\tHFI_HOST_FLAGS_NON_DISCARDABLE = HFI_TX_FLAGS_NON_DISCARDABLE,\n};\n",
     "\tHFI_HOST_FLAGS_NON_DISCARDABLE = HFI_TX_FLAGS_NON_DISCARDABLE,\n};\n"
     "#endif\n"),
    # display_modules.bzl lists msm/dp/edp_pll_4nm.c and edp_pll_5nm.c; the Kbuild's object list has
    # only the dp_pll_* ones, and dp_pll.c calls into both edp ones (modpost:
    # edp_pll_clock_register_4nm undefined)
    ("display-drivers/msm/Kbuild",
     "\t\t\t\tdp/dp_pll_4nm.o \\\n\t\t\t\tdp/dp_pll_3nm.o\n",
     "\t\t\t\tdp/dp_pll_4nm.o \\\n\t\t\t\tdp/dp_pll_3nm.o \\\n"
     "\t\t\t\tdp/edp_pll_4nm.o \\\n\t\t\t\tdp/edp_pll_5nm.o\n"),
    # a platform driver's .remove returns void since 6.10; sde_rsc_remove right above it was
    # updated for that, sde_rsc_rpmh_remove was missed
    ("display-drivers/msm/sde_rsc.c",
     "int sde_rsc_rpmh_remove(struct platform_device *pdev)\n{\n\tint i;\n\n"
     "\tfor (i = 0; i < MAX_RSC_COUNT; i++)\n\t\trpmh_dev[i] = NULL;\n\n\treturn 0;\n}\n",
     "#if (KERNEL_VERSION(6, 10, 0) <= LINUX_VERSION_CODE)\n"
     "void sde_rsc_rpmh_remove(struct platform_device *pdev)\n"
     "#else\n"
     "int sde_rsc_rpmh_remove(struct platform_device *pdev)\n"
     "#endif\n{\n\tint i;\n\n"
     "\tfor (i = 0; i < MAX_RSC_COUNT; i++)\n\t\trpmh_dev[i] = NULL;\n\n"
     "#if (KERNEL_VERSION(6, 10, 0) > LINUX_VERSION_CODE)\n\treturn 0;\n#endif\n}\n"),
    # the kalama sound-card driver is 5.15-era and was not moved along with the rest of this release:
    # three API changes keep it (and so all 31 audio modules) from building.
    # 1) asoc_rtd_to_codec() is snd_soc_rtd_to_codec() since 6.5
    ("audio-kernel/asoc/kalama.c",
     "asoc_rtd_to_codec(rtd, 0);\n",
     "snd_soc_rtd_to_codec(rtd, 0);\n"),
    # 2) the display-port back end is LPASS_BE_DISPLAY_PORT_RX_0 now (as sun.c has it); with the old
    #    name undefined, ext_disp_be_dai_link[] has no definition and three ARRAY_SIZE uses fail too
    ("audio-kernel/asoc/kalama.c",
     "\t\t.name = LPASS_BE_DISPLAY_PORT_RX,\n\t\t.stream_name = LPASS_BE_DISPLAY_PORT_RX,\n",
     "\t\t.name = LPASS_BE_DISPLAY_PORT_RX_0,\n\t\t.stream_name = LPASS_BE_DISPLAY_PORT_RX_0,\n"),
    # 3) a platform driver's .remove returns void since 6.10
    ("audio-kernel/asoc/kalama.c",
     "static int msm_asoc_machine_remove(struct platform_device *pdev)\n",
     "#if (KERNEL_VERSION(6, 10, 0) <= LINUX_VERSION_CODE)\n"
     "static void msm_asoc_machine_remove(struct platform_device *pdev)\n"
     "#else\n"
     "static int msm_asoc_machine_remove(struct platform_device *pdev)\n"
     "#endif\n"),
    ("audio-kernel/asoc/kalama.c",
     "\tsnd_soc_unregister_card(card);\n\n\treturn 0;\n}\n",
     "\tsnd_soc_unregister_card(card);\n\n"
     "#if (KERNEL_VERSION(6, 10, 0) > LINUX_VERSION_CODE)\n\treturn 0;\n#endif\n}\n"),
    # 6.12 no longer reaches of.h through of_platform.h, and this file calls of_device_is_compatible
    ("graphics-kernel/governor_msm_adreno_tz.c",
     "#include <linux/of_platform.h>\n",
     "#include <linux/of.h>\n#include <linux/of_platform.h>\n"),
    # OnePlus replaced Qualcomm's DSI_CTRL_HW_ERR instead of adding to it: the definition now exists
    # only inside their OPLUS_FEATURE_DISPLAY block (which also #undefs it), so with their feature
    # off the macro is gone. Put Qualcomm's definition back -- the same as their own #else branch
    # btfm_slim_codec.c picks its snd_soc_dai op signatures with KERNEL_VERSION() but never includes
    # version.h, so the macro is undefined, the test takes the old branch and the ops do not match
    ("bt-kernel/slimbus/btfm_slim_codec.c",
     "#include <linux/init.h>\n",
     "#include <linux/version.h>\n#include <linux/init.h>\n"),
    # cnss2's pci.c defines cnss_pci_get_link_status from generic PCIe calls, unguarded, while
    # pci_platform.h also defines it as a stub when CONFIG_PCI_MSM is off (this phone has no
    # pci-msm-drv) -- a duplicate. The real one is the generic code, so drop the stub
    ("wlan/platform/cnss2/pci_platform.h",
     "int cnss_pci_get_link_status(struct cnss_pci_data *pci_priv)\n{\n\treturn 0;\n}\n"
     "#endif /* CONFIG_PCI_MSM */\n",
     "/* k618: pci.c has this one, from generic PCIe calls and with no CONFIG_PCI_MSM guard */\n"
     "#endif /* CONFIG_PCI_MSM */\n"),
    # the generic (non-PCI_MSM) PCIe backend calls of_translate_dma_region, which the kernel exports
    # but declares in of_address.h -- a header it does not include
    ("wlan/platform/cnss2/pci_qcom_ecam.c",
     "#include \"pci_platform.h\"\n",
     "#include <linux/of_address.h>\n#include \"pci_platform.h\"\n"),


    ("bt-kernel/pwr/btpower.c",
     "\tif (pinctrl1) {\n",
     "\tif (!IS_ERR_OR_NULL(pinctrl1)) {\n"),


    ("securemsm-kernel/include/uapi/linux/smcinvoke.h",
     "\t__u64 args;\n\t__s64 reserved;\n};",
     "\t__u64 args;\n};"),
    ("securemsm-kernel/include/uapi/linux/smcinvoke.h",
     "struct smcinvoke_accept {\n\t__u32 has_resp;\n\t__s32 result;\n\t__u32 op;\n\t__u32 counts;\n"
     "\t__s32 reserved;\n\t__u32 argsize;\n\t__u64 txn_id;\n\t__s64 cbobj_id;\n",
     "struct smcinvoke_accept {\n\t__u32 has_resp;\n\t__u32 txn_id;\n\t__s32 result;\n"
     "\t__s32 cbobj_id;\n\t__u32 op;\n\t__u32 counts;\n\t__s32 reserved;\n\t__u32 argsize;\n"),
    ("securemsm-kernel/include/uapi/linux/smcinvoke.h",
     "struct smcinvoke_server {\n\t__u64 cb_buf_size;\n\t__s64 reserved;\n};",
     "struct smcinvoke_server {\n\t__u32 cb_buf_size;\n};"),
    ("securemsm-kernel/include/uapi/linux/smcinvoke.h",
     "\t_IOWR(SMCINVOKE_IOC_MAGIC, 4, __s64)",
     "\t_IOWR(SMCINVOKE_IOC_MAGIC, 4, __s32)"),


    ("securemsm-kernel/include/uapi/linux/smcinvoke.h",
     "\t__s64 fd;\n\t__s64 cb_server_fd;\n\t__s64 reserved;\n};",
     "\t__s64 fd;\n\t__s32 cb_server_fd;\n\t__s32 reserved;\n};"),


    ("display-drivers/msm/msm_atomic.c",
     "void msm_atomic_commit_tail(struct drm_atomic_state *state)\n{\n",
     "void msm_atomic_commit_tail(struct drm_atomic_state *state)\n{\n"
     "\tpr_err_once(\"k618_disp: first atomic commit\\n\");\n"),
    ("display-drivers/msm/dsi/dsi_display.c",
     "int dsi_display_prepare(struct dsi_display *display)\n{\n",
     "int dsi_display_prepare(struct dsi_display *display)\n{\n"
     "\tpr_err(\"k618_disp: prepare\\n\");\n"),
    ("display-drivers/msm/dsi/dsi_display.c",
     "int dsi_display_enable(struct dsi_display *display)\n{\n",
     "int dsi_display_enable(struct dsi_display *display)\n{\n"
     "\tpr_err(\"k618_disp: display enable\\n\");\n"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     "int dsi_panel_enable(struct dsi_panel *panel)\n{\n",
     "int dsi_panel_enable(struct dsi_panel *panel)\n{\n"
     "\tpr_err(\"k618_disp: panel enable\\n\");\n"),


    ("display-drivers/msm/dsi/dsi_panel.c",
     "#include \"dsi_panel.h\"\n",
     "#include <linux/moduleparam.h>\n"
     "#include \"dsi_panel.h\"\n\n"
     "static uint k618_bl_level;\n"
     "module_param(k618_bl_level, uint, 0644);\n"
     "MODULE_PARM_DESC(k618_bl_level, \"brightness to set when the panel comes up"
     " (0: leave it alone; this phone's own driver extension is not published)\");\n"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     "\tpanel->panel_initialized = true;\n",
     "\tpanel->panel_initialized = true;\n\n"
     "\tif (k618_bl_level && panel->bl_config.type == DSI_BACKLIGHT_DCS) {\n"
     "\t\tint k618_rc = dsi_panel_set_backlight(panel, k618_bl_level);\n\n"
     "\t\tDSI_INFO(\"k618: brightness %u of %u: %d\\n\", k618_bl_level,\n"
     "\t\t\t panel->bl_config.bl_max_level, k618_rc);\n"
     "\t}\n"),


    ("display-drivers/msm/sde/sde_kms.c",
     "module_param(sdecustom, bool, 0400);\n",
     "module_param(sdecustom, bool, 0400);\n\n"
     "static bool k618_cont_splash = true;\n"
     "module_param(k618_cont_splash, bool, 0400);\n"
     "MODULE_PARM_DESC(k618_cont_splash, \"take over a display the bootloader left running"
     " (0: map its splash memory but power the panel up here, for a fastboot-booted image)\");\n"),
    ("display-drivers/msm/sde/sde_kms.c",
     "\trc = _sde_kms_mmu_init(sde_kms);\n"
     "\tif (rc) {\n"
     "\t\tSDE_ERROR(\"sde_kms_mmu_init failed: %d\\n\", rc);\n"
     "\t\tgoto power_error;\n"
     "\t}\n",
     "\trc = _sde_kms_mmu_init(sde_kms);\n"
     "\tif (rc) {\n"
     "\t\tSDE_ERROR(\"sde_kms_mmu_init failed: %d\\n\", rc);\n"
     "\t\tgoto power_error;\n"
     "\t}\n\n"
     "\t/*\n"
     "\t * k618: the splash memory is mapped one-to-one by now (mmu init maps it for every display\n"
     "\t * the devicetree gives a splash region; the CTL check comes after), so a scan-out the\n"
     "\t * bootloader left running keeps reading valid memory. The display itself is not handed\n"
     "\t * over: nothing is left for that check to claim, and the panel is powered up here.\n"
     "\t */\n"
     "\tif (!k618_cont_splash) {\n"
     "\t\tDRM_INFO(\"k618: splash memory mapped one-to-one for %d display(s);\"\n"
     "\t\t\t \" none handed over, the panel is powered up here\\n\",\n"
     "\t\t\t sde_kms->splash_data.num_splash_displays);\n"
     "\t\tmemset(sde_kms->splash_data.splash_display, 0,\n"
     "\t\t       sizeof(sde_kms->splash_data.splash_display));\n"
     "\t\tsde_kms->splash_data.num_splash_displays = 0;\n"
     "\t}\n"),


    ("display-drivers/msm/sde/sde_kms.c",
     "\t/* All CRTCs are compatible with all encoders */\n"
     "\tfor (i = 0; i < priv->num_encoders; i++)\n"
     "\t\tpriv->encoders[i]->possible_crtcs = (1 << priv->num_crtcs) - 1;\n",
     "\t/* All CRTCs are compatible with all encoders */\n"
     "\tfor (i = 0; i < priv->num_encoders; i++)\n"
     "\t\tpriv->encoders[i]->possible_crtcs = (1 << priv->num_crtcs) - 1;\n"
     "\n"
     "\t/* k618: and every encoder may be a clone of every other (concurrent writeback) */\n"
     "\t{\n"
     "\t\tu32 clones = 0;\n"
     "\n"
     "\t\tfor (i = 0; i < priv->num_encoders; i++)\n"
     "\t\t\tclones |= drm_encoder_mask(priv->encoders[i]);\n"
     "\t\tfor (i = 0; i < priv->num_encoders; i++)\n"
     "\t\t\tpriv->encoders[i]->possible_clones = clones;\n"
     "\t}\n"),


    ("graphics-kernel/kgsl_util.c",
     "\tret = qcom_scm_kgsl_init_regs(gpu_req);\n"
     "\tif (ret)\n"
     "\t\tdev_err(dev, \"Scm call for requests:0x%x failed with ret:: %d\\n\",\n"
     "\t\t\t\t\t\t\t\t\tgpu_req, ret);\n"
     "\n"
     "\treturn ret;\n",
     "\tret = qcom_scm_kgsl_init_regs(gpu_req);\n"
     "\tif (ret)\n"
     "\t\tdev_err(dev, \"Scm call for requests:0x%x failed with ret:: %d\\n\",\n"
     "\t\t\t\t\t\t\t\t\tgpu_req, ret);\n"
     "\n"
     "\t/* k618: this phone's trusted side does not have it; its own driver never asks */\n"
     "\treturn ret ? -EOPNOTSUPP : 0;\n"),


    ("graphics-kernel/adreno.c",
     "\tstatus = adreno_of_get_power(adreno_dev, pdev);\n",
     "\tstatus = adreno_of_get_power(adreno_dev, pdev);\n"
     "\tif (status)\n\t\tpr_err(\"k618_kgsl: adreno_of_get_power -> %d\\n\", status);\n"),
    ("graphics-kernel/adreno.c",
     "\tstatus = kgsl_bus_init(device, pdev);\n",
     "\tstatus = kgsl_bus_init(device, pdev);\n"
     "\tif (status)\n\t\tpr_err(\"k618_kgsl: kgsl_bus_init -> %d\\n\", status);\n"),
    # (this call spans two lines, so the mark goes after the whole statement)
    ("graphics-kernel/adreno.c",
     "\tstatus = kgsl_regmap_init(pdev, &device->regmap, \"kgsl_3d0_reg_memory\",\n"
     "\t\t&adreno_regmap_ops, device);\n",
     "\tstatus = kgsl_regmap_init(pdev, &device->regmap, \"kgsl_3d0_reg_memory\",\n"
     "\t\t&adreno_regmap_ops, device);\n"
     "\tif (status)\n\t\tpr_err(\"k618_kgsl: kgsl_regmap_init -> %d\\n\", status);\n"),
    ("graphics-kernel/adreno.c",
     "\tstatus = adreno_probe_llcc(adreno_dev, pdev);\n",
     "\tstatus = adreno_probe_llcc(adreno_dev, pdev);\n"
     "\tif (status)\n\t\tpr_err(\"k618_kgsl: adreno_probe_llcc -> %d\\n\", status);\n"),
    ("graphics-kernel/adreno.c",
     "\tstatus = component_bind_all(dev, NULL);\n",
     "\tstatus = component_bind_all(dev, NULL);\n"
     "\tif (status)\n\t\tpr_err(\"k618_kgsl: component_bind_all -> %d\\n\", status);\n"),
    ("graphics-kernel/adreno.c",
     "\tstatus = adreno_irq_setup(pdev, adreno_dev);\n",
     "\tstatus = adreno_irq_setup(pdev, adreno_dev);\n"
     "\tif (status < 0)\n\t\tpr_err(\"k618_kgsl: adreno_irq_setup -> %d\\n\", status);\n"),
    ("graphics-kernel/adreno.c",
     "\tstatus = kgsl_device_platform_probe(device);\n",
     "\tstatus = kgsl_device_platform_probe(device);\n"
     "\tif (status)\n\t\tpr_err(\"k618_kgsl: kgsl_device_platform_probe -> %d\\n\", status);\n"),
    # nxp-nci's probe, step by step (K618_NFC_STEP is k618_nfc_trace.h's, empty without
    # -DK618_NFC_TRACE): which of its hardware steps resets the phone
    ("../../nxp/opensource/driver/nfc/i2c_drv.c",
     "\tret = nfc_parse_dt(&client->dev,nfc_configs, PLATFORM_IF_I2C);\n",
     "\tK618_NFC_STEP(\"probe: parse dt\");\n\tret = nfc_parse_dt(&client->dev,nfc_configs, PLATFORM_IF_I2C);\n"),
    ("../../nxp/opensource/driver/nfc/i2c_drv.c",
     "\tret = configure_gpio(nfc_gpio->irq, GPIO_IRQ);\n",
     "\tK618_NFC_STEP(\"probe: irq gpio\");\n\tret = configure_gpio(nfc_gpio->irq, GPIO_IRQ);\n"),
    ("../../nxp/opensource/driver/nfc/i2c_drv.c",
     "\tret = nfc_misc_register(nfc_dev, &nfc_i2c_dev_fops, DEV_COUNT,\n",
     "\tK618_NFC_STEP(\"probe: misc device\");\n\tret = nfc_misc_register(nfc_dev, &nfc_i2c_dev_fops, DEV_COUNT,\n"),
    ("../../nxp/opensource/driver/nfc/i2c_drv.c",
     "\tret = request_irq(client->irq, i2c_irq_handler, IRQF_TRIGGER_HIGH,\n",
     "\tK618_NFC_STEP(\"probe: request_irq (level high)\");\n"
     "\tret = request_irq(client->irq, i2c_irq_handler, IRQF_TRIGGER_HIGH,\n"),
    ("../../nxp/opensource/driver/nfc/i2c_drv.c",
     "\tret = nfc_vbat_monitor_init(nfc_dev, nfc_gpio, client);\n",
     "\tK618_NFC_STEP(\"probe: vbat monitor\");\n\tret = nfc_vbat_monitor_init(nfc_dev, nfc_gpio, client);\n"),
    ("../../nxp/opensource/driver/nfc/i2c_drv.c",
     "\tret = nfc_ldo_config(&client->dev, nfc_dev);\n",
     "\tK618_NFC_STEP(\"probe: ldo\");\n\tret = nfc_ldo_config(&client->dev, nfc_dev);\n"),
    ("../../nxp/opensource/driver/nfc/i2c_drv.c",
     "\t    if(!nfc_hw_secure_check()) {\n",
     "\t    K618_NFC_STEP(\"probe: ask TZ about the secure zone\");\n\t    if(!nfc_hw_secure_check()) {\n"),
    ("../../nxp/opensource/driver/nfc/i2c_drv.c",
     "\tdevice_init_wakeup(&client->dev, true);\n",
     "\tK618_NFC_STEP(\"probe: done with the hardware\");\n\tdevice_init_wakeup(&client->dev, true);\n"),
    ("../../nxp/opensource/driver/nfc/common.c",
     "\tret = configure_gpio(nfc_gpio->ven, GPIO_OUTPUT);\n",
     "\tK618_NFC_STEP(\"post_init: ven gpio\");\n\tret = configure_gpio(nfc_gpio->ven, GPIO_OUTPUT);\n"),
    ("../../nxp/opensource/driver/nfc/common.c",
     "\tret = configure_gpio(nfc_gpio->dwl_req, GPIO_OUTPUT);\n",
     "\tK618_NFC_STEP(\"post_init: download gpio\");\n\tret = configure_gpio(nfc_gpio->dwl_req, GPIO_OUTPUT);\n"),
    ("../../nxp/opensource/driver/nfc/common.c",
     "        ret = msm_gpio_mpm_wake_set(clkreq_gpio, true);\n",
     "        K618_NFC_STEP(\"post_init: clkreq gpio as a wake-up (msm_gpio_mpm_wake_set)\");\n"
     "        ret = msm_gpio_mpm_wake_set(clkreq_gpio, true);\n"),
    ("../../nxp/opensource/driver/nfc/common.c",
     "\tret = nfcc_hw_check(nfc_dev);\n",
     "\tK618_NFC_STEP(\"post_init: hw check (ven toggles, first i2c to the chip)\");\n"
     "\tret = nfcc_hw_check(nfc_dev);\n"),
    ("../../nxp/opensource/driver/nfc/common.c",
     "\tpost_init_success = 1;\n",
     "\tK618_NFC_STEP(\"post_init: done\");\n\tpost_init_success = 1;\n"),
    # cnss2's debug.h includes OnePlus' own oplus_project.h unguarded (their IPC-logging branch),
    # and that header is not published. The only thing this file takes from it is get_eng_version()
    # with FACTORY/AGING, in the CNSS_ASSERT of their own commented-out block -- k618_oplus_stub.h,
    # force-included into this repo (k618_tp.sh), has both. main.c's and qmi.c's copies of the
    # include are inside #ifdef OPLUS_FEATURE_WIFI_MAC / _BDF, which are off, so this is the only
    # one that gets compiled. (It only surfaced once the techpacks saw the vendor configuration:
    # the include sits under IS_ENABLED(CONFIG_IPC_LOGGING).)
    ("wlan/platform/cnss2/debug.h",
     "#include <soc/oplus/system/oplus_project.h>\n",
     "/* k618: OnePlus' own header; get_eng_version() comes from k618_oplus_stub.h */\n"),
    ("display-drivers/msm/dsi/dsi_ctrl_hw.h",
     "#ifdef OPLUS_FEATURE_DISPLAY\n#undef DSI_CTRL_HW_ERR\n",
     "#define DSI_CTRL_HW_ERR(c, fmt, ...)\tDRM_DEV_ERROR(NULL, \"[msm-dsi-error]: DSI_%d: \"\\\n"
     "\t\tfmt, c ? c->index : -1,\t##__VA_ARGS__)\n"
     "#ifdef OPLUS_FEATURE_DISPLAY\n#undef DSI_CTRL_HW_ERR\n"),
    ("display-drivers/msm/sde/sde_hw_interrupts.c",
     "\tg_regmap1 = ioremap(SDE_REG_INTR2_STATUS_ADDR, 0X4);\n"
     "\tg_regmap2 = ioremap(SDE_REG_INTR_STATUS_ADDR, 0X4);\n"
     "\tg_regmap3 = ioremap(SDE_REG_HIST_STATUS_ADDR, 0x4);\n"
     "\tg_regmap4 = ioremap(SDE_REG_HIST_CLEAR_ADDR, 0x4);\n"
     "\tg_regmap5 = ioremap(SDE_REG_LTM0_STATUS_ADDR, 0x4);\n"
     "\tg_regmap6 = ioremap(SDE_REG_LTM0_CLEAR_ADDR, 0x4);\n"
     "\tg_regmap7 = ioremap(SDE_REG_LTM1_STATUS_ADDR, 0x4);\n"
     "\tg_regmap8 = ioremap(SDE_REG_LTM1_CLEAR_ADDR, 0x4);\n"
     "\tg_regmap9 = ioremap(SDE_REG_LTM2_STATUS_ADDR, 0x4);\n"
     "\tg_regmap10 = ioremap(SDE_REG_LTM2_CLEAR_ADDR, 0x4);\n"
     "\tg_regmap11 = ioremap(SDE_REG_LTM3_STATUS_ADDR, 0x4);\n"
     "\tg_regmap12 = ioremap(SDE_REG_LTM3_CLEAR_ADDR, 0x4);\n",
     "\t/* k618: canoe's fixed MDSS addresses (0x98xxxxx); this phone's MDSS is at 0xae00000 and\n"
     "\t * the first read aborts in the IRQ handler -- left unmapped, every use checks for NULL */\n"),


    ('display-drivers/msm/sde/sde_crtc.c',
     '\tif (copy_from_user(&ds_data, usr_ptr, sizeof(ds_data))) {\n'
     '\t\tSDE_ERROR("%s:failed to copy dest scaler data from user\\n",\n'
     '\t\t\tsde_crtc->name);\n'
     '\t\treturn -EINVAL;\n'
     '\t}\n',
     """	{
		/* Device compatibility. */
		struct k618_ds_cfg_515 {
			__u32 flags;
			__u32 index;
			__u32 lm_width;
			__u32 lm_height;
			__u64 scaler_cfg;
		};
		struct {
			__u32 num_dest_scaler;
			struct k618_ds_cfg_515 ds_cfg[SDE_MAX_DS_COUNT];
		} ds_515;
		int k;

		if (copy_from_user(&ds_515, usr_ptr, sizeof(ds_515))) {
			SDE_ERROR(\"%s:failed to copy dest scaler data from user\\n\",
				sde_crtc->name);
			return -EINVAL;
		}
		memset(&ds_data, 0, sizeof(ds_data));
		ds_data.num_dest_scaler = ds_515.num_dest_scaler;
		for (k = 0; k < SDE_MAX_DS_COUNT; k++) {
			ds_data.ds_cfg[k].flags = ds_515.ds_cfg[k].flags;
			ds_data.ds_cfg[k].index = ds_515.ds_cfg[k].index;
			ds_data.ds_cfg[k].lm_width = ds_515.ds_cfg[k].lm_width;
			ds_data.ds_cfg[k].lm_height = ds_515.ds_cfg[k].lm_height;
			ds_data.ds_cfg[k].scaler_cfg = ds_515.ds_cfg[k].scaler_cfg;
		}
	}
"""),
    ('display-drivers/msm/sde/sde_crtc.c',
     '\t\t\tif (copy_from_user(&scaler_v2, scaler_v2_usr,\n'
     '\t\t\t\t\tsizeof(scaler_v2))) {\n',
     "\t\t\t/* k618: 5.15's sde_drm_scaler_v2 ends at de_lpf_m */\n"
     '\t\t\tif (copy_from_user(&scaler_v2, scaler_v2_usr,\n'
     '\t\t\t\t\toffsetof(struct sde_drm_scaler_v2, dir45_en))) {\n'),
    ('display-drivers/msm/sde/sde_plane.c',
     '\tif (copy_from_user(&scale_v2, usr, sizeof(scale_v2))) {\n',
     '\t/*\n'
     "\t * k618: 5.15's sde_drm_scaler_v2 (the ROM's composer) ends at de_lpf_m; the\n"
     "\t * QSEED6/7 fields after it would be read from beyond the caller's struct\n"
     '\t */\n'
     '\tmemset(&scale_v2, 0, sizeof(scale_v2));\n'
     '\tif (copy_from_user(&scale_v2, usr, offsetof(struct sde_drm_scaler_v2, dir45_en))) {\n'),
    ('display-drivers/msm/sde/sde_crtc.c',
     '\tif (copy_from_user(&roi_v1, usr_ptr, sizeof(roi_v1))) {\n',
     "\t/* k618: 5.15's sde_drm_roi_v1 (the ROM's composer) has no SPR ROI after roi[] */\n"
     '\tmemset(&roi_v1, 0, sizeof(roi_v1));\n'
     '\tif (copy_from_user(&roi_v1, usr_ptr, offsetof(struct sde_drm_roi_v1, roi_feature_flags))) {\n'),
    ('display-drivers/msm/sde/sde_connector.c',
     '\tif (copy_from_user(&roi_v1, usr_ptr, sizeof(roi_v1))) {\n',
     "\t/* k618: 5.15's sde_drm_roi_v1 (the ROM's composer) has no SPR ROI after roi[] */\n"
     '\tmemset(&roi_v1, 0, sizeof(roi_v1));\n'
     '\tif (copy_from_user(&roi_v1, usr_ptr, offsetof(struct sde_drm_roi_v1, roi_feature_flags))) {\n'),
    ('display-drivers/msm/sde/sde_hw_ds.c',
     '\tcase DEST_SCALER_QUAD_PIPE:\n'
     '\t\top_mode = (BIT(0) | BIT(1));\n'
     '\t\tbreak;\n'
     '\t}\n',
     '\tcase DEST_SCALER_QUAD_PIPE:\n'
     '\t\top_mode = (BIT(0) | BIT(1));\n'
     '\t\tbreak;\n'
     '\tdefault:\n'
     '\t\t/*\n'
     "\t\t * k618: no merge mode (a 5.15 composer never sends one): 5.15's driver\n"
     "\t\t * wrote dual pipe here when both mixers' scalers run, and nothing else\n"
     '\t\t */\n'
     '\t\tif (!(op_mode & SDE_DS_OP_MODE_DUAL))\n'
     '\t\t\treturn;\n'
     '\t\top_mode = BIT(0);\n'
     '\t\tbreak;\n'
     '\t}\n'),
]

# securemsm-kernel keeps two copies of that ABI, word for word: the driver includes
# include/linux/smcinvoke.h, while a user-space build would take include/uapi/linux/smcinvoke.h.
# The four edits above have to land in both, or the driver keeps the sizes the ROM does not use.
EDITS += [("securemsm-kernel/include/linux/smcinvoke.h", old, new)
          for f, old, new in EDITS if f.endswith("uapi/linux/smcinvoke.h")]

GPIO_TBL_H = """
#include <linux/version.h>
#if KERNEL_VERSION(6, 18, 0) <= LINUX_VERSION_CODE
/**
 * struct gpio - a GPIO with its configuration, as <linux/gpio.h> had it up to 6.17
 * @gpio:	the GPIO number
 * @flags:	GPIO configuration as specified by GPIOF_*
 * @label:	a literal description string of this GPIO
 *
 * k618: 6.18 dropped this type along with gpio_request_array()/gpio_free_array(), its only
 * in-kernel consumers. The camera driver never passes it to the kernel: it is its own table,
 * read out of the gpio-req-tbl-{num,flags,label} properties (ten camera nodes on this phone,
 * two GPIOs each) and applied one GPIO at a time with gpio_request_one()/gpio_free(), which
 * 6.18 still has. Same members, same meaning as the kernel's own up to 6.17.
 */
struct gpio {
	unsigned int	gpio;
	unsigned long	flags;
	const char	*label;
};
#endif
"""

EDITS += [
    # Every one of the camera driver's 19 errors is this type. It goes into cam_debug_util.h
    # because that is the one header all three files that walk the table include, and the only one
    # that comes BEFORE cam_res_mgr_api.h in cam_res_mgr.c -- the prototypes there would otherwise
    # declare the tag at prototype scope and the definitions below would conflict with it.
    ("camera-kernel/drivers/cam_utils/cam_debug_util.h",
     "#include \"cam_trace.h\"\n\nextern unsigned long long debug_mdl;",
     "#include \"cam_trace.h\"\n" + GPIO_TBL_H + "\nextern unsigned long long debug_mdl;"),

    # --- the GPU driver. Two kernel changes: a bin_attribute's callbacks take it as const (6.13),
    # --- and a fence's 64-bit sequence numbers are a per-fence flag set by dma_fence_init64()
    # --- rather than an ops member, while ->fence_value_str / ->timeline_value_str were deleted
    # --- outright (6.15/6.16; the core now prints fence->seqno itself, and sw_sync reads its own
    # --- structures). So the driver recognises its own fences instead of going through the ops.
    ("graphics-kernel/kgsl_snapshot.c",
     "static ssize_t snapshot_show(struct file *filep, struct kobject *kobj,\n"
     "\tstruct bin_attribute *attr, char *buf, loff_t off,",
     "static ssize_t snapshot_show(struct file *filep, struct kobject *kobj,\n"
     "\tconst struct bin_attribute *attr, char *buf, loff_t off,"),
    ("graphics-kernel/kgsl_timeline.c",
     "\treturn !__dma_fence_is_later(fence->seqno, f->timeline->value,\n\t\tfence->ops);",
     "\treturn !__dma_fence_is_later(fence, fence->seqno, f->timeline->value);"),
    ("graphics-kernel/kgsl_timeline.c",
     "\t.enable_signaling = timeline_fence_enable_signaling,\n"
     "\t.timeline_value_str = timeline_get_value_str,\n"
     "\t.use_64bit_seqno = true,\n};",
     "\t.enable_signaling = timeline_fence_enable_signaling,\n};\n\n"
     "/* k618: 6.18 has no dma_fence_ops.timeline_value_str; our own fences are recognised here */\n"
     "void kgsl_timeline_fence_value_str(struct dma_fence *fence, char *str, int size)\n{\n"
     "\tif (fence->ops == &timeline_fence_ops)\n\t\ttimeline_get_value_str(fence, str, size);\n}"),
    # the flag that .use_64bit_seqno used to carry: without it the timeline's sequence numbers
    # would be compared as 32-bit ones and signalling would go wrong past 2^31
    ("graphics-kernel/kgsl_timeline.c",
     "\tdma_fence_init(&fence->base, &timeline_fence_ops,",
     "\tdma_fence_init64(&fence->base, &timeline_fence_ops,"),
    ("graphics-kernel/kgsl_timeline.h",
     "struct dma_fence *kgsl_timelines_to_fence_array(struct kgsl_device *device,\n"
     "\t\tu64 timelines, u32 count, u64 usize, bool any);",
     "struct dma_fence *kgsl_timelines_to_fence_array(struct kgsl_device *device,\n"
     "\t\tu64 timelines, u32 count, u64 usize, bool any);\n\n"
     "/* k618: stands in for the &dma_fence_ops.timeline_value_str callback 6.18 removed */\n"
     "void kgsl_timeline_fence_value_str(struct dma_fence *fence, char *str, int size);"),
    ("graphics-kernel/kgsl_sync.c", "#include \"kgsl_sync.h\"",
     "#include \"kgsl_sync.h\"\n#include \"kgsl_timeline.h\""),
    ("graphics-kernel/kgsl_sync.c",
     "static void kgsl_sync_fence_value_str(struct dma_fence *fence,\n"
     "\t\t\t\t\tchar *str, int size)\n{\n"
     "\tstruct kgsl_sync_fence *kfence = (struct kgsl_sync_fence *)fence;\n\n"
     "\tsnprintf(str, size, \"%u\", kfence->timestamp);\n}\n\n", ""),
    ("graphics-kernel/kgsl_sync.c",
     "\t.release = kgsl_sync_fence_release,\n\n"
     "\t.fence_value_str = kgsl_sync_fence_value_str,\n"
     "\t.timeline_value_str = kgsl_sync_timeline_value_str,\n};",
     "\t.release = kgsl_sync_fence_release,\n};"),
    ("graphics-kernel/kgsl_sync.c",
     "\tif (f->ops->fence_value_str) {\n"
     "\t\tlen += scnprintf(name + len, max_size - len, \": \");\n"
     "\t\tf->ops->fence_value_str(f, name + len, max_size - len);\n\t}\n\n}",
     "\t/*\n"
     "\t * k618: dma_fence_ops has no ->fence_value_str since 6.16; the core prints fence->seqno\n"
     "\t * in its place, and for a KGSL timestamp fence the seqno IS the timestamp this printed.\n"
     "\t */\n"
     "\tscnprintf(name + len, max_size - len, \": %llu\", f->seqno);\n}\n\n"
     "/* and this is where the ->timeline_value_str callback's dispatch went */\n"
     "void kgsl_get_fence_timeline_value(struct dma_fence *f, char *str, int size)\n{\n"
     "\tif (f->ops == &kgsl_sync_fence_ops)\n\t\tkgsl_sync_timeline_value_str(f, str, size);\n"
     "\telse\n\t\tkgsl_timeline_fence_value_str(f, str, size);\n}"),
    ("graphics-kernel/kgsl_sync.c",
     "static void kgsl_syncsource_fence_value_str(struct dma_fence *fence,\n"
     "\t\t\t\t\t\tchar *str, int size)\n{\n"
     "\t/*\n"
     "\t * Each fence is independent of the others on the same timeline.\n"
     "\t * We use a different context for each of them.\n"
     "\t */\n"
     "\tsnprintf(str, size, \"%llu\", fence->context);\n}\n\n", ""),
    ("graphics-kernel/kgsl_sync.c",
     "\t.release = kgsl_syncsource_fence_release,\n\n"
     "\t.fence_value_str = kgsl_syncsource_fence_value_str,\n};",
     "\t.release = kgsl_syncsource_fence_release,\n};"),
    ("graphics-kernel/kgsl_sync.h",
     "void kgsl_get_fence_name(struct dma_fence *f, char *name, u32 max_size);",
     "void kgsl_get_fence_name(struct dma_fence *f, char *name, u32 max_size);\n\n"
     "void kgsl_get_fence_timeline_value(struct dma_fence *f, char *str, int size);"),
    ("graphics-kernel/kgsl_sync.h",
     "void kgsl_get_fence_name(struct dma_fence *f, char *name, u32 max_size)\n{\n\n}",
     "void kgsl_get_fence_name(struct dma_fence *f, char *name, u32 max_size)\n{\n\n}\n\n"
     "void kgsl_get_fence_timeline_value(struct dma_fence *f, char *str, int size)\n{\n\n}"),
    # the three debug prints that asked a foreign fence for its timeline value
    ("graphics-kernel/adreno_gen7_hwsched_hfi.c",
     "\tif (fence->ops->timeline_value_str)\n"
     "\t\tfence->ops->timeline_value_str(fence, value, sizeof(value));",
     "\tkgsl_get_fence_timeline_value(fence, value, sizeof(value));"),
    ("graphics-kernel/adreno_gen8_hwsched_hfi.c",
     "\tif (fence->ops->timeline_value_str)\n"
     "\t\tfence->ops->timeline_value_str(fence, value, sizeof(value));",
     "\tkgsl_get_fence_timeline_value(fence, value, sizeof(value));"),
    ("graphics-kernel/adreno_hwsched.c",
     "\t\tif (fence->ops->timeline_value_str)\n"
     "\t\t\tfence->ops->timeline_value_str(fence, value, sizeof(value));",
     "\t\tkgsl_get_fence_timeline_value(fence, value, sizeof(value));"),

    # --- the data path. rtnl_link_ops.newlink takes its netlink arguments in one struct since
    # --- 6.14 (struct rtnl_newlink_params in net/rtnetlink.h); the three locals unpack exactly
    # --- the fields the old arguments were, so the body is untouched. (src_net, not
    # --- rtnl_newlink_link_net(params): that would also honour IFLA_LINK_NETNSID, which is a
    # --- behaviour change for this driver, not a port.)
    ("datarmnet/core/rmnet_config.c",
     "static int rmnet_newlink(struct net *src_net, struct net_device *dev,\n"
     "\t\t\t struct nlattr *tb[], struct nlattr *data[],\n"
     "\t\t\t struct netlink_ext_ack *extack)\n{\n"
     "\tstruct net_device *real_dev;",
     "static int rmnet_newlink(struct net_device *dev,\n"
     "\t\t\t struct rtnl_newlink_params *params,\n"
     "\t\t\t struct netlink_ext_ack *extack)\n{\n"
     "\t/* k618: one struct since 6.14 (struct rtnl_newlink_params) */\n"
     "\tstruct net *src_net = params->src_net;\n"
     "\tstruct nlattr **tb = params->tb;\n"
     "\tstruct nlattr **data = params->data;\n"
     "\tstruct net_device *real_dev;"),
    # softnet_data's atomic_t dropped became struct numa_drop_counters drop_counters in 6.18,
    # written through numa_drop_add() -- which is what net/core/dev.c does with the same field
    ("datarmnet-ext/shs/rmnet_shs_main.c",
     "\t\t\trps_input_queue_head_incr(sd);\n\t\t\tatomic_inc(&sd->dropped);",
     "\t\t\trps_input_queue_head_incr(sd);\n"
     "\t\t\t/* k618: softnet_data counts drops per node since 6.18 */\n"
     "\t\t\tnuma_drop_add(&sd->drop_counters, 1);"),
    # the two established-socket lookups lost their hashinfo argument: the table now comes from
    # the netns the first argument already names
    ("datarmnet-ext/perf/rmnet_perf_tcp.c",
     "\t\treturn inet_lookup_established(net, &tcp_hashinfo,\n",
     "\t\t/* k618: no hashinfo argument since 6.18 -- the netns carries the table */\n"
     "\t\treturn inet_lookup_established(net,\n"),
    ("datarmnet-ext/perf/rmnet_perf_tcp.c",
     "\treturn __inet6_lookup_established(net, &tcp_hashinfo, &tuple->v6_saddr,",
     "\treturn __inet6_lookup_established(net, &tuple->v6_saddr,"),

    # --- the video driver. v4l2_fh_add() / v4l2_fh_del() also take the struct file in 6.18, and
    # --- use it for one assignment each (filp->private_data = fh, then NULL) which this driver
    # --- already does itself in msm_v4l2_open() / msm_v4l2_close(). Neither call site here has
    # --- the file (the queue is set up from msm_vidc_open(), which is not handed it, and torn
    # --- down from the instance's kref release, which can run after the file is gone), so the
    # --- two helpers are kept as the kernel's own minus that assignment.
    ("video-driver-515/driver/vidc/src/msm_vidc_driver.c",
     "int msm_vidc_event_queue_init(struct msm_vidc_inst *inst)",
     "/*\n"
     " * k618: 6.18's v4l2_fh_add() and v4l2_fh_del() take the struct file as well and use it for\n"
     " * one assignment each -- filp->private_data = fh on add, NULL on del -- which this driver\n"
     " * already does in msm_v4l2_open() / msm_v4l2_close(). Neither call site here has the file,\n"
     " * so these do what the two helpers do, less that assignment.\n"
     " */\n"
     "static void msm_vidc_fh_add(struct v4l2_fh *fh)\n{\n"
     "\tunsigned long flags;\n\n"
     "\tv4l2_prio_open(fh->vdev->prio, &fh->prio);\n"
     "\tspin_lock_irqsave(&fh->vdev->fh_lock, flags);\n"
     "\tlist_add(&fh->list, &fh->vdev->fh_list);\n"
     "\tspin_unlock_irqrestore(&fh->vdev->fh_lock, flags);\n}\n\n"
     "static void msm_vidc_fh_del(struct v4l2_fh *fh)\n{\n"
     "\tunsigned long flags;\n\n"
     "\tspin_lock_irqsave(&fh->vdev->fh_lock, flags);\n"
     "\tlist_del_init(&fh->list);\n"
     "\tspin_unlock_irqrestore(&fh->vdev->fh_lock, flags);\n"
     "\tv4l2_prio_close(fh->vdev->prio, fh->prio);\n}\n\n"
     "int msm_vidc_event_queue_init(struct msm_vidc_inst *inst)"),
    ("video-driver-515/driver/vidc/src/msm_vidc_driver.c",
     "\tv4l2_fh_add(&inst->event_handler);", "\tmsm_vidc_fh_add(&inst->event_handler);"),
    ("video-driver-515/driver/vidc/src/msm_vidc_driver.c",
     "\tv4l2_fh_del(&inst->event_handler);", "\tmsm_vidc_fh_del(&inst->event_handler);"),
    # v4l2_ioctl_ops has no vidioc_queryctrl in 6.18: the control framework answers
    # VIDIOC_QUERYCTRL from the file handle's ctrl_handler, which this driver always sets, and
    # falls back only to vidioc_query_ext_ctrl -- so the callback was already dead code
    ("video-driver-515/driver/platform/common/src/msm_vidc_platform.c",
     "\t.vidioc_queryctrl               = msm_v4l2_queryctrl,\n", ""),


    ("video-driver-515/driver/platform/common/src/msm_vidc_platform.c",
     "static struct vb2_ops msm_vb2_ops = {\n",
     "/* k618: 5.15 called no wait op; 6.18 wants them (or a queue lock), and these do nothing */\n"
     "static void k618_vb2_wait_nop(struct vb2_queue *q)\n{\n}\n\n"
     "static struct vb2_ops msm_vb2_ops = {\n"
     "\t.wait_prepare                   = k618_vb2_wait_nop,\n"
     "\t.wait_finish                    = k618_vb2_wait_nop,\n"),


    ("audio-kernel/asoc/msm_dailink.h",
     '\t\t\t   COMP_CODEC("wcd939x_codec", "wcd939x_cdc"),\n',
     '\t\t\t   COMP_CODEC("wcd938x_codec", "wcd938x_cdc"),\n'
     '\t\t\t   COMP_CODEC("wcd939x_codec", "wcd939x_cdc"),\n'),
    ("audio-kernel/asoc/msm_dailink.h",
     '\t\t\tCOMP_CODEC("wcd939x_codec", "wcd939x_cdc"),\n',
     '\t\t\tCOMP_CODEC("wcd938x_codec", "wcd938x_cdc"),\n'
     '\t\t\tCOMP_CODEC("wcd939x_codec", "wcd939x_cdc"),\n'),

    # --- EVA. 6.13 replaced file->f_count with the biased file_ref_t file->f_ref: file_ref_get()
    # --- is the "take one reference unless the file is already dead" this spelled out, and
    # --- file_ref_inc() adds each further one, which is legal once one is held. The one caller
    # --- asks for a single reference, so the loop body does not run.
    ("eva-kernel-515/msm/eva/msm_cvp_buf.c",
     "\t\telse if (!atomic_long_add_unless(&file->f_count, refs, 0))\n\t\t\tgoto loop;",
     "\t\telse if (!file_ref_get(&file->f_ref))\n\t\t\tgoto loop;\n"
     "\t\telse\n\t\t\twhile (refs-- > 1)\n\t\t\t\tfile_ref_inc(&file->f_ref);"),

    # the lock that stands in for drm_device.struct_mutex (see SWEEPS): declared where the GEM
    # functions it guards are declared, defined once in the driver's main file
    ("display-drivers/msm/msm_drv.h",
     "void msm_gem_free_object(struct drm_gem_object *obj);",
     "/* k618: 6.18 has no drm_device.struct_mutex; this is the driver's own GEM lock */\n"
     "extern struct mutex k618_gem_lock;\n"
     "void msm_gem_free_object(struct drm_gem_object *obj);"),
    ("display-drivers/msm/msm_drv.c",
     "static bool reglog = false;",
     "/* k618: stands in for the drm_device.struct_mutex 6.18 no longer has */\n"
     "DEFINE_MUTEX(k618_gem_lock);\n\n"
     "static bool reglog = false;"),
    # close_fd() is exported into the namespace the GKI reserves for filesystems
    # (EXPORT_SYMBOL_NS(close_fd, "ANDROID_GKI_VFS_internal_...") in fs/file.c), so a module that
    # calls it has to say it means to -- smcinvoke closes the file descriptor it handed the TEE
    # client when the object goes away, which is the same thing it did before the export was
    # namespaced.
    ("securemsm-kernel/smcinvoke/compat/smcinvoke.c",
     "MODULE_DESCRIPTION(\"SMC Invoke driver (compat)\");",
     "MODULE_DESCRIPTION(\"SMC Invoke driver (compat)\");\n"
     "MODULE_IMPORT_NS(\"ANDROID_GKI_VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver\");"),
]


EDITS += [
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


    ("camera-kernel/drivers/cam_icp/icp_hw/icp_hw_mgr/cam_icp_hw_mgr.c",
     "\t\t\t\thw_mgr->hw_mgr_name, cam_caps[i], icp_dev_mask[i]);\n\t\t\treturn -ENODEV;\n\t\t}\n",
     "\t\t\t\thw_mgr->hw_mgr_name, cam_caps[i], icp_dev_mask[i]);\n"
     "\t\t\t/* k618: 5.15 never failed on this; carry on */\n\t\t}\n"),


    ("camera-kernel/drivers/cam_sensor_module/cam_res_mgr/cam_res_mgr.c",
     "#include <linux/gpio.h>\n",
     "#include <linux/gpio.h>\n#include <linux/gpio/driver.h>\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_res_mgr/cam_res_mgr.c",
     "static int cam_res_mgr_parse_dt_shared_gpio(\n\tstruct device *dev)\n{",
     "/* k618: 5.15's global GPIO numbers in the DT -> 6.18's (k618_tp_fix.py, camera 8) */\n"
     "#define K618_TLMM_LABEL\t\t\"f000000.pinctrl\"\n"
     "#define K618_TLMM_BASE_515\t301\n"
     "#define K618_TLMM_NGPIOS\t211\n\n"
     "static void k618_rebase_gpios(uint *gpio, int n, const char *what)\n{\n"
     "\tstruct gpio_device *gdev = gpio_device_find_by_label(K618_TLMM_LABEL);\n"
     "\tint base, i;\n\n"
     "\tif (!gdev) {\n"
     "\t\tCAM_WARN(CAM_RES, \"k618: no %s, %s left as they are\", K618_TLMM_LABEL, what);\n"
     "\t\treturn;\n\t}\n"
     "\tbase = gpio_device_get_base(gdev);\n"
     "\tgpio_device_put(gdev);\n"
     "\tfor (i = 0; i < n; i++) {\n"
     "\t\tif (gpio[i] < K618_TLMM_BASE_515 ||\n"
     "\t\t    gpio[i] >= K618_TLMM_BASE_515 + K618_TLMM_NGPIOS)\n"
     "\t\t\tcontinue;\n"
     "\t\tCAM_INFO(CAM_RES, \"k618: %s %u -> %u (TLMM pin %u)\", what, gpio[i],\n"
     "\t\t\tgpio[i] - K618_TLMM_BASE_515 + base, gpio[i] - K618_TLMM_BASE_515);\n"
     "\t\tgpio[i] = gpio[i] - K618_TLMM_BASE_515 + base;\n"
     "\t}\n}\n\n"
     "static int cam_res_mgr_parse_dt_shared_gpio(\n\tstruct device *dev)\n{"),
    ("camera-kernel/drivers/cam_sensor_module/cam_res_mgr/cam_res_mgr.c",
     "\t\tCAM_ERR(CAM_RES, \"Get shared gpio array failed.\");\n\t\treturn -EINVAL;\n\t}\n",
     "\t\tCAM_ERR(CAM_RES, \"Get shared gpio array failed.\");\n\t\treturn -EINVAL;\n\t}\n"
     "\tk618_rebase_gpios(dt->shared_gpio, dt->num_shared_gpio, \"gpios-shared\");\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_res_mgr/cam_res_mgr.c",
     "\t\tCAM_ERR(CAM_RES, \"Get shared pinctrl gpio array failed.\");\n\t\treturn -EINVAL;\n\t}\n",
     "\t\tCAM_ERR(CAM_RES, \"Get shared pinctrl gpio array failed.\");\n\t\treturn -EINVAL;\n\t}\n"
     "\tk618_rebase_gpios(dt->shared_pctrl_gpio, dt->num_shared_pctrl_gpio,\n"
     "\t\t\"gpios-shared-pinctrl\");\n"),


    ("camera-kernel/drivers/cam_isp/isp_hw_mgr/isp_hw/vfe_hw/vfe17x/cam_vfe780.h",
     "\t.max_bw_counter_limit  = 0xFF,\n",
     "\t.max_bw_counter_limit  = 0xFF,\n"
     "\t.ubwc_clients_mask     = 0x33,\t/* k618: the clients with ubwc_regs (0, 1, 4, 5) */\n"),
    # --- 10. "M1 QUEUE_OVER_UNDER_FLOW OR CMD ERROR" during the OIS firmware download, after which
    # the master stays in error and the sensors' init writes on it time out (-110). 6.12 sends
    # MSM_CCI_I2C_WRITE_BURST through a new path paced by queue-threshold interrupts; kalama's CCI
    # is v1.7.0 (hw_version 0x10070000, read live) and overruns with it. 5.15's only path -- which
    # 6.12 keeps as cam_cci_data_queue and which handles WRITE_BURST the same way -- for anything
    # older than the 1.10 the burst path was written for.
    ("camera-kernel/drivers/cam_sensor_module/cam_cci/cam_cci_core.c",
     "\tif (c_ctrl->cmd == MSM_CCI_I2C_WRITE_BURST) {\n"
     "\t\trc = cam_cci_data_queue_burst(cci_dev, c_ctrl, queue, sync_en);\n",
     "\t/* k618: the threshold-paced burst path only on CCI >= 1.10 (kalama is 1.7: overruns) */\n"
     "\tif (c_ctrl->cmd == MSM_CCI_I2C_WRITE_BURST &&\n"
     "\t\tcci_dev->hw_version >= CCI_VERSION_1_10_0) {\n"
     "\t\trc = cam_cci_data_queue_burst(cci_dev, c_ctrl, queue, sync_en);\n"),
    # --- 11. every stream start: "Response poll timed out: status=0x00000000" (-110), and the
    # error path leaves the PHY's enable count raised (later "Unbalanced enable for IRQ 313").
    # After enabling the PHY 6.12 polls its REFGEN status at refgen_status_offset, which only the
    # 2.2.0+ register files define; this phone's PHYs are qcom,csiphy-v2.1.2, the offset is 0 and
    # the poll reads an unrelated register. 5.15 has no such poll: skip it where there is none.
    ("camera-kernel/drivers/cam_sensor_module/cam_csiphy/cam_csiphy_core.c",
     "\t\tif (cam_common_read_poll_timeout(csiphybase +\n"
     "\t\t\tstatus_reg_ptr->refgen_status_offset,\n",
     "\t\t/* k618: no REFGEN status on csiphy < 2.2.0 (offset 0) -- nothing to poll */\n"
     "\t\tif (status_reg_ptr->refgen_status_offset &&\n"
     "\t\t\tcam_common_read_poll_timeout(csiphybase +\n"
     "\t\t\tstatus_reg_ptr->refgen_status_offset,\n"),
    # --- 12. frames reach the IFE, then "Write master name: RDI_1, violated constraint
    # description: Pack 16 BPP format Not Supported" and a fatal error to CRM on every request.
    # RDI_1 carries the PDAF data as CAM_FORMAT_PLAIN16_10. 6.12's CSID unpacks PLAIN16_10/12/14
    # to MSB itself and hands the packing to the write master (use_wm_pack: pack_fmt 37 = 16-bit
    # 10 bpp, LSB aligned, read live with camera debug_mdl), which kalama's RDI write masters do not
    # support. 5.15 unpacks only MIPI_RAW_10/12/14 and PLAIN16_16 there and lets the CSID produce
    # PLAIN16_10 directly (no WM packing): its list again.
    ("camera-kernel/drivers/cam_isp/isp_hw_mgr/isp_hw/ife_csid_hw/cam_ife_csid_hw_ver2.c",
     "\tcase CAM_FORMAT_PLAIN16_10:\n\tcase CAM_FORMAT_PLAIN16_12:\n\tcase CAM_FORMAT_PLAIN16_14:\n"
     "\tcase CAM_FORMAT_PLAIN16_16:\n"
     "\t\tneed_unpack = (bool)(path_reg->capabilities & CAM_IFE_CSID_CAP_RDI_UNPACK_MSB);\n",
     "\t/* k618: as 5.15 -- PLAIN16_10/12/14 come out of the CSID itself, no WM packing (kalama) */\n"
     "\tcase CAM_FORMAT_PLAIN16_16:\n"
     "\t\tneed_unpack = (bool)(path_reg->capabilities & CAM_IFE_CSID_CAP_RDI_UNPACK_MSB);\n"),


    ("camera-kernel/drivers/cam_isp/cam_isp_context.c",
     "\tif (ctx_isp->last_sof_timestamp >= ctx_isp->sof_timestamp_val) {\n"
     "\t\tCAM_ERR(CAM_ISP,\n"
     "\t\t\t\"SOF timestamp recovery fail, current timestamp:0x%llx, last timestamp:0x%llx, ctx:%u, link:0x%x\",\n"
     "\t\t\tctx_isp->sof_timestamp_val, ctx_isp->last_sof_timestamp,\n"
     "\t\t\tctx->ctx_id, ctx->link_hdl);\n"
     "\t\treturn;\n"
     "\t}\n",
     "\t/* k618: the event goes out even so, as on 5.15 -- CamX needs it for every request */\n"
     "\tif (ctx_isp->last_sof_timestamp >= ctx_isp->sof_timestamp_val)\n"
     "\t\tCAM_WARN_RATE_LIMIT(CAM_ISP,\n"
     "\t\t\t\"SOF timestamp recovery fail, current timestamp:0x%llx, last timestamp:0x%llx, ctx:%u, link:0x%x, sent anyway\",\n"
     "\t\t\tctx_isp->sof_timestamp_val, ctx_isp->last_sof_timestamp,\n"
     "\t\t\tctx->ctx_id, ctx->link_hdl);\n"),
]


EVA_ICC = """/*
 * k618: EVA's bus paths. The phone's devicetree is 5.15's: the cvp-cnoc and cvp-ddr children give
 * qcom,bus-master / qcom,bus-slave node ids (kalama's global numbering: APPSS_PROC 2 -> VENUS_CFG
 * 551, VIDEO_PROC 31 -> EBI1 512) and the cvp node has no interconnects / interconnect-names, so
 * of_icc_get() returned NULL and EVA ran with no bus votes at all -- the config-NoC path the CPU
 * reaches EVA's registers through among them. Give the node the two properties, each id resolved to
 * the NoC provider that has it (the vendor providers take the global ids, as the DT's other
 * interconnects users show).
 */
static struct device_node *k618_icc_provider(u32 id)
{
	struct device_node *np;
	struct of_phandle_args args;
	struct icc_node_data *data;
	u32 cells;

	for_each_node_with_property(np, \"#interconnect-cells\") {
		if (of_property_read_u32(np, \"#interconnect-cells\", &cells) || cells != 1)
			continue;
		args.np = np;
		args.args_count = 1;
		args.args[0] = id;
		data = of_icc_get_from_provider(&args);
		if (IS_ERR_OR_NULL(data))
			continue;
		kfree(data);
		return np;	/* keeps the reference the walk took: the property points at it */
	}
	return NULL;
}

static void k618_cvp_icc_props(struct iris_hfi_device *device)
{
	struct device_node *np = device->res->pdev->dev.of_node;
	struct bus_info *bus;
	struct property *ic = NULL, *names = NULL;
	__be32 *cells;
	char *s;
	int n = 0, len = 0, i = 0;

	if (of_find_property(np, \"interconnects\", NULL))
		return;
	iris_hfi_for_each_bus(device, bus) {
		n++;
		len += strlen(bus->name) + 1;
	}
	if (!n)
		return;
	ic = kzalloc(sizeof(*ic) + n * 4 * sizeof(__be32), GFP_KERNEL);
	names = kzalloc(sizeof(*names) + len, GFP_KERNEL);
	if (!ic || !names)
		goto fail;
	cells = (__be32 *)(ic + 1);
	s = (char *)(names + 1);
	iris_hfi_for_each_bus(device, bus) {
		struct device_node *src = k618_icc_provider(bus->master);
		struct device_node *dst = k618_icc_provider(bus->slave);

		if (!src || !dst || !src->phandle || !dst->phandle) {
			dprintk(CVP_ERR, \"k618: bus %s: no NoC provider for %d -> %d\\n\",
				bus->name, bus->master, bus->slave);
			goto fail;
		}
		cells[i++] = cpu_to_be32(src->phandle);
		cells[i++] = cpu_to_be32(bus->master);
		cells[i++] = cpu_to_be32(dst->phandle);
		cells[i++] = cpu_to_be32(bus->slave);
		strcpy(s, bus->name);
		s += strlen(bus->name) + 1;
		dprintk(CVP_WARN, \"k618: bus %s: %pOFn %d -> %pOFn %d\\n\",
			bus->name, src, bus->master, dst, bus->slave);
	}
	ic->name = \"interconnects\";
	ic->length = n * 4 * sizeof(__be32);
	ic->value = ic + 1;
	names->name = \"interconnect-names\";
	names->length = len;
	names->value = names + 1;
	if (of_add_property(np, ic)) {
		dprintk(CVP_ERR, \"k618: cannot add interconnects to %pOF\\n\", np);
		goto fail;
	}
	if (of_add_property(np, names))
		dprintk(CVP_ERR, \"k618: cannot add interconnect-names to %pOF\\n\", np);
	return;
fail:
	kfree(ic);
	kfree(names);
}

/* Device compatibility. */
static bool k618_eva_steps;
module_param_named(eva_steps, k618_eva_steps, bool, 0644);
void k618_eva_step(const char *what);
void k618_eva_step(const char *what)
{
	if (!k618_eva_steps)
		return;
	pr_emerg(\"k618_eva: %s\\n\", what);
	msleep(30);
}

"""

EDITS += [
    # --- EVA 1. bus paths from the 5.15 devicetree's ids (EVA_ICC above)
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "#include <linux/of.h>\n#include <linux/pm_qos.h>\n",
     "#include <linux/of.h>\n#include <linux/interconnect-provider.h>\n#include <linux/pm_qos.h>\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "static int __init_bus(struct iris_hfi_device *device)\n{\n\tstruct bus_info *bus = NULL;\n"
     "\tint rc = 0;\n\n\tif (!device)\n\t\treturn -EINVAL;\n",
     EVA_ICC +
     "static int __init_bus(struct iris_hfi_device *device)\n{\n\tstruct bus_info *bus = NULL;\n"
     "\tint rc = 0;\n\n\tif (!device)\n\t\treturn -EINVAL;\n\tk618_cvp_icc_props(device);\n"),
    # --- EVA 2. eva_steps=1: the power-on and boot steps, each said before it runs
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\t/* Vote for all hardware resources */\n\trc = __vote_buses(device, device->bus_vote.data,\n",
     "\t/* Vote for all hardware resources */\n\tk618_eva_step(\"power on: vote buses\");\n"
     "\trc = __vote_buses(device, device->bus_vote.data,\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\trc = __power_on_controller(device);\n\tif (rc)\n\t\tgoto fail_enable_controller;\n",
     "\tk618_eva_step(\"power on: controller (GDSC, clocks)\");\n"
     "\trc = __power_on_controller(device);\n\tif (rc)\n\t\tgoto fail_enable_controller;\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\trc = __power_on_core(device);\n\tif (rc)\n\t\tgoto fail_enable_core;\n",
     "\tk618_eva_step(\"power on: core (GDSC, clocks)\");\n"
     "\trc = __power_on_core(device);\n\tif (rc)\n\t\tgoto fail_enable_core;\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\trc = msm_cvp_scale_clocks(device);\n\tif (rc) {\n\t\tdprintk(CVP_WARN,\n"
     "\t\t\t\"Failed to scale clocks, perf may regress\\n\");\n",
     "\tk618_eva_step(\"power on: scale clocks\");\n"
     "\trc = msm_cvp_scale_clocks(device);\n\tif (rc) {\n\t\tdprintk(CVP_WARN,\n"
     "\t\t\t\"Failed to scale clocks, perf may regress\\n\");\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\t * regulator_disable() and _enable()\n\t */\n\t__set_registers(device);\n",
     "\t * regulator_disable() and _enable()\n\t */\n"
     "\tk618_eva_step(\"power on: first register writes\");\n\t__set_registers(device);\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\tcall_iris_op(device, interrupt_init, device);\n"
     "\tdprintk(CVP_CORE, \"Done with interrupt enabling\\n\");\n",
     "\tk618_eva_step(\"power on: interrupt init\");\n"
     "\tcall_iris_op(device, interrupt_init, device);\n"
     "\tdprintk(CVP_CORE, \"Done with interrupt enabling\\n\");\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\t\trc = load_cvp_fw_impl(device);\n",
     "\t\tk618_eva_step(\"load firmware\");\n\t\trc = load_cvp_fw_impl(device);\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\trc = __boot_firmware(device);\n",
     "\tk618_eva_step(\"boot firmware (control status poll)\");\n\trc = __boot_firmware(device);\n"),
    ("eva-kernel-515/msm/eva/cvp_fw_load.c",
     "#include \"cvp_dump.h\"\n",
     "#include \"cvp_dump.h\"\n\nvoid k618_eva_step(const char *what);\t/* k618: cvp_hfi.c, eva_steps= */\n"),
    ("eva-kernel-515/msm/eva/cvp_fw_load.c",
     "\trc = qcom_mdt_load(&pdev->dev, firmware, firmware_name,\n",
     "\tk618_eva_step(\"firmware: mdt load\");\n"
     "\trc = qcom_mdt_load(&pdev->dev, firmware, firmware_name,\n"),
    ("eva-kernel-515/msm/eva/cvp_fw_load.c",
     "\trc = qcom_scm_pas_auth_and_reset(pas_id);\n",
     "\tk618_eva_step(\"firmware: pas auth and reset\");\n"
     "\trc = qcom_scm_pas_auth_and_reset(pas_id);\n\tk618_eva_step(\"firmware: pas returned\");\n"),


    ("eva-kernel-515/msm/eva/msm_cvp_synx.c",
     "\t\t\trc = synx_wait(ssid, h_synx, timeout_ms);\n\t\t\tif (rc) {\n",
     "\t\t\trc = synx_wait(ssid, h_synx, timeout_ms);\n"
     "\t\t\t/* k618: 6.12's synx_wait gives the state; 5.15's gave 0 for success */\n"
     "\t\t\tif (rc == SYNX_STATE_SIGNALED_SUCCESS)\n"
     "\t\t\t\trc = 0;\n"
     "\t\t\tif (rc) {\n"),


    ("eva-kernel-515/msm/eva/cvp_core_hfi.h",
     "\tu32 last_packet_type;\n\tu32 error;\n",
     "\tu32 last_packet_type;\n\tu32 error;\n"
     "\tu32 k618_boot_gen;\t/* k618: EVA 4, counts firmware boots */\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\trc = __boot_firmware(dev);\n\tif (rc) {\n\t\tdprintk(CVP_ERR, \"Failed to start core\\n\");\n",
     "\tdev->k618_boot_gen++;\t/* k618: EVA 4 */\n"
     "\trc = __boot_firmware(dev);\n\tif (rc) {\n\t\tdprintk(CVP_ERR, \"Failed to start core\\n\");\n"),


    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\tk618_eva_step(\"boot firmware (control status poll)\");\n\trc = __boot_firmware(device);\n",
     "\tdevice->k618_boot_gen++;\t/* k618: EVA 4 */\n"
     "\tk618_eva_step(\"boot firmware (control status poll)\");\n\trc = __boot_firmware(device);\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\t\t__process_sys_error(device);\n\t\tbreak;\n",
     "\t\t__process_sys_error(device);\n"
     "\t\tinfo->response.cmd.size = device->k618_boot_gen;\t/* k618: EVA 4 */\n\t\tbreak;\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\tcmd_done.device_id = device->device_id;\n\tdevice->callback(HAL_SYS_ERROR, &cmd_done);\n",
     "\tcmd_done.device_id = device->device_id;\n"
     "\tcmd_done.size = device->k618_boot_gen;\t/* k618: EVA 4 */\n"
     "\tdevice->callback(HAL_SYS_ERROR, &cmd_done);\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_common.c",
     "\tresponse.device_id = (u32) core->id;\n\thandle_sys_error(cmd, (void *) &response);\n",
     "\tresponse.device_id = (u32) core->id;\n"
     "\tif (core->device && core->device->hfi_device_data)\t/* k618: EVA 4 */\n"
     "\t\tresponse.size = ((struct iris_hfi_device *)\n"
     "\t\t\tcore->device->hfi_device_data)->k618_boot_gen;\n"
     "\thandle_sys_error(cmd, (void *) &response);\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_common.c",
     "\tcur_state = core->state;\n\tcore->state = CVP_CORE_UNINIT;\n",
     "\t/* k618: EVA 4 -- a SYS_ERROR from an earlier firmware boot is stale */\n"
     "\tif (response->size && hdev->hfi_device_data && response->size !=\n"
     "\t\t((struct iris_hfi_device *)hdev->hfi_device_data)->k618_boot_gen) {\n"
     "\t\tdprintk(CVP_WARN, \"k618: stale SYS_ERROR of boot %u ignored (boot %u now)\\n\",\n"
     "\t\t\tresponse->size,\n"
     "\t\t\t((struct iris_hfi_device *)hdev->hfi_device_data)->k618_boot_gen);\n"
     "\t\tmutex_unlock(&core->lock);\n\t\treturn;\n\t}\n\n"
     "\tcur_state = core->state;\n\tcore->state = CVP_CORE_UNINIT;\n"),


    ("eva-kernel-515/msm/eva/msm_cvp_common.c",
     "static void handle_sys_init_done(enum hal_command_response cmd, void *data)\n{\n",
     "/*\n"
     " * k618: EVA 5 -- a core init is under way (set and cleared under core->lock), and the boot\n"
     " * whose firmware raised SYS_ERROR during it\n"
     " */\n"
     "static bool k618_init_waiting;\n"
     "static u32 k618_boot_died;\n"
     "\n"
     "static void handle_sys_init_done(enum hal_command_response cmd, void *data)\n{\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_common.c",
     "\thdev = core->device;\n\n\tmutex_lock(&core->lock);\n\tif (core->state == CVP_CORE_UNINIT) {\n"
     "\t\tdprintk(CVP_ERR,\n\t\t\t\"%s: Core %pK already moved to state %d\\n\",\n",
     "\thdev = core->device;\n\n"
     "\t/*\n"
     "\t * k618: EVA 5 -- the firmware of the boot under way died: the init wait holds core->lock\n"
     "\t * and handles it, this thread (EVA's IRQ thread) must not sit on that lock meanwhile\n"
     "\t */\n"
     "\tif (READ_ONCE(k618_init_waiting) && response->size && hdev->hfi_device_data &&\n"
     "\t\tresponse->size ==\n"
     "\t\t((struct iris_hfi_device *)hdev->hfi_device_data)->k618_boot_gen) {\n"
     "\t\tif (!READ_ONCE(k618_boot_died)) {\n"
     "\t\t\tWRITE_ONCE(k618_boot_died, response->size);\n"
     "\t\t\tcomplete(&core->completions[SYS_MSG_INDEX(HAL_SYS_INIT_DONE)]);\n"
     "\t\t}\n"
     "\t\tdprintk(CVP_WARN, \"k618: SYS_ERROR of boot %u handed to the init wait\\n\",\n"
     "\t\t\tresponse->size);\n"
     "\t\treturn;\n"
     "\t}\n\n"
     "\tmutex_lock(&core->lock);\n\tif (core->state == CVP_CORE_UNINIT) {\n"
     "\t\tdprintk(CVP_ERR,\n\t\t\t\"%s: Core %pK already moved to state %d\\n\",\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_common.c",
     "\tdprintk(CVP_CORE, \"%s: core %pK\\n\", __func__, core);\n"
     "\trc = call_hfi_op(hdev, core_init, hdev->hfi_device_data);\n"
     "\tif (rc) {\n\t\tdprintk(CVP_ERR, \"Failed to init core, id = %d\\n\",\n",
     "\tdprintk(CVP_CORE, \"%s: core %pK\\n\", __func__, core);\n"
     "\t/* k618: EVA 5 -- nothing from an earlier boot answers this one's wait */\n"
     "\treinit_completion(&core->completions[SYS_MSG_INDEX(HAL_SYS_INIT_DONE)]);\n"
     "\tWRITE_ONCE(k618_boot_died, 0);\n"
     "\tWRITE_ONCE(k618_init_waiting, true);\n"
     "\trc = call_hfi_op(hdev, core_init, hdev->hfi_device_data);\n"
     "\tif (rc) {\n\t\tWRITE_ONCE(k618_init_waiting, false);\t/* k618: EVA 5 */\n"
     "\t\tdprintk(CVP_ERR, \"Failed to init core, id = %d\\n\",\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_common.c",
     "int msm_cvp_comm_check_core_init(struct msm_cvp_core *core)\n{\n\tint rc = 0;\n"
     "\tstruct cvp_hfi_device *hdev;\n\n",
     "int msm_cvp_comm_check_core_init(struct msm_cvp_core *core)\n{\n\tint rc = 0;\n"
     "\tstruct cvp_hfi_device *hdev;\n\tint k618_boots = 1;\t/* k618: EVA 5 */\n\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_common.c",
     "\tdprintk(CVP_CORE, \"Waiting for SYS_INIT_DONE\\n\");\n"
     "\trc = wait_for_completion_timeout(\n"
     "\t\t&core->completions[SYS_MSG_INDEX(HAL_SYS_INIT_DONE)],\n"
     "\t\tmsecs_to_jiffies(core->resources.msm_cvp_hw_rsp_timeout));\n"
     "\tif (!rc) {\n",
     "k618_wait:\n"
     "\tdprintk(CVP_CORE, \"Waiting for SYS_INIT_DONE\\n\");\n"
     "\trc = wait_for_completion_timeout(\n"
     "\t\t&core->completions[SYS_MSG_INDEX(HAL_SYS_INIT_DONE)],\n"
     "\t\tmsecs_to_jiffies(core->resources.msm_cvp_hw_rsp_timeout));\n"
     "\tif (rc && READ_ONCE(k618_boot_died)) {\n"
     "\t\t/*\n"
     "\t\t * k618: EVA 5 -- woken by the SYS_ERROR of the firmware that is booting, not by\n"
     "\t\t * SYS_INIT_DONE: take it down and boot it once more\n"
     "\t\t */\n"
     "\t\thdev = core->device;\n"
     "\t\trc = 0;\n"
     "\t\tif (k618_boots++ == 1) {\n"
     "\t\t\tdprintk(CVP_WARN, \"k618: EVA firmware died while booting, booting it again\\n\");\n"
     "\t\t\tcall_hfi_op(hdev, core_release, hdev->hfi_device_data);\n"
     "\t\t\treinit_completion(\n"
     "\t\t\t\t&core->completions[SYS_MSG_INDEX(HAL_SYS_INIT_DONE)]);\n"
     "\t\t\tWRITE_ONCE(k618_boot_died, 0);\n"
     "\t\t\tif (!call_hfi_op(hdev, core_init, hdev->hfi_device_data))\n"
     "\t\t\t\tgoto k618_wait;\n"
     "\t\t\tdprintk(CVP_ERR, \"k618: EVA firmware did not start again\\n\");\n"
     "\t\t\tcore->state = CVP_CORE_UNINIT;\n"
     "\t\t\trc = -EIO;\n"
     "\t\t\tgoto exit;\n"
     "\t\t}\n"
     "\t\tdprintk(CVP_ERR, \"k618: EVA firmware died while booting, again\\n\");\n"
     "\t}\n"
     "\tif (!rc) {\n"),
    ("eva-kernel-515/msm/eva/msm_cvp_common.c",
     "\tdprintk(CVP_CORE, \"SYS_INIT_DONE!!!\\n\");\nexit:\n\tmutex_unlock(&core->lock);\n",
     "\tdprintk(CVP_CORE, \"SYS_INIT_DONE!!!\\n\");\nexit:\n"
     "\tWRITE_ONCE(k618_init_waiting, false);\t/* k618: EVA 5 */\n"
     "\tmutex_unlock(&core->lock);\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "static void process_system_msg(struct msm_cvp_cb_info *info,\n"
     "\t\tstruct iris_hfi_device *device,\n\t\tvoid *raw_packet)\n{\n",
     """/* Device compatibility. */
static void k618_dsp_q_dump(struct iris_hfi_device *device)
{
	struct msm_cvp_smem *mem = &device->dsp_iface_q_table.mem_data;
	struct cvp_hfi_queue_table_header *t =
		(struct cvp_hfi_queue_table_header *)device->dsp_iface_q_table.align_virtual_addr;
	struct cvp_hfi_queue_header *q;
	int i;

	dprintk(CVP_ERR,
		\"k618: ctrl %#x; dsp qtbl %#x uc %#x size %#x (given %#x, %#x); phys %pad\\n\",
		__read_register(device, CVP_CTRL_STATUS),
		__read_register(device, HFI_DSP_QTBL_ADDR),
		__read_register(device, HFI_DSP_UC_REGION_ADDR),
		__read_register(device, HFI_DSP_UC_REGION_SIZE),
		(u32)device->dsp_iface_q_table.align_device_addr, mem->size,
		&mem->dma_handle);
	if (!t)
		return;
	dprintk(CVP_ERR, \"k618: dsp qtbl ver %#x size %#x qhdr0 %#x qhdr %#x num %u/%u\\n\",
		t->qtbl_version, t->qtbl_size, t->qtbl_qhdr0_offset,
		t->qtbl_qhdr_size, t->qtbl_num_q, t->qtbl_num_active_q);
	for (i = 0; i < CVP_IFACEQ_NUMQ; i++) {
		q = CVP_IFACEQ_GET_QHDR_START_ADDR(
			device->dsp_iface_q_table.align_virtual_addr, i);
		dprintk(CVP_ERR,
			\"k618: dsp q%d status %#x start %#x type %#x size %#x pkt %#x rd %#x wr %#x\\n\",
			i, q->qhdr_status, q->qhdr_start_addr, q->qhdr_type,
			q->qhdr_q_size, q->qhdr_pkt_size, q->qhdr_read_idx,
			q->qhdr_write_idx);
	}
}

static void process_system_msg(struct msm_cvp_cb_info *info,
		struct iris_hfi_device *device,
		void *raw_packet)
{
"""),
    # (ahead of the SFR print, not inside EVA 4's lines, so each edit still finds its own text)
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\tcase HAL_SYS_ERROR:\n\t\t__process_sys_error(device);\n",
     "\tcase HAL_SYS_ERROR:\n\t\tk618_dsp_q_dump(device);\t/* k618: EVA 5 */\n"
     "\t\t__process_sys_error(device);\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\tif (!(ctrl_status & CVP_CTRL_INIT_STATUS__M)) {\n"
     "\t\tctrl_init_val = __read_register(device, CVP_CTRL_INIT);\n",
     "\tif (ctrl_status & CVP_CTRL_ERROR_STATUS__M)\t/* k618: EVA 5 */\n"
     "\t\tdprintk(CVP_WARN, \"k618: EVA boot control status %#x after %u polls\\n\",\n"
     "\t\t\tctrl_status, count);\n"
     "\tif (!(ctrl_status & CVP_CTRL_INIT_STATUS__M)) {\n"
     "\t\tctrl_init_val = __read_register(device, CVP_CTRL_INIT);\n"),


    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "static int __interface_dsp_queues_init(struct iris_hfi_device *dev)\n{\n",
     "/*\n"
     " * k618: EVA 6 -- the CPU writes the DSP queues through mem_cdsp's non-cacheable mapping, the\n"
     " * firmware reads them through a dma-coherent context bank, and the pages keep the kernel's\n"
     " * cacheable linear mapping: drop whatever that alias holds once the headers have landed, so\n"
     " * the firmware's reads cannot be answered with the pages' old contents (kernel-6.18/k618_tp_fix.py)\n"
     " */\n"
     "static void k618_dsp_q_drop_alias(struct iris_hfi_device *dev, dma_addr_t pa, size_t size)\n"
     "{\n"
     "\tstatic bool said;\n"
     "\n"
     "\tmb();\n"
     "\tdma_sync_single_for_cpu(dev->res->mem_cdsp.dev, pa, size, DMA_FROM_DEVICE);\n"
     "\tif (!said) {\n"
     "\t\tsaid = true;\n"
     "\t\tdprintk(CVP_WARN, \"k618: EVA dsp queues written, cacheable alias dropped (%pad, %zu)\\n\",\n"
     "\t\t\t&pa, size);\n"
     "\t}\n"
     "}\n"
     "\n"
     "static int __interface_dsp_queues_init(struct iris_hfi_device *dev)\n{\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\t\tmemset((void *)mem_data->kvaddr, 0, q_size);\n"
     "\t\tcvp_dsp_init_hfi_queue_hdr(dev);\n"
     "\t\treturn 0;\n",
     "\t\tmemset((void *)mem_data->kvaddr, 0, q_size);\n"
     "\t\tcvp_dsp_init_hfi_queue_hdr(dev);\n"
     "\t\tk618_dsp_q_drop_alias(dev, mem_data->dma_handle, q_size);\t/* k618: EVA 6 */\n"
     "\t\treturn 0;\n"),
    ("eva-kernel-515/msm/eva/cvp_hfi.c",
     "\tcvp_dsp_init_hfi_queue_hdr(dev);\n\n\treturn rc;\n\nfail_dma_map:\n",
     "\tcvp_dsp_init_hfi_queue_hdr(dev);\n"
     "\tk618_dsp_q_drop_alias(dev, dma_handle, q_size);\t/* k618: EVA 6 */\n\n"
     "\treturn rc;\n\nfail_dma_map:\n"),
]


CAM_OIS_ROHM = r"""/*
 * k618 (camera 16): Xiaomi's ROHM OIS path. The phone's own 5.15 camera.ko has it as bu24721_ois_*
 * (pkt_download, update_i2c_info, apply_settings, apply_calib_settings) and runs it instead of the
 * generic init when the third extra byte of the slave info (k618_ois_flag) is 1..4; both OIS of this
 * phone (CamX nuwa_wide/tele_bu24618_ois, firmware /odm/firmware/<name>.prog/.coeff/.mem) are
 * driven so. 6.12's generic path instead burst-writes .prog/.coeff byte by byte to the run-time
 * slave, never loads .mem and never restarts the chip: its status register then reads 0xff.
 * Reconstructed from the stock disassembly:
 *  1. F050 = 0 (out of standby), wait for status F024 == 1, read the 32-bit program id F01C;
 *  2. if it is not the id the flag expects: CCI client -> download slave 0xA0 at 1 MHz, apply the
 *     packet's init settings there (a failure is logged, not fatal), write .prog/.coeff/.mem as
 *     big-endian dwords, one register per dword from opcode.prog/.coeff/.memory on (address type
 *     k618_fw_addr_type), client -> run-time slave 0x7C at 1 MHz, F097 = 0 (+1 ms), F058 = 0
 *     (+8 ms), wait, F050 = 0, wait, read F01C again;
 *  3. when is_ois_calib: gyro offsets = the values the calib settings write to F09D, sent as
 *     F09C = 0 / F09D = x and F09C = 1 / F09D = y, each followed by the status wait;
 *  4. the packet's fwinit settings -- the only step whose failure fails the init.
 * Writes, reads and status waits of 1-3 are not checked (5.15 did not check them either).
 */
#define K618_ROHM_STANDBY	0xF050
#define K618_ROHM_STATUS	0xF024
#define K618_ROHM_PROG_ID	0xF01C
#define K618_ROHM_GYRO_GAIN_X	0xF07A
#define K618_ROHM_GYRO_GAIN_Y	0xF07C
#define K618_ROHM_RESTART_1	0xF097
#define K618_ROHM_RESTART_2	0xF058
#define K618_ROHM_GOFS_SEL	0xF09C
#define K618_ROHM_GOFS_VAL	0xF09D
#define K618_ROHM_DL_SLAVE	0xA0	/* 8-bit addresses, as the slave info carries them */
#define K618_ROHM_RUN_SLAVE	0x7C
#define K618_ROHM_WAIT_MS	100

/* the program id each flag expects (the stock module's table, indexed by flag - 1) */
static const uint32_t k618_rohm_prog_id[] = {
	0x1a8f025b, 0x1a930253, 0x1a8f024b, 0x1a940043,
};

static void k618_rohm_set_slave(struct cam_ois_ctrl_t *o_ctrl, uint16_t addr)
{
	struct cam_sensor_cci_client *cci = o_ctrl->io_master_info.cci_client;

	cci->sid = addr >> 1;
	cci->retries = 3;
	cci->id_map = 0;
	cci->i2c_freq_mode = I2C_FAST_PLUS_MODE;
	CAM_DBG(CAM_OIS, "k618: ROHM OIS slave 0x%x", addr);
}

static int k618_rohm_write(struct cam_ois_ctrl_t *o_ctrl, uint32_t addr,
	uint32_t data, enum camera_sensor_i2c_type data_type, unsigned short delay_ms)
{
	struct cam_sensor_i2c_reg_array reg = {
		.reg_addr = addr,
		.reg_data = data,
	};
	struct cam_sensor_i2c_reg_setting setting = {
		.reg_setting = &reg,
		.size = 1,
		.addr_type = CAMERA_SENSOR_I2C_TYPE_WORD,
		.data_type = data_type,
		.delay = delay_ms,
	};
	int rc = camera_io_dev_write(&(o_ctrl->io_master_info), &setting);

	if (rc < 0)
		CAM_ERR(CAM_OIS, "k618: ROHM OIS write 0x%x = 0x%x failed: %d",
			addr, data, rc);
	return rc;
}

static void k618_rohm_wait_ready(struct cam_ois_ctrl_t *o_ctrl)
{
	int rc = camera_io_dev_poll(&(o_ctrl->io_master_info), K618_ROHM_STATUS,
		0x01, 0, CAMERA_SENSOR_I2C_TYPE_WORD, CAMERA_SENSOR_I2C_TYPE_BYTE,
		K618_ROHM_WAIT_MS);

	if (rc)
		CAM_WARN(CAM_OIS, "k618: ROHM OIS not ready (status wait %d)", rc);
}

static uint32_t k618_rohm_read(struct cam_ois_ctrl_t *o_ctrl, uint32_t addr,
	enum camera_sensor_i2c_type data_type, bool quiet)
{
	uint32_t val = 0;
	int rc = camera_io_dev_read(&(o_ctrl->io_master_info), addr, &val,
		CAMERA_SENSOR_I2C_TYPE_WORD, data_type, quiet);

	if (rc < 0)
		CAM_WARN(CAM_OIS, "k618: ROHM OIS read 0x%x failed: %d", addr, rc);
	return val;
}

/* stock bu24721_ois_apply_settings: no burst writes, a poll that times out goes on */
static int k618_rohm_apply_settings(struct cam_ois_ctrl_t *o_ctrl,
	struct i2c_settings_array *i2c_set)
{
	struct i2c_settings_list *i2c_list;
	int32_t rc = 0;
	uint32_t i;

	list_for_each_entry(i2c_list, &(i2c_set->list_head), list) {
		switch (i2c_list->op_code) {
		case CAM_SENSOR_I2C_WRITE_RANDOM:
			rc = camera_io_dev_write(&(o_ctrl->io_master_info),
				&(i2c_list->i2c_settings));
			if (rc < 0) {
				CAM_ERR(CAM_OIS, "k618: ROHM OIS settings write failed: %d", rc);
				return rc;
			}
			break;
		case CAM_SENSOR_I2C_WRITE_SEQ:
			rc = camera_io_dev_write_continuous(&(o_ctrl->io_master_info),
				&(i2c_list->i2c_settings), CAM_SENSOR_I2C_WRITE_SEQ);
			if (rc < 0) {
				CAM_ERR(CAM_OIS, "k618: ROHM OIS settings seq write failed: %d", rc);
				return rc;
			}
			break;
		case CAM_SENSOR_I2C_POLL:
			for (i = 0; i < i2c_list->i2c_settings.size; i++) {
				rc = camera_io_dev_poll(&(o_ctrl->io_master_info),
					i2c_list->i2c_settings.reg_setting[i].reg_addr,
					i2c_list->i2c_settings.reg_setting[i].reg_data,
					i2c_list->i2c_settings.reg_setting[i].data_mask,
					i2c_list->i2c_settings.addr_type,
					i2c_list->i2c_settings.data_type,
					i2c_list->i2c_settings.reg_setting[i].delay);
				if (rc < 0) {
					CAM_ERR(CAM_OIS, "k618: ROHM OIS settings poll failed: %d", rc);
					return rc;
				}
			}
			break;
		default:
			break;
		}
	}
	return rc;	/* as 5.15: the last operation's result, 1 if a last poll timed out */
}

static int k618_rohm_apply_retry(struct cam_ois_ctrl_t *o_ctrl,
	struct i2c_settings_array *i2c_set)
{
	int rc = k618_rohm_apply_settings(o_ctrl, i2c_set);

	if (rc == -EAGAIN) {
		CAM_WARN(CAM_OIS, "k618: ROHM OIS: CCI resetting, applying again");
		usleep_range(1000, 1010);
		rc = k618_rohm_apply_settings(o_ctrl, i2c_set);
	}
	return rc;
}

/* <ois_name>.<suffix>: one register per big-endian dword from base on; *write_rc = the CCI result */
static int k618_rohm_load_fw(struct cam_ois_ctrl_t *o_ctrl, const char *suffix,
	uint32_t base, int *write_rc)
{
	struct cam_sensor_i2c_reg_setting setting = {0};
	struct cam_sensor_i2c_reg_array *regs;
	const struct firmware *fw = NULL;
	char name[MAX_OIS_NAME_SIZE + 8];
	uint32_t total, count, i;
	int rc;

	snprintf(name, sizeof(name), "%s.%s", o_ctrl->ois_name, suffix);
	rc = request_firmware(&fw, name, &(o_ctrl->pdev->dev));
	if (rc) {
		CAM_ERR(CAM_OIS, "k618: ROHM OIS: failed to locate %s: %d", name, rc);
		return rc;
	}
	if (fw->size > 0xffff)
		CAM_WARN(CAM_OIS, "k618: ROHM OIS: %s is %zu bytes, sending the first %zu as 5.15",
			name, fw->size, fw->size & 0xffff);
	total = fw->size & 0xffff;	/* 5.15 kept the size in 16 bits */
	count = DIV_ROUND_UP(total, 4);
	regs = vzalloc((count ? count : 1) * sizeof(*regs));
	if (!regs) {
		release_firmware(fw);
		return -ENOMEM;
	}
	for (i = 0; i < count; i++) {
		uint8_t b[4] = {0};

		memcpy(b, fw->data + 4 * i, min_t(uint32_t, 4, total - 4 * i));
		regs[i].reg_addr = base + i;
		regs[i].reg_data = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
			((uint32_t)b[2] << 8) | b[3];
	}
	setting.reg_setting = regs;
	setting.size = count;
	setting.addr_type = o_ctrl->k618_fw_addr_type;
	setting.data_type = CAMERA_SENSOR_I2C_TYPE_DWORD;
	if (setting.addr_type <= CAMERA_SENSOR_I2C_TYPE_INVALID ||
		setting.addr_type >= CAMERA_SENSOR_I2C_TYPE_MAX) {
		CAM_WARN(CAM_OIS, "k618: ROHM OIS: firmware address type %u, using word",
			o_ctrl->k618_fw_addr_type);
		setting.addr_type = CAMERA_SENSOR_I2C_TYPE_WORD;
	}
	*write_rc = count ? camera_io_dev_write(&(o_ctrl->io_master_info), &setting) : 0;
	if (*write_rc < 0)
		CAM_ERR(CAM_OIS, "k618: ROHM OIS: %s (%u bytes) download failed: %d",
			name, total, *write_rc);
	else
		CAM_DBG(CAM_OIS, "k618: ROHM OIS: %s, %u dwords from 0x%x", name, count, base);
	vfree(regs);
	release_firmware(fw);
	return 0;
}

/* stock bu24721_ois_apply_calib_settings */
static void k618_rohm_apply_calib(struct cam_ois_ctrl_t *o_ctrl,
	struct i2c_settings_array *i2c_set)
{
	struct i2c_settings_list *i2c_list;
	uint32_t ofs[4] = {0}, n = 0, i;

	list_for_each_entry(i2c_list, &(i2c_set->list_head), list) {
		if (i2c_list->op_code != CAM_SENSOR_I2C_WRITE_RANDOM)
			continue;
		for (i = 0; i < i2c_list->i2c_settings.size && n < ARRAY_SIZE(ofs); i++)
			if (i2c_list->i2c_settings.reg_setting[i].reg_addr == K618_ROHM_GOFS_VAL)
				ofs[n++] = i2c_list->i2c_settings.reg_setting[i].reg_data & 0xffff;
	}
	k618_rohm_write(o_ctrl, K618_ROHM_GOFS_SEL, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 0);
	k618_rohm_wait_ready(o_ctrl);
	k618_rohm_write(o_ctrl, K618_ROHM_GOFS_VAL, ofs[0], CAMERA_SENSOR_I2C_TYPE_WORD, 0);
	k618_rohm_wait_ready(o_ctrl);
	k618_rohm_write(o_ctrl, K618_ROHM_GOFS_SEL, 1, CAMERA_SENSOR_I2C_TYPE_BYTE, 0);
	k618_rohm_wait_ready(o_ctrl);
	k618_rohm_write(o_ctrl, K618_ROHM_GOFS_VAL, ofs[1], CAMERA_SENSOR_I2C_TYPE_WORD, 0);
	k618_rohm_wait_ready(o_ctrl);
	CAM_DBG(CAM_OIS, "k618: ROHM OIS gyro offsets x 0x%x y 0x%x (%u in calib)",
		ofs[0], ofs[1], n);
}

/* stock bu24721_ois_pkt_download; the caller has powered the OIS up */
static int k618_rohm_ois_init(struct cam_ois_ctrl_t *o_ctrl)
{
	uint32_t want = k618_rohm_prog_id[o_ctrl->k618_ois_flag - 1];
	uint32_t id, gyro_x, gyro_y;
	int rc = 0, write_rc = 0;

	if (o_ctrl->io_master_info.master_type != CCI_MASTER ||
		!o_ctrl->io_master_info.cci_client) {
		CAM_ERR(CAM_OIS, "k618: ROHM OIS needs a CCI master (type %d)",
			o_ctrl->io_master_info.master_type);
		return -EINVAL;
	}

	k618_rohm_write(o_ctrl, K618_ROHM_STANDBY, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 1);
	k618_rohm_wait_ready(o_ctrl);
	id = k618_rohm_read(o_ctrl, K618_ROHM_PROG_ID, CAMERA_SENSOR_I2C_TYPE_DWORD, true);
	k618_rohm_wait_ready(o_ctrl);
	CAM_INFO(CAM_OIS, "k618: ROHM OIS %s flag %u: program id 0x%08x, wants 0x%08x",
		o_ctrl->ois_name, o_ctrl->k618_ois_flag, id, want);

	if (id != want) {
		k618_rohm_set_slave(o_ctrl, K618_ROHM_DL_SLAVE);
		if (o_ctrl->i2c_init_data.is_settings_valid == 1) {
			rc = k618_rohm_apply_retry(o_ctrl, &o_ctrl->i2c_init_data);
			if (rc)	/* 5.15 logged this and downloaded all the same */
				CAM_ERR(CAM_OIS, "k618: ROHM OIS: download-mode settings: %d", rc);
		}
		rc = k618_rohm_load_fw(o_ctrl, "prog", o_ctrl->opcode.prog, &write_rc);
		if (!rc && write_rc >= 0)
			rc = k618_rohm_load_fw(o_ctrl, "coeff", o_ctrl->opcode.coeff, &write_rc);
		if (!rc && write_rc >= 0)
			rc = k618_rohm_load_fw(o_ctrl, "mem", o_ctrl->opcode.memory, &write_rc);
		k618_rohm_set_slave(o_ctrl, K618_ROHM_RUN_SLAVE);
		if (rc)		/* no firmware file / no memory: 5.15 gave up here as well */
			return rc;

		k618_rohm_write(o_ctrl, K618_ROHM_RESTART_1, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 1);
		k618_rohm_write(o_ctrl, K618_ROHM_RESTART_2, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 8);
		k618_rohm_wait_ready(o_ctrl);
		k618_rohm_write(o_ctrl, K618_ROHM_STANDBY, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 1);
		k618_rohm_wait_ready(o_ctrl);
		id = k618_rohm_read(o_ctrl, K618_ROHM_PROG_ID, CAMERA_SENSOR_I2C_TYPE_DWORD, true);
		k618_rohm_wait_ready(o_ctrl);
		if (id == want && write_rc >= 0)
			CAM_INFO(CAM_OIS, "k618: ROHM OIS %s: firmware loaded, program id 0x%08x",
				o_ctrl->ois_name, id);
		else
			CAM_ERR(CAM_OIS,
				"k618: ROHM OIS %s: program id 0x%08x after the download, wants 0x%08x (write %d)",
				o_ctrl->ois_name, id, want, write_rc);
	}

	if (o_ctrl->is_ois_calib)
		k618_rohm_apply_calib(o_ctrl, &o_ctrl->i2c_calib_data);

	gyro_x = k618_rohm_read(o_ctrl, K618_ROHM_GYRO_GAIN_X, CAMERA_SENSOR_I2C_TYPE_WORD, false);
	gyro_y = k618_rohm_read(o_ctrl, K618_ROHM_GYRO_GAIN_Y, CAMERA_SENSOR_I2C_TYPE_WORD, false);
	CAM_DBG(CAM_OIS, "k618: ROHM OIS gyro gain x 0x%x y 0x%x", gyro_x, gyro_y);

	if (o_ctrl->i2c_fwinit_data.is_settings_valid != 1)
		return 0;
	rc = k618_rohm_apply_retry(o_ctrl, &o_ctrl->i2c_fwinit_data);
	if (rc)
		CAM_ERR(CAM_OIS, "k618: ROHM OIS %s: fwinit settings: %d (flag %u)",
			o_ctrl->ois_name, rc, o_ctrl->k618_ois_flag);
	return rc;
}

"""

EDITS += [


    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_dev.h",
     "\tstruct cam_ois_opcode opcode;\n",
     "\tstruct cam_ois_opcode opcode;\n"
     "\t/*\n"
     "\t * k618: Xiaomi's OIS slave info is 59 bytes, three more after opcode (the phone's 5.15\n"
     "\t * camera.ko copies opcode and these, 19 bytes): the address type of the firmware writes, a\n"
     "\t * page-increment flag of its byte-burst download, and 1..4 = the ROHM path (camera 16)\n"
     "\t */\n"
     "\tbool    k618_xiaomi_info;\n"
     "\tuint8_t k618_fw_addr_type;\n"
     "\tuint8_t k618_fw_page_inc;\n"
     "\tuint8_t k618_ois_flag;\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "#include <linux/firmware.h>\n",
     "#include <linux/firmware.h>\n"
     "#include <linux/vmalloc.h>\t/* k618: ROHM OIS firmware */\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\tmemcpy(&(o_ctrl->opcode), &(ois_info->opcode),\n"
     "\t\t\tsizeof(struct cam_ois_opcode));\n",
     "\t\tmemcpy(&(o_ctrl->opcode), &(ois_info->opcode),\n"
     "\t\t\tsizeof(struct cam_ois_opcode));\n"
     "\t\t/* k618: Xiaomi's three bytes after opcode (cam_ois_dev.h) */\n"
     "\t\to_ctrl->k618_xiaomi_info = len >= sizeof(struct cam_cmd_ois_info) + 3;\n"
     "\t\tif (o_ctrl->k618_xiaomi_info) {\n"
     "\t\t\tconst uint8_t *ext = (const uint8_t *)ois_info +\n"
     "\t\t\t\tsizeof(struct cam_cmd_ois_info);\n"
     "\n"
     "\t\t\to_ctrl->k618_fw_addr_type = ext[0];\n"
     "\t\t\to_ctrl->k618_fw_page_inc = ext[1];\n"
     "\t\t\to_ctrl->k618_ois_flag = ext[2];\n"
     "\t\t} else {\n"
     "\t\t\to_ctrl->k618_fw_addr_type = 0;\n"
     "\t\t\to_ctrl->k618_fw_page_inc = 0;\n"
     "\t\t\to_ctrl->k618_ois_flag = 0;\n"
     "\t\t}\n"
     "\t\tCAM_INFO(CAM_OIS,\n"
     "\t\t\t\"k618: OIS %s slave 0x%x mode %u fw %u calib %u prog 0x%x coeff 0x%x mem 0x%x extra %u/%u/%u\",\n"
     "\t\t\to_ctrl->ois_name, ois_info->slave_addr, ois_info->i2c_freq_mode,\n"
     "\t\t\to_ctrl->ois_fw_flag, o_ctrl->is_ois_calib, o_ctrl->opcode.prog,\n"
     "\t\t\to_ctrl->opcode.coeff, o_ctrl->opcode.memory, o_ctrl->k618_fw_addr_type,\n"
     "\t\t\to_ctrl->k618_fw_page_inc, o_ctrl->k618_ois_flag);\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "/**\n"
     " * cam_ois_pkt_parse - Parse csl packet\n",
     CAM_OIS_ROHM +
     "/**\n"
     " * cam_ois_pkt_parse - Parse csl packet\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\tCAM_DBG(CAM_OIS, \"ois_fw_flag: %d\", o_ctrl->ois_fw_flag);\n",
     "\t\t/* k618: Xiaomi's ROHM OIS path, as the phone's 5.15 camera.ko (camera 16) */\n"
     "\t\tif (o_ctrl->k618_xiaomi_info && o_ctrl->k618_ois_flag >= 1 &&\n"
     "\t\t\to_ctrl->k618_ois_flag <= ARRAY_SIZE(k618_rohm_prog_id)) {\n"
     "\t\t\to_ctrl->cam_ois_state = CAM_OIS_CONFIG;\n"
     "\t\t\trc = k618_rohm_ois_init(o_ctrl);\n"
     "\t\t\tif (rc < 0) {\n"
     "\t\t\t\tCAM_ERR(CAM_OIS, \"k618: ROHM OIS init failed: %d (flag %u)\",\n"
     "\t\t\t\t\trc, o_ctrl->k618_ois_flag);\n"
     "\t\t\t\tgoto pwr_dwn;\n"
     "\t\t\t}\n"
     "\t\t\tif (rc)\t/* 5.15 returned this 1 to CamX and kept the OIS powered */\n"
     "\t\t\t\tCAM_WARN(CAM_OIS, \"k618: ROHM OIS: a poll at the end of fwinit timed out\");\n"
     "\t\t\trc = 0;\n"
     "\t\t\tgoto k618_init_done;\n"
     "\t\t}\n"
     "\n"
     "\t\tCAM_DBG(CAM_OIS, \"ois_fw_flag: %d\", o_ctrl->ois_fw_flag);\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\to_ctrl->cam_ois_state = CAM_OIS_CONFIG;\n"
     "\n"
     "\t\trc = delete_request(&o_ctrl->i2c_fwinit_data);\n",
     "k618_init_done:\n"
     "\t\to_ctrl->cam_ois_state = CAM_OIS_CONFIG;\n"
     "\n"
     "\t\trc = delete_request(&o_ctrl->i2c_fwinit_data);\n"),
]


CAM_OIS_XIAOMI_DL = """/* Device compatibility. */
static int k618_xiaomi_fw_download(struct cam_ois_ctrl_t *o_ctrl)
{
	static const char * const sfx[] = { \"prog\", \"coeff\", \"mem\" };
	uint32_t base[] = { o_ctrl->opcode.prog, o_ctrl->opcode.coeff, o_ctrl->opcode.memory };
	struct cam_sensor_i2c_reg_setting setting = {0};
	struct cam_sensor_i2c_reg_array *regs;
	const struct firmware *fw = NULL;
	char name[MAX_OIS_NAME_SIZE + 8];
	uint32_t total, done, page, n, i;
	int f, rc = 0;

	setting.addr_type = o_ctrl->k618_fw_addr_type;
	if (setting.addr_type <= CAMERA_SENSOR_I2C_TYPE_INVALID ||
		setting.addr_type >= CAMERA_SENSOR_I2C_TYPE_MAX) {
		CAM_ERR(CAM_OIS, \"k618: OIS %s: firmware address type %u\",
			o_ctrl->ois_name, o_ctrl->k618_fw_addr_type);
		return -EINVAL;
	}
	setting.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
	setting.delay = 0;
	regs = vzalloc(256 * sizeof(*regs));
	if (!regs)
		return -ENOMEM;
	setting.reg_setting = regs;

	for (f = 0; f < ARRAY_SIZE(sfx); f++) {
		snprintf(name, sizeof(name), \"%s.%s\", o_ctrl->ois_name, sfx[f]);
		rc = request_firmware(&fw, name, &(o_ctrl->pdev->dev));
		if (rc) {
			CAM_ERR(CAM_OIS, \"Failed to locate %s\", name);
			break;
		}
		total = fw->size & 0xffff;	/* 5.15 kept the size in 16 bits */
		for (done = 0, page = 0; done < total; done += n) {
			n = min_t(uint32_t, 256, total - done);
			for (i = 0; i < n; i++) {
				regs[i].reg_addr = base[f] + (page << 8);
				regs[i].reg_data = fw->data[done + i];
				regs[i].delay = 0;
				regs[i].data_mask = 0;
			}
			setting.size = n;
			if (o_ctrl->k618_fw_page_inc)
				page++;
			rc = camera_io_dev_write_continuous(&(o_ctrl->io_master_info),
				&setting, CAM_SENSOR_I2C_WRITE_BURST);
			if (rc < 0)
				break;
		}
		release_firmware(fw);
		if (rc < 0) {
			CAM_ERR(CAM_OIS, \"OIS FW(%s) size(%u) download failed rc: %d\",
				sfx[f], total, rc);
			break;
		}
		CAM_INFO(CAM_OIS, \"k618: OIS %s: %s, %u bytes from 0x%x, address type %u, page step %u\",
			o_ctrl->ois_name, name, total, base[f], o_ctrl->k618_fw_addr_type,
			o_ctrl->k618_fw_page_inc);
	}
	vfree(regs);
	return rc;
}

"""

EDITS += [


    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_dev.h",
     "\tuint8_t k618_ois_flag;\n",
     "\tuint8_t k618_ois_flag;\n"
     "\tstruct i2c_settings_array k618_postinit_data;\t/* k618: Xiaomi's fourth settings buffer */\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_soc.c",
     "\tINIT_LIST_HEAD(&(o_ctrl->i2c_fwinit_data.list_head));\n",
     "\tINIT_LIST_HEAD(&(o_ctrl->i2c_fwinit_data.list_head));\n"
     "\tINIT_LIST_HEAD(&(o_ctrl->k618_postinit_data.list_head));\t/* k618 */\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "static int cam_ois_fw_download(struct cam_ois_ctrl_t *o_ctrl)\n"
     "{\n",
     CAM_OIS_XIAOMI_DL +
     "static int cam_ois_fw_download(struct cam_ois_ctrl_t *o_ctrl)\n"
     "{\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\tdev = &(o_ctrl->pdev->dev);\n"
     "\n"
     "\tsnprintf(name_coeff, 32, \"%s.coeff\", o_ctrl->ois_name);\n",
     "\tdev = &(o_ctrl->pdev->dev);\n"
     "\n"
     "\t/* k618: camera 16 -- Xiaomi's slave info, Xiaomi's download */\n"
     "\tif (o_ctrl->k618_xiaomi_info)\n"
     "\t\treturn k618_xiaomi_fw_download(o_ctrl);\n"
     "\n"
     "\tsnprintf(name_coeff, 32, \"%s.coeff\", o_ctrl->ois_name);\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\t\t\tif (rc < 0) {\n"
     "\t\t\t\t\tCAM_DBG(CAM_OIS,\n"
     "\t\t\t\t\t\"fw init parsing failed: %d\", rc);\n"
     "\t\t\t\t\tbreak;\n"
     "\t\t\t\t}\n"
     "\t\t\t}\n"
     "\t\t\tbreak;\n"
     "\t\t\t}\n",
     "\t\t\t\tif (rc < 0) {\n"
     "\t\t\t\t\tCAM_DBG(CAM_OIS,\n"
     "\t\t\t\t\t\"fw init parsing failed: %d\", rc);\n"
     "\t\t\t\t\tbreak;\n"
     "\t\t\t\t}\n"
     "\t\t\t} else if (o_ctrl->k618_xiaomi_info &&\n"
     "\t\t\t\to_ctrl->k618_postinit_data.is_settings_valid == 0) {\n"
     "\t\t\t\t/* k618: camera 16 -- Xiaomi's fourth buffer, applied after calib as 5.15 did */\n"
     "\t\t\t\ti2c_reg_settings = &(o_ctrl->k618_postinit_data);\n"
     "\t\t\t\ti2c_reg_settings->is_settings_valid = 1;\n"
     "\t\t\t\ti2c_reg_settings->request_id = 0;\n"
     "\t\t\t\trc = cam_sensor_i2c_command_parser(\n"
     "\t\t\t\t\t&o_ctrl->io_master_info,\n"
     "\t\t\t\t\ti2c_reg_settings,\n"
     "\t\t\t\t\t&cmd_desc[i], 1, NULL);\n"
     "\t\t\t\tif (rc < 0) {\n"
     "\t\t\t\t\tCAM_ERR(CAM_OIS, \"k618: postinit parsing failed: %d\", rc);\n"
     "\t\t\t\t\tbreak;\n"
     "\t\t\t\t}\n"
     "\t\t\t\tCAM_INFO(CAM_OIS, \"k618: OIS %s: postinit settings received\",\n"
     "\t\t\t\t\to_ctrl->ois_name);\n"
     "\t\t\t}\n"
     "\t\t\tbreak;\n"
     "\t\t\t}\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\t\t\tCAM_DBG(CAM_OIS, \"apply calib data settings success\");\n"
     "\t\t\t}\n"
     "\t\t}\n"
     "\n"
     "k618_init_done:\n",
     "\t\t\t\tCAM_DBG(CAM_OIS, \"apply calib data settings success\");\n"
     "\t\t\t}\n"
     "\t\t}\n"
     "\t\t/* k618: camera 16 -- Xiaomi's postinit, last, as 5.15 (a poll that timed out: logged) */\n"
     "\t\tif (o_ctrl->k618_postinit_data.is_settings_valid == 1) {\n"
     "\t\t\trc = cam_ois_apply_settings(o_ctrl,\n"
     "\t\t\t\t&o_ctrl->k618_postinit_data);\n"
     "\t\t\tif ((rc == -EAGAIN) &&\n"
     "\t\t\t\t(o_ctrl->io_master_info.master_type == CCI_MASTER)) {\n"
     "\t\t\t\tusleep_range(1000, 1010);\n"
     "\t\t\t\trc = cam_ois_apply_settings(o_ctrl,\n"
     "\t\t\t\t\t&o_ctrl->k618_postinit_data);\n"
     "\t\t\t}\n"
     "\t\t\tif (rc < 0) {\n"
     "\t\t\t\tCAM_ERR(CAM_OIS, \"k618: cannot apply postinit data: %d\", rc);\n"
     "\t\t\t\tgoto pwr_dwn;\n"
     "\t\t\t}\n"
     "\t\t\tif (rc)\n"
     "\t\t\t\tCAM_WARN(CAM_OIS, \"k618: OIS %s: a postinit poll timed out\",\n"
     "\t\t\t\t\to_ctrl->ois_name);\n"
     "\t\t\trc = 0;\n"
     "\t\t}\n"
     "\n"
     "k618_init_done:\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\trc = delete_request(&o_ctrl->i2c_calib_data);\n"
     "\t\tif (rc < 0) {\n"
     "\t\t\tCAM_WARN(CAM_OIS,\n"
     "\t\t\t\t\"Fail deleting Calibration data: rc: %d\", rc);\n"
     "\t\t\trc = 0;\n"
     "\t\t}\n",
     "\t\trc = delete_request(&o_ctrl->i2c_calib_data);\n"
     "\t\tif (rc < 0) {\n"
     "\t\t\tCAM_WARN(CAM_OIS,\n"
     "\t\t\t\t\"Fail deleting Calibration data: rc: %d\", rc);\n"
     "\t\t\trc = 0;\n"
     "\t\t}\n"
     "\t\tif (o_ctrl->k618_postinit_data.is_settings_valid == 1)\t/* k618 */\n"
     "\t\t\tdelete_request(&o_ctrl->k618_postinit_data);\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\tif (o_ctrl->i2c_calib_data.is_settings_valid == 1)\n"
     "\t\tdelete_request(&o_ctrl->i2c_calib_data);\n",
     "\tif (o_ctrl->i2c_calib_data.is_settings_valid == 1)\n"
     "\t\tdelete_request(&o_ctrl->i2c_calib_data);\n"
     "\tif (o_ctrl->k618_postinit_data.is_settings_valid == 1)\t/* k618 */\n"
     "\t\tdelete_request(&o_ctrl->k618_postinit_data);\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\tif (o_ctrl->i2c_calib_data.is_settings_valid == 1)\n"
     "\t\t\tdelete_request(&o_ctrl->i2c_calib_data);\n",
     "\t\tif (o_ctrl->i2c_calib_data.is_settings_valid == 1)\n"
     "\t\t\tdelete_request(&o_ctrl->i2c_calib_data);\n"
     "\t\tif (o_ctrl->k618_postinit_data.is_settings_valid == 1)\t/* k618 */\n"
     "\t\t\tdelete_request(&o_ctrl->k618_postinit_data);\n"),
]


EDITS += [


    ("camera-kernel/drivers/cam_utils/cam_compat.c",
     "bool cam_secure_get_vfe_fd_port_config(void)\n"
     "{\n"
     "#if KERNEL_VERSION(6, 0, 0) <= LINUX_VERSION_CODE\n"
     "\treturn false;\n"
     "#else\n"
     "\treturn true;\n"
     "#endif\n"
     "}\n",
     """/* Device compatibility. */
static bool k618_fd_secure;
module_param(k618_fd_secure, bool, 0644);
MODULE_PARM_DESC(k618_fd_secure, \"k618: the FD output may be secure, as on 5.15 (face unlock)\");

bool cam_secure_get_vfe_fd_port_config(void)
{
	return k618_fd_secure;
}
"""),
    ("camera-kernel/drivers/cam_isp/cam_isp_context.c",
     "\t\t\tCAM_WARN(CAM_ISP,\n"
     "\t\t\t\t\"Apply delayed req:%llu, link:0x%x, timestamp sof:0x%llx applied:0x%llx\",\n"
     "\t\t\t\tapply->request_id, ctx->link_hdl,\n"
     "\t\t\t\tctx_isp->boot_timestamp, apply->last_applied_done_timestamp);\n"
     "\t\t\t/* Notify userland on request error due to setting mismatched */\n"
     "\t\t\t__cam_isp_ctx_notify_v4l2_error_event(CAM_REQ_MGR_ERROR_TYPE_REQUEST,\n"
     "\t\t\t\tCAM_REQ_MGR_ISP_ERR_SETTING_MISMATCHED, apply->request_id, ctx);\n",
     """			CAM_WARN_RATE_LIMIT(CAM_ISP,
				\"Apply delayed req:%llu, link:0x%x, timestamp sof:0x%llx applied:0x%llx (not reported, as 5.15)\",
				apply->request_id, ctx->link_hdl,
				ctx_isp->boot_timestamp, apply->last_applied_done_timestamp);
			/* Device compatibility. */
"""),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_dev.h",
     "\tstruct i2c_settings_array k618_postinit_data;\t/* k618: Xiaomi's fourth settings buffer */\n",
     "\tstruct i2c_settings_array k618_postinit_data;\t/* k618: Xiaomi's fourth settings buffer */\n"
     "\tint k618_poll_go_on;\t/* k618: bring-up polls that may still time out and go on */\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\t\t\t} else if (rc ==  I2C_COMPARE_MISMATCH) {\n"
     "\t\t\t\t\tCAM_ERR(CAM_OIS, \"i2c poll mismatch\");\n"
     "\t\t\t\t\treturn rc;\n"
     "\t\t\t\t}\n",
     """				} else if (rc ==  I2C_COMPARE_MISMATCH) {
					/* Device compatibility. */
					if (o_ctrl->k618_poll_go_on > 0) {
						o_ctrl->k618_poll_go_on--;
						CAM_WARN(CAM_OIS,
							\"k618: OIS %s: poll 0x%x == 0x%x timed out at bring-up, going on (%d more)\",
							o_ctrl->ois_name,
							i2c_list->i2c_settings.reg_setting[i].reg_addr,
							i2c_list->i2c_settings.reg_setting[i].reg_data,
							o_ctrl->k618_poll_go_on);
						continue;
					}
					CAM_ERR(CAM_OIS, \"i2c poll mismatch\");
					return rc;
				}
"""),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\t/* k618: Xiaomi's ROHM OIS path, as the phone's 5.15 camera.ko (camera 16) */\n",
     "\t\t/* k618: camera 16, third round -- see cam_ois_apply_settings() */\n"
     "\t\to_ctrl->k618_poll_go_on = o_ctrl->k618_xiaomi_info ? 4 : 0;\n"
     "\t\t/* k618: Xiaomi's ROHM OIS path, as the phone's 5.15 camera.ko (camera 16) */\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\tcase CAM_OIS_PACKET_OPCODE_OIS_CONTROL:\n"
     "\t\tCAM_DBG(CAM_OIS, \"CAM_OIS_PACKET_OPCODE_OIS_CONTROL\");\n",
     "\tcase CAM_OIS_PACKET_OPCODE_OIS_CONTROL:\n"
     "\t\tCAM_DBG(CAM_OIS, \"CAM_OIS_PACKET_OPCODE_OIS_CONTROL\");\n"
     "\t\to_ctrl->k618_poll_go_on = 0;\t/* k618: per-frame packets fail fast */\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "pwr_dwn:\n"
     "\tcam_ois_power_down(o_ctrl);\n",
     "pwr_dwn:\n"
     "\to_ctrl->k618_poll_go_on = 0;\t/* k618 */\n"
     "\tcam_ois_power_down(o_ctrl);\n"),
]


# Camera 24: the 13 Ultra's OIS (from its own 5.15 camera.ko, ishtar OS3.0.308.0).
#
# Placement: after the camera-16 edits, i.e. after the EDITS list that ends with the "pwr_dwn:"
# tuple ("\tcam_ois_power_down(o_ctrl);\n"). Several anchors below are the last lines of
# camera-16 edits (each is extended at its end only, so camera 16's own `new` texts stay whole), so
# these tuples must come after camera 16 in EDITS. Every tuple keeps the file's idempotency rules:
# either `old` is inside `new`, or `new` contains no copy of `old`.
#
# What the 13 Pro sees: nothing new runs. o_ctrl->k618_ultra is false there (root "model" is
# "... 8550 nuwa"), so the slave-info, INIT, packet, READ, START/STOP/RELEASE hooks return at once
# or call the code they replace; the OIS registers no request-manager ops; the struct grows.
#
# Device choice: the root node's "model". On the phones' live device trees it is the only root
# property that differs -- compatible ("qcom,kalama-mtp", "qcom,kalama", "qcom,mtp") and
# qcom,board-id (<0x10008 0>) are the same on both; the 13 Pro's live DT has
# "Qualcomm Technologies, Inc. 8550 nuwa", ishtar's dtbo overlay "Qualcomm Technologies, Inc.
# 8550 ishtar" (the 13 Pro's live DT shows the bootloader copies the overlay's model). The slave
# info's length cannot decide it: stock passes the buffer's remaining length, not the struct's
# (13 Ultra 0x15f84c: x21 = len_of_buff - cmd_desc->offset), and refuses 64 or fewer bytes; the 13
# Pro's module refuses 58 or fewer. Other kalama Xiaomi models (fuxi, babylon) keep the 13 Pro's
# path; their layout is not checked here.

CAM_OIS_ULTRA_DECL = r"""
/* k618 (camera 24): the 13 Ultra's OIS -- defined at the end of this file */
#include <linux/of.h>
#define K618_ULTRA_SLAVE_INFO_LEN	0x41	/* stock 0x162714: cmp x2, #0x40; b.ls -> -EINVAL */
#define K618_ULTRA_INIT_DONE		0x10000	/* k618_ultra_init(): the chip function ran */
#define K618_OIS_OPCODE_APERTURE	4	/* stock pkt opcode jump table .rodata 0x89588 */
#define K618_OIS_OPCODE_REDOWNLOAD	5
#define K618_OIS_OPCODE_PARKLENS	6
#define K618_OIS_OPCODE_SKIPFRAME	0x7e
#define K618_SEM1217S_CHIP_ID		0x4c1	/* stock 0x162d84 */
static int k618_ultra_slave_info(struct cam_ois_ctrl_t *o_ctrl, const uint8_t *info, size_t len);
static int k618_ultra_init(struct cam_ois_ctrl_t *o_ctrl);
static int k618_ultra_fw_download(struct cam_ois_ctrl_t *o_ctrl);
static int k618_ultra_flush_check(struct cam_ois_ctrl_t *o_ctrl, struct cam_packet *csl_packet);
static int k618_ultra_pkt(struct cam_ois_ctrl_t *o_ctrl, struct cam_packet *csl_packet);
static int k618_ultra_read_data(struct cam_ois_ctrl_t *o_ctrl, struct i2c_settings_array *set);
static void k618_ultra_start(struct cam_ois_ctrl_t *o_ctrl);
static void k618_ultra_stop(struct cam_ois_ctrl_t *o_ctrl);
static void k618_ultra_release(struct cam_ois_ctrl_t *o_ctrl);
"""

CAM_OIS_ULTRA = r"""
/*
 * k618 (camera 24): the 13 Ultra's OIS, rebuilt from that phone's own 5.15 camera.ko (ishtar
 * OS3.0.308.0.WMACNXM, symbols kept; the addresses are offsets in its .text). Every entry point
 * returns early unless o_ctrl->k618_ultra is set, which k618_ois_ultra_setup() does at probe when
 * the root node's "model" names ishtar; on the 13 Pro camera 16's paths run as before.
 *  - slave info: 65 bytes (0x162714 refuses 64 or fewer); 0x38 address type and 0x39 page step as
 *    on the 13 Pro, 0x3a chip id (u16), 0x3c expected program id (u32; RUMBA-S4H: checksum),
 *    0x40 OIS type (0x1626fc copies 0x28..0x40 to o_ctrl+0xe72..0xe8a).
 *  - INIT by type (0x1603e4; table pflash_ois at .rodata 0x895e0 = 1, 2, 4, 3, 5, 6, 7): 1-4 and
 *    7 BU24721, 5 BU24532, 6 RUMBA-S4H; 0 the generic path, where chip id 0x4c1 is a SEM1217S
 *    (cam_ois_fw_download 0x162d74).
 *  - a request-manager device (cam_ois_component_bind 0x157b9c-0x157bdc: get_dev_info,
 *    link_setup, apply_req; only the I2C variant 0x1583b4-0x1583e0 adds flush_req and the
 *    frame-skip query): packets 4 (settings by request id, "aperture mode"), 0x7e (skip frames),
 *    0x7f (NOP). Packet 5 runs the chip function again with new calib/postinit buffers; 6 (park
 *    lens) is accepted and not acted on.
 *  - RUMBA-S4H and SEM1217S: stock rewrites the chip's flash when its version differs from the
 *    packaged image; here only with camera.k618_ois_flash_update=1, otherwise both versions are
 *    logged and the flash is left as it is.
 */
static bool k618_ois_flash_update;
module_param(k618_ois_flash_update, bool, 0644);
MODULE_PARM_DESC(k618_ois_flash_update,
	"k618: 13 Ultra OIS -- rewrite the RUMBA-S4H / SEM1217S flash when its version differs from the packaged image (off: log both versions)");

static uint k618_bu24532_fw_update_op;
module_param(k618_bu24532_fw_update_op, uint, 0644);
MODULE_PARM_DESC(k618_bu24532_fw_update_op,
	"k618: 13 Ultra BU24532 -- stock bu24532_ois_fw_update_op: 0 download if the program id differs after auto boot, 1 download unless the expected id is 0, 2 always");

#define K618_BU24532_STATUS	0x6024	/* SETTING_6024: == 1 within 100 ms */
#define K618_BU24532_PROG_ID	0x6010	/* SETTING_PROGRAM_ID, dword */
#define K618_BU24532_DL_SID	0x50	/* 7-bit; stock sets cci_client->sid to it around each burst */
#define K618_RUMBAS4H_FW_SIZE	0x7000
#define K618_RUMBAS4H_VER_OFS	0x6ff4
#define K618_SEM1217S_FW_SIZE	0xc000
#define K618_SEM1217S_VER_OFS	0xbff4

static uint32_t k618_le32(const uint8_t *p)
{
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int k618_ultra_cci(struct cam_ois_ctrl_t *o_ctrl, const char *chip)
{
	if (o_ctrl->io_master_info.master_type != CCI_MASTER ||
		!o_ctrl->io_master_info.cci_client) {
		CAM_ERR(CAM_OIS, "k618: %s OIS %s needs a CCI master (type %d)", chip,
			o_ctrl->ois_name, o_ctrl->io_master_info.master_type);
		return -EINVAL;
	}
	return 0;
}

/* len bytes from addr, word address (stock rumbas4h_i2c_read_data 0x15c320, i2c_read_data 0x16448c) */
static int k618_ois_rd(struct cam_ois_ctrl_t *o_ctrl, uint32_t addr, uint8_t *buf, int32_t len)
{
	int rc = camera_io_dev_read_seq(&(o_ctrl->io_master_info), addr, buf,
		CAMERA_SENSOR_I2C_TYPE_WORD, CAMERA_SENSOR_I2C_TYPE_BYTE, len);

	if (rc < 0)
		CAM_ERR(CAM_OIS, "k618: OIS %s: read 0x%04x (%d bytes) failed: %d",
			o_ctrl->ois_name, addr, len, rc);
	return rc;
}

/*
 * len (1..256) bytes from addr on, word address, byte data, then delay_ms: as one burst (the
 * RUMBA-S4H data buffer and checksum) or sequential (stock i2c_write_data 0x1645c0, SEM1217S)
 */
static int k618_ois_wr(struct cam_ois_ctrl_t *o_ctrl, uint32_t addr, const uint8_t *buf,
	uint32_t len, uint8_t mode, unsigned short delay_ms)
{
	struct cam_sensor_i2c_reg_setting setting = {0};
	struct cam_sensor_i2c_reg_array *regs;
	uint32_t i;
	int rc;

	if (!len || len > 256)
		return -EINVAL;
	regs = kcalloc(len, sizeof(*regs), GFP_KERNEL);
	if (!regs)
		return -ENOMEM;
	for (i = 0; i < len; i++) {
		regs[i].reg_addr = (mode == CAM_SENSOR_I2C_WRITE_SEQ) ? addr + i : addr;
		regs[i].reg_data = buf[i];
	}
	setting.reg_setting = regs;
	setting.size = len;
	setting.addr_type = CAMERA_SENSOR_I2C_TYPE_WORD;
	setting.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
	setting.delay = delay_ms;
	rc = camera_io_dev_write_continuous(&(o_ctrl->io_master_info), &setting, mode);
	kfree(regs);
	if (rc < 0)
		CAM_ERR(CAM_OIS, "k618: OIS %s: write 0x%04x (%u bytes) failed: %d",
			o_ctrl->ois_name, addr, len, rc);
	return rc;
}

/*
 * stock bu24721_ois_pkt_download, the 13 Ultra's (0x158980): camera 16's k618_rohm_ois_init()
 * with three differences -- a restart ahead of the first program-id read (F050, wait, F097 +1 ms,
 * F058 +8 ms, F050, wait: 0x158a20-0x158aa8), the expected program id from the slave info
 * (0x158af8, o_ctrl+0xe86) where the 13 Pro's module indexes a table by type (0x156a3c), and no
 * gyro-gain reads. A failed download goes back to the run-time slave before returning, as
 * camera 16 does (stock returns with the download slave set).
 */
static int k618_ultra_bu24721_init(struct cam_ois_ctrl_t *o_ctrl)
{
	uint32_t want = o_ctrl->k618_ultra_expect_id;
	uint32_t id;
	int rc, write_rc = 0;

	rc = k618_ultra_cci(o_ctrl, "BU24721");
	if (rc)
		return rc;

	k618_rohm_write(o_ctrl, K618_ROHM_STANDBY, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 1);
	k618_rohm_wait_ready(o_ctrl);
	k618_rohm_write(o_ctrl, K618_ROHM_RESTART_1, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 1);
	k618_rohm_write(o_ctrl, K618_ROHM_RESTART_2, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 8);
	k618_rohm_write(o_ctrl, K618_ROHM_STANDBY, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 1);
	k618_rohm_wait_ready(o_ctrl);
	id = k618_rohm_read(o_ctrl, K618_ROHM_PROG_ID, CAMERA_SENSOR_I2C_TYPE_DWORD, true);
	k618_rohm_wait_ready(o_ctrl);
	CAM_INFO(CAM_OIS, "k618: BU24721 OIS %s type %u: program id 0x%08x, slave info wants 0x%08x",
		o_ctrl->ois_name, o_ctrl->k618_ultra_type, id, want);

	if (id != want) {
		k618_rohm_set_slave(o_ctrl, K618_ROHM_DL_SLAVE);
		if (o_ctrl->i2c_init_data.is_settings_valid == 1) {
			rc = k618_rohm_apply_retry(o_ctrl, &o_ctrl->i2c_init_data);
			if (rc)	/* stock logs this and downloads all the same (0x158d08) */
				CAM_ERR(CAM_OIS, "k618: BU24721 OIS %s: download-mode settings: %d",
					o_ctrl->ois_name, rc);
		}
		rc = k618_rohm_load_fw(o_ctrl, "prog", o_ctrl->opcode.prog, &write_rc);
		if (!rc && write_rc >= 0)
			rc = k618_rohm_load_fw(o_ctrl, "coeff", o_ctrl->opcode.coeff, &write_rc);
		if (!rc && write_rc >= 0)
			rc = k618_rohm_load_fw(o_ctrl, "mem", o_ctrl->opcode.memory, &write_rc);
		k618_rohm_set_slave(o_ctrl, K618_ROHM_RUN_SLAVE);
		if (rc)
			return rc;

		k618_rohm_write(o_ctrl, K618_ROHM_RESTART_1, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 1);
		k618_rohm_write(o_ctrl, K618_ROHM_RESTART_2, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 8);
		k618_rohm_wait_ready(o_ctrl);
		k618_rohm_write(o_ctrl, K618_ROHM_STANDBY, 0, CAMERA_SENSOR_I2C_TYPE_BYTE, 1);
		k618_rohm_wait_ready(o_ctrl);
		id = k618_rohm_read(o_ctrl, K618_ROHM_PROG_ID, CAMERA_SENSOR_I2C_TYPE_DWORD, true);
		k618_rohm_wait_ready(o_ctrl);
		if (id == want && write_rc >= 0)
			CAM_INFO(CAM_OIS, "k618: BU24721 OIS %s: firmware loaded, program id 0x%08x",
				o_ctrl->ois_name, id);
		else
			CAM_ERR(CAM_OIS,
				"k618: BU24721 OIS %s: program id 0x%08x after the download, wants 0x%08x (write %d)",
				o_ctrl->ois_name, id, want, write_rc);
	}

	if (o_ctrl->is_ois_calib)
		k618_rohm_apply_calib(o_ctrl, &o_ctrl->i2c_calib_data);
	if (o_ctrl->i2c_fwinit_data.is_settings_valid != 1)
		return 0;
	rc = k618_rohm_apply_retry(o_ctrl, &o_ctrl->i2c_fwinit_data);
	if (rc)
		CAM_ERR(CAM_OIS, "k618: BU24721 OIS %s: fwinit settings: %d", o_ctrl->ois_name, rc);
	return rc;
}

static void k618_bu24532_wait(struct cam_ois_ctrl_t *o_ctrl)
{
	int rc = camera_io_dev_poll(&(o_ctrl->io_master_info), K618_BU24532_STATUS, 0x01, 0,
		CAMERA_SENSOR_I2C_TYPE_WORD, CAMERA_SENSOR_I2C_TYPE_BYTE, 100);

	if (rc)
		CAM_WARN(CAM_OIS, "k618: BU24532 OIS %s: status 0x%04x wait %d",
			o_ctrl->ois_name, K618_BU24532_STATUS, rc);
}

/* stock apply_fw_setting (0x1622bc), the "auto boot" */
static void k618_bu24532_boot(struct cam_ois_ctrl_t *o_ctrl)
{
	CAM_DBG(CAM_OIS, "k618: [BU24532] %s: apply kernel firmware setting", o_ctrl->ois_name);
	k618_rohm_write(o_ctrl, 0x7010, 0x00, CAMERA_SENSOR_I2C_TYPE_BYTE, 0);
	k618_bu24532_wait(o_ctrl);
	k618_rohm_write(o_ctrl, 0x6019, 0x87, CAMERA_SENSOR_I2C_TYPE_BYTE, 0);
	k618_rohm_write(o_ctrl, 0x6018, 0x10, CAMERA_SENSOR_I2C_TYPE_BYTE, 0);
	k618_bu24532_wait(o_ctrl);
	k618_rohm_write(o_ctrl, 0x6080, 0xc800b4e1, CAMERA_SENSOR_I2C_TYPE_DWORD, 0);
	k618_rohm_write(o_ctrl, 0x6084, 0x00000000, CAMERA_SENSOR_I2C_TYPE_DWORD, 0);
	k618_bu24532_wait(o_ctrl);
	k618_rohm_write(o_ctrl, 0x6080, 0xc800b0e1, CAMERA_SENSOR_I2C_TYPE_DWORD, 0);
	k618_rohm_write(o_ctrl, 0x6084, 0x00000001, CAMERA_SENSOR_I2C_TYPE_DWORD, 0);
	k618_bu24532_wait(o_ctrl);
	k618_rohm_write(o_ctrl, 0xF011, 0x00, CAMERA_SENSOR_I2C_TYPE_BYTE, 0);
	msleep(40);
}

/* stock 0x15a008-0x15a114, ahead of a download */
static void k618_bu24532_dl_setup(struct cam_ois_ctrl_t *o_ctrl)
{
	k618_rohm_write(o_ctrl, 0x7010, 0x00, CAMERA_SENSOR_I2C_TYPE_BYTE, 0);
	k618_bu24532_wait(o_ctrl);
	k618_rohm_write(o_ctrl, 0x6019, 0x87, CAMERA_SENSOR_I2C_TYPE_BYTE, 0);
	k618_rohm_write(o_ctrl, 0x6018, 0x10, CAMERA_SENSOR_I2C_TYPE_BYTE, 0);
	k618_bu24532_wait(o_ctrl);
	k618_rohm_write(o_ctrl, 0x6080, 0x10868ce1, CAMERA_SENSOR_I2C_TYPE_DWORD, 0);
	k618_rohm_write(o_ctrl, 0x6084, 0x08068180, CAMERA_SENSOR_I2C_TYPE_DWORD, 0);
	k618_bu24532_wait(o_ctrl);
	k618_rohm_write(o_ctrl, 0x6080, 0x108690e1, CAMERA_SENSOR_I2C_TYPE_DWORD, 0);
	k618_rohm_write(o_ctrl, 0x6084, 0x08068100, CAMERA_SENSOR_I2C_TYPE_DWORD, 0);
	k618_bu24532_wait(o_ctrl);
	k618_rohm_write(o_ctrl, 0xF013, 0x00, CAMERA_SENSOR_I2C_TYPE_BYTE, 0);
	usleep_range(2000, 2010);
}

/*
 * <ois_name>.<suffix> (stock 0x15a4f4-0x15a708 for .prog; .coeff, .mem and .pher alike): size / 4
 * big-endian dwords, 16 per burst to the download slave at base + 64 * page, the page counted up
 * per burst when the slave info's page-step byte is set, 10 ms after each burst
 */
static int k618_bu24532_load(struct cam_ois_ctrl_t *o_ctrl, const char *suffix, uint32_t base)
{
	struct cam_sensor_cci_client *cci = o_ctrl->io_master_info.cci_client;
	struct cam_sensor_i2c_reg_array regs[16];
	struct cam_sensor_i2c_reg_setting setting = {0};
	const struct firmware *fw = NULL;
	char name[MAX_OIS_NAME_SIZE + 8];
	uint32_t count, done, page = 0, n = 0, i;
	uint16_t sid;
	int rc;

	snprintf(name, sizeof(name), "%s.%s", o_ctrl->ois_name, suffix);
	rc = request_firmware(&fw, name, &(o_ctrl->pdev->dev));
	if (rc) {
		CAM_ERR(CAM_OIS, "k618: [BU24532]Failed to locate %s: %d", name, rc);
		return rc;
	}
	count = (fw->size >> 2) & 0xffff;	/* stock: ubfx w24, w8, #2, #16 */
	setting.reg_setting = regs;
	setting.addr_type = o_ctrl->k618_fw_addr_type;
	if (setting.addr_type <= CAMERA_SENSOR_I2C_TYPE_INVALID ||
		setting.addr_type >= CAMERA_SENSOR_I2C_TYPE_MAX) {
		CAM_WARN(CAM_OIS, "k618: [BU24532] firmware address type %u, using word",
			o_ctrl->k618_fw_addr_type);
		setting.addr_type = CAMERA_SENSOR_I2C_TYPE_WORD;
	}
	setting.data_type = CAMERA_SENSOR_I2C_TYPE_DWORD;
	setting.delay = 10;
	sid = cci->sid;
	for (done = 0; done < count; done += n) {
		n = min_t(uint32_t, ARRAY_SIZE(regs), count - done);
		for (i = 0; i < n; i++) {
			const uint8_t *p = fw->data + 4 * (done + i);

			regs[i].reg_addr = base + (page << 6);
			regs[i].reg_data = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
				((uint32_t)p[2] << 8) | p[3];
			regs[i].delay = 0;
			regs[i].data_mask = 0;
		}
		setting.size = n;
		if (o_ctrl->k618_fw_page_inc)
			page++;
		cci->sid = K618_BU24532_DL_SID;
		rc = camera_io_dev_write_continuous(&(o_ctrl->io_master_info), &setting,
			CAM_SENSOR_I2C_WRITE_BURST);
		cci->sid = sid;
		if (rc < 0) {
			CAM_ERR(CAM_OIS, "k618: [BU24532]OIS FW(%s) size(%u) download failed rc: %d",
				suffix, count, rc);
			break;
		}
	}
	if (rc >= 0)
		CAM_INFO(CAM_OIS, "k618: [BU24532] %s: %s, %u dwords from 0x%x, page step %u",
			o_ctrl->ois_name, name, count, base, o_ctrl->k618_fw_page_inc);
	release_firmware(fw);
	return rc < 0 ? rc : 0;
}

/*
 * stock bu24532_ois_pkt_download (0x159d74). A fwinit buffer is applied instead of all the rest
 * (0x159dac). Expected id 0xff: no boot, no download (0x159e8c). With update op 0 (stock's
 * default): auto boot (skipped in packet 5), read the program id; in packet 5 a mismatch gets up
 * to three more boots, then -EINVAL (0x15a234-0x15a4ec). A download (decided at 0x159f88: op 2,
 * or op 0/1 with a mismatch) sends .prog .coeff .mem .pher, boots and reads the id again; with
 * op 0 a mismatch then fails (0x15bce0). Then postinit and calib. Stock's fallback of a download
 * read from the module's EEPROM (bu24532_ois_enable_read_eeprom, default 0) is not ported.
 */
static int k618_bu24532_init(struct cam_ois_ctrl_t *o_ctrl)
{
	uint32_t want = o_ctrl->k618_ultra_expect_id, id = 0;
	uint32_t op = k618_bu24532_fw_update_op;
	int rc, retry;

	rc = k618_ultra_cci(o_ctrl, "BU24532");
	if (rc)
		return rc;

	if (o_ctrl->i2c_fwinit_data.is_settings_valid == 1) {
		rc = k618_rohm_apply_retry(o_ctrl, &o_ctrl->i2c_fwinit_data);
		if (rc)
			CAM_ERR(CAM_OIS, "k618: [BU24532] %s: Cannot apply fwinit data %d",
				o_ctrl->ois_name, rc);
		return rc;
	}

	if (want != 0xff) {
		if (op == 0) {
			if (!o_ctrl->k618_ultra_redl)
				k618_bu24532_boot(o_ctrl);
			k618_bu24532_wait(o_ctrl);
			k618_bu24532_wait(o_ctrl);
			id = k618_rohm_read(o_ctrl, K618_BU24532_PROG_ID,
				CAMERA_SENSOR_I2C_TYPE_DWORD, true);
			CAM_INFO(CAM_OIS, "k618: [BU24532] %s: read program ID:0x%x, current ID:0x%x",
				o_ctrl->ois_name, id, want);
			for (retry = 0; o_ctrl->k618_ultra_redl && id != want && retry < 3; retry++) {
				CAM_INFO(CAM_OIS,
					"k618: [BU24532] %s: retry[%d:max%d] auto boot when open camera",
					o_ctrl->ois_name, retry, 3);
				k618_bu24532_boot(o_ctrl);
				k618_bu24532_wait(o_ctrl);
				k618_bu24532_wait(o_ctrl);
				id = k618_rohm_read(o_ctrl, K618_BU24532_PROG_ID,
					CAMERA_SENSOR_I2C_TYPE_DWORD, true);
				CAM_INFO(CAM_OIS, "k618: [BU24532] %s: read program ID:0x%x, current ID:0x%x",
					o_ctrl->ois_name, id, want);
			}
			if (o_ctrl->k618_ultra_redl && id != want) {
				CAM_ERR(CAM_OIS,
					"k618: [BU24532] %s: read program ID:0x%x, current ID:0x%x, AUTO BOOT failed",
					o_ctrl->ois_name, id, want);
				return -EINVAL;
			}
		}
		if (op == 2 || ((op == 0 || op == 1) && id != want)) {
			k618_bu24532_dl_setup(o_ctrl);
			rc = k618_bu24532_load(o_ctrl, "prog", o_ctrl->opcode.prog);
			if (!rc)
				rc = k618_bu24532_load(o_ctrl, "coeff", o_ctrl->opcode.coeff);
			if (!rc)
				rc = k618_bu24532_load(o_ctrl, "mem", o_ctrl->opcode.memory);
			if (!rc)
				rc = k618_bu24532_load(o_ctrl, "pher", o_ctrl->opcode.pheripheral);
			if (!rc) {
				k618_bu24532_boot(o_ctrl);
				k618_bu24532_wait(o_ctrl);
				k618_bu24532_wait(o_ctrl);
				id = k618_rohm_read(o_ctrl, K618_BU24532_PROG_ID,
					CAMERA_SENSOR_I2C_TYPE_DWORD, true);
				CAM_INFO(CAM_OIS,
					"k618: [BU24532] %s: after DL, read program ID:0x%x, current ID:0x%x",
					o_ctrl->ois_name, id, want);
				if (op == 0 && id != want)
					rc = -EINVAL;
			}
			if (rc) {
				CAM_ERR(CAM_OIS, "k618: [BU24532] %s: fw update failed: %d",
					o_ctrl->ois_name, rc);
				return rc;
			}
			CAM_INFO(CAM_OIS, "k618: [BU24532] %s: FW update success", o_ctrl->ois_name);
		}
	}

	if (o_ctrl->k618_postinit_data.is_settings_valid == 1) {
		rc = k618_rohm_apply_retry(o_ctrl, &o_ctrl->k618_postinit_data);
		if (rc) {
			CAM_ERR(CAM_OIS, "k618: [BU24532] %s: Cannot apply postinit data %d",
				o_ctrl->ois_name, rc);
			return rc;
		}
	}
	if (o_ctrl->is_ois_calib) {
		rc = k618_rohm_apply_retry(o_ctrl, &o_ctrl->i2c_calib_data);
		if (rc) {
			CAM_ERR(CAM_OIS, "k618: [BU24532] %s: Cannot apply calib data %d",
				o_ctrl->ois_name, rc);
			return rc;
		}
	}
	return 0;
}

/*
 * stock RUMBA-S4H flash rewrite (0x15cb68-0x15d1f0), at FAST mode: 0x0001 must read 1; 0x000C =
 * 0x75 (+65 ms); the image as 112 bursts of 256 bytes to 0x0100 (+13 ms each); 0x0006 must read
 * 0; 0x0008..0x000B = checksum low, checksum high, 0x00, 0x80 (+210 ms), the checksum being the
 * slave info's 0x3c (0x15cd98); 0x0006 must read 0; 0x0036 = 0x41 (+190 ms), 0x000D = 0x01
 * (+20 ms); 0x0001 must reach 9 within ten reads 1 ms apart; 0x000E = 0x06 (+10 ms)
 */
static int k618_rumbas4h_flash(struct cam_ois_ctrl_t *o_ctrl, const uint8_t *img)
{
	uint16_t cksum = o_ctrl->k618_ultra_expect_id & 0xffff;
	uint8_t b[4] = {0};
	uint32_t blk;
	int rc, i;

	rc = k618_ois_rd(o_ctrl, 0x0001, b, 1);
	if (rc < 0)
		return rc;
	if (b[0] != 1) {
		CAM_ERR(CAM_OIS, "k618: [RUMBAS4H] %s: not in idle status (0x%02x)",
			o_ctrl->ois_name, b[0]);
		return -EINVAL;
	}
	CAM_INFO(CAM_OIS, "k618: [RUMBAS4H] %s: in idle status, begin FW update", o_ctrl->ois_name);
	k618_rohm_write(o_ctrl, 0x000C, 0x75, CAMERA_SENSOR_I2C_TYPE_BYTE, 65);
	for (blk = 0; blk < K618_RUMBAS4H_FW_SIZE / 256; blk++) {
		rc = k618_ois_wr(o_ctrl, 0x0100, img + 256 * blk, 256,
			CAM_SENSOR_I2C_WRITE_BURST, 13);
		if (rc < 0)
			return rc;
	}
	rc = k618_ois_rd(o_ctrl, 0x0006, b, 2);
	if (rc < 0)
		return rc;
	if (b[0] | b[1]) {
		CAM_ERR(CAM_OIS, "k618: [RUMBAS4H] %s: FW update fail, 0x0006 = 0x%04x",
			o_ctrl->ois_name, b[0] | (b[1] << 8));
		return -EINVAL;
	}
	b[0] = cksum & 0xff;
	b[1] = cksum >> 8;
	b[2] = 0x00;
	b[3] = 0x80;
	rc = k618_ois_wr(o_ctrl, 0x0008, b, 4, CAM_SENSOR_I2C_WRITE_BURST, 210);
	if (rc < 0)
		return rc;
	rc = k618_ois_rd(o_ctrl, 0x0006, b, 2);
	if (rc < 0)
		return rc;
	CAM_INFO(CAM_OIS, "k618: [RUMBAS4H] %s: CHKSUM status = 0x%x", o_ctrl->ois_name,
		b[0] | (b[1] << 8));
	if (b[0] | b[1]) {
		CAM_ERR(CAM_OIS, "k618: [RUMBAS4H] %s: FW update fail", o_ctrl->ois_name);
		return -EINVAL;
	}
	k618_rohm_write(o_ctrl, 0x0036, 0x41, CAMERA_SENSOR_I2C_TYPE_BYTE, 190);
	k618_rohm_write(o_ctrl, 0x000D, 0x01, CAMERA_SENSOR_I2C_TYPE_BYTE, 20);
	for (i = 0; i < 10; i++) {
		b[0] = 0;
		k618_ois_rd(o_ctrl, 0x0001, b, 1);
		msleep(1);
		if (b[0] == 9)
			break;
	}
	if (b[0] != 9) {
		CAM_ERR(CAM_OIS, "k618: [RUMBAS4H] %s: software reset fail, addr 0x1 rxdata 0x%x",
			o_ctrl->ois_name, b[0]);
		return -EINVAL;
	}
	CAM_INFO(CAM_OIS, "k618: [RUMBAS4H] %s: software reset success", o_ctrl->ois_name);
	k618_rohm_write(o_ctrl, 0x000E, 0x06, CAMERA_SENSOR_I2C_TYPE_BYTE, 10);
	CAM_INFO(CAM_OIS, "k618: [RUMBAS4H] %s: FW update success", o_ctrl->ois_name);
	return 0;
}

/*
 * stock rumbas4h_ois_pkt_download (0x15c840): the packaged version is the u32 at 0x6ff4 of
 * <ois_name>.prog, the chip's 0x00FC (4 bytes; three tries 1 ms apart, then -EINVAL); a
 * difference rewrites the flash (here only with k618_ois_flash_update); then calib (a failure is
 * logged and the init settings still go out, 0x15ce58) and the init settings.
 */
static int k618_rumbas4h_init(struct cam_ois_ctrl_t *o_ctrl)
{
	struct cam_sensor_cci_client *cci;
	const struct firmware *fw = NULL;
	char name[MAX_OIS_NAME_SIZE + 8];
	enum i2c_freq_mode freq;
	uint8_t v[4] = {0};
	uint32_t cur, pkg = 0;
	bool have_pkg = false;
	int rc, i;

	rc = k618_ultra_cci(o_ctrl, "RUMBAS4H");
	if (rc)
		return rc;
	cci = o_ctrl->io_master_info.cci_client;
	freq = cci->i2c_freq_mode;

	snprintf(name, sizeof(name), "%s.prog", o_ctrl->ois_name);
	if (request_firmware(&fw, name, &(o_ctrl->pdev->dev))) {
		CAM_ERR(CAM_OIS, "k618: [RUMBAS4H] Failed to load %s", name);
		fw = NULL;
	} else if (fw->size < K618_RUMBAS4H_FW_SIZE) {
		CAM_ERR(CAM_OIS, "k618: [RUMBAS4H] %s is %zu bytes, the image has %d",
			name, fw->size, K618_RUMBAS4H_FW_SIZE);
	} else {
		pkg = k618_le32(fw->data + K618_RUMBAS4H_VER_OFS);
		have_pkg = true;
	}

	for (i = 0; i < 3; i++) {
		rc = k618_ois_rd(o_ctrl, 0x00FC, v, 4);
		if (rc >= 0)
			break;
		msleep(1);
	}
	if (rc < 0) {
		CAM_ERR(CAM_OIS, "k618: [RUMBAS4H] %s: version read failed %d times",
			o_ctrl->ois_name, i);
		rc = -EINVAL;
		goto out;
	}
	cur = k618_le32(v);
	CAM_INFO(CAM_OIS, "k618: [RUMBAS4H] %s: current firmware version = %u, packaged = %u%s",
		o_ctrl->ois_name, cur, pkg, have_pkg ? "" : " (no image)");
	if (have_pkg && cur != pkg) {
		if (!k618_ois_flash_update) {
			CAM_INFO(CAM_OIS,
				"k618: [RUMBAS4H] %s: versions differ, flash left as it is (camera.k618_ois_flash_update=0)",
				o_ctrl->ois_name);
		} else {
			cci->i2c_freq_mode = I2C_FAST_MODE;
			rc = k618_rumbas4h_flash(o_ctrl, fw->data);
			cci->i2c_freq_mode = freq;
			if (rc)
				goto out;
		}
	}

	rc = 0;
	if (o_ctrl->is_ois_calib) {
		rc = k618_rohm_apply_retry(o_ctrl, &o_ctrl->i2c_calib_data);
		if (rc)
			CAM_ERR(CAM_OIS, "k618: [RUMBAS4H] %s: Cannot apply calib data: %d",
				o_ctrl->ois_name, rc);
	}
	if (o_ctrl->i2c_init_data.is_settings_valid == 1) {
		rc = k618_rohm_apply_retry(o_ctrl, &o_ctrl->i2c_init_data);
		if (rc)
			CAM_ERR(CAM_OIS, "k618: [RUMBAS4H] %s: Cannot apply Init settings: rc = %d",
				o_ctrl->ois_name, rc);
	}
out:
	release_firmware(fw);
	return rc;
}

/*
 * stock cam_sem1217s_ois_fw_download, inlined in cam_ois_fw_download (0x163274-0x163d58): with a
 * firmware already there, OIS and AF stopped unless idle (REG_OIS_STS 0x0001 / REG_OIS_CTRL
 * 0x0000, REG_AF_STS 0x0201 / REG_AF_CTRL 0x0200); REG_FWUP_CTRL 0x1000 = 7, 60 ms, 0x0001 must
 * read 2; 192 blocks of 256 bytes to REG_DATA_BUF 0x1100, 20 ms each; the 16-bit sum of the
 * image's little-endian halfwords to REG_FWUP_CHKSUM 0x1002, 200 ms; REG_FWUP_ERR 0x1001 must
 * read 0; REG_FWUP_CTRL = 0x80, 200 ms; REG_APP_VER 0x1008 must then read the packaged version
 */
static int k618_sem1217s_flash(struct cam_ois_ctrl_t *o_ctrl, const uint8_t *img, uint32_t cur,
	uint32_t pkg)
{
	uint8_t b[4] = {0};
	uint32_t sum = 0, ver, i, j;
	int rc;

	if (cur) {
		rc = k618_ois_rd(o_ctrl, 0x0001, b, 1);
		if (rc < 0)
			return rc;
		if (b[0] != 1) {
			b[0] = 0;
			rc = k618_ois_wr(o_ctrl, 0x0000, b, 1, CAM_SENSOR_I2C_WRITE_SEQ, 0);
			if (rc < 0)
				return rc;
		}
		rc = k618_ois_rd(o_ctrl, 0x0201, b, 1);
		if (rc < 0)
			return rc;
		if (b[0] != 1) {
			b[0] = 0;
			rc = k618_ois_wr(o_ctrl, 0x0200, b, 1, CAM_SENSOR_I2C_WRITE_SEQ, 0);
			if (rc < 0)
				return rc;
		}
	}
	b[0] = 0x07;
	rc = k618_ois_wr(o_ctrl, 0x1000, b, 1, CAM_SENSOR_I2C_WRITE_SEQ, 0);
	if (rc < 0)
		return rc;
	msleep(60);
	rc = k618_ois_rd(o_ctrl, 0x0001, b, 1);
	if (rc < 0)
		return rc;
	if (b[0] != 2) {
		CAM_ERR(CAM_OIS, "k618: [SEM1217S] %s: OIS firmware upgrade status check failed (0x%02x)",
			o_ctrl->ois_name, b[0]);
		return -EINVAL;
	}
	for (i = 0; i < K618_SEM1217S_FW_SIZE / 256; i++) {
		const uint8_t *blk = img + 256 * i;

		for (j = 0; j < 256; j += 2)
			sum += blk[j] | (blk[j + 1] << 8);
		rc = k618_ois_wr(o_ctrl, 0x1100, blk, 256, CAM_SENSOR_I2C_WRITE_SEQ, 0);
		if (rc < 0) {
			CAM_ERR(CAM_OIS, "k618: [SEM1217S] %s: Failed to write REG_DATA_BUF, index = %u",
				o_ctrl->ois_name, i);
			return rc;
		}
		msleep(20);
	}
	b[0] = sum & 0xff;
	b[1] = (sum >> 8) & 0xff;
	rc = k618_ois_wr(o_ctrl, 0x1002, b, 2, CAM_SENSOR_I2C_WRITE_SEQ, 0);
	if (rc < 0)
		return rc;
	msleep(200);
	rc = k618_ois_rd(o_ctrl, 0x1002, b, 2);
	if (rc < 0)
		return rc;
	CAM_INFO(CAM_OIS, "k618: [SEM1217S] %s: REG_FWUP_CHKSUM written 0x%x, read 0x%x",
		o_ctrl->ois_name, sum & 0xffff, b[0] | (b[1] << 8));
	rc = k618_ois_rd(o_ctrl, 0x1001, b, 1);
	if (rc < 0)
		return rc;
	if (b[0]) {
		CAM_ERR(CAM_OIS, "k618: [SEM1217S] %s: Failed to update firmware, REG_FWUP_ERR = 0x%x",
			o_ctrl->ois_name, b[0]);
		return -EINVAL;
	}
	b[0] = 0x80;
	rc = k618_ois_wr(o_ctrl, 0x1000, b, 1, CAM_SENSOR_I2C_WRITE_SEQ, 0);
	if (rc < 0)
		return rc;
	msleep(200);
	rc = k618_ois_rd(o_ctrl, 0x1008, b, 4);
	if (rc < 0)
		return rc;
	ver = k618_le32(b);
	if (ver != pkg) {
		CAM_ERR(CAM_OIS, "k618: [SEM1217S] %s: firmware version %u after the update, wants %u",
			o_ctrl->ois_name, ver, pkg);
		return -EINVAL;
	}
	CAM_INFO(CAM_OIS, "k618: [SEM1217S] %s: Firmware update success, version %u",
		o_ctrl->ois_name, ver);
	return 0;
}

/*
 * stock cam_ois_fw_download, chip id 0x4c1 (0x162d84): stands in for the generic download. The
 * packaged version is the u32 at 0xbff4 of <ois_name>.prog (0xc000 bytes), the chip's REG_APP_VER
 * 0x1008; a failed version read fails the INIT as on stock. Stock's semco_ois_fw_update_op
 * (default 1) rewrites on any difference; here k618_ois_flash_update does, off by default.
 */
static int k618_sem1217s_fw(struct cam_ois_ctrl_t *o_ctrl)
{
	const struct firmware *fw = NULL;
	char name[MAX_OIS_NAME_SIZE + 8];
	uint8_t b[4] = {0};
	uint32_t cur, pkg = 0;
	bool have_pkg = false;
	int rc;

	snprintf(name, sizeof(name), "%s.prog", o_ctrl->ois_name);
	rc = request_firmware(&fw, name, &(o_ctrl->pdev->dev));
	if (rc) {
		CAM_ERR(CAM_OIS, "k618: [SEM1217S] Failed to load firmware: %s (%d)", name, rc);
		fw = NULL;
	} else if (fw->size < K618_SEM1217S_FW_SIZE) {
		CAM_ERR(CAM_OIS, "k618: [SEM1217S] %s is %zu bytes, the image has %d",
			name, fw->size, K618_SEM1217S_FW_SIZE);
	} else {
		pkg = k618_le32(fw->data + K618_SEM1217S_VER_OFS);
		have_pkg = true;
	}
	if (!have_pkg && k618_ois_flash_update) {
		rc = rc ? rc : -EINVAL;	/* stock: no image, the INIT fails */
		goto out;
	}

	rc = k618_ois_rd(o_ctrl, 0x1008, b, 4);
	if (rc < 0)
		goto out;
	cur = k618_le32(b);
	CAM_INFO(CAM_OIS, "k618: [SEM1217S] %s: current firmware version = %u, packaged = %u%s",
		o_ctrl->ois_name, cur, pkg, have_pkg ? "" : " (no image)");
	rc = 0;
	if (have_pkg && cur != pkg) {
		if (!k618_ois_flash_update)
			CAM_INFO(CAM_OIS,
				"k618: [SEM1217S] %s: versions differ, flash left as it is (camera.k618_ois_flash_update=0)",
				o_ctrl->ois_name);
		else
			rc = k618_sem1217s_flash(o_ctrl, fw->data, cur, pkg);
	}
out:
	release_firmware(fw);
	return rc;
}

/* stock pflash_ois (.rodata 0x895e0) as the compare chains 0x1603e4-0x1605c0 / 0x15fd00-0x160328 */
static int k618_ultra_chip_init(struct cam_ois_ctrl_t *o_ctrl)
{
	switch (o_ctrl->k618_ultra_type) {
	case 1:
	case 2:
	case 3:
	case 4:
	case 7:
		return k618_ultra_bu24721_init(o_ctrl);
	case 5:
		return k618_bu24532_init(o_ctrl);
	case 6:
		return k618_rumbas4h_init(o_ctrl);
	default:	/* stock logs "need pkt function" (0x160d6c) and returns 0 */
		CAM_ERR(CAM_OIS, "k618: OIS %s: type %u has no chip function",
			o_ctrl->ois_name, o_ctrl->k618_ultra_type);
		return 0;
	}
}

/* called from cam_ois_slaveInfo_pkt_parser() after camera 16's 59-byte fields */
static int k618_ultra_slave_info(struct cam_ois_ctrl_t *o_ctrl, const uint8_t *info, size_t len)
{
	const uint8_t *x = info + 0x38;

	o_ctrl->k618_ultra_info = false;
	o_ctrl->k618_ultra_type = 0;
	o_ctrl->k618_ultra_chip_id = 0;
	o_ctrl->k618_ultra_expect_id = 0;
	if (!o_ctrl->k618_ultra)
		return 0;
	if (len < K618_ULTRA_SLAVE_INFO_LEN) {
		CAM_ERR(CAM_OIS, "k618: OIS %s: slave info %zu bytes, the 13 Ultra's has %d",
			o_ctrl->ois_name, len, K618_ULTRA_SLAVE_INFO_LEN);
		return -EINVAL;
	}
	o_ctrl->k618_ultra_info = true;
	o_ctrl->k618_ois_flag = 0;	/* camera 16's ROHM branch is the 13 Pro's; the type is below */
	o_ctrl->k618_ultra_chip_id = x[2] | ((uint16_t)x[3] << 8);
	o_ctrl->k618_ultra_expect_id = k618_le32(x + 4);
	o_ctrl->k618_ultra_type = x[8];
	CAM_INFO(CAM_OIS,
		"k618: OIS %s (13 Ultra) slave info 0x38-0x40: %02x %02x %02x %02x %02x %02x %02x %02x %02x: address type %u, page step %u, chip id 0x%04x, expected id 0x%08x, type %u",
		o_ctrl->ois_name, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8],
		x[0], x[1], o_ctrl->k618_ultra_chip_id, o_ctrl->k618_ultra_expect_id,
		o_ctrl->k618_ultra_type);
	return 0;
}

/*
 * INIT, after power-up: type != 0 runs the chip function (stock 0x1603e4; a nonzero result is
 * "Failed OIS Customer Pkt Download", 0x1605cc). Type 0 goes on to the generic path; stock applies
 * fwinit there whether or not ois_fw_flag is set (0x160410; the download alone is under the flag,
 * 0x1604a8), 6.x only under the flag -- so with the flag clear it is applied here.
 */
static int k618_ultra_init(struct cam_ois_ctrl_t *o_ctrl)
{
	int rc;

	if (o_ctrl->k618_ultra_type) {
		o_ctrl->cam_ois_state = CAM_OIS_CONFIG;
		rc = k618_ultra_chip_init(o_ctrl);
		if (rc < 0) {
			CAM_ERR(CAM_OIS, "k618: OIS %s: Failed OIS Customer Pkt Download: %d (type %u)",
				o_ctrl->ois_name, rc, o_ctrl->k618_ultra_type);
			return rc;
		}
		if (rc)	/* a poll at the end timed out: logged, as camera 16 does */
			CAM_WARN(CAM_OIS, "k618: OIS %s: type %u: a poll timed out (%d)",
				o_ctrl->ois_name, o_ctrl->k618_ultra_type, rc);
		return K618_ULTRA_INIT_DONE;
	}
	if (!o_ctrl->ois_fw_flag && o_ctrl->i2c_fwinit_data.is_settings_valid == 1) {
		rc = cam_ois_apply_settings(o_ctrl, &o_ctrl->i2c_fwinit_data);
		if ((rc == -EAGAIN) && (o_ctrl->io_master_info.master_type == CCI_MASTER)) {
			CAM_WARN(CAM_OIS, "k618: Reapplying fwinit settings");
			usleep_range(1000, 1010);
			rc = cam_ois_apply_settings(o_ctrl, &o_ctrl->i2c_fwinit_data);
		}
		if (rc) {
			CAM_ERR(CAM_OIS, "k618: OIS %s: Cannot apply fwinit data %d",
				o_ctrl->ois_name, rc);
			return rc < 0 ? rc : -EINVAL;
		}
	}
	return 0;
}

/* the generic path's download: a SEM1217S (chip id 0x4c1) gets its own, as stock 0x162d84 */
static int k618_ultra_fw_download(struct cam_ois_ctrl_t *o_ctrl)
{
	if (o_ctrl->k618_ultra_info && o_ctrl->k618_ultra_chip_id == K618_SEM1217S_CHIP_ID)
		return k618_sem1217s_fw(o_ctrl);
	return cam_ois_fw_download(o_ctrl);
}

/* stock cam_ois_publish_dev_info (0x1613b8) */
static int k618_ois_publish_dev_info(struct cam_req_mgr_device_info *info)
{
	if (!info) {
		CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Invalid Args");
		return -EINVAL;
	}
	info->dev_id = CAM_REQ_MGR_DEVICE_OIS;
	strscpy(info->name, "cam-ois", sizeof(info->name));
	info->p_delay = CAM_PIPELINE_DELAY_1;
	info->m_delay = CAM_MODESWITCH_DELAY_1;	/* 6.x field (not in 5.15); as actuator and flash */
	info->trigger = CAM_TRIGGER_POINT_SOF;
	return 0;
}

/* stock cam_ois_establish_link (0x161444) */
static int k618_ois_establish_link(struct cam_req_mgr_core_dev_link_setup *link)
{
	struct cam_ois_ctrl_t *o_ctrl;

	if (!link) {
		CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Invalid Args");
		return -EINVAL;
	}
	o_ctrl = (struct cam_ois_ctrl_t *)cam_get_device_priv(link->dev_hdl);
	if (!o_ctrl) {
		CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Device data is NULL");
		return -EINVAL;
	}
	mutex_lock(&(o_ctrl->ois_mutex));
	if (link->link_enable) {
		o_ctrl->bridge_intf.link_hdl = link->link_hdl;
		o_ctrl->bridge_intf.crm_cb = link->crm_cb;
	} else {
		o_ctrl->bridge_intf.link_hdl = -1;
		o_ctrl->bridge_intf.crm_cb = NULL;
	}
	mutex_unlock(&(o_ctrl->ois_mutex));
	return 0;
}

/*
 * stock cam_ois_apply_request (0x161974): the slot of this request id is applied and marked
 * spent; a request in the skip table returns -EAGAIN while its count lasts, counting down (6.x's
 * request manager does not count -EAGAIN as a failed retry); otherwise the slot two requests back
 * is cleared.
 */
static int k618_ois_apply_request(struct cam_req_mgr_apply_request *apply)
{
	struct cam_ois_ctrl_t *o_ctrl;
	struct i2c_settings_array *slot;
	uint32_t n, idx, del;
	int rc = 0;

	if (!apply) {
		CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Invalid Input Args");
		return -EINVAL;
	}
	o_ctrl = (struct cam_ois_ctrl_t *)cam_get_device_priv(apply->dev_hdl);
	if (!o_ctrl) {
		CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Device data is NULL");
		return -EINVAL;
	}
	n = ARRAY_SIZE(o_ctrl->k618_req_slot);
	idx = apply->request_id % n;
	CAM_DBG(CAM_OIS, "k618: [CRM-OIS] OIS apply request:%llu", apply->request_id);

	mutex_lock(&(o_ctrl->ois_mutex));
	slot = &(o_ctrl->k618_req_slot[idx]);
	if ((slot->request_id == apply->request_id) && (slot->is_settings_valid == 1)) {
		o_ctrl->k618_poll_go_on = 0;	/* per-frame settings fail fast */
		rc = cam_ois_apply_settings(o_ctrl, slot);
		if (rc < 0) {
			CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Failed in applying the request: %llu.",
				apply->request_id);
			goto unlock;
		}
		slot->is_settings_valid = 0;
		CAM_DBG(CAM_OIS, "k618: Apply setting at req %llu, rc = %d", apply->request_id, rc);
	}

	if ((o_ctrl->k618_skip_req[idx] == apply->request_id) && o_ctrl->k618_skip_cnt[idx]) {
		CAM_INFO(CAM_OIS, "k618: [CRM-OIS] Skipframe number from %llu to %llu at request:%llu",
			o_ctrl->k618_skip_cnt[idx], o_ctrl->k618_skip_cnt[idx] - 1,
			apply->request_id);
		if (!--o_ctrl->k618_skip_cnt[idx])
			o_ctrl->k618_skip_req[idx] = 0;
		rc = -EAGAIN;
		goto unlock;
	}

	del = (idx + n - 2) % n;
	slot = &(o_ctrl->k618_req_slot[del]);
	if (apply->request_id > slot->request_id) {
		slot->request_id = 0;
		rc = delete_request(slot);
		if (rc < 0)
			CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Fail deleting the req: %u err: %d.", del, rc);
	} else {
		CAM_DBG(CAM_OIS, "k618: [CRM-OIS] No Valid Req to clean Up");
	}
unlock:
	mutex_unlock(&(o_ctrl->ois_mutex));
	return rc;
}

/* stock cam_ois_update_req_mgr (0x16152c); trigger_eof for packet 4 (0x16156c) */
static int k618_ois_add_req(struct cam_ois_ctrl_t *o_ctrl, struct cam_packet *csl_packet)
{
	struct cam_req_mgr_add_request add_req = {0};
	int rc;

	add_req.link_hdl = o_ctrl->bridge_intf.link_hdl;
	add_req.dev_hdl = o_ctrl->bridge_intf.device_hdl;
	add_req.req_id = csl_packet->header.request_id;
	if ((csl_packet->header.op_code & 0xFFFFFF) == K618_OIS_OPCODE_APERTURE)
		add_req.trigger_eof = true;
	if (!o_ctrl->bridge_intf.crm_cb || !o_ctrl->bridge_intf.crm_cb->add_req) {
		CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Can't add Request ID: %llu to CRM",
			add_req.req_id);
		return -EINVAL;
	}
	rc = o_ctrl->bridge_intf.crm_cb->add_req(&add_req);
	if (rc) {
		if (rc == -EBADR)
			CAM_INFO(CAM_OIS,
				"k618: [CRM-OIS] Adding request: %llu failed: rc: %d, it has been flushed",
				add_req.req_id, rc);
		else
			CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Adding request: %llu failed: rc: %d",
				add_req.req_id, rc);
		return rc;
	}
	CAM_DBG(CAM_OIS, "k618: [CRM-OIS] Request Id: %llu added to CRM", add_req.req_id);
	return 0;
}

/*
 * stock packet 0x7e (0x15fffc-0x160a68): the first buffer holds u64 request id, u64 count; the
 * table keeps the count less one (0x160a44), apply_request holds that request so many frames
 */
static int k618_ois_skip_frame(struct cam_ois_ctrl_t *o_ctrl, struct cam_packet *csl_packet,
	struct cam_cmd_buf_desc *cmd_desc)
{
	const uint64_t *p;
	uintptr_t cpu = 0;
	size_t len = 0;
	uint32_t idx;
	int rc;

	CAM_DBG(CAM_OIS, "k618: [SkipFrame] num_cmd_buf %u", csl_packet->num_cmd_buf);
	if (!csl_packet->num_cmd_buf) {
		CAM_ERR(CAM_OIS, "k618: [SkipFrame] Recvied skip frame packet is empty.");
		return -EINVAL;
	}
	if (cmd_desc->length < 2 * sizeof(uint64_t)) {
		CAM_ERR(CAM_OIS, "k618: [SkipFrame] Insufficient number of packet bytes received: %u.",
			cmd_desc->length);
		return -EINVAL;
	}
	rc = cam_mem_get_cpu_buf(cmd_desc->mem_handle, &cpu, &len);
	if (rc < 0) {
		CAM_ERR(CAM_OIS, "k618: [SkipFrame] Failed to get cpu buf: 0x%x", cmd_desc->mem_handle);
		return rc;
	}
	if (!cpu || len < 2 * sizeof(uint64_t) || len - 2 * sizeof(uint64_t) < cmd_desc->offset) {
		CAM_ERR(CAM_OIS, "k618: [SkipFrame] Invalid length for skip frame packet");
		rc = -EINVAL;
		goto put;
	}
	p = (const uint64_t *)(cpu + (cmd_desc->offset & ~3U));
	if (p[1]) {
		idx = p[0] % ARRAY_SIZE(o_ctrl->k618_skip_req);
		o_ctrl->k618_skip_req[idx] = p[0];
		o_ctrl->k618_skip_cnt[idx] = p[1] - 1;
		CAM_INFO(CAM_OIS, "k618: [SkipFrame] skip reqId: %llu, skip num: %llu queue index:%u",
			o_ctrl->k618_skip_req[idx], o_ctrl->k618_skip_cnt[idx], idx);
	}
put:
	cam_mem_put_cpu_buf(cmd_desc->mem_handle);
	return rc;
}

/*
 * stock packet 5 (0x15f510-0x15f6a4, 0x15fcfc, 0x160d8c): its buffers fill calib (when
 * is_ois_calib) and then postinit; with o_ctrl+0x11d8 set (k618_ultra_redl) the type's chip
 * function runs again; the two buffers are deleted. No state check, as stock.
 */
static int k618_ultra_redownload(struct cam_ois_ctrl_t *o_ctrl, struct cam_packet *csl_packet,
	struct cam_cmd_buf_desc *cmd_desc)
{
	struct i2c_settings_array *set;
	uintptr_t ptr = 0;
	size_t len = 0;
	uint32_t i;
	int rc = 0;

	CAM_DBG(CAM_OIS, "k618: OIS %s: num_cmd_buf %u", o_ctrl->ois_name, csl_packet->num_cmd_buf);
	for (i = 0; i < csl_packet->num_cmd_buf; i++) {
		if (!cmd_desc[i].length)
			continue;
		rc = cam_packet_util_validate_cmd_desc(&cmd_desc[i]);
		if (rc) {
			CAM_ERR(CAM_OIS, "k618: OIS %s: invalid cmd desc %u", o_ctrl->ois_name, i);
			goto out;
		}
		rc = cam_mem_get_cpu_buf(cmd_desc[i].mem_handle, &ptr, &len);
		if (rc < 0) {
			CAM_ERR(CAM_OIS, "k618: OIS %s: Failed to get cpu buf: 0x%x",
				o_ctrl->ois_name, cmd_desc[i].mem_handle);
			goto out;
		}
		cam_mem_put_cpu_buf(cmd_desc[i].mem_handle);
		if (!ptr || len < 8 || len - 8 < cmd_desc[i].offset) {	/* stock 0x15f6d0 */
			CAM_ERR(CAM_OIS, "k618: OIS %s: invalid length of cmd buf %u",
				o_ctrl->ois_name, i);
			rc = -EINVAL;
			goto out;
		}
		if (o_ctrl->is_ois_calib && (o_ctrl->i2c_calib_data.is_settings_valid == 0))
			set = &(o_ctrl->i2c_calib_data);
		else if (o_ctrl->k618_postinit_data.is_settings_valid == 0)
			set = &(o_ctrl->k618_postinit_data);
		else
			continue;
		set->is_settings_valid = 1;
		set->request_id = 0;
		rc = cam_sensor_i2c_command_parser(&(o_ctrl->io_master_info), set, &cmd_desc[i], 1,
			NULL);
		if (rc < 0) {
			CAM_ERR(CAM_OIS, "k618: OIS %s: re-download settings parsing failed: %d",
				o_ctrl->ois_name, rc);
			goto out;
		}
	}
	if (o_ctrl->k618_ultra_info && o_ctrl->k618_ultra_type) {
		o_ctrl->k618_ultra_redl = true;
		rc = k618_ultra_chip_init(o_ctrl);
		o_ctrl->k618_ultra_redl = false;
		if (rc < 0)
			CAM_ERR(CAM_OIS, "k618: OIS %s: Failed OIS Customer Pkt Download: %d",
				o_ctrl->ois_name, rc);
		else
			rc = 0;
	}
out:
	if (o_ctrl->i2c_calib_data.is_settings_valid == 1)
		delete_request(&(o_ctrl->i2c_calib_data));
	if (o_ctrl->k618_postinit_data.is_settings_valid == 1)
		delete_request(&(o_ctrl->k618_postinit_data));
	return rc;
}

/* cam_ois_pkt_parse(): stock packets 4, 5, 6, 0x7e and 0x7f (jump table 0x15f308, 0x15f3a8) */
static int k618_ultra_pkt(struct cam_ois_ctrl_t *o_ctrl, struct cam_packet *csl_packet)
{
	uint32_t op = csl_packet->header.op_code & 0xFFFFFF;
	uint32_t *offset = (uint32_t *)&csl_packet->payload_flex;
	struct cam_cmd_buf_desc *cmd_desc;
	struct i2c_settings_array *slot;
	uint64_t req = csl_packet->header.request_id;
	int rc;

	offset += (csl_packet->cmd_buf_offset / sizeof(uint32_t));
	cmd_desc = (struct cam_cmd_buf_desc *)(offset);

	switch (op) {
	case K618_OIS_OPCODE_APERTURE:	/* 0x15f4c0, 0x15ff64 */
		if (o_ctrl->cam_ois_state < CAM_OIS_CONFIG) {
			CAM_WARN(CAM_OIS, "k618: [CRM-OIS] Not in right state to control OIS: %d",
				o_ctrl->cam_ois_state);
			return -EINVAL;
		}
		CAM_DBG(CAM_OIS, "k618: [CRM-OIS] Received aperture mode packets");
		slot = &(o_ctrl->k618_req_slot[req % ARRAY_SIZE(o_ctrl->k618_req_slot)]);
		/* a slot left from a request 32 back is emptied first (stock appends to it) */
		if (slot->request_id != req && !list_empty(&(slot->list_head)))
			delete_request(slot);
		slot->request_id = req;
		slot->is_settings_valid = 1;
		rc = cam_sensor_i2c_command_parser(&(o_ctrl->io_master_info), slot, cmd_desc, 1,
			NULL);
		if (rc < 0) {
			CAM_ERR(CAM_OIS, "k618: [CRM-OIS] CRM OIS parsing failed: %d", rc);
			return rc;
		}
		rc = k618_ois_add_req(o_ctrl, csl_packet);
		if (rc)
			CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Failed in adding request to request manager");
		return rc;
	case CAM_PKT_NOP_OPCODE:	/* 0x15f3b0, 0x15fd30 */
		if (o_ctrl->cam_ois_state < CAM_OIS_CONFIG) {
			CAM_DBG(CAM_OIS, "k618: [CRM-OIS] Received NOP packets in invalid state: %d",
				o_ctrl->cam_ois_state);
			return -EINVAL;
		}
		rc = k618_ois_add_req(o_ctrl, csl_packet);
		if (rc)
			CAM_ERR(CAM_OIS, "k618: [CRM-OIS] Failed in adding request to request manager");
		return rc;
	case K618_OIS_OPCODE_SKIPFRAME:	/* 0x15f604 */
		if (o_ctrl->cam_ois_state < CAM_OIS_CONFIG) {
			CAM_WARN(CAM_OIS,
				"k618: [SkipFrame] Recvied skip frame packet in invalid state: %d",
				o_ctrl->cam_ois_state);
			return -EINVAL;
		}
		return k618_ois_skip_frame(o_ctrl, csl_packet, cmd_desc);
	case K618_OIS_OPCODE_REDOWNLOAD:	/* 0x15f510 */
		return k618_ultra_redownload(o_ctrl, csl_packet, cmd_desc);
	case K618_OIS_OPCODE_PARKLENS:	/* 0x15f560 */
		/*
		 * Stock parses the packet's park settings into o_ctrl+0xf88 and starts
		 * "parklens-thread", which walks them after release and then powers the OIS
		 * down; release skips its own power-down while the thread runs (0x15ee6c). Not
		 * ported: the packet is accepted, nothing is applied, and release powers the OIS
		 * down at once, as 6.x does and as stock does when no park thread was started.
		 */
		if (o_ctrl->cam_ois_state < CAM_OIS_CONFIG)
			return -EINVAL;
		CAM_DBG(CAM_OIS, "k618: OIS %s: park-lens packet (not ported) for request %llu",
			o_ctrl->ois_name, req);
		return 0;
	default:
		CAM_ERR(CAM_OIS, "Invalid Opcode: %d", op);
		return -EINVAL;
	}
}

/*
 * stock 0x15f280-0x15f2f4: a packet other than INIT for a request at or below the last flushed
 * one is refused, -EBADR. Only flush_req sets that id (0x161768), and stock registers flush_req
 * for the I2C variant only (0x1583c4) -- these OIS are platform devices -- so on both phones the
 * id stays 0 and nothing is refused.
 */
static int k618_ultra_flush_check(struct cam_ois_ctrl_t *o_ctrl, struct cam_packet *csl_packet)
{
	uint64_t req = csl_packet->header.request_id;

	if ((csl_packet->header.op_code & 0xFFFFFF) && o_ctrl->k618_last_flush_req &&
		(req <= o_ctrl->k618_last_flush_req)) {
		CAM_INFO(CAM_OIS, "k618: reject request %llu, last request to flush %llu",
			req, o_ctrl->k618_last_flush_req);
		return -EBADR;
	}
	if (req > o_ctrl->k618_last_flush_req)
		o_ctrl->k618_last_flush_req = 0;
	return 0;
}

/*
 * stock cam_sensor_i2c_read_write_ois_data (0x173ac8), which the 13 Ultra's OIS read packet uses
 * (0x15fe74) in place of cam_sensor_i2c_read_data: a random write in the list is sent where it
 * stands (6.x stops at it, -EINVAL: it has no read buffer), and a read lands after the bytes of
 * the read before it in the shared output buffer (6.x puts every read at the buffer's start).
 * The bound check against the buffer's end, offset included, is added here.
 */
static int k618_ultra_read_data(struct cam_ois_ctrl_t *o_ctrl, struct i2c_settings_array *set)
{
	struct i2c_settings_list *i2c_list;
	uint32_t prev = 0, len, cnt;
	uint8_t *buf;
	int rc = 0;

	if (!o_ctrl->k618_ultra)
		return cam_sensor_i2c_read_data(set, &(o_ctrl->io_master_info));

	list_for_each_entry(i2c_list, &(set->list_head), list) {
		struct cam_sensor_i2c_reg_setting *s = &(i2c_list->i2c_settings);

		if (i2c_list->op_code == CAM_SENSOR_I2C_WRITE_RANDOM) {
			rc = camera_io_dev_write(&(o_ctrl->io_master_info), s);
			if (rc) {
				CAM_ERR(CAM_OIS, "k618: OIS %s: write failed rc %d", o_ctrl->ois_name, rc);
				return rc;
			}
			continue;
		}
		if (!s->read_buff || !s->read_buff_len) {
			CAM_ERR(CAM_OIS, "k618: Invalid input buffer, buffer: %pK, length: %d",
				s->read_buff, s->read_buff_len);
			return -EINVAL;
		}
		if (i2c_list->op_code == CAM_SENSOR_I2C_READ_SEQ) {
			len = s->size;
			if (len > s->read_buff_len || prev > s->read_buff_len - len) {
				CAM_ERR(CAM_OIS, "k618: Invalid size, readLen:%u, offset:%u, bufLen:%u",
					len, prev, s->read_buff_len);
				return -EINVAL;
			}
			rc = camera_io_dev_read_seq(&(o_ctrl->io_master_info),
				s->reg_setting[0].reg_addr, s->read_buff + prev, s->addr_type,
				s->data_type, len);
			if (rc < 0) {
				CAM_ERR(CAM_OIS, "k618: failed: seq read I2C settings: %d", rc);
				return rc;
			}
			prev = len;
		} else if (i2c_list->op_code == CAM_SENSOR_I2C_READ_RANDOM) {
			len = s->data_type * s->size;
			if (len > s->read_buff_len || len < s->size || prev > s->read_buff_len - len) {
				CAM_ERR(CAM_OIS,
					"k618: Invalid size, readLen:%u, offset:%u, bufLen:%u, size: %u",
					len, prev, s->read_buff_len, s->size);
				return -EINVAL;
			}
			buf = s->read_buff + prev;
			for (cnt = 0; cnt < s->size; cnt++) {
				rc = camera_io_dev_read(&(o_ctrl->io_master_info),
					s->reg_setting[cnt].reg_addr, &(s->reg_setting[cnt].reg_data),
					s->addr_type, s->data_type, false);
				if (rc < 0) {
					CAM_ERR(CAM_OIS, "k618: Failed: random read I2C settings: %d", rc);
					return rc;
				}
				if (s->data_type <= CAMERA_SENSOR_I2C_TYPE_DWORD) {
					memcpy(buf, &(s->reg_setting[cnt].reg_data), s->data_type);
					buf += s->data_type;
				}
			}
			prev = len;
		}
	}
	return rc;
}

/* stock CAM_START_DEV (0x15eca8) */
static void k618_ultra_start(struct cam_ois_ctrl_t *o_ctrl)
{
	if (o_ctrl->k618_ultra)
		o_ctrl->k618_last_flush_req = 0;
}

/* stock CAM_STOP_DEV (0x15ed68-0x15ed80) */
static void k618_ultra_stop(struct cam_ois_ctrl_t *o_ctrl)
{
	if (!o_ctrl->k618_ultra)
		return;
	o_ctrl->k618_last_flush_req = 0;
	memset(o_ctrl->k618_skip_req, 0, sizeof(o_ctrl->k618_skip_req));
	memset(o_ctrl->k618_skip_cnt, 0, sizeof(o_ctrl->k618_skip_cnt));
}

/*
 * stock CAM_RELEASE_DEV (0x15eec0-0x15efb8): skip table and flush id cleared, the request slots
 * deleted. Stock deletes the slots still marked valid; here also a slot already applied whose
 * list apply_request had not cleared yet. Called from cam_ois_shutdown() as well.
 */
static void k618_ultra_release(struct cam_ois_ctrl_t *o_ctrl)
{
	uint32_t i;

	if (!o_ctrl->k618_ultra)
		return;
	o_ctrl->k618_last_flush_req = 0;
	memset(o_ctrl->k618_skip_req, 0, sizeof(o_ctrl->k618_skip_req));
	memset(o_ctrl->k618_skip_cnt, 0, sizeof(o_ctrl->k618_skip_cnt));
	for (i = 0; i < ARRAY_SIZE(o_ctrl->k618_req_slot); i++) {
		if ((o_ctrl->k618_req_slot[i].is_settings_valid == 1) ||
			!list_empty(&(o_ctrl->k618_req_slot[i].list_head)))
			delete_request(&(o_ctrl->k618_req_slot[i]));
		o_ctrl->k618_req_slot[i].request_id = 0;
	}
}

/*
 * The live device tree's root "model": "Qualcomm Technologies, Inc. 8550 nuwa" on the 13 Pro,
 * "... 8550 ishtar" in the 13 Ultra's dtbo overlay; compatible and qcom,board-id are the same.
 */
static bool k618_ois_is_ishtar(void)
{
	struct device_node *root = of_find_node_by_path("/");
	const char *model = NULL;
	bool ishtar = false;

	if (!root)
		return false;
	if (!of_property_read_string(root, "model", &model) && model)
		ishtar = strstr(model, "ishtar") != NULL;
	of_node_put(root);
	return ishtar;
}

/* from cam_ois_driver_soc_init(): stock cam_ois_component_bind 0x157970-0x157bdc */
void k618_ois_ultra_setup(struct cam_ois_ctrl_t *o_ctrl)
{
	uint32_t i;

	for (i = 0; i < ARRAY_SIZE(o_ctrl->k618_req_slot); i++)
		INIT_LIST_HEAD(&(o_ctrl->k618_req_slot[i].list_head));
	o_ctrl->k618_ultra = k618_ois_is_ishtar();
	if (!o_ctrl->k618_ultra)
		return;
	o_ctrl->bridge_intf.link_hdl = -1;
	o_ctrl->bridge_intf.ops.get_dev_info = k618_ois_publish_dev_info;
	o_ctrl->bridge_intf.ops.link_setup = k618_ois_establish_link;
	o_ctrl->bridge_intf.ops.apply_req = k618_ois_apply_request;
	CAM_INFO(CAM_OIS, "k618: OIS %u: 13 Ultra -- request-manager device, 65-byte slave info",
		o_ctrl->soc_info.index);
}
"""

EDITS += [
# --- camera 24: the 13 Ultra's OIS, after its own 5.15 camera.ko (ishtar OS3.0.308.0;
# see CAM_OIS_ULTRA). Its slave info is 65 bytes -- chip id at 0x3a, expected program id at 0x3c,
# OIS type at 0x40, where camera 16 (59 bytes) reads the type at 0x3a; the type picks BU24721
# (1-4, 7), BU24532 (5), RUMBA-S4H (6) or the generic path (0; chip id 0x4c1 = SEM1217S). The OIS
# is a request-manager device there ("cam-ois", dev id 10) with per-request packets. Chosen by the
# root "model" (k618_ois_is_ishtar); the 13 Pro runs camera 16's paths as before.
    # struct fields, after camera 16's last one
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_dev.h",
     "\tint k618_poll_go_on;\t/* k618: bring-up polls that may still time out and go on */\n",
     "\tint k618_poll_go_on;\t/* k618: bring-up polls that may still time out and go on */\n"
     "\t/* k618: camera 24 -- the 13 Ultra (k618_ultra_* in cam_ois_core.c) */\n"
     "\tbool     k618_ultra;\t\t/* root model names ishtar: request-manager ops, 65-byte slave info */\n"
     "\tbool     k618_ultra_info;\t/* this INIT's slave info had the 65 bytes */\n"
     "\tbool     k618_ultra_redl;\t/* packet 5 running (stock o_ctrl+0x11d8) */\n"
     "\tuint8_t  k618_ultra_type;\t/* slave info 0x40 (stock +0xe8a) */\n"
     "\tuint16_t k618_ultra_chip_id;\t/* slave info 0x3a (stock +0xe84) */\n"
     "\tuint32_t k618_ultra_expect_id;\t/* slave info 0x3c (stock +0xe86) */\n"
     "\tuint64_t k618_last_flush_req;\t/* stock +0xfc8 */\n"
     "\tstruct i2c_settings_array k618_req_slot[32];\t/* stock +0xf38: packet 4, by request id % 32 */\n"
     "\tuint64_t k618_skip_req[32];\t/* stock +0xfd8: packet 0x7e, by request id % 32 */\n"
     "\tuint64_t k618_skip_cnt[32];\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_dev.h",
     "void cam_ois_driver_exit(void);\n",
     "void cam_ois_driver_exit(void);\n"
     "\n"
     "/* k618: camera 24 -- cam_ois_core.c, called from cam_ois_driver_soc_init() */\n"
     "void k618_ois_ultra_setup(struct cam_ois_ctrl_t *o_ctrl);\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_soc.c",
     "\tINIT_LIST_HEAD(&(o_ctrl->k618_postinit_data.list_head));\t/* k618 */\n",
     "\tINIT_LIST_HEAD(&(o_ctrl->k618_postinit_data.list_head));\t/* k618 */\n"
     "\tk618_ois_ultra_setup(o_ctrl);\t/* k618: camera 24 */\n"),
    # the device id stock's OIS publishes (10, one past TPG); nothing indexes by it or by MAX in
    # 6.x, and it does not reach user space
    ("camera-kernel/drivers/cam_req_mgr/cam_req_mgr_interface.h",
     "\tCAM_REQ_MGR_DEVICE_TPG,\n"
     "\tCAM_REQ_MGR_DEVICE_ID_MAX,\n",
     "\tCAM_REQ_MGR_DEVICE_TPG,\n"
     "\tCAM_REQ_MGR_DEVICE_OIS,\t/* k618: camera 24 -- the 13 Ultra's OIS, 10 as on its 5.15 */\n"
     "\tCAM_REQ_MGR_DEVICE_ID_MAX,\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "#define CAM_OIS_FW_VERSION_CHECK_MASK 0x1\n",
     "#define CAM_OIS_FW_VERSION_CHECK_MASK 0x1\n" + CAM_OIS_ULTRA_DECL),
    # slave info: after camera 16's fields and its log line
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\t\to_ctrl->k618_fw_page_inc, o_ctrl->k618_ois_flag);\n",
     "\t\t\to_ctrl->k618_fw_page_inc, o_ctrl->k618_ois_flag);\n"
     "\t\t/* k618: camera 24 -- the 13 Ultra's 65-byte slave info */\n"
     "\t\trc = k618_ultra_slave_info(o_ctrl, (const uint8_t *)ois_info, len);\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\tswitch (csl_packet->header.op_code & 0xFFFFFF) {\n",
     "\t/* k618: camera 24 -- stock refuses packets of a flushed request (see k618_ultra_flush_check) */\n"
     "\tif (o_ctrl->k618_ultra) {\n"
     "\t\trc = k618_ultra_flush_check(o_ctrl, csl_packet);\n"
     "\t\tif (rc)\n"
     "\t\t\tgoto end;\n"
     "\t}\n"
     "\n"
     "\tswitch (csl_packet->header.op_code & 0xFFFFFF) {\n"),
    # INIT, after power-up: after camera 16's ROHM branch and its "ois_fw_flag" line
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\tCAM_DBG(CAM_OIS, \"ois_fw_flag: %d\", o_ctrl->ois_fw_flag);\n",
     "\t\tCAM_DBG(CAM_OIS, \"ois_fw_flag: %d\", o_ctrl->ois_fw_flag);\n"
     "\t\t/* k618: camera 24 -- the 13 Ultra's chip types, and its fwinit with ois_fw_flag clear */\n"
     "\t\tif (o_ctrl->k618_ultra_info) {\n"
     "\t\t\trc = k618_ultra_init(o_ctrl);\n"
     "\t\t\tif (rc < 0)\n"
     "\t\t\t\tgoto pwr_dwn;\n"
     "\t\t\tif (rc == K618_ULTRA_INIT_DONE) {\n"
     "\t\t\t\trc = 0;\n"
     "\t\t\t\tgoto k618_init_done;\n"
     "\t\t\t}\n"
     "\t\t}\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\t\t\t\trc = cam_ois_fw_download(o_ctrl);\n",
     "\t\t\t\t\trc = k618_ultra_fw_download(o_ctrl);\t/* k618: camera 24 (SEM1217S) */\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\trc = cam_sensor_i2c_read_data(\n"
     "\t\t\t&i2c_read_settings,\n"
     "\t\t\t&o_ctrl->io_master_info);\n",
     "\t\trc = k618_ultra_read_data(o_ctrl,\t/* k618: camera 24; elsewhere cam_sensor_i2c_read_data */\n"
     "\t\t\t&i2c_read_settings);\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\tdefault:\n"
     "\t\tCAM_ERR(CAM_OIS, \"Invalid Opcode: %d\",\n",
     "\tcase K618_OIS_OPCODE_APERTURE:\n"
     "\tcase K618_OIS_OPCODE_REDOWNLOAD:\n"
     "\tcase K618_OIS_OPCODE_PARKLENS:\n"
     "\tcase K618_OIS_OPCODE_SKIPFRAME:\n"
     "\tcase CAM_PKT_NOP_OPCODE:\n"
     "\t\t/* k618: camera 24 -- the 13 Ultra's packets; elsewhere as before */\n"
     "\t\tif (o_ctrl->k618_ultra) {\n"
     "\t\t\trc = k618_ultra_pkt(o_ctrl, csl_packet);\n"
     "\t\t\tgoto end;\n"
     "\t\t}\n"
     "\t\tCAM_ERR(CAM_OIS, \"Invalid Opcode: %d\",\n"
     "\t\t\t(csl_packet->header.op_code & 0xFFFFFF));\n"
     "\t\trc = -EINVAL;\n"
     "\t\tgoto end;\n"
     "\tdefault:\n"
     "\t\tCAM_ERR(CAM_OIS, \"Invalid Opcode: %d\",\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\to_ctrl->cam_ois_state = CAM_OIS_START;\n",
     "\t\tk618_ultra_start(o_ctrl);\t/* k618: camera 24 */\n"
     "\t\to_ctrl->cam_ois_state = CAM_OIS_START;\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\to_ctrl->cam_ois_state = CAM_OIS_CONFIG;\n"
     "\t\tbreak;\n"
     "\tcase CAM_FLUSH_REQ:\n",
     "\t\to_ctrl->cam_ois_state = CAM_OIS_CONFIG;\n"
     "\t\tk618_ultra_stop(o_ctrl);\t/* k618: camera 24 */\n"
     "\t\tbreak;\n"
     "\tcase CAM_FLUSH_REQ:\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\t\to_ctrl->bridge_intf.session_hdl = -1;\n"
     "\t\to_ctrl->cam_ois_state = CAM_OIS_INIT;\n",
     "\t\to_ctrl->bridge_intf.session_hdl = -1;\n"
     "\t\to_ctrl->cam_ois_state = CAM_OIS_INIT;\n"
     "\t\tk618_ultra_release(o_ctrl);\t/* k618: camera 24 */\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "\tif (o_ctrl->cam_ois_state == CAM_OIS_INIT)\n"
     "\t\treturn;\n",
     "\tif (o_ctrl->cam_ois_state == CAM_OIS_INIT)\n"
     "\t\treturn;\n"
     "\n"
     "\tk618_ultra_release(o_ctrl);\t/* k618: camera 24 */\n"),
    ("camera-kernel/drivers/cam_sensor_module/cam_ois/cam_ois_core.c",
     "#ifdef OPLUS_FEATURE_CAMERA_COMMON\n"
     "EXPORT_SYMBOL(cam_ois_driver_cmd);\n"
     "#endif\n",
     "#ifdef OPLUS_FEATURE_CAMERA_COMMON\n"
     "EXPORT_SYMBOL(cam_ois_driver_cmd);\n"
     "#endif\n" + CAM_OIS_ULTRA),
]


EDITS += [


    ("camera-kernel/drivers/cam_isp/cam_isp_context.h",
     "\tatomic_t                              unserved_rup;\n",
     "\tatomic_t                              unserved_rup;\n"
     "\tint                                   k618_early_rup;\t/* k618: camera 19 */\n"),
    ("camera-kernel/drivers/cam_isp/cam_isp_context.c",
     "\tif (ctx_isp->frame_id == 1) {\n"
     "\t\tCAM_DBG(CAM_ISP, \"Reg update in Substate[%s] for early PCR\",\n"
     "\t\t\t__cam_isp_ctx_substate_val_to_type(\n"
     "\t\t\tctx_isp->substate_activated));\n"
     "\n"
     "\t\tif (!list_empty(&ctx->active_req_list)) {\n"
     "\t\t\treq = list_first_entry(&ctx->active_req_list, struct cam_ctx_request,\n"
     "\t\t\t\tlist);\n"
     "\t\t\tif (req->request_id)\n"
     "\t\t\t\tctx_isp->reported_req_id = req->request_id;\n"
     "\n"
     "\t\t\t__cam_isp_ctx_send_sof_timestamp(ctx_isp, req->request_id,\n"
     "\t\t\t\tCAM_REQ_MGR_SOF_EVENT_SUCCESS);\n"
     "\t\t}\n"
     "\t} else {\n"
     "\t\tif (!atomic_read(&ctx_isp->last_applied_default))\n"
     "\t\t\tatomic_set(&ctx_isp->unserved_rup, 1);\n"
     "\t\tCAM_WARN_RATE_LIMIT(CAM_ISP,\n"
     "\t\t\t\"ctx:%u Unexpected regupdate in activated Substate[%s] for frame_id:%lld\",\n"
     "\t\t\t\"last_applied_default:%d, unserved_rup:%d\",\n"
     "\t\t\tctx_isp->base->ctx_id,\n",
     """	/* Device compatibility. */
	if (ctx_isp->frame_id == 0 &&
		ctx_isp->substate_activated == CAM_ISP_CTX_ACTIVATED_EPOCH) {
		ctx_isp->k618_early_rup = 1;
		CAM_INFO(CAM_ISP,
			\"k618: ctx:%u link: 0x%x reg update ahead of the first SOF, request reported at that SOF\",
			ctx->ctx_id, ctx->link_hdl);
		return 0;
	}

	if (ctx_isp->frame_id == 1) {
		CAM_DBG(CAM_ISP, \"Reg update in Substate[%s] for early PCR\",
			__cam_isp_ctx_substate_val_to_type(
			ctx_isp->substate_activated));

		if (ctx_isp->k618_early_rup == 2) {
			/* k618: camera 19 -- reported at the first SOF already */
			ctx_isp->k618_early_rup = 0;
		} else if (!list_empty(&ctx->active_req_list)) {
			req = list_first_entry(&ctx->active_req_list, struct cam_ctx_request,
				list);
			if (req->request_id)
				ctx_isp->reported_req_id = req->request_id;

			__cam_isp_ctx_send_sof_timestamp(ctx_isp, req->request_id,
				CAM_REQ_MGR_SOF_EVENT_SUCCESS);
		}
	} else {
		if (!atomic_read(&ctx_isp->last_applied_default))
			atomic_set(&ctx_isp->unserved_rup, 1);
		/* k618: one format string -- two made every argument print in the wrong place */
		CAM_WARN_RATE_LIMIT(CAM_ISP,
			\"ctx:%u Unexpected regupdate in activated Substate[%s] for frame_id:%lld last_applied_default:%d, unserved_rup:%d\",
			ctx_isp->base->ctx_id,
"""),
    ("camera-kernel/drivers/cam_isp/cam_isp_context.c",
     "\t\tCAM_INFO(CAM_ISP, \"Apply is in progress at the time of SOF, ctx: %u, link: 0x%x\",\n"
     "\t\t\tctx->ctx_id, ctx->link_hdl);\n"
     "\n"
     "\t__cam_isp_ctx_update_sof_ts_util(sof_event_data, ctx_isp);\n",
     "\t\tCAM_INFO(CAM_ISP, \"Apply is in progress at the time of SOF, ctx: %u, link: 0x%x\",\n"
     "\t\t\tctx->ctx_id, ctx->link_hdl);\n"
     "\n"
     "\t__cam_isp_ctx_update_sof_ts_util(sof_event_data, ctx_isp);\n"
     "\n"
     "\t/* k618: camera 19 -- the early PCR request's reg update came ahead of this SOF */\n"
     "\tif (ctx_isp->k618_early_rup == 1) {\n"
     "\t\tctx_isp->k618_early_rup = 2;\n"
     "\t\tif (ctx_isp->frame_id == 1 && !list_empty(&ctx->active_req_list)) {\n"
     "\t\t\treq = list_first_entry(&ctx->active_req_list,\n"
     "\t\t\t\tstruct cam_ctx_request, list);\n"
     "\t\t\tif (req->request_id)\n"
     "\t\t\t\tctx_isp->reported_req_id = req->request_id;\n"
     "\t\t\t__cam_isp_ctx_send_sof_timestamp(ctx_isp, req->request_id,\n"
     "\t\t\t\tCAM_REQ_MGR_SOF_EVENT_SUCCESS);\n"
     "\t\t}\n"
     "\t}\n"),
    ("camera-kernel/drivers/cam_isp/cam_isp_context.c",
     "\tctx_isp->last_applied_jiffies = 0;\n"
     "\tctx_isp->num_inits_post_flush = 0;\n"
     "\tctx_isp->standby_en = false;\n"
     "}\n",
     "\tctx_isp->last_applied_jiffies = 0;\n"
     "\tctx_isp->num_inits_post_flush = 0;\n"
     "\tctx_isp->standby_en = false;\n"
     "\tctx_isp->k618_early_rup = 0;\n"
     "}\n"),
]


EDITS += [


    ("camera-kernel/drivers/cam_smmu/cam_smmu_api.h",
     "struct region_buf_info {\n"
     "\tstruct dma_buf *buf;\n"
     "\tstruct dma_buf_attachment *attach;\n"
     "\tstruct sg_table *table;\n"
     "};\n",
     "struct region_buf_info {\n"
     "\tstruct dma_buf *buf;\n"
     "\tstruct dma_buf_attachment *attach;\n"
     "\tstruct sg_table *table;\n"
     "\tsize_t k618_mapped_len;\t/* k618: camera 23 -- what the reserve mapped */\n"
     "};\n"),
    ("camera-kernel/drivers/cam_smmu/cam_smmu_api.c",
     "\t*iova = (uint32_t)region_info->iova_start;\n"
     "\t/* Assign size mapped */\n"
     "\t*request_len = size;\n",
     "\t*iova = (uint32_t)region_info->iova_start;\n"
     "\t/* Assign size mapped */\n"
     "\t*request_len = size;\n"
     "\tbuf_info->k618_mapped_len = size;\t/* k618: camera 23 -- release unmaps this much */\n"),
    ("camera-kernel/drivers/cam_smmu/cam_smmu_api.c",
     "\tsize = iommu_unmap(cb_info->domain,\n"
     "\t\tregion_info->iova_start,\n"
     "\t\tregion_info->iova_len);\n"
     "\tif (size != region_info->iova_len) {\n"
     "\t\tCAM_ERR(CAM_SMMU, \"Failed: Unmapped = %zu, requested = %zu\",\n"
     "\t\t\tsize,\n"
     "\t\t\tregion_info->iova_len);\n"
     "\t}\n",
     "\t/*\n"
     "\t * k618: camera 23 -- unmap what the reserve mapped. The ICP reserves only what it carves\n"
     "\t * out of the region (about 2 MB of the 7 MB FW uncached region here; 5.15 reserved all of\n"
     "\t * it); unmapping the region's whole length ran into the unmapped rest and WARNed twice.\n"
     "\t */\n"
     "\tsize = iommu_unmap(cb_info->domain,\n"
     "\t\tregion_info->iova_start,\n"
     "\t\tbuf_info->k618_mapped_len);\n"
     "\tif (size != buf_info->k618_mapped_len) {\n"
     "\t\tCAM_ERR(CAM_SMMU, \"Failed: Unmapped = %zu, requested = %zu\",\n"
     "\t\t\tsize,\n"
     "\t\t\tbuf_info->k618_mapped_len);\n"
     "\t}\n"
     "\tbuf_info->k618_mapped_len = 0;\n"),
]

EDITS += [


    ("camera-kernel/drivers/cam_isp/cam_isp_context.c",
     "static int __cam_isp_ctx_reg_upd_in_epoch_bubble_state(\n"
     "\tstruct cam_isp_context *ctx_isp, void *evt_data)\n"
     "{\n"
     "\tstruct cam_context                      *ctx = ctx_isp->base;\n"
     "\tstruct cam_ctx_request                  *req;\n"
     "\tstruct cam_isp_hw_reg_update_event_data *rup_event_data =\n"
     "\t\t(struct cam_isp_hw_reg_update_event_data *)evt_data;\n"
     "\n"
     "\tif (rup_event_data && rup_event_data->camif_irq) {\n"
     "\t\tif (rup_event_data->timestamp != ctx_isp->sof_timestamp_val)\n",
     "/*\n"
     " * k618: camera 22 -- a reg update starts a frame only with the SOF timestamp it reports. The\n"
     " * CSID's reg update can come with 0 (no SOF stamp latched on its path, e.g. the first frame of the\n"
     " * main camera in macro mode); as \"!= the SOF's\" it was counted as another frame, which skipped the\n"
     " * report of the first request and counted frames twice. SOF IRQs count the frames.\n"
     " */\n"
     "static bool k618_rup_new_frame(struct cam_isp_context *ctx_isp, uint64_t rup_ts)\n"
     "{\n"
     "\tif (rup_ts)\n"
     "\t\treturn true;\n"
     "\tCAM_INFO_RATE_LIMIT(CAM_ISP,\n"
     "\t\t\"k618: ctx:%u link: 0x%x reg update without a SOF timestamp at frame %lld, not a new frame\",\n"
     "\t\tctx_isp->base->ctx_id, ctx_isp->base->link_hdl, ctx_isp->frame_id);\n"
     "\treturn false;\n"
     "}\n"
     "\n"
     "static int __cam_isp_ctx_reg_upd_in_epoch_bubble_state(\n"
     "\tstruct cam_isp_context *ctx_isp, void *evt_data)\n"
     "{\n"
     "\tstruct cam_context                      *ctx = ctx_isp->base;\n"
     "\tstruct cam_ctx_request                  *req;\n"
     "\tstruct cam_isp_hw_reg_update_event_data *rup_event_data =\n"
     "\t\t(struct cam_isp_hw_reg_update_event_data *)evt_data;\n"
     "\n"
     "\tif (rup_event_data && rup_event_data->camif_irq) {\n"
     "\t\tif (rup_event_data->timestamp != ctx_isp->sof_timestamp_val &&\n"
     "\t\t\tk618_rup_new_frame(ctx_isp, rup_event_data->timestamp))\t/* k618: camera 22 */\n"),
    ("camera-kernel/drivers/cam_isp/cam_isp_context.c",
     "\t\t\treq->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);\n"
     "\t}\n"
     "\n"
     "\tif (rup_event_data && rup_event_data->camif_irq) {\n"
     "\t\tif (rup_event_data->timestamp != ctx_isp->sof_timestamp_val)\n",
     "\t\t\treq->request_id, ctx_isp->active_req_cnt, ctx->ctx_id, ctx->link_hdl);\n"
     "\t}\n"
     "\n"
     "\tif (rup_event_data && rup_event_data->camif_irq) {\n"
     "\t\tif (rup_event_data->timestamp != ctx_isp->sof_timestamp_val &&\n"
     "\t\t\tk618_rup_new_frame(ctx_isp, rup_event_data->timestamp))\t/* k618: camera 22 */\n"),
]

EDITS += [


    ("camera-kernel/drivers/cam_isp/isp_hw_mgr/isp_hw/vfe_hw/vfe_bus/cam_vfe_bus_ver3.c",
     "\tuint32_t                         secure_mode;\n"
     "\tvoid                            *priv;\n",
     "\tuint32_t                         secure_mode;\n"
     "\tbool                             k618_secure_caps;\t/* k618: camera 21, as counted at acquire */\n"
     "\tvoid                            *priv;\n"),
    ("camera-kernel/drivers/cam_isp/isp_hw_mgr/isp_hw/vfe_hw/vfe_bus/cam_vfe_bus_ver3.c",
     "\tsecure_caps = cam_vfe_bus_ver3_can_be_secure(\n"
     "\t\trsrc_data->out_type);\n"
     "\tmode = out_acquire_args->out_port_info->secure_mode;\n",
     "\tsecure_caps = cam_vfe_bus_ver3_can_be_secure(\n"
     "\t\trsrc_data->out_type);\n"
     "\trsrc_data->k618_secure_caps = secure_caps;\t/* k618: camera 21 -- release goes by this */\n"
     "\tmode = out_acquire_args->out_port_info->secure_mode;\n"),
    ("camera-kernel/drivers/cam_isp/isp_hw_mgr/isp_hw/vfe_hw/vfe_bus/cam_vfe_bus_ver3.c",
     "\tsecure_caps = cam_vfe_bus_ver3_can_be_secure(rsrc_data->out_type);\n"
     "\tmutex_lock(&rsrc_data->common_data->bus_mutex);\n",
     "\t/* k618: camera 21 -- counted down only if it was counted up (not by the switch as it is now) */\n"
     "\tsecure_caps = rsrc_data->k618_secure_caps;\n"
     "\trsrc_data->k618_secure_caps = false;\n"
     "\tmutex_lock(&rsrc_data->common_data->bus_mutex);\n"),
    ("camera-kernel/drivers/cam_isp/isp_hw_mgr/isp_hw/vfe_hw/vfe_bus/cam_vfe_bus_ver3.c",
     "\t\t\t\t\t\"VFE:%u Mismatch: Acquire mode[%d], drvr mode[%d]\",\n"
     "\t\t\t\t\tver3_bus_priv->common_data.core_index,\n"
     "\t\t\t\t\trsrc_data->common_data->secure_mode,\n"
     "\t\t\t\t\tmode);\n",
     "\t\t\t\t\t\"VFE:%u Mismatch: Acquire mode[%d], drvr mode[%d] (k618: out_type %u, secure outs %u)\",\n"
     "\t\t\t\t\tver3_bus_priv->common_data.core_index,\n"
     "\t\t\t\t\trsrc_data->common_data->secure_mode,\n"
     "\t\t\t\t\tmode, rsrc_data->out_type,\n"
     "\t\t\t\t\trsrc_data->common_data->num_sec_out);\n"),


    ("camera-kernel/drivers/cam_utils/cam_compat.c",
     "static bool k618_fd_secure;\n",
     "static bool k618_fd_secure = true;\n"),
]

EDITS += [


    ("display-drivers/msm/msm_smmu.c",
     "\t\tif (of_compat_cmp(compat, curr->compat, strlen(compat)) == 0 &&\n"
     "\t\t\t\tcurr->host_dev == dev) {\n",
     '\t\t/* Device compatibility. */\n\t\tif (of_compat_cmp(compat, curr->compat, strlen(compat)) == 0 &&\n\t\t\t\t(curr->host_dev == dev || !curr->host_dev)) {\n'),


    ("display-drivers/msm/dsi/dsi_drm.c",
     "\tbridge->display = display;\n\tbridge->base.funcs = &dsi_bridge_ops;\n\tbridge->base.encoder = encoder;\n",
     "\tbridge->display = display;\n\tbridge->base.funcs = &dsi_bridge_ops;\n\tbridge->base.encoder = encoder;\n"
     "\tkref_init(&bridge->base.refcount);\t/* k618: display 2, the driver's own reference */\n"),
    ("display-drivers/msm/dp/dp_drm.c",
     "\tbridge->display = display;\n\tbridge->base.funcs = &dp_bridge_ops;\n\tbridge->base.encoder = encoder;\n",
     "\tbridge->display = display;\n\tbridge->base.funcs = &dp_bridge_ops;\n\tbridge->base.encoder = encoder;\n"
     "\tkref_init(&bridge->base.refcount);\t/* k618: display 2, the driver's own reference */\n"),
    ("display-drivers/msm/dp/dp_mst_drm.c",
     "\tbridge->display = display;\n\tbridge->base.funcs = &dp_mst_bridge_ops;\n\tbridge->base.encoder = encoder;\n",
     "\tbridge->display = display;\n\tbridge->base.funcs = &dp_mst_bridge_ops;\n\tbridge->base.encoder = encoder;\n"
     "\tkref_init(&bridge->base.refcount);\t/* k618: display 2, the driver's own reference */\n"),


    ("camera-kernel/drivers/cam_smmu/cam_smmu_api.c",
     "\tCAM_DBG(CAM_SMMU, \"idx=%d, ion_fd=%d, i_ino=%lu, dev=%pOFfp, paddr=0x%llx, len=%zu\",\n"
     "\t\tidx, ion_fd, mapping_info->i_ino,\n"
     "\t\tiommu_cb_set.cb_info[idx].dev->of_node,\n"
     "\t\t*paddr_ptr, *len_ptr);\n",
     "\t/* k618: camera 20 -- every protected buffer, as the hardware gets it */\n"
     "\tCAM_INFO(CAM_SMMU,\n"
     "\t\t\"k618: protected buffer: cb %s, fd %d, inode %lu, dma-buf %zu bytes, addr 0x%llx, len %zu, dir %d\",\n"
     "\t\tiommu_cb_set.cb_info[idx].name[0], ion_fd, mapping_info->i_ino,\n"
     "\t\t(size_t)dmabuf->size, *paddr_ptr, *len_ptr, dma_dir);\n"),
    ("camera-kernel/drivers/cam_isp/isp_hw_mgr/isp_hw/vfe_hw/vfe_bus/cam_vfe_bus_ver3.c",
     "\t\trsrc_data->common_data->num_sec_out++;\n"
     "\t}\n"
     "\tmutex_unlock(&rsrc_data->common_data->bus_mutex);\n",
     "\t\trsrc_data->common_data->num_sec_out++;\n"
     "\t}\n"
     "\tmutex_unlock(&rsrc_data->common_data->bus_mutex);\n"
     "\n"
     "\t/* k618: camera 20 -- which output ports of a secure session are secure */\n"
     "\tif (secure_caps || mode)\n"
     "\t\tCAM_INFO(CAM_ISP,\n"
     "\t\t\t\"k618: VFE:%u out_type %u (res 0x%x): asked mode %u, port mode %u, can be secure %u, secure outs %u\",\n"
     "\t\t\tver3_bus_priv->common_data.core_index, rsrc_data->out_type,\n"
     "\t\t\tout_acquire_args->out_port_info->res_type, mode, rsrc_data->secure_mode,\n"
     "\t\t\tsecure_caps, rsrc_data->common_data->num_sec_out);\n"),
]

EDITS += [


    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_peer.h",
     "/**\n"
     " * dp_rx_peer_map_handler() - handle peer map event from firmware\n",
     "/*\n"
     " * k618: Wi-Fi 1 -- an LFR3 roam makes the new peer in the firmware, and its HTT peer map can come\n"
     " * before the host's roam sync has made that peer; see dp_k618_map_stash() in dp_peer.c\n"
     " */\n"
     "void dp_k618_map_replay(struct dp_soc *soc, uint8_t vdev_id, uint8_t *peer_mac);\n"
     "void dp_k618_map_forget(struct dp_soc *soc, uint8_t vdev_id);\n"
     "void dp_k618_addba_stash(struct dp_soc *soc, uint16_t peer_id, uint8_t tid,\n"
     "\t\t\t\t\t\t uint16_t win_sz);\n"
     "void dp_k618_addba_flush(struct dp_soc *soc, uint16_t peer_id, bool apply);\n"
     "\n"
     "/**\n"
     " * dp_rx_peer_map_handler() - handle peer map event from firmware\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_peer.c",
     "QDF_STATUS\n"
     "dp_rx_peer_map_handler(struct dp_soc *soc, uint16_t peer_id,\n",
     """/* Device compatibility. */
#include <linux/bottom_half.h>
#include <linux/spinlock.h>

#define K618_EARLY_KEEP 8
#define K618_ALL_VDEVS  0xff

struct k618_early_map {
	struct dp_soc *soc;
	uint32_t seq;
	uint16_t peer_id;
	uint16_t hw_peer_id;
	uint16_t ast_hash;
	uint8_t vdev_id;
	uint8_t mac[QDF_MAC_ADDR_SIZE];
};

static struct k618_early_map k618_early_maps[K618_EARLY_KEEP];
static uint32_t k618_early_seq;
static DEFINE_SPINLOCK(k618_early_map_lock);

/* the entry for this id or this peer, else a free one, else the oldest (*evicted set) */
static struct k618_early_map *k618_early_slot(struct dp_soc *soc,
											  uint16_t peer_id,
											  uint8_t vdev_id, uint8_t *mac,
											  bool *evicted)
{
	struct k618_early_map *e, *free = NULL, *oldest = NULL;
	int i;

	for (i = 0; i < K618_EARLY_KEEP; i++) {
		e = &k618_early_maps[i];
		if (!e->soc) {
			if (!free)
				free = e;
			continue;
		}
		if (e->soc == soc &&
			(e->peer_id == peer_id ||
			 (e->vdev_id == vdev_id &&
			  !qdf_mem_cmp(e->mac, mac, QDF_MAC_ADDR_SIZE))))
			return e;
		if (!oldest || (int32_t)(e->seq - oldest->seq) < 0)
			oldest = e;
	}
	if (free)
		return free;
	*evicted = true;
	return oldest;
}

static void dp_k618_map_stash(struct dp_soc *soc, uint16_t peer_id,
							  uint16_t hw_peer_id, uint8_t vdev_id,
							  uint8_t *mac, uint16_t ast_hash)
{
	struct k618_early_map *slot;
	bool evicted = false;

	spin_lock_bh(&k618_early_map_lock);
	slot = k618_early_slot(soc, peer_id, vdev_id, mac, &evicted);
	slot->soc = soc;
	slot->seq = ++k618_early_seq;
	slot->peer_id = peer_id;
	slot->hw_peer_id = hw_peer_id;
	slot->ast_hash = ast_hash;
	slot->vdev_id = vdev_id;
	qdf_mem_copy(slot->mac, mac, QDF_MAC_ADDR_SIZE);
	spin_unlock_bh(&k618_early_map_lock);

	dp_err(\"k618: peer map of peer_id %u (\" QDF_MAC_ADDR_FMT \", vdev %u) found no peer to take it, kept%s\",
		   peer_id, QDF_MAC_ADDR_REF(mac), vdev_id,
		   evicted ? \" (table full, the oldest dropped)\" : \"\");
}

static void dp_k618_map_drop(struct dp_soc *soc, uint16_t peer_id)
{
	int i;

	spin_lock_bh(&k618_early_map_lock);
	for (i = 0; i < K618_EARLY_KEEP; i++)
		if (k618_early_maps[i].soc == soc &&
			k618_early_maps[i].peer_id == peer_id)
			k618_early_maps[i].soc = NULL;
	spin_unlock_bh(&k618_early_map_lock);
	dp_k618_addba_flush(soc, peer_id, false);
}

/* the vdev (or, K618_ALL_VDEVS, the whole DP soc) goes away: its kept maps with it */
void dp_k618_map_forget(struct dp_soc *soc, uint8_t vdev_id)
{
	int i, n = 0;

	spin_lock_bh(&k618_early_map_lock);
	for (i = 0; i < K618_EARLY_KEEP; i++) {
		struct k618_early_map *e = &k618_early_maps[i];

		if (e->soc == soc &&
			(vdev_id == K618_ALL_VDEVS || e->vdev_id == vdev_id)) {
			e->soc = NULL;
			n++;
		}
	}
	spin_unlock_bh(&k618_early_map_lock);
	if (vdev_id == K618_ALL_VDEVS)
		dp_k618_addba_flush(soc, HTT_INVALID_PEER, false);
	if (n)
		dp_err(\"k618: %d kept peer map(s) dropped with vdev %u\", n, vdev_id);
}

void dp_k618_map_replay(struct dp_soc *soc, uint8_t vdev_id, uint8_t *peer_mac)
{
	struct k618_early_map m = { 0 };
	bool have = false, busy;
	int i;

	spin_lock_bh(&k618_early_map_lock);
	for (i = 0; i < K618_EARLY_KEEP; i++) {
		struct k618_early_map *e = &k618_early_maps[i];

		if (e->soc == soc && e->vdev_id == vdev_id &&
			!qdf_mem_cmp(e->mac, peer_mac, QDF_MAC_ADDR_SIZE)) {
			m = *e;
			e->soc = NULL;
			have = true;
			break;
		}
	}
	spin_unlock_bh(&k618_early_map_lock);
	if (!have)
		return;

	qdf_spin_lock_bh(&soc->peer_map_lock);
	busy = m.peer_id >= soc->max_peer_id ||
		   soc->peer_id_to_obj_map[m.peer_id];
	qdf_spin_unlock_bh(&soc->peer_map_lock);
	if (busy) {
		dp_err(\"k618: peer_id %u (\" QDF_MAC_ADDR_FMT \") is taken, early map not replayed\",
			   m.peer_id, QDF_MAC_ADDR_REF(peer_mac));
		return;
	}

	dp_err(\"k618: replaying the early peer map: peer_id %u (\" QDF_MAC_ADDR_FMT \", vdev %u)\",
		   m.peer_id, QDF_MAC_ADDR_REF(peer_mac), vdev_id);
	local_bh_disable();
	dp_rx_peer_map_handler(soc, m.peer_id, m.hw_peer_id, m.vdev_id,
						   m.mac, m.ast_hash, 0);
	local_bh_enable();
	dp_k618_addba_flush(soc, m.peer_id, true);
}

QDF_STATUS
dp_rx_peer_map_handler(struct dp_soc *soc, uint16_t peer_id,
"""),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_peer.c",
     "\t\tpeer = dp_peer_find_add_id(soc, peer_mac_addr, peer_id,\n"
     "\t\t\t\t\t   hw_peer_id, vdev_id,\n"
     "\t\t\t\t\t   CDP_LINK_PEER_TYPE);\n"
     "\n"
     "\t\tif (peer) {\n",
     "\t\tpeer = dp_peer_find_add_id(soc, peer_mac_addr, peer_id,\n"
     "\t\t\t\t\t   hw_peer_id, vdev_id,\n"
     "\t\t\t\t\t   CDP_LINK_PEER_TYPE);\n"
     "\n"
     "\t\t/* k618: Wi-Fi 1 -- every map is logged; one no peer took is kept */\n"
     "\t\tdp_err(\"k618: peer map peer_id %u hw %u vdev %u \" QDF_MAC_ADDR_FMT \": %s\",\n"
     "\t\t\t   peer_id, hw_peer_id, vdev_id,\n"
     "\t\t\t   QDF_MAC_ADDR_REF(peer_mac_addr),\n"
     "\t\t\t   peer ? \"mapped\" : \"no peer took it\");\n"
     "\t\tif (!peer)\n"
     "\t\t\tdp_k618_map_stash(soc, peer_id, hw_peer_id, vdev_id,\n"
     "\t\t\t\t\t\t\t  peer_mac_addr, ast_hash);\n"
     "\n"
     "\t\tif (peer) {\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_peer.c",
     "\t/* Peer map event for WDS ast entry get the peer from\n"
     "\t * obj map\n"
     "\t */\n"
     "\tif (is_wds) {\n",
     "\t/* Peer map event for WDS ast entry get the peer from\n"
     "\t * obj map\n"
     "\t */\n"
     "\tif (is_wds) {\n"
     "\t\tdp_err(\"k618: peer map peer_id %u hw %u vdev %u \" QDF_MAC_ADDR_FMT \": as a WDS entry\",\n"
     "\t\t\t   peer_id, hw_peer_id, vdev_id,\n"
     "\t\t\t   QDF_MAC_ADDR_REF(peer_mac_addr));\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_peer.c",
     "\t\t} else {\n"
     "\t\t\tdp_peer_unref_delete(peer, DP_MOD_ID_CONFIG);\n"
     "\t\t\tQDF_ASSERT(0);\n"
     "\t\t\treturn NULL;\n"
     "\t\t}\n"
     "\t\tdp_peer_find_id_to_obj_add(soc, peer, peer_id);\n",
     "\t\t} else {\n"
     "\t\t\tdp_err(\"k618: peer \" QDF_MAC_ADDR_FMT \" still holds peer_id %u, map of %u not taken\",\n"
     "\t\t\t\t   QDF_MAC_ADDR_REF(peer_mac_addr), peer->peer_id,\n"
     "\t\t\t\t   peer_id);\n"
     "\t\t\tdp_peer_unref_delete(peer, DP_MOD_ID_CONFIG);\n"
     "\t\t\tQDF_ASSERT(0);\n"
     "\t\t\treturn NULL;\n"
     "\t\t}\n"
     "\t\tdp_peer_find_id_to_obj_add(soc, peer, peer_id);\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_peer.c",
     "\tDP_STATS_INC(soc, t2h_msg_stats.peer_unmap, 1);\n",
     "\tDP_STATS_INC(soc, t2h_msg_stats.peer_unmap, 1);\n"
     "\t/* k618: Wi-Fi 1 -- every unmap is logged; a kept early map of this id is void */\n"
     "\tdp_err(\"k618: peer unmap peer_id %u vdev %u is_wds %u\", peer_id,\n"
     "\t\t   vdev_id, is_wds);\n"
     "\tdp_k618_map_drop(soc, peer_id);\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_main.c",
     "\t\t\tdp_peer_update_state(soc, peer, DP_PEER_STATE_INIT);\n"
     "\n"
     "\t\tdp_vdev_unref_delete(soc, vdev, DP_MOD_ID_CDP);\n"
     "\t\treturn QDF_STATUS_SUCCESS;\n",
     "\t\t\tdp_peer_update_state(soc, peer, DP_PEER_STATE_INIT);\n"
     "\n"
     "\t\tdp_vdev_unref_delete(soc, vdev, DP_MOD_ID_CDP);\n"
     "\t\t/* k618: Wi-Fi 1 -- a peer map that came before this peer, see dp_k618_map_stash() */\n"
     "\t\tdp_err(\"k618: peer create \" QDF_MAC_ADDR_FMT \" vdev %u: reused, peer_id %u\",\n"
     "\t\t\t   QDF_MAC_ADDR_REF(peer_mac_addr), vdev_id, peer->peer_id);\n"
     "\t\tdp_k618_map_replay(soc, vdev_id, peer_mac_addr);\n"
     "\t\treturn QDF_STATUS_SUCCESS;\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_main.c",
     "\tdp_peer_update_state(soc, peer, DP_PEER_STATE_INIT);\n"
     "\n"
     "\tdp_vdev_unref_delete(soc, vdev, DP_MOD_ID_CDP);\n"
     "\n"
     "\treturn QDF_STATUS_SUCCESS;\n"
     "fail:\n",
     "\tdp_peer_update_state(soc, peer, DP_PEER_STATE_INIT);\n"
     "\n"
     "\tdp_vdev_unref_delete(soc, vdev, DP_MOD_ID_CDP);\n"
     "\n"
     "\t/* k618: Wi-Fi 1 -- a peer map that came before this peer, see dp_k618_map_stash() */\n"
     "\tdp_err(\"k618: peer create \" QDF_MAC_ADDR_FMT \" vdev %u: new\",\n"
     "\t\t   QDF_MAC_ADDR_REF(peer_mac_addr), vdev_id);\n"
     "\tdp_k618_map_replay(soc, vdev_id, peer_mac_addr);\n"
     "\n"
     "\treturn QDF_STATUS_SUCCESS;\n"
     "fail:\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_main.c",
     "\tpeer->valid = 0;\n"
     "\n"
     "\tdp_rx_fst_inv_peer_id(soc, peer);\n",
     "\tpeer->valid = 0;\n"
     "\tdp_err(\"k618: peer delete \" QDF_MAC_ADDR_FMT \" vdev %u peer_id %u state %u\",\n"
     "\t\t   QDF_MAC_ADDR_REF(peer->mac_addr.raw), vdev_id, peer->peer_id,\n"
     "\t\t   peer->peer_state);\t/* k618: Wi-Fi 1 */\n"
     "\n"
     "\tdp_rx_fst_inv_peer_id(soc, peer);\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_main.c",
     "\tif (!vdev)\n"
     "\t\treturn QDF_STATUS_E_FAILURE;\n"
     "\n"
     "\tsoc->arch_ops.txrx_vdev_detach(soc, vdev);\n",
     "\tif (!vdev)\n"
     "\t\treturn QDF_STATUS_E_FAILURE;\n"
     "\n"
     "\tdp_k618_map_forget(soc, vdev_id);\t/* k618: Wi-Fi 1 */\n"
     "\tsoc->arch_ops.txrx_vdev_detach(soc, vdev);\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_main.c",
     "\tstruct dp_soc *soc = (struct dp_soc *)txrx_soc;\n"
     "\n"
     "\tsoc->arch_ops.txrx_soc_deinit(soc);\n"
     "}\n",
     "\tstruct dp_soc *soc = (struct dp_soc *)txrx_soc;\n"
     "\n"
     "\tdp_k618_map_forget(soc, 0xff);\t/* k618: Wi-Fi 1 -- every vdev */\n"
     "\tsoc->arch_ops.txrx_soc_deinit(soc);\n"
     "}\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_htt.c",
     "/**\n"
     " * dp_htt_rx_addba_handler() - RX Addba HTT msg handler\n",
     "/*\n"
     " * k618: Wi-Fi 1. An RX ADDBA of the firmware's new peer can come with its early peer map (see\n"
     " * dp_k618_map_stash() in dp_peer.c); it is kept until that map is replayed (then replayed itself)\n"
     " * or the id is unmapped or the DP soc goes away.\n"
     " */\n"
     "#include <linux/bottom_half.h>\n"
     "#include <linux/spinlock.h>\n"
     "\n"
     "#define K618_EARLY_ADDBA_KEEP 16\n"
     "\n"
     "struct k618_early_addba {\n"
     "\tstruct dp_soc *soc;\n"
     "\tuint32_t seq;\n"
     "\tuint16_t peer_id;\n"
     "\tuint16_t win_sz;\n"
     "\tuint8_t tid;\n"
     "};\n"
     "\n"
     "static struct k618_early_addba k618_early_addbas[K618_EARLY_ADDBA_KEEP];\n"
     "static uint32_t k618_early_addba_seq;\n"
     "static DEFINE_SPINLOCK(k618_early_addba_lock);\n"
     "\n"
     "void dp_k618_addba_stash(struct dp_soc *soc, uint16_t peer_id, uint8_t tid,\n"
     "\t\t\t\t\t\t uint16_t win_sz)\n"
     "{\n"
     "\tstruct k618_early_addba *e, *slot = NULL, *free = NULL, *oldest = NULL;\n"
     "\tint i;\n"
     "\n"
     "\tspin_lock_bh(&k618_early_addba_lock);\n"
     "\tfor (i = 0; i < K618_EARLY_ADDBA_KEEP; i++) {\n"
     "\t\te = &k618_early_addbas[i];\n"
     "\t\tif (!e->soc) {\n"
     "\t\t\tif (!free)\n"
     "\t\t\t\tfree = e;\n"
     "\t\t\tcontinue;\n"
     "\t\t}\n"
     "\t\tif (e->soc == soc && e->peer_id == peer_id && e->tid == tid) {\n"
     "\t\t\tslot = e;\n"
     "\t\t\tbreak;\n"
     "\t\t}\n"
     "\t\tif (!oldest || (int32_t)(e->seq - oldest->seq) < 0)\n"
     "\t\t\toldest = e;\n"
     "\t}\n"
     "\tif (!slot)\n"
     "\t\tslot = free ? free : oldest;\n"
     "\tslot->soc = soc;\n"
     "\tslot->seq = ++k618_early_addba_seq;\n"
     "\tslot->peer_id = peer_id;\n"
     "\tslot->tid = tid;\n"
     "\tslot->win_sz = win_sz;\n"
     "\tspin_unlock_bh(&k618_early_addba_lock);\n"
     "\n"
     "\tdp_err(\"k618: RX ADDBA of peer_id %u tid %u win %u before its peer map, kept\",\n"
     "\t\t   peer_id, tid, win_sz);\n"
     "}\n"
     "\n"
     "/**\n"
     " * dp_htt_rx_addba_handler() - RX Addba HTT msg handler\n"),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_htt.c",
     "\tif (!peer) {\n"
     "\t\tdp_err(\"Peer not found peer id %d\", peer_id);\n"
     "\t\treturn;\n"
     "\t}\n"
     "\n"
     "\tstatus = dp_addba_requestprocess_wifi3(",
     "\tif (!peer) {\n"
     "\t\tdp_err(\"Peer not found peer id %d\", peer_id);\n"
     "\t\tdp_k618_addba_stash(soc, peer_id, tid, win_sz);\t/* k618: Wi-Fi 1 */\n"
     "\t\treturn;\n"
     "\t}\n"
     "\n"
     "\tstatus = dp_addba_requestprocess_wifi3("),
    ("wlan/qca-wifi-host-cmn/dp/wifi3.0/dp_htt.c",
     "\tdp_info(\"PeerID %d BAW %d TID %d stat %d\",\n"
     "\t\tpeer_id, win_sz, tid, status);\n"
     "}\n",
     "\tdp_info(\"PeerID %d BAW %d TID %d stat %d\",\n"
     "\t\tpeer_id, win_sz, tid, status);\n"
     "}\n"
     "\n"
     "/*\n"
     " * k618: Wi-Fi 1 -- the kept RX ADDBAs of peer_id (HTT_INVALID_PEER: all of this soc's): replayed\n"
     " * (apply, after the id's early map was) or dropped\n"
     " */\n"
     "void dp_k618_addba_flush(struct dp_soc *soc, uint16_t peer_id, bool apply)\n"
     "{\n"
     "\tstruct k618_early_addba todo[K618_EARLY_ADDBA_KEEP];\n"
     "\tint i, n = 0;\n"
     "\n"
     "\tspin_lock_bh(&k618_early_addba_lock);\n"
     "\tfor (i = 0; i < K618_EARLY_ADDBA_KEEP; i++) {\n"
     "\t\tstruct k618_early_addba *e = &k618_early_addbas[i];\n"
     "\n"
     "\t\tif (e->soc != soc ||\n"
     "\t\t\t(peer_id != HTT_INVALID_PEER && e->peer_id != peer_id))\n"
     "\t\t\tcontinue;\n"
     "\t\tif (apply)\n"
     "\t\t\ttodo[n++] = *e;\n"
     "\t\te->soc = NULL;\n"
     "\t}\n"
     "\tspin_unlock_bh(&k618_early_addba_lock);\n"
     "\n"
     "\tfor (i = 0; i < n; i++) {\n"
     "\t\tdp_err(\"k618: replaying the early RX ADDBA: peer_id %u tid %u win %u\",\n"
     "\t\t\t   todo[i].peer_id, todo[i].tid, todo[i].win_sz);\n"
     "\t\tlocal_bh_disable();\n"
     "\t\tdp_htt_rx_addba_handler(soc, todo[i].peer_id, todo[i].tid,\n"
     "\t\t\t\t\t\t\t\ttodo[i].win_sz);\n"
     "\t\tlocal_bh_enable();\n"
     "\t}\n"
     "}\n"),
    ("wlan/qca-wifi-host-cmn/wmi/src/wmi_unified.c",
     "\t} else if (exec_ctx == WMI_RX_SERIALIZER_CTX) {\n"
     "\t\twmi_process_fw_event_sched_thread_ctx(wmi_handle, evt_buf);\n",
     "\t} else if (exec_ctx == WMI_RX_SERIALIZER_CTX) {\n"
     "\t\t/* k618: Wi-Fi 1 -- when the roam sync reaches the host, for the early peer maps */\n"
     "\t\tif (id == wmi_handle->wmi_events[wmi_roam_synch_event_id])\n"
     "\t\t\twmi_err(\"k618: WMI roam sync event in, to the scheduler thread\");\n"
     "\t\twmi_process_fw_event_sched_thread_ctx(wmi_handle, evt_buf);\n"),
    ("wlan/qcacld-3.0/components/target_if/connection_mgr/src/target_if_cm_roam_event.c",
     "\ttarget_if_prevent_pm_during_roam_sync(psoc);\n"
     "\n"
     "\tqdf_status = wmi_extract_roam_sync_event(wmi_handle, event,\n",
     "\ttarget_if_prevent_pm_during_roam_sync(psoc);\n"
     "\ttarget_if_err(\"k618: roam sync event: the scheduler thread starts on it\");\t/* k618: Wi-Fi 1 */\n"
     "\n"
     "\tqdf_status = wmi_extract_roam_sync_event(wmi_handle, event,\n"),
]


SYNX_V1 = r"""/*
 * k618: synx's v1 user interface, which this ROM's libsynx (vendor lib64/libsynx.so, built for the
 * 5.15 vendor) speaks. 5.15's synx made the session in open ("umd-client-<pid>") and took commands
 * 0-11 with the v1 structures: signed handles, synx_info for CREATE, 8-byte SIGNAL and GETSTATUS,
 * 16-byte MERGE, the v1 external descriptor in BIND, ADDREFCOUNT, synx_id_info for IMPORT and
 * EXPORT; a successful WAIT said 0. 6.12 kept the ids, changed most structures, wants
 * SYNX_INITIALIZE first and has no ADDREFCOUNT or EXPORT. Each command below makes the 6.12 call
 * 5.15's handler made; RELEASE and DEREGISTER_PAYLOAD have the same structure and meaning in both
 * and use 6.12's handlers. A client that sends SYNX_INITIALIZE is a v2 one: synx_ioctl drops the
 * session open made, and the client gets the one it asks for.
 */
#define K618_SYNX_V1_MAX_REF_COUNTS	2048	/* 5.15's SYNX_MAX_REF_COUNTS */
#define K618_SYNX_V1_FLAG_GLOBAL_FENCE	0x1	/* 5.15's SYNX_FLAG_GLOBAL_FENCE (EXPORT's padding) */

static bool k618_synx_v1(struct synx_session *session)
{
	struct synx_client *client = synx_get_client(session);
	bool v1;

	if (IS_ERR_OR_NULL(client))
		return false;
	v1 = client->k618_v1;
	synx_put_client(client);
	return v1;
}

static struct synx_session *k618_synx_v1_session(void)
{
	struct synx_initialization_params params = {0};
	char name[SYNX_OBJ_NAME_LEN];
	struct synx_session *session;
	struct synx_client *client;

	scnprintf(name, sizeof(name), "umd-client-%d", current->pid);
	params.name = name;
	params.id = SYNX_CLIENT_NATIVE;
	session = synx_initialize(&params);
	if (IS_ERR_OR_NULL(session))
		return NULL;
	client = synx_get_client(session);
	if (IS_ERR_OR_NULL(client)) {
		synx_uninitialize(session);
		return NULL;
	}
	client->k618_v1 = true;
	synx_put_client(client);
	return session;
}

static int k618_synx_v1_get(struct synx_private_ioctl_arg *k_ioctl, void *data, u32 size)
{
	if (k_ioctl->size != size)
		return -EINVAL;
	if (copy_from_user(data, u64_to_user_ptr(k_ioctl->ioctl_ptr), size))
		return -EFAULT;
	return 0;
}

static int k618_synx_v1_put(struct synx_private_ioctl_arg *k_ioctl, void *data, u32 size)
{
	if (copy_to_user(u64_to_user_ptr(k_ioctl->ioctl_ptr), data, size))
		return -EFAULT;
	return 0;
}

static int k618_synx_v1_create(struct synx_private_ioctl_arg *k_ioctl,
	struct synx_session *session)
{
	struct synx_info info;
	struct synx_create_params params = {0};
	u32 h_synx = 0;
	int rc = k618_synx_v1_get(k_ioctl, &info, sizeof(info));

	if (rc)
		return rc;
	info.name[sizeof(info.name) - 1] = '\0';
	params.name = info.name;
	params.h_synx = &h_synx;
	params.flags = SYNX_CREATE_LOCAL_FENCE;	/* 5.15: type 0, a local object */
	rc = synx_create(session, &params);
	if (rc)
		return rc;
	info.synx_obj = h_synx;
	return k618_synx_v1_put(k_ioctl, &info, sizeof(info));
}

static int k618_synx_v1_signal(struct synx_private_ioctl_arg *k_ioctl,
	struct synx_session *session)
{
	struct synx_signal info;
	int rc = k618_synx_v1_get(k_ioctl, &info, sizeof(info));

	return rc ? rc : synx_signal(session, info.synx_obj, info.synx_state);
}

/* 5.15 read the state off the fence (a merged object: its group's) and gave 0 for a bad handle */
static int k618_synx_v1_getstatus(struct synx_private_ioctl_arg *k_ioctl,
	struct synx_session *session)
{
	struct synx_signal info;
	struct synx_client *client;
	struct synx_handle_coredata *synx_data;
	struct synx_coredata *synx_obj;
	int rc = k618_synx_v1_get(k_ioctl, &info, sizeof(info));

	if (rc)
		return rc;
	client = synx_get_client(session);
	if (IS_ERR_OR_NULL(client))
		return -EINVAL;
	info.synx_state = SYNX_STATE_INVALID;
	synx_data = synx_util_acquire_handle(client, info.synx_obj);
	synx_obj = synx_util_obtain_object(synx_data);
	if (!IS_ERR_OR_NULL(synx_obj) && !IS_ERR_OR_NULL(synx_obj->fence)) {
		mutex_lock(&synx_obj->obj_lock);
		info.synx_state = synx_util_get_object_status(synx_obj);
		mutex_unlock(&synx_obj->obj_lock);
	}
	if (!IS_ERR_OR_NULL(synx_data))
		synx_util_release_handle(synx_data);
	synx_put_client(client);
	return k618_synx_v1_put(k_ioctl, &info, sizeof(info));
}

static int k618_synx_v1_merge(struct synx_private_ioctl_arg *k_ioctl,
	struct synx_session *session)
{
	struct synx_merge info;
	struct synx_merge_params params = {0};
	u32 *h_synxs, merged = 0;
	int rc = k618_synx_v1_get(k_ioctl, &info, sizeof(info));

	if (rc)
		return rc;
	if (info.num_objs >= SYNX_MAX_OBJS)
		return -EINVAL;
	h_synxs = kcalloc(info.num_objs, sizeof(*h_synxs), GFP_KERNEL);
	if (!h_synxs)
		return -ENOMEM;
	if (copy_from_user(h_synxs, u64_to_user_ptr(info.synx_objs),
			sizeof(*h_synxs) * info.num_objs)) {
		kfree(h_synxs);
		return -EFAULT;
	}
	params.num_objs = info.num_objs;
	params.h_synxs = h_synxs;
	/* 5.15: a local fence array signalled when all are (6.12 always merges so) */
	params.flags = SYNX_MERGE_LOCAL_FENCE | SYNX_MERGE_NOTIFY_ON_ALL;
	params.h_merged_obj = &merged;
	rc = synx_merge(session, &params);
	kfree(h_synxs);
	if (rc)
		return rc;
	info.merged = merged;
	return k618_synx_v1_put(k_ioctl, &info, sizeof(info));
}

/* 5.15 used payload[0] only and had no timeout; 6.12 takes payload[2] as one, in ms */
static int k618_synx_v1_register_payload(struct synx_private_ioctl_arg *k_ioctl,
	struct synx_session *session)
{
	struct synx_userpayload_info data;
	struct synx_callback_params params = {0};
	int rc = k618_synx_v1_get(k_ioctl, &data, sizeof(data));

	if (rc)
		return rc;
	params.h_synx = data.synx_obj;
	params.cb_func = synx_util_default_user_callback;
	params.userdata = (void *)data.payload[0];
	params.timeout_ms = SYNX_NO_TIMEOUT;
	return synx_async_wait(session, &params);
}

/* v1's descriptor is type, reserved, id[2] (the CSL sync object in id[0]); v2's starts with a u64 id */
static int k618_synx_v1_bind(struct synx_private_ioctl_arg *k_ioctl,
	struct synx_session *session)
{
	struct synx_bind info;
	struct synx_external_desc_v2 desc = {0};
	int rc = k618_synx_v1_get(k_ioctl, &info, sizeof(info));

	if (rc)
		return rc;
	desc.id = (u32)info.ext_sync_desc.id[0];
	desc.type = info.ext_sync_desc.type;
	return synx_bind(session, info.synx_obj, desc);
}

/* count more releases for a handle the client holds: what 6.12's import does for such a handle */
static int k618_synx_v1_addrefcount(struct synx_private_ioctl_arg *k_ioctl,
	struct synx_session *session)
{
	struct synx_addrefcount info;
	struct synx_client *client;
	struct synx_handle_coredata *synx_data;
	int rc = k618_synx_v1_get(k_ioctl, &info, sizeof(info));

	if (rc)
		return rc;
	if (info.count > K618_SYNX_V1_MAX_REF_COUNTS)
		return -EINVAL;
	client = synx_get_client(session);
	if (IS_ERR_OR_NULL(client))
		return -EINVAL;
	synx_data = synx_util_acquire_handle(client, info.synx_obj);
	if (IS_ERR_OR_NULL(synx_data))
		rc = -EINVAL;
	else if (IS_ERR_OR_NULL(synx_util_obtain_object(synx_data)))
		rc = -EINVAL;
	else {
		spin_lock_bh(&client->handle_map_lock);
		while (info.count--) {
			synx_data->rel_count++;
			kref_get(&synx_data->refcount);
		}
		spin_unlock_bh(&client->handle_map_lock);
	}
	if (!IS_ERR_OR_NULL(synx_data))
		synx_util_release_handle(synx_data);
	synx_put_client(client);
	return rc;
}

/*
 * IMPORT / EXPORT. 5.15's local import went by the handle (secure_key unchecked) and needed an
 * export first, which held a reference for the importer. 6.12 has no export step: a client imports
 * a local handle by its value and holds its own reference -- which is how the EVA driver imports
 * CamX's objects here. So EXPORT only checks the handle: a reference held per export would never
 * come back from the EVA driver's import and would leak one object per request.
 */
static int k618_synx_v1_import(struct synx_private_ioctl_arg *k_ioctl,
	struct synx_session *session)
{
	struct synx_id_info info;
	struct synx_import_params params = {0};
	u32 h_synx, new_h_synx = 0;
	int rc = k618_synx_v1_get(k_ioctl, &info, sizeof(info));

	if (rc)
		return rc;
	h_synx = info.synx_obj;
	params.type = SYNX_IMPORT_INDV_PARAMS;
	params.indv.fence = &h_synx;
	params.indv.flags = SYNX_IMPORT_SYNX_FENCE | (synx_util_is_global_handle(h_synx) ?
		SYNX_IMPORT_GLOBAL_FENCE : SYNX_IMPORT_LOCAL_FENCE);
	params.indv.new_h_synx = &new_h_synx;
	if (synx_import(session, &params))
		return -EINVAL;
	info.new_synx_obj = new_h_synx;
	return k618_synx_v1_put(k_ioctl, &info, sizeof(info));
}

static int k618_synx_v1_export(struct synx_private_ioctl_arg *k_ioctl,
	struct synx_session *session)
{
	struct synx_id_info info;
	struct synx_client *client;
	struct synx_handle_coredata *synx_data;
	int rc = k618_synx_v1_get(k_ioctl, &info, sizeof(info));

	if (rc)
		return rc;
	if (info.padding & K618_SYNX_V1_FLAG_GLOBAL_FENCE) {
		/* no global synx on this phone (no IPCLite), as on 5.15 */
		dprintk(SYNX_WARN, "k618: v1 global export of %d refused\n", info.synx_obj);
		return -EINVAL;
	}
	client = synx_get_client(session);
	if (IS_ERR_OR_NULL(client))
		return -EINVAL;
	synx_data = synx_util_acquire_handle(client, info.synx_obj);
	if (IS_ERR_OR_NULL(synx_data))
		rc = -EINVAL;
	else
		synx_util_release_handle(synx_data);
	synx_put_client(client);
	/* 5.15's local export left secure_key as it came */
	return rc ? rc : k618_synx_v1_put(k_ioctl, &info, sizeof(info));
}

static long k618_synx_v1_ioctl(struct synx_private_ioctl_arg *k_ioctl,
	struct synx_session *session, unsigned long arg)
{
	int rc;

	switch (k_ioctl->id) {
	case SYNX_CREATE:
		return k618_synx_v1_create(k_ioctl, session);
	case SYNX_RELEASE:
		return synx_handle_release(k_ioctl, session);
	case SYNX_SIGNAL:
		return k618_synx_v1_signal(k_ioctl, session);
	case SYNX_MERGE:
		return k618_synx_v1_merge(k_ioctl, session);
	case SYNX_REGISTER_PAYLOAD:
		return k618_synx_v1_register_payload(k_ioctl, session);
	case SYNX_DEREGISTER_PAYLOAD:
		return synx_handle_cancel_async_wait(k_ioctl, session);
	case SYNX_WAIT:
		rc = synx_handle_wait(k_ioctl, session);
		/* 5.15's synx_wait said 0 for a success signal, 6.12's says the state */
		if (k_ioctl->result == SYNX_STATE_SIGNALED_SUCCESS)
			k_ioctl->result = 0;
		if (copy_to_user((void __user *)arg, k_ioctl, sizeof(*k_ioctl)))
			rc = -EFAULT;
		return rc;
	case SYNX_BIND:
		return k618_synx_v1_bind(k_ioctl, session);
	case SYNX_ADDREFCOUNT:
		return k618_synx_v1_addrefcount(k_ioctl, session);
	case SYNX_GETSTATUS:
		return k618_synx_v1_getstatus(k_ioctl, session);
	case SYNX_IMPORT:
		return k618_synx_v1_import(k_ioctl, session);
	case SYNX_EXPORT:
		return k618_synx_v1_export(k_ioctl, session);
	}
	return -EINVAL;
}

"""

EDITS += [
    # --- synx v1 1. the client flag
    ("synx-kernel/msm/synx/synx_private.h",
     "\tstruct work_struct dispatch;\n\tstruct hlist_node node;\n};\n\nstruct synx_native {\n",
     "\tstruct work_struct dispatch;\n\tstruct hlist_node node;\n"
     "\tbool k618_v1;\t/* k618: the session open made, v1 user interface (synx.c) */\n"
     "};\n\nstruct synx_native {\n"),
    # --- synx v1 2. the v1 commands (SYNX_V1 above), and synx_ioctl hands them over
    ("synx-kernel/msm/synx/synx.c",
     "static long synx_ioctl(struct file *filep,\n",
     SYNX_V1 + "static long synx_ioctl(struct file *filep,\n"),
    ("synx-kernel/msm/synx/synx.c",
     "\tif (IS_ERR_OR_NULL(session) && k_ioctl.id != SYNX_INITIALIZE) {\n",
     "\t/* k618: 5.15's user interface on the session open made (k618_synx_v1_ioctl) */\n"
     "\tif (k618_synx_v1(session)) {\n"
     "\t\tif (k_ioctl.id <= SYNX_EXPORT)\n"
     "\t\t\treturn k618_synx_v1_ioctl(&k_ioctl, session, arg);\n"
     "\t\tif (k_ioctl.id == SYNX_INITIALIZE || k_ioctl.id == SYNX_INITIALIZE_V3) {\n"
     "\t\t\tsynx_uninitialize(session);\n"
     "\t\t\tsession = NULL;\n"
     "\t\t\tfilep->private_data = NULL;\n"
     "\t\t}\n"
     "\t}\n\n"
     "\tif (IS_ERR_OR_NULL(session) && k_ioctl.id != SYNX_INITIALIZE) {\n"),
    # --- synx v1 3. open makes the session, as 5.15's did
    ("synx-kernel/msm/synx/synx.c",
     "\tdprintk(SYNX_VERB, \"Enter pid: %d\\n\", current->pid);\n\tfilep->private_data = NULL;\n",
     "\tdprintk(SYNX_VERB, \"Enter pid: %d\\n\", current->pid);\n"
     "\t/* k618: a session from the start, as 5.15 made it (v1 user interface) */\n"
     "\tfilep->private_data = k618_synx_v1_session();\n"),
    # --- synx v1 4. 5.15 took any state above ACTIVE in a signal; 6.12's API check takes only
    # success, cancel and > 64, and refused the error state 3 (QEMU: SIGNAL -22, the object left
    # active and its waiters with it). 6.12's own signal path handles 3 (fence error -3, read back
    # as SIGNALED_ERROR); a v1 session gets past the check with any state, every client with 3
    # (synx v1 6 below).
    ("synx-kernel/msm/synx/synx.c",
     "\t\t\tstatus == SYNX_STATE_SIGNALED_CANCEL ||\n"
     "\t\t\tstatus > SYNX_STATE_SIGNALED_MAX)) {\n"
     "\t\tdprintk(SYNX_ERR,\n"
     "\t\t\t\"[sess :%llu] signaling with wrong status: %u\\n\",\n",
     "\t\t\tstatus == SYNX_STATE_SIGNALED_CANCEL ||\n"
     "\t\t\tstatus > SYNX_STATE_SIGNALED_MAX ||\n"
     "\t\t\tclient->k618_v1)) {\t/* k618: a v1 session, any state > ACTIVE (5.15) */\n"
     "\t\tdprintk(SYNX_ERR,\n"
     "\t\t\t\"[sess :%llu] signaling with wrong status: %u\\n\",\n"),
    # --- synx v1 5. a bound object (SYNX_BIND) was never signalled when its external sync fired:
    # synx_external_callback hands the signal to synx_signal_handler, which signals the fence only
    # for IPC signals, then dispatches only if the fence is no longer active -- so nothing happened
    # (QEMU: the CSL stand-in fired, no event, the object still active; 6.12's header calls bind
    # "NOT SUPPORTED"). 5.15's external callback signalled the fence and then dispatched. Only
    # synx_external_callback sets FROM_CALLBACK, and only bind ops call it.
    ("synx-kernel/msm/synx/synx.c",
     "\tmutex_lock(&synx_obj->obj_lock);\n"
     "\tif (signal_cb->flag & SYNX_SIGNAL_FROM_IPC &&\n"
     "\t\tsynx_util_get_object_status(synx_obj) == SYNX_STATE_ACTIVE) {\n",
     "\tmutex_lock(&synx_obj->obj_lock);\n"
     "\t/* k618: a bound external sync's signal too, as 5.15's synx_external_callback did */\n"
     "\tif (signal_cb->flag & (SYNX_SIGNAL_FROM_IPC | SYNX_SIGNAL_FROM_CALLBACK) &&\n"
     "\t\tsynx_util_get_object_status(synx_obj) == SYNX_STATE_ACTIVE) {\n"),


    ("synx-kernel/msm/synx/synx.c",
     "\t\t\tstatus > SYNX_STATE_SIGNALED_MAX ||\n"
     "\t\t\tclient->k618_v1)) {",
     "\t\t\tstatus > SYNX_STATE_SIGNALED_MAX ||\n"
     "\t\t\tstatus == SYNX_STATE_SIGNALED_ERROR ||\t/* k618: every client, as 5.15 */\n"
     "\t\t\tclient->k618_v1)) {"),
    # (The EVA node also binds each synx object to a CSL fence -- camera 13, k618_tp.sh's camera row.)
]


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

EDITS += [
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

EDITS += [
    # 6.14 moved GRO out of the NAPI layer: a napi_struct's GRO lists are in napi->gro (struct
    # gro_node), and napi_gro_flush() is an inline in <net/gro.h>, which netdevice.h no longer pulls in
    ("wlan/qcacld-3.0/os_if/dp/src/os_if_dp_txrx.c",
     "#include <linux/netdevice.h>\n",
     "#include <linux/netdevice.h>\n#include <net/gro.h>\t/* k618: napi_gro_flush, 6.14+ */\n"),
    ("wlan/qcacld-3.0/os_if/dp/src/os_if_dp_txrx.c",
     "\t\tif (napi->rx_count) {\n"
     "\t\t\tnetif_receive_skb_list(&napi->rx_list);\n"
     "\t\t\tqdf_init_list_head(&napi->rx_list);\n"
     "\t\t\tnapi->rx_count = 0;\n",
     "\t\tif (napi->gro.rx_count) {\t/* k618: napi->gro, 6.14+ */\n"
     "\t\t\tnetif_receive_skb_list(&napi->gro.rx_list);\n"
     "\t\t\tqdf_init_list_head(&napi->gro.rx_list);\n"
     "\t\t\tnapi->gro.rx_count = 0;\n"),
    ("wlan/qcacld-3.0/components/dp/core/src/wlan_dp_txrx.c",
     "#include <qdf_types.h>\n",
     "#include <qdf_types.h>\n#include <net/gro.h>\t/* k618: napi_gro_flush, 6.14+ */\n"),
    # txq_trans_update() takes the device too (6.18: it skips lockless-TX devices)
    ("wlan/qcacld-3.0/core/hdd/src/wlan_hdd_tx_rx.c",
     "\t\t\t\ttxq_trans_update(txq);\n",
     "\t\t\t\ttxq_trans_update(dev, txq);\n"),
]


K618_CP_LEGACY = """/*
 * k618: a blob the size of 5.15's drm_msm_igc_lut / drm_msm_pgc_lut (everything up to the 6.x
 * *_extended tables) is copied into one of today's size: tables zero and the 6.x high-precision
 * flag -- the only thing that reads them -- clear, which is what a client that never heard of them
 * meant. Anything else is returned as it was.
 */
static struct drm_property_blob *k618_cp_legacy_blob(struct drm_device *dev,
	struct drm_property_blob *blob, u32 feature, size_t want)
{
	struct drm_property_blob *big;
	size_t old;

	if (feature == SDE_CP_CRTC_DSPP_IGC && want == sizeof(struct drm_msm_igc_lut))
		old = offsetof(struct drm_msm_igc_lut, c0_extended);
	else if (feature == SDE_CP_CRTC_DSPP_GC && want == sizeof(struct drm_msm_pgc_lut))
		old = offsetof(struct drm_msm_pgc_lut, c0_extended);
	else
		return blob;
	if (blob->length != old)
		return blob;
	big = drm_property_create_blob(dev, want, NULL);	/* zero-filled */
	if (IS_ERR(big))
		return blob;
	memcpy(big->data, blob->data, old);
	if (feature == SDE_CP_CRTC_DSPP_IGC)
		((struct drm_msm_igc_lut *)big->data)->flags &= ~(__u64)IGC_HIGH_PREC_ENABLE;
	else
		((struct drm_msm_pgc_lut *)big->data)->flags &= ~(__u64)PGC_HIGHPREC_EN;
	drm_property_blob_put(blob);
	return big;
}

"""

EDITS += [
    ("display-drivers/msm/sde/sde_color_processing.c",
     "static int _sde_cp_crtc_cache_blob_property(struct drm_crtc *crtc,\n",
     K618_CP_LEGACY + "static int _sde_cp_crtc_cache_blob_property(struct drm_crtc *crtc,\n"),
    ("display-drivers/msm/sde/sde_color_processing.c",
     "\t\tDRM_ERROR(\"invalid blob id %lld feature %d\\n\", val, prop_node->feature);\n"
     "\t\treturn -EINVAL;\n"
     "\t}\n"
     "\tif (blob->length != prop_node->prop_blob_sz) {\n",
     "\t\tDRM_ERROR(\"invalid blob id %lld feature %d\\n\", val, prop_node->feature);\n"
     "\t\treturn -EINVAL;\n"
     "\t}\n"
     "\tblob = k618_cp_legacy_blob(crtc->dev, blob, prop_node->feature,\n"
     "\t\t\t\t   prop_node->prop_blob_sz);\t/* k618: 5.15 clients */\n"
     "\tif (blob->length != prop_node->prop_blob_sz) {\n"),
    # the planes' DMA degamma / gamma read c2 only (the first 256 / 128 words), all of it inside
    # the 5.15 layout: a blob of that size is taken as it is
    ("display-drivers/msm/sde/sde_hw_reg_dma_v1_color_proc.c",
     "\t\tdma_igcv5_off(ctx, cfg, idx);\n"
     "\t\treturn;\n"
     "\t}\n"
     "\n"
     "\tif (hw_cfg->len != sizeof(struct drm_msm_igc_lut)) {\n",
     "\t\tdma_igcv5_off(ctx, cfg, idx);\n"
     "\t\treturn;\n"
     "\t}\n"
     "\n"
     "\tif (hw_cfg->len != sizeof(struct drm_msm_igc_lut) &&\t/* k618: or 5.15's, c2 only read */\n"
     "\t    hw_cfg->len != offsetof(struct drm_msm_igc_lut, c0_extended)) {\n"),
    ("display-drivers/msm/sde/sde_hw_reg_dma_v1_color_proc.c",
     "\t\tdma_gcv5_off(ctx, cfg, idx);\n"
     "\t\treturn;\n"
     "\t}\n"
     "\n"
     "\tif (hw_cfg->len != sizeof(struct drm_msm_pgc_lut)) {\n",
     "\t\tdma_gcv5_off(ctx, cfg, idx);\n"
     "\t\treturn;\n"
     "\t}\n"
     "\n"
     "\tif (hw_cfg->len != sizeof(struct drm_msm_pgc_lut) &&\t/* k618: or 5.15's, c2 only read */\n"
     "\t    hw_cfg->len != offsetof(struct drm_msm_pgc_lut, c0_extended)) {\n"),
]


EDITS += [
    ("camera-kernel/include/uapi/camera/media/cam_defs.h",
     "struct cam_acquire_dev_cmd {\n"
     "\t__s32        session_handle;\n"
     "\t__s32        dev_handle;\n"
     "\t__u32        handle_type;\n"
     "\t__u32        num_resources;\n"
     "\t__u64        resource_hdl;\n"
     "};",
     "struct cam_acquire_dev_cmd {\n"
     "\t__s32        session_handle;\n"
     "\t__s32        dev_handle;\n"
     "\t__u32        handle_type;\n"
     "\t__u32        num_resources;\n"
     "\t__u64        k618_xiaomi_rsvd;\t/* k618: Xiaomi's 32-byte layout; CamX sends 0, never read */\n"
     "\t__u64        resource_hdl;\t\t/* k618: at 0x18, where the phone's camera.ko reads it */\n"
     "};"),
    ("camera-kernel/include/uapi/camera/media/cam_defs.h",
     "\t__u32        data_size;\n"
     "\t__u64        resource_hdl;\n"
     "};",
     "\t__u32        data_size;\n"
     "\t__u32        k618_xiaomi_link_hdl;\t/* k618: Xiaomi's 40-byte layout, not used */\n"
     "\t__u32        k618_xiaomi_pad;\n"
     "\t__u64        resource_hdl;\n"
     "};"),
    ("camera-kernel/include/uapi/camera/media/cam_defs.h",
     "\t__u32                    data_size;\n"
     "\t__u64                    resource_hdl;\n"
     "\tstruct cam_acquired_hw_info hw_info;",
     "\t__u32                    data_size;\n"
     "\t__u32                    k618_xiaomi_link_hdl;\t/* k618: Xiaomi's 128-byte layout, not used */\n"
     "\t__u32                    k618_xiaomi_pad;\n"
     "\t__u64                    resource_hdl;\n"
     "\tstruct cam_acquired_hw_info hw_info;"),

    # ---- camera 23 (the 13 Ultra): qcom,csiphy-v2.1.2-m1, 4 of its 8 PHYs. Unknown to this
    # driver ("invalid hw version", the PHY does not probe). Its stock 5.15 camera.ko (ishtar
    # OS3.0.308.0.WMACNXM; ctrl_reg_2_1_2_m1 -> csiphy_2ph_v2_1_2_reg_m1, symbols kept) differs from
    # its own 2.1.2 D-PHY table in 28 entries -- six registers of the per-lane template, on every
    # lane: 94h 07 -> d0, 00h 8e -> 92, 14h 60 -> 20, 04h 0c -> 0d, 10h 52 -> 72 and the skew-cal
    # 60h bd -> bc. The template here is the same registers per lane (offsets in csiphy_ln_offsets),
    # so -m1 is this table with those six values; the rest of 2.1.2 is shared.
    ("camera-kernel/drivers/cam_sensor_module/cam_csiphy/include/cam_csiphy_2_1_2_hwreg.h",
     "#endif /* _CAM_CSIPHY_2_1_2_HWREG_H_ */",
     "/* k618 camera 23: the 13 Ultra's qcom,csiphy-v2.1.2-m1 (see k618_tp_fix.py) */\n"
     "struct csiphy_reg_t csiphy_2ph_v2_1_2_reg_m1[] = {\n"
     "\t{0x0094, 0x00, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x00A0, 0x00, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x0090, 0x0f, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x0098, 0x08, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x0094, 0xD0, 0x01, CSIPHY_DEFAULT_PARAMS},\t/* 2.1.2: 0x07 */\n"
     "\t{0x0030, 0x00, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x0028, 0x0A, 0x00, CSIPHY_2PH_SEC_CLK_LN_SETTINGS},\n"
     "\t{0x0000, 0x92, 0x00, CSIPHY_DEFAULT_PARAMS},\t/* 2.1.2: 0x8E */\n"
     "\t{0x0038, 0xFE, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x002C, 0x01, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x0034, 0x0F, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x001C, 0x0A, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x0014, 0x20, 0x00, CSIPHY_DEFAULT_PARAMS},\t/* 2.1.2: 0x60 */\n"
     "\t{0x003C, 0xB8, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x0004, 0x0D, 0x00, CSIPHY_DEFAULT_PARAMS},\t/* 2.1.2: 0x0C */\n"
     "\t{0x0020, 0x00, 0x00, CSIPHY_DEFAULT_PARAMS},\n"
     "\t{0x0008, 0x10, 0x00, CSIPHY_SETTLE_CNT_LOWER_BYTE},\n"
     "\t{0x0010, 0x72, 0x00, CSIPHY_DEFAULT_PARAMS},\t/* 2.1.2: 0x52 */\n"
     "\t{0x0094, 0xD7, 0x00, CSIPHY_SKEW_CAL},\n"
     "\t{0x005C, 0x04, 0x00, CSIPHY_SKEW_CAL},\n"
     "\t{0x0060, 0xBC, 0x00, CSIPHY_SKEW_CAL},\t/* 2.1.2: 0xBD */\n"
     "\t{0x0064, 0x7F, 0x00, CSIPHY_SKEW_CAL},\n"
     "};\n\n"
     "struct csiphy_ctrl_t ctrl_reg_2_1_2_m1 = {\n"
     "\t.csiphy_common_reg = csiphy_common_reg_2_1_2,\n"
     "\t.csiphy_2ph_reg = csiphy_2ph_v2_1_2_reg_m1,\n"
     "\t.csiphy_2ph_clk_ln_reg = csiphy_2ph_v2_1_2_clk_ln_reg,\n"
     "\t.csiphy_3ph_reg = csiphy_3ph_v2_1_2_reg,\n"
     "\t.csiphy_reg = &csiphy_v2_1_2,\n"
     "\t.csiphy_irq_reg = csiphy_irq_reg_2_1_2,\n"
     "\t.csiphy_reset_enter_regs = csiphy_reset_enter_reg_2_1_2,\n"
     "\t.csiphy_reset_exit_regs = csiphy_reset_exit_reg_2_1_2,\n"
     "\t.csiphy_lane_config_reg = csiphy_lane_en_reg_2_1_2,\n"
     "\t.csiphy_ln_offsets[DPHY_DATA_LANE_POS_0] = 0x0000,\n"
     "\t.csiphy_ln_offsets[DPHY_DATA_LANE_POS_1] = 0x0400,\n"
     "\t.csiphy_ln_offsets[DPHY_DATA_LANE_POS_2] = 0x0800,\n"
     "\t.csiphy_ln_offsets[DPHY_DATA_LANE_POS_3] = 0x0C00,\n"
     "\t.csiphy_ln_offsets[DPHY_CLOCK_LANE_POS] = 0x0E00,\n"
     "\t.csiphy_ln_offsets[CPHY_LANE_POS_0] = 0x0200,\n"
     "\t.csiphy_ln_offsets[CPHY_LANE_POS_1] = 0x0600,\n"
     "\t.csiphy_ln_offsets[CPHY_LANE_POS_2] = 0x0A00,\n"
     "\t.data_rates_settings_table = &data_rate_delta_table_2_1_2,\n"
     "\t.csiphy_bist_reg = &bist_setting_2_1_2,\n"
     "\t.getclockvoting = get_clk_voting_dynamic,\n"
     "};\n\n"
     "#endif /* _CAM_CSIPHY_2_1_2_HWREG_H_ */"),
    ("camera-kernel/drivers/cam_sensor_module/cam_csiphy/cam_csiphy_soc.c",
     "\t} else if (of_device_is_compatible(soc_info->dev->of_node, \"qcom,csiphy-v2.1.2\")) {\n",
     "\t} else if (of_device_is_compatible(soc_info->dev->of_node, \"qcom,csiphy-v2.1.2-m1\")) {\n"
     "\t\t/* k618 camera 23: the 13 Ultra (cam_csiphy_2_1_2_hwreg.h) */\n"
     "\t\tcsiphy_dev->ctrl_reg = &ctrl_reg_2_1_2_m1;\n"
     "\t\tcsiphy_dev->hw_version = CSIPHY_VERSION_V212;\n"
     "\t\tcsiphy_dev->is_divisor_32_comp = true;\n"
     "\t\tcsiphy_dev->clk_lane = 0;\n"
     "\t} else if (of_device_is_compatible(soc_info->dev->of_node, \"qcom,csiphy-v2.1.2\")) {\n"),
]


MI_CMD_SETS = [
    ("DSI_CMD_SET_MI_DIMMINGON",            "mi,mdss-dsi-dimmingon-command"),
    ("DSI_CMD_SET_MI_DIMMINGOFF",           "mi,mdss-dsi-dimmingoff-command"),
    ("DSI_CMD_SET_MI_HBM_ON",               "mi,mdss-dsi-hbm-on-command"),
    ("DSI_CMD_SET_MI_HBM_OFF",              "mi,mdss-dsi-hbm-off-command"),
    ("DSI_CMD_SET_MI_FLAT_MODE_ON",         "mi,mdss-dsi-flat-mode-on-command"),
    ("DSI_CMD_SET_MI_FLAT_MODE_OFF",        "mi,mdss-dsi-flat-mode-off-command"),
    ("DSI_CMD_SET_MI_EM_CYCLE_16PULSE_ON",  "mi,mdss-dsi-panel-em-cycle-16pulse-on-command"),
    ("DSI_CMD_SET_MI_EM_CYCLE_16PULSE_OFF", "mi,mdss-dsi-panel-em-cycle-16pulse-off-command"),
]

_enum_block = "\n".join("\t%s," % e for e, _ in MI_CMD_SETS) + "\n"
_prop_block = "".join('\t"%s",\n' % p for _, p in MI_CMD_SETS)
_state_block = "".join('\t"%s-state",\n' % p for _, p in MI_CMD_SETS)

EDITS += [
    # 1. enum dsi_cmd_set_type: the Xiaomi sets go right before DSI_CMD_SET_MAX, so priv_info->
    #    cmd_sets[] (sized [DSI_CMD_SET_MAX]) grows to hold them and the parser's
    #    "for (i = PRE_ON; i < MAX; i++)" loop picks them up with no further change.
    ("display-drivers/msm/dsi/dsi_defs.h",
     "\tDSI_CMD_SET_FPS_SWITCH,\n"
     "\tDSI_CMD_SET_MAX\n",
     "\tDSI_CMD_SET_FPS_SWITCH,\n"
     + _enum_block +
     "\tDSI_CMD_SET_MAX\n"),

    # 2. cmd_set_prop_map[] / cmd_set_state_map[]: same ordinal as the enum (both positional).
    #    The map's last stock entry is the fps-switch command; append ours after it.
    ("display-drivers/msm/dsi/dsi_panel.c",
     '\t"qcom,mdss-dsi-fps-switch-command",\n'
     "};\n\n"
     "const char *cmd_set_state_map[DSI_CMD_SET_MAX] = {",
     '\t"qcom,mdss-dsi-fps-switch-command",\n'
     + _prop_block +
     "};\n\n"
     "const char *cmd_set_state_map[DSI_CMD_SET_MAX] = {"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     '\t"qcom,mdss-dsi-fps-switch-command-state",\n'
     "};\n#endif /* OPLUS_FEATURE_DISPLAY */",
     '\t"qcom,mdss-dsi-fps-switch-command-state",\n'
     + _state_block +
     "};\n#endif /* OPLUS_FEATURE_DISPLAY */"),

    # 3. dsi_panel.c: include the hooks; fire panel-on (POWER + dimming) after the panel is up;
    #    pass the DCS backlight level through as a 51_brightness event.
    # (its own anchor: the k618_bl_level edit owns the one after "dsi_panel.h", and an include
    # wedged in there made that edit apply a second time)
    ("display-drivers/msm/dsi/dsi_panel.c",
     '#include "dsi_parser.h"\n',
     '#include "dsi_parser.h"\n#include "mi_disp_k618.h"\t/* k618: Xiaomi\'s display extension */\n'),
    # k618_mi_disp.c sends the Xiaomi sets with dsi_panel_tx_cmd_set(), which only OnePlus' build
    # (OPLUS_FEATURE_DISPLAY) leaves non-static
    ("display-drivers/msm/dsi/dsi_panel.c",
     "#ifndef OPLUS_FEATURE_DISPLAY\n"
     "static int dsi_panel_tx_cmd_set(struct dsi_panel *panel,\n",
     "#ifndef OPLUS_FEATURE_DISPLAY\n"
     "int dsi_panel_tx_cmd_set(struct dsi_panel *panel,\t/* k618: k618_mi_disp.c calls it */\n"),
    # The panel-on hook goes right after the k618_bl_level block (which the edit above puts after
    # "panel->panel_initialized = true;"): inside that edit's text it would make the edit apply again
    # on the next run. The block is there twice -- the live path and a CONFIG_PXLW_IRIS copy that is
    # compiled out -- so the hook is too.
    ("display-drivers/msm/dsi/dsi_panel.c",
     "\t\t\t panel->bl_config.bl_max_level, k618_rc);\n\t}\n",
     "\t\t\t panel->bl_config.bl_max_level, k618_rc);\n\t}\n"
     "\tk618_mi_disp_panel_on(panel);\t/* k618: Xiaomi's display extension */\n"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     "\tcase DSI_BACKLIGHT_DCS:\n"
     "\t\trc = dsi_panel_update_backlight(panel, bl_lvl);\n"
     "\t\tbreak;\n",
     "\tcase DSI_BACKLIGHT_DCS:\n"
     "\t\trc = dsi_panel_update_backlight(panel, bl_lvl);\n"
     "\t\tk618_mi_disp_bl(panel, bl_lvl);\n"
     "\t\tbreak;\n"),

    # 4. dsi_display.c: include the hooks; start the mi_disp char device/class/sysfs early in the
    #    DSI display probe (idempotent), so the composer's open of /dev/mi_display/disp_feature and
    #    its read of .../disp-DSI-0/panel_info succeed before any HAL connects.
    ("display-drivers/msm/dsi/dsi_display.c",
     "#include <linux/list.h>\n",
     "#include <linux/list.h>\n#include \"mi_disp_k618.h\"\n"),
    ("display-drivers/msm/dsi/dsi_display.c",
     "\tdisplay = devm_kzalloc(&pdev->dev, sizeof(*display), GFP_KERNEL);\n",
     "\tk618_mi_disp_init();\n\n"
     "\tdisplay = devm_kzalloc(&pdev->dev, sizeof(*display), GFP_KERNEL);\n"),

    # 5. Kbuild: link the port into msm_drm -- after the last list, because
    #    msm_drm-$(CONFIG_HDCP_QSEECOM) further up ASSIGNS (:=) msm_drm-y.
    ("display-drivers/msm/Kbuild",
     "\t\t\t     msm_prop.o\n",
     "\t\t\t     msm_prop.o\n"
     "\n# k618: Xiaomi's display extension (k618_mi_disp.c), after every := above\n"
     "msm_drm-y += mi_disp_k618.o\n"),
]


MI_FOD_CMD_SETS = [
    # local HBM, normal mode, with the 16-pulse (1920 Hz) variants for a base level at or below
    # the PWM threshold
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT",
     "mi,mdss-dsi-local-hbm-normal-white-1000nit-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT_16P",
     "mi,mdss-dsi-local-hbm-normal-white-1000nit-em-cycle-16pulse-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT",
     "mi,mdss-dsi-local-hbm-normal-white-110nit-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT_16P",
     "mi,mdss-dsi-local-hbm-normal-white-110nit-em-cycle-16pulse-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT",
     "mi,mdss-dsi-local-hbm-normal-green-500nit-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT_16P",
     "mi,mdss-dsi-local-hbm-normal-green-500nit-em-cycle-16pulse-command"),
    # local HBM from AOD
    ("DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT",
     "mi,mdss-dsi-local-hbm-hlpm-white-1000nit-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT",
     "mi,mdss-dsi-local-hbm-hlpm-white-110nit-command"),
    # local HBM off
    ("DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL",
     "mi,mdss-dsi-local-hbm-off-to-normal-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL_16P",
     "mi,mdss-dsi-local-hbm-off-to-normal-em-cycle-16pulse-command"),
    ("DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM",
     "mi,mdss-dsi-local-hbm-off-to-hbm-command"),
    # AOD brightness, and AOD -> normal for a fingerprint touch
    ("DSI_CMD_SET_MI_DOZE_HBM",      "mi,mdss-dsi-doze-hbm-command"),
    ("DSI_CMD_SET_MI_DOZE_LBM",      "mi,mdss-dsi-doze-lbm-command"),
    ("DSI_CMD_SET_MI_DOZE_HBM_NOLP", "mi,mdss-dsi-doze-hbm-nolp-command"),
    ("DSI_CMD_SET_MI_DOZE_LBM_NOLP", "mi,mdss-dsi-doze-lbm-nolp-command"),
]
_last_e, _last_p = MI_CMD_SETS[-1]

EDITS += [
    ("display-drivers/msm/dsi/dsi_defs.h",
     "\t%s,\n\tDSI_CMD_SET_MAX\n" % _last_e,
     "\t%s,\n" % _last_e + "".join("\t%s,\n" % e for e, _ in MI_FOD_CMD_SETS)
     + "\tDSI_CMD_SET_MAX\n"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     '\t"%s",\n};\n\nconst char *cmd_set_state_map[DSI_CMD_SET_MAX] = {' % _last_p,
     '\t"%s",\n' % _last_p + "".join('\t"%s",\n' % p for _, p in MI_FOD_CMD_SETS)
     + "};\n\nconst char *cmd_set_state_map[DSI_CMD_SET_MAX] = {"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     '\t"%s-state",\n};\n#endif /* OPLUS_FEATURE_DISPLAY */' % _last_p,
     '\t"%s-state",\n' % _last_p + "".join('\t"%s-state",\n' % p for _, p in MI_FOD_CMD_SETS)
     + "};\n#endif /* OPLUS_FEATURE_DISPLAY */"),
    # AOD entered (LP1, LP2): the doze level the HAL asked for goes out now -- stock queues
    # SET_DOZE_BRIGHTNESS until the panel is in LP1/LP2 (is_aod_and_panel_initialized) and wakes
    # it from the end of dsi_panel_set_lp1() (mi_dsi_display_wakeup_pending_doze_work); sent
    # while the panel is on, a doze command would put a lit panel into its 16-pulse doze mode
    ("display-drivers/msm/dsi/dsi_panel.c",
     '\t\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_LP1, false);\n'
     '\t\tif (rc)\n'
     '\t\t\tDSI_ERR("[%s] failed to send DSI_CMD_SET_LP1 cmd, rc=%d\\n",\n'
     '\t\t\t\tpanel->name, rc);\n'
     '\t}\n',
     '\t\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_LP1, false);\n'
     '\t\tif (rc)\n'
     '\t\t\tDSI_ERR("[%s] failed to send DSI_CMD_SET_LP1 cmd, rc=%d\\n",\n'
     '\t\t\t\tpanel->name, rc);\n'
     '\t}\n'
     '\tk618_mi_disp_post_lp(panel, rc);\t/* k618: in AOD -- the doze level asked for */\n'),
    ("display-drivers/msm/dsi/dsi_panel.c",
     '\t\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_LP2, false);\n'
     '\t\tif (rc)\n'
     '\t\t\tDSI_ERR("[%s] failed to send DSI_CMD_SET_LP2 cmd, rc=%d\\n",\n'
     '\t\t\t\tpanel->name, rc);\n'
     '\t}\n',
     '\t\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_LP2, false);\n'
     '\t\tif (rc)\n'
     '\t\t\tDSI_ERR("[%s] failed to send DSI_CMD_SET_LP2 cmd, rc=%d\\n",\n'
     '\t\t\t\tpanel->name, rc);\n'
     '\t}\n'
     '\tk618_mi_disp_post_lp(panel, rc);\t/* k618: in AOD -- the doze level asked for */\n'),
]


# The 13's M3 and the 13 Ultra's M1 panels (k618_mi_disp.c)
# The 13 Ultra's DBI-BWG and skip-source command sets (stock cmd_set_prop_map entries 88..123 of
# the 5.15 msm_drm.ko, .data+0x7f40), selected by DBV band and panel temperature in
# k618_m1_dbi() (stock mi_dsi_panel_vrr_set_by_dbv 0x18f4f4). Names exactly as the panel's DT has
# them; every one has a "-state" twin ("dsi_hs_mode" in the DT). Stock selects 88..97, 102..109
# and 114..123; 98..101 and 110..113 (bwg-37..40, bwg-49..52) are parsed but never sent, as on
# stock. Appended after MI_FOD_CMD_SETS' last entry by edits of their own: the tree is patched in
# place build after build. Positional like the rest.
MI_M1_CMD_SETS = [
    ("DSI_CMD_SET_MI_DBI_BWG_500_1300NIT", "mi,mdss-dsi-dbi-bwg-500-1300nit-mode-command"),
    ("DSI_CMD_SET_MI_DBI_BWG_200_500NIT",  "mi,mdss-dsi-dbi-bwg-200-500nit-mode-command"),
    ("DSI_CMD_SET_MI_DBI_BWG_110_200NIT",  "mi,mdss-dsi-dbi-bwg-110-200nit-mode-command"),
    ("DSI_CMD_SET_MI_DBI_BWG_80_110NIT",   "mi,mdss-dsi-dbi-bwg-80-110nit-mode-command"),
    ("DSI_CMD_SET_MI_DBI_BWG_72_79NIT",    "mi,mdss-dsi-dbi-bwg-72-79nit-mode-command"),
    ("DSI_CMD_SET_MI_DBI_BWG_65_72NIT",    "mi,mdss-dsi-dbi-bwg-65-72nit-mode-command"),
    ("DSI_CMD_SET_MI_DBI_BWG_60_65NIT",    "mi,mdss-dsi-dbi-bwg-60-65nit-mode-command"),
    ("DSI_CMD_SET_MI_DBI_BWG_53_60NIT",    "mi,mdss-dsi-dbi-bwg-53-60nit-mode-command"),
    ("DSI_CMD_SET_MI_DBI_BWG_30_53NIT",    "mi,mdss-dsi-dbi-bwg-30-53nit-mode-command"),
    ("DSI_CMD_SET_MI_DBI_BWG_0_30NIT",     "mi,mdss-dsi-dbi-bwg-0-30nit-mode-command"),
] + [
    # the temperature variants of the 0-30 nit set (stock 98..113)
    ("DSI_CMD_SET_MI_DBI_BWG_%d" % n, "mi,mdss-dsi-dbi-bwg-%d-mode-command" % n)
    for n in range(37, 53)
] + [
    ("DSI_CMD_SET_MI_SKIP_SOURCE_500_1300NIT", "mi,mdss-dsi-skip-source-500-1300nit-mode-command"),
    ("DSI_CMD_SET_MI_SKIP_SOURCE_200_500NIT",  "mi,mdss-dsi-skip-source-200-500nit-mode-command"),
    ("DSI_CMD_SET_MI_SKIP_SOURCE_110_200NIT",  "mi,mdss-dsi-skip-source-110-200nit-mode-command"),
    ("DSI_CMD_SET_MI_SKIP_SOURCE_80_110NIT",   "mi,mdss-dsi-skip-source-80-110nit-mode-command"),
    ("DSI_CMD_SET_MI_SKIP_SOURCE_72_79NIT",    "mi,mdss-dsi-skip-source-72-79nit-mode-command"),
    ("DSI_CMD_SET_MI_SKIP_SOURCE_65_72NIT",    "mi,mdss-dsi-skip-source-65-72nit-mode-command"),
    ("DSI_CMD_SET_MI_SKIP_SOURCE_60_65NIT",    "mi,mdss-dsi-skip-source-60-65nit-mode-command"),
    ("DSI_CMD_SET_MI_SKIP_SOURCE_53_60NIT",    "mi,mdss-dsi-skip-source-53-60nit-mode-command"),
    ("DSI_CMD_SET_MI_SKIP_SOURCE_30_53NIT",    "mi,mdss-dsi-skip-source-30-53nit-mode-command"),
    ("DSI_CMD_SET_MI_SKIP_SOURCE_0_30NIT",     "mi,mdss-dsi-skip-source-0-30nit-mode-command"),
]
assert len(MI_M1_CMD_SETS) == 36
_fod_last_e, _fod_last_p = MI_FOD_CMD_SETS[-1]

EDITS += [
    ("display-drivers/msm/dsi/dsi_defs.h",
     "\t%s,\n\tDSI_CMD_SET_MAX\n" % _fod_last_e,
     "\t%s,\n" % _fod_last_e + "".join("\t%s,\n" % e for e, _ in MI_M1_CMD_SETS)
     + "\tDSI_CMD_SET_MAX\n"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     '\t"%s",\n};\n\nconst char *cmd_set_state_map[DSI_CMD_SET_MAX] = {' % _fod_last_p,
     '\t"%s",\n' % _fod_last_p + "".join('\t"%s",\n' % p for _, p in MI_M1_CMD_SETS)
     + "};\n\nconst char *cmd_set_state_map[DSI_CMD_SET_MAX] = {"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     '\t"%s-state",\n};\n#endif /* OPLUS_FEATURE_DISPLAY */' % _fod_last_p,
     '\t"%s-state",\n' % _fod_last_p + "".join('\t"%s-state",\n' % p for _, p in MI_M1_CMD_SETS)
     + "};\n#endif /* OPLUS_FEATURE_DISPLAY */"),
]


EDITS += [
    # --- the speakers: two CS35L41 on TDM-LPAIF-RX-SECONDARY (see the header of this file)
    ("audio-kernel/asoc/kalama.c",
     "static struct snd_soc_ops msm_common_be_ops = {\n"
     "\t.hw_params = msm_common_snd_hw_params,\n"
     "\t.startup = msm_common_snd_startup,\n"
     "\t.shutdown = msm_common_snd_shutdown,\n"
     "};\n",
     """static struct snd_soc_ops msm_common_be_ops = {
	.hw_params = msm_common_snd_hw_params,
	.startup = msm_common_snd_startup,
	.shutdown = msm_common_snd_shutdown,
};

/*
 * k618: the speakers. No WSA here (qcom,wsa-max-devs = <0>): two Cirrus Logic CS35L41 on I2C
 * bus 2, cs35l41@40 \"B\" and cs35l41@42 \"T\" (cs35l41_dlkm.ko, upstream driver). Xiaomi's 5.15
 * machine driver puts both on this back end (its sec_tdm_rx_0 codecs: cs35l41.2-0040 and
 * cs35l41.2-0042) and leaves TDM-LPAIF-TX-SECONDARY on the stub; it names the I2C devices,
 * this takes the devicetree nodes. k618_spk_amps=0 keeps the stub codec, as before.
 */
static bool k618_spk_amps = true;
module_param(k618_spk_amps, bool, 0444);
MODULE_PARM_DESC(k618_spk_amps, \"k618: CS35L41 speaker amps on TDM-LPAIF-RX-SECONDARY\");

#define K618_TDM_SLOT_WIDTH	32	/* TDM_SLOT_WIDTH_BITS in msm_common.c */
#define K618_CS35L41_CLKID_SCLK	0	/* CS35L41_CLKID_SCLK, include/sound/cs35l41.h */

/*
 * The codec side the stock codec driver did by itself: DSP_A, codec as clock consumer, normal
 * polarity (its set_dai_fmt(0x4004) at every startup), PLL reference = the TDM bit clock
 * msm_common_snd_hw_params() programs, rate x 32 x qcom,tdm-max-slots (6.144 MHz at 48 kHz),
 * and 32-bit slots for the S24_LE samples. Which slot each amp takes stays with its
 * \"ASPRX1/ASPTX1 Slot Position\" controls (the HAL: T 0, B 1) and the devicetree default.
 */
static int k618_spk_amp_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct msm_common_pdata *common = msm_common_get_pdata(rtd->card);
	struct snd_soc_dai *codec_dai;
	unsigned int slots, sclk;
	int i, ret;

	ret = msm_common_snd_hw_params(substream, params);
	if (ret)
		return ret;

	slots = (common && common->tdm_max_slots) ? common->tdm_max_slots : 4;
	sclk = params_rate(params) * K618_TDM_SLOT_WIDTH * slots;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_DSP_A |
					  SND_SOC_DAIFMT_CBC_CFC | SND_SOC_DAIFMT_NB_NF);
		if (ret)
			goto err;
		/* the slot width only (both masks = the stereo pair, as the back end has) */
		ret = snd_soc_dai_set_tdm_slot(codec_dai, 0x3, 0x3, slots, K618_TDM_SLOT_WIDTH);
		if (ret && ret != -ENOTSUPP)
			goto err;
		ret = snd_soc_component_set_sysclk(codec_dai->component, K618_CS35L41_CLKID_SCLK,
						   0, sclk, SND_SOC_CLOCK_IN);
		if (ret)
			goto err;
		/* frame-sync monitor values: the codec has them up to 6.144 MHz and for 12.288 */
		if (snd_soc_dai_set_sysclk(codec_dai, K618_CS35L41_CLKID_SCLK, sclk,
					   SND_SOC_CLOCK_IN))
			dev_dbg(rtd->card->dev, \"k618: %s: no FS monitor values for %u Hz\\n\",
				codec_dai->name, sclk);
	}

	return 0;
err:
	dev_err(rtd->card->dev, \"k618: %s: amp setup failed (SCLK %u Hz): %d\\n\",
		codec_dai->name, sclk, ret);
	return ret;
}

static const struct snd_soc_ops k618_spk_amp_be_ops = {
	.hw_params = k618_spk_amp_hw_params,
	.startup = msm_common_snd_startup,
	.shutdown = msm_common_snd_shutdown,
};

/*
 * Every enabled \"cirrus,cs35l41\" node becomes a codec of the secondary TDM playback link, in
 * devicetree order (B @40, T @42 -- the stock order), DAI \"cs35l41-pcm\" (upstream's name;
 * the stock matched the legacy one, which equals the I2C device name). Like the stock, the
 * card then waits for both amps by deferring its probe until cs35l41_dlkm has bound them.
 */
/* Device compatibility. */
static bool k618_amp_ready(struct device_node *np)
{
	struct snd_soc_dai_link_component dlc = {
		.of_node = np,
		.dai_name = \"cs35l41-pcm\",
	};

	return of_device_is_available(np) && snd_soc_find_dai_with_mutex(&dlc);
}

static void k618_link_spk_amps(struct device *dev, struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link = NULL;
	struct snd_soc_dai_link_component *codecs;
	struct device_node *np;
	int i, n = 0;

	if (!k618_spk_amps)
		return;
	for (i = 0; i < card->num_links; i++)
		if (card->dai_link[i].name &&
		    !strcmp(card->dai_link[i].name, LPASS_BE_SEC_TDM_RX_0))
			link = &card->dai_link[i];
	if (!link)
		return;
	for_each_compatible_node(np, NULL, \"cirrus,cs35l41\")
		if (k618_amp_ready(np))
			n++;
	if (!n) {
		dev_warn(dev, \"k618: no CS35L41 bound, %s stays on the stub\\n\", link->name);
		return;
	}
	codecs = devm_kcalloc(dev, n, sizeof(*codecs), GFP_KERNEL);
	if (!codecs)
		return;
	i = 0;
	for_each_compatible_node(np, NULL, \"cirrus,cs35l41\") {
		if (!k618_amp_ready(np) || i == n)
			continue;
		codecs[i].of_node = of_node_get(np);
		codecs[i].dai_name = \"cs35l41-pcm\";
		i++;
	}
	link->codecs = codecs;
	link->num_codecs = i;
	link->ops = &k618_spk_amp_be_ops;
	dev_info(dev, \"k618: %d CS35L41 speaker amp(s) on %s\\n\", i, link->name);
}
"""),
    ("audio-kernel/asoc/kalama.c",
     "\tcard = populate_snd_card_dailinks(&pdev->dev, pdata->wsa_max_devs);\n"
     "\tif (!card) {\n"
     "\t\tdev_err(&pdev->dev, \"%s: Card uninitialized\\n\", __func__);\n"
     "\t\tret = -EINVAL;\n"
     "\t\tgoto err;\n"
     "\t}\n",
     "\tcard = populate_snd_card_dailinks(&pdev->dev, pdata->wsa_max_devs);\n"
     "\tif (!card) {\n"
     "\t\tdev_err(&pdev->dev, \"%s: Card uninitialized\\n\", __func__);\n"
     "\t\tret = -EINVAL;\n"
     "\t\tgoto err;\n"
     "\t}\n"
     "\tk618_link_spk_amps(&pdev->dev, card);\n"),
]


# ipa_5_5_mem_part, field by field: (field, 6.12 release value, kalama value = 5.15 source =
# the phone's own ipam.ko). Same field order as both initializers.
K618_IPA_PART = [
    ("uc_descriptor_ram_ofst",      "0x0",    "0x0"),
    ("uc_descriptor_ram_size",      "0x300",  "0x1000"),
    ("uc_ofst",                     "0x300",  "0x1000"),
    ("uc_size",                     "0x80",   "0x80"),
    ("uc_info_ofst",                "0x380",  "0x1080"),
    ("uc_info_size",                "0x80",   "0x200"),
    ("ofst_start",                  "0x400",  "0x1280"),
    ("v4_flt_hash_ofst",            "0x408",  "0x1288"),
    ("v4_flt_hash_size",            "0x78",   "0x78"),
    ("v4_flt_hash_size_ddr",        "0x4000", "0x4000"),
    ("v4_flt_nhash_ofst",           "0x488",  "0x1308"),
    ("v4_flt_nhash_size",           "0x78",   "0x78"),
    ("v4_flt_nhash_size_ddr",       "0x4000", "0x4000"),
    ("v6_flt_hash_ofst",            "0x508",  "0x1388"),
    ("v6_flt_hash_size",            "0x78",   "0x78"),
    ("v6_flt_hash_size_ddr",        "0x4000", "0x4000"),
    ("v6_flt_nhash_ofst",           "0x588",  "0x1408"),
    ("v6_flt_nhash_size",           "0x78",   "0x78"),
    ("v6_flt_nhash_size_ddr",       "0x4000", "0x4000"),
    ("v4_rt_num_index",             "0x13",   "0x13"),
    ("v4_modem_rt_index_lo",        "0x0",    "0x0"),
    ("v4_modem_rt_index_hi",        "0xa",    "0xa"),
    ("v4_apps_rt_index_lo",         "0xb",    "0xb"),
    ("v4_apps_rt_index_hi",         "0x12",   "0x12"),
    ("v4_rt_hash_ofst",             "0x608",  "0x1488"),
    ("v4_rt_hash_size",             "0x98",   "0x98"),
    ("v4_rt_hash_size_ddr",         "0x4000", "0x4000"),
    ("v4_rt_nhash_ofst",            "0x6A8",  "0x1528"),
    ("v4_rt_nhash_size",            "0x98",   "0x98"),
    ("v4_rt_nhash_size_ddr",        "0x4000", "0x4000"),
    ("v6_rt_num_index",             "0x13",   "0x13"),
    ("v6_modem_rt_index_lo",        "0x0",    "0x0"),
    ("v6_modem_rt_index_hi",        "0xa",    "0xa"),
    ("v6_apps_rt_index_lo",         "0xb",    "0xb"),
    ("v6_apps_rt_index_hi",         "0x12",   "0x12"),
    ("v6_rt_hash_ofst",             "0x748",  "0x15c8"),
    ("v6_rt_hash_size",             "0x98",   "0x98"),
    ("v6_rt_hash_size_ddr",         "0x4000", "0x4000"),
    ("v6_rt_nhash_ofst",            "0x7E8",  "0x1668"),
    ("v6_rt_nhash_size",            "0x098",  "0x098"),
    ("v6_rt_nhash_size_ddr",        "0x4000", "0x4000"),
    ("modem_hdr_ofst",              "0x888",  "0x1708"),
    ("modem_hdr_size",              "0x240",  "0x240"),
    ("apps_hdr_ofst",               "0xAC8",  "0x1948"),
    ("apps_hdr_size",               "0x1e0",  "0x1e0"),
    ("apps_hdr_size_ddr",           "0x800",  "0x800"),
    ("modem_hdr_proc_ctx_ofst",     "0xCC0",  "0x1b40"),
    ("modem_hdr_proc_ctx_size",     "0xb20",  "0xb20"),
    ("apps_hdr_proc_ctx_ofst",      "0x17E0", "0x2660"),
    ("apps_hdr_proc_ctx_size",      "0x200",  "0x200"),
    ("apps_hdr_proc_ctx_size_ddr",  "0x0",    "0x0"),
    ("stats_quota_q6_ofst",         "0x19E8", "0x2868"),
    ("stats_quota_q6_size",         "0x60",   "0x60"),
    ("stats_quota_ap_ofst",         "0x1A48", "0x28C8"),
    ("stats_quota_ap_size",         "0x48",   "0x48"),
    ("stats_tethering_ofst",        "0x1A90", "0x2910"),
    ("stats_tethering_size",        "0x3c0",  "0x3c0"),
    ("stats_flt_v4_ofst",           "0",      "0"),
    ("stats_flt_v4_size",           "0",      "0"),
    ("stats_flt_v6_ofst",           "0",      "0"),
    ("stats_flt_v6_size",           "0",      "0"),
    ("stats_rt_v4_ofst",            "0",      "0"),
    ("stats_rt_v4_size",            "0",      "0"),
    ("stats_rt_v6_ofst",            "0",      "0"),
    ("stats_rt_v6_size",            "0",      "0"),
    ("stats_fnr_ofst",              "0x1E50", "0x2cd0"),
    ("stats_fnr_size",              "0x1000", "0xba0"),
    ("stats_drop_ofst",             "0x2E50", "0x3870"),
    ("stats_drop_size",             "0x20",   "0x20"),
    ("modem_comp_decomp_ofst",      "0x0",    "0x0"),
    ("modem_comp_decomp_size",      "0x0",    "0x0"),
    ("modem_ofst",                  "0x2E78", "0x3898"),
    ("modem_size",                  "0xd48",  "0xd48"),
    ("nat_tbl_ofst",                "0x3BC0", "0x45e0"),
    ("nat_tbl_size",                "0x900",  "0x900"),
    ("apps_v4_flt_hash_ofst",       "0x2718", "0x2718"),
    ("apps_v4_flt_hash_size",       "0x0",    "0x0"),
    ("apps_v4_flt_nhash_ofst",      "0x2718", "0x2718"),
    ("apps_v4_flt_nhash_size",      "0x0",    "0x0"),
    ("apps_v6_flt_hash_ofst",       "0x2718", "0x2718"),
    ("apps_v6_flt_hash_size",       "0x0",    "0x0"),
    ("apps_v6_flt_nhash_ofst",      "0x2718", "0x2718"),
    ("apps_v6_flt_nhash_size",      "0x0",    "0x0"),
    ("apps_v4_rt_hash_ofst",        "0x2718", "0x2718"),
    ("apps_v4_rt_hash_size",        "0x0",    "0x0"),
    ("apps_v4_rt_nhash_ofst",       "0x2718", "0x2718"),
    ("apps_v4_rt_nhash_size",       "0x0",    "0x0"),
    ("apps_v6_rt_hash_ofst",        "0x2718", "0x2718"),
    ("apps_v6_rt_hash_size",        "0x0",    "0x0"),
    ("apps_v6_rt_nhash_ofst",       "0x2718", "0x2718"),
    ("apps_v6_rt_nhash_size",       "0x0",    "0x0"),
    ("pdn_config_ofst",             "0x44C8", "0x4ee8"),
    ("pdn_config_size",             "0x50",   "0x100"),
    ("end_ofst",                    "0x4518", "0x4fe8"),
]


def _k618_ipa_part(col):
    """The ipa_5_5_mem_part initializer with column col of PART (1 = 6.12 text, 2 = kalama)."""
    return ("static struct ipa3_mem_partition ipa_5_5_mem_part = {\n"
            + "".join("\t.%s = %s,\n" % (r[0], r[col]) for r in K618_IPA_PART)
            + "};\n")


K618_IPA_V3 = "dataipa/drivers/platform/msm/ipa/ipa_v3/"

EDITS += [
    # --- 1. kalama's IPA 5.5 local-memory map (see the header). Everything that addresses IPA
    # SRAM reads this table: the uC mailbox ioremap (ipa3_uc_interface_init: SRAM + uc_ofst), the
    # canaries (_ipa_init_sram_v3), header/proc-ctx/routing/filter/NAT/stats setup and commits,
    # the Q6 area zeroing at every modem power-up (ipa3_init_q6_smem from ipa3_wwan_probe), and
    # every offset QMI_IPA_INIT_MODEM_DRIVER_REQ hands the modem. All alignment checks in
    # ipa3_init_mem_partition() pass with these values (they did on 5.15).
    (K618_IPA_V3 + "ipa_utils.c",
     _k618_ipa_part(1),
     '/* Device compatibility. */\n'
     + _k618_ipa_part(2)),

    # --- 2. the modem's FnR (filter/route) stats window, QMI hw_filter_stats_info. Stock sends
    # start index 128, end 179, 832 bytes at stats_fnr_ofst (the phone's ipam.ko: mov w12, #0x340
    # and strh #0xb380 = bytes 0x80, 0xb3, in ipa3_q6_clnt_svc_arrive). 6.12's 64 + 191 counters
    # (3056 bytes) fit its own 0x1000-byte area but would run past kalama's 0xba0-byte one, into
    # stats_drop and the modem area. Only ipa_qmi_service.c uses the three macros. The AP's own FnR
    # counters stay at 1..IPA_MAX_FLT_RT_CNT_INDEX (32), well below 128.
    (K618_IPA_V3 + "ipa_i.h",
     "#define IPA_Q6_FNR_START_IDX (64)\n"
     "#define IPA_Q6_FNR_IDX_CNT (191)\n",
     "/* k618: kalama's modem FnR window, as the 5.15 driver and the phone's ipam.ko give it */\n"
     "#define IPA_Q6_FNR_START_IDX (128)\n"
     "#define IPA_Q6_FNR_IDX_CNT (52)\n"),

    # --- 3. _ipa_init_sram_v3 writes 0 and then 0xABCDEF01 into the first word of the uC
    # descriptor RAM for IPA >= 5.5 -- a marker of this release's IPA 5.5, absent from the 5.15
    # driver and from the phone's ipam.ko. With kalama's map that word is the start of the uC's own
    # 4 KB descriptor RAM; leave it to the uC, as stock does.
    (K618_IPA_V3 + "ipa.c",
     "\t\tIPA_MEM_PART(modem_hdr_proc_ctx_ofst));\n"
     "\tif (IPA_MEM_PART(uc_descriptor_ram_size) > 0)\n"
     "\t\tipa_sram_mmio[IPA_MEM_PART(uc_descriptor_ram_ofst) / 4] = 0x00000000;\n",
     "\t\tIPA_MEM_PART(modem_hdr_proc_ctx_ofst));\n"
     "\t/*\n"
     "\t * k618: no write to the first word of the uC descriptor RAM (here or at the end): kalama's\n"
     "\t * 5.15 driver never makes one, and on this phone that word belongs to the uC.\n"
     "\t */\n"),
    (K618_IPA_V3 + "ipa.c",
     "\tif (ipa_get_hw_type() >= IPA_HW_v5_5) {\n"
     "\t\tif (IPA_MEM_PART(uc_descriptor_ram_size) > 0)\n"
     "\t\t\tipa_sram_mmio[IPA_MEM_PART(uc_descriptor_ram_ofst) / 4] = 0xABCDEF01;\n"
     "\t}\n"
     "\n"
     "\tiounmap(ipa_sram_mmio);\n",
     "\t/* k618: no uC descriptor RAM marker (see above) */\n"
     "\tiounmap(ipa_sram_mmio);\n"),
]


EDITS += [
    ("dataipa/drivers/platform/msm/gsi/gsi.c",
     "\tif (ctx->state == GSI_CHAN_STATE_STOP_IN_PROC) {\n"
     "\t\tGSIERR(\"chan=%lu busy try again\\n\", chan_hdl);\n"
     "\t\tres = -GSI_STATUS_AGAIN;\n"
     "\t\tgoto free_lock;\n"
     "\t}\n"
     "\n"
     "\t/* If channel is stopped succesfully",
     "\tif (ctx->state == GSI_CHAN_STATE_STOP_IN_PROC) {\n"
     "\t\tGSIDBG(\"chan=%lu busy try again\\n\", chan_hdl);\n"
     "\t\tres = -GSI_STATUS_AGAIN;\n"
     "\t\tgoto free_lock;\n"
     "\t}\n"
     "\n"
     "\t/* If channel is stopped succesfully"),


    ("dataipa/drivers/platform/msm/ipa/ipa_v3/ipa_dp.c",
     "\t\tdelete_avail_tx_wrapper_list(ep);\n"
     "\n"
     "\t\tif (ep->sys->napi_tx_enable)\n"
     "\t\t\tnetif_napi_del(&ep->sys->napi_tx);\n",
     "\t\tdelete_avail_tx_wrapper_list(ep);\n"
     "\n"
     "\t\tif (ep->sys->napi_tx_enable) {\n"
     "\t\t\t/* k618: 6.x warns on deleting a NAPI that is still enabled */\n"
     "\t\t\tnapi_disable(&ep->sys->napi_tx);\n"
     "\t\t\tnetif_napi_del(&ep->sys->napi_tx);\n"
     "\t\t}\n"),
]


EDITS += [
    ("bt-kernel/pwr/btpower.c",
     "\tperisec_cnss_bt_hw_disable_check(pwr_data);\n"
     "\n"
     "\tif (pdev->dev.of_node) {\n"
     "\t\tret = bt_power_populate_dt_pinfo(pdev);\n",
     """	perisec_cnss_bt_hw_disable_check(pwr_data);

	/* Device compatibility. */
	if (of_property_read_bool(pdev->dev.of_node, \"qcom,converged-dt\")) {
		int k618_sw = of_get_named_gpio(pdev->dev.of_node, \"qcom,wlan-sw-ctrl-gpio\", 0);
		int k618_val = k618_sw >= 0 ? gpio_get_value(k618_sw) : -1;
		const char *k618_want = k618_val > 0 ? \"bt_qca6490\" : \"bt_kiwi\";
		struct device_node *k618_child;

		for_each_available_child_of_node(pdev->dev.of_node, k618_child) {
			if (!strcmp(k618_child->name, k618_want)) {
				pr_info(\"%s: converged DT: %s (WLAN switch control %d)\\n\",
					__func__, k618_want, k618_val);
				pdev->dev.of_node = k618_child;
				break;
			}
		}
	}

	if (pdev->dev.of_node) {
		ret = bt_power_populate_dt_pinfo(pdev);
"""),
    ("bt-kernel/pwr/btpower.c",
     "int btpower_aop_mbox_init(struct platform_pwr_data *pdata)\n"
     "{\n"
     "\tint ret = 0;\n"
     "// ret = of_find_property(pdata->pdev->dev.of_node, \"qcom,qmp\", NULL);\n"
     "\tpdata->qmp = qmp_get(&pdata->pdev->dev);\n",
     "/*\n"
     " * k618: qmp_get() looks for \"qcom,qmp\"; the 13 Pro's DT names the AOP mailbox the 5.15 way,\n"
     " * \"mboxes\" -> a qcom,qmp-mbox node whose own \"qcom,qmp\" is the AOSS node. Give our node that\n"
     " * property, pointing at the same AOSS node, before asking (once; nothing if it has one).\n"
     " */\n"
     "static void k618_qmp_alias(struct device *dev)\n"
     "{\n"
     "\tstruct device_node *np = dev->of_node, *mbox, *aoss;\n"
     "\tstruct property *p;\n"
     "\t__be32 *v;\n"
     "\n"
     "\tif (!np || of_find_property(np, \"qcom,qmp\", NULL))\n"
     "\t\treturn;\n"
     "\tmbox = of_parse_phandle(np, \"mboxes\", 0);\n"
     "\taoss = mbox ? of_parse_phandle(mbox, \"qcom,qmp\", 0) : NULL;\n"
     "\tof_node_put(mbox);\n"
     "\tif (!aoss)\n"
     "\t\treturn;\n"
     "\tp = kzalloc(sizeof(*p) + sizeof(*v), GFP_KERNEL);\n"
     "\tif (p) {\n"
     "\t\tv = (__be32 *)(p + 1);\n"
     "\t\t*v = cpu_to_be32(aoss->phandle);\n"
     "\t\tp->name = kstrdup(\"qcom,qmp\", GFP_KERNEL);\n"
     "\t\tp->value = v;\n"
     "\t\tp->length = sizeof(*v);\n"
     "\t\tif (!p->name || of_add_property(np, p)) {\n"
     "\t\t\tkfree(p->name);\n"
     "\t\t\tkfree(p);\n"
     "\t\t} else {\n"
     "\t\t\tpr_info(\"%s: qcom,qmp taken from mboxes: %pOF\\n\", __func__, aoss);\n"
     "\t\t}\n"
     "\t}\n"
     "\tof_node_put(aoss);\n"
     "}\n"
     "\n"
     "int btpower_aop_mbox_init(struct platform_pwr_data *pdata)\n"
     "{\n"
     "\tint ret = 0;\n"
     "// ret = of_find_property(pdata->pdev->dev.of_node, \"qcom,qmp\", NULL);\n"
     "\tk618_qmp_alias(&pdata->pdev->dev);\n"
     "\tpdata->qmp = qmp_get(&pdata->pdev->dev);\n"),
]


EDITS += [
    ('display-drivers/msm/dsi/dsi_panel.c',
     '\tDSI_DEBUG("backlight type:%d lvl:%d\\n", bl->type, bl_lvl);\n'
     '\tswitch (bl->type) {\n',
     '\tDSI_DEBUG("backlight type:%d lvl:%d\\n", bl->type, bl_lvl);\n'
     "\t/* k618: the 13 Pro's 1920 Hz PWM switch and the dead zone around it go before the\n"
     '\t * 51h write (stock dsi_panel_update_backlight); no 51h while qsync is on */\n'
     '\tif (bl->type == DSI_BACKLIGHT_DCS && !k618_mi_disp_bl_pre(panel, &bl_lvl)) {\n'
     '\t\tk618_mi_disp_bl(panel, bl_lvl);\n'
     '\t\treturn 0;\n'
     '\t}\n'
     '\tswitch (bl->type) {\n'),
    ('display-drivers/msm/dsi/dsi_panel.c',
     '#else /* CONFIG_PXLW_IRIS */\n'
     '\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_TIMING_SWITCH, false);\n'
     '#endif /* CONFIG_PXLW_IRIS */\n',
     '#else /* CONFIG_PXLW_IRIS */\n'
     '\tk618_mi_disp_timing_switch(panel);\t/* k618: em-cycle bytes for the current level */\n'
     '\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_TIMING_SWITCH, false);\n'
     '#endif /* CONFIG_PXLW_IRIS */\n'),
    ('display-drivers/msm/dsi/dsi_panel.c',
     '\tDSI_DEBUG("ctrl:%d qsync on\\n", ctrl_idx);\n'
     '\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_ON, false);\n',
     '\tDSI_DEBUG("ctrl:%d qsync on\\n", ctrl_idx);\n'
     '\tk618_mi_disp_qsync(panel, true);\t/* k618: em bytes for the current level */\n'
     '\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_ON, false);\n'),
    ('display-drivers/msm/dsi/dsi_panel.c',
     '\tDSI_DEBUG("ctrl:%d qsync off\\n", ctrl_idx);\n'
     '\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_OFF, false);\n',
     '\tDSI_DEBUG("ctrl:%d qsync off\\n", ctrl_idx);\n'
     '\tk618_mi_disp_qsync(panel, false);\t/* k618: em bytes + 51h = the current level */\n'
     '\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_QSYNC_OFF, false);\n'),
    ('display-drivers/msm/dsi/dsi_panel.c',
     '\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP, false);\n'
     '\tif (rc)\n'
     '\t\tDSI_ERR("[%s] failed to send DSI_CMD_SET_NOLP cmd, rc=%d\\n",\n'
     '\t\t       panel->name, rc);\n',
     '\tk618_mi_disp_pre_nolp(panel);\t/* k618: 51h of nolp = the current level */\n'
     '\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP, false);\n'
     '\tif (rc)\n'
     '\t\tDSI_ERR("[%s] failed to send DSI_CMD_SET_NOLP cmd, rc=%d\\n",\n'
     '\t\t       panel->name, rc);\n'
     '\tk618_mi_disp_post_nolp(panel, rc);\t/* k618: nolp leaves the panel in 16-pulse mode */\n'),
    ('display-drivers/msm/dsi/dsi_panel.c',
     '\tpanel->panel_initialized = false;\n'
     '\tpanel->power_mode = SDE_MODE_DPMS_OFF;\n',
     '\tpanel->panel_initialized = false;\n'
     '\tpanel->power_mode = SDE_MODE_DPMS_OFF;\n'
     '\tk618_mi_disp_panel_off(panel);\t/* k618: em-cycle / qsync / HBM state back to power-on */\n'),
]

# The 13's M3 and the 13 Ultra's M1 panels (k618_mi_disp.c)
# Two more dsi_panel.c call sites for the 13 Ultra (k618_mi_disp.c):
#  - dsi_panel_get_mode(), right after dsi_panel_parse_dsc_params(): its three DSC rate-control
#    values (stock sde_dsc_populate_dsc_config 0x117bf4 takes the panel id; 6.18's does not), set
#    before the PPS is built from the config;
#  - dsi_panel_post_switch(), under panel_lock, before DSI_CMD_SET_POST_TIMING_SWITCH: after a
#    switch to 1 Hz, the post-switch command's 26h/2Fh and its DBI set again (stock 0x163d68).
EDITS += [
    ("display-drivers/msm/dsi/dsi_panel.c",
     "\t\trc = dsi_panel_parse_dsc_params(mode, utils);\n"
     "\t\tif (rc) {\n"
     "\t\t\tDSI_ERR(\"failed to parse dsc params, rc=%d\\n\", rc);\n"
     "\t\t\tgoto parse_fail;\n"
     "\t\t}\n",
     "\t\trc = dsi_panel_parse_dsc_params(mode, utils);\n"
     "\t\tif (rc) {\n"
     "\t\t\tDSI_ERR(\"failed to parse dsc params, rc=%d\\n\", rc);\n"
     "\t\t\tgoto parse_fail;\n"
     "\t\t}\n"
     "\t\tk618_mi_disp_dsc(panel, mode);\t/* k618: the 13 Ultra's DSC rate-control values */\n"),
    ("display-drivers/msm/dsi/dsi_panel.c",
     "\tmutex_lock(&panel->panel_lock);\n"
     "\n"
     "\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_POST_TIMING_SWITCH, false);\n",
     "\tmutex_lock(&panel->panel_lock);\n"
     "\n"
     "\tk618_mi_disp_post_switch(panel);\t/* k618: the 13 Ultra's 1 Hz post-switch bytes */\n"
     "\trc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_POST_TIMING_SWITCH, false);\n"),
]


WLAN_CFG80211_C = "wlan/qcacld-3.0/core/hdd/src/wlan_hdd_cfg80211.c"
EDITS += [
    (WLAN_CFG80211_C, "static struct cfg80211_ops wlan_hdd_cfg80211_ops = {\n",
     CFG80211_WRAPPERS + "static struct cfg80211_ops wlan_hdd_cfg80211_ops = {\n"),
] + [
    (WLAN_CFG80211_C, f"\t.{op} = {old},\n", f"\t.{op} = k618_{new},\n") for op, old, new in [
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
    (WLAN_CFG80211_C, "\t.assoc_ml_reconf =\n\t\twlan_hdd_cfg80211_setup_link_reconfig,\n",
     "\t.assoc_ml_reconf = k618_assoc_ml_reconf,\n"),
]


# OnePlus merged some of their own code into these repos without a guard (sde_hw_interrupts.c's
# interrupt-storm dump calls get_eng_version(), which lives in OnePlus' own boot driver and does not
# exist here). This stub, force-included where that happens, says "an ordinary production build", so
# their debug paths stay off. Mirrors enum OPLUS_ENG_VERSION of
# vendor/oplus/kernel/boot/include/oplus_project_data_ocdt.h.
STUB_H = """/* generated by kernel-6.12/k618_tp_fix.py -- OnePlus' build-type query, stubbed */
#ifndef _K618_OPLUS_STUB_H_
#define _K618_OPLUS_STUB_H_
enum OPLUS_ENG_VERSION {
	OEM_RELEASE		= 0x00,
	AGING			= 0x01,
	CTA			= 0x02,
	PERFORMANCE		= 0x03,
	PREVERSION		= 0x04,
	ALL_NET_CMCC_TEST	= 0x05,
	ALL_NET_CMCC_FIELD	= 0x06,
	ALL_NET_CU_TEST		= 0x07,
	ALL_NET_CU_FIELD	= 0x08,
	ALL_NET_CT_TEST		= 0x09,
	ALL_NET_CT_FIELD	= 0x0A,
	HIGH_TEMP_AGING		= 0x0B,
	FACTORY			= 0x0C,
	CE			= 0x0D,
	PTCRB			= 0x0E,
};
static inline unsigned int get_eng_version(void)
{
	return OEM_RELEASE;
}
#endif /* _K618_OPLUS_STUB_H_ */
"""

# the video driver's Makefile writes this one before calling make; we call make straight away, so it
# is written here instead -- with the fixed values this build uses everywhere else
VIDEO_GEN_H = """/* generated by kernel-6.12/k618_tp_fix.py */
#define VIDEO_COMPILE_TIME "build time"
#define VIDEO_COMPILE_BY "build-user"
#define VIDEO_COMPILE_HOST "build-host"
"""


NFC_TRACE_H = """/* generated by kernel-6.12/k618_tp_fix.py: progress marks for the NFC probe on a test boot */
#ifndef _K618_NFC_TRACE_H_
#define _K618_NFC_TRACE_H_
#include <linux/module.h>
#include <linux/printk.h>
#ifdef K618_NFC_TRACE
unsigned int k618_probe_flush(unsigned int spin_ms);
#define K618_NFC_STEP(s) do {							\\
	unsigned int (*k618_f)(unsigned int) = symbol_get(k618_probe_flush);	\\
	pr_emerg("k618 nfc: %s\\n", s);						\\
	if (k618_f) {								\\
		k618_f(300);							\\
		symbol_put(k618_probe_flush);					\\
	}									\\
} while (0)
#else
#define K618_NFC_STEP(s) do { } while (0)
#endif
#endif /* _K618_NFC_TRACE_H_ */
"""

_K618_DIR = os.path.dirname(os.path.abspath(__file__))
GENERATED = [("k618_oplus_stub.h", STUB_H),

             ("display-drivers/msm/mi_disp_k618.c",
              open(os.path.join(_K618_DIR, "k618_mi_disp.c"), encoding="utf-8").read()),
             ("display-drivers/msm/mi_disp_k618.h",
              open(os.path.join(_K618_DIR, "k618_mi_disp.h"), encoding="utf-8").read()),
             ("k618_nfc_trace.h", NFC_TRACE_H),
             ("video-driver-515/driver/vidc/inc/video_generated_h", VIDEO_GEN_H)]

# the same change in every source of a repo: (repo, old, new, why)
SWEEPS = [


    ("wlan/qca-wifi-host-cmn", "destroy_timer_on_stack(", "timer_destroy_on_stack("),
    # skb_flow_get_ports() takes the header pointer and length __skb_flow_get_ports() took (6.17
    # folded the two); NULL, 0 is what the three-argument form passed
    ("wlan/qca-wifi-host-cmn", "skb_flow_get_ports(skb, flow->control.thoff,\n\t\t\t\t  flow->basic.ip_proto);",
     "skb_flow_get_ports(skb, flow->control.thoff,\n\t\t\t\t  flow->basic.ip_proto, NULL, 0);"),
    # 6.18 has no drm_device.struct_mutex (dropped upstream with the legacy GEM locking). The
    # display driver uses it as one lock around its own GEM objects' creation and release, so one
    # mutex of the driver's own takes its place -- the same mutual exclusion, since there is one
    # drm_device here (declared in msm_drv.h, defined in msm_drv.c, both below in EDITS).
    ("display-drivers", "&obj->dev->struct_mutex", "&k618_gem_lock"),
    ("display-drivers", "&dev->struct_mutex", "&k618_gem_lock"),
    ("display-drivers", "&display->drm_dev->struct_mutex", "&k618_gem_lock"),
    ("display-drivers", "&dsi_ctrl->drm_dev->struct_mutex", "&k618_gem_lock"),
    ("display-drivers", "&reg_dma[dpu_idx]->drm_dev->struct_mutex", "&k618_gem_lock"),
    # struct snd_soc_dai's rate / channels / sample_bits are symmetric_rate / symmetric_channels /
    # symmetric_sample_bits in 6.18. They are still the DAI's live parameters, not a symmetry-only
    # copy: soc_pcm_set_dai_params() writes params_rate(params) into symmetric_rate for every DAI
    # right after hw_params and clears it when the DAI goes inactive -- exactly where 6.12 wrote
    # dai->rate. The Bluetooth codec reads it in prepare()/shutdown(), after hw_params, so it sees
    # the same value. (The same two reads exist in btfmcodec and soundwire, which kalama does not
    # build; the sweep covers them for the day they are turned on.)
    ("bt-kernel", "dai->rate", "dai->symmetric_rate"),
    # 6.18 has no pfn_t at all (upstream deleted the type and its header) and vmf_insert_mixed()
    # takes a plain pfn -- which is all this call ever carried (PFN_DEV meant nothing for it)
    ("display-drivers", "#include <linux/pfn_t.h>\n", ""),
    ("display-drivers", "vmf_insert_mixed(vma, vmf->address, __pfn_to_pfn_t(pfn, PFN_DEV));",
     "vmf_insert_mixed(vma, vmf->address, pfn);"),
    # drm_get_format_info() takes the pixel format and the modifier since 6.15, not the whole
    # drm_mode_fb_cmd2 (upstream 6.15 "drm: Pass format and modifier to drm_get_format_info")
    ("display-drivers", "drm_get_format_info(dev, &mode_cmd);",
     "drm_get_format_info(dev, mode_cmd.pixel_format, mode_cmd.modifier[0]);"),
    ("display-drivers", "drm_get_format_info(dev,\n\t\t\t\t\t\t\t\tmode_cmd);",
     "drm_get_format_info(dev, mode_cmd->pixel_format,\n\t\t\t\t\t\t\tmode_cmd->modifier[0]);"),
    # ...and the same change gave drm_mode_config_funcs.fb_create and
    # drm_helper_mode_fill_fb_struct() the format info as an argument. The driver works it out for
    # itself either way, so fb_create's new argument is accepted and ignored.
    ("display-drivers",
     "struct drm_framebuffer *msm_framebuffer_create(struct drm_device *dev,\n"
     "\t\tstruct drm_file *file, const struct drm_mode_fb_cmd2 *mode_cmd)",
     "struct drm_framebuffer *msm_framebuffer_create(struct drm_device *dev,\n"
     "\t\tstruct drm_file *file, const struct drm_format_info *k618_info,\n"
     "\t\tconst struct drm_mode_fb_cmd2 *mode_cmd)"),
    ("display-drivers",
     "struct drm_framebuffer *msm_framebuffer_create(struct drm_device *dev,\n"
     "\t\tstruct drm_file *file, const struct drm_mode_fb_cmd2 *mode_cmd);",
     "struct drm_framebuffer *msm_framebuffer_create(struct drm_device *dev,\n"
     "\t\tstruct drm_file *file, const struct drm_format_info *k618_info,\n"
     "\t\tconst struct drm_mode_fb_cmd2 *mode_cmd);"),
    ("display-drivers", "drm_helper_mode_fill_fb_struct(dev, fb, mode_cmd);",
     "drm_helper_mode_fill_fb_struct(dev, fb, info, mode_cmd);"),
    # mipi_dsi_dcs_set_tear_off() is gone from 6.18's DSI helpers (only the _multi form is left);
    # it was one DCS command, which is what the panel gets here -- the helper's own body
    ("display-drivers", "rc = mipi_dsi_dcs_set_tear_off(dsi);",
     "/* k618: 6.18 has only mipi_dsi_dcs_set_tear_off_multi(); this is what the helper did\n"
     "\t * (MIPI_DCS_SET_TEAR_OFF = 0x34, include/video/mipi_display.h)\n\t */\n"
     "\trc = mipi_dsi_dcs_write(dsi, 0x34, NULL, 0);\n"
     "\tif (rc > 0)\n\t\trc = 0;"),
    # dma_fence_ops lost its two debug printers (fence_value_str / timeline_value_str) in 6.16:
    # nothing in the kernel called them any more. The driver's own two functions stay (its fence
    # dump prints the values itself) and the fence dump of a foreign fence prints no timeline.
    ("display-drivers", "\t.fence_value_str = sde_fence_value_str,\n"
                        "\t.timeline_value_str = sde_fence_timeline_value_str,\n", ""),
    ("display-drivers", "\tif (fence->ops->timeline_value_str)\n"
                        "\t\tfence->ops->timeline_value_str(fence, timeline_str, TIMELINE_VAL_LENGTH);",
     "\t/* k618: dma_fence_ops has no timeline_value_str since 6.16 */\n"
     "\ttimeline_str[0] = '\\0';"),
    ("display-drivers", "\tif (fence->ops->timeline_value_str)\n"
                        "\t\tfence->ops->timeline_value_str(fence,\n"
                        "\t\ttimeline_str, TIMELINE_VAL_LENGTH);",
     "\t/* k618: dma_fence_ops has no timeline_value_str since 6.16 */\n"
     "\ttimeline_str[0] = '\\0';"),
    # a drm_connector_helper_funcs.mode_valid takes the mode as const since 6.15. Only the display
    # driver's own connector helper has to change: the vendor's sde_connector_ops.mode_valid is a
    # different callback (it also takes the display and its resource caps) and keeps its signature,
    # so the mode is handed on to it with the const dropped again, which is what it did before.
    ("display-drivers",
     "sde_connector_mode_valid(struct drm_connector *connector,\n\t\tstruct drm_display_mode *mode)",
     "sde_connector_mode_valid(struct drm_connector *connector,\n\t\tconst struct drm_display_mode *mode)"),
    ("display-drivers",
     "\t\treturn c_conn->ops.mode_valid(connector, mode, c_conn->display,",
     "\t\treturn c_conn->ops.mode_valid(connector, (struct drm_display_mode *)mode,\n\t\t\t\tc_conn->display,"),
    # 6.16's drm_bridge_funcs.attach is told which encoder it is being attached to. All four of the
    # display driver's bridges (DSI, DP, DP MST, HDMI) ignore it -- they take their encoder from
    # their own *_drm_bridge_init() -- so the parameter is only added to their signatures.
    ("display-drivers", "_bridge_attach(struct drm_bridge *bridge,\n\t\t\tenum drm_bridge_attach_flags flags)",
     "_bridge_attach(struct drm_bridge *bridge, struct drm_encoder *encoder,\n"
     "\t\t\tenum drm_bridge_attach_flags flags)"),
    ("display-drivers", "_bridge_attach(struct drm_bridge *dp_bridge,\n\t\t\t\tenum drm_bridge_attach_flags flags)",
     "_bridge_attach(struct drm_bridge *dp_bridge, struct drm_encoder *encoder,\n"
     "\t\t\t\tenum drm_bridge_attach_flags flags)"),
    ("display-drivers", "_bridge_attach(struct drm_bridge *hdmi_bridge,\n\t\t\t\tenum drm_bridge_attach_flags flags)",
     "_bridge_attach(struct drm_bridge *hdmi_bridge, struct drm_encoder *encoder,\n"
     "\t\t\t\tenum drm_bridge_attach_flags flags)"),
    # strlcpy went away in 6.8; strscpy takes its place everywhere
    ("video-driver-515", "strlcpy(", "strscpy("),
    ("eva-kernel-515", "strlcpy(", "strscpy("),
    # struct dma_buf_map became struct iosys_map in 5.18
    ("eva-kernel-515", "struct dma_buf_map ", "struct iosys_map "),
    # the same two Qualcomm 5.15 extensions, answered as their own 6.12 drivers answer them
    ("eva-kernel-515", "DMA_ATTR_IOMMU_USE_UPSTREAM_HINT",
     "0UL /* k618: no such attribute since 6.2 */"),
    # struct dma_buf_map's setters became iosys_map's with it
    ("eva-kernel-515", "dma_buf_map_set_vaddr(", "iosys_map_set_vaddr("),
    # a synx session is a pointer in this release's synx API, and a fence handle is unsigned
    ("eva-kernel-515", "struct synx_session ssid;", "struct synx_session *ssid;"),
    ("eva-kernel-515", "\ts32 h_synx;\n", "\tu32 h_synx;\n"),
    # soc's own Gunyah resource manager calls carry a ghd_ prefix now (so does their 6.12 EVA)
    ("eva-kernel-515", "gh_rm_get_vmid(", "ghd_rm_get_vmid("),
]


def main(argv):
    root = argv[1].rstrip("/")
    done = 0
    for rel, text in GENERATED:
        path = os.path.join(root, rel)
        if os.path.exists(path) and open(path, encoding="utf-8").read() == text:
            continue
        open(path, "w", encoding="utf-8", newline="\n").write(text)
        print(f"k618_tp_fix: wrote {rel}")
    for repo, old, new in SWEEPS:
        n = 0
        for dp, _dn, fns in os.walk(os.path.join(root, repo)):
            for fn in fns:
                if not fn.endswith((".c", ".h")):
                    continue
                p = os.path.join(dp, fn)
                text = open(p, encoding="utf-8", errors="surrogateescape").read()
                if old not in text:
                    continue
                open(p, "w", encoding="utf-8", errors="surrogateescape",
                     newline="").write(text.replace(old, new))
                n += 1
        if n:
            print(f"k618_tp_fix: {repo}: {old} -> {new} in {n} files")
    for rel, old, new, in ((e[0], e[1], e[2]) for e in EDITS):
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            print(f"k618_tp_fix: no {rel}", file=sys.stderr)
            continue
        text = open(path, encoding="utf-8", errors="surrogateescape").read()
        # an edit that keeps its own anchor (an include added before the one it names) leaves `old`
        # in place; one whose `new` is a substring of `old` leaves `new` findable before it is made
        if old in new:
            if new in text:
                continue
        elif new and new in text and old not in text:
            continue
        if old not in text:
            print(f"k618_tp_fix: {rel}: text to change not found", file=sys.stderr)
            continue
        open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text.replace(old, new))
        print(f"k618_tp_fix: {rel}")
        done += 1
    print(f"k618_tp_fix: {done} of {len(EDITS)} edits applied (the rest were already there)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
