#!/usr/bin/env python3
"""k618_rpc515_fix.py <staging dir>: the 5.15 -> 6.18 edits Xiaomi's adsprpc needs, and the small
compat header that goes beside it. Each entry says what changed in the kernel; idempotent. Driven by
k618_rpc515.sh, which stages the sources and builds them."""
import io
import os
import re
import sys

D = sys.argv[1]

COMPAT = """/* k618: the 5.15 calls this driver uses that 6.x renamed or dropped */
#ifndef K618_RPC_COMPAT_H
#define K618_RPC_COMPAT_H
#include <linux/firmware/qcom/qcom_scm.h>

/*
 * hyp_assign_phys() was Qualcomm's wrapper around the SCM memory-assign call; 6.x has the call
 * itself, taking the source list as a bitmap and the destinations as {vmid, perm} pairs.
 */
static inline int k618_hyp_assign_phys(phys_addr_t addr, u64 size, u32 *source_vm_list,
		int source_nelems, int *dest_vmids, int *dest_perms, int dest_nelems)
{
	struct qcom_scm_vmperm dst[8];
	u64 src = 0;
	int i;

	if (dest_nelems > (int)ARRAY_SIZE(dst))
		return -EINVAL;
	for (i = 0; i < source_nelems; i++)
		src |= BIT_ULL(source_vm_list[i]);
	for (i = 0; i < dest_nelems; i++) {
		dst[i].vmid = dest_vmids[i];
		dst[i].perm = dest_perms[i];
	}
	return qcom_scm_assign_mem(addr, size, &src, dst, dest_nelems);
}
#define hyp_assign_phys(a, s, svl, sn, dv, dp, dn) \\
	k618_hyp_assign_phys((a), (s), (svl), (sn), (dv), (dp), (dn))

/*
 * Qualcomm's own DMA attributes, which 5.15 carried in the core dma-mapping.h and 6.x does not.
 * The driver passes this one for the buffers it wants kept in the system cache only; without the
 * attribute the mapping is simply the ordinary cached one.
 */
#ifndef DMA_ATTR_SYS_CACHE_ONLY
#define DMA_ATTR_SYS_CACHE_ONLY		0
#endif

/*
 * iommu_dma_enable_best_fit_algo() was Qualcomm's 5.15 addition to the DMA-IOMMU allocator (pick
 * the tightest free IOVA range instead of the first). 6.x has no such knob, and the allocator's
 * own choice is what every other driver here lives with.
 */
static inline int k618_iommu_best_fit_noop(struct device *dev)
{
	return 0;
}
#define iommu_dma_enable_best_fit_algo(dev) k618_iommu_best_fit_noop(dev)

/* Device compatibility. */
#include <linux/ratelimit.h>
static DEFINE_RATELIMIT_STATE(k618_rpc_rs, 5 * HZ, 10);

#endif /* K618_RPC_COMPAT_H */
"""

EDITS = [
    # linux/dma-iommu.h is gone in 6.x: its helpers moved into dma-mapping and the iommu core
    ("adsprpc.c", "#include <linux/dma-iommu.h>\n",
     "/* k618: gone in 6.x, folded into dma-mapping */\n"),
    # the compat header above, next to the driver's own
    ("adsprpc.c", '#include "adsprpc_shared.h"\n',
     '#include "adsprpc_shared.h"\n#include "k618_rpc_compat.h"\n'),
    # 5.15 got platform_device through of_platform.h; 6.x does not include it any more
    ("adsprpc.c", "#include <linux/of_platform.h>\n",
     "#include <linux/of_platform.h>\n#include <linux/platform_device.h>\n"),
    # iommu_map_sg() takes the allocation flags since 6.3
    ("adsprpc.c", "err = iommu_map_sg(domain, frpc_gen_addr_pool[0], sgt.sgl,\n"
     "\t\t\t\t\tsgt.nents, IOMMU_READ | IOMMU_WRITE);\n",
     "err = iommu_map_sg(domain, frpc_gen_addr_pool[0], sgt.sgl,\n"
     "\t\t\t\t\tsgt.nents, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);\n"),
    # the header declares it with the enum type, the definition uses unsigned int
    ("adsprpc.c", "long fastrpc_driver_invoke(struct fastrpc_device *dev, unsigned int invoke_num,",
     "long fastrpc_driver_invoke(struct fastrpc_device *dev,\n"
     "\tenum fastrpc_driver_invoke_nums invoke_num,"),
    # bus_type.match() takes a const driver since 6.11
    ("adsprpc.c", "static int fastrpc_bus_match(struct device *dev, struct device_driver *driver)",
     "static int fastrpc_bus_match(struct device *dev, const struct device_driver *driver)"),
    # class_create() lost its module argument in 6.4
    ("adsprpc.c", 'me->class = class_create(THIS_MODULE, "fastrpc");',
     'me->class = class_create("fastrpc");'),
    # the dma-buf calls live in a namespace a module has to name since 5.16
    ("adsprpc.c", 'MODULE_LICENSE("GPL v2");',
     'MODULE_IMPORT_NS("DMA_BUF");\nMODULE_DESCRIPTION("Qualcomm FastRPC (5.15 interface) for 6.18");\n'
     'MODULE_LICENSE("GPL v2");'),
    # cma_get_base()/cma_get_size() are not exported to modules in GKI. They are read here only to
    # hyp-assign the ADSP ION heap's CMA range, and this phone's devicetree has no "qcom,msm-ion"
    # node at all (ION went with 5.4), so the lookup above never finds one and the range stays 0.
    ("adsprpc.c", "\t\t\t\tcma = dev_get_cma_area(&ion_pdev->dev);\n"
     "\t\t\t\tif (cma) {\n"
     "\t\t\t\t\trange.addr = cma_get_base(cma);\n"
     "\t\t\t\t\trange.size = (size_t)cma_get_size(cma);\n"
     "\t\t\t\t}\n",
     "\t\t\t\t/* k618: no ION node on this phone, and GKI exports neither helper */\n"),
    ("adsprpc.c", "\tstruct platform_device *ion_pdev;\n\tstruct cma *cma;\n",
     "\tstruct platform_device *ion_pdev;\n"),

    ("adsprpc.c", "\tif (err == -EUSERS) {\n\t\tADSPRPC_WARN(\n"
     "\t\t\t\"max concurrent sessions limit (%d) already reached on %s err %d\\n\",",
     "\tif (err == -EUSERS && __ratelimit(&k618_rpc_rs)) {\n\t\tADSPRPC_WARN(\n"
     "\t\t\t\"max concurrent sessions limit (%d) already reached on %s err %d\\n\","),
    ("adsprpc.c", "\t\t\tif (err) {\n\t\t\t\tADSPRPC_ERR(\n"
     "\t\t\t\t\t\"fastrpc_session_alloc failed for fd %d ret %d\\n\",",
     "\t\t\tif (err) {\n\t\t\t\tif (__ratelimit(&k618_rpc_rs))\n\t\t\t\tADSPRPC_ERR(\n"
     "\t\t\t\t\t\"fastrpc_session_alloc failed for fd %d ret %d\\n\","),
]

# The driver defines the three functions its own header declares under CONFIG_MSM_ADSPRPC; that
# config is not set in our build, so the header took its "not built" branch and gave them static
# inline bodies. Build the module as that config.
KBUILD_DEFS = ["CONFIG_MSM_ADSPRPC"]


def edit(rel, old, new):
    p = os.path.join(D, rel)
    s = io.open(p, encoding="utf-8", errors="surrogateescape", newline="").read()
    if new in s and old not in s:
        return "already there"
    if s.count(old) != 1:
        return f"FOUND {s.count(old)} TIMES"
    io.open(p, "w", encoding="utf-8", errors="surrogateescape", newline="").write(
        s.replace(old, new))
    return "applied"


io.open(os.path.join(D, "k618_rpc_compat.h"), "w", encoding="utf-8", newline="\n").write(COMPAT)
for rel, old, new in EDITS:
    r = edit(rel, old, new)
    print(f"rpc_edits: {rel}: {r}")
    if r.startswith("FOUND"):
        sys.exit(f"rpc_edits: {rel}: an edit no longer matches -- the source moved")

# 6.18's __assign_str() takes the field name only; the source string comes from __string()
p = os.path.join(D, "include/trace/events/fastrpc.h")
s = io.open(p, encoding="utf-8", newline="").read()
n = len(re.findall(r"__assign_str\(\s*\w+\s*,", s))
s = re.sub(r"__assign_str\(\s*(\w+)\s*,[^;]*?\)", r"__assign_str(\1)", s)
io.open(p, "w", encoding="utf-8", newline="").write(s)
print(f"rpc_edits: trace header: {n} __assign_str calls cut to one argument")
