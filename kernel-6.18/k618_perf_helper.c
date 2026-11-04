// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#define pr_fmt(fmt) "perf_helper: " fmt

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/memcontrol.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/rtc.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/sysfs.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

/* 0 (default) = stock: a write to /proc/global_reclaim reclaims that many pages.
 * 1 = the write is accepted and nothing is reclaimed (runtime switch for an A/B). */
static bool global_reclaim_absorb;
module_param(global_reclaim_absorb, bool, 0644);
MODULE_PARM_DESC(global_reclaim_absorb, "accept /proc/global_reclaim writes without reclaiming");

/* one kernel log line per /proc/global_reclaim write: who, how much, how long */
static bool global_reclaim_log = true;
module_param(global_reclaim_log, bool, 0644);
MODULE_PARM_DESC(global_reclaim_log, "log every /proc/global_reclaim request");

/* 0 leaves out the perf HAL's nodes (perflock_records, kcompactd_pid, kswapd_c) */
static bool perf_nodes = true;
module_param(perf_nodes, bool, 0444);
MODULE_PARM_DESC(perf_nodes, "create the perf HAL nodes as the stock module does");

/* 1: at load, run each node's kernel side once and log it -- for QEMU, where no mimd runs */
static bool selftest;
module_param(selftest, bool, 0444);
MODULE_PARM_DESC(selftest, "exercise reclaim, the event queue, the rings and the pids at load");

/* ---------------------------------------------------------------------------------------------
 * Time-stamped line rings: /proc/mimdlog and /proc/perflock_records (stock: 2048 lines of at most
 * 127 characters each, written with a trylock that drops a line when it is contended; this keeps
 * every line).
 */
#define PH_REC_LEN	128
#define PH_REC_NUM	2048

struct ph_rec {
	char msg[PH_REC_LEN];
	struct timespec64 ts;
};

struct ph_ring {
	struct ph_rec *rec;
	unsigned int head;	/* next slot to write */
	unsigned int count;	/* valid lines, at most PH_REC_NUM */
	spinlock_t lock;
};

static struct ph_rec mimdlog_rec[PH_REC_NUM];
static struct ph_rec perflock_rec[PH_REC_NUM];
static struct ph_ring mimdlog_ring = {
	.rec = mimdlog_rec, .lock = __SPIN_LOCK_UNLOCKED(mimdlog_ring.lock),
};
static struct ph_ring perflock_ring = {
	.rec = perflock_rec, .lock = __SPIN_LOCK_UNLOCKED(perflock_ring.lock),
};

static unsigned int ph_ring_add(struct ph_ring *r, const char *msg)
{
	struct timespec64 ts;
	struct ph_rec *e;
	unsigned int count;

	ktime_get_real_ts64(&ts);
	spin_lock(&r->lock);
	e = &r->rec[r->head];
	e->ts = ts;
	strscpy(e->msg, msg, sizeof(e->msg));
	r->head = (r->head + 1) % PH_REC_NUM;
	if (r->count < PH_REC_NUM)
		r->count++;
	count = r->count;
	spin_unlock(&r->lock);
	return count;
}

static ssize_t ph_ring_write(struct ph_ring *r, const char __user *ubuf, size_t count)
{
	char kbuf[PH_REC_LEN];
	size_t n;

	/* stock: more than 128 bytes is -EINVAL */
	if (count > PH_REC_LEN)
		return -EINVAL;
	n = min_t(size_t, count, PH_REC_LEN - 1);
	if (copy_from_user(kbuf, ubuf, n))
		return -EFAULT;
	kbuf[n] = '\0';
	ph_ring_add(r, kbuf);
	return count;
}

static int ph_ring_show(struct seq_file *m, struct ph_ring *r)
{
	unsigned int i, idx;
	struct rtc_time tm;

	spin_lock(&r->lock);
	idx = (r->head + PH_REC_NUM - r->count) % PH_REC_NUM;
	for (i = 0; i < r->count; i++, idx = (idx + 1) % PH_REC_NUM) {
		struct ph_rec *e = &r->rec[idx];

		rtc_time64_to_tm(e->ts.tv_sec, &tm);
		seq_printf(m, "%d-%02d-%02d %02d:%02d:%02d UTC { %s }\n",
			   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			   tm.tm_hour, tm.tm_min, tm.tm_sec, e->msg);
	}
	spin_unlock(&r->lock);
	return 0;
}

static int mimdlog_show(struct seq_file *m, void *v)
{
	return ph_ring_show(m, &mimdlog_ring);
}

static int mimdlog_open(struct inode *inode, struct file *file)
{
	return single_open(file, mimdlog_show, NULL);
}

static ssize_t mimdlog_write(struct file *file, const char __user *ubuf, size_t count,
			     loff_t *ppos)
{
	return ph_ring_write(&mimdlog_ring, ubuf, count);
}

static const struct proc_ops mimdlog_ops = {
	.proc_open	= mimdlog_open,
	.proc_read	= seq_read,
	.proc_write	= mimdlog_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int perflock_records_show(struct seq_file *m, void *v)
{
	return ph_ring_show(m, &perflock_ring);
}

static int perflock_records_open(struct inode *inode, struct file *file)
{
	return single_open(file, perflock_records_show, NULL);
}

static ssize_t perflock_records_write(struct file *file, const char __user *ubuf, size_t count,
				      loff_t *ppos)
{
	return ph_ring_write(&perflock_ring, ubuf, count);
}

static const struct proc_ops perflock_records_ops = {
	.proc_open	= perflock_records_open,
	.proc_read	= seq_read,
	.proc_write	= perflock_records_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ---------------------------------------------------------------------------------------------
 * /proc/global_reclaim
 */
#define PH_RECLAIM_RETRIES	16	/* mm/internal.h MAX_RECLAIM_RETRIES */

static DEFINE_SPINLOCK(reclaim_res_lock);
static char reclaim_res[64];	/* what a read shows: stock "reclaim %lu pages", no newline */

/*
 * The 6.18 form of stock's single try_to_free_mem_cgroup_pages(NULL, nr, GFP_KERNEL, true):
 * the loop `echo <bytes> > /sys/fs/cgroup/memory.reclaim` runs on the root cgroup
 * (user_proactive_reclaim(), not exported) -- a quarter of what is left per call, stop on a
 * signal or after 16 calls in a row that freed nothing. The target is the same (nr pages from
 * the whole system, anon and file); reclaim stops at the goal on 6.18 (partial memcg walks and
 * MGLRU's should_abort_scan), and it is booked as proactive, not as direct reclaim.
 */
static unsigned long ph_global_reclaim(unsigned long nr_to_reclaim)
{
	struct mem_cgroup *target = mem_cgroup_disabled() ? NULL : root_mem_cgroup;
	unsigned int retries = PH_RECLAIM_RETRIES;
	unsigned long nr_reclaimed = 0;

	while (nr_reclaimed < nr_to_reclaim) {
		unsigned long batch = (nr_to_reclaim - nr_reclaimed) / 4;
		unsigned long reclaimed;

		if (signal_pending(current))
			break;
		reclaimed = try_to_free_mem_cgroup_pages(target, batch, GFP_KERNEL,
							 MEMCG_RECLAIM_MAY_SWAP |
							 MEMCG_RECLAIM_PROACTIVE, NULL);
		if (!reclaimed && !retries--)
			break;
		nr_reclaimed += reclaimed;
	}
	return nr_reclaimed;
}

static int global_reclaim_show(struct seq_file *m, void *v)
{
	spin_lock(&reclaim_res_lock);
	seq_printf(m, "%s", reclaim_res);
	spin_unlock(&reclaim_res_lock);
	return 0;
}

static int global_reclaim_open(struct inode *inode, struct file *file)
{
	return single_open(file, global_reclaim_show, NULL);
}

static ssize_t global_reclaim_write(struct file *file, const char __user *ubuf, size_t count,
				    loff_t *ppos)
{
	bool absorb = READ_ONCE(global_reclaim_absorb);
	unsigned long reclaimed = 0;
	unsigned long long nr;
	char buf[64] = {};
	u64 t0, ms;
	int ret;

	if (count > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	ret = kstrtoull(strim(buf), 10, &nr);
	if (ret)
		return ret;
	/* stock returns 0 for "0", which a C stdio writer takes as a failed write: say done */
	if (!nr)
		return count;
	/* stock had no bound; more than the whole RAM cannot be meant */
	nr = min_t(unsigned long long, nr, totalram_pages());

	t0 = ktime_get_ns();
	if (!absorb)
		reclaimed = ph_global_reclaim(nr);
	ms = div_u64(ktime_get_ns() - t0, NSEC_PER_MSEC);

	spin_lock(&reclaim_res_lock);
	if (absorb)
		snprintf(reclaim_res, sizeof(reclaim_res), "absorbed %llu pages", nr);
	else
		snprintf(reclaim_res, sizeof(reclaim_res), "reclaim %lu pages", reclaimed);
	spin_unlock(&reclaim_res_lock);

	if (READ_ONCE(global_reclaim_log))
		pr_info("global_reclaim %llu pages by %s[%d]: %lu reclaimed in %llu ms%s\n",
			nr, current->comm, task_pid_nr(current), reclaimed, ms,
			absorb ? " (absorbed)" : "");
	return count;
}

static const struct proc_ops global_reclaim_ops = {
	.proc_open	= global_reclaim_open,
	.proc_read	= seq_read,
	.proc_write	= global_reclaim_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ---------------------------------------------------------------------------------------------
 * /proc/kcompactd_pid and /proc/kswapd_c (one memory node: NODE_DATA(0) is contig_page_data)
 */
static pid_t ph_kswapd_pid(void)
{
	struct pglist_data *pgdat = NODE_DATA(0);
	pid_t pid = 0;

	mutex_lock(&pgdat->kswapd_lock);
	if (pgdat->kswapd)
		pid = task_pid_nr(pgdat->kswapd);
	mutex_unlock(&pgdat->kswapd_lock);
	return pid;
}

static int kcompactd_pid_show(struct seq_file *m, void *v)
{
	struct task_struct *t = READ_ONCE(NODE_DATA(0)->kcompactd);

	/* stock: "%d", no newline, 0 when there is none */
	seq_printf(m, "%d", t ? task_pid_nr(t) : 0);
	return 0;
}

static int kcompactd_pid_open(struct inode *inode, struct file *file)
{
	return single_open(file, kcompactd_pid_show, NULL);
}

static const struct proc_ops kcompactd_pid_ops = {
	.proc_open	= kcompactd_pid_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static DECLARE_WAIT_QUEUE_HEAD(kswapd_c_wait);
static atomic_t kswapd_c_trigger = ATOMIC_INIT(0);

static int kswapd_c_show(struct seq_file *m, void *v)
{
	/* stock: the pids of kswapd0 and its extra threads, space-separated, then "\n" */
	seq_printf(m, "%d\n", ph_kswapd_pid());
	return 0;
}

static int kswapd_c_open(struct inode *inode, struct file *file)
{
	return single_open(file, kswapd_c_show, NULL);
}

static ssize_t kswapd_c_write(struct file *file, const char __user *ubuf, size_t count,
			      loff_t *ppos)
{
	unsigned long long nr;
	int ret;

	if (count > 64)
		return -EINVAL;
	ret = kstrtoull_from_user(ubuf, count, 10, &nr);
	if (ret)
		return ret;
	if (nr < 1 || nr > 8) {
		pr_err("extend kswapd nr need in (1 - 8), nr is %llu\n", nr);
		return -EINVAL;
	}
	/* no extra kswapd threads here (see the top); pollers are told, as stock does */
	atomic_set(&kswapd_c_trigger, 1);
	wake_up_interruptible(&kswapd_c_wait);
	return count;
}

static __poll_t kswapd_c_poll(struct file *file, struct poll_table_struct *wait)
{
	__poll_t mask = EPOLLIN | EPOLLRDNORM | EPOLLOUT | EPOLLWRNORM;	/* stock 0x145 */

	poll_wait(file, &kswapd_c_wait, wait);
	if (atomic_xchg(&kswapd_c_trigger, 0))
		mask |= EPOLLPRI;					/* stock 0x147 */
	return mask;
}

static const struct proc_ops kswapd_c_ops = {
	.proc_open	= kswapd_c_open,
	.proc_read	= seq_read,
	.proc_write	= kswapd_c_write,
	.proc_poll	= kswapd_c_poll,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ---------------------------------------------------------------------------------------------
 * /sys/module/perf_helper/mimd/mimdtrigger
 */
#define MIMD_TRIGGER_MAX	11

struct mimd_trigger {
	struct list_head node;
	long long val;
};

static LIST_HEAD(trigger_head);
static DEFINE_MUTEX(trigger_lock);
static int trigger_count;

/* the oldest queued event; false when there is none */
static bool ph_trigger_pop(long long *val)
{
	struct mimd_trigger *t;

	mutex_lock(&trigger_lock);
	t = list_first_entry_or_null(&trigger_head, struct mimd_trigger, node);
	if (t) {
		*val = t->val;
		list_del(&t->node);
		kfree(t);
		trigger_count--;
	}
	mutex_unlock(&trigger_lock);
	return t != NULL;
}

/* stock: a 12th queued event, or one that cannot be allocated, is logged and dropped */
static void ph_trigger_push(long long val)
{
	struct mimd_trigger *t;

	mutex_lock(&trigger_lock);
	if (trigger_count >= MIMD_TRIGGER_MAX) {
		mutex_unlock(&trigger_lock);
		pr_err_ratelimited("%s: trigger_event so manay that discard\n", "mimdtrigger");
		return;
	}
	t = kmalloc(sizeof(*t), GFP_KERNEL);
	if (!t) {
		mutex_unlock(&trigger_lock);
		pr_err_ratelimited("%s: kmalloc struct trigger_event failed\n", "mimdtrigger");
		return;
	}
	t->val = val;
	list_add_tail(&t->node, &trigger_head);
	trigger_count++;
	mutex_unlock(&trigger_lock);
}

static ssize_t mimdtrigger_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	long long val;

	/* each read takes one event (mimd opens, reads, closes per event); empty: 0 bytes */
	if (!ph_trigger_pop(&val))
		return 0;
	return sysfs_emit(buf, "%lld\n", val);
}

static ssize_t mimdtrigger_store(struct kobject *kobj, struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned long long val;
	int ret;

	ret = kstrtoull(buf, 10, &val);
	if (ret)
		return ret;
	ph_trigger_push((long long)val);
	return count;
}

static struct kobj_attribute mimdtrigger_attr =
	__ATTR(mimdtrigger, 0664, mimdtrigger_show, mimdtrigger_store);

static struct attribute *mimd_attrs[] = {
	&mimdtrigger_attr.attr,
	NULL,
};

static const struct attribute_group mimd_attr_group = {
	.attrs = mimd_attrs,
};

static struct kobject *mimd_kobj;

/* ---------------------------------------------------------------------------------------------
 */
struct ph_proc {
	const char *name;
	const struct proc_ops *ops;
	bool perf;		/* a perf HAL node (perf_nodes=0 leaves it out) */
	bool made;
};

/* stock order and mode (0664 each) */
enum { PH_PERFLOCK, PH_MIMDLOG, PH_GLOBAL_RECLAIM, PH_KSWAPD_C, PH_KCOMPACTD_PID };
static struct ph_proc ph_procs[] = {
	[PH_PERFLOCK]		= { "perflock_records",	&perflock_records_ops,	true },
	[PH_MIMDLOG]		= { "mimdlog",		&mimdlog_ops,		false },
	[PH_GLOBAL_RECLAIM]	= { "global_reclaim",	&global_reclaim_ops,	false },
	[PH_KSWAPD_C]		= { "kswapd_c",		&kswapd_c_ops,		true },
	[PH_KCOMPACTD_PID]	= { "kcompactd_pid",	&kcompactd_pid_ops,	true },
};

static void ph_remove(void)
{
	int i;

	if (mimd_kobj) {
		sysfs_remove_group(mimd_kobj, &mimd_attr_group);
		kobject_put(mimd_kobj);
		mimd_kobj = NULL;
	}
	for (i = ARRAY_SIZE(ph_procs) - 1; i >= 0; i--) {
		if (ph_procs[i].made)
			remove_proc_entry(ph_procs[i].name, NULL);
		ph_procs[i].made = false;
	}
}

static void __init ph_selftest(void)
{
	long long a = -1, b = -1, c = -1;
	unsigned long reclaimed;
	struct task_struct *kc;
	unsigned int lines;
	bool empty;
	u64 t0;

	t0 = ktime_get_ns();
	reclaimed = ph_global_reclaim(256);
	pr_info("selftest: reclaim 256 pages -> %lu in %llu us\n", reclaimed,
		div_u64(ktime_get_ns() - t0, NSEC_PER_USEC));

	ph_trigger_push(7);
	ph_trigger_push(9);
	ph_trigger_pop(&a);
	ph_trigger_pop(&b);
	empty = !ph_trigger_pop(&c);
	pr_info("selftest: mimdtrigger queue 7,9 -> %lld,%lld, then %s\n", a, b,
		empty ? "empty (ok)" : "NOT empty");

	lines = ph_ring_add(&mimdlog_ring, "perf_helper selftest");
	pr_info("selftest: mimdlog holds %u line(s)\n", lines);

	kc = READ_ONCE(NODE_DATA(0)->kcompactd);
	pr_info("selftest: kswapd0 pid %d, kcompactd0 pid %d\n", ph_kswapd_pid(),
		kc ? task_pid_nr(kc) : 0);
}

static int __init perf_helper_init(void)
{
	int i, made = 0;

	/* like stock, a node that cannot be made is logged and the rest still are */
	for (i = 0; i < ARRAY_SIZE(ph_procs); i++) {
		if (ph_procs[i].perf && !perf_nodes)
			continue;
		if (proc_create(ph_procs[i].name, 0664, NULL, ph_procs[i].ops)) {
			ph_procs[i].made = true;
			made++;
		} else {
			pr_err("%s: create %s node failed\n", __func__, ph_procs[i].name);
		}
	}

	mimd_kobj = kobject_create_and_add("mimd", &THIS_MODULE->mkobj.kobj);
	if (mimd_kobj && sysfs_create_group(mimd_kobj, &mimd_attr_group)) {
		pr_err("create mimd sysfs nodes group failed\n");
		kobject_put(mimd_kobj);
		mimd_kobj = NULL;
	}

	/* without /proc/global_reclaim mimd aborts: worth a load failure, not a silent half */
	if (!ph_procs[PH_GLOBAL_RECLAIM].made) {
		ph_remove();
		return -ENOMEM;
	}
	pr_info("%d proc nodes, mimdtrigger %s\n", made, mimd_kobj ? "up" : "missing");
	if (selftest)
		ph_selftest();
	return 0;
}

static void __exit perf_helper_exit(void)
{
	struct mimd_trigger *t, *n;

	ph_remove();
	list_for_each_entry_safe(t, n, &trigger_head, node) {
		list_del(&t->node);
		kfree(t);
	}
}

module_init(perf_helper_init);
module_exit(perf_helper_exit);

MODULE_DESCRIPTION("k618: the 13 Pro's 5.15 perf_helper nodes (mimd, perf HAL) on 6.18");
MODULE_LICENSE("GPL");
