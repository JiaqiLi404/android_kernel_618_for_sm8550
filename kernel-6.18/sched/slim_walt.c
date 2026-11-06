// SPDX-License-Identifier: GPL-2.0
/*
 * slim WALT -- how busy each CPU was, sampled at the scheduler tick.
 *
 * A light stand-in for window-assisted load tracking: at every tick the time since the tick
 * before is booked as busy if the CPU has anything runnable at that moment, and the busy share of
 * the last complete window (20 ms) is the CPU's load. The rule itself is in sugov_ext_math.h; this
 * file is where it is fed and read. The schedutilmium governor (cpufreq_sugov_ext.c) is the reader
 * while its "refined" is 0.
 *
 * The tick is only looked at while somebody wants the result (slim_walt_get()): with another
 * governor, or with "refined", this costs nothing. The hook on the tick is a tracepoint, and putting
 * one in or taking it out switches a static key, which takes the CPU hotplug lock -- held already
 * where a governor is started or stopped by CPU hotplug. So a work item does that, after the count
 * of users has changed.
 *
 *   /sys/module/slim_walt/parameters/slim_walt_ctrl        0: off, loads read 0
 *   /sys/module/slim_walt/parameters/slim_walt_window_ms   the window, 4 to 1000
 *   /proc/slim_sched/walt_ctrl     the same switch
 *   /proc/slim_sched/window_ms     the window
 *   /proc/slim_sched/walt_busy     a line for each CPU: the busy share of its last window in
 *                                  percent and of 1024, the runnable tasks per tick of that
 *                                  window times 1024 and their maximum, windows so far
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "sched.h"

#include <linux/proc_fs.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/seqlock.h>
#include <linux/workqueue.h>
#include <trace/hooks/sched.h>

#include "slim_walt.h"
#include "sugov_ext_math.h"

static unsigned int slim_walt_ctrl __read_mostly = 1;
static unsigned int slim_walt_window_ms __read_mostly = 20;

struct slim_walt_cpu {
	seqcount_t	seq;	/* written by the CPU's tick, read from anywhere */
	struct sx_slim	sw;
};

static DEFINE_PER_CPU(struct slim_walt_cpu, slim_walt_cpu);

/* who holds a reference (slim_walt_get()), and whether the tick hook is in (the work item's own) */
static atomic_t slim_walt_users = ATOMIC_INIT(0);
static bool slim_walt_hooked;

static inline u64 slim_walt_window(void)
{
	return (u64)READ_ONCE(slim_walt_window_ms) * NSEC_PER_MSEC;
}

/*
 * The tasks runnable on @rq. 6.18 keeps a fair task that went to sleep with a negative lag queued
 * until that lag is gone (delayed dequeue), and nr_running counts it: those are taken off, as
 * fair.c does (cfs_h_nr_delayed()). Read without the run queue's lock (the tick hook comes after
 * it is let go): a count caught half way is cut at nothing.
 */
static unsigned int slim_walt_runnable(struct rq *rq)
{
	unsigned int nr = READ_ONCE(rq->nr_running), queued = READ_ONCE(rq->cfs.h_nr_queued);
	unsigned int runnable = READ_ONCE(rq->cfs.h_nr_runnable);
	unsigned int delayed = queued > runnable ? queued - runnable : 0;

	return nr > delayed ? nr - delayed : 0;
}

/* at the end of sched_tick(), on the CPU that ticks */
static void slim_walt_tick(void *data, struct rq *rq)
{
	struct slim_walt_cpu *c;

	if (!READ_ONCE(slim_walt_ctrl))
		return;

	c = this_cpu_ptr(&slim_walt_cpu);
	raw_write_seqcount_begin(&c->seq);
	sx_slim_tick(&c->sw, sched_clock(), slim_walt_runnable(rq), slim_walt_window());
	raw_write_seqcount_end(&c->seq);
}

static void slim_walt_snapshot(int cpu, struct sx_slim *sw)
{
	struct slim_walt_cpu *c = &per_cpu(slim_walt_cpu, cpu);
	unsigned int seq;

	do {
		seq = raw_read_seqcount_begin(&c->seq);
		*sw = c->sw;
	} while (read_seqcount_retry(&c->seq, seq));
}

unsigned long slim_walt_load(int cpu, u64 now)
{
	struct sx_slim sw;

	if (!READ_ONCE(slim_walt_ctrl))
		return 0;
	slim_walt_snapshot(cpu, &sw);
	return sx_slim_share(&sw, now, slim_walt_window(), SCHED_CAPACITY_SCALE);
}

/*
 * The hook in while there are users, out while there are none. (A work item runs on one CPU at a
 * time: whatever changed the count meanwhile is seen at its next run, which that change queued.)
 */
static void slim_walt_hook(struct work_struct *work)
{
	bool want = atomic_read(&slim_walt_users) > 0;
	int ret;

	if (want == slim_walt_hooked)
		return;
	if (want) {
		ret = register_trace_android_vh_scheduler_tick(slim_walt_tick, NULL);
		slim_walt_hooked = !ret;
		if (ret)
			pr_err("register tick hook failed %d\n", ret);
	} else {
		unregister_trace_android_vh_scheduler_tick(slim_walt_tick, NULL);
		slim_walt_hooked = false;
	}
}

static DECLARE_WORK(slim_walt_hook_work, slim_walt_hook);

void slim_walt_get(void)
{
	if (atomic_inc_return(&slim_walt_users) == 1)
		schedule_work(&slim_walt_hook_work);
}

void slim_walt_put(void)
{
	int users = atomic_dec_return(&slim_walt_users);

	if (WARN_ON(users < 0)) {
		atomic_inc(&slim_walt_users);
		return;
	}
	if (!users)
		schedule_work(&slim_walt_hook_work);
}

/* anything but 0 is 1 */
static int slim_walt_ctrl_set(const char *val, const struct kernel_param *kp)
{
	unsigned int value;
	int ret = kstrtouint(val, 10, &value);

	if (!ret)
		WRITE_ONCE(slim_walt_ctrl, !!value);
	return ret;
}

static const struct kernel_param_ops slim_walt_ctrl_ops = {
	.set = slim_walt_ctrl_set,
	.get = param_get_uint,
};
module_param_cb(slim_walt_ctrl, &slim_walt_ctrl_ops, &slim_walt_ctrl, 0644);

/* shorter than a tick there is nothing to sample; the limit above keeps the sums small */
static int slim_walt_window_set(const char *val, const struct kernel_param *kp)
{
	return param_set_uint_minmax(val, kp, 4, 1000);
}

static const struct kernel_param_ops slim_walt_window_ops = {
	.set = slim_walt_window_set,
	.get = param_get_uint,
};
module_param_cb(slim_walt_window_ms, &slim_walt_window_ops, &slim_walt_window_ms, 0644);

static int slim_walt_ctrl_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", READ_ONCE(slim_walt_ctrl));
	return 0;
}

static int slim_walt_ctrl_open(struct inode *inode, struct file *file)
{
	return single_open(file, slim_walt_ctrl_show, NULL);
}

static ssize_t slim_walt_ctrl_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
	unsigned int value;
	int ret;

	if (count > 7)
		return -EINVAL;
	ret = kstrtouint_from_user(ubuf, count, 10, &value);
	if (ret)
		return ret;
	WRITE_ONCE(slim_walt_ctrl, !!value);
	return count;
}

static const struct proc_ops slim_walt_ctrl_proc_ops = {
	.proc_open	= slim_walt_ctrl_open,
	.proc_read	= seq_read,
	.proc_write	= slim_walt_ctrl_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int slim_walt_window_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", READ_ONCE(slim_walt_window_ms));
	return 0;
}

static int slim_walt_busy_show(struct seq_file *m, void *v)
{
	bool on = READ_ONCE(slim_walt_ctrl);
	u64 now = sched_clock(), win = slim_walt_window();
	int cpu;

	for_each_possible_cpu(cpu) {
		struct sx_slim sw;

		slim_walt_snapshot(cpu, &sw);
		seq_printf(m, "%d %lu %lu %u %u %llu\n", cpu,
			   on ? sx_slim_share(&sw, now, win, 100) : 0,
			   on ? sx_slim_share(&sw, now, win, SCHED_CAPACITY_SCALE) : 0,
			   sw.prev_nr_avg, sw.prev_nr_max, sw.nr_windows);
	}
	return 0;
}

static int __init slim_walt_init(void)
{
	struct proc_dir_entry *dir;
	int cpu;

	for_each_possible_cpu(cpu)
		seqcount_init(&per_cpu(slim_walt_cpu, cpu).seq);

	dir = proc_mkdir("slim_sched", NULL);
	if (dir) {
		proc_create("walt_ctrl", 0644, dir, &slim_walt_ctrl_proc_ops);
		proc_create_single("walt_busy", 0444, dir, slim_walt_busy_show);
		proc_create_single("window_ms", 0444, dir, slim_walt_window_show);
	}
	return 0;
}
/* before the cpufreq drivers come: a governor may ask for the tick as soon as it starts */
core_initcall(slim_walt_init);
