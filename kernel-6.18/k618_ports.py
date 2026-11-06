#!/usr/bin/env python3
"""Apply the project memory, scheduling, power and LZ4 adaptations to ACK 6.18. Each edit validates its source text and is idempotent. LZ4_DEV or NB selects the pinned LZ4 source. See docs/PROVENANCE.en.md for source attribution."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
P515 = os.path.normpath(os.path.join(HERE, "..", "tools", "kernel-patches"))
LZ4_DEV = os.environ.get("LZ4_DEV", os.path.join(os.environ.get("NB", os.path.expanduser("~/xiaomi13-k618-build")), "lz4-dev"))
LZ4_PIN = "83376b665fe6742171407350bb51e963215d6088"	# kernel.yml's lz4 ref

CHANGES = []


def change(name, edits):
    CHANGES.append((name, edits))


# ---- psci_idle_resched: recheck pending work before entering firmware idle.
# 6.18's cpuidle_enter_state() still runs the governor's and the state's bookkeeping with interrupts
# off between call_cpuidle()'s last need_resched() look and the PSCI call; a wakeup sent in that
# window still costs the CPU PM notifiers, the firmware call and (deepest state) the cluster domain.
change("psci_idle_resched", [
    ("drivers/cpuidle/cpuidle-psci.c",
     "\tu32 state = states[idx];\n\tint ret;\n\n\tret = cpu_pm_enter();\n\tif (ret)\n\t\treturn -1;\n\n"
     "\t/* Do runtime PM to manage a hierarchical CPU toplogy. */\n",
     "\tu32 state = states[idx];\n\tint ret;\n\n"
     "\t/* k618 port psci_idle_resched: a task was woken onto this CPU meanwhile, no idle state for nothing */\n"
     "\tif (!s2idle && need_resched())\n\t\treturn -1;\n\n"
     "\tret = cpu_pm_enter();\n\tif (ret)\n\t\treturn -1;\n\n"
     "\t/* Do runtime PM to manage a hierarchical CPU toplogy. */\n", 1),
    ("drivers/cpuidle/cpuidle-psci.c",
     "\tu32 *state = __this_cpu_read(psci_cpuidle_data.psci_states);\n\n"
     "\treturn CPU_PM_CPU_IDLE_ENTER_PARAM_RCU(psci_cpu_suspend_enter, idx, state[idx]);\n",
     "\tu32 *state = __this_cpu_read(psci_cpuidle_data.psci_states);\n\n"
     "\t/* k618 port psci_idle_resched: as above */\n\tif (need_resched())\n\t\treturn -1;\n\n"
     "\treturn CPU_PM_CPU_IDLE_ENTER_PARAM_RCU(psci_cpu_suspend_enter, idx, state[idx]);\n", 1),
])

# ---- erofs_worker_prio (5.15: erofs_worker_prio.py, note 31). 6.18 still makes the per-CPU
# decompression threads SCHED_FIFO 1 (CONFIG_EROFS_FS_PCPU_KTHREAD_HIPRI=y, sched_set_fifo_low()),
# behind every other real-time thread of HyperOS; 3 as on 5.15 (wake-to-run p99.9 1.0-1.4 ->
# 0.17-0.31 ms there).
ZD = "fs/erofs/zdata.c"
change("erofs_worker_prio", [
    (ZD, "#include <linux/cpuhotplug.h>\n",
     "#include <linux/cpuhotplug.h>\n#include <linux/cpu.h>\n#include <linux/moduleparam.h>\n"
     "#include <uapi/linux/sched/types.h>\n", 1),
    (ZD, "#ifdef CONFIG_EROFS_FS_PCPU_KTHREAD\nstatic struct kthread_worker __rcu **z_erofs_pcpu_workers;\n",
     """#ifdef CONFIG_EROFS_FS_PCPU_KTHREAD
static struct kthread_worker __rcu **z_erofs_pcpu_workers;

/*
 * k618 port erofs_worker_prio: the SCHED_FIFO priority of the per-CPU
 * decompression threads (6.18 has 1, sched_set_fifo_low(): behind every other
 * real-time thread). Writing it changes the threads that exist.
 */
static unsigned int z_erofs_worker_prio = 3;

static void z_erofs_worker_set_prio(struct task_struct *task)
{
	struct sched_param sp = {
		.sched_priority = READ_ONCE(z_erofs_worker_prio),
	};

	WARN_ON_ONCE(sched_setscheduler_nocheck(task, SCHED_FIFO, &sp) != 0);
}

static int z_erofs_worker_prio_set(const char *val,
				   const struct kernel_param *kp)
{
	unsigned int prio, cpu;
	int err = kstrtouint(val, 0, &prio);

	if (err)
		return err;
	if (prio < 1 || prio > MAX_RT_PRIO - 1)
		return -EINVAL;
	if (!z_erofs_pcpu_workers) {	/* on the command line: no threads yet */
		z_erofs_worker_prio = prio;
		return 0;
	}
	/* the threads come and go with their CPUs: keep the callbacks out */
	cpus_read_lock();
	WRITE_ONCE(z_erofs_worker_prio, prio);
	for_each_possible_cpu(cpu) {
		struct kthread_worker *worker = rcu_dereference_protected(
					z_erofs_pcpu_workers[cpu], 1);

		if (worker)
			z_erofs_worker_set_prio(worker->task);
	}
	cpus_read_unlock();
	return 0;
}

static const struct kernel_param_ops z_erofs_worker_prio_ops = {
	.set = z_erofs_worker_prio_set,
	.get = param_get_uint,
};
module_param_cb(worker_prio, &z_erofs_worker_prio_ops, &z_erofs_worker_prio,
		0644);
""", 1),
    (ZD, "\t\t\tkthread_destroy_worker(worker);\n\t}\n\tkfree(z_erofs_pcpu_workers);\n}\n",
     "\t\t\tkthread_destroy_worker(worker);\n\t}\n\tkfree(z_erofs_pcpu_workers);\n"
     "\tz_erofs_pcpu_workers = NULL;\t/* k618 port erofs_worker_prio */\n}\n", 1),
    (ZD, "\tif (IS_ENABLED(CONFIG_EROFS_FS_PCPU_KTHREAD_HIPRI))\n\t\tsched_set_fifo_low(worker->task);\n",
     "\tif (IS_ENABLED(CONFIG_EROFS_FS_PCPU_KTHREAD_HIPRI))\n"
     "\t\tz_erofs_worker_set_prio(worker->task);\t/* k618 port erofs_worker_prio */\n", 1),
])

# ---- binder_debug_off (5.15: binder_debug_off.py, note 23). 6.18 made binder_debug() a function:
# each of its 69 calls (eight of them in the lock helpers every proc / node lock goes through)
# sets up a variadic call, calls, loads the mask and tests it -- more than 5.15's macro did. The
# classes off as shipped compile out; the three on as shipped work as before.
change("binder_debug_off", [
    ("drivers/android/binder.c",
     """static __printf(2, 3) void binder_debug(int mask, const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	if (binder_debug_mask & mask) {
		va_start(args, format);
		vaf.va = &args;
		vaf.fmt = format;
		pr_info_ratelimited("%pV", &vaf);
		va_end(args);
	}
}
""",
     """/*
 * k618 port binder_debug_off: the messages of the classes that are off unless debug_mask asks for
 * them are compiled out -- each was a variadic call, a load of the mask and a test where it stood,
 * at every lock and unlock, all along a transaction. What debug_mask has on as shipped is as it
 * was; another class's bit in it prints nothing.
 */
#define BINDER_DEBUG_BUILT_IN \\
	(BINDER_DEBUG_USER_ERROR | BINDER_DEBUG_FAILED_TRANSACTION | BINDER_DEBUG_DEAD_TRANSACTION)

static __printf(1, 2) void __binder_debug(const char *format, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, format);
	vaf.va = &args;
	vaf.fmt = format;
	pr_info_ratelimited("%pV", &vaf);
	va_end(args);
}

#define binder_debug(mask, x...) \\
	do { \\
		if ((((mask) & BINDER_DEBUG_BUILT_IN) != 0) && (binder_debug_mask & (mask))) \\
			__binder_debug(x); \\
	} while (0)
""", 1),
    ("drivers/android/binder_alloc.c",
     """#define binder_alloc_debug(mask, x...) \\
	do { \\
		if (binder_alloc_debug_mask & mask) \\
			pr_info_ratelimited(x); \\
	} while (0)
""",
     """/* k618 port binder_debug_off: as binder.c's binder_debug(), only the class on as shipped */
#define BINDER_ALLOC_DEBUG_BUILT_IN BINDER_DEBUG_USER_ERROR

#define binder_alloc_debug(mask, x...) \\
	do { \\
		if ((((mask) & BINDER_ALLOC_DEBUG_BUILT_IN) != 0) && (binder_alloc_debug_mask & (mask))) \\
			pr_info_ratelimited(x); \\
	} while (0)
""", 1),
])

# ---- loop_wq_highpri (5.15: loop_wq_highpri.py, note 25). 6.18's loop workqueue is still
# WQ_UNBOUND | WQ_FREEZABLE (nice 0 workers) on the path of every APEX read.
LOOP = "drivers/block/loop.c"
change("loop_wq_highpri", [
    (LOOP, "static int max_part;\n",
     "static int max_part;\n/*\n * k618 port loop_wq_highpri: a loop device's workers run at nice -20, as its\n"
     " * thread did before 5.13 made it a workqueue. Read when a device is set up; one\n"
     " * that exists is changed through /sys/devices/virtual/workqueue/loopN/nice.\n */\n"
     "static bool loop_hipri_wq = true;\n", 1),
    (LOOP, "\t\tlo->workqueue = alloc_workqueue(\"loop%d\",\n\t\t\t\t\t\tWQ_UNBOUND | WQ_FREEZABLE,\n"
     "\t\t\t\t\t\t0, lo->lo_number);\n",
     "\t\tlo->workqueue = alloc_workqueue(\"loop%d\",\n\t\t\t\t\t\tWQ_UNBOUND | WQ_FREEZABLE | WQ_SYSFS |\n"
     "\t\t\t\t\t\t(READ_ONCE(loop_hipri_wq) ? WQ_HIGHPRI : 0),\n\t\t\t\t\t\t0, lo->lo_number);\n", 1),
    (LOOP, "module_param(max_part, int, 0444);\nMODULE_PARM_DESC(max_part, \"Maximum number of partitions per loop device\");\n",
     "module_param(max_part, int, 0444);\nMODULE_PARM_DESC(max_part, \"Maximum number of partitions per loop device\");\n"
     "module_param_named(hipri_wq, loop_hipri_wq, bool, 0644);\n"
     "MODULE_PARM_DESC(hipri_wq, \"Loop devices set up from now on get workers at nice -20\");\n", 1),
])

# ---- sync_file_info (5.15: sync_file_info.py). 6.18's SYNC_IOC_FILE_INFO still kzalloc()s the
# records' array and copies two names per fence (now under RCU) plus formats the file's name, on
# every frame's fences; nothing in the frame path reads a name.
SF = "drivers/dma-buf/sync_file.c"
change("sync_file_info", [
    # moduleparam.h, not module.h: with module.h struct module is defined in this unit's DWARF and
    # gendwarfksyms' CRC of sync_file_create() (which reaches it through struct file) changes
    (SF, "#include <linux/kernel.h>\n#include <linux/poll.h>\n",
     "#include <linux/kernel.h>\n#include <linux/moduleparam.h>\n#include <linux/poll.h>\n", 1),
    (SF, "static const struct file_operations sync_file_fops;\n",
     "static const struct file_operations sync_file_fops;\n\n/*\n"
     " * k618 port sync_file_info -- omit_names: SYNC_IOC_FILE_INFO leaves the file's\n"
     " * name and each fence's driver and timeline name empty. User space asks for a\n"
     " * fence's state and signal time with it on every frame and reads no name.\n */\n"
     "static bool sync_file_omit_names = true;\nmodule_param_named(omit_names, sync_file_omit_names, bool, 0644);\n", 1),
    (SF, """static int sync_fill_fence_info(struct dma_fence *fence,
				 struct sync_fence_info *info)
{
	const char __rcu *timeline;
	const char __rcu *driver;

	rcu_read_lock();

	driver = dma_fence_driver_name(fence);
	timeline = dma_fence_timeline_name(fence);

	strscpy(info->obj_name, rcu_dereference(timeline),
		sizeof(info->obj_name));
	strscpy(info->driver_name, rcu_dereference(driver),
		sizeof(info->driver_name));
""",
     """static int sync_fill_fence_info(struct dma_fence *fence,
				 struct sync_fence_info *info, bool names)
{
	const char __rcu *timeline;
	const char __rcu *driver;

	rcu_read_lock();

	if (names) {	/* k618 port sync_file_info */
		driver = dma_fence_driver_name(fence);
		timeline = dma_fence_timeline_name(fence);

		strscpy(info->obj_name, rcu_dereference(timeline),
			sizeof(info->obj_name));
		strscpy(info->driver_name, rcu_dereference(driver),
			sizeof(info->driver_name));
	}
""", 1),
    (SF, """	struct sync_fence_info *fence_info = NULL;
	struct dma_fence_unwrap iter;
	struct sync_file_info info;
	unsigned int num_fences;
	struct dma_fence *fence;
	int ret;
	__u32 size;
""",
     """	bool names = !READ_ONCE(sync_file_omit_names);	/* k618 port sync_file_info */
	struct sync_fence_info __user *ufence_info;
	struct dma_fence_unwrap iter;
	struct sync_file_info info;
	unsigned int num_fences;
	struct dma_fence *fence;
	int ret = 0;
""", 1),
    (SF, """	size = num_fences * sizeof(*fence_info);
	fence_info = kzalloc(size, GFP_KERNEL);
	if (!fence_info)
		return -ENOMEM;

	num_fences = 0;
	dma_fence_unwrap_for_each(fence, &iter, sync_file->fence) {
		int status;

		status = sync_fill_fence_info(fence, &fence_info[num_fences++]);
		info.status = info.status <= 0 ? info.status : status;
	}

	if (copy_to_user(u64_to_user_ptr(info.sync_fence_info), fence_info,
			 size)) {
		ret = -EFAULT;
		goto out;
	}

no_fences:
	sync_file_get_name(sync_file, info.name, sizeof(info.name));
	info.num_fences = num_fences;

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		ret = -EFAULT;
	else
		ret = 0;

out:
	kfree(fence_info);

	return ret;
}
""",
     """	/*
	 * k618 port sync_file_info: one record at a time, from the stack, no
	 * allocation for the array. The walk is never left early -- the iterator
	 * holds a reference that only its end drops -- so a fault is remembered and
	 * the rest skipped. Chain links that signalled since the count above are
	 * gone from the walk: it can only have become shorter than the caller's room.
	 */
	ufence_info = u64_to_user_ptr(info.sync_fence_info);
	num_fences = 0;
	dma_fence_unwrap_for_each(fence, &iter, sync_file->fence) {
		struct sync_fence_info fence_info;
		int status;

		memset(&fence_info, 0, sizeof(fence_info));
		status = sync_fill_fence_info(fence, &fence_info, names);
		info.status = info.status <= 0 ? info.status : status;
		if (!ret && copy_to_user(&ufence_info[num_fences], &fence_info,
					 sizeof(fence_info)))
			ret = -EFAULT;
		num_fences++;
	}
	if (ret)
		return ret;

no_fences:
	if (names)
		sync_file_get_name(sync_file, info.name, sizeof(info.name));
	else
		memset(info.name, 0, sizeof(info.name));
	info.num_fences = num_fences;

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}
""", 1),
])

# ---- throttle_exempt (5.15: throttle_exempt.py, note 25). 6.18's throttle_direct_reclaim() still
# puts every user thread to sleep on pfmemalloc_wait below half the min watermark; real-time and
# negative-nice threads (UI, render, audio, SurfaceFlinger) go on reclaiming for themselves.
VS = "mm/vmscan.c"
change("throttle_exempt", [
    (VS, "#include <linux/sched/mm.h>\n", "#include <linux/sched/mm.h>\n#include <linux/sched/rt.h>\n", 1),
    (VS, "static bool allow_direct_reclaim(pg_data_t *pgdat)\n{\n",
     """/*
 * k618 port throttle_exempt -- vm.throttle_exempt: a thread with a real-time policy or a negative
 * nice value is not put to sleep by throttle_direct_reclaim() when the reserves run low; it
 * reclaims for itself right away, as kernel threads do (what OnePlus' kernels do for real-time and
 * "UX" tasks). vm.throttle_exempt_count: how often that happened.
 */
static int vm_throttle_exempt = 1;
static unsigned long vm_throttle_exempt_count;

static bool allow_direct_reclaim(pg_data_t *pgdat)
{
""", 1),
    (VS, "\ttrace_android_vh_throttle_direct_reclaim_bypass(&bypass);\n\tif (bypass)\n\t\tgoto out;\n",
     "\ttrace_android_vh_throttle_direct_reclaim_bypass(&bypass);\n\tif (bypass)\n\t\tgoto out;\n\n"
     "\t/* k618 port throttle_exempt */\n"
     "\tif (READ_ONCE(vm_throttle_exempt) && (rt_or_dl_task(current) || task_nice(current) < 0)) {\n"
     "\t\tvm_throttle_exempt_count++;\n\t\tgoto out;\n\t}\n", 1),
    (VS, "static const struct ctl_table vmscan_sysctl_table[] = {\n\t{\n\t\t.procname\t= \"swappiness\",\n",
     """static const struct ctl_table vmscan_sysctl_table[] = {
	{	/* k618 port throttle_exempt */
		.procname	= "throttle_exempt",
		.data		= &vm_throttle_exempt,
		.maxlen		= sizeof(vm_throttle_exempt),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "throttle_exempt_count",
		.data		= &vm_throttle_exempt_count,
		.maxlen		= sizeof(vm_throttle_exempt_count),
		.mode		= 0444,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "swappiness",
""", 1),
])

# ---- kill_reap (5.15: kill_reap.py, note 19). 6.18 still has android_vh_killed_process in
# group_send_sig_info() and add_to_oom_reaper(); the framework's kills (system_server cleaning up,
# an app swiped away) still only send SIGKILL, and a background app's threads leave slowly.
change("kill_reap", [
    ("include/linux/oom.h", "void add_to_oom_reaper(struct task_struct *p);\n",
     "void add_to_oom_reaper(struct task_struct *p);\nextern int sysctl_reap_killed_mb;\t/* k618 port kill_reap */\n", 1),
    ("mm/oom_kill.c", "static const struct ctl_table vm_oom_kill_table[] = {\n\t{\n\t\t.procname\t= \"panic_on_oom\",\n",
     """/*
 * k618 port kill_reap -- vm.reap_killed_mb: a process killed by another one is
 * handed to the OOM reaper when it has more than this much anonymous memory and
 * swap (in MB), so that the memory comes back without waiting for all its
 * threads to exit (kernel/signal.c). 0: never.
 */
int sysctl_reap_killed_mb __read_mostly = 32;

static const struct ctl_table vm_oom_kill_table[] = {
	{
		.procname	= "reap_killed_mb",
		.data		= &sysctl_reap_killed_mb,
		.maxlen		= sizeof(sysctl_reap_killed_mb),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
	},
	{
		.procname	= "panic_on_oom",
""", 1),
    ("kernel/signal.c",
     "/*\n * send signal info to all the members of a thread group or to the\n"
     " * individual thread if type == PIDTYPE_PID.\n */\nint group_send_sig_info(int sig, struct kernel_siginfo *info,\n",
     """/* k618 port kill_reap: see vm.reap_killed_mb in mm/oom_kill.c */
static bool reap_killed_process(struct task_struct *p)
{
	int mb = READ_ONCE(sysctl_reap_killed_mb);
	unsigned long pages = 0;
	struct task_struct *t;

	if (mb <= 0 || same_thread_group(current, p))
		return false;
	t = find_lock_task_mm(p);
	if (t) {
		pages = get_mm_counter(t->mm, MM_ANONPAGES) +
			get_mm_counter(t->mm, MM_SWAPENTS);
		task_unlock(t);
	}
	return pages > ((unsigned long)mb << (20 - PAGE_SHIFT));
}

/*
 * send signal info to all the members of a thread group or to the
 * individual thread if type == PIDTYPE_PID.
 */
int group_send_sig_info(int sig, struct kernel_siginfo *info,
""", 1),
    ("kernel/signal.c",
     "\t\t\ttrace_android_vh_killed_process(current, p, &reap);\n\t\t\tif (reap)\n\t\t\t\tadd_to_oom_reaper(p);\n",
     "\t\t\ttrace_android_vh_killed_process(current, p, &reap);\n"
     "\t\t\tif (!reap)\t/* k618 port kill_reap */\n\t\t\t\treap = reap_killed_process(p);\n"
     "\t\t\tif (reap)\n\t\t\t\tadd_to_oom_reaper(p);\n", 1),
])

# ---- nohz.nr_cpus goes (mainline 7.0 6b67c8a72e56, 94e70734b4d0, 5d86d542f68f; 5.15:
# sched_fair_backports.py). 6.18 still counts the tick-stopped CPUs in one atomic all CPUs share,
# written at every idle entry and exit; the idle mask has the same information.
FAIR = "kernel/sched/fair.c"
change("nohz_nr_cpus", [
    (FAIR, "\tcpumask_var_t idle_cpus_mask;\n\tatomic_t nr_cpus;\n\tint has_blocked;",
     "\tcpumask_var_t idle_cpus_mask;\n\tint has_blocked;", 1),
    (FAIR, """	nohz_balance_exit_idle(rq);

	/*
	 * None are in tickless mode and hence no need for NOHZ idle load
	 * balancing:
	 */
	if (likely(!atomic_read(&nohz.nr_cpus)))
		return;

	if (READ_ONCE(nohz.has_blocked) &&
	    time_after(now, READ_ONCE(nohz.next_blocked)))
		flags = NOHZ_STATS_KICK;

	if (time_before(now, nohz.next_balance))
		goto out;
""",
     """	nohz_balance_exit_idle(rq);

	if (READ_ONCE(nohz.has_blocked) &&
	    time_after(now, READ_ONCE(nohz.next_blocked)))
		flags = NOHZ_STATS_KICK;

	/*
	 * Most of the time system is not 100% busy. i.e nohz.nr_cpus > 0
	 * Skip the read if time is not due.
	 *
	 * If none are in tickless mode, there maybe a narrow window
	 * (28 jiffies, HZ=1000) where flags maybe set and kick_ilb called.
	 * But idle load balancing is not done as find_new_ilb fails.
	 * That's very rare. So read nohz.nr_cpus only if time is due.
	 */
	if (time_before(now, nohz.next_balance))
		goto out;

	/*
	 * None are in tickless mode and hence no need for NOHZ idle load
	 * balancing (k618 port: mainline 7.0's nohz.nr_cpus removal)
	 */
	if (unlikely(cpumask_empty(nohz.idle_cpus_mask)))
		return;
""", 1),
    (FAIR, "\tcpumask_clear_cpu(rq->cpu, nohz.idle_cpus_mask);\n\tatomic_dec(&nohz.nr_cpus);\n",
     "\tcpumask_clear_cpu(rq->cpu, nohz.idle_cpus_mask);\n", 1),
    (FAIR, "\tcpumask_set_cpu(cpu, nohz.idle_cpus_mask);\n\tatomic_inc(&nohz.nr_cpus);\n",
     "\tcpumask_set_cpu(cpu, nohz.idle_cpus_mask);\n", 1),
])

# ---- avg_idle (mainline 7.0 4b603f1551a7 + 7.3 c6dcd97c8be7; 5.15: avg_idle.py). 6.18 still
# updates rq->avg_idle only in ttwu_do_activate(): an idle spell ended by a migration, fork or exec
# is not counted and its stamp stays, so the next wakeup counts busy time as idle. avg_idle decides
# whether a CPU about to go idle tries newidle balancing at all (sched_balance_newidle()).
change("avg_idle", [
    ("kernel/sched/core.c", """		rq_unpin_lock(rq, rf);
		p->sched_class->task_woken(rq, p);
		rq_repin_lock(rq, rf);
	}

	if (rq->idle_stamp) {
		u64 delta = rq_clock(rq) - rq->idle_stamp;
		u64 max = 2*rq->max_idle_balance_cost;

		update_avg(&rq->avg_idle, delta);

		if (rq->avg_idle > max)
			rq->avg_idle = max;

		rq->idle_stamp = 0;
	}
}
""",
     """		rq_unpin_lock(rq, rf);
		p->sched_class->task_woken(rq, p);
		rq_repin_lock(rq, rf);
	}
}

/*
 * k618 port avg_idle: the CPU stops being idle, however its new task got there
 * (mainline 7.0 4b603f1551a7 and 7.3 c6dcd97c8be7): the idle spell since the
 * stamp counts into avg_idle; without a stamp there is nothing to count.
 */
void update_rq_avg_idle(struct rq *rq)
{
	u64 idle_stamp = rq->idle_stamp;
	u64 delta, max;

	if (!idle_stamp)
		return;

	delta = rq_clock(rq) - idle_stamp;

	update_avg(&rq->avg_idle, delta);

	max = 2 * rq->max_idle_balance_cost;
	if (rq->avg_idle > max)
		rq->avg_idle = max;

	rq->idle_stamp = 0;
}
""", 1),
    ("kernel/sched/idle.c", """static void put_prev_task_idle(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
	update_curr_idle(rq);
	scx_update_idle(rq, false, true);
}
""",
     """static void put_prev_task_idle(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
	update_curr_idle(rq);
	scx_update_idle(rq, false, true);
	update_rq_avg_idle(rq);		/* k618 port avg_idle */
}
""", 1),
    ("kernel/sched/sched.h", "extern void update_rq_clock(struct rq *rq);\n",
     "extern void update_rq_avg_idle(struct rq *rq);\t/* k618 port avg_idle */\nextern void update_rq_clock(struct rq *rq);\n", 1),
])


# ---- rmap_stop (5.15: rmap_stop.py, note 20; Huawei's CONFIG_STOP_PAGE_REF). The phone runs the
# classic LRU on 6.18 too (lru_gen/enabled reads 0x0000), and 6.18's folio_referenced() still visits
# every mapping of a folio -- for a library every app maps, hundreds of page tables -- while the two
# reclaim callers act on the first one or two references. folio_referenced() itself, exported,
# keeps its signature (and so its CRC) and always walks in full; reclaim uses folio_referenced_why().
RM = "mm/rmap.c"
change("rmap_stop", [
    ("mm/internal.h", "bool folio_isolate_lru(struct folio *folio);\n",
     """/*
 * k618 port rmap_stop: why reclaim asks for a folio's references (folio_referenced_why(),
 * mm/rmap.c): with vm.rmap_walk_stop the walk over the folio's mappings ends as soon as what
 * the caller will do with the folio can no longer change.
 */
enum folio_ref_why {
	FOLIO_REF_ALL,		/* every mapping, as folio_referenced() */
	FOLIO_REF_ACTIVE,	/* shrink_active_list(): keep the folio on the active list? */
	FOLIO_REF_INACTIVE,	/* folio_check_references(): activate the folio? */
};
int folio_referenced_why(struct folio *folio, enum folio_ref_why why, int is_locked,
			 struct mem_cgroup *memcg, vm_flags_t *vm_flags);

bool folio_isolate_lru(struct folio *folio);
""", 1),
    (RM, "#include <linux/userfaultfd_k.h>\n",
     "#include <linux/userfaultfd_k.h>\n#include <linux/random.h>\n#include <linux/sysctl.h>\n", 1),
    (RM, """struct folio_referenced_arg {
	int mapcount;
	int referenced;
	vm_flags_t vm_flags;
	struct mem_cgroup *memcg;
};
""",
     """struct folio_referenced_arg {
	int mapcount;
	int referenced;
	vm_flags_t vm_flags;
	struct mem_cgroup *memcg;
	enum folio_ref_why stop;	/* k618 port rmap_stop */
};

/*
 * k618 port rmap_stop -- vm.rmap_walk_stop: reclaim's two looks at a folio's
 * references end the walk over its mappings once what they decide is settled
 * (Huawei's CONFIG_STOP_PAGE_REF; on 5.15 tools/kernel-patches/rmap_stop.py).
 * A folio of a library that every process maps has hundreds of mappings to
 * visit, and the first one or two of them that referenced it decide.
 * vm.rmap_walk_stops: walks ended early (a statistic: not exact).
 */
static int sysctl_rmap_walk_stop __read_mostly = 1;
static unsigned long rmap_walk_stops;

static enum folio_ref_why folio_referenced_stop(struct folio *folio,
						enum folio_ref_why why)
{
	if (why == FOLIO_REF_ALL || !READ_ONCE(sysctl_rmap_walk_stop) ||
	    lru_gen_enabled())
		return FOLIO_REF_ALL;
	/* the active list keeps only file folios for having been referenced */
	if (why == FOLIO_REF_ACTIVE && folio_test_swapbacked(folio))
		return FOLIO_REF_ALL;
	/*
	 * A walk that ends early leaves the accessed bits of the mappings it did
	 * not get to, and they would count the next time although the folio may
	 * not have been touched since. Every other walk, on average, goes all the
	 * way and clears them.
	 */
	if (get_random_u32() & 1)
		return FOLIO_REF_ALL;
	return why;
}

/* is what the caller will do with the folio settled by the references so far? */
static bool folio_referenced_settled(struct folio *folio,
				     struct folio_referenced_arg *pra)
{
	if (pra->referenced <= 0)
		return false;
	/* a file folio referenced through an executable mapping stays active */
	if (pra->stop == FOLIO_REF_ACTIVE)
		return pra->vm_flags & VM_EXEC;
	/* what makes folio_check_references() activate a folio */
	if (folio_test_referenced(folio) || pra->referenced > 1)
		return true;
	return (pra->vm_flags & VM_EXEC) && !folio_test_swapbacked(folio);
}

static const struct ctl_table rmap_stop_sysctls[] = {
	{
		.procname	= "rmap_walk_stop",
		.data		= &sysctl_rmap_walk_stop,
		.maxlen		= sizeof(sysctl_rmap_walk_stop),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "rmap_walk_stops",
		.data		= &rmap_walk_stops,
		.maxlen		= sizeof(rmap_walk_stops),
		.mode		= 0444,
		.proc_handler	= proc_doulongvec_minmax,
	},
};

static int __init rmap_stop_sysctl_init(void)
{
	register_sysctl_init("vm", rmap_stop_sysctls);
	return 0;
}
late_initcall(rmap_stop_sysctl_init);
""", 1),
    (RM, "\tif (!pra->mapcount)\n\t\treturn false; /* To break the loop */\n\n\treturn true;\n}\n",
     "\tif (!pra->mapcount)\n\t\treturn false; /* To break the loop */\n\n"
     "\t/* k618 port rmap_stop */\n\tif (pra->stop != FOLIO_REF_ALL && folio_referenced_settled(folio, pra)) {\n"
     "\t\trmap_walk_stops++;\n\t\treturn false;\n\t}\n\n\treturn true;\n}\n", 1),
    (RM, "int folio_referenced(struct folio *folio, int is_locked,\n\t\t     struct mem_cgroup *memcg, vm_flags_t *vm_flags)\n{\n\tbool we_locked = false;\n",
     "int folio_referenced_why(struct folio *folio, enum folio_ref_why why, int is_locked,\n"
     "\t\t\t struct mem_cgroup *memcg, vm_flags_t *vm_flags)\n{\n\tbool we_locked = false;\n", 1),
    (RM, "\trmap_walk(folio, &rwc);\n\t*vm_flags = pra.vm_flags;\n",
     "\tpra.stop = folio_referenced_stop(folio, why);\t/* k618 port rmap_stop */\n\trmap_walk(folio, &rwc);\n\t*vm_flags = pra.vm_flags;\n", 1),
    (RM, "\treturn rwc.contended ? -1 : pra.referenced;\n}\nEXPORT_SYMBOL_GPL(folio_referenced);\n",
     """	return rwc.contended ? -1 : pra.referenced;
}

/* k618 port rmap_stop: every mapping -- what modules get, all there was before vm.rmap_walk_stop */
int folio_referenced(struct folio *folio, int is_locked,
		     struct mem_cgroup *memcg, vm_flags_t *vm_flags)
{
	return folio_referenced_why(folio, FOLIO_REF_ALL, is_locked, memcg, vm_flags);
}
EXPORT_SYMBOL_GPL(folio_referenced);
""", 1),
    (VS, "\treferenced_ptes = folio_referenced(folio, 1, sc->target_mem_cgroup,\n\t\t\t\t\t   &vm_flags);\n",
     "\treferenced_ptes = folio_referenced_why(folio, FOLIO_REF_INACTIVE, 1,\t/* k618 port rmap_stop */\n"
     "\t\t\t\t\t       sc->target_mem_cgroup, &vm_flags);\n", 1),
    (VS, "\t\tif (folio_referenced(folio, 0, sc->target_mem_cgroup,\n\t\t\t\t     &vm_flags) != 0) {\n",
     "\t\tif (folio_referenced_why(folio, FOLIO_REF_ACTIVE, 0,\t/* k618 port rmap_stop */\n"
     "\t\t\t\t\t sc->target_mem_cgroup, &vm_flags) != 0) {\n", 1),
])

# ---- rmap_mapcount_keep 32 (5.15: rmap_mapcount.py, note 20; Xiaomi's mi_rmap_efficiency rule). A
# folio mapped by 32 or more processes is taken as in use without the walk, at both places that ask;
# once a run has had to try harder, at most one in 16 of those it asked about. Classic LRU only:
# with MGLRU on it steps aside (shrink_active_list() is not used then at all).
change("rmap_mapcount_keep", [
    (VS, "\t/* Number of pages freed so far during a call to shrink_zones() */\n\tunsigned long nr_reclaimed;\n",
     "\t/* Number of pages freed so far during a call to shrink_zones() */\n\tunsigned long nr_reclaimed;\n\n"
     "\t/*\n\t * k618 port rmap_mapcount_keep: folios this run asked about, and how many of\n"
     "\t * them it kept for the number of processes that map them\n\t */\n"
     "\tunsigned long nr_mapcount_asked;\n\tunsigned long nr_mapcount_kept;\n", 1),
    (VS, "static enum folio_references folio_check_references(struct folio *folio,\n\t\t\t\t\t\t  struct scan_control *sc)\n{\n",
     """/*
 * k618 port rmap_mapcount_keep -- vm.rmap_mapcount_keep: a folio mapped by that
 * many processes or more is taken as in use -- activated, or left on the active
 * list -- without the walk over its mappings that would find out (Xiaomi's
 * mi_rmap_efficiency rule, here for both places that ask; on 5.15
 * tools/kernel-patches/rmap_mapcount.py). vm.rmap_mapcount_kept: folios kept.
 */
static int sysctl_rmap_mapcount_keep __read_mostly = 32;
static unsigned long rmap_mapcount_kept;	/* a statistic: not exact */

static bool rmap_mapcount_keeps(struct folio *folio, struct scan_control *sc)
{
	int min = READ_ONCE(sysctl_rmap_mapcount_keep);

	if (min < 2 || lru_gen_enabled())
		return false;
	sc->nr_mapcount_asked++;
	if (folio_mapcount(folio) < min)
		return false;
	/*
	 * Once this run has had to try harder, not more than one folio in 16 of
	 * those it asked about: memory that many processes share cannot starve it.
	 */
	if (sc->priority < DEF_PRIORITY - 2 &&
	    sc->nr_mapcount_kept > (sc->nr_mapcount_asked >> 4))
		return false;
	sc->nr_mapcount_kept++;
	rmap_mapcount_kept++;
	return true;
}

static const struct ctl_table rmap_mapcount_sysctls[] = {
	{
		.procname	= "rmap_mapcount_keep",
		.data		= &sysctl_rmap_mapcount_keep,
		.maxlen		= sizeof(sysctl_rmap_mapcount_keep),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
	},
	{
		.procname	= "rmap_mapcount_kept",
		.data		= &rmap_mapcount_kept,
		.maxlen		= sizeof(rmap_mapcount_kept),
		.mode		= 0444,
		.proc_handler	= proc_doulongvec_minmax,
	},
};

static int __init rmap_mapcount_sysctl_init(void)
{
	register_sysctl_init("vm", rmap_mapcount_sysctls);
	return 0;
}
late_initcall(rmap_mapcount_sysctl_init);

static enum folio_references folio_check_references(struct folio *folio,
						  struct scan_control *sc)
{
""", 1),
    (VS, "\ttrace_android_vh_check_folio_look_around_ref(folio, &ret);\n\tif (ret)\n\t\treturn ret;\n",
     "\ttrace_android_vh_check_folio_look_around_ref(folio, &ret);\n\tif (ret)\n\t\treturn ret;\n\n"
     "\t/* k618 port rmap_mapcount_keep: so many processes map it that one of them uses it */\n"
     "\tif (rmap_mapcount_keeps(folio, sc))\n\t\treturn FOLIOREF_ACTIVATE;\n", 1),
    (VS, "\t\tif (unlikely(should_protect)) {\n\t\t\tnr_rotated += folio_nr_pages(folio);\n"
     "\t\t\tlist_add(&folio->lru, &l_active);\n\t\t\tcontinue;\n\t\t}\n",
     "\t\tif (unlikely(should_protect)) {\n\t\t\tnr_rotated += folio_nr_pages(folio);\n"
     "\t\t\tlist_add(&folio->lru, &l_active);\n\t\t\tcontinue;\n\t\t}\n\n"
     "\t\t/* k618 port rmap_mapcount_keep: it stays active without a look at each mapping */\n"
     "\t\tif (rmap_mapcount_keeps(folio, sc)) {\n\t\t\tnr_rotated += folio_nr_pages(folio);\n"
     "\t\t\tlist_add(&folio->lru, &l_active);\n\t\t\tcontinue;\n\t\t}\n", 1),
])


# ---- zram_tiered: LZ4 with background ZSTD recompression. HyperOS sizes zram
# itself (system_perf_init, 12 GB here) and writes no algorithm, so zram ran on the driver's default,
# lzo-rle; the 13 Pro's post_boot writes lz4 only with ro.config.low_ram. Here, in the driver (a GKI
# module this build makes, GKIMODS in k618_timg.sh): lz4 on the way in (CONFIG_ZRAM_DEF_COMP_LZ4,
# k618_gki.sh), zstd as every device's second algorithm, a round every ten minutes awake that
# recompresses what stayed untouched since the round before, and the size taken from disksize_mb.
# All four are module parameters (/sys/module/zram/parameters/), for the module's switches.
ZR = "drivers/block/zram/zram_drv.c"
change("zram_tiered", [
    (ZR, "#include <linux/kernel_read_file.h>\n",
     "#include <linux/kernel_read_file.h>\n#include <linux/freezer.h>\n#include <linux/kthread.h>\n"
     "#include <linux/sched/topology.h>\n", 1),
    (ZR, "static const char *default_compressor = CONFIG_ZRAM_DEF_COMP;\n",
     """static const char *default_compressor = CONFIG_ZRAM_DEF_COMP;

/*
 * k618 zram_tiered (implemented in the driver; CONFIG_ZRAM_DEF_COMP
 * is lz4 in this build): pages are compressed with lz4 on the way in, and what then stays untouched
 * from one round to the next is compressed again with recomp_default, in the background.
 *  recomp_default    the second algorithm a device starts with, empty for none (read when a
 *                    device is created and when it is reset)
 *  recomp_idle_secs  seconds the phone is awake between two rounds (0: no rounds); a write
 *                    takes effect at once
 *  recomp_now        write 1: a round now (the module's WebUI, the QEMU test)
 *  disksize_mb       the size a device gets whatever user space writes to disksize (0: as written)
 *  recomp_rounds, recomp_ms   rounds run since boot and how long they took (read-only)
 */
static char recomp_default[16] = "zstd";
module_param_string(recomp_default, recomp_default, sizeof(recomp_default), 0644);
static DECLARE_WAIT_QUEUE_HEAD(zram_recomp_wait);
static atomic_t recomp_kick = ATOMIC_INIT(0), recomp_now_req = ATOMIC_INIT(0);
static unsigned int recomp_idle_secs = 600;

static int recomp_idle_secs_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (!ret) {
		atomic_set(&recomp_kick, 1);
		wake_up(&zram_recomp_wait);
	}
	return ret;
}

static const struct kernel_param_ops recomp_idle_secs_ops = {
	.set = recomp_idle_secs_set,
	.get = param_get_uint,
};
module_param_cb(recomp_idle_secs, &recomp_idle_secs_ops, &recomp_idle_secs, 0644);

static int recomp_now_set(const char *val, const struct kernel_param *kp)
{
	bool now;
	int ret = kstrtobool(val, &now);

	if (!ret && now) {
		atomic_set(&recomp_now_req, 1);
		wake_up(&zram_recomp_wait);
	}
	return ret;
}

static const struct kernel_param_ops recomp_now_ops = {
	.set = recomp_now_set,
};
module_param_cb(recomp_now, &recomp_now_ops, NULL, 0200);
static unsigned int disksize_mb = 16384;
module_param(disksize_mb, uint, 0644);
static unsigned long recomp_rounds, recomp_ms;
module_param(recomp_rounds, ulong, 0444);
module_param(recomp_ms, ulong, 0444);
""", 1),
    (ZR, """static void comp_algorithm_set(struct zram *zram, u32 prio, const char *alg)
{
	/* Do not free statically defined compression algorithms */
	if (zram->comp_algs[prio] != default_compressor)
		kfree(zram->comp_algs[prio]);

	zram->comp_algs[prio] = alg;
}
""",
     """static void comp_algorithm_set(struct zram *zram, u32 prio, const char *alg)
{
	/* Do not free statically defined compression algorithms */
	if (zram->comp_algs[prio] != default_compressor)
		kfree(zram->comp_algs[prio]);

	zram->comp_algs[prio] = alg;
}

/* k618 port zram_tiered: a device starts with recomp_default as its second algorithm */
static void zram_set_recomp_default(struct zram *zram)
{
#ifdef CONFIG_ZRAM_MULTI_COMP
	char buf[sizeof(recomp_default)];
	char *name, *alg;

	strscpy(buf, recomp_default, sizeof(buf));
	name = strim(buf);	/* a write to the parameter brings its newline */
	if (!*name)
		return;
	if (!zcomp_available_algorithm(name)) {
		pr_warn("recomp_default: no compressor %s\\n", name);
		return;
	}
	alg = kstrdup(name, GFP_KERNEL);
	if (alg)
		comp_algorithm_set(zram, ZRAM_SECONDARY_COMP, alg);
#endif
}
""", 1),
    (ZR, "\tzram_comp_params_reset(zram);\n\tcomp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);\n",
     "\tzram_comp_params_reset(zram);\n\tcomp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);\n"
     "\tzram_set_recomp_default(zram);\t/* k618 port zram_tiered */\n", 1),
    (ZR, "\treset_bdev(zram);\n\n\tcomp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);\n}\n",
     "\treset_bdev(zram);\n\n\tcomp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);\n"
     "\tzram_set_recomp_default(zram);\t/* k618 port zram_tiered */\n}\n", 1),
    (ZR, "\tdisksize = memparse(buf, NULL);\n\tif (!disksize)\n\t\treturn -EINVAL;\n",
     "\tdisksize = memparse(buf, NULL);\n\tif (!disksize)\n\t\treturn -EINVAL;\n"
     "\t/* k618 port zram_tiered: disksize_mb decides the size */\n"
     "\tif (READ_ONCE(disksize_mb))\n\t\tdisksize = (u64)READ_ONCE(disksize_mb) << 20;\n", 1),
    (ZR, "\tif (page)\n\t\t__free_page(page);\n\trelease_pp_ctl(zram, ctl);\n\tatomic_set(&zram->pp_in_progress, 0);\n"
     "\treturn ret;\n}\n#endif\n\nstatic void zram_bio_discard(",
     """	if (page)
		__free_page(page);
	release_pp_ctl(zram, ctl);
	atomic_set(&zram->pp_in_progress, 0);
	return ret;
}

/*
 * k618 port zram_tiered: one round of the cold tier on one device. What the round before marked
 * idle and nothing touched since (a slot that is read or written loses the mark) is compressed
 * again with the second algorithm; then every slot is marked again. A slot the second algorithm
 * cannot make smaller is remembered (ZRAM_INCOMPRESSIBLE) and not tried again. False when it
 * stopped early: kthread_stop(), or the system freezing for a suspend.
 */
static bool zram_recompress_round(struct zram *zram)
{
	u64 num_recomp_pages = ULLONG_MAX;
	struct zram_pp_ctl *ctl = NULL;
	struct zram_pp_slot *pps;
	struct page *page = NULL;
	u32 prio_max = ZRAM_SECONDARY_COMP + 1;
	bool done = true;

	guard(rwsem_read)(&zram->init_lock);
	if (!init_done(zram) || zram->num_active_comps < prio_max)
		return true;
	/* a recompress or writeback of user space's own is running: next time */
	if (atomic_xchg(&zram->pp_in_progress, 1))
		return true;

	page = alloc_page(GFP_KERNEL);
	if (page)
		ctl = init_pp_ctl();
	if (ctl) {
		scan_slots_for_recompress(zram, RECOMPRESS_IDLE, prio_max, ctl);
		while ((pps = select_pp_slot(ctl))) {
			int err = 0;

			if (kthread_should_stop() || freezing(current)) {
				release_pp_slot(zram, pps);
				done = false;
				break;
			}
			slot_lock(zram, pps->index);
			/* as recompress_store(): a prefetched slot is skipped */
			if (test_slot_flag(zram, pps->index, ZRAM_PP_SLOT) &&
			    !zram_prefetch_cache_exist(zram, pps->index))
				err = recompress_slot(zram, pps->index, page,
						      &num_recomp_pages, 0,
						      ZRAM_SECONDARY_COMP, prio_max);
			slot_unlock(zram, pps->index);
			release_pp_slot(zram, pps);
			if (err)
				break;
			cond_resched();
		}
	}
	if (page)
		__free_page(page);
	release_pp_ctl(zram, ctl);
	if (done)
		mark_idle(zram, 0);
	atomic_set(&zram->pp_in_progress, 0);
	return done;
}

static struct task_struct *zram_recomp_task;

/* Run background work on the CPUs of the lowest capacity. */
static void zram_recomp_little(void)
{
	unsigned long min = ULONG_MAX;
	cpumask_var_t mask;
	int cpu;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		return;
	for_each_online_cpu(cpu)
		min = min(min, arch_scale_cpu_capacity(cpu));
	for_each_online_cpu(cpu)
		if (arch_scale_cpu_capacity(cpu) == min)
			cpumask_set_cpu(cpu, mask);
	if (!cpumask_empty(mask))
		set_cpus_allowed_ptr(current, mask);
	free_cpumask_var(mask);
}

static int zram_recomp_thread(void *unused)
{
	set_freezable();
	set_user_nice(current, MAX_NICE);
	while (!kthread_should_stop()) {
		unsigned int secs = READ_ONCE(recomp_idle_secs);
		struct zram *zram;
		ktime_t start;
		int id;

		/* jiffies stand still while the system is suspended: this is time awake */
		wait_event_freezable_timeout(zram_recomp_wait, kthread_should_stop() ||
					     atomic_read(&recomp_kick) || atomic_read(&recomp_now_req),
					     (secs ? secs : 600) * (unsigned long)HZ);
		if (kthread_should_stop())
			break;
		/* a new interval: wait again, with it (unless a round was asked for as well) */
		if (atomic_xchg(&recomp_kick, 0) && !atomic_read(&recomp_now_req))
			continue;
		if (!atomic_xchg(&recomp_now_req, 0) && !READ_ONCE(recomp_idle_secs))
			continue;
		zram_recomp_little();	/* the capacities are known only once cpufreq is up */
		start = ktime_get();
		mutex_lock(&zram_index_mutex);
		idr_for_each_entry(&zram_index_idr, zram, id)
			if (!zram_recompress_round(zram))
				break;
		mutex_unlock(&zram_index_mutex);
		recomp_rounds++;
		recomp_ms += ktime_ms_delta(ktime_get(), start);
	}
	return 0;
}

static void zram_recomp_start(void)
{
	zram_recomp_task = kthread_run(zram_recomp_thread, NULL, "zram_recomp");
	if (IS_ERR(zram_recomp_task))
		zram_recomp_task = NULL;
}

static void zram_recomp_stop(void)
{
	if (zram_recomp_task)
		kthread_stop(zram_recomp_task);
}
#else
static inline void zram_recomp_start(void) {}
static inline void zram_recomp_stop(void) {}
#endif

static void zram_bio_discard(""", 1),
    (ZR, "\t\tnum_devices--;\n\t}\n\n\treturn 0;\n\nout_error:\n",
     "\t\tnum_devices--;\n\t}\n\n\tzram_recomp_start();\t/* k618 port zram_tiered */\n\treturn 0;\n\nout_error:\n", 1),
    (ZR, "static void __exit zram_exit(void)\n{\n\tdestroy_devices();\n}\n",
     "static void __exit zram_exit(void)\n{\n\tzram_recomp_stop();\t/* k618 port zram_tiered */\n\tdestroy_devices();\n}\n", 1),
])


# ---- initramfs_last: the order in which the kernel unpacks its ramdisks. `fastboot boot` takes
# init_boot's ramdisk instead of the image's, so the kernel carries our module layer itself
# (k618_irbuild.sh). The recovery partition of this phone holds an image with no kernel
# (kernel_size 0): recovery is the boot partition's kernel with the recovery ramdisk (the ROM's or
# TWRP's), and another model brings its own fstab and init in its own ramdisks. So the bootloader's
# ramdisk is unpacked first and the built-in one -- our 6.18 modules and their lists, nothing else
# -- over it: ours win where both have a file (modules.load, modules.load.recovery, same-named .ko)
# and the rest is the device's own.
change("initramfs_last", [
    ("init/initramfs.c", """static void __init do_populate_rootfs(void *unused, async_cookie_t cookie)
{
	/* Load the built in initramfs */
	char *err = unpack_to_rootfs(__initramfs_start, __initramfs_size);
	if (err)
		panic_show_mem("%s", err); /* Failed to decompress INTERNAL initramfs */

	if (!initrd_start || IS_ENABLED(CONFIG_INITRAMFS_FORCE))
		goto done;
""",
     """static void __init do_populate_rootfs(void *unused, async_cookie_t cookie)
{
	char *err;

	/*
	 * k618 initramfs_last: the built-in initramfs goes on after the ramdisk the bootloader passes
	 * (the device's own vendor_boot and init_boot: its fstab, its init, its recovery), so that
	 * what it carries -- our modules and their lists -- wins where both have a file. With no
	 * ramdisk passed, or with INITRAMFS_FORCE, it is the only one.
	 */
	if (!initrd_start || IS_ENABLED(CONFIG_INITRAMFS_FORCE)) {
		err = unpack_to_rootfs(__initramfs_start, __initramfs_size);
		if (err)
			panic_show_mem("%s", err); /* Failed to decompress INTERNAL initramfs */
		goto done;
	}
""", 1),
    ("init/initramfs.c", """		printk(KERN_EMERG "Initramfs unpacking failed: %s\\n", err);
#endif
	}

done:
""",
     """		printk(KERN_EMERG "Initramfs unpacking failed: %s\\n", err);
#endif
	}

	/* k618 initramfs_last: and the built-in one over it */
	err = unpack_to_rootfs(__initramfs_start, __initramfs_size);
	if (err)
		panic_show_mem("%s", err); /* Failed to decompress INTERNAL initramfs */

done:
""", 1),
])


def done(t, old, new):
    """an insertion keeps its old text (new contains it); a removal's new text is part of its old one"""
    if old in new:
        return new in t
    return new in t and old not in t


def apply_change(root, name, edits):
    texts = {}
    for f, _, _, _ in edits:
        if f not in texts:
            with open(os.path.join(root, f), encoding="utf-8", newline="") as fh:
                texts[f] = fh.read()
    have = [done(texts[f], old, new) for f, old, new, n in edits]
    if all(have):
        return "already applied"
    if any(have):
        raise SystemExit(f"k618_ports: {name}: only partly applied ({sum(have)} of {len(edits)} edits)")
    for f, old, new, n in edits:
        c = texts[f].count(old)
        if c != n:
            raise SystemExit(f"k618_ports: {name}: {f}: expected {n}x {old.splitlines()[0]!r}..., found {c}")
        texts[f] = texts[f].replace(old, new)
    for f, t in texts.items():
        with open(os.path.join(root, f), "w", encoding="utf-8", newline="") as fh:
            fh.write(t)
    return "applied"


def run515(root, script, *args):
    """a 5.15 script that applies to 6.18 as it is: run as kernel.yml runs it"""
    r = subprocess.run([sys.executable, "-I", os.path.join(P515, script), "."] + list(args),
                       cwd=root, capture_output=True, text=True)
    out = (r.stdout + r.stderr).strip().splitlines()
    if r.returncode != 0:
        raise SystemExit(f"k618_ports: {script} failed: {out[-1] if out else r.returncode}")
    return out[-1] if out else "ok"


FUNC = re.compile(r"^(?:int|void)\s+(LZ4_\w+)\(([^)]*)\)\s*\{", re.M)


def params(plist):
    """'LZ4_stream_t *LZ4_stream, const char *source' -> [('LZ4_stream_t *', 'LZ4_stream'), ...]"""
    out = []
    for p in plist.split(","):
        p = " ".join(p.split())
        m = re.match(r"(.*?[\s*])(\w+)$", p)
        if not m:
            raise SystemExit(f"k618_ports: lz4: cannot read the parameter {p!r}")
        out.append((m.group(1).replace(" *", "*").strip(), m.group(2)))
    return out


def lz4_param_names(root, k):
    """gendwarfksyms puts the parameters' names into a CRC: an export whose names differ from
    6.18's own lib/lz4 gets another CRC, and the 18 Pro Max's zram.ko (LZ4_loadDict,
    LZ4_compress_fast_continue, LZ4_decompress_safe_continue among its six) then would not load.
    So every exported wrapper of lz4_kernel.c takes the names of 6.18's definition, by position."""
    orig = {}
    for f in ("lib/lz4/lz4_compress.c", "lib/lz4/lz4_decompress.c", "lib/lz4/lz4hc_compress.c"):
        src = subprocess.run(["git", "-C", root, "show", f"HEAD:{f}"], capture_output=True, text=True).stdout
        for m in FUNC.finditer(src):
            orig[m.group(1)] = params(m.group(2))
    with open(k, encoding="utf-8", newline="") as fh:
        t = fh.read()
    exported = set(re.findall(r"^EXPORT_SYMBOL\((LZ4_\w+)\);", t, re.M))
    renamed = 0
    for m in list(FUNC.finditer(t)):
        name = m.group(1)
        if name not in exported:
            continue
        if name not in orig:
            raise SystemExit(f"k618_ports: lz4: {name} is not defined in 6.18's lib/lz4")
        ours, theirs = params(m.group(2)), orig[name]
        if [p[0] for p in ours] != [p[0] for p in theirs]:
            raise SystemExit(f"k618_ports: lz4: {name}: parameter types {ours} vs 6.18's {theirs}")
        if [p[1] for p in ours] == [p[1] for p in theirs]:
            continue
        # the function's whole text: from its signature to the first line that is a lone "}"
        start = t.index(m.group(0))
        end = t.index("\n}\n", start) + 3
        body = t[start:end]
        for i, (_, a) in enumerate(ours):	# (type, name): the names
            body = re.sub(r"\b%s\b" % re.escape(a), f"__k618_p{i}", body)
        for i, (_, b) in enumerate(theirs):
            body = body.replace(f"__k618_p{i}", b)
        t = t[:start] + body + t[end:]
        renamed += 1
    for m in FUNC.finditer(t):
        if m.group(1) in exported and [p[1] for p in params(m.group(2))] != [p[1] for p in orig[m.group(1)]]:
            raise SystemExit(f"k618_ports: lz4: {m.group(1)}: names still differ after renaming")
    if renamed:
        with open(k, "w", encoding="utf-8", newline="") as fh:
            fh.write(t)
    return renamed


def lz4(root):
    head = subprocess.run(["git", "-C", LZ4_DEV, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if head != LZ4_PIN:
        raise SystemExit(f"k618_ports: {LZ4_DEV} is at {head or 'nothing'}, not kernel.yml's lz4 {LZ4_PIN[:7]}")
    # 6.18 exports LZ4_resetStreamHC as well (5.15 did not): zram's lz4hc backend calls it
    k = os.path.join(root, "lib/lz4/lz4_kernel.c")
    old = "void LZ4_resetStreamHC(LZ4_streamHC_t *streamHCPtr, int compressionLevel)\n{\n\tlz4k_resetStreamHC(streamHCPtr, compressionLevel);\n}\n"
    new = old + "EXPORT_SYMBOL(LZ4_resetStreamHC);\t/* k618: 6.18 exports it (zram's lz4hc backend) */\n"
    # since 6.10 (b1992c3772e6) $(src) is the source directory itself: 5.15's $(srctree)/$(src) doubles it
    mk = os.path.join(root, "lib/lz4/Makefile")
    mk_old, mk_new = "-I$(srctree)/$(src)/shim", "-I$(src)/shim"
    final = False
    if os.path.isfile(k) and os.path.isdir(os.path.join(root, "lib/lz4/upstream")):
        with open(k, encoding="utf-8", newline="") as fh, open(mk, encoding="utf-8", newline="") as fm:
            final = new[len(old):] in fh.read() and mk_new in fm.read()	# the export line: the names change below
    if final:	# lz4_upgrade.py would copy its own files back
        said = "already applied (LZ4_resetStreamHC exported, 6.18's include path)"
    else:
        said = run515(root, "lz4_upgrade.py", LZ4_DEV)
        for path, o, n in ((k, old, new), (mk, mk_old, mk_new)):
            with open(path, encoding="utf-8", newline="") as fh:
                t = fh.read()
            if n not in t:
                if t.count(o) != 1:
                    raise SystemExit(f"k618_ports: lz4: {path}: {o.splitlines()[0]!r} not found once")
                with open(path, "w", encoding="utf-8", newline="") as fh:
                    fh.write(t.replace(o, n))
    n = lz4_param_names(root, k)
    return said + (f"; {n} wrappers given 6.18's parameter names" if n else "")


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    root = argv[1].rstrip("/")
    print(f"k618_ports: lz4: {lz4(root)}")
    print(f"k618_ports: wake_holds: {run515(root, 'wake_holds.py', '1')}")
    print(f"k618_ports: wakelock_blocker: {run515(root, 'wakelock_blocker.py', '--defaults')[:90]}")
    for name, edits in CHANGES:
        print(f"k618_ports: {name}: {apply_change(root, name, edits)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
