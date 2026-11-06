// SPDX-License-Identifier: GPL-2.0
/*
 * ufs-qcom-mi.c: the sysfs nodes of Xiaomi's ufs-qcom that this ROM reads, on the GKI's UFS core.
 * k618_ufs.py copies this file next to ufs-qcom.c and includes it there (one translation unit), so
 * the build lists stay as they are. Reverse-engineered from the stock modules: the 18 Pro Max's
 * 6.18 ufs-qcom.ko (it carries its own copy of the UFS core) and the 13 Pro's 5.15 ufs_qcom.ko.
 *
 * <ufshc>/ufs_monitor/ (all devices): command latency statistics, host side only.
 *   monitor_enable                  0/1; init.rc writes it on persist.sys.stability.iostat;
 *                                   turning it off clears every statistic
 *   monitor_latency_{read,write}_threshold_us
 *                                   5 comma-separated thresholds (one per size class, default
 *                                   100000); only while the monitor is off
 *   monitor_window_period_s         window length (default 60); only while the monitor is on
 *   monitor_daily_cmd_lat           since the last "reset": per size class sum / count / peak, the
 *                                   commands above the threshold, a latency histogram (hypsys)
 *   monitor_window_cmd_lat          the current and the previous window (system_server's ANR
 *                                   report); "reset" clears them
 *   READ(6/10/16) and WRITE(6/10/16) are counted, from the send to the completion hook.
 *
 * <ufshc>/ufscld/ (KIOXIA UFS 4.0 / 3.1 parts only, as stock): the vendor's defragmentation. The
 * 13 Pro's vendor CLD HAL reads frag_level, writes trigger (miui-services' MiuiCldService asks it)
 * and waits for the CLD_TRIGGER=OFF uevent; dumpstate reads frag_level, hid_state and
 * hid_available_size. Device commands, exactly the stock ones for these parts:
 *   THGJFJ (Gen11): flag 0x13 set / clear / read, attribute 0x34 (level) and 0x35 (state) read
 *   THGJFL (Gen12): flag 0xa0, attributes 0xa0 and 0xa1, likewise
 *   hid_state: attribute 0x39 read (the JEDEC HID state on UFS 4.1; refused by these parts)
 * Not sent: stock's hid_available_size first writes attribute 0x35 = 1 (JEDEC bDefragOperation,
 * analysis) on any non-YMTC device; on these KIOXIA parts 0x35 is the vendor's defrag state, so
 * the write is only made where the device reports JEDEC HID (dev_info.hid_sup, UFS 4.1), and
 * otherwise the node shows 1, stock's value when that write is refused. Other vendors' CLD
 * (Samsung, SK hynix, Micron, WDC, YMTC) and the UFS 4.1 JEDEC HID path are not ported: no part
 * of the 13 series that could be checked uses them; their devices get no ufscld directory.
 * ufs_qcom.k618_cld=0 leaves ufscld out, ufs_qcom.k618_ufs_monitor=0 ufs_monitor.
 *
 * Not here: stock's ufseua (eua_support, eua_support_size, eua_file_lba_unmap). Stock creates it
 * only for 11 part numbers (none of them this phone's KIOXIA THGJFJT1E45BATPC) and 5.15 had no
 * such node, so vold / fs_mgr / update_engine find no EUA path, as on stock.
 */

/*
 * ufs-qcom.c has CREATE_TRACE_POINTS defined at this point: only headers it has already pulled
 * in (and so no new trace event header) may be included here; the vendor hooks come from its
 * <trace/hooks/ufshcd.h>
 */
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/pm_runtime.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>

static bool k618_mon_param = true;
module_param_named(k618_ufs_monitor, k618_mon_param, bool, 0444);
MODULE_PARM_DESC(k618_ufs_monitor, "create ufs_monitor (command latency statistics)");
static bool k618_cld_param = true;
module_param_named(k618_cld, k618_cld_param, bool, 0444);
MODULE_PARM_DESC(k618_cld, "create ufscld (KIOXIA defragmentation) on the parts stock supports");

/* ---------------------------------------------------------------- ufs_monitor ---- */

#define K618_MON_RW	2	/* read, write */
#define K618_MON_SIZES	5	/* <=16K, <=128K, <=256K, <=512K, larger */
#define K618_MON_DELAYS	5	/* <1, <5, <10, <50 ms, longer */

struct k618_mon_stat {
	u64 sum;	/* ns */
	u64 max;	/* ns */
	u32 cnt;
};

struct k618_mon {
	struct kobject kobj;
	struct mutex sysfs_lock;
	spinlock_t lock;
	bool added;
	bool enabled;
	u64 window_ns;
	u64 thr_us[K618_MON_RW][K618_MON_SIZES];
	u32 win_idx;
	u64 win_start;
	struct k618_mon_stat win[2][K618_MON_RW][K618_MON_SIZES];
	struct k618_mon_stat win_slow[2][K618_MON_RW][K618_MON_SIZES];
	struct k618_mon_stat day[K618_MON_RW][K618_MON_SIZES];
	struct k618_mon_stat day_slow[K618_MON_RW][K618_MON_SIZES];
	struct k618_mon_stat delay[K618_MON_RW][K618_MON_DELAYS];
	u64 *issue_ns;		/* per task tag, 0 = not timed */
	int ntags;
};

/* ------------------------------------------------------------------- ufscld ---- */

struct k618_cld {
	struct kobject kobj;
	struct mutex sysfs_lock;
	bool added;
	bool present;
	int trigger;			/* 0/1, what the HAL asked for */
	u32 interval_ms;		/* trigger_interval: the state poll period */
	bool debug;
	bool blocked;			/* runtime PM and clock gating held */
	bool ah8_off;			/* auto-hibern8 off for the defrag (SAMSUNG / SKhynix only) */
	bool ah8_on;			/* auto_hibern8_enable */
	u32 saved_ahit;
	u64 start_ms;			/* boot-time ms of the trigger, for the 420 s limit */
	u32 flag_idn;			/* the vendor flag that runs the defrag */
	u32 level_idn;			/* fragmentation level attribute */
	u32 state_idn;			/* defrag state attribute */
	char vendor[9];
	char product[17];
	struct delayed_work work;
};

static struct k618_ufs_mi {
	struct ufs_hba *hba;
	struct delayed_work init_work;
	int init_tries;
	bool hooks;
	struct k618_mon mon;
	struct k618_cld cld;
} k618_mi;

static void k618_kobj_release(struct kobject *kobj)
{
	/* the objects live in k618_mi (static storage): nothing to free */
}

/* ---- latency collection (vendor hooks of the UFS core) ---- */

static int k618_mon_rw(u8 op)
{
	switch (op) {
	case READ_6:
	case READ_10:
	case READ_16:
		return 0;
	case WRITE_6:
	case WRITE_10:
	case WRITE_16:
		return 1;
	}
	return -1;
}

static void k618_mon_clear(struct k618_mon_stat *s, size_t n)
{
	memset(s, 0, n * sizeof(*s));
}

static void k618_mon_add(struct k618_mon_stat *s, u64 lat)
{
	s->sum += lat;
	s->cnt++;
	if (lat > s->max)
		s->max = lat;
}

static void k618_mon_send(void *data, struct ufs_hba *hba, struct ufshcd_lrb *lrbp)
{
	struct k618_mon *m = &k618_mi.mon;

	if (hba != k618_mi.hba || !READ_ONCE(m->enabled) || !lrbp || !lrbp->cmd ||
	    lrbp->task_tag < 0 || lrbp->task_tag >= m->ntags)
		return;
	WRITE_ONCE(m->issue_ns[lrbp->task_tag], ktime_get_ns() ? : 1);
}

/* stock ufshcd_update_cmd_latency(): size class, latency class, the 2 windows, the daily sets */
static void k618_mon_compl(void *data, struct ufs_hba *hba, struct ufshcd_lrb *lrbp)
{
	struct k618_mon *m = &k618_mi.mon;
	struct scsi_cmnd *cmd;
	unsigned long flags;
	u64 now, issued, lat;
	u32 len, kb;
	int rw, sz, dl, w;

	if (hba != k618_mi.hba || !lrbp || !(cmd = lrbp->cmd) ||
	    lrbp->task_tag < 0 || lrbp->task_tag >= m->ntags)
		return;
	issued = xchg(&m->issue_ns[lrbp->task_tag], 0);
	if (!issued || !READ_ONCE(m->enabled))
		return;
	rw = k618_mon_rw(cmd->cmnd[0]);
	if (rw < 0)
		return;
	now = ktime_get_ns();
	lat = now > issued ? now - issued : 0;
	len = scsi_bufflen(cmd);
	kb = len >> 10;
	sz = (kb > 16) + (kb > 128) + (kb > 256) + (kb > 512);
	dl = lat < 1000000 ? 0 : lat < 5000000 ? 1 : lat < 10000000 ? 2 : lat < 50000000 ? 3 : 4;

	spin_lock_irqsave(&m->lock, flags);
	if (now - m->win_start > m->window_ns) {
		m->win_idx = (m->win_idx + 1) & 1;
		m->win_start = now;
		k618_mon_clear(&m->win[m->win_idx][0][0], K618_MON_RW * K618_MON_SIZES);
		k618_mon_clear(&m->win_slow[m->win_idx][0][0], K618_MON_RW * K618_MON_SIZES);
	}
	w = m->win_idx;
	k618_mon_add(&m->win[w][rw][sz], lat);
	k618_mon_add(&m->day[rw][sz], lat);
	if (lat > m->thr_us[rw][sz] * 1000) {
		k618_mon_add(&m->win_slow[w][rw][sz], lat);
		k618_mon_add(&m->day_slow[rw][sz], lat);
	}
	k618_mon_add(&m->delay[rw][dl], lat);
	spin_unlock_irqrestore(&m->lock, flags);
}

/* ---- ufs_monitor sysfs ---- */

struct k618_mon_attr {
	struct attribute attr;
	ssize_t (*show)(struct k618_mon *m, char *buf);
	ssize_t (*store)(struct k618_mon *m, const char *buf, size_t count);
};

static const char * const k618_mon_type[K618_MON_RW] = { "READ", "WRITE" };
static const char * const k618_mon_size[K618_MON_SIZES] = { "16", "128", "256", "512", "512+" };
static const char * const k618_mon_range[K618_MON_DELAYS] = { "1", "5", "10", "50", "50+" };

/* stock stops adding lines once the page is (nearly) full */
#define K618_MON_FULL(n) ((n) > PAGE_SIZE - 2)

static ssize_t k618_mon_enable_show(struct k618_mon *m, char *buf)
{
	return sysfs_emit(buf, "%s\n", m->enabled ? "true" : "false");
}

static void k618_mon_clear_all(struct k618_mon *m)
{
	k618_mon_clear(&m->win[0][0][0], 2 * K618_MON_RW * K618_MON_SIZES);
	k618_mon_clear(&m->win_slow[0][0][0], 2 * K618_MON_RW * K618_MON_SIZES);
	k618_mon_clear(&m->day[0][0], K618_MON_RW * K618_MON_SIZES);
	k618_mon_clear(&m->day_slow[0][0], K618_MON_RW * K618_MON_SIZES);
	k618_mon_clear(&m->delay[0][0], K618_MON_RW * K618_MON_DELAYS);
}

static ssize_t k618_mon_enable_store(struct k618_mon *m, const char *buf, size_t count)
{
	unsigned long long v;
	unsigned long flags;

	if (kstrtoull(buf, 0, &v))
		return -EINVAL;
	spin_lock_irqsave(&m->lock, flags);
	WRITE_ONCE(m->enabled, !!v);
	if (!v)
		k618_mon_clear_all(m);
	spin_unlock_irqrestore(&m->lock, flags);
	pr_info("ufs_monitor: monitor_enable %s\n", v ? "true" : "false");
	return count;
}

static ssize_t k618_mon_thr_show(struct k618_mon *m, int rw, char *buf)
{
	int i, n = 0;

	for (i = 0; i < K618_MON_SIZES; i++)
		n += sysfs_emit_at(buf, n, "%llu%s", m->thr_us[rw][i],
				   i < K618_MON_SIZES - 1 ? "," : "\n");
	return n;
}

/* stock store_latency_threshold(): up to 5 decimal values, missing ones 0; refused while on */
static ssize_t k618_mon_thr_store(struct k618_mon *m, int rw, const char *buf, size_t count)
{
	u64 v[K618_MON_SIZES] = { 0 };
	char *s, *p, *tok;
	unsigned long flags;
	int i;

	if (m->enabled) {
		pr_err("ufs_monitor: the monitor is on, thresholds are not changed\n");
		return -EINVAL;
	}
	s = kstrdup(buf, GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	p = s;
	for (i = 0; i < K618_MON_SIZES; i++) {
		tok = strsep(&p, ",");
		if (!tok) {
			pr_warn("ufs_monitor: only %d thresholds given, the rest set to 0\n", i);
			break;
		}
		if (!*tok || kstrtoull(tok, 10, &v[i])) {
			kfree(s);
			return -EINVAL;
		}
	}
	kfree(s);
	spin_lock_irqsave(&m->lock, flags);
	memcpy(m->thr_us[rw], v, sizeof(v));
	spin_unlock_irqrestore(&m->lock, flags);
	return count;
}

static ssize_t k618_mon_rthr_show(struct k618_mon *m, char *buf)
{
	return k618_mon_thr_show(m, 0, buf);
}

static ssize_t k618_mon_rthr_store(struct k618_mon *m, const char *buf, size_t count)
{
	return k618_mon_thr_store(m, 0, buf, count);
}

static ssize_t k618_mon_wthr_show(struct k618_mon *m, char *buf)
{
	return k618_mon_thr_show(m, 1, buf);
}

static ssize_t k618_mon_wthr_store(struct k618_mon *m, const char *buf, size_t count)
{
	return k618_mon_thr_store(m, 1, buf, count);
}

static ssize_t k618_mon_period_show(struct k618_mon *m, char *buf)
{
	return sysfs_emit(buf, "%llu\n", div_u64(m->window_ns, NSEC_PER_SEC));
}

/* stock: only while the monitor is on, 1 .. U64_MAX / 1e9 seconds */
static ssize_t k618_mon_period_store(struct k618_mon *m, const char *buf, size_t count)
{
	unsigned long long v;

	if (kstrtoull(buf, 10, &v))
		return -EINVAL;
	if (!m->enabled) {
		pr_err("ufs_monitor: the monitor is off, the window period is not changed\n");
		return -EINVAL;
	}
	if (!v || v > div_u64(U64_MAX, NSEC_PER_SEC))
		return -EINVAL;
	WRITE_ONCE(m->window_ns, v * NSEC_PER_SEC);
	return count;
}

static ssize_t k618_mon_daily_show(struct k618_mon *m, char *buf)
{
	const struct k618_mon_stat *s;
	unsigned long flags;
	int rw, i, n;

	spin_lock_irqsave(&m->lock, flags);
	n = sysfs_emit_at(buf, 0, "Normal:\n");
	n += sysfs_emit_at(buf, n, "Size         Sum(ms)          Count            Peak(us)\n");
	for (rw = 0; rw < K618_MON_RW && !K618_MON_FULL(n); rw++) {
		n += sysfs_emit_at(buf, n, "[%s]\n", k618_mon_type[rw]);
		for (i = 0; i < K618_MON_SIZES && !K618_MON_FULL(n); i++) {
			s = &m->day[rw][i];
			n += sysfs_emit_at(buf, n, "%-12s%-18llu%-18u%-12llu\n", k618_mon_size[i],
					   div_u64(s->sum, NSEC_PER_MSEC), s->cnt,
					   div_u64(s->max, NSEC_PER_USEC));
		}
	}
	if (K618_MON_FULL(n))
		goto out;
	n += sysfs_emit_at(buf, n, "\nAbnormal:\n");
	n += sysfs_emit_at(buf, n,
			   "Size         Sum(ms)          Count           Peak(us)      Threshold(us)\n");
	for (rw = 0; rw < K618_MON_RW && !K618_MON_FULL(n); rw++) {
		n += sysfs_emit_at(buf, n, "[%s]\n", k618_mon_type[rw]);
		for (i = 0; i < K618_MON_SIZES && !K618_MON_FULL(n); i++) {
			s = &m->day_slow[rw][i];
			n += sysfs_emit_at(buf, n, "%-12s%-18llu%-16u%-14llu%-14llu\n",
					   k618_mon_size[i], div_u64(s->sum, NSEC_PER_MSEC), s->cnt,
					   div_u64(s->max, NSEC_PER_USEC), m->thr_us[rw][i]);
		}
	}
	if (K618_MON_FULL(n))
		goto out;
	n += sysfs_emit_at(buf, n, "Delay:\n");
	n += sysfs_emit_at(buf, n, "Range(ms)         Sum(ms)          Count            Peak(ms)\n");
	for (rw = 0; rw < K618_MON_RW && !K618_MON_FULL(n); rw++) {
		n += sysfs_emit_at(buf, n, "[%s]\n", k618_mon_type[rw]);
		for (i = 0; i < K618_MON_DELAYS && !K618_MON_FULL(n); i++) {
			s = &m->delay[rw][i];
			n += sysfs_emit_at(buf, n, "%-12s%-18llu%-18u%-12llu\n", k618_mon_range[i],
					   div_u64(s->sum, NSEC_PER_MSEC), s->cnt,
					   div_u64(s->max, NSEC_PER_MSEC));
		}
	}
out:
	spin_unlock_irqrestore(&m->lock, flags);
	return n;
}

static ssize_t k618_mon_daily_store(struct k618_mon *m, const char *buf, size_t count)
{
	unsigned long flags;

	if (sysfs_streq(buf, "reset")) {
		spin_lock_irqsave(&m->lock, flags);
		k618_mon_clear(&m->day[0][0], K618_MON_RW * K618_MON_SIZES);
		k618_mon_clear(&m->day_slow[0][0], K618_MON_RW * K618_MON_SIZES);
		k618_mon_clear(&m->delay[0][0], K618_MON_RW * K618_MON_DELAYS);
		spin_unlock_irqrestore(&m->lock, flags);
		pr_info("ufs_monitor: daily statistics reset\n");
	}
	return count;
}

/* the current window first, then the previous one, each with its index */
static ssize_t k618_mon_window_show(struct k618_mon *m, char *buf)
{
	const struct k618_mon_stat *s;
	unsigned long flags;
	int rw, k, w, i, n;

	spin_lock_irqsave(&m->lock, flags);
	n = sysfs_emit_at(buf, 0, "Normal:\n");
	n += sysfs_emit_at(buf, n,
			   "Window   Size(k)      Sum(ms)          Count            Peak(us)\n");
	for (rw = 0; rw < K618_MON_RW && !K618_MON_FULL(n); rw++) {
		n += sysfs_emit_at(buf, n, "[%s]\n", k618_mon_type[rw]);
		for (k = 0; k < 2 && !K618_MON_FULL(n); k++) {
			w = (m->win_idx - k) & 1;
			for (i = 0; i < K618_MON_SIZES && !K618_MON_FULL(n); i++) {
				s = &m->win[w][rw][i];
				n += sysfs_emit_at(buf, n, "%-8d %-12s %-16llu %-16u %-12llu\n", w,
						   k618_mon_size[i], div_u64(s->sum, NSEC_PER_MSEC),
						   s->cnt, div_u64(s->max, NSEC_PER_USEC));
			}
		}
	}
	if (K618_MON_FULL(n))
		goto out;
	n += sysfs_emit_at(buf, n, "\nAbnormal:\n");
	n += sysfs_emit_at(buf, n,
			   "Window   Size(k)      Sum(ms)          Count           Peak(us)      Threshold(us)\n");
	for (rw = 0; rw < K618_MON_RW && !K618_MON_FULL(n); rw++) {
		n += sysfs_emit_at(buf, n, "[%s]\n", k618_mon_type[rw]);
		for (k = 0; k < 2 && !K618_MON_FULL(n); k++) {
			w = (m->win_idx - k) & 1;
			for (i = 0; i < K618_MON_SIZES && !K618_MON_FULL(n); i++) {
				s = &m->win_slow[w][rw][i];
				n += sysfs_emit_at(buf, n, "%-8d %-12s %-16llu %-15u %-12llu %-14llu\n",
						   w, k618_mon_size[i], div_u64(s->sum, NSEC_PER_MSEC),
						   s->cnt, div_u64(s->max, NSEC_PER_USEC),
						   m->thr_us[rw][i]);
			}
		}
	}
out:
	spin_unlock_irqrestore(&m->lock, flags);
	return n;
}

static ssize_t k618_mon_window_store(struct k618_mon *m, const char *buf, size_t count)
{
	unsigned long flags;

	if (sysfs_streq(buf, "reset")) {
		spin_lock_irqsave(&m->lock, flags);
		k618_mon_clear(&m->win[0][0][0], 2 * K618_MON_RW * K618_MON_SIZES);
		k618_mon_clear(&m->win_slow[0][0][0], 2 * K618_MON_RW * K618_MON_SIZES);
		spin_unlock_irqrestore(&m->lock, flags);
		pr_info("ufs_monitor: window statistics reset\n");
	}
	return count;
}

#define K618_MON_ATTR(_name, _show, _store) \
	static struct k618_mon_attr k618_mon_attr_##_name = { \
		.attr = { .name = #_name, .mode = 0644 }, .show = _show, .store = _store }

K618_MON_ATTR(monitor_enable, k618_mon_enable_show, k618_mon_enable_store);
K618_MON_ATTR(monitor_latency_read_threshold_us, k618_mon_rthr_show, k618_mon_rthr_store);
K618_MON_ATTR(monitor_latency_write_threshold_us, k618_mon_wthr_show, k618_mon_wthr_store);
K618_MON_ATTR(monitor_window_period_s, k618_mon_period_show, k618_mon_period_store);
K618_MON_ATTR(monitor_daily_cmd_lat, k618_mon_daily_show, k618_mon_daily_store);
K618_MON_ATTR(monitor_window_cmd_lat, k618_mon_window_show, k618_mon_window_store);

static struct attribute *k618_mon_attrs[] = {
	&k618_mon_attr_monitor_enable.attr,
	&k618_mon_attr_monitor_latency_read_threshold_us.attr,
	&k618_mon_attr_monitor_latency_write_threshold_us.attr,
	&k618_mon_attr_monitor_window_period_s.attr,
	&k618_mon_attr_monitor_daily_cmd_lat.attr,
	&k618_mon_attr_monitor_window_cmd_lat.attr,
	NULL,
};
ATTRIBUTE_GROUPS(k618_mon);

static ssize_t k618_mon_sysfs_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct k618_mon *m = container_of(kobj, struct k618_mon, kobj);
	struct k618_mon_attr *a = container_of(attr, struct k618_mon_attr, attr);
	ssize_t rc;

	if (!a->show)
		return -EIO;
	mutex_lock(&m->sysfs_lock);
	rc = a->show(m, buf);
	mutex_unlock(&m->sysfs_lock);
	return rc;
}

static ssize_t k618_mon_sysfs_store(struct kobject *kobj, struct attribute *attr,
				    const char *buf, size_t count)
{
	struct k618_mon *m = container_of(kobj, struct k618_mon, kobj);
	struct k618_mon_attr *a = container_of(attr, struct k618_mon_attr, attr);
	ssize_t rc;

	if (!a->store)
		return -EIO;
	mutex_lock(&m->sysfs_lock);
	rc = a->store(m, buf, count);
	mutex_unlock(&m->sysfs_lock);
	return rc;
}

static const struct sysfs_ops k618_mon_sysfs_ops = {
	.show = k618_mon_sysfs_show,
	.store = k618_mon_sysfs_store,
};

static const struct kobj_type k618_mon_ktype = {
	.release = k618_kobj_release,
	.sysfs_ops = &k618_mon_sysfs_ops,
	.default_groups = k618_mon_groups,
};

/* stock ufshcd_monitor_init(): off, 60 s windows, 100 ms thresholds */
static int k618_mon_init(struct ufs_hba *hba)
{
	struct k618_mon *m = &k618_mi.mon;
	int i, j, rc;

	m->ntags = hba->nutrs;
	m->issue_ns = kcalloc(m->ntags, sizeof(*m->issue_ns), GFP_KERNEL);
	if (!m->issue_ns)
		return -ENOMEM;
	mutex_init(&m->sysfs_lock);
	spin_lock_init(&m->lock);
	m->window_ns = 60ULL * NSEC_PER_SEC;
	for (i = 0; i < K618_MON_RW; i++)
		for (j = 0; j < K618_MON_SIZES; j++)
			m->thr_us[i][j] = 100000;
	rc = kobject_init_and_add(&m->kobj, &k618_mon_ktype, &hba->dev->kobj, "ufs_monitor");
	if (rc) {
		kobject_put(&m->kobj);
		kfree(m->issue_ns);
		m->issue_ns = NULL;
		m->ntags = 0;
		return rc;
	}
	kobject_uevent(&m->kobj, KOBJ_ADD);
	m->added = true;
	return 0;
}

/* ---- ufscld: the KIOXIA defragmentation (stock kioxia_*_cld / ufscld_* of both modules) ---- */

static int k618_cld_get_level(struct k618_cld *c, int *level)
{
	u32 v = U32_MAX;
	int rc;

	rc = ufshcd_query_attr_retry(k618_mi.hba, UPIU_QUERY_OPCODE_READ_ATTR,
				     (enum attr_idn)c->level_idn, 0, 0, &v);
	if (rc)
		return rc;
	if (v == 0)
		*level = 1;
	else if (v == 1)
		*level = 2;
	else if (v == 2 || v == 3)
		*level = 3;
	else {
		pr_err("ufscld: unknown fragmentation level %u\n", v);
		return -EINVAL;
	}
	return 0;
}

static int k618_cld_set_trigger(struct k618_cld *c, bool on)
{
	int rc = ufshcd_query_flag_retry(k618_mi.hba, on ? UPIU_QUERY_OPCODE_SET_FLAG :
					 UPIU_QUERY_OPCODE_CLEAR_FLAG,
					 (enum flag_idn)c->flag_idn, 0, NULL);

	if (rc)
		pr_err("ufscld: flag %#x %s failed: %d\n", c->flag_idn, on ? "set" : "clear", rc);
	return rc;
}

/* the 18 Pro Max's mapping: 0, 3, 4 = done (1), 1, 2 = in progress (2), others 0 */
static int k618_cld_get_state(struct k618_cld *c, int *state)
{
	u32 v = 0;
	int rc;

	rc = ufshcd_query_attr_retry(k618_mi.hba, UPIU_QUERY_OPCODE_READ_ATTR,
				     (enum attr_idn)c->state_idn, 0, 0, &v);
	if (rc)
		pr_err("ufscld: defrag state read failed: %d\n", rc);
	if (v == 0 || v == 3 || v == 4)
		*state = 1;
	else if (v == 1 || v == 2)
		*state = 2;
	else
		*state = 0;
	if (c->debug)
		pr_info("ufscld: defrag state %u -> %d\n", v, *state);
	return rc;
}

/* the defrag keeps the device and its clocks up (stock ufscld_block_enter_suspend) */
static void k618_cld_block(struct k618_cld *c)
{
	struct ufs_hba *hba = k618_mi.hba;

	if (c->blocked)
		return;
	c->blocked = true;
	ufshcd_rpm_get_sync(hba);
	ufshcd_hold(hba);
}

static void k618_cld_allow(struct k618_cld *c)
{
	struct ufs_hba *hba = k618_mi.hba;

	if (!c->blocked)
		return;
	c->blocked = false;
	ufshcd_release(hba);
	ufshcd_rpm_put(hba);
}

static void k618_cld_ah8(struct k618_cld *c, bool enable)
{
	struct ufs_hba *hba = k618_mi.hba;

	if (!ufshcd_is_auto_hibern8_supported(hba) || enable == c->ah8_on)
		return;
	if (!enable) {
		c->saved_ahit = READ_ONCE(hba->ahit);
		ufshcd_auto_hibern8_update(hba, 0);
	} else {
		ufshcd_auto_hibern8_update(hba, c->saved_ahit);
	}
	c->ah8_on = enable;
}

static u64 k618_boot_ms(void)
{
	return div_u64(ktime_get_boottime_ns(), NSEC_PER_MSEC);
}

static void k618_cld_uevent(struct k618_cld *c)
{
	char *envp[] = { "CLD_TRIGGER=OFF", NULL };

	kobject_uevent_env(&k618_mi.hba->dev->kobj, KOBJ_CHANGE, envp);
}

/* stock ufscld_trigger_off(), sysfs_lock held */
static void k618_cld_off(struct k618_cld *c)
{
	int level = 0;

	if (!c->trigger)
		return;
	c->trigger = 0;
	if (!k618_cld_set_trigger(c, false) && !k618_cld_get_level(c, &level) && c->debug)
		pr_info("ufscld: defrag off, fragmentation level %d\n", level);
	if (c->ah8_off)
		k618_cld_ah8(c, true);
	k618_cld_allow(c);
	k618_cld_uevent(c);
}

/* stock ufscld_trigger_on(), sysfs_lock held: the first poll at once */
static void k618_cld_on(struct k618_cld *c)
{
	if (c->trigger)
		return;
	c->trigger = 1;
	k618_cld_block(c);
	if (c->ah8_off)
		k618_cld_ah8(c, false);
	c->start_ms = k618_boot_ms();
	if (c->debug)
		pr_info("ufscld: defrag on at %llu ms\n", c->start_ms);
	mod_delayed_work(system_freezable_wq, &c->work, 0);
}

/*
 * stock ufscld_trigger_work_fn(): the flag again, then the state; off when done or after 420 s,
 * else the next poll after trigger_interval ms. The freezable queue keeps it out of system
 * suspend (5.15 used it; the 6.18 module cancels the work in its suspend path instead)
 */
static void k618_cld_work(struct work_struct *w)
{
	struct k618_cld *c = container_of(to_delayed_work(w), struct k618_cld, work);
	struct ufs_hba *hba = k618_mi.hba;
	int state = 0;
	u64 dt;

	if (ufshcd_rpm_get_sync(hba) < 0) {
		pm_runtime_put_noidle(&hba->ufs_device_wlun->sdev_gendev);
		pr_err("ufscld: runtime resume failed\n");
		/* try again later: a trigger left on would hold the device without an end */
		if (READ_ONCE(c->trigger))
			mod_delayed_work(system_freezable_wq, &c->work,
					 msecs_to_jiffies(c->interval_ms));
		return;
	}
	mutex_lock(&c->sysfs_lock);
	if (!c->trigger) {
		if (c->debug)
			pr_info("ufscld: trigger is off, poll ends\n");
		goto out;
	}
	dt = k618_boot_ms() - c->start_ms;
	if (k618_cld_set_trigger(c, true)) {
		/* stock polls on after a failed set; the 420 s limit is kept here as well */
		if (dt > 419999) {
			pr_warn("ufscld: defrag off, the flag cannot be set (%llu ms)\n", dt);
			k618_cld_off(c);
			goto out;
		}
		goto again;
	}
	k618_cld_get_state(c, &state);
	if (state == 1 || dt > 419999) {
		if (state != 1)
			pr_warn("ufscld: defrag off with state %d after %llu ms\n", state, dt);
		k618_cld_off(c);
		goto out;
	}
again:
	mod_delayed_work(system_freezable_wq, &c->work, msecs_to_jiffies(c->interval_ms));
out:
	mutex_unlock(&c->sysfs_lock);
	ufshcd_rpm_put(hba);
}

struct k618_cld_attr {
	struct attribute attr;
	ssize_t (*show)(struct k618_cld *c, char *buf);
	ssize_t (*store)(struct k618_cld *c, const char *buf, size_t count);
};

static ssize_t k618_cld_level_show(struct k618_cld *c, char *buf)
{
	int level = 0;

	k618_cld_get_level(c, &level);
	return sysfs_emit(buf, "%d\n", level);
}

static ssize_t k618_cld_state_show(struct k618_cld *c, char *buf)
{
	int state = 0;

	k618_cld_get_state(c, &state);
	return sysfs_emit(buf, "%d\n", state);
}

/*
 * stock: write bDefragOperation = 1 (3 tries), then read dHIDAvailableSize; 1 when the write
 * fails, 2 when the read does. The write is made only where the device reports JEDEC HID.
 */
static ssize_t k618_cld_hid_size_show(struct k618_cld *c, char *buf)
{
	struct ufs_hba *hba = k618_mi.hba;
	u32 v = 1;
	int tries, rc = -EOPNOTSUPP;

	if (!hba->dev_info.hid_sup)
		return sysfs_emit(buf, "%d\n", 1);
	for (tries = 0; tries < 3 && rc; tries++) {
		v = 1;
		rc = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_WRITE_ATTR,
					     QUERY_ATTR_IDN_HID_DEFRAG_OPERATION, 0, 0, &v);
	}
	if (rc)
		return sysfs_emit(buf, "%d\n", 1);
	rc = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_READ_ATTR,
				     QUERY_ATTR_IDN_HID_AVAILABLE_SIZE, 0, 0, &v);
	return sysfs_emit(buf, "%u\n", rc ? 2 : v);
}

/* stock: bHIDState (attribute 0x39), 15 when the read fails */
static ssize_t k618_cld_hid_state_show(struct k618_cld *c, char *buf)
{
	u32 v = 15;

	ufshcd_query_attr_retry(k618_mi.hba, UPIU_QUERY_OPCODE_READ_ATTR,
				QUERY_ATTR_IDN_HID_STATE, 0, 0, &v);
	return sysfs_emit(buf, "%u\n", v);
}

static ssize_t k618_cld_trigger_show(struct k618_cld *c, char *buf)
{
	return sysfs_emit(buf, "%d\n", c->trigger);
}

static ssize_t k618_cld_trigger_store(struct k618_cld *c, const char *buf, size_t count)
{
	unsigned long long v;

	if (kstrtoull(buf, 0, &v) || v > 1)
		return -EINVAL;
	if (c->debug)
		pr_info("ufscld: trigger %llu\n", v);
	if (v != c->trigger) {
		if (v)
			k618_cld_on(c);
		else
			k618_cld_off(c);
	}
	return count;
}

static ssize_t k618_cld_interval_show(struct k618_cld *c, char *buf)
{
	return sysfs_emit(buf, "%u\n", c->interval_ms);
}

static ssize_t k618_cld_interval_store(struct k618_cld *c, const char *buf, size_t count)
{
	unsigned long long v;

	if (kstrtoull(buf, 0, &v))
		return -EINVAL;
	if (v < 100 || v > 10000) {
		pr_info("ufscld: trigger_interval is 100 .. 10000 ms\n");
		return -EINVAL;
	}
	c->interval_ms = v;
	return count;
}

static ssize_t k618_cld_work_state_show(struct k618_cld *c, char *buf)
{
	return sysfs_emit(buf, "%d\n", delayed_work_pending(&c->work) ? 1 : 0);
}

static ssize_t k618_cld_debug_show(struct k618_cld *c, char *buf)
{
	return sysfs_emit(buf, "%d\n", c->debug);
}

static ssize_t k618_cld_debug_store(struct k618_cld *c, const char *buf, size_t count)
{
	unsigned long long v;

	if (kstrtoull(buf, 0, &v) || v > 1)
		return -EINVAL;
	c->debug = v;
	return count;
}

static ssize_t k618_cld_block_show(struct k618_cld *c, char *buf)
{
	return sysfs_emit(buf, "%d\n", c->blocked);
}

static ssize_t k618_cld_block_store(struct k618_cld *c, const char *buf, size_t count)
{
	unsigned long long v;

	if (kstrtoull(buf, 0, &v) || v > 1)
		return -EINVAL;
	if (v)
		k618_cld_block(c);
	else
		k618_cld_allow(c);
	return count;
}

static ssize_t k618_cld_ah8_show(struct k618_cld *c, char *buf)
{
	return sysfs_emit(buf, "%d\n", c->ah8_on);
}

static ssize_t k618_cld_ah8_store(struct k618_cld *c, const char *buf, size_t count)
{
	unsigned long long v;

	if (kstrtoull(buf, 0, &v) || v > 1)
		return -EINVAL;
	k618_cld_ah8(c, v);
	return count;
}

#define K618_CLD_ATTR(_name, _mode, _show, _store) \
	static struct k618_cld_attr k618_cld_attr_##_name = { \
		.attr = { .name = #_name, .mode = _mode }, .show = _show, .store = _store }

K618_CLD_ATTR(frag_level, 0444, k618_cld_level_show, NULL);
K618_CLD_ATTR(cld_operation_status, 0444, k618_cld_state_show, NULL);
K618_CLD_ATTR(hid_available_size, 0444, k618_cld_hid_size_show, NULL);
K618_CLD_ATTR(hid_state, 0444, k618_cld_hid_state_show, NULL);
K618_CLD_ATTR(trigger, 0644, k618_cld_trigger_show, k618_cld_trigger_store);
K618_CLD_ATTR(trigger_interval, 0644, k618_cld_interval_show, k618_cld_interval_store);
K618_CLD_ATTR(work_state, 0444, k618_cld_work_state_show, NULL);
K618_CLD_ATTR(debug, 0644, k618_cld_debug_show, k618_cld_debug_store);
K618_CLD_ATTR(block_suspend, 0644, k618_cld_block_show, k618_cld_block_store);
K618_CLD_ATTR(auto_hibern8_enable, 0644, k618_cld_ah8_show, k618_cld_ah8_store);

static struct attribute *k618_cld_attrs[] = {
	&k618_cld_attr_frag_level.attr,
	&k618_cld_attr_cld_operation_status.attr,
	&k618_cld_attr_hid_available_size.attr,
	&k618_cld_attr_hid_state.attr,
	&k618_cld_attr_trigger.attr,
	&k618_cld_attr_trigger_interval.attr,
	&k618_cld_attr_work_state.attr,
	&k618_cld_attr_debug.attr,
	&k618_cld_attr_block_suspend.attr,
	&k618_cld_attr_auto_hibern8_enable.attr,
	NULL,
};
ATTRIBUTE_GROUPS(k618_cld);

/* stock ufscld_attr_show/store(): the device resumed and the node lock held around each call */
static ssize_t k618_cld_sysfs_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct k618_cld *c = container_of(kobj, struct k618_cld, kobj);
	struct k618_cld_attr *a = container_of(attr, struct k618_cld_attr, attr);
	struct ufs_hba *hba = k618_mi.hba;
	ssize_t rc;

	if (!a->show)
		return -EIO;
	if (ufshcd_rpm_get_sync(hba) < 0) {
		pm_runtime_put_noidle(&hba->ufs_device_wlun->sdev_gendev);
		return -EBUSY;
	}
	mutex_lock(&c->sysfs_lock);
	rc = a->show(c, buf);
	mutex_unlock(&c->sysfs_lock);
	ufshcd_rpm_put(hba);
	return rc;
}

static ssize_t k618_cld_sysfs_store(struct kobject *kobj, struct attribute *attr,
				    const char *buf, size_t count)
{
	struct k618_cld *c = container_of(kobj, struct k618_cld, kobj);
	struct k618_cld_attr *a = container_of(attr, struct k618_cld_attr, attr);
	struct ufs_hba *hba = k618_mi.hba;
	ssize_t rc;

	if (!a->store)
		return -EIO;
	if (ufshcd_rpm_get_sync(hba) < 0) {
		pm_runtime_put_noidle(&hba->ufs_device_wlun->sdev_gendev);
		return -EBUSY;
	}
	mutex_lock(&c->sysfs_lock);
	rc = a->store(c, buf, count);
	mutex_unlock(&c->sysfs_lock);
	ufshcd_rpm_put(hba);
	return rc;
}

static const struct sysfs_ops k618_cld_sysfs_ops = {
	.show = k618_cld_sysfs_show,
	.store = k618_cld_sysfs_store,
};

static const struct kobj_type k618_cld_ktype = {
	.release = k618_kobj_release,
	.sysfs_ops = &k618_cld_sysfs_ops,
	.default_groups = k618_cld_groups,
};

/*
 * stock ufscld_init_ops() + kioxia_init_idn() for this phone's vendor: KIOXIA below UFS 4.1, its
 * vendor flag and attributes by product line; anything else gets no ufscld
 */
static int k618_cld_ids(struct k618_cld *c, struct ufs_hba *hba)
{
	struct scsi_device *sdev = READ_ONCE(hba->ufs_device_wlun);

	if (!sdev || !sdev->inquiry || sdev->inquiry_len < 36)
		return -ENODEV;
	memcpy(c->vendor, sdev->inquiry + 8, 8);
	memcpy(c->product, sdev->inquiry + 16, 16);
	if (strncmp(c->vendor, "KIOXIA", 6) || hba->dev_info.wspecversion >= 0x410)
		return -EOPNOTSUPP;
	if (!strncmp(c->product, "THGJFL", 6)) {
		c->flag_idn = 0xa0;	/* Gen12 */
		c->level_idn = 0xa0;
		c->state_idn = 0xa1;
	} else if (!strncmp(c->product, "THGJFJ", 6)) {
		c->flag_idn = 0x13;	/* Gen11: the 13 Pro's THGJFJT1E45BATPC */
		c->level_idn = 0x34;
		c->state_idn = 0x35;
	} else {
		return -EOPNOTSUPP;
	}
	return 0;
}

static int k618_cld_init(struct ufs_hba *hba)
{
	struct k618_cld *c = &k618_mi.cld;
	int rc;

	rc = k618_cld_ids(c, hba);
	if (rc) {
		dev_info(hba->dev, "ufscld: not for %.8s %.16s (spec %#x): %d\n", c->vendor,
			 c->product, hba->dev_info.wspecversion, rc);
		return rc;
	}
	mutex_init(&c->sysfs_lock);
	INIT_DELAYED_WORK(&c->work, k618_cld_work);
	c->interval_ms = 2000;
	c->ah8_off = false;	/* stock: only SAMSUNG and SKhynix turn auto-hibern8 off */
	c->ah8_on = ufshcd_is_auto_hibern8_supported(hba);
	rc = kobject_init_and_add(&c->kobj, &k618_cld_ktype, &hba->dev->kobj, "ufscld");
	if (rc) {
		kobject_put(&c->kobj);
		return rc;
	}
	kobject_uevent(&c->kobj, KOBJ_ADD);
	c->added = true;
	c->present = true;
	dev_info(hba->dev, "ufscld: %.8s %.16s, flag %#x, attributes %#x/%#x\n", c->vendor,
		 c->product, c->flag_idn, c->level_idn, c->state_idn);
	return 0;
}

/* ---- probe / remove (called from ufs_qcom_probe / ufs_qcom_remove) ---- */

/* the device WLUN (and its INQUIRY data) only exists once the UFS core's async scan is done */
static void k618_ufs_mi_init_fn(struct work_struct *w)
{
	struct ufs_hba *hba = k618_mi.hba;

	if (!READ_ONCE(hba->ufs_device_wlun)) {
		if (++k618_mi.init_tries < 300)
			schedule_delayed_work(&k618_mi.init_work, msecs_to_jiffies(100));
		else
			dev_warn(hba->dev, "ufscld: no UFS device WLUN after 30 s\n");
		return;
	}
	k618_cld_init(hba);
}

static void k618_ufs_mi_probe(struct ufs_hba *hba)
{
	int rc;

	if (!hba || k618_mi.hba)
		return;
	k618_mi.hba = hba;
	if (k618_mon_param) {
		rc = k618_mon_init(hba);
		if (rc)
			dev_warn(hba->dev, "ufs_monitor: not created: %d\n", rc);
	}
	if (k618_mi.mon.added) {
		rc = register_trace_android_vh_ufs_send_command(k618_mon_send, NULL);
		if (!rc) {
			rc = register_trace_android_vh_ufs_compl_command(k618_mon_compl, NULL);
			if (rc)
				unregister_trace_android_vh_ufs_send_command(k618_mon_send, NULL);
		}
		k618_mi.hooks = !rc;
		if (rc)
			dev_warn(hba->dev, "ufs_monitor: hooks not registered: %d\n", rc);
	}
	if (k618_cld_param) {
		INIT_DELAYED_WORK(&k618_mi.init_work, k618_ufs_mi_init_fn);
		schedule_delayed_work(&k618_mi.init_work, 0);
	}
}

static void k618_ufs_mi_remove(struct ufs_hba *hba)
{
	struct k618_cld *c = &k618_mi.cld;
	struct k618_mon *m = &k618_mi.mon;

	if (!hba || hba != k618_mi.hba)
		return;
	if (k618_cld_param)
		cancel_delayed_work_sync(&k618_mi.init_work);
	if (c->present) {
		mutex_lock(&c->sysfs_lock);
		k618_cld_off(c);
		mutex_unlock(&c->sysfs_lock);
		cancel_delayed_work_sync(&c->work);
		k618_cld_allow(c);
		c->present = false;
	}
	if (c->added) {
		kobject_del(&c->kobj);
		kobject_put(&c->kobj);
		c->added = false;
	}
	if (k618_mi.hooks) {
		unregister_trace_android_vh_ufs_compl_command(k618_mon_compl, NULL);
		unregister_trace_android_vh_ufs_send_command(k618_mon_send, NULL);
		tracepoint_synchronize_unregister();
		k618_mi.hooks = false;
	}
	if (m->added) {
		kobject_del(&m->kobj);
		kobject_put(&m->kobj);
		m->added = false;
	}
	kfree(m->issue_ns);
	m->issue_ns = NULL;
	m->ntags = 0;
	k618_mi.hba = NULL;
}
