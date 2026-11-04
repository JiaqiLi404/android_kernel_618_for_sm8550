#!/usr/bin/env python3
'Device support and compatibility helpers.'

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import k618_fuxi_mods as fm  # noqa: E402

# ---- k618_fuxi_mods.py MODS: from Xiaomi's 5.15 tree
MODS = [
    # memlat's 5.15 SCMI client: binds protocol 0x80 ("scmi_protocol_memlat") and calls memlat's
    # cpucp_memlat_init(), which the adapter below makes exported again (the 6.12 note in MODS that
    # it is static inside memlat.c held only while memlat spoke the unified protocol)
    ("memlat_scmi", "CONFIG_QTI_HW_MEMLAT_SCMI_CLIENT", "drivers/soc/qcom/dcvs",
     ["memlat_scmi.c"], ["drivers/soc/qcom/dcvs/memlat"]),
]

# ---- k618_fuxi_mods.py SOC_MODS: soc-repo has the sources, only its registry and Kconfig miss them
# (the clients' Kconfig entries come from fuxi's drivers/soc/qcom/dcvs/Kconfig, as for every entry)
SOC_MODS = [
    # the PMU map protocol (0x86) and the C1DCVS protocol (0x87): soc's files are the 5.15 ones
    # with vendor_id "Qualcomm" -- the same message ids and layouts (SET_PMU_MAP sends u8[8][9],
    # MAX_CPUCP_EVT = 9 in both trees)
    ("pmu_vendor", "CONFIG_QTI_SCMI_PMU_PROTOCOL", "drivers/firmware/arm_scmi", ["pmu_vendor.c"], []),
    ("c1dcvs_vendor", "CONFIG_QTI_SCMI_C1DCVS_PROTOCOL", "drivers/firmware/arm_scmi",
     ["c1dcvs_vendor.c"], []),
    # their clients: pmu_scmi hands the protocol to qcom-pmu-lib (cpucp_pmu_init, exported there);
    # c1dcvs_scmi is the whole C1DCVS client (sysfs under /sys/devices/system/cpu/c1dcvs)
    ("pmu_scmi", "CONFIG_QTI_PMU_SCMI_CLIENT", "drivers/soc/qcom/dcvs", ["pmu_scmi.c"],
     ["drivers/soc/qcom/dcvs/qcom-pmu-lib"]),
    ("c1dcvs_scmi", "CONFIG_QTI_C1DCVS_SCMI_CLIENT", "drivers/soc/qcom/dcvs", ["c1dcvs_scmi.c"], []),
]

# the one comment the three protocol drivers share
PROTOCOLS_H = (
    "/*\n"
    " * k618: the SCMI core is the kernel's own (built in), so a protocol is built against the core's\n"
    " * own definitions -- drivers/firmware/arm_scmi/protocols.h, found through -I$(srctree) -- not\n"
    " * soc's 5.15-era common.h, whose struct scmi_protocol_handle has no hops (6.x put it before\n"
    " * set_priv/get_priv) and whose struct scmi_xfer predates 6.x's flags. These drivers only use\n"
    " * ph->dev, ph->xops and the xfer's hdr/tx/rx, which sit alike in both, but the core's header is\n"
    " * the one the core was compiled with.\n"
    " */\n"
    "#include \"drivers/firmware/arm_scmi/protocols.h\"\n")

# 6.18's request_module() fallback for a vendor protocol asks for "scmi-protocol-0x<id>-<vendor>"
# (driver.c, SCMI_VENDOR_MODULE_ALIAS_FMT); 6.x's in-tree vendor protocols declare that alias, the
# 5.15 ones could not. The load lists bring these modules in anyway (the protocol's client probes
# again once it is registered), so this is only the core's own way of finding them.
def alias(name, desc, proto_id_macro):
    return (f"drivers/firmware/arm_scmi/{name}", desc,
            desc + "\nMODULE_ALIAS(\"scmi-protocol-\" __stringify(" + proto_id_macro + ") \"-Qualcomm\");")


# 6.18's mailbox transport: 104 payload bytes unless the DT says arm,max-msg-size
RX_LIMIT = (
    "/*\n"
    " * k618: 6.18's mailbox transport carries at most 104 bytes of payload\n"
    " * (SCMI_SHMEM_MAX_PAYLOAD_SIZE) unless the DT gives arm,max-msg-size, which this phone's\n"
    " * qcom,scmi node does not; 5.15's allowed 128. xfer_get_init() refuses a larger rx_size with\n"
    " * -ERANGE, so every read here failed. 0 asks for the transport's own maximum, and the core\n"
    " * then trims rx.len to what the firmware actually sent.\n"
    " */\n")

# ---- k618_fuxi_mods.py EDITS: (file, old, new)
EDITS = [
    # ---------------- memlat_vendor.c (fuxi's 0x80, imported already)
    ("drivers/firmware/arm_scmi/memlat_vendor.c",
     "#include \"common.h\"\n#include <linux/scmi_memlat.h>\n",
     PROTOCOLS_H + "#include <linux/scmi_memlat.h>\n"),
    # the monitor's name went in as the format string
    ("drivers/firmware/arm_scmi/memlat_vendor.c",
     "snprintf(msg->mon_name, MAX_NAME_LEN, mon_name);",
     "snprintf(msg->mon_name, MAX_NAME_LEN, \"%s\", mon_name);"),
    # the two reads: an exact rx size (see RX_LIMIT: 128 is refused on 6.18), the reply copied only
    # after a transfer that worked, and the xfer given back on every path -- 5.15 returned
    # -EMSGSIZE with the xfer still held, which leaks one of the channel's 20 message slots per call
    ("drivers/firmware/arm_scmi/memlat_vendor.c",
     "\tret = ph->xops->xfer_get_init(ph, MEMLAT_GET_TIMESTAMP, sizeof(u32),\n"
     "\t\t\t\t      SCMI_MAX_RX_SIZE, &t);\n"
     "\tif (ret)\n\t\treturn ret;\n\n"
     "\tret = ph->xops->do_xfer(ph, t);\n"
     "\tif (t->rx.len != sizeof(u64))\n\t\treturn -EMSGSIZE;\n\n"
     "\tmemcpy(buf, t->rx.buf, t->rx.len);\n"
     "\tph->xops->xfer_put(ph, t);\n\treturn ret;\n",
     "\t/* k618: exact sizes (6.18 refuses rx 128, see c1dcvs_vendor.c), the xfer put on every path */\n"
     "\tret = ph->xops->xfer_get_init(ph, MEMLAT_GET_TIMESTAMP, sizeof(u32),\n"
     "\t\t\t\t      sizeof(u64), &t);\n"
     "\tif (ret)\n\t\treturn ret;\n\n"
     "\tret = ph->xops->do_xfer(ph, t);\n"
     "\tif (!ret && t->rx.len != sizeof(u64))\n\t\tret = -EMSGSIZE;\n"
     "\tif (!ret)\n\t\tmemcpy(buf, t->rx.buf, sizeof(u64));\n"
     "\tph->xops->xfer_put(ph, t);\n\treturn ret;\n"),
    ("drivers/firmware/arm_scmi/memlat_vendor.c",
     "\tret = ph->xops->xfer_get_init(ph, msg_id, sizeof(*msg),\n"
     "\t\t\t\t      SCMI_MAX_RX_SIZE, &t);\n"
     "\tif (ret)\n\t\treturn ret;\n\n"
     "\tmsg = t->tx.buf;\n"
     "\tmsg->hw_type = cpu_to_le32(hw_type);\n"
     "\tmsg->mon_idx = cpu_to_le32(mon_idx);\n"
     "\tret = ph->xops->do_xfer(ph, t);\n"
     "\tif (t->rx.len != sizeof(u32))\n\t\treturn -EMSGSIZE;\n\n"
     "\tmemcpy(buf, t->rx.buf, t->rx.len);\n"
     "\tph->xops->xfer_put(ph, t);\n\treturn ret;\n",
     "\tret = ph->xops->xfer_get_init(ph, msg_id, sizeof(*msg),\n"
     "\t\t\t\t      sizeof(u32), &t);\n"
     "\tif (ret)\n\t\treturn ret;\n\n"
     "\tmsg = t->tx.buf;\n"
     "\tmsg->hw_type = cpu_to_le32(hw_type);\n"
     "\tmsg->mon_idx = cpu_to_le32(mon_idx);\n"
     "\tret = ph->xops->do_xfer(ph, t);\n"
     "\tif (!ret && t->rx.len != sizeof(u32))\n\t\tret = -EMSGSIZE;\n"
     "\tif (!ret)\n\t\tmemcpy(buf, t->rx.buf, sizeof(u32));\n"
     "\tph->xops->xfer_put(ph, t);\n\treturn ret;\n"),
    # the frequency map stays the 5.15 message (u32 rows, 124 bytes): too big for 6.18's 104-byte
    # mailbox payload, so it fails with -ERANGE -- it is sent only for a "qcom,cpucp-enabled"
    # monitor, which this phone's DT does not have; raising the limit (arm,max-msg-size = <128>) is
    # the fix if one ever appears
    ("drivers/firmware/arm_scmi/memlat_vendor.c",
     "\tif (nr_rows > MAX_MAP_ENTRIES)\n\t\treturn -EINVAL;\n",
     "\t/*\n\t * k618: sizeof(*msg) is 124 bytes, more than 6.18's mailbox payload (104) -- -ERANGE\n"
     "\t * below. Only a CPUCP-run monitor sends it; this phone's DT has none.\n\t */\n"
     "\tif (nr_rows > MAX_MAP_ENTRIES)\n\t\treturn -EINVAL;\n"),
    alias("memlat_vendor.c", "MODULE_DESCRIPTION(\"SCMI memlat vendor Protocol\");", "SCMI_PROTOCOL_MEMLAT"),

    # ---------------- pmu_vendor.c (soc's 0x86): 72-byte map, 4-byte tunables -- sizes fit
    ("drivers/firmware/arm_scmi/pmu_vendor.c",
     "#include <soc/qcom/pmu_lib.h>\n#include \"common.h\"\n",
     "#include <soc/qcom/pmu_lib.h>\n" + PROTOCOLS_H),
    alias("pmu_vendor.c", "MODULE_DESCRIPTION(\"SCMI PMU vendor Protocol\");", "SCMI_PMU_PROTOCOL"),

    # ---------------- c1dcvs_vendor.c (soc's 0x87)
    ("drivers/firmware/arm_scmi/c1dcvs_vendor.c",
     "#include <linux/scmi_c1dcvs.h>\n#include \"common.h\"\n\n#define SCMI_MAX_RX_SIZE\t128\n",
     "#include <linux/scmi_c1dcvs.h>\n" + PROTOCOLS_H + "\n" + RX_LIMIT + "#define SCMI_MAX_RX_SIZE\t0\n"),
    # with rx 0 an rx.len of up to 104 bytes -- and the whole 104 after a failed transfer, which
    # 5.15 copied anyway -- would land in the sysfs callers' buffers: one unsigned int, or one per
    # CPU for the two thresholds (c1dcvs_scmi.c's show_c1dcvs_attr / show_c1dcvs_thresh)
    ("drivers/firmware/arm_scmi/c1dcvs_vendor.c",
     "\tstruct c1dcvs_thresh *msg;\n\n"
     "\tret = ph->xops->xfer_get_init(ph, msg_id, sizeof(*msg), SCMI_MAX_RX_SIZE,\n"
     "\t\t\t\t      &t);\n"
     "\tif (ret)\n\t\treturn ret;\n\n"
     "\tret = ph->xops->do_xfer(ph, t);\n"
     "\tmemcpy(buf, t->rx.buf, t->rx.len);\n",
     "\tstruct c1dcvs_thresh *msg;\n"
     "\t/* k618: what the caller's buffer holds -- one u32, or one per CPU for the thresholds */\n"
     "\tsize_t max = (msg_id == GET_IPC_THRESH || msg_id == GET_EFREQ_THRESH) ?\n"
     "\t\t     num_possible_cpus() * sizeof(u32) : sizeof(u32);\n\n"
     "\tret = ph->xops->xfer_get_init(ph, msg_id, sizeof(*msg), SCMI_MAX_RX_SIZE,\n"
     "\t\t\t\t      &t);\n"
     "\tif (ret)\n\t\treturn ret;\n\n"
     "\tret = ph->xops->do_xfer(ph, t);\n"
     "\tif (!ret)\n\t\tmemcpy(buf, t->rx.buf, min_t(size_t, t->rx.len, max));\n"),
    alias("c1dcvs_vendor.c", "MODULE_DESCRIPTION(\"SCMI C1DCVS vendor Protocol\");", "SCMI_C1DCVS_PROTOCOL"),
    # c1dcvs_scmi.c (soc's client): the two thresholds are unsigned int, printed as %lu
    ("drivers/soc/qcom/dcvs/c1dcvs_scmi.c",
     "pr_info(\"Input threshold :%lu for cluster :%lu\\n\", msg[1], msg[0]);",
     "pr_info(\"Input threshold :%u for cluster :%u\\n\", msg[1], msg[0]);"),

    # ---------------- arm_scmi/Kconfig: kalama's transport is RIMPS, not CPUCP's mailbox
    # (soc's entries depend on QCOM_CPUCP only -- which canoe's dict happens to turn on; fuxi's on
    # QCOM_RIMPS). Either satisfies them now, so neither going away takes the protocols with it.
    ("drivers/firmware/arm_scmi/Kconfig",
     "config QTI_SCMI_PMU_PROTOCOL\n"
     "\ttristate \"Qualcomm Technologies, Inc. SCMI PMU vendor Protocol\"\n"
     "\tdepends on ARM || ARM64 || COMPILE_TEST\n"
     "\tdepends on ARM_SCMI_PROTOCOL && QCOM_CPUCP\n",
     "config QTI_SCMI_PMU_PROTOCOL\n"
     "\ttristate \"Qualcomm Technologies, Inc. SCMI PMU vendor Protocol\"\n"
     "\tdepends on ARM || ARM64 || COMPILE_TEST\n"
     "\tdepends on ARM_SCMI_PROTOCOL && (QCOM_CPUCP || QCOM_RIMPS)\n"),
    ("drivers/firmware/arm_scmi/Kconfig",
     "config QTI_SCMI_C1DCVS_PROTOCOL\n"
     "\ttristate \"Qualcomm Technologies, Inc. SCMI C1DCVS vendor Protocol\"\n"
     "\tdepends on ARM || ARM64 || COMPILE_TEST\n"
     "\tdepends on ARM_SCMI_PROTOCOL && QCOM_CPUCP\n",
     "config QTI_SCMI_C1DCVS_PROTOCOL\n"
     "\ttristate \"Qualcomm Technologies, Inc. SCMI C1DCVS vendor Protocol\"\n"
     "\tdepends on ARM || ARM64 || COMPILE_TEST\n"
     "\tdepends on ARM_SCMI_PROTOCOL && (QCOM_CPUCP || QCOM_RIMPS)\n"),

    # ---------------- pmu_lib.c: its old path (!CONFIG_QTI_SCMI_VENDOR_PROTOCOL) as it is, except
    # that pmu_scmi's probe before qcom-pmu's own was a permanent -EINVAL. On the phone's order it
    # cannot happen (qcom-pmu probes at qcom-pmu-lib's load, 71st; the SCMI device for 0x86 exists
    # only once qcom_rimps, 96th, lets arm-scmi probe), but a reordered list must not lose the map.
    ("drivers/soc/qcom/dcvs/pmu_lib.c",
     "\tif (!sdev || !sdev->handle || !pmu_base)\n\t\treturn -EINVAL;\n",
     "\tif (!sdev || !sdev->handle)\n\t\treturn -EINVAL;\n"
     "\t/* k618: qcom-pmu not probed yet (no shared page mapped): try again, do not give up */\n"
     "\tif (!pmu_base)\n\t\treturn qcom_pmu_inited ? -ENODEV : -EPROBE_DEFER;\n"),

    # ---------------- memlat.c: the 5.15 protocol behind the driver's unified-protocol calls
    ("drivers/soc/qcom/dcvs/memlat.c",
     "#if IS_ENABLED(CONFIG_QTI_SCMI_VENDOR_PROTOCOL)\nstatic int configure_cpucp_common_events(void)\n",
     "#ifdef K618_MEMLAT_CPUCP\n@ADAPTER@static int configure_cpucp_common_events(void)\n"),
    ("drivers/soc/qcom/dcvs/memlat.c",
     "static int cpucp_memlat_init(struct scmi_device *sdev)\n{\n",
     "#ifdef K618_MEMLAT_KALAMA\nint cpucp_memlat_init(struct scmi_device *sdev);\n"
     "int cpucp_memlat_init(struct scmi_device *sdev)\n#else\n"
     "static int cpucp_memlat_init(struct scmi_device *sdev)\n#endif\n{\n"),
    ("drivers/soc/qcom/dcvs/memlat.c",
     "\tops = sdev->handle->devm_protocol_get(sdev, QCOM_SCMI_VENDOR_PROTOCOL, &ph);\n",
     "#ifdef K618_MEMLAT_KALAMA\n"
     "\t/*\n\t * k618: called from memlat_scmi's SCMI probe, which can come before the last monitor:\n"
     "\t * defer until all are in (memlat_mon_probe calls it only then on the unified path)\n\t */\n"
     "\tif (!memlat_data || !memlat_data->inited)\n\t\treturn -EPROBE_DEFER;\n"
     "\tops = k618_memlat_get_ops(sdev, &ph);\n#else\n"
     "\tops = sdev->handle->devm_protocol_get(sdev, QCOM_SCMI_VENDOR_PROTOCOL, &ph);\n#endif\n"),
    ("drivers/soc/qcom/dcvs/memlat.c",
     "\t\tmemlat_data->ops = NULL;\n\tmutex_unlock(&memlat_lock);\n\treturn ret;\n}\n#endif\n#define INST_EV",
     "\t\tmemlat_data->ops = NULL;\n\tmutex_unlock(&memlat_lock);\n\treturn ret;\n}\n"
     "#ifdef K618_MEMLAT_KALAMA\nEXPORT_SYMBOL_GPL(cpucp_memlat_init);\n#endif\n#endif\n#define INST_EV"),
]

# which protocol memlat.c speaks; before its first use of anything CPUCP
K618_MEMLAT_CFG = """/*
 * k618: which CPUCP protocol this driver speaks. The unified Qualcomm vendor protocol
 * (CONFIG_QTI_SCMI_VENDOR_PROTOCOL), as OnePlus built it; or, without it, kalama's 5.15
 * memory-latency protocol (CONFIG_QTI_SCMI_MEMLAT_PROTOCOL: memlat_vendor.ko, with memlat_scmi.ko
 * as its client) through the translation before configure_cpucp_common_events(); with neither,
 * HLOS sampling only.
 */
#if IS_ENABLED(CONFIG_QTI_SCMI_VENDOR_PROTOCOL)
#define K618_MEMLAT_CPUCP
#elif IS_ENABLED(CONFIG_QTI_SCMI_MEMLAT_PROTOCOL)
#define K618_MEMLAT_CPUCP
#define K618_MEMLAT_KALAMA
#include <linux/scmi_memlat.h>
#endif
"""

K618_MEMLAT_ADAPTER = """#ifdef K618_MEMLAT_KALAMA
/*
 * k618: kalama's CPUCP firmware -- the 13 Pro's -- has the 5.15 memory-latency protocol on SCMI
 * protocol 0x80, the id this driver's unified Qualcomm vendor protocol uses as well. The unified
 * one sends each parameter in a SET_PARAM / GET_PARAM / START_ACTIVITY / STOP_ACTIVITY message
 * (0x10-0x13: algorithm string, parameter id, payload); kalama's has one message per parameter,
 * and its message ids are this driver's parameter ids (MEMLAT_SET_LOG_LEVEL = 3 ...
 * MEMLAT_GET_TIMESTAMP = 38, the same enum in both trees). Sent as it is, SET_PARAM would reach
 * this firmware as MEMLAT_SET_MEM_GROUP (16) with the algorithm string for a CPU mask. So the
 * driver's calls stay and land here, and each parameter goes to the matching call of
 * memlat_vendor.ko, Xiaomi's 5.15 protocol driver, which builds the 5.15 message.
 *
 * On this phone no monitor is "qcom,cpucp-enabled": CPUCP gets the common event map and the
 * sample period at init, and the debug nodes (cpucp_log_level, flush_cpucp_log,
 * hlos_cpucp_offset) work again -- what the 5.15 kernel does with it.
 */
static const struct scmi_memlat_vendor_ops *k618_memlat_ops;

/*
 * 5.15's map has u32 rows of CPU MHz and memory kHz -- struct cpufreq_memfreq_map as it is -- where
 * this driver's message has u16 rows in MHz: hand the monitor's own table over, as 5.15 did.
 */
static int k618_memlat_freq_map(const struct scmi_protocol_handle *ph,
				const struct map_param_msg *m)
{
	struct memlat_group *grp;
	struct memlat_mon *mon;

	if (m->hw_type >= MAX_MEMLAT_GRPS)
		return -EINVAL;
	grp = memlat_data->groups[m->hw_type];
	if (!grp || m->mon_idx >= grp->num_inited_mons)
		return -EINVAL;
	mon = &grp->mons[m->mon_idx];
	return k618_memlat_ops->freq_map(ph, grp->hw_type, mon->index,
					 mon->freq_map_len, mon->freq_map);
}

#define K618_NEED(type) do {				\\
	if (!buf || size < sizeof(type))		\\
		return -EINVAL;				\\
} while (0)

static int k618_memlat_set_param(const struct scmi_protocol_handle *ph, void *buf,
				 u64 algo_str, u32 param_id, size_t size)
{
	const struct scmi_memlat_vendor_ops *o = k618_memlat_ops;
	const struct scalar_param_msg *s = buf;
	const struct node_msg *n = buf;
	const struct ev_map_msg *e = buf;

	if (!o || algo_str != MEMLAT_ALGO_STR)
		return -EINVAL;

	switch (param_id) {
	case MEMLAT_SET_LOG_LEVEL:
		K618_NEED(u32);
		return o->set_log_level(ph, *(const u32 *)buf);
	case MEMLAT_FLUSH_LOGBUF:
		return o->flush_cpucp_log(ph);
	case MEMLAT_SAMPLE_MS:
		K618_NEED(u32);
		return o->sample_ms(ph, *(const u32 *)buf);
	case MEMLAT_SET_MEM_GROUP:
		K618_NEED(*n);
		return o->set_mem_grp(ph, n->cpumask, n->hw_type);
	case MEMLAT_SET_MONITOR:
		K618_NEED(*n);
		return o->set_mon(ph, n->cpumask, n->hw_type, n->mon_type, n->mon_idx,
				  n->mon_name);
	case MEMLAT_SET_COMMON_EV_MAP:
		K618_NEED(*e);
		return o->set_common_ev_map(ph, (void *)e->cid, e->num_evs);
	case MEMLAT_SET_GRP_EV_MAP:
		K618_NEED(*e);
		return o->set_grp_ev_map(ph, e->hw_type, (void *)e->cid, e->num_evs);
	case MEMLAT_MON_FREQ_MAP:
		K618_NEED(struct map_param_msg);
		return k618_memlat_freq_map(ph, buf);
	}

	K618_NEED(*s);
	switch (param_id) {
	case MEMLAT_ADAPTIVE_LOW_FREQ:
		return o->adaptive_low_freq(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_ADAPTIVE_HIGH_FREQ:
		return o->adaptive_high_freq(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_IPM_CEIL:
		return o->ipm_ceil(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_FE_STALL_FLOOR:
		return o->fe_stall_floor(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_BE_STALL_FLOOR:
		return o->be_stall_floor(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_WB_PCT:
		return o->wb_pct_thres(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_IPM_FILTER:
		return o->wb_filter_ipm(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_FREQ_SCALE_PCT:
		return o->freq_scale_pct(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_FREQ_SCALE_CEIL_MHZ:
		return o->freq_scale_ceil_mhz(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_FREQ_SCALE_FLOOR_MHZ:
		return o->freq_scale_floor_mhz(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_SET_MIN_FREQ:
		return o->min_freq(ph, s->hw_type, s->mon_idx, s->val);
	case MEMLAT_SET_MAX_FREQ:
		return o->max_freq(ph, s->hw_type, s->mon_idx, s->val);
	}
	/*
	 * newer than this firmware (MEMLAT_SET_EFFECTIVE_FREQ_METHOD, MEMLAT_ADAPTIVE_LEVEL_1,
	 * MEMLAT_SET_SUBSAMPLING_ENABLED): kalama's protocol has no such message
	 */
	return -EOPNOTSUPP;
}

static int k618_memlat_get_param(const struct scmi_protocol_handle *ph, void *buf,
				 u64 algo_str, u32 param_id, size_t tx_size, size_t rx_size)
{
	const struct scmi_memlat_vendor_ops *o = k618_memlat_ops;
	const struct scalar_param_msg *s = buf;
	u32 val;
	int ret;

	if (!o || !buf || algo_str != MEMLAT_ALGO_STR)
		return -EINVAL;

	switch (param_id) {
	case MEMLAT_GET_TIMESTAMP:
		if (rx_size < sizeof(u64))
			return -EINVAL;
		return o->get_timestamp(ph, buf);
	case MEMLAT_GET_CUR_FREQ:
	case MEMLAT_GET_ADAPTIVE_CUR_FREQ:
		if (tx_size < sizeof(*s) || rx_size < sizeof(val))
			return -EINVAL;
		if (param_id == MEMLAT_GET_CUR_FREQ)
			ret = o->get_cur_freq(ph, s->hw_type, s->mon_idx, &val);
		else
			ret = o->get_adaptive_cur_freq(ph, s->hw_type, s->mon_idx, &val);
		/* the reply goes where the request was, as the unified get_param leaves it */
		if (!ret)
			memcpy(buf, &val, sizeof(val));
		return ret;
	}
	return -EOPNOTSUPP;
}

static int k618_memlat_start(const struct scmi_protocol_handle *ph, void *buf, u64 algo_str,
			     u32 param_id, size_t size)
{
	if (!k618_memlat_ops || algo_str != MEMLAT_ALGO_STR || param_id != MEMLAT_START_TIMER)
		return -EINVAL;
	return k618_memlat_ops->start_timer(ph);
}

static int k618_memlat_stop(const struct scmi_protocol_handle *ph, void *buf, u64 algo_str,
			    u32 param_id, size_t size)
{
	if (!k618_memlat_ops || algo_str != MEMLAT_ALGO_STR || param_id != MEMLAT_STOP_TIMER)
		return -EINVAL;
	return k618_memlat_ops->stop_timer(ph);
}

static const struct qcom_scmi_vendor_ops k618_memlat_vendor_ops = {
	.set_param	= k618_memlat_set_param,
	.get_param	= k618_memlat_get_param,
	.start_activity	= k618_memlat_start,
	.stop_activity	= k618_memlat_stop,
};

/* kalama's protocol 0x80 through memlat_vendor's ops, presented as the unified protocol's */
static const struct qcom_scmi_vendor_ops *k618_memlat_get_ops(struct scmi_device *sdev,
							      struct scmi_protocol_handle **ph)
{
	const struct scmi_memlat_vendor_ops *ops;

	ops = sdev->handle->devm_protocol_get(sdev, SCMI_PROTOCOL_MEMLAT, ph);
	if (IS_ERR(ops))
		return ERR_CAST(ops);
	k618_memlat_ops = ops;
	return &k618_memlat_vendor_ops;
}
#endif /* K618_MEMLAT_KALAMA */

"""

EDITS = [(f, o, n.replace("@ADAPTER@", K618_MEMLAT_ADAPTER)) for f, o, n in EDITS]

# ---- k618_fuxi_mods.py INSERTS: (file, anchor, text) -- text goes before the anchor
INSERTS = [
    ("drivers/soc/qcom/dcvs/memlat.c", "#include <linux/cpu_phys_log_map.h>\n", K618_MEMLAT_CFG),
]

# ---- k618_overrides.bzl
OVERRIDES = [
    ("CONFIG_QTI_SCMI_VENDOR_PROTOCOL", "n",
     "kalama's CPUCP firmware speaks the 5.15 per-algorithm SCMI protocols (memory latency 0x80,\n"
     "PMU map 0x86, C1DCVS 0x87 -- k618_fuxi_mods.py), and the unified Qualcomm vendor protocol 6.x's\n"
     "drivers use claims 0x80 too: with it off qcom-pmu-lib and memlat take the per-protocol path,\n"
     "and qcom_scmi_client (which only that protocol needs) is not built"),
    ("CONFIG_QTI_QCOM_SCMI_CLIENT", "n", None),
    ("CONFIG_QTI_SCMI_PMU_PROTOCOL", "m", "the protocols and their clients, as the phone's 5.15 kernel loads them"),
    ("CONFIG_QTI_SCMI_C1DCVS_PROTOCOL", "m", None),
    ("CONFIG_QTI_PMU_SCMI_CLIENT", "m", None),
    ("CONFIG_QTI_C1DCVS_SCMI_CLIENT", "m", None),
    ("CONFIG_QTI_HW_MEMLAT_SCMI_CLIENT", "m", None),
]

# ---- module lists (names as the phone's lists have them; k618_bootimg.sh then puts each where the
# phone's own modules.load does)
MORE = ["pmu_vendor", "pmu_scmi", "c1dcvs_vendor", "c1dcvs_scmi"]	# k618_more.txt (first stage)
STAGE2 = ["memlat_scmi"]						# k618_stage2.txt


def overrides(src, dst):
    """k618_overrides.bzl with OVERRIDES before its closing brace (keys already there are left)"""
    text = open(src, encoding="utf-8").read()
    add = ""
    for key, val, why in OVERRIDES:
        if f'"{key}"' in text:
            continue
        if why:
            add += "".join(f"    # {line}\n" for line in why.split("\n"))
        add += f'    "{key}": "{val}",\n'
    i = text.rindex("}")
    open(dst, "w", encoding="utf-8", newline="\n").write(text[:i] + add + text[i:])
    print(f"k618_cpucp_scmi_port: {dst}: {sum(1 for k, _v, _w in OVERRIDES if k not in text)} options added")
    return 0


def main(argv):
    if len(argv) == 4 and argv[1] == "overrides":
        return overrides(argv[2], argv[3])
    if len(argv) != 4:
        print(__doc__)
        return 2
    fm.MODS += MODS
    fm.SOC_MODS += SOC_MODS
    fm.EDITS += EDITS
    fm.INSERTS += INSERTS
    return fm.main(argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
