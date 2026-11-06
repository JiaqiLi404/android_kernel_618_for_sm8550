// SPDX-License-Identifier: GPL-2.0
/*
 * k618_place: four changes around the kernel's own energy-aware placement (EAS), and the CPU energy
 * model EAS prices with taken from measured power instead of the device tree.
 *
 * On 6.18 no vendor placement module runs on the 13 Pro: tasks are placed by the scheduler's EAS, which
 * picks the CPU whose performance domain adds the least energy according to the energy model. This
 * module keeps that and adds, each behind a module parameter (0644) its hooks read on every call:
 *
 *   rt=1     RT tasks go to the little cores first, and never onto a CPU running a draw thread
 *   pull=1   a draw thread that has waited runnable for pull_wait_us is pulled to an idle CPU of its
 *            own cluster (or of a smaller one, with pull_down=1)
 *   slice=1  draw threads get an EEVDF slice of slice_us instead of the base slice
 *   sync=1   a sync wake-up sent from a middle or prime CPU loses its hint when the task woken fits a
 *            little core
 *   em=1     the measured energy model (k618_place_em.py); em=0 puts the device tree's back
 *
 * A "draw thread" is a thread of a process in the cpuset top-app (or in a cpuset made inside it) that is
 * the process's main thread or HWUI's RenderThread: the two threads that produce an app's frames.
 *
 * The scheduler's restricted hooks (android_rvh_*) cannot be unregistered, and each takes at most two
 * probes. So the module never unloads (no module_exit), a parameter set to 0 turns its part off, and
 * the hooks are chosen to stay clear of the ones the loaded vendor modules hold (see "Hooks").
 *
 * (1) rt -- android_rvh_select_task_rq_rt and android_rvh_find_lowest_rq.
 *     The kernel's RT wake-up keeps a task on its previous CPU unless that CPU runs an RT task of
 *     equal or higher priority (rt.c select_task_rq_rt: with RT tasks' uclamp floor 0 every CPU
 *     "fits"), so an RT task whose previous CPU is a middle core running an app's main thread preempts
 *     it there; find_lowest_rq() is only reached when that CPU is taken. Here every RT wake-up and
 *     fork, and every push through find_lowest_rq(), looks at the clusters from the smallest up: in
 *     each, the task's previous CPU if idle, else any idle CPU, else the least loaded CPU running
 *     nothing of the task's priority or above, nothing of deadline class, no long softirq, and no draw
 *     thread; the first cluster with a candidate wins. A CPU running a draw thread is never chosen;
 *     with no candidate anywhere the kernel's own choice stands. An RT waker that is about to sleep
 *     keeps the wake-up on its CPU as the kernel does (should_honor_rt_sync()), and a draw thread that
 *     is RT itself is left to the kernel. Cost: only RT wake-ups and pushes, ~20-30 instructions per
 *     CPU looked at, early out at an idle little CPU.
 *
 * (2) pull -- android_vh_scheduler_tick and android_rvh_sched_newidle_balance.
 *     While the system is not overutilized EAS does no load balancing at all (find_busiest_group()
 *     gives up), so a draw thread queued behind another task waits for that CPU even with its
 *     neighbours idle. Each CPU's tick, when it has a waiting fair task, looks through its run queue
 *     (under its lock, at most 8 entries) for a draw thread runnable for pull_wait_us
 *     (sched_info.last_queued) and sends an idle CPU of the same cluster (with pull_down, of a
 *     smaller one the thread fits) an IPI in which that CPU takes the thread -- one queue lock at a
 *     time, as the kernel's own balancing moves tasks. Any CPU going idle runs the newidle hook too,
 *     which takes such a thread from a CPU of its cluster (with pull_down, of a bigger one) under a
 *     trylock of that queue while keeping its own: no lock is dropped, so nothing the kernel's own
 *     newidle balance relies on changes, and a contended queue is simply tried at the next chance.
 *     Cost: a busy tick ~6 instructions when nothing waits; a newidle ~5 per CPU of the cluster when
 *     no CPU has a waiting task.
 *
 * (3) slice -- android_vh_cgroup_attach, android_rvh_set_task_comm, android_rvh_sched_fork and
 *     android_rvh_setscheduler. Nothing on the enqueue path: the slice is set once, when a thread
 *     becomes a draw thread (its process moves into top-app, a thread is named RenderThread, a
 *     sched_setattr() without a slice of its own reset it), and taken off when it stops being one or
 *     slice=0. EEVDF does the rest: a shorter slice gives an earlier virtual deadline, and a waking
 *     task with a shorter slice than the running one cancels that one's slice protection
 *     (PREEMPT_SHORT), so an eligible draw thread that wakes takes the CPU at once instead of after the
 *     running task's protected slice. The base slice is 2.8 ms (0.7 ms x (1 + log2 8)); slice_us 1000
 *     is shorter. A slice set here carries a mark in its low 10 bits (see k618_slice_ns()), so a
 *     slice someone else asked for is never touched, and a child forked from a draw thread does not
 *     keep it.
 *
 * (4) sync -- android_rvh_task_fits_cpu. With a sync wake-up EAS puts the woken task on the waker's
 *     CPU when that CPU runs only the waker and the task fits it (find_energy_efficient_cpu()'s first
 *     test, the only caller of task_fits_cpu() with a waking task). When that CPU is a middle or prime
 *     core and the task would fit a little one, "does not fit" is answered there, and EAS goes on to
 *     price the CPUs as for any wake-up. android_vh_set_wake_flags cannot do this: it runs before the
 *     task woken is known; android_rvh_try_to_wake_up_begin could, but its two slots are taken.
 *     Cost: task_fits_cpu() also runs on every pick of a fair task and every tick (misfit check):
 *     ~5 instructions there besides the hook call itself.
 *
 * (5) em -- em_dev_update_perf_domain() with a table of our own: for every OPP of every perf domain,
 *     power = K * E(f) * performance, E = the energy one unit of a blend of four measured workloads
 *     takes there (little at 1228.8 MHz = 1), so that the kernel's own cost (10 * power / performance)
 *     is proportional to the measured energy per unit of work on every cluster. k618_place_em.py has
 *     the measurements, the fits, the table and the checks of this arithmetic. The device tree's
 *     model is computed again from the OPPs' voltages whenever it is put back (dev_pm_opp_calc_power,
 *     as the kernel registered it). The kernel derives "inefficient" OPPs from a new table and from
 *     then on skips them in EAS and in cpufreq's frequency choice; with em_ineff=0 (default) the
 *     registered marks are kept and cpufreq's table is put back as it was, so the frequency governor
 *     chooses among the same OPPs as before -- the measured knee (little 441-902 MHz within 3 %) is
 *     too flat for that call. No cost after the swap: EAS reads whichever table is in force.
 *
 * Hooks, and the vendor modules of the boot image that hold restricted ones: cameramsger has
 * android_rvh_sched_newidle_balance (ours is the second and last slot; its probe only sets "done" on
 * CPUs the camera reserves, and ours stops on "done"), select_task_rq_fair, can_migrate_task,
 * check_preempt_wakeup_fair and is_cpu_allowed (whose word a CPU chosen here also takes, see
 * k618_cpu_allowed()); mi_sched_ext and plat_common hold both slots of
 * android_rvh_try_to_wake_up_begin. Nothing else uses the restricted hooks taken here. A vendor WALT
 * (sched-walt.ko) would need android_rvh_sched_newidle_balance too: this module and WALT are
 * alternatives, not to be loaded together.
 *
 * Read-only: /sys/module/k618_place/parameters/state (what each part did, for A/Bs) and em_table
 * (every OPP: performance, the device tree's power, the power in force, cost, inefficient).
 */
#define pr_fmt(fmt) "k618_place: " fmt

#include <linux/cgroup.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/energy_model.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pm_opp.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <trace/hooks/cgroup.h>
#include <trace/hooks/sched.h>

/* struct rq, its locks and helpers: the module is built against this kernel's own tree */
#include <../kernel/sched/sched.h>

/* ---------------------------------------------------------------- parameters */

static bool rt = true;
static bool pull = true;
static unsigned int pull_wait_us = 2000;
static bool pull_down;
static bool slice = true;
static unsigned int slice_us = 1000;
static bool sync = true;
static unsigned int em = 1;
static bool em_ineff;
static char test_comm[TASK_COMM_LEN];

static DEFINE_STATIC_KEY_FALSE(k618_rt_key);
static DEFINE_STATIC_KEY_FALSE(k618_pull_key);
static DEFINE_STATIC_KEY_FALSE(k618_sync_key);
static DEFINE_STATIC_KEY_FALSE(k618_test_key);

/* set once the hooks are in: before that a parameter write (module options at load) only stores */
static bool k618_live;
/* which hooks went in (a restricted hook with both slots taken refuses a third probe) */
static bool k618_have_rt, k618_have_pull, k618_have_sync, k618_have_slice;

/* ---------------------------------------------------------------- counters */

struct k618_stat {
	unsigned long rt_picks;		/* RT placements made here */
	unsigned long rt_little;	/* ... of them on the smallest cluster */
	unsigned long rt_draw_skips;	/* CPUs passed over because a draw thread ran there */
	unsigned long rt_kernel;	/* no candidate: the kernel's own choice stood */
	unsigned long pull_kicks;	/* IPIs sent to an idle CPU for a waiting draw thread */
	unsigned long pulls;		/* draw threads moved to an idle CPU */
	unsigned long pulls_newidle;	/* ... of them by a CPU going idle (the rest: in the IPI) */
	unsigned long pull_down_moves;	/* ... of them to a smaller cluster */
	unsigned long sync_drops;	/* sync wake-ups not taken to a middle / prime waker */
};
static DEFINE_PER_CPU(struct k618_stat, k618_stat);
/* every hook that counts runs with interrupts off: no atomics needed, a lost count does not matter */
#define K618_INC(f)	raw_cpu_inc(k618_stat.f)

static atomic_long_t k618_slice_sets, k618_slice_clears, k618_slice_forks, k618_em_swaps;

/* ---------------------------------------------------------------- topology */

#define K618_NR_CL	4

static int k618_ncl;
static struct cpumask k618_cl[K618_NR_CL];	/* CPUs by capacity, smallest first */
static unsigned long k618_cl_cap[K618_NR_CL];
static u8 k618_cl_of[NR_CPUS] __read_mostly;
/* where a CPU going idle may pull from: the rest of its cluster, and the bigger clusters */
static struct cpumask k618_src_same[NR_CPUS], k618_src_up[NR_CPUS];

static void __init k618_topology(void)
{
	int cpu, c, i;

	for_each_possible_cpu(cpu) {
		unsigned long cap = arch_scale_cpu_capacity(cpu);

		for (c = 0; c < k618_ncl && k618_cl_cap[c] != cap; c++)
			;
		if (c == k618_ncl) {
			if (k618_ncl == K618_NR_CL) {
				c = K618_NR_CL - 1;	/* more classes than this SoC has: lump them */
			} else {
				k618_cl_cap[c] = cap;
				k618_ncl++;
			}
		}
		cpumask_set_cpu(cpu, &k618_cl[c]);
	}
	/* ascending capacity (insertion sort of at most four) */
	for (i = 1; i < k618_ncl; i++)
		for (c = i; c > 0 && k618_cl_cap[c - 1] > k618_cl_cap[c]; c--) {
			struct cpumask m;

			swap(k618_cl_cap[c - 1], k618_cl_cap[c]);
			cpumask_copy(&m, &k618_cl[c - 1]);
			cpumask_copy(&k618_cl[c - 1], &k618_cl[c]);
			cpumask_copy(&k618_cl[c], &m);
		}
	for (c = 0; c < k618_ncl; c++)
		for_each_cpu(cpu, &k618_cl[c])
			k618_cl_of[cpu] = c;
	for_each_possible_cpu(cpu) {
		cpumask_copy(&k618_src_same[cpu], &k618_cl[k618_cl_of[cpu]]);
		cpumask_clear_cpu(cpu, &k618_src_same[cpu]);
		for (c = k618_cl_of[cpu] + 1; c < k618_ncl; c++)
			cpumask_or(&k618_src_up[cpu], &k618_src_up[cpu], &k618_cl[c]);
	}
}

/* ---------------------------------------------------------------- draw threads */

/* the cpuset top-app, from the first move into it (Android creates it at boot and keeps it):
 * compared, never followed */
static struct cgroup *k618_top;

/* "RenderThread" as it lies in a task's name: a word and a half */
#define K618_RENDER_LO	0x68547265646e6552ULL	/* "RenderTh" */
#define K618_RENDER_HI	0x64616572U		/* "read" */

static __always_inline bool k618_render(const struct task_struct *p)
{
	return get_unaligned_le64(p->comm) == K618_RENDER_LO &&
	       get_unaligned_le32(p->comm + 8) == K618_RENDER_HI;
}

/* the QEMU test: a task of this name counts as a draw thread wherever it is */
static noinline bool k618_test_match(const struct task_struct *p)
{
	return test_comm[0] && !strncmp(p->comm, test_comm, TASK_COMM_LEN);
}

/*
 * Is @p a draw thread: the main thread or RenderThread of a process in @top or in a cpuset made
 * inside it? With preemption off or under RCU (the task's css_set is freed by RCU).
 */
static __always_inline bool k618_draws(struct task_struct *p, struct cgroup *top)
{
	struct cgroup *cg;

	if (static_branch_unlikely(&k618_test_key) && k618_test_match(p))
		return true;
	if (!top || (!thread_group_leader(p) && !k618_render(p)))
		return false;
	cg = task_css_check(p, cpuset_cgrp_id, true)->cgroup;
	return cg == top || cgroup_parent(cg) == top;
}

static __always_inline bool k618_looks(struct cgroup *top)
{
	return top || static_branch_unlikely(&k618_test_key);
}

/* the cgroup named top-app right under its hierarchy's root */
static bool k618_is_top_app(struct cgroup *cg)
{
	struct cgroup *parent = cgroup_parent(cg);
	const char *name;
	bool ret;

	if (!parent || cgroup_parent(parent))
		return false;
	rcu_read_lock();
	name = rcu_dereference(cg->kn->name);
	ret = name && !strcmp(name, "top-app");
	rcu_read_unlock();
	return ret;
}

static void k618_learn_top(struct cgroup *cg)
{
	if (cg != READ_ONCE(k618_top) && k618_is_top_app(cg)) {
		WRITE_ONCE(k618_top, cg);
		pr_info("cpuset top-app found\n");
	}
}

/* a task's utilisation as EAS takes it (task_util_est()) */
static __always_inline unsigned long k618_util(struct task_struct *p)
{
	return max_t(unsigned long, READ_ONCE(p->se.avg.util_avg),
		     READ_ONCE(p->se.avg.util_est) & ~UTIL_AVG_UNCHANGED);
}

/* fits_capacity()'s 80 % with the task's uclamp floor */
static __always_inline bool k618_fits(unsigned long util, unsigned long umin, unsigned long cap)
{
	return util * 1280 < cap * 1024 && umin <= cap;
}

/*
 * What the kernel's is_cpu_allowed() adds to the affinity mask for a user task: an active CPU, and
 * the vendor hook's word (cameramsger keeps tasks off the CPUs the camera reserves). Asked once, for
 * the CPU chosen, not for every CPU looked at.
 */
static bool k618_cpu_allowed(struct task_struct *p, int cpu)
{
	bool allowed = true;

	trace_android_rvh_is_cpu_allowed(p, cpu, &allowed);
	return allowed && cpu_active(cpu);
}

/* ---------------------------------------------------------------- (1) RT placement */

static __always_inline int k618_rt_got(struct task_struct *p, int c, int cpu)
{
	if (!k618_cpu_allowed(p, cpu)) {
		K618_INC(rt_kernel);
		return -1;
	}
	K618_INC(rt_picks);
	if (!c)
		K618_INC(rt_little);
	return cpu;
}

/*
 * The CPU for RT task @p, smallest cluster first: @prev if idle, else an idle CPU, else the least
 * loaded CPU that runs nothing of @p's priority or above and no draw thread. @within: only CPUs of
 * this mask (find_lowest_rq()'s lowest-priority set). -1: no candidate, leave it to the kernel.
 */
static int k618_rt_pick(struct task_struct *p, int prev, const struct cpumask *within)
{
	struct cgroup *top = READ_ONCE(k618_top);
	bool look = k618_looks(top);
	unsigned long umin = uclamp_is_used() ? uclamp_eff_value(p, UCLAMP_MIN) : 0;
	int c, cpu;

	for (c = 0; c < k618_ncl; c++) {
		unsigned long best_util = ULONG_MAX;
		int idle = -1, busy = -1;

		/* a cluster too small for the task's own floor (the biggest is always tried) */
		if (umin > k618_cl_cap[c] && c < k618_ncl - 1)
			continue;
		for_each_cpu_and(cpu, &k618_cl[c], p->cpus_ptr) {
			struct task_struct *curr;
			unsigned long util;
			struct rq *rq;

			if (within && !cpumask_test_cpu(cpu, within))
				continue;
			if (!cpu_active(cpu))
				continue;
			if (available_idle_cpu(cpu)) {
				if (cpu == prev)
					return k618_rt_got(p, c, cpu);
				if (idle < 0)
					idle = cpu;
				continue;
			}
			if (idle >= 0)
				continue;
			rq = cpu_rq(cpu);
			if (READ_ONCE(rq->dl.dl_nr_running) ||
			    READ_ONCE(rq->rt.highest_prio.curr) <= p->prio)
				continue;
			curr = rcu_dereference_sched(rq->curr);
			if (look && k618_draws(curr, top)) {
				K618_INC(rt_draw_skips);
				continue;
			}
			if (cpu_busy_with_softirqs(cpu))
				continue;
			util = READ_ONCE(rq->cfs.avg.util_avg) + READ_ONCE(rq->avg_rt.util_avg);
			if (util < best_util) {
				best_util = util;
				busy = cpu;
			}
		}
		if (idle >= 0)
			return k618_rt_got(p, c, idle);
		if (busy >= 0)
			return k618_rt_got(p, c, busy);
	}
	K618_INC(rt_kernel);
	return -1;
}

/* the kernel's should_honor_rt_sync(): an RT waker about to sleep keeps the wake-up on its CPU */
static bool k618_rt_sync_here(struct task_struct *p)
{
	struct rq *rq = this_rq();
	struct task_struct *curr = rcu_dereference_sched(rq->curr);

	return task_has_rt_policy(curr) && p->prio <= rq->rt.highest_prio.next &&
	       rq->rt.rt_nr_running <= 2 && cpumask_test_cpu(cpu_of(rq), p->cpus_ptr);
}

/*
 * A draw thread made RT (the ROM's touch boost can make an app's main thread and RenderThread FIFO for
 * a while) is not sent to the little cores: the kernel's own placement keeps it where it ran.
 */
static __always_inline bool k618_rt_own(struct task_struct *p)
{
	struct cgroup *top = READ_ONCE(k618_top);

	return k618_looks(top) && k618_draws(p, top);
}

static void k618_select_rt(void *unused, struct task_struct *p, int prev_cpu, int sd_flag,
			   int wake_flags, int *new_cpu)
{
	int cpu;

	if (!static_branch_unlikely(&k618_rt_key))
		return;
	if (!(wake_flags & (WF_TTWU | WF_FORK)) || p->nr_cpus_allowed == 1 || *new_cpu >= 0)
		return;
	if ((wake_flags & WF_SYNC) && k618_rt_sync_here(p))
		return;
	if (k618_rt_own(p))
		return;
	cpu = k618_rt_pick(p, prev_cpu, NULL);
	if (cpu >= 0)
		*new_cpu = cpu;
}

static void k618_lowest_rq(void *unused, struct task_struct *sched_ctx, struct task_struct *exec_ctx,
			   struct cpumask *lowest_mask, int ret, int *lowest_cpu)
{
	struct task_struct *p = exec_ctx ? exec_ctx : sched_ctx;
	int cpu;

	if (!static_branch_unlikely(&k618_rt_key) || !ret || *lowest_cpu >= 0 || k618_rt_own(p))
		return;
	cpu = k618_rt_pick(p, task_cpu(p), lowest_mask);
	if (cpu >= 0)
		*lowest_cpu = cpu;
}

/* ---------------------------------------------------------------- (2) pull */

#define K618_SCAN_MAX	8	/* run-queue entries looked at per queue */
#define K618_SRC_MAX	2	/* queues locked per newidle */

/* may @p (queued on @src, locked) move to @dst; @cap: @dst's capacity when it is smaller, else 0 */
static bool k618_can_move(struct task_struct *p, int dst, unsigned long cap)
{
	/* (a per-CPU kthread fails the first test: it is allowed one CPU) */
	if (!cpumask_test_cpu(dst, p->cpus_ptr) || is_migration_disabled(p) || task_is_blocked(p))
		return false;
#ifdef CONFIG_CFS_BANDWIDTH
	if (p->throttled || task_group(p)->cfs_rq[dst]->throttle_count)
		return false;
#endif
	if (cap && !k618_fits(k618_util(p), uclamp_is_used() ? uclamp_eff_value(p, UCLAMP_MIN) : 0, cap))
		return false;
	return k618_cpu_allowed(p, dst);
}

/*
 * The draw thread on @rq (locked) that has waited runnable longest, if that is pull_wait_us or more;
 * @dst >= 0: only one that may move there (@down: @dst is in a smaller cluster).
 */
static struct task_struct *k618_waiting_draw(struct rq *rq, struct cgroup *top, u64 now, int dst,
					     bool down)
{
	u64 limit = (u64)READ_ONCE(pull_wait_us) * NSEC_PER_USEC, best_wait = 0;
	unsigned long cap = down ? k618_cl_cap[k618_cl_of[dst]] : 0;
	struct task_struct *p, *best = NULL;
	int n = 0;

	list_for_each_entry(p, &rq->cfs_tasks, se.group_node) {
		u64 since = READ_ONCE(p->sched_info.last_queued), wait;

		if (++n > K618_SCAN_MAX)
			break;
		if (!since || task_on_cpu(rq, p) || task_current_donor(rq, p) || p->se.sched_delayed)
			continue;
		wait = now > since ? now - since : 0;
		if (wait < limit || wait <= best_wait)
			continue;
		if (!k618_draws(p, top))
			continue;
		if (dst >= 0 && !k618_can_move(p, dst, cap))
			continue;
		best = p;
		best_wait = wait;
	}
	return best;
}

/* an idle CPU for @p (queued on @src, locked): its own cluster, then smaller ones with pull_down */
static int k618_kick_target(struct task_struct *p, int src)
{
	int c = k618_cl_of[src], lo = READ_ONCE(pull_down) ? 0 : c, cpu;
	unsigned long util = 0, umin = 0;

	if (lo < c) {
		util = k618_util(p);
		umin = uclamp_is_used() ? uclamp_eff_value(p, UCLAMP_MIN) : 0;
	}
	for (; c >= lo; c--) {
		if (c != k618_cl_of[src] && !k618_fits(util, umin, k618_cl_cap[c]))
			break;
		for_each_cpu_and(cpu, &k618_cl[c], p->cpus_ptr)
			if (cpu != src && cpu_active(cpu) && available_idle_cpu(cpu))
				return cpu;
	}
	return -1;
}

/*
 * A tick found a draw thread waiting on CPU k618_pull_src[target] and the target idle: the target
 * itself, in this IPI, takes it. Its own queue is idle and unlocked, so the source's lock can be
 * waited for (a newidle has to make do with a trylock); the thread is detached under the source's
 * lock and attached under ours, one lock at a time, as the kernel's own load balancing does.
 */
static DEFINE_PER_CPU(call_single_data_t, k618_pull_csd);
static DEFINE_PER_CPU(int, k618_pull_src);

static void k618_pull_ipi(void *unused)
{
	int this_cpu = smp_processor_id(), src = READ_ONCE(*this_cpu_ptr(&k618_pull_src));
	struct rq *this_rq = cpu_rq(this_cpu), *srq = cpu_rq(src);
	struct task_struct *p;
	struct rq_flags rf;

	if (!static_branch_unlikely(&k618_pull_key) || !available_idle_cpu(this_cpu))
		return;
	raw_spin_rq_lock(srq);
	p = k618_waiting_draw(srq, READ_ONCE(k618_top), sched_clock(), this_cpu,
			      k618_cl_of[this_cpu] < k618_cl_of[src]);
	if (p) {
		update_rq_clock(srq);
		deactivate_task(srq, p, DEQUEUE_NOCLOCK);
		set_task_cpu(p, this_cpu);
	}
	raw_spin_rq_unlock(srq);
	if (!p)
		return;		/* it got its CPU meanwhile */
	rq_lock(this_rq, &rf);
	update_rq_clock(this_rq);
	activate_task(this_rq, p, ENQUEUE_NOCLOCK);
	wakeup_preempt(this_rq, p, 0);
	rq_unlock(this_rq, &rf);
	K618_INC(pulls);
	if (k618_cl_of[this_cpu] < k618_cl_of[src])
		K618_INC(pull_down_moves);
}

static noinline void k618_tick_kick(struct rq *rq, struct cgroup *top)
{
	struct task_struct *p;
	int dst = -1;

	raw_spin_rq_lock(rq);
	p = k618_waiting_draw(rq, top, sched_clock(), -1, false);
	if (p)
		dst = k618_kick_target(p, cpu_of(rq));
	raw_spin_rq_unlock(rq);
	if (dst < 0)
		return;
	/* one in flight per target: a busy one is skipped (-EBUSY) */
	WRITE_ONCE(*per_cpu_ptr(&k618_pull_src, dst), cpu_of(rq));
	if (!smp_call_function_single_async(dst, per_cpu_ptr(&k618_pull_csd, dst)))
		K618_INC(pull_kicks);
}

/* sched_tick(), after it has dropped the run queue's lock; interrupts off */
static void k618_tick(void *unused, struct rq *rq)
{
	struct cgroup *top;

	if (!static_branch_unlikely(&k618_pull_key))
		return;
	if (READ_ONCE(rq->nr_running) < 2 || !READ_ONCE(rq->cfs.h_nr_runnable))
		return;
	top = READ_ONCE(k618_top);
	if (k618_looks(top))
		k618_tick_kick(rq, top);
}

/* a CPU of @m whose queue holds a fair task besides the one running (lockless, a hint) */
static __always_inline bool k618_any_waiting(const struct cpumask *m)
{
	int cpu;

	for_each_cpu(cpu, m) {
		struct rq *rq = cpu_rq(cpu);

		if (READ_ONCE(rq->nr_running) >= 2 && READ_ONCE(rq->cfs.h_nr_runnable))
			return true;
	}
	return false;
}

/* take the longest-waiting movable draw thread of the first CPU of @m that has one */
static struct task_struct *k618_pull_from(const struct cpumask *m, struct cgroup *top, int this_cpu,
					  bool down, int *tried)
{
	struct task_struct *p = NULL;
	u64 now = 0;
	int cpu;

	for_each_cpu(cpu, m) {
		struct rq *src = cpu_rq(cpu);

		if (READ_ONCE(src->nr_running) < 2 || !READ_ONCE(src->cfs.h_nr_runnable))
			continue;
		if ((*tried)++ >= K618_SRC_MAX)
			break;
		/* keep our own lock: a contended queue is simply not tried this time */
		if (!raw_spin_rq_trylock(src))
			continue;
		if (!now)
			now = sched_clock();
		p = k618_waiting_draw(src, top, now, this_cpu, down);
		if (p) {
			update_rq_clock(src);
			deactivate_task(src, p, DEQUEUE_NOCLOCK);
			set_task_cpu(p, this_cpu);
		}
		raw_spin_rq_unlock(src);
		if (p)
			break;
	}
	return p;
}

static noinline void k618_newidle_pull(struct rq *this_rq, struct cgroup *top, int *pulled_task,
				       int *done)
{
	int this_cpu = cpu_of(this_rq), tried = 0;
	struct task_struct *p;

	if (this_rq->ttwu_pending || !cpu_active(this_cpu))
		return;
	p = k618_pull_from(&k618_src_same[this_cpu], top, this_cpu, false, &tried);
	if (!p && READ_ONCE(pull_down) && tried < K618_SRC_MAX) {
		p = k618_pull_from(&k618_src_up[this_cpu], top, this_cpu, true, &tried);
		if (p)
			K618_INC(pull_down_moves);
	}
	if (!p)
		return;
	/* __schedule() updated our clock before it came here */
	activate_task(this_rq, p, ENQUEUE_NOCLOCK);
	wakeup_preempt(this_rq, p, 0);
	*pulled_task = 1;
	*done = 1;
	K618_INC(pulls);
	K618_INC(pulls_newidle);
}

/* sched_balance_newidle(), first thing, with this_rq locked and pinned */
static void k618_newidle(void *unused, struct rq *this_rq, struct rq_flags *rf, int *pulled_task,
			 int *done)
{
	int cpu = cpu_of(this_rq);
	struct cgroup *top;

	if (!static_branch_unlikely(&k618_pull_key) || *done)
		return;
	top = READ_ONCE(k618_top);
	if (!k618_looks(top))
		return;
	if (k618_any_waiting(&k618_src_same[cpu]) ||
	    (READ_ONCE(pull_down) && k618_any_waiting(&k618_src_up[cpu])))
		k618_newidle_pull(this_rq, top, pulled_task, done);
}

/* ---------------------------------------------------------------- (3) slices */

/* a slice set here has 0x2a5 in its low 10 bits (a value someone picks on purpose has no reason to) */
#define K618_SLICE_MASK	0x3ffULL
#define K618_SLICE_MARK	0x2a5ULL

static __always_inline bool k618_slice_ours(const struct sched_entity *se)
{
	return se->custom_slice && (se->slice & K618_SLICE_MASK) == K618_SLICE_MARK;
}

/* slice_us within sched_setattr()'s own range, 0.1-100 ms, with the mark */
static u64 k618_slice_ns(void)
{
	u64 ns = (u64)clamp_t(unsigned int, READ_ONCE(slice_us), 100, 100000) * NSEC_PER_USEC;

	return (ns & ~K618_SLICE_MASK) | K618_SLICE_MARK;
}

/* with @p's run queue locked, or @p not yet seen by the scheduler */
static void k618_slice_set(struct task_struct *p, bool draw)
{
	struct sched_entity *se = &p->se;

	if (draw && fair_policy(p->policy)) {
		u64 ns = k618_slice_ns();

		/* never over a slice the task asked for itself */
		if (se->custom_slice && !k618_slice_ours(se))
			return;
		if (se->custom_slice && se->slice == ns)
			return;
		/*
		 * A queued task's new slice reaches the run queue's min_slice when it is next enqueued (it is
		 * read from the entities at insertion); its deadline follows at its next update.
		 */
		se->custom_slice = 1;
		se->slice = ns;
		atomic_long_inc(&k618_slice_sets);
	} else if (k618_slice_ours(se)) {
		se->custom_slice = 0;
		se->slice = sysctl_sched_base_slice;
		atomic_long_inc(&k618_slice_clears);
	}
}

static void k618_slice_update(struct task_struct *p, struct cgroup *top)
{
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(p, &rf);
	k618_slice_set(p, READ_ONCE(slice) && k618_draws(p, top));
	task_rq_unlock(rq, p, &rf);
}

/* worth a look: could be a draw thread, or carries our slice */
static __always_inline bool k618_slice_candidate(struct task_struct *p)
{
	return thread_group_leader(p) || k618_render(p) || k618_slice_ours(&p->se) ||
	       (static_branch_unlikely(&k618_test_key) && k618_test_match(p));
}

/* every thread once: a parameter changed, or the module came up after the system did */
static void k618_slice_all(void)
{
	struct cgroup *top = READ_ONCE(k618_top);
	struct task_struct *g, *t;

	rcu_read_lock();
	for_each_process_thread(g, t)
		if (k618_slice_candidate(t))
			k618_slice_update(t, top);
	rcu_read_unlock();
}

/* a move between cgroups has been made; the controller's attach callback comes next */
static void k618_cgroup_attach(void *unused, struct cgroup_subsys *ss, struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct cgroup *seen = NULL, *top;
	struct task_struct *task;

	if (ss->id != cpuset_cgrp_id)
		return;
	cgroup_taskset_for_each(task, css, tset)
		if (css->cgroup != seen) {
			seen = css->cgroup;
			k618_learn_top(seen);
		}
	if (!READ_ONCE(slice))
		return;
	top = READ_ONCE(k618_top);
	cgroup_taskset_for_each(task, css, tset)
		if (k618_slice_candidate(task))
			k618_slice_update(task, top);
}

/* __set_task_comm(), after the name changed; no lock held */
static void k618_comm(void *unused, struct task_struct *tsk, bool exec)
{
	if (!k618_have_slice)
		return;
	/* named RenderThread (or the test's name) while slices are on, or renamed away from it */
	if (k618_slice_ours(&tsk->se) ||
	    (READ_ONCE(slice) && (k618_render(tsk) ||
				  (static_branch_unlikely(&k618_test_key) && k618_test_match(tsk)))))
		k618_slice_update(tsk, READ_ONCE(k618_top));
}

/* sched_fork(), first thing: the child is a copy of the parent and not yet seen by the scheduler */
static void k618_fork(void *unused, struct task_struct *p)
{
	if (unlikely(k618_slice_ours(&p->se))) {
		p->se.custom_slice = 0;
		p->se.slice = sysctl_sched_base_slice;
		atomic_long_inc(&k618_slice_forks);
	}
}

/* __sched_setscheduler(), right after the new parameters, task dequeued, run queue locked */
static void k618_setsched(void *unused, struct task_struct *p)
{
	struct cgroup *top;

	if (!READ_ONCE(slice) || p->se.custom_slice)
		return;		/* off, or the caller asked for a slice of its own */
	top = READ_ONCE(k618_top);
	if (k618_looks(top) && k618_draws(p, top))
		k618_slice_set(p, true);
}

/* ---------------------------------------------------------------- (4) sync */

static void k618_fits_cpu(void *unused, struct task_struct *p, unsigned long util,
			  unsigned long uclamp_min, unsigned long uclamp_max, int cpu, bool *fits,
			  bool *done)
{
	if (!static_branch_unlikely(&k618_sync_key))
		return;
	/* only find_energy_efficient_cpu()'s sync test asks about a waking task, for the waker's CPU */
	if (READ_ONCE(p->__state) != TASK_WAKING || !k618_cl_of[cpu] || cpu != raw_smp_processor_id())
		return;
	if (!k618_fits(min(util, uclamp_max), uclamp_min, k618_cl_cap[0]))
		return;
	*fits = false;
	*done = true;
	K618_INC(sync_drops);
}

/* ---------------------------------------------------------------- (5) the energy model */

/*
 * K: uW per capacity unit at E = 1000 (little at 1228.8 MHz then gets 108 mW with the capacity 302
 * this kernel gives the little cores: its core's measured power for CPU-bound work there).
 */
#define K618_EM_UW	588

struct k618_em_point {
	u32 khz;
	u16 e;
};

/* generated by k618_place_em.py --c: {kHz, E x 1000}, E = energy per unit of blended work, little at
 * 1228.8 MHz = 1000 */
static const struct k618_em_point k618_em_little[] = {
	{307200, 1041}, {441600, 911}, {556800, 894}, {672000, 886}, {787200, 883}, {902400, 882},
	{1017600, 934}, {1113600, 944}, {1228800, 1000}, {1344000, 1048}, {1459200, 1116}, {1555200, 1165},
	{1670400, 1220}, {1785600, 1280}, {1900800, 1335}, {2016000, 1385},
};
static const struct k618_em_point k618_em_middle[] = {
	{499200, 1142}, {614400, 1157}, {729600, 1176}, {844800, 1208}, {940800, 1236}, {1056000, 1285},
	{1171200, 1296}, {1286400, 1344}, {1401600, 1384}, {1536000, 1440}, {1651200, 1514}, {1785600, 1591},
	{1920000, 1685}, {2054400, 1771}, {2188800, 1859}, {2323200, 1971}, {2457600, 2086}, {2592000, 2186},
	{2707200, 2327}, {2803200, 2347},
};
static const struct k618_em_point k618_em_prime[] = {
	{595200, 1580}, {729600, 1539}, {864000, 1518}, {998400, 1531}, {1132800, 1535}, {1248000, 1570},
	{1363200, 1592}, {1478400, 1617}, {1593600, 1662}, {1708800, 1714}, {1843200, 1798}, {1977600, 1868},
	{2092800, 1946}, {2227200, 2063}, {2342400, 2168}, {2476800, 2294}, {2592000, 2409}, {2726400, 2528},
	{2841600, 2651}, {2956800, 2799}, {3187200, 3161},
};

static const struct {
	const char *name;
	const struct k618_em_point *tab;
	int n;
} k618_em_tabs[] = {
	{ "little", k618_em_little, ARRAY_SIZE(k618_em_little) },
	{ "middle", k618_em_middle, ARRAY_SIZE(k618_em_middle) },
	{ "prime", k618_em_prime, ARRAY_SIZE(k618_em_prime) },
};

struct k618_pd {
	struct device *dev;
	struct em_perf_domain *pd;
	int cpu, nr, tab;
	unsigned long *dt_power;	/* the device tree's model, uW per state */
	unsigned long *dt_flags;	/* the states' flags as registered */
	unsigned long pd_skip;		/* EM_PERF_DOMAIN_SKIP_INEFFICIENCIES as registered */
	unsigned int *cf_freq;		/* cpufreq's table: frequencies ... */
	bool *cf_ineff;			/* ... and their inefficient marks, as found */
	int cf_n;
	bool cf_avail;
};

static struct k618_pd k618_pds[K618_NR_CL];
static int k618_npd;
static int k618_em_now = -1;		/* table in force: 0 device tree, 1 measured, -1 none set */
static DEFINE_MUTEX(k618_em_lock);
static struct delayed_work k618_em_work;
static int k618_em_tries;

/* E at @khz: the table's own entry, or linear in kHz between its neighbours, the end value outside */
static unsigned int k618_em_e(const struct k618_em_point *t, int n, unsigned long khz)
{
	int i;

	if (khz <= t[0].khz)
		return t[0].e;
	for (i = 1; i < n; i++) {
		if (khz == t[i].khz)
			return t[i].e;
		if (khz < t[i].khz)
			return t[i - 1].e + (int)(t[i].e - t[i - 1].e) * (long)(khz - t[i - 1].khz) /
					    (long)(t[i].khz - t[i - 1].khz);
	}
	return t[n - 1].e;
}

static unsigned long k618_em_power(const struct k618_pd *d, const struct em_perf_state *ps)
{
	const struct k618_em_point *t = k618_em_tabs[d->tab].tab;
	u64 uw = (u64)K618_EM_UW * k618_em_e(t, k618_em_tabs[d->tab].n, ps->frequency) * ps->performance;

	return clamp_t(u64, div_u64(uw, 1000), 1, EM_MAX_POWER);
}

static void k618_em_release(struct kref *kref)
{
	struct em_perf_table *t = container_of(kref, struct em_perf_table, kref);

	kfree_rcu(t, rcu);
}

/* cpufreq's inefficient marks and the domain's skip flag back as they were found */
static void k618_cpufreq_restore(struct k618_pd *d)
{
	struct cpufreq_frequency_table *pos;
	struct cpufreq_policy *policy;
	int i;

	policy = cpufreq_cpu_get(d->cpu);
	if (policy) {
		if (policy->freq_table) {
			cpufreq_for_each_valid_entry(pos, policy->freq_table) {
				bool was = false;

				for (i = 0; i < d->cf_n; i++)
					if (d->cf_freq[i] == pos->frequency)
						was = d->cf_ineff[i];
				if (was)
					pos->flags |= CPUFREQ_INEFFICIENT_FREQ;
				else
					pos->flags &= ~CPUFREQ_INEFFICIENT_FREQ;
			}
		}
		policy->efficiencies_available = d->cf_avail;
		cpufreq_cpu_put(policy);
	}
	d->pd->flags = (d->pd->flags & ~EM_PERF_DOMAIN_SKIP_INEFFICIENCIES) | d->pd_skip;
}

/* put table @which (0 device tree, 1 measured) into @d, costs as em_compute_costs() makes them */
static int k618_em_apply_one(struct k618_pd *d, int which, bool ineff)
{
	unsigned long prev = ULONG_MAX;
	struct em_perf_table *t;
	int i, ret;

	t = kzalloc(struct_size(t, state, d->nr), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	kref_init(&t->kref);
	rcu_read_lock();
	memcpy(t->state, rcu_dereference(d->pd->em_table)->state, d->nr * sizeof(*t->state));
	rcu_read_unlock();

	for (i = 0; i < d->nr; i++)
		t->state[i].power = which ? k618_em_power(d, &t->state[i]) : d->dt_power[i];
	for (i = d->nr - 1; i >= 0; i--) {
		struct em_perf_state *ps = &t->state[i];

		ps->cost = ps->power * 10 / max(ps->performance, 1UL);
		if (which && ineff) {
			ps->flags = ps->cost >= prev ? EM_PERF_STATE_INEFFICIENT : 0;
			if (!ps->flags)
				prev = ps->cost;
		} else {
			ps->flags = d->dt_flags[i];
		}
	}
	ret = em_dev_update_perf_domain(d->dev, t);
	/* the domain holds its own reference now; on an error this frees the table */
	kref_put(&t->kref, k618_em_release);
	if (ret)
		return ret;
	if (!(which && ineff))
		k618_cpufreq_restore(d);
	return 0;
}

static void k618_em_apply(int which, bool ineff)
{
	int i, ret, done = 0;

	lockdep_assert_held(&k618_em_lock);
	for (i = 0; i < k618_npd; i++) {
		ret = k618_em_apply_one(&k618_pds[i], which, ineff);
		if (ret)
			pr_err("cpu%d: energy model not updated (%d)\n", k618_pds[i].cpu, ret);
		else
			done++;
	}
	if (done) {
		k618_em_now = which;
		atomic_long_inc(&k618_em_swaps);
		pr_info("energy model: %s%s in %d of %d domains\n", which ? "measured" : "device tree",
			which && ineff ? " with its inefficient OPPs" : "", done, k618_npd);
	}
}

/* the domain of @cpu: snapshot of what the kernel registered, and our table for it */
static int k618_em_add(int cpu)
{
	struct cpufreq_frequency_table *pos;
	struct cpufreq_policy *policy;
	struct k618_pd *d = &k618_pds[k618_npd];
	struct em_perf_state *st;
	int i;

	d->cpu = cpu;
	d->dev = get_cpu_device(cpu);
	d->pd = em_pd_get(d->dev);
	if (!d->pd)
		return -EAGAIN;
	d->nr = d->pd->nr_perf_states;
	d->dt_power = kcalloc(d->nr, sizeof(*d->dt_power), GFP_KERNEL);
	d->dt_flags = kcalloc(d->nr, sizeof(*d->dt_flags), GFP_KERNEL);
	if (!d->dt_power || !d->dt_flags)
		return -ENOMEM;
	rcu_read_lock();
	st = rcu_dereference(d->pd->em_table)->state;
	for (i = 0; i < d->nr; i++) {
		d->dt_power[i] = st[i].power;
		d->dt_flags[i] = st[i].flags;
	}
	rcu_read_unlock();
	/* the device tree's model from the OPPs' voltages, as the kernel registered it */
	for (i = 0; i < d->nr; i++) {
		unsigned long uw, khz, want;

		rcu_read_lock();
		want = rcu_dereference(d->pd->em_table)->state[i].frequency;
		rcu_read_unlock();
		khz = want;
		if (!dev_pm_opp_calc_power(d->dev, &uw, &khz) && uw && khz == want)
			d->dt_power[i] = uw;
	}
	d->pd_skip = d->pd->flags & EM_PERF_DOMAIN_SKIP_INEFFICIENCIES;

	policy = cpufreq_cpu_get(cpu);
	if (policy && policy->freq_table) {
		int n = 0;

		cpufreq_for_each_valid_entry(pos, policy->freq_table)
			n++;
		d->cf_freq = kcalloc(n, sizeof(*d->cf_freq), GFP_KERNEL);
		d->cf_ineff = kcalloc(n, sizeof(*d->cf_ineff), GFP_KERNEL);
		if (d->cf_freq && d->cf_ineff) {
			cpufreq_for_each_valid_entry(pos, policy->freq_table) {
				d->cf_freq[d->cf_n] = pos->frequency;
				d->cf_ineff[d->cf_n++] = pos->flags & CPUFREQ_INEFFICIENT_FREQ;
			}
		}
		d->cf_avail = policy->efficiencies_available;
	}
	if (policy)
		cpufreq_cpu_put(policy);
	k618_npd++;
	return 0;
}

/* find the domains; three, told apart by capacity and then by top frequency, get the three tables */
static int k618_em_setup(void)
{
	unsigned long top[K618_NR_CL];
	struct cpumask seen;
	int cpu, i, j, ret;

	cpumask_clear(&seen);
	for_each_possible_cpu(cpu) {
		struct em_perf_domain *pd = em_cpu_get(cpu);

		if (!pd)
			return -EAGAIN;		/* cpufreq is not up yet */
		if (cpumask_test_cpu(cpu, &seen))
			continue;
		cpumask_or(&seen, &seen, em_span_cpus(pd));
		if (k618_npd == K618_NR_CL)
			return -E2BIG;
		ret = k618_em_add(cpu);
		if (ret)
			return ret;
	}
	if (k618_npd != ARRAY_SIZE(k618_em_tabs)) {
		pr_info("%d perf domains, the measured model has 3: device tree model kept\n", k618_npd);
		return -ENODEV;
	}
	for (i = 0; i < k618_npd; i++) {
		rcu_read_lock();
		top[i] = rcu_dereference(k618_pds[i].pd->em_table)->state[k618_pds[i].nr - 1].frequency;
		rcu_read_unlock();
	}
	/* smallest capacity first, then lowest top frequency */
	for (i = 1; i < k618_npd; i++)
		for (j = i; j > 0; j--) {
			unsigned long ca = arch_scale_cpu_capacity(k618_pds[j - 1].cpu);
			unsigned long cb = arch_scale_cpu_capacity(k618_pds[j].cpu);

			if (ca < cb || (ca == cb && top[j - 1] <= top[j]))
				break;
			swap(k618_pds[j - 1], k618_pds[j]);
			swap(top[j - 1], top[j]);
		}
	for (i = 0; i < k618_npd; i++) {
		const struct k618_em_point *t = k618_em_tabs[i].tab;
		unsigned long want = t[k618_em_tabs[i].n - 1].khz;

		k618_pds[i].tab = i;
		if (top[i] * 10 < want * 9 || top[i] * 9 > want * 10) {
			pr_info("cpu%d: top %lu kHz, the %s table ends at %lu: device tree model kept\n",
				k618_pds[i].cpu, top[i], k618_em_tabs[i].name, want);
			return -ENODEV;
		}
	}
	return 0;
}

static void k618_em_workfn(struct work_struct *work)
{
	int ret;

	mutex_lock(&k618_em_lock);
	ret = k618_em_setup();
	if (ret == -EAGAIN) {
		/* forget a half-found set and look again */
		while (k618_npd > 0) {
			struct k618_pd *d = &k618_pds[--k618_npd];

			kfree(d->dt_power);
			kfree(d->dt_flags);
			kfree(d->cf_freq);
			kfree(d->cf_ineff);
			memset(d, 0, sizeof(*d));
		}
		if (++k618_em_tries < 60)
			schedule_delayed_work(&k618_em_work, 2 * HZ);
		else
			pr_err("no energy model after 120 s: device tree model kept\n");
	} else if (ret) {
		k618_npd = 0;
	} else if (READ_ONCE(em)) {
		k618_em_apply(1, READ_ONCE(em_ineff));
	} else {
		k618_em_now = 0;
	}
	mutex_unlock(&k618_em_lock);
}

static void k618_em_param(void)
{
	mutex_lock(&k618_em_lock);
	if (k618_npd && k618_em_now >= 0)
		k618_em_apply(READ_ONCE(em) ? 1 : 0, READ_ONCE(em_ineff));
	mutex_unlock(&k618_em_lock);
}

/* ---------------------------------------------------------------- parameter plumbing */

static void k618_key(struct static_key_false *key, bool on)
{
	if (on)
		static_branch_enable(key);
	else
		static_branch_disable(key);
}

static void k618_keys(void)
{
	k618_key(&k618_rt_key, READ_ONCE(rt) && k618_have_rt);
	k618_key(&k618_pull_key, READ_ONCE(pull) && k618_have_pull);
	k618_key(&k618_sync_key, READ_ONCE(sync) && k618_have_sync);
	k618_key(&k618_test_key, test_comm[0]);
}

static void k618_changed(const struct kernel_param *kp)
{
	if (!READ_ONCE(k618_live))
		return;
	k618_keys();
	if (kp->arg == &slice || kp->arg == &slice_us || kp->arg == test_comm)
		k618_slice_all();
	if (kp->arg == &em || kp->arg == &em_ineff)
		k618_em_param();
}

static int k618_set_bool(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (!ret)
		k618_changed(kp);
	return ret;
}

static int k618_set_uint(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	if (kp->arg == &em && v > 1)
		return -EINVAL;
	if (kp->arg == &slice_us && (v < 100 || v > 100000))
		return -EINVAL;
	if (kp->arg == &pull_wait_us && v > 1000000)
		return -EINVAL;
	*(unsigned int *)kp->arg = v;
	k618_changed(kp);
	return 0;
}

static int k618_set_comm(const char *val, const struct kernel_param *kp)
{
	strscpy(test_comm, val, sizeof(test_comm));
	test_comm[strcspn(test_comm, "\n")] = '\0';
	k618_changed(kp);
	return 0;
}

static int k618_get_comm(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%s\n", test_comm);
}

static const struct kernel_param_ops k618_bool_ops = { .set = k618_set_bool, .get = param_get_bool };
static const struct kernel_param_ops k618_uint_ops = { .set = k618_set_uint, .get = param_get_uint };
static const struct kernel_param_ops k618_comm_ops = { .set = k618_set_comm, .get = k618_get_comm };

module_param_cb(rt, &k618_bool_ops, &rt, 0644);
MODULE_PARM_DESC(rt, "RT tasks: little cores first, never onto a CPU running a draw thread");
module_param_cb(pull, &k618_bool_ops, &pull, 0644);
MODULE_PARM_DESC(pull, "pull a draw thread waiting pull_wait_us to an idle CPU");
module_param_cb(pull_wait_us, &k618_uint_ops, &pull_wait_us, 0644);
module_param_cb(pull_down, &k618_bool_ops, &pull_down, 0644);
MODULE_PARM_DESC(pull_down, "pull also to a smaller cluster, when the thread fits it");
module_param_cb(slice, &k618_bool_ops, &slice, 0644);
MODULE_PARM_DESC(slice, "draw threads get an EEVDF slice of slice_us");
module_param_cb(slice_us, &k618_uint_ops, &slice_us, 0644);
module_param_cb(sync, &k618_bool_ops, &sync, 0644);
MODULE_PARM_DESC(sync, "no sync wake-up from a middle / prime CPU for a task that fits a little one");
module_param_cb(em, &k618_uint_ops, &em, 0644);
MODULE_PARM_DESC(em, "CPU energy model: 0 device tree, 1 measured");
module_param_cb(em_ineff, &k618_bool_ops, &em_ineff, 0644);
MODULE_PARM_DESC(em_ineff, "let the measured model's inefficient OPPs be skipped by EAS and cpufreq");
module_param_cb(test_comm, &k618_comm_ops, test_comm, 0644);
MODULE_PARM_DESC(test_comm, "QEMU test only: a task of this name counts as a draw thread");

static int k618_get_state(char *buf, const struct kernel_param *kp)
{
	struct k618_stat s = {};
	int cpu, n;

	for_each_possible_cpu(cpu) {
		struct k618_stat *c = per_cpu_ptr(&k618_stat, cpu);

		s.rt_picks += READ_ONCE(c->rt_picks);
		s.rt_little += READ_ONCE(c->rt_little);
		s.rt_draw_skips += READ_ONCE(c->rt_draw_skips);
		s.rt_kernel += READ_ONCE(c->rt_kernel);
		s.pull_kicks += READ_ONCE(c->pull_kicks);
		s.pulls += READ_ONCE(c->pulls);
		s.pulls_newidle += READ_ONCE(c->pulls_newidle);
		s.pull_down_moves += READ_ONCE(c->pull_down_moves);
		s.sync_drops += READ_ONCE(c->sync_drops);
	}
	n = sysfs_emit(buf, "top_app %s\nclusters %d\n", READ_ONCE(k618_top) ? "found" : "not yet", k618_ncl);
	n += sysfs_emit_at(buf, n, "hooks rt %d pull %d slice %d sync %d\n",
			   k618_have_rt, k618_have_pull, k618_have_slice, k618_have_sync);
	n += sysfs_emit_at(buf, n, "rt_picks %lu\nrt_little %lu\nrt_draw_skips %lu\nrt_kernel %lu\n",
			   s.rt_picks, s.rt_little, s.rt_draw_skips, s.rt_kernel);
	n += sysfs_emit_at(buf, n, "pull_kicks %lu\npulls %lu\npulls_newidle %lu\npull_down_moves %lu\n",
			   s.pull_kicks, s.pulls, s.pulls_newidle, s.pull_down_moves);
	n += sysfs_emit_at(buf, n, "slice_sets %ld\nslice_clears %ld\nslice_forks %ld\n",
			   atomic_long_read(&k618_slice_sets), atomic_long_read(&k618_slice_clears),
			   atomic_long_read(&k618_slice_forks));
	n += sysfs_emit_at(buf, n, "sync_drops %lu\n", s.sync_drops);
	n += sysfs_emit_at(buf, n, "em %s\nem_domains %d\nem_swaps %ld\n",
			   k618_em_now < 0 ? "not set" : k618_em_now ? "measured" : "device tree",
			   k618_npd, atomic_long_read(&k618_em_swaps));
	return n;
}

static int k618_get_em_table(char *buf, const struct kernel_param *kp)
{
	int i, j, n = 0;

	mutex_lock(&k618_em_lock);
	for (i = 0; i < k618_npd; i++) {
		struct k618_pd *d = &k618_pds[i];
		struct em_perf_state *st;

		n += sysfs_emit_at(buf, n, "cpu%d %s: kHz perf dt_uW uW cost ineff\n", d->cpu,
				   k618_em_tabs[d->tab].name);
		rcu_read_lock();
		st = rcu_dereference(d->pd->em_table)->state;
		for (j = 0; j < d->nr && n < PAGE_SIZE - 64; j++)
			n += sysfs_emit_at(buf, n, "%lu %lu %lu %lu %lu %d\n", st[j].frequency,
					   st[j].performance, d->dt_power[j], st[j].power, st[j].cost,
					   !!(st[j].flags & EM_PERF_STATE_INEFFICIENT));
		rcu_read_unlock();
	}
	mutex_unlock(&k618_em_lock);
	return n;
}

static const struct kernel_param_ops k618_state_ops = { .get = k618_get_state };
static const struct kernel_param_ops k618_em_table_ops = { .get = k618_get_em_table };
module_param_cb(state, &k618_state_ops, NULL, 0444);
module_param_cb(em_table, &k618_em_table_ops, NULL, 0444);

/* ---------------------------------------------------------------- init */

/* the module came up after the system did: top-app is already there */
static void __init k618_find_top(void)
{
	struct task_struct *g, *t;

	rcu_read_lock();
	for_each_process_thread(g, t) {
		struct cgroup *cg = task_css(t, cpuset_cgrp_id)->cgroup;

		if (k618_is_top_app(cg) || (cgroup_parent(cg) && k618_is_top_app(cgroup_parent(cg)))) {
			WRITE_ONCE(k618_top, k618_is_top_app(cg) ? cg : cgroup_parent(cg));
			break;
		}
	}
	rcu_read_unlock();
}

static int __init k618_place_init(void)
{
	int r1, r2, r3, r4, r5, r6, r7, r8, r9, cpu;

	k618_topology();
	k618_find_top();
	for_each_possible_cpu(cpu)
		INIT_CSD(per_cpu_ptr(&k618_pull_csd, cpu), k618_pull_ipi, NULL);

	r1 = register_trace_android_rvh_select_task_rq_rt(k618_select_rt, NULL);
	r2 = register_trace_android_rvh_find_lowest_rq(k618_lowest_rq, NULL);
	k618_have_rt = !r1 && !r2;
	r3 = register_trace_android_rvh_sched_newidle_balance(k618_newidle, NULL);
	r4 = register_trace_android_vh_scheduler_tick(k618_tick, NULL);
	k618_have_pull = !r3 && !r4;
	r5 = register_trace_android_rvh_task_fits_cpu(k618_fits_cpu, NULL);
	k618_have_sync = !r5;
	r6 = register_trace_android_vh_cgroup_attach(k618_cgroup_attach, NULL);
	r7 = register_trace_android_rvh_set_task_comm(k618_comm, NULL);
	r8 = register_trace_android_rvh_sched_fork(k618_fork, NULL);
	r9 = register_trace_android_rvh_setscheduler(k618_setsched, NULL);
	k618_have_slice = !r6 && !r7 && !r8 && !r9;
	if (r1 || r2 || r3 || r4 || r5 || r6 || r7 || r8 || r9)
		pr_err("hooks refused: select_task_rq_rt %d find_lowest_rq %d sched_newidle_balance %d "
		       "scheduler_tick %d task_fits_cpu %d cgroup_attach %d set_task_comm %d "
		       "sched_fork %d setscheduler %d\n", r1, r2, r3, r4, r5, r6, r7, r8, r9);

	WRITE_ONCE(k618_live, true);
	k618_keys();
	if (READ_ONCE(slice) && k618_have_slice)
		k618_slice_all();

	INIT_DELAYED_WORK(&k618_em_work, k618_em_workfn);
	schedule_delayed_work(&k618_em_work, 0);

	pr_info("%d clusters (capacity %lu..%lu); rt %d pull %d (%u us%s) slice %d (%u us) sync %d em %u\n",
		k618_ncl, k618_cl_cap[0], k618_cl_cap[k618_ncl - 1], rt && k618_have_rt,
		pull && k618_have_pull, pull_wait_us, pull_down ? ", down" : "", slice && k618_have_slice,
		slice_us, sync && k618_have_sync, em);
	/*
	 * Always 0: a restricted hook that went in cannot come out again, so the module must stay even
	 * when some hook was refused (that part then stays off).
	 */
	return 0;
}
module_init(k618_place_init);
/* no module_exit: see above */

MODULE_DESCRIPTION("k618: placement changes around EAS and a measured CPU energy model");
MODULE_LICENSE("GPL v2");
