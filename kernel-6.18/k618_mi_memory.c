// SPDX-License-Identifier: GPL-2.0
/*
 * mi_memory (built as mi_memory.ko from k618_mi_memory.c): the 13 Pro's own 5.15 mi_memory.ko
 * ("Interface for xiaomi memory", Venco <duwenchao@xiaomi.com>, vendor_boot) rewritten for 6.18
 * from its disassembly and the older public version of the same driver (MiCode zeus-s-oss,
 * drivers/misc/mi-memory). Same paths and output formats:
 *
 * /sys/class/mi_memory/mi_memory_device/ufshcd0/      (/sys/devices/virtual/mi_memory/...)
 *     dump_device_desc         the 35 device-descriptor fields, one
 *                              "Device Descriptor[Byte offset 0x..]: <name> = 0x..\n" line each
 *     dump_health_desc         the 5 health-descriptor fields, same line format
 *     dump_string_desc_serial  "serial:" + the serial number's UTF-16 code units in hex + "\n"
 *     show_hba                 host state, the standard UFS event counters and stock's 23 UIC
 *                              error counters (counted here, see mm_uic_update)
 *     err_state, err_reason    Xiaomi's UFS error record, kept by Xiaomi's 5.15 ufs_qcom, which this
 *                              port's ufs-qcom does not have: "0\n" and "" (what a phone with no
 *                              recorded error shows)
 * /sys/class/mi_memory/mi_memory_device/ddr/          ddr_type, ddr_size, ddr_id, ddr_vendor
 *                              (the bootloader's /memory properties)
 * /proc/mv                     "D: 0x<ddr id> <ddr GB>\nU: 0x<ufs vendor> <ufs GB> <product> <rev>\n"
 *
 * Readers on this ROM: PowerKeeper and system_perf_init (health), DeviceInfoQR and vendor MI_IC
 * (serial), MiuiCit (factory test), dumpstate (show_hba, err_*, health), hwinfo / MI_RIC (/proc/mv),
 * vendor ffu (only when vendor.ffu.is.start=1 is set). mimd does not read any of it.
 *
 * Left out on purpose:
 *  - ufshcd0/hr, the vendor "health report": stock enters each UFS vendor's private command mode
 *    (vendor-specific SCSI commands with per-vendor keys) to read it. Not reproduced blind on
 *    the phone's boot storage; readers get "no such file".
 * 6.18 calls only exported, stable UFS core functions (ufshcd_read_desc_param, the runtime-PM
 * helpers of <ufs/ufshcd.h>) and the core's android_vh_ufs_check_int_errors hook; the host is found
 * by its SCSI template name, "ufshcd".
 */
#define pr_fmt(fmt) "mi_memory: " fmt

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/tracepoint.h>
#include <linux/unaligned.h>
#include <scsi/scsi_host.h>
#include <trace/hooks/ufshcd.h>
#include <ufs/ufs.h>
#include <ufs/ufshcd.h>

#define MI_MEMORY_MODULE	"mi_memory_module"
#define MI_MEMORY_DEVICE	"mi_memory_device"
#define MI_MEMORY_CLASS		"mi_memory"
#define MEMORYDEV_MINOR		1

/* ------------------------------------------------------------------------------------------- */
/* the UFS host                                                                                 */

static DEFINE_MUTEX(mm_lock);
static struct Scsi_Host *mm_shost;	/* held from the first lookup to module exit */

static struct ufs_hba *mm_hba(void)
{
	struct ufs_hba *hba = NULL;
	unsigned int i;

	mutex_lock(&mm_lock);
	/* stock took SCSI host 0 blindly; look for the UFS host among the first few */
	for (i = 0; !mm_shost && i < 8; i++) {
		struct Scsi_Host *sh = scsi_host_lookup(i);

		if (!sh)
			continue;
		if (sh->hostt && sh->hostt->proc_name && !strcmp(sh->hostt->proc_name, UFSHCD))
			mm_shost = sh;
		else
			scsi_host_put(sh);
	}
	if (mm_shost)
		hba = shost_priv(mm_shost);
	mutex_unlock(&mm_lock);
	return hba;
}

/*
 * The 23 UIC error counters at the end of stock's show_hba. Qualcomm's 5.15 ufs_qcom kept them in its
 * ufs_qcom_host (at +108, updated in its copy of ufshcd_check_errors): one count per set bit of the
 * PA error code (lanes 0-3, line reset) and of the DL error code (bits 0-14), the totals counting
 * the same bits, and one per DME error. 6.18's host driver keeps only the core's event history
 * (the last 8 register values of each kind and how many there were), so the counts are taken from
 * it: at every error interrupt (the core calls android_vh_ufs_check_int_errors, under the host
 * lock, right after ufshcd_update_uic_error recorded the registers), at load for the errors before,
 * and at each show_hba read. An interrupt adds at most one entry of each kind, so nothing is missed
 * while the hook runs.
 */
static struct {
	u32 pa_total;
	u32 pa[5];
	u32 dl_total;
	u32 dl[15];
	u32 dme;
} uic;
static const enum ufs_event_type uic_evts[] = { UFS_EVT_PA_ERR, UFS_EVT_DL_ERR, UFS_EVT_DME_ERR };
static unsigned long long uic_seen[ARRAY_SIZE(uic_evts)];	/* history entries counted */
static struct ufs_hba *uic_hba;
static bool uic_hooked;

static void mm_uic_count(enum ufs_event_type evt, u32 val)
{
	unsigned long bits;
	int b;

	switch (evt) {
	case UFS_EVT_PA_ERR:
		bits = val & UIC_PHY_ADAPTER_LAYER_ERROR_CODE_MASK;
		for_each_set_bit(b, &bits, ARRAY_SIZE(uic.pa)) {
			uic.pa[b]++;
			uic.pa_total++;
		}
		break;
	case UFS_EVT_DL_ERR:
		bits = val & UIC_DATA_LINK_LAYER_ERROR_CODE_MASK;
		for_each_set_bit(b, &bits, ARRAY_SIZE(uic.dl)) {
			uic.dl[b]++;
			uic.dl_total++;
		}
		break;
	default:
		uic.dme++;
		break;
	}
}

/* under the host lock: the history entries recorded since the last call */
static void mm_uic_update(struct ufs_hba *hba)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(uic_evts); i++) {
		struct ufs_event_hist *e = &hba->ufs_stats.event[uic_evts[i]];
		unsigned long long n = e->cnt - uic_seen[i];
		int k;

		/* the core never clears the history; if it did, its entries are new */
		if (e->cnt < uic_seen[i])
			n = e->cnt;
		/* more than the history holds only if the hook did not run (not registered) */
		if (n > UFS_EVENT_HIST_LENGTH)
			n = UFS_EVENT_HIST_LENGTH;
		for (k = n; k > 0; k--)
			mm_uic_count(uic_evts[i], e->val[(e->pos + UFS_EVENT_HIST_LENGTH - k) %
							 UFS_EVENT_HIST_LENGTH]);
		uic_seen[i] = e->cnt;
	}
}

static void mm_check_int_errors(void *data, struct ufs_hba *hba, bool queue_eh_work)
{
	if (hba == READ_ONCE(uic_hba))
		mm_uic_update(hba);
}

static void mm_uic_start(void)
{
	struct ufs_hba *hba = mm_hba();
	unsigned long flags;
	int ret;

	if (!hba) {
		pr_err("%s: no UFS host, the UIC error counters stay 0\n", __func__);
		return;
	}
	spin_lock_irqsave(hba->host->host_lock, flags);
	mm_uic_update(hba);
	WRITE_ONCE(uic_hba, hba);
	spin_unlock_irqrestore(hba->host->host_lock, flags);
	ret = register_trace_android_vh_ufs_check_int_errors(mm_check_int_errors, NULL);
	if (ret)
		pr_err("%s: check_int_errors hook not registered (%d), counted at each read only\n",
		       __func__, ret);
	uic_hooked = !ret;
}

/* stock: the UFS device W-LU resumed around every query (ufshcd_rpm_get_sync/put_sync) */
static int mm_read_desc(struct ufs_hba *hba, enum desc_idn id, int index, u8 offset,
			u8 *buf, u8 size)
{
	int ret;

	if (!hba->ufs_device_wlun)
		return -ENODEV;
	ufshcd_rpm_get_sync(hba);
	ret = ufshcd_read_desc_param(hba, id, index, offset, buf, size);
	ufshcd_rpm_put_sync(hba);
	return ret;
}

/* ------------------------------------------------------------------------------------------- */
/* ufshcd0/                                                                                     */

struct mm_field {
	const char *name;
	u8 offset;
	u8 width;	/* bytes, big-endian on the wire */
};

/* stock's table, in its order (the 5.15 module's .rodata) */
static const struct mm_field device_desc_fields[] = {
	{ "bLength", 0x00, 1 },			{ "bDescriptorType", 0x01, 1 },
	{ "bDevice", 0x02, 1 },			{ "bDeviceClass", 0x03, 1 },
	{ "bDeviceSubClass", 0x04, 1 },		{ "bProtocol", 0x05, 1 },
	{ "bNumberLU", 0x06, 1 },		{ "bNumberWLU", 0x07, 1 },
	{ "bBootEnable", 0x08, 1 },		{ "bDescrAccessEn", 0x09, 1 },
	{ "bInitPowerMode", 0x0a, 1 },		{ "bHighPriorityLUN", 0x0b, 1 },
	{ "bSecureRemovalType", 0x0c, 1 },	{ "bSecurityLU", 0x0d, 1 },
	{ "Reserved", 0x0e, 1 },		{ "bInitActiveICCLevel", 0x0f, 1 },
	{ "wSpecVersion", 0x10, 2 },		{ "wManufactureDate", 0x12, 2 },
	{ "iManufactureName", 0x14, 1 },	{ "iProductName", 0x15, 1 },
	{ "iSerialNumber", 0x16, 1 },		{ "iOemID", 0x17, 1 },
	{ "wManufactureID", 0x18, 2 },		{ "bUD0BaseOffset", 0x1a, 1 },
	{ "bUDConfigPLength", 0x1b, 1 },	{ "bDeviceRTTCap", 0x1c, 1 },
	{ "wPeriodicRTCUpdate", 0x1d, 2 },	{ "bUFSFeaturesSupport", 0x1f, 1 },
	{ "bFFUTimeout", 0x20, 1 },		{ "bQueueDepth", 0x21, 1 },
	{ "wDeviceVersion", 0x22, 2 },		{ "bNumSecureWpArea", 0x24, 1 },
	{ "dPSAMaxDataSize", 0x25, 4 },		{ "bPSAStateTimeout", 0x29, 1 },
	{ "iProductRevisionLevel", 0x2a, 1 },
};

static const struct mm_field health_desc_fields[] = {
	{ "bLength", HEALTH_DESC_PARAM_LEN, 1 },
	{ "bDescriptorType", HEALTH_DESC_PARAM_TYPE, 1 },
	{ "bPreEOLInfo", HEALTH_DESC_PARAM_EOL_INFO, 1 },
	{ "bDeviceLifeTimeEstA", HEALTH_DESC_PARAM_LIFE_TIME_EST_A, 1 },
	{ "bDeviceLifeTimeEstB", HEALTH_DESC_PARAM_LIFE_TIME_EST_B, 1 },
};

static ssize_t mm_dump_desc(char *buf, enum desc_idn id, const struct mm_field *f, int n,
			    const char *nomem, const char *who)
{
	struct ufs_hba *hba = mm_hba();
	int i, ret, len = 0;
	u8 *desc;

	if (!hba)
		return -ENODEV;
	desc = kzalloc(QUERY_DESC_MAX_SIZE, GFP_KERNEL);
	if (!desc)
		return sysfs_emit(buf, "%s", nomem);
	ret = mm_read_desc(hba, id, 0, 0, desc, QUERY_DESC_MAX_SIZE);
	if (ret) {
		dev_err(hba->dev, "%s: Failed reading %s, ret %d\n", __func__, who, ret);
		kfree(desc);
		return 0;		/* stock: an empty read */
	}
	for (i = 0; i < n; i++) {
		const u8 *p = desc + f[i].offset;
		u32 v;

		switch (f[i].width) {
		case 2:
			v = get_unaligned_be16(p);
			break;
		case 4:
			v = get_unaligned_be32(p);
			break;
		default:
			v = *p;
			break;
		}
		len += sysfs_emit_at(buf, len, "Device Descriptor[Byte offset 0x%x]: %s = 0x%x\n",
				     f[i].offset, f[i].name, v);
	}
	kfree(desc);
	return len;
}

static ssize_t dump_device_desc_show(struct device *dev, struct device_attribute *attr,
				     char *buf)
{
	return mm_dump_desc(buf, QUERY_DESC_IDN_DEVICE, device_desc_fields,
			    ARRAY_SIZE(device_desc_fields), "get desc info fail\n",
			    "dump_device_desc_show");
}

static ssize_t dump_health_desc_show(struct device *dev, struct device_attribute *attr,
				     char *buf)
{
	return mm_dump_desc(buf, QUERY_DESC_IDN_HEALTH, health_desc_fields,
			    ARRAY_SIZE(health_desc_fields), "get health info fail\n",
			    "dump_health_desc_show");
}

/* the string descriptor named by the device descriptor's byte at pname (stock ufs_get_string_desc) */
static u8 *mm_read_string_desc(struct ufs_hba *hba, u8 pname)
{
	u8 index = 0;
	u8 *uc;

	if (mm_read_desc(hba, QUERY_DESC_IDN_DEVICE, 0, pname, &index, 1))
		return NULL;
	uc = kzalloc(QUERY_DESC_MAX_SIZE, GFP_KERNEL);
	if (!uc)
		return NULL;
	if (mm_read_desc(hba, QUERY_DESC_IDN_STRING, index, 0, uc, QUERY_DESC_MAX_SIZE)) {
		dev_err(hba->dev, "Reading String Desc failed\n");
		kfree(uc);
		return NULL;
	}
	return uc;	/* uc[0] length (header included), uc[1] type, then UTF-16BE */
}

/* ASCII form, non-printable characters as spaces, at most size-1 characters */
static void mm_string_ascii(struct ufs_hba *hba, u8 pname, char *out, size_t size)
{
	u8 *uc = mm_read_string_desc(hba, pname);
	size_t i, n = 0;

	if (uc) {
		for (i = 2; i + 1 < uc[0] && n + 1 < size; i += 2) {
			u16 ch = get_unaligned_be16(uc + i);

			out[n++] = (ch >= 0x20 && ch <= 0x7e) ? ch : ' ';
		}
		kfree(uc);
	}
	out[n] = '\0';
}

static ssize_t dump_string_desc_serial_show(struct device *dev, struct device_attribute *attr,
					    char *buf)
{
	struct ufs_hba *hba = mm_hba();
	int i, len;
	u8 *uc;

	if (!hba)
		return -ENODEV;
	uc = mm_read_string_desc(hba, DEVICE_DESC_PARAM_SN);
	len = sysfs_emit(buf, "serial:");
	/* stock: every UTF-16 code unit as 4 hex digits (the serial may be binary) */
	for (i = 2; uc && i + 1 < uc[0]; i += 2)
		len += sysfs_emit_at(buf, len, "%02x%02x", uc[i], uc[i + 1]);
	len += sysfs_emit_at(buf, len, "\n");
	kfree(uc);
	return len;
}

/* stock's names of the DL error code bits 0-14, in bit order ("dll_" and the last one sic) */
static const char * const dl_names[15] = {
	"dl_nac_received_err_cnt",		"dl_tcx_replay_timer_expired_err_cnt",
	"dl_afcx_request_timer_expired_err_cnt",	"dl_fcx_protection_timer_expired_err_cnt",
	"dl_crc_err_cnt",			"dll_rx_buffer_overflow_err_cnt",
	"dl_max_frame_length_exceeded_err_cnt",	"dl_wrong_sequence_number_err_cnt",
	"dl_afc_frame_syntax_err_cnt",		"dl_nac_frame_syntax_err_cnt",
	"dl_eof_syntax_err_cnt",		"dl_frame_syntax_err_cnt",
	"dl_bad_ctrl_symbol_type_err_cnt",	"dl_pa_init_err_cnt",
	"dl_pa_error_ind_received",
};

static ssize_t show_hba_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static const struct {
		const char *fmt;
		enum ufs_event_type evt;
	} evts[] = {
		{ "ufs_event_pa_error_cnt = 0x%x\n", UFS_EVT_PA_ERR },
		{ "ufs_event_dl_error_cnt = 0x%x\n", UFS_EVT_DL_ERR },
		{ "ufs_event_nl_error_cnt = 0x%x\n", UFS_EVT_NL_ERR },
		{ "ufs_event_tl_error_cnt = 0x%x\n", UFS_EVT_TL_ERR },
		{ "ufs_event_dme_error_cnt = 0x%x\n", UFS_EVT_DME_ERR },
		{ "ufs_event_auto_hibern8_cnt = 0x%x\n", UFS_EVT_AUTO_HIBERN8_ERR },
		{ "ufs_event_fatal_error_cnt= 0x%x\n", UFS_EVT_FATAL_ERR },	/* sic, stock */
		{ "ufs_event_link_startup_fail_cnt = 0x%x\n", UFS_EVT_LINK_STARTUP_FAIL },
		{ "ufs_event_resume_error_cnt = 0x%x\n", UFS_EVT_RESUME_ERR },
		{ "ufs_event_suspend_error_cnt = 0x%x\n", UFS_EVT_SUSPEND_ERR },
		{ "ufs_event_device_reset_cnt = 0x%x\n", UFS_EVT_DEV_RESET },
		{ "ufs_event_host_reset_cnt = 0x%x\n", UFS_EVT_HOST_RESET },
		{ "ufs_event_abort_cnt = 0x%x\n", UFS_EVT_ABORT },
	};
	struct ufs_hba *hba = mm_hba();
	unsigned long flags;
	int i, n = 0;

	if (!hba)
		return -ENODEV;
	if (hba == READ_ONCE(uic_hba)) {
		spin_lock_irqsave(hba->host->host_lock, flags);
		mm_uic_update(hba);
		spin_unlock_irqrestore(hba->host->host_lock, flags);
	}
	/* a snapshot without the host lock, as stock */
	n += sysfs_emit_at(buf, n, "hba->outstanding_tasks = 0x%x\n", (u32)hba->outstanding_tasks);
	n += sysfs_emit_at(buf, n, "hba->outstanding_reqs = 0x%x\n", (u32)hba->outstanding_reqs);
	n += sysfs_emit_at(buf, n, "hba->capabilities = 0x%x\n", hba->capabilities);
	n += sysfs_emit_at(buf, n, "hba->nutrs = %d\n", hba->nutrs);
	n += sysfs_emit_at(buf, n, "hba->nutmrs = %d\n", hba->nutmrs);
	n += sysfs_emit_at(buf, n, "hba->ufs_version = 0x%x\n", hba->ufs_version);
	n += sysfs_emit_at(buf, n, "hba->irq = 0x%x\n", hba->irq);
	n += sysfs_emit_at(buf, n, "hba->auto_bkops_enabled = %d\n", hba->auto_bkops_enabled);
	n += sysfs_emit_at(buf, n, "hba->ufshcd_state = 0x%x\n", hba->ufshcd_state);
	n += sysfs_emit_at(buf, n, "hba->clk_gating.state = 0x%x\n", hba->clk_gating.state);
	n += sysfs_emit_at(buf, n, "hba->eh_flags = 0x%x\n", hba->eh_flags);
	n += sysfs_emit_at(buf, n, "hba->intr_mask = 0x%x\n", hba->intr_mask);
	n += sysfs_emit_at(buf, n, "hba->ee_ctrl_mask = 0x%x\n", hba->ee_ctrl_mask);
	n += sysfs_emit_at(buf, n, "hba->errors = 0x%x\n", hba->errors);
	n += sysfs_emit_at(buf, n, "hba->uic_error = 0x%x\n", hba->uic_error);
	n += sysfs_emit_at(buf, n, "hba->saved_err = 0x%x\n", hba->saved_err);
	n += sysfs_emit_at(buf, n, "hba->saved_uic_err = 0x%x\n", hba->saved_uic_err);
	n += sysfs_emit_at(buf, n, "hibern8_exit_cnt = %d\n", hba->ufs_stats.hibern8_exit_cnt);
	for (i = 0; i < ARRAY_SIZE(evts); i++)
		n += sysfs_emit_at(buf, n, evts[i].fmt, (u32)hba->ufs_stats.event[evts[i].evt].cnt);
	n += sysfs_emit_at(buf, n, "pa_err_cnt_total = %d\n", uic.pa_total);
	for (i = 0; i < 4; i++)
		n += sysfs_emit_at(buf, n, "pa_lane_%d_err_cnt = %d\n", i, uic.pa[i]);
	n += sysfs_emit_at(buf, n, "pa_line_reset_err_cnt = %d\n", uic.pa[4]);
	n += sysfs_emit_at(buf, n, "dl_err_cnt_total = %d\n", uic.dl_total);
	for (i = 0; i < ARRAY_SIZE(dl_names); i++)
		n += sysfs_emit_at(buf, n, "%s = %d\n", dl_names[i], uic.dl[i]);
	n += sysfs_emit_at(buf, n, "dme_err_cnt = %d\n", uic.dme);
	return n;
}

static ssize_t err_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", 0);
}

static ssize_t err_reason_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return 0;
}

static DEVICE_ATTR_RO(dump_health_desc);
static DEVICE_ATTR_RO(dump_string_desc_serial);
static DEVICE_ATTR_RO(dump_device_desc);
static DEVICE_ATTR_RO(show_hba);
static DEVICE_ATTR_RO(err_state);
static DEVICE_ATTR_RO(err_reason);

static struct attribute *ufshcd_sysfs[] = {
	&dev_attr_dump_health_desc.attr,
	&dev_attr_dump_string_desc_serial.attr,
	&dev_attr_dump_device_desc.attr,
	&dev_attr_show_hba.attr,
	&dev_attr_err_state.attr,
	&dev_attr_err_reason.attr,
	NULL,
};

static const struct attribute_group ufshcd_sysfs_group = {
	.name = "ufshcd0",
	.attrs = ufshcd_sysfs,
};

/* ------------------------------------------------------------------------------------------- */
/* ddr/                                                                                         */

static struct {
	bool read;
	u32 type, size, id;
} ddr;

static void mm_ddr_info(void)
{
	struct device_node *np;

	mutex_lock(&mm_lock);
	if (!ddr.read) {
		np = of_find_node_by_path("/memory");
		if (np) {
			of_property_read_u32(np, "ddr_device_type", &ddr.type);
			of_property_read_u32(np, "ddr_size", &ddr.size);
			of_property_read_u32(np, "ddr_manufacturer_id", &ddr.id);
			of_node_put(np);
		}
		ddr.read = true;
	}
	mutex_unlock(&mm_lock);
}

static ssize_t ddr_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	mm_ddr_info();
	return sysfs_emit(buf, "0x%0x\n", ddr.type);
}

static ssize_t ddr_size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	mm_ddr_info();
	return sysfs_emit(buf, "%d\n", (u8)ddr.size);
}

static ssize_t ddr_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	mm_ddr_info();
	return sysfs_emit(buf, "0x%02x\n", (u8)ddr.id);
}

/* stock's table (JEDEC LPDDR manufacturer ids), everything else UNKNOWN */
static const char *mm_ddr_vendor(u8 id)
{
	switch (id) {
	case 0x01: return "SAMSUNG";
	case 0x03: return "ELPIDA";
	case 0x05: return "NANYA";
	case 0x06: return "SKHYNIX";
	case 0x0e: return "INTEL";
	case 0x13: return "CXMT";
	case 0xff: return "MICRON";
	default:   return "UNKNOWN";
	}
}

static ssize_t ddr_vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	mm_ddr_info();
	return sysfs_emit(buf, "%s\n", mm_ddr_vendor((u8)ddr.id));
}

static DEVICE_ATTR_RO(ddr_size);
static DEVICE_ATTR_RO(ddr_id);
static DEVICE_ATTR_RO(ddr_vendor);
static DEVICE_ATTR_RO(ddr_type);

static struct attribute *dram_sysfs[] = {
	&dev_attr_ddr_size.attr,
	&dev_attr_ddr_id.attr,
	&dev_attr_ddr_vendor.attr,
	&dev_attr_ddr_type.attr,
	NULL,
};

static const struct attribute_group dram_sysfs_group = {
	.name = "ddr",
	.attrs = dram_sysfs,
};

static const struct attribute_group *memory_sysfs_groups[] = {
	&ufshcd_sysfs_group,
	&dram_sysfs_group,
	NULL,
};

/* ------------------------------------------------------------------------------------------- */
/* /proc/mv                                                                                     */

static struct {
	bool read;
	u16 manfid;
	int gb;
	char name[17];
	char rev[5];
} mv;

/* stock's rounding of the raw capacity, quirks included (above 1 TB gives 512, 9-16 GB gives 8) */
static int mm_ufs_gb(u64 raw_512b_units)
{
	u64 gb = raw_512b_units >> 21;

	if (gb >= 0x201 && gb <= 0x400)
		return 1024;
	if (gb >= 0x101)
		return 512;
	if (gb >= 0x81)
		return 256;
	if (gb >= 0x41)
		return 128;
	if (gb >= 0x21)
		return 64;
	if (gb >= 0x11)
		return 32;
	if (gb >= 0x9)
		return 8;
	pr_info("mv unkonwn ufs size %llu\n", gb);
	return 0;
}

static void mm_mv_info(struct ufs_hba *hba)
{
	u8 b[8];

	mutex_lock(&mm_lock);
	if (mv.read) {
		mutex_unlock(&mm_lock);
		return;
	}
	mutex_unlock(&mm_lock);

	/* the queries sleep: outside the lock, the result stored once */
	{
		char name[17], rev[5];
		u16 manfid = 0;
		u64 cap = 0;

		mm_string_ascii(hba, DEVICE_DESC_PARAM_PRDCT_NAME, name, sizeof(name));
		mm_string_ascii(hba, DEVICE_DESC_PARAM_PRDCT_REV, rev, sizeof(rev));
		if (!mm_read_desc(hba, QUERY_DESC_IDN_DEVICE, 0, DEVICE_DESC_PARAM_MANF_ID, b, 2))
			manfid = get_unaligned_be16(b);
		if (!mm_read_desc(hba, QUERY_DESC_IDN_GEOMETRY, 0, GEOMETRY_DESC_PARAM_DEV_CAP, b, 8))
			cap = get_unaligned_be64(b);

		mutex_lock(&mm_lock);
		if (!mv.read) {
			strscpy(mv.name, name, sizeof(mv.name));
			strscpy(mv.rev, rev, sizeof(mv.rev));
			mv.manfid = manfid;
			mv.gb = mm_ufs_gb(cap);
			mv.read = true;
		}
		mutex_unlock(&mm_lock);
	}
}

static int mv_proc_show(struct seq_file *m, void *v)
{
	struct ufs_hba *hba = mm_hba();

	mm_ddr_info();
	if (hba)
		mm_mv_info(hba);
	seq_printf(m, "D: 0x%02x %d\n", (u8)ddr.id, (u8)ddr.size);
	seq_printf(m, "U: 0x%04x %d %s %s\n", mv.manfid, mv.gb, mv.name, mv.rev);
	return 0;
}

static int mv_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mv_proc_show, NULL);
}

static const struct proc_ops mv_proc_fops = {
	.proc_open	= mv_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ------------------------------------------------------------------------------------------- */

static const struct file_operations mem_ops = {
	.owner = THIS_MODULE,
};

static struct class *mem_class;
static struct device *mem_dev;
static int mem_major;
static bool mv_made;

static int __init mi_memory_init(void)
{
	int ret;

	mem_class = class_create(MI_MEMORY_CLASS);
	if (IS_ERR(mem_class)) {
		ret = PTR_ERR(mem_class);
		pr_err("%s: mi memory info class creation failed (err = %d)\n", __func__, ret);
		return ret;
	}
	/* stock: a char device with no operations, so that the device has a number */
	mem_major = register_chrdev(0, MI_MEMORY_MODULE, &mem_ops);
	if (mem_major < 0) {
		ret = mem_major;
		pr_err("%s: mi memory info chrdev creation failed (err = %d)\n", __func__, ret);
		goto class_destroy;
	}
	mem_dev = device_create(mem_class, NULL, MKDEV(mem_major, MEMORYDEV_MINOR), NULL,
				MI_MEMORY_DEVICE);
	if (IS_ERR(mem_dev)) {
		ret = -ENODEV;
		pr_err("%s: mi memory info device creation failed (err = %d)\n", __func__, ret);
		goto chrdev_unreg;
	}
	ret = sysfs_create_groups(&mem_dev->kobj, memory_sysfs_groups);
	if (ret) {
		pr_err("%s: sysfs groups creation failed (err = %d)\n", __func__, ret);
		goto device_destroy;
	}
	mv_made = proc_create("mv", 0555, NULL, &mv_proc_fops) != NULL;	/* stock's mode */
	if (!mv_made)
		pr_err("%s: /proc/mv creation failed\n", __func__);
	mm_uic_start();
	return 0;

device_destroy:
	device_destroy(mem_class, MKDEV(mem_major, MEMORYDEV_MINOR));
chrdev_unreg:
	unregister_chrdev(mem_major, MI_MEMORY_MODULE);
class_destroy:
	class_destroy(mem_class);
	return ret;
}

static void __exit mi_memory_exit(void)
{
	if (uic_hooked) {
		unregister_trace_android_vh_ufs_check_int_errors(mm_check_int_errors, NULL);
		tracepoint_synchronize_unregister();
	}
	if (mv_made)
		remove_proc_entry("mv", NULL);
	sysfs_remove_groups(&mem_dev->kobj, memory_sysfs_groups);
	device_destroy(mem_class, MKDEV(mem_major, MEMORYDEV_MINOR));
	unregister_chrdev(mem_major, MI_MEMORY_MODULE);
	class_destroy(mem_class);
	if (mm_shost)
		scsi_host_put(mm_shost);
}

module_init(mi_memory_init);
module_exit(mi_memory_exit);

MODULE_DESCRIPTION("Interface for xiaomi memory (k618 rewrite for 6.18)");
MODULE_LICENSE("GPL");
