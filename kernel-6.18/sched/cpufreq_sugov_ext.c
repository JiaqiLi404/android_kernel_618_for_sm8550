// SPDX-License-Identifier: GPL-2.0
/*
 * sugov_ext -- schedutilmium, a cpufreq governor on schedutil's skeleton (android17-6.18).
 *
 * The governor this project first wrote for the phone's 5.15 kernel, with the same rules, ported to
 * 6.18: where it copies schedutil's skeleton it follows 6.18's kernel/sched/cpufreq_schedutil.c,
 * and its built-in tunables are the values the 5.15 kernel's user-space tuning module wrote, so
 * that the kernel alone runs as the phone did there (sx_tunables_defaults()).
 * kernel-6.18/k618_sched.py lists what differs from 5.15 and why.
 *
 * It takes the scheduler's utilisation as schedutil does and differs from it in these rules:
 *
 *  - two rate limits instead of one: the frequency goes up no sooner than up_rate_limit_us after
 *    it last changed and down no sooner than down_rate_limit_us (500 and 1000); a change of the
 *    policy's limits is held by neither (a maximum lowered by thermal throttling acts at once)
 *  - a request in the lowest fifth of the gap between two steps of the frequency table is served
 *    by the lower step
 *  - what is runnable but has not run counts as utilisation (cpu_util_cfs_boost())
 *  - "enabled" (1): the utilisation is replaced by how busy the CPU was and mapped to a frequency
 *    with a steeper slope (target_loads, target_load_thresh, target_load_shift). With "refined" (1)
 *    the busy share is the CPU's busy time from the kernel's idle accounting, weighted by the
 *    frequency it ran at; with refined at 0 the run time of the tasks it ran, in windows of
 *    ravg_window_us, and the tick-sampled share of slim WALT (use_slim_walt) stand in. A CPU busy
 *    all of the last react_us is "saturated" and gets the step above. With enabled at 0 it is
 *    schedutil with the rules above.
 *  - "boost": percent more of the utilisation, or less, before it becomes a frequency -- what a
 *    mode (save power, balanced, performance) comes to in one number
 *  - floors (kHz, 0: none): "floor", for user space to write when it knows what the scheduler's
 *    numbers do not show, and "touch_floor", which holds for touch_ms after the touch screen was
 *    last touched. On the 13 Pro the threads that put a frame on the screen run on the little
 *    cores, and a cluster left to drop its frequency between two frames has the compositor miss
 *    frames. A request is never below a floor; the policy's own limits hold as ever.
 *  - a ceiling (kHz, 0: none): "ceiling", written by user space, or with "caps" one that follows
 *    the load (sx_caps_ceiling()), lifted for an app that comes to the front, a finger put down and
 *    a burst of a thread that draws (caps_ui_ms). It is not the policy's maximum, and that is what
 *    it is for: the scheduler sizes a CPU by its policy's maximum, so a ceiling held there also
 *    tells it the cluster has become small and tasks leave it; this one holds the frequency alone.
 *  - schedutil's boost for a task that wakes after an I/O wait only with "iowait_boost" at 1: a
 *    few small synchronous reads were enough to take an idle cluster to its top for a tick, and
 *    the flash does not answer faster for it
 *  - a policy's tunables outlive its CPUs going offline (sx_kept); a change of governor forgets
 *    them
 *
 * EAS takes this governor as it takes schedutil: sugov_is_governor() asks sugov_ext_is_governor(),
 * and the scheduling domains' energy data is rebuilt when the governor starts and stops on a
 * policy, as schedutil has it rebuilt.
 *
 * The rules are in sugov_ext_math.h, free of kernel state; this file is the plumbing.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "sched.h"

#include <linux/ctype.h>
#include <linux/energy_model.h>
#include <linux/input.h>
#include <linux/sched/clock.h>
#include <linux/sched/cpufreq.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <uapi/linux/sched/types.h>
#include <trace/events/cgroup.h>
#include <trace/hooks/sched.h>

#include "slim_walt.h"
#include "sugov_ext.h"
#include "sugov_ext_math.h"

#define IOWAIT_BOOST_MIN	(SCHED_CAPACITY_SCALE / 8)
#define SX_MAX_STEPS		16
/* the CPUs of a policy and the steps of its table kept as lists (sx_lists); a bigger one goes without */
#define SX_LIST_CPUS		8
#define SX_LIST_STEPS		32
/* refined: a busy share from which on a CPU counts as busy all the time (of 1024) */
#define SX_SATURATED		1000

/* the target loads; replaced as a whole, read under RCU */
struct sx_loads {
	struct rcu_head		rcu;
	unsigned int		nr;
	struct sx_load_step	step[SX_MAX_STEPS];		/* lowest frequency first */
};

struct sx_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		up_rate_limit_us;
	unsigned int		down_rate_limit_us;
	int			boost;		/* percent more (or less) of the utilisation */
	unsigned int		floor;		/* kHz: no request is below it */
	unsigned int		touch_floor;	/* kHz: nor below this for touch_ms after a touch */
	unsigned int		ceiling;	/* kHz: no request is above it (0: none) */
	bool			iowait_boost;	/* schedutil's boost after I/O waits (off: 0) */
	unsigned int		touch_ms;

	/* a ceiling that follows the load (sx_caps_ceiling; off: 0) */
	bool			caps;
	unsigned int		caps_boost;	/* percent on the smoothed demand */
	unsigned int		caps_floor;	/* kHz: the ceiling is never below it (0: half the top step) */
	unsigned int		caps_max;	/* kHz: nor above it, but while it is lifted (0: no such limit) */
	unsigned int		caps_burst;	/* kHz: nor above it until a load has lasted (0: no such limit) */
	unsigned int		caps_burst_ms;	/* that long */
	unsigned int		caps_front_ms;	/* an app come to the front lifts it this long */
	unsigned int		caps_front_pct;	/* to this share of the top frequency (100: no ceiling) */
	unsigned int		caps_touch_ms;	/* a finger put down lifts it this long */
	unsigned int		caps_touch;	/* to this frequency (kHz) */
	unsigned int		caps_ui_ms;	/* a burst this long of a thread that draws lifts it as caps_front_ms does (0: none) */

	struct sx_loads __rcu	*loads;
	unsigned int		target_load_thresh;
	unsigned int		target_load_shift;
	unsigned int		ravg_window_us;
	bool			enabled;
	bool			use_slim_walt;
	bool			refined;
	unsigned int		react_us;	/* refined: the window under way counts from this long on */

	/* where they are kept when their last policy goes, and whether (sx_kept) */
	unsigned int		home;
	bool			keep;
};

struct sx_policy {
	/* schedutil's struct sugov_policy, member for member */
	struct cpufreq_policy	*policy;

	struct sx_tunables	*tunables;
	struct list_head	tunables_hook;

	raw_spinlock_t		update_lock;
	u64			last_freq_update_time;
	s64			freq_update_delay_ns;	/* the shorter of the two rate limits */
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;

	/* The next fields are only needed if fast switch cannot be used: */
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;

	bool			limits_changed;
	bool			need_freq_update;

	/* and these are not */
	s64			up_rate_delay_ns;
	s64			down_rate_delay_ns;

	bool			started;
	bool			slim;		/* holds a reference on slim WALT */
	/* refined: until when after a start the frequency is not lowered (sx_warm); 0: not set yet */
	u64			warm_until;

	/*
	 * caps: the smoothed demand (SX_CAPS_NONE: no sample yet; kept over a stop and start of the
	 * governor -- every system suspend is one), when the CPUs were last looked at, from when the
	 * cluster has been busy look after look (0: it is not; caps_burst), the table's top step that
	 * is no boost frequency, the ceiling of the last request (0: none), and whether the rule was
	 * on at it (turned on, it starts afresh)
	 */
	u32			caps_smooth;
	u64			caps_look;
	u64			caps_burst_since;
	unsigned int		caps_fmax;
	unsigned int		caps_now;
	bool			caps_on;
	/* caps_ui_ms: lifted until then (the scheduler's clock; 0: not since the start), and how often it was */
	u64			ui_until;
	unsigned long		ui_lifts;

	/*
	 * The policy's CPUs, and its table's valid steps in ascending order without repeats, taken at
	 * every start (sx_lists; 0: too many, cpumask and table are walked instead). Every look at the
	 * policy goes through both: cpumask_next() is a call per CPU, and cpufreq's own lookup of a step
	 * a walk over all of the table's entries, three to five times per look.
	 */
	unsigned int		nr_cpus;
	unsigned int		cpus[SX_LIST_CPUS];
	unsigned int		nr_steps;
	unsigned int		steps[SX_LIST_STEPS];
	/* the steps within the policy's limits as they were last seen ([lim_lo, lim_hi) for lim_min, lim_max) */
	unsigned int		lim_min;
	unsigned int		lim_max;
	unsigned int		lim_lo;
	unsigned int		lim_hi;
	/* arch_scale_freq_invariant() at the start (a call on every look otherwise; it is settled at boot) */
	bool			invariant;
};

/*
 * cached_raw_freq when there is none to go by. Not 0: with "refined" a CPU that did nothing asks
 * for 0, and such a request would be answered with the frequency of the last one that differed.
 */
#define SX_NO_RAW_FREQ	UINT_MAX

struct sx_cpu {
	struct update_util_data	update_util;
	struct sx_policy	*sg_policy;
	unsigned int		cpu;

	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;

	/* the performance asked for, the headroom (map_util_perf()) in it, as 6.18's sugov_cpu.util */
	unsigned long		util;
	/* the least that has to be given (6.18's bw_min; cpufreq_driver_adjust_perf()) */
	unsigned long		bw_min;
	/* the bandwidth of deadline tasks at the last look (sx_dl_bw()) */
	unsigned long		bw_dl;
	unsigned long		max;
	/* refined: the busy share says busy all the time (SX_SATURATED) */
	bool			saturated;
	/* the run queue's ceiling (uclamp) when no real-time task was runnable on it */
	unsigned long		fair_max;

	/* The field below is for single-CPU policies only: */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long		saved_idle_calls;
#endif

	struct sx_runwin	runwin;
	struct sx_busyrec	busyrec;	/* refined */
	struct sx_caps_cpu	caps;

	/*
	 * caps_ui_ms: the thread that draws this CPU ran at its last look (compared, never followed),
	 * and its run time and the run queue's clock when its burst began (sx_ui_look)
	 */
	const struct task_struct *ui_task;
	u64			ui_sum;
	u64			ui_since;
};

static DEFINE_PER_CPU(struct sx_cpu, sx_cpu);

/************************ Governor internals ***********************/

/* the lists sx_policy keeps of its CPUs and of its table's steps (at the start of the governor) */
static void sx_lists(struct sx_policy *sg_policy)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	struct cpufreq_frequency_table *pos;
	unsigned int cpu, n = 0, i;
	bool fits = true;

	for_each_cpu(cpu, policy->cpus) {
		if (n == SX_LIST_CPUS) {
			fits = false;
			break;
		}
		sg_policy->cpus[n++] = cpu;
	}
	sg_policy->nr_cpus = fits ? n : 0;

	n = 0;
	fits = !!policy->freq_table;
	if (fits) {
		cpufreq_for_each_valid_entry(pos, policy->freq_table) {
			unsigned int f = pos->frequency;

			for (i = 0; i < n && sg_policy->steps[i] < f; i++)
				;
			if (i < n && sg_policy->steps[i] == f)
				continue;
			if (n == SX_LIST_STEPS) {
				fits = false;
				break;
			}
			memmove(&sg_policy->steps[i + 1], &sg_policy->steps[i], (n - i) * sizeof(sg_policy->steps[0]));
			sg_policy->steps[i] = f;
			n++;
		}
	}
	sg_policy->nr_steps = fits ? n : 0;
	/* (limits no policy has: worked out at the first lookup) */
	sg_policy->lim_min = UINT_MAX;
	sg_policy->lim_max = 0;
}

/*
 * The table's step for @khz within the policy's limits, as cpufreq_frequency_table_target() finds
 * it without "efficient" steps (sugov_ext_math.h, 8): @high false (CPUFREQ_RELATION_L), the lowest
 * at or above, else the highest under it; true (CPUFREQ_RELATION_H), the highest at or below, else
 * the lowest above. From the sorted list, or cpufreq's own walk where there is none or no step lies
 * within the limits (cpufreq warns then).
 */
static unsigned int sx_step(struct sx_policy *sg_policy, unsigned int khz, bool high)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int min = READ_ONCE(policy->min), max = READ_ONCE(policy->max);

	if (sg_policy->nr_steps) {
		/* (the limits' place in the list is worked out again when they change; under the policy's lock) */
		if (min != sg_policy->lim_min || max != sg_policy->lim_max) {
			sx_steps_limits(sg_policy->steps, sg_policy->nr_steps, min, max, &sg_policy->lim_lo, &sg_policy->lim_hi);
			sg_policy->lim_min = min;
			sg_policy->lim_max = max;
		}
		if (sg_policy->lim_lo < sg_policy->lim_hi)
			return sx_steps_pick(sg_policy->steps, sg_policy->nr_steps, sg_policy->lim_lo, sg_policy->lim_hi,
					     khz, high);
	}
	if (!policy->freq_table)
		return clamp_val(khz, min, max);
	return policy->freq_table[cpufreq_frequency_table_target(policy, khz, min, max,
			high ? CPUFREQ_RELATION_H : CPUFREQ_RELATION_L)].frequency;
}

/*
 * cpufreq_driver_resolve_freq() without what it leaves in the policy for the driver
 * (cached_resolved_idx: the table entry Qualcomm's fast switch writes to the hardware) -- the
 * frequency is worked out on every look, and resolved for the driver only when it changes
 * (sx_switch).
 */
static unsigned int sx_resolve(struct sx_policy *sg_policy, unsigned int khz)
{
	struct cpufreq_policy *policy = sg_policy->policy;

	return sx_step(sg_policy, clamp_val(khz, READ_ONCE(policy->min), READ_ONCE(policy->max)), false);
}

/*
 * A new frequency to the driver. Qualcomm's fast switch writes the table entry the cpufreq core
 * resolved last, whatever frequency it is called with, so the step is resolved first. 6.18 resolves
 * to the steps the energy model calls efficient only, once it has found any inefficient (5.15 had
 * no such rule); the governor has picked its step among all of them, so where cpufreq would take
 * another the entry is set to the step's own, as cpufreq's resolution does it.
 */
static void sx_switch(struct sx_policy *sg_policy, unsigned int next_f)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int f = cpufreq_driver_resolve_freq(policy, next_f);

	if (unlikely(f != next_f && policy->efficiencies_available && policy->freq_table)) {
		unsigned int min = READ_ONCE(policy->min), max = READ_ONCE(policy->max);
		int idx;

		if (min > max)
			min = max;
		idx = cpufreq_frequency_table_target(policy, next_f, min, max, CPUFREQ_RELATION_L);
		policy->cached_resolved_idx = idx;
		policy->cached_target_freq = clamp_val(next_f, min, max);
		f = policy->freq_table[idx].frequency;
	}
	cpufreq_driver_fast_switch(policy, f);
}

static void sx_update_rate_limits(struct sx_policy *sg_policy)
{
	struct sx_tunables *tunables = sg_policy->tunables;

	sg_policy->up_rate_delay_ns = (s64)tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns = (s64)tunables->down_rate_limit_us * NSEC_PER_USEC;
	/* nothing has to be worked out before the shorter of the two has passed */
	WRITE_ONCE(sg_policy->freq_update_delay_ns,
		   min(sg_policy->up_rate_delay_ns, sg_policy->down_rate_delay_ns));
}

/* see sugov_should_update_freq() */
static bool sx_should_update_freq(struct sx_policy *sg_policy, u64 time)
{
	s64 delta_ns;

	if (!cpufreq_this_cpu_can_update(sg_policy->policy))
		return false;

	if (unlikely(READ_ONCE(sg_policy->limits_changed))) {
		WRITE_ONCE(sg_policy->limits_changed, false);
		sg_policy->need_freq_update = true;

		/* pairs with the write memory barrier in sx_limits() */
		smp_mb();

		return true;
	} else if (sg_policy->need_freq_update) {
		/* sx_ignore_dl_rate_limit() wants a new frequency to be found */
		return true;
	}

	delta_ns = time - sg_policy->last_freq_update_time;

	return delta_ns >= READ_ONCE(sg_policy->freq_update_delay_ns);
}

static bool sx_update_next_freq(struct sx_policy *sg_policy, u64 time,
				unsigned int next_freq)
{
	bool limits = sg_policy->need_freq_update;
	s64 delta_ns;

	if (limits) {
		sg_policy->need_freq_update = false;
		/* see sugov_update_next_freq() */
		if (sg_policy->next_freq == next_freq &&
		    !cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS))
			return false;
	} else if (sg_policy->next_freq == next_freq) {
		return false;
	}

	/*
	 * The two rate limits. They are not for a change of the policy's limits (nor for a deadline
	 * task's bandwidth that grew): a frequency above a maximum that was just lowered is left at once.
	 */
	delta_ns = time - sg_policy->last_freq_update_time;
	if (!limits &&
	    ((next_freq > sg_policy->next_freq && delta_ns < sg_policy->up_rate_delay_ns) ||
	     (next_freq < sg_policy->next_freq && delta_ns < sg_policy->down_rate_delay_ns))) {
		/*
		 * Too early in this direction. The request has to be worked out again once it
		 * is not: without this a steady request -- a CPU that stays at its capacity --
		 * is answered from the cache with the frequency that was not left.
		 */
		sg_policy->cached_raw_freq = SX_NO_RAW_FREQ;
		return false;
	}

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	return true;
}

static void sx_deferred_update(struct sx_policy *sg_policy)
{
	if (!sg_policy->work_in_progress) {
		sg_policy->work_in_progress = true;
		irq_work_queue(&sg_policy->irq_work);
	}
}

/* when the screen was last touched, on the scheduler's clock; 0: not since this was asked for */
static u64 sx_touch_stamp;
/*
 * caps: when a finger was last put down on it, and when a process was last moved into the cpuset
 * top-app (an app is being started or switched to) -- ktime, the clock of the CPUs' busy times;
 * 0: not since this was asked for
 */
static u64 sx_touch_down_stamp;
static u64 sx_front_stamp;

/* was the screen touched in the last touch_ms? */
static bool sx_touched(const struct sx_tunables *tunables, u64 time)
{
	unsigned int ms = READ_ONCE(tunables->touch_ms);
	u64 stamp;

	if (!ms)
		return false;
	stamp = READ_ONCE(sx_touch_stamp);
	/* (@time is a run queue's clock, which can be behind the one the stamp was taken from) */
	return stamp && (s64)(time - stamp) < (s64)ms * NSEC_PER_MSEC;
}

/* from a utilisation that has schedutil's 1.25 in it to a frequency */
static unsigned int sx_mium_map(struct sx_policy *sg_policy, unsigned long util, unsigned long max)
{
	const struct sx_tunables *tunables = sg_policy->tunables;
	const struct sx_loads *loads = rcu_dereference_sched(tunables->loads);
	unsigned long freq;

	freq = sx_mium_freq(sg_policy->policy->cpuinfo.max_freq, util, max,
			    loads ? loads->step : NULL, loads ? loads->nr : 0,
			    READ_ONCE(tunables->target_load_thresh),
			    READ_ONCE(tunables->target_load_shift));
	return min_t(unsigned long, freq, UINT_MAX);
}

/* see get_capacity_ref_freq(): the frequency the CPUs' capacity is measured at */
static __always_inline unsigned long sx_ref_freq(struct sx_policy *sg_policy)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq = arch_scale_freq_ref(policy->cpu);

	if (freq)
		return freq;

	if (sg_policy->invariant)
		return policy->cpuinfo.max_freq;

	/* (a 25 % margin: a frequency above the current one before the CPU is fully busy) */
	return policy->cur + (policy->cur >> 2);
}

static unsigned int sx_caps_ceiling(struct sx_policy *sg_policy, bool ui);

/**
 * sx_get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @sg_policy: policy object to compute the new frequency for.
 * @util: the performance asked for, with schedutil's headroom (sx_get_util()).
 * @max: CPU capacity.
 * @time: The time of the update.
 * @saturated: a CPU of the policy was busy all the time ("refined").
 *
 * schedutil's get_next_freq() with the governor's own mapping and floors put in, and with
 * the choice between the two steps of the table that the result falls between -- but for a
 * CPU that is busy all the time: what it needs is more than its share says, whatever that
 * is, and the step below can be where it is. (In powersave, -30, such a CPU asks for 1.09
 * times its frequency: worked out for the little cores' lowest step on the 13 Pro, 307 MHz
 * with the next at 442, that is 333 MHz after the rounding of the utilisation -- inside the
 * lowest fifth of the gap, the step it is at.)
 */
static unsigned int sx_get_next_freq(struct sx_policy *sg_policy,
				     unsigned long util, unsigned long max, u64 time, bool saturated)
{
	const struct sx_tunables *tunables = sg_policy->tunables;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq = sx_ref_freq(sg_policy);
	unsigned int above, below, floor, ceiling;
	unsigned long next_freq = 0;

	util = sx_boost(util, READ_ONCE(tunables->boost));
	trace_android_vh_map_util_freq(util, freq, max, &next_freq, policy,
				       &sg_policy->need_freq_update);
	if (next_freq)
		freq = next_freq;
	else if (READ_ONCE(tunables->enabled))
		freq = sx_mium_map(sg_policy, util, max);
	else
		freq = map_util_freq(util, freq, max);

	/* the floors: user space's, and for touch_ms after a touch the touch's */
	floor = READ_ONCE(tunables->floor);
	if (sx_touched(tunables, time))
		floor = max(floor, READ_ONCE(tunables->touch_floor));
	freq = max(freq, floor);
	/*
	 * and the ceiling, over the floors as the policy's maximum is over them: user space's, or
	 * where it has written none the one that follows the load (caps -- which goes on looking
	 * meanwhile, so that it is where the load says when user space lets go)
	 */
	ceiling = READ_ONCE(tunables->ceiling);
	if (READ_ONCE(tunables->caps)) {
		unsigned int caps = sx_caps_ceiling(sg_policy, (s64)(time - sg_policy->ui_until) < 0);

		if (!ceiling)
			ceiling = caps;
	} else {
		sg_policy->caps_on = false;
	}
	sg_policy->caps_now = ceiling;
	if (ceiling)
		freq = min(freq, ceiling);

	/* (the cache has no room for "saturated", which can change the step for the same request) */
	if (freq == sg_policy->cached_raw_freq && !sg_policy->need_freq_update && !saturated)
		return sg_policy->next_freq;

	sg_policy->cached_raw_freq = freq;
	above = sx_resolve(sg_policy, freq);
	/* (a ceiling between two steps: the step under it, where that is within the limits) */
	if (ceiling && above > ceiling && policy->freq_table) {
		below = sx_step(sg_policy, ceiling, true);
		if (below >= policy->min && below <= ceiling)
			return sx_resolve(sg_policy, below);
	}
	/* (a request at or beyond a step, or the policy's maximum: there is nothing to choose) */
	if (above <= freq || above == policy->min || !policy->freq_table)
		return above;

	below = sx_step(sg_policy, freq, true);
	/* (and never a step under a floor that falls between two) */
	if (below < policy->min || below > policy->max || below < floor || saturated ||
	    sx_pick_step(freq, below, above) == above)
		return above;
	/* the step below it is (resolved for the driver when it is switched to: sx_switch) */
	return sx_resolve(sg_policy, below);
}

/*
 * The run queue's clamps on @util, as 5.15's uclamp_rq_util_with(rq, util, NULL) applied them:
 * 6.18's effective_cpu_util() hands the clamps out instead and sugov_effective_cpu_perf() applies
 * them after the headroom; the busy share has them applied before it, as on 5.15.
 */
static unsigned long sx_uclamp_util(struct rq *rq, unsigned long util)
{
	unsigned long lo, hi;

	if (!uclamp_is_used())
		return util;
	lo = uclamp_rq_get(rq, UCLAMP_MIN);
	hi = uclamp_rq_get(rq, UCLAMP_MAX);
	/* (each is the highest of the runnable tasks': they can cross) */
	if (unlikely(lo >= hi))
		return lo;
	return clamp(util, lo, hi);
}

/*
 * The run queue's clamps on a busy share. They are the highest of its runnable tasks', and a
 * real-time task has no ceiling: the moment one becomes runnable behind a task of the fair
 * class -- and that is a moment the governor is asked at -- the ceiling a group was given is
 * gone. The scheduler's utilisation shrugs that off: what a held-down task has used is little.
 * A share of time is all of the CPU, and the policy would go to its top frequency for a tick
 * every time a real-time kernel thread wakes there (RCU's does every few milliseconds). So
 * while a real-time task waits behind a fair one, the share -- which is the fair tasks' doing
 * -- keeps the ceiling the run queue had before. Once the real-time task runs it is its own.
 * (The task that runs, for this, is the one whose scheduling context runs: 6.18's rq->donor.)
 */
static unsigned long sx_clamp_share(struct sx_cpu *sg_cpu, struct rq *rq, unsigned long share)
{
	if (uclamp_is_used()) {
		bool rt_waits = false;

		if (rq->rt.rt_nr_running) {
			const struct task_struct *donor = rcu_dereference_sched(rq->donor);

			rt_waits = donor->sched_class == &fair_sched_class;
		} else {
			sg_cpu->fair_max = uclamp_rq_get(rq, UCLAMP_MAX);
		}
		share = sx_uclamp_util(rq, share);
		if (rt_waits)
			share = min(share, max_t(unsigned long, sg_cpu->fair_max, uclamp_rq_get(rq, UCLAMP_MIN)));
	}
	return share;
}

/*
 * The bandwidth that deadline tasks are promised on @rq (capacity units), without what 6.18's
 * deadline servers reserve there: the fair class's server -- 5 % of the CPU by default, and the
 * sched_ext one's likewise -- is in the run queue's running bandwidth from its start (a fair task
 * enqueued) until a while after its stop, nearly all the time on a CPU that runs anything; that
 * reservation is no work to be done. 5.15 had no such servers. A server's bandwidth is in while it
 * is active and while it is non-contending after a stop (sub_running_bw() at its 0-lag time).
 */
static u64 sx_server_bw(const struct sched_dl_entity *dl_se)
{
	return dl_se->dl_server_active || dl_se->dl_non_contending ? dl_se->dl_bw : 0;
}

static unsigned long sx_dl_bw(struct rq *rq)
{
	u64 bw = READ_ONCE(rq->dl.running_bw);

	bw -= min(bw, sx_server_bw(&rq->fair_server));
#ifdef CONFIG_SCHED_CLASS_EXT
	bw -= min(bw, sx_server_bw(&rq->ext_server));
#endif
	return (bw * SCHED_CAPACITY_SCALE) >> BW_SHIFT;
}

/*
 * A CPU's busy time so far (ns): the time that passed less its idle time, I/O waits included --
 * of another CPU as well. The counts are read, not brought up to date (that is the CPU's own
 * business when it leaves idle); a look from elsewhere can catch them half done, and a reading
 * that goes backwards the record takes as its new base. The two counts take the idle stretch
 * under way by whether a task that waits for I/O is counted on the CPU at that moment, which
 * changes between the two reads when such a task is woken: the I/O wait first, so that the
 * stretch is then counted twice -- the busy time reads low once -- not in neither -- it would
 * read a whole idle stretch as busy (an idle CPU's count of such tasks can only go down).
 */
static u64 sx_cpu_busy_ns(unsigned int cpu)
{
	u64 iowait = get_cpu_iowait_time_us(cpu, NULL), idle, wall;

	if (iowait == -1ULL) {
		/* (without NO_HZ: the tick's counts, read only as well) */
		idle = get_cpu_idle_time(cpu, &wall, 0);
	} else {
		idle = get_cpu_idle_time_us(cpu, NULL) + iowait;
		wall = ktime_to_us(ktime_get());
	}
	return wall > idle ? (wall - idle) * NSEC_PER_USEC : 0;
}

/*
 * caps -- a ceiling that follows the load: a rule first run in user space on the 5.15 kernel, a
 * program that looked at the CPUs 25 times a second while anything ran with an eBPF program in
 * every CPU's way into and out of idle, and that did not even look at a burst from rest for up to
 * 350 ms. Here the CPUs' busy times -- the ones "refined" reads -- are looked at when the policy is
 * worked out, every SX_CAPS_LOOK at most, and the lifts follow the touch screen and the cpuset
 * top-app as they happen.
 *
 * The ceiling: the step at or above the smoothed demand of the cluster's busiest CPU x
 * (100 + caps_boost) % of the table's top step that is no boost frequency, never below caps_floor
 * (by default the step at or above half the top one) and never above caps_max -- nor above
 * caps_burst until the cluster's busiest CPU has been busy for caps_burst_ms (sugov_ext_math.h, 9:
 * a burst lifted the smoothed demand of a whole cluster). For caps_front_ms after a process was
 * moved into the cpuset top-app it is at least the step at or above caps_front_pct % of the top
 * frequency; for caps_touch_ms after a finger was put down on the screen at least the step at or
 * above caps_touch. Neither caps_max nor caps_burst holds a lift: an app that starts is not held to
 * what a long load is. A burst of a thread that draws (caps_ui_ms, sx_ui_look) lifts it as an app
 * come to the front does, for as long as the burst lasts. "ceiling", user space's own, takes the
 * place of all of it while it is not 0 (a game's frame pacing holds the ceilings then).
 */
/*
 * (the program's own pace while anything ran: a look every 4 ms took the window's one early sample
 * at the first 4 ms of whatever ran, and a burst that came later in the window had none left)
 */
#define SX_CAPS_LOOK	(40 * NSEC_PER_MSEC)
/* (the CPUs of a policy taken into account: all of them on the 13 Pro) */
#define SX_CAPS_CPUS	8

/* the step at or above @khz within the policy's limits, as cpufreq resolves a request */
static unsigned int sx_step_up(struct sx_policy *sg_policy, unsigned int khz)
{
	return sx_step(sg_policy, khz, false);
}

/* the table's highest step that is no boost frequency */
static unsigned int sx_caps_top_step(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *pos;
	unsigned int top = 0;

	if (policy->freq_table) {
		cpufreq_for_each_valid_entry(pos, policy->freq_table) {
			if (!(pos->flags & CPUFREQ_BOOST_FREQ) && pos->frequency > top)
				top = pos->frequency;
		}
	}
	return top ?: policy->cpuinfo.max_freq;
}

/* is @stamp (a ktime) less than @ms before @now? (a stamp taken after @now, on another CPU, is) */
static bool sx_within(u64 stamp, u64 now, unsigned int ms)
{
	return ms && stamp && (s64)(now - stamp) < (s64)ms * NSEC_PER_MSEC;
}

/*
 * the ceiling of the moment (kHz), 0 for none -- @ui: lifted for a burst of a thread that draws;
 * under the policy's lock, or its CPU's run queue's
 */
static unsigned int sx_caps_ceiling(struct sx_policy *sg_policy, bool ui)
{
	const struct sx_tunables *tunables = sg_policy->tunables;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int fmax = sg_policy->caps_fmax, top = policy->cpuinfo.max_freq, want, limit;
	u64 now = ktime_get_ns();
	unsigned int j;

	/* (turned on: from nothing) */
	if (!sg_policy->caps_on) {
		sg_policy->caps_on = true;
		sg_policy->caps_smooth = SX_CAPS_NONE;
		sg_policy->caps_look = 0;
		sg_policy->caps_burst_since = 0;
		for_each_cpu(j, policy->cpus)
			per_cpu(sx_cpu, j).caps.start = 0;
	}
	if (now - sg_policy->caps_look >= SX_CAPS_LOOK) {
		struct sx_caps_cpu *cpus[SX_CAPS_CPUS];
		int n = 0, i = 0;
		u64 most = 0;

		for_each_cpu(j, policy->cpus) {
			if (n == SX_CAPS_CPUS)
				break;
			cpus[n++] = &per_cpu(sx_cpu, j).caps;
		}
		for_each_cpu(j, policy->cpus) {
			u64 busy;

			if (i == n)
				break;
			busy = sx_cpu_busy_ns(j);
			/* (what it ran since the last look, for caps_burst) */
			if (cpus[i]->start && busy > cpus[i]->last)
				most = max(most, busy - cpus[i]->last);
			sx_caps_look(cpus[i++], cpus, n, now, busy, &sg_policy->caps_smooth);
		}
		sg_policy->caps_burst_since = sx_burst_since(sg_policy->caps_burst_since, sg_policy->caps_look, now, most);
		sg_policy->caps_look = now;
	}

	/* (no sample yet: no ceiling; a demand of nothing: the floor) */
	if (sg_policy->caps_smooth == SX_CAPS_NONE)
		want = top;
	else
		want = sx_step_up(sg_policy, min_t(u64, sx_caps_raw(sg_policy->caps_smooth, READ_ONCE(tunables->caps_boost), fmax), fmax));
	limit = READ_ONCE(tunables->caps_floor) ?: fmax / 2;
	want = max(want, sx_step_up(sg_policy, min(limit, fmax)));
	limit = READ_ONCE(tunables->caps_max);
	if (limit && want > limit && policy->freq_table)
		want = sx_step(sg_policy, limit, true);
	limit = READ_ONCE(tunables->caps_burst);
	if (limit && want > limit && policy->freq_table &&
	    sx_burst_held(sg_policy->caps_burst_since, now, READ_ONCE(tunables->caps_burst_ms)))
		want = sx_step(sg_policy, limit, true);

	if (ui || sx_within(READ_ONCE(sx_front_stamp), now, READ_ONCE(tunables->caps_front_ms))) {
		unsigned int pct = READ_ONCE(tunables->caps_front_pct);

		want = max(want, pct >= 100 ? top : sx_step_up(sg_policy, (unsigned int)div_u64((u64)top * pct, 100)));
	}
	if (sx_within(READ_ONCE(sx_touch_down_stamp), now, READ_ONCE(tunables->caps_touch_ms)))
		want = max(want, sx_step_up(sg_policy, READ_ONCE(tunables->caps_touch)));
	return want >= top ? 0 : want;
}

/*
 * How busy the CPU was, in place of the scheduler's utilisation; true where it stands in, with
 * sg_cpu->util set (capacity units, without schedutil's headroom).
 *
 * Without "refined", two measures of the same thing, both shares of time. The run time of what
 * the CPU ran, in windows of ravg_window_us: taken when this runs on the CPU itself, read with its
 * age from elsewhere. And slim WALT's, sampled at the tick, which wins when it has a value. Either
 * replaces the utilisation only when it is not zero, and only on a CPU with something of the fair
 * class to run. The share is used as it is, cut at the CPU's capacity: 30 % busy is "300 of 1024"
 * on a big core at any frequency, and all of a little core's 300-odd. That loses the frequency the
 * CPU ran at (a CPU 60 % busy at its lowest frequency needs a fraction of what the same share at
 * its top one does). The tick's sample reads high whenever ticks are far apart: a stretch between
 * two of them is booked whole by what the CPU does at its end. And the run time is that of the task
 * that runs at two samples in a row: what ran in between is not seen.
 *
 * With "refined" the share is the CPU's own busy time: the time that passed less what the kernel
 * booked as idle, taken whenever the policy is worked out -- for every CPU of it, under the
 * policy's lock (a shared one) or the run queue's (a CPU of its own); read from elsewhere, a busy
 * CPU whose own looks the rate limits turn away still reads as busy. It is booked at the frequency
 * the CPU ran at (sx_busyrec): the work done, the top frequency's worth, which is what the mapping
 * expects -- this CPU's capacity, at the top. A CPU busy all the last react_us is "saturated": it
 * needs more than it did, however much that is. Nothing done replaces the utilisation as well.
 *
 * Either way the clamps of the run queue are applied to it, which the scheduler's estimate has
 * applied as well: a floor that Android writes for the app in front (cpu.uclamp.min) or a ceiling
 * for what is behind it (cpu.uclamp.max) holds under this governor too.
 */
static bool sx_busy_share(struct sx_cpu *sg_cpu, struct rq *rq, u64 time)
{
	const struct sx_tunables *tunables = sg_cpu->sg_policy->tunables;
	u64 win = (u64)READ_ONCE(tunables->ravg_window_us) * NSEC_PER_USEC;
	bool refined = READ_ONCE(tunables->refined), saturated = false;
	unsigned long share, slim, clamped;

	/*
	 * (The run-time measures count the fair class's run time: they stand in only where it has
	 * something to run -- runnable, not a task 6.18 keeps queued while its dequeue is delayed.
	 * "refined" counts all of a CPU's busy time, and stands in always: on a CPU with nothing to
	 * run the scheduler's utilisation is what it has not yet taken off -- a task that ran a few
	 * milliseconds and exited keeps its half of a CPU there until the CPU is next looked at by the
	 * scheduler, which an idle one is not.)
	 */
	if (!READ_ONCE(tunables->enabled) || (!refined && !READ_ONCE(rq->cfs.h_nr_runnable)))
		return false;

	if (refined) {
		u64 react = (u64)READ_ONCE(tunables->react_us) * NSEC_PER_USEC;
		struct sx_busyval v;

		sx_busyrec_sample(&sg_cpu->busyrec, time, win, react, sx_cpu_busy_ns(sg_cpu->cpu),
				  arch_scale_freq_capacity(sg_cpu->cpu));
		v = sx_busyrec_read(&sg_cpu->busyrec, time, win, react);
		share = v.work;
		saturated = v.busy >= SX_SATURATED;
	} else {
		/* on the CPU itself nothing else changes what it runs or what that has run */
		if (sg_cpu->cpu == smp_processor_id()) {
			struct task_struct *curr = rcu_dereference_sched(rq->curr);

			sx_runwin_sample(&sg_cpu->runwin, time, win, curr->pid, curr->se.sum_exec_runtime);
		}
		share = sx_runwin_read(&sg_cpu->runwin, time, win);

		if (READ_ONCE(tunables->use_slim_walt)) {
			slim = slim_walt_load(sg_cpu->cpu, time);
			if (slim)
				share = slim;
		}
	}
	/*
	 * Nothing in the record: the CPU has been idle and has perhaps just been given something.
	 * With "refined" that is what it is asked for -- not the scheduler's estimate, which gives
	 * a task just forked half of the CPU's spare capacity: every process a script started on
	 * an idle core took its cluster from the floor towards the top. What the new task does shows
	 * from react_us on. The clamps apply to it all the same.
	 */
	if (!share && !refined)
		return false;

	if (refined)
		share = (share * sg_cpu->max) >> SCHED_CAPACITY_SHIFT;
	clamped = sx_clamp_share(sg_cpu, rq, share);
	/* (one that a ceiling of its group holds down is not asking for more) */
	sg_cpu->saturated = saturated && clamped >= share;
	/* (a deadline task's bandwidth is a promise the busy time of the past does not know of) */
	sg_cpu->util = min(max(clamped, refined ? sg_cpu->bw_dl : 0), sg_cpu->max);
	return true;
}

/*
 * See sugov_get_util(): the performance the CPU asks for, schedutil's headroom in it, with @boost --
 * schedutil's boost after I/O waits, in capacity units -- taken into account. The busy share,
 * where it stands in, replaces the scheduler's utilisation: that is worked out only where it does
 * not (with the defaults, never). Where it does not, this is 6.18's sugov_get_util() as it is.
 */
static void sx_get_util(struct sx_cpu *sg_cpu, u64 time, unsigned long max_cap, unsigned long boost)
{
	unsigned int cpu = sg_cpu->cpu;
	struct rq *rq = cpu_rq(cpu);
	unsigned long min, max, util;

	sg_cpu->max = max_cap;
	sg_cpu->bw_dl = sx_dl_bw(rq);
	sg_cpu->saturated = false;

	if (sx_busy_share(sg_cpu, rq, time)) {
		/* (the I/O boost clamped as the share is) */
		if (boost)
			sg_cpu->util = max(sg_cpu->util, sx_uclamp_util(rq, boost));
		sg_cpu->bw_min = sg_cpu->bw_dl;
		sg_cpu->util = map_util_perf(sg_cpu->util);
		return;
	}

	util = scx_cpuperf_target(cpu);
	if (!scx_switched_all())
		util += cpu_util_cfs_boost(cpu);
	util = effective_cpu_util(cpu, util, &min, &max);
	util = max(util, boost);
	sg_cpu->bw_min = min;
	sg_cpu->util = sugov_effective_cpu_perf(cpu, util, min, max);
}

/* see sugov_iowait_reset() */
static bool sx_iowait_reset(struct sx_cpu *sg_cpu, u64 time, bool set_iowait_boost)
{
	s64 delta_ns = time - sg_cpu->last_update;

	/* Reset boost only if a tick has elapsed since last request */
	if (delta_ns <= TICK_NSEC)
		return false;

	sg_cpu->iowait_boost = set_iowait_boost ? IOWAIT_BOOST_MIN : 0;
	sg_cpu->iowait_boost_pending = set_iowait_boost;

	return true;
}

/* see sugov_iowait_boost(); nothing of it unless the tunable iowait_boost is 1 */
static void sx_iowait_boost(struct sx_cpu *sg_cpu, u64 time, unsigned int flags)
{
	bool set_iowait_boost = flags & SCHED_CPUFREQ_IOWAIT;

	if (!READ_ONCE(sg_cpu->sg_policy->tunables->iowait_boost)) {
		/* (and what was boosted when it was turned off goes at once; nothing is written otherwise) */
		if (unlikely(sg_cpu->iowait_boost || sg_cpu->iowait_boost_pending)) {
			sg_cpu->iowait_boost = 0;
			sg_cpu->iowait_boost_pending = false;
		}
		return;
	}

	/* Reset boost if the CPU appears to have been idle enough */
	if (sg_cpu->iowait_boost && sx_iowait_reset(sg_cpu, time, set_iowait_boost))
		return;

	/* Boost only tasks waking up after IO */
	if (!set_iowait_boost)
		return;

	/* Ensure boost doubles only one time at each request */
	if (sg_cpu->iowait_boost_pending)
		return;
	sg_cpu->iowait_boost_pending = true;

	/* Double the boost at each request */
	if (sg_cpu->iowait_boost) {
		sg_cpu->iowait_boost =
			min_t(unsigned int, sg_cpu->iowait_boost << 1, SCHED_CAPACITY_SCALE);
		return;
	}

	/* First wakeup after IO: start with minimum boost */
	sg_cpu->iowait_boost = IOWAIT_BOOST_MIN;
}

/* see sugov_iowait_apply(): the boost in capacity units, 0 for none */
static unsigned long sx_iowait_apply(struct sx_cpu *sg_cpu, u64 time, unsigned long max_cap)
{
	/* No boost currently required */
	if (!sg_cpu->iowait_boost)
		return 0;

	/* Reset boost if the CPU appears to have been idle enough */
	if (sx_iowait_reset(sg_cpu, time, false))
		return 0;

	if (!sg_cpu->iowait_boost_pending) {
		/* No boost pending; reduce the boost value. */
		sg_cpu->iowait_boost >>= 1;
		if (sg_cpu->iowait_boost < IOWAIT_BOOST_MIN) {
			sg_cpu->iowait_boost = 0;
			return 0;
		}
	}

	sg_cpu->iowait_boost_pending = false;

	/* in capacity units, as the utilisation it is compared with */
	return (sg_cpu->iowait_boost * max_cap) >> SCHED_CAPACITY_SHIFT;
}

#ifdef CONFIG_NO_HZ_COMMON
static bool sx_cpu_is_busy(struct sx_cpu *sg_cpu)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls_cpu(sg_cpu->cpu);
	bool ret = idle_calls == sg_cpu->saved_idle_calls;

	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool sx_cpu_is_busy(struct sx_cpu *sg_cpu) { return false; }
#endif /* CONFIG_NO_HZ_COMMON */

/*
 * A single CPU's frequency is not reduced while the CPU has not been idle since the last
 * look: schedutil's rule (sugov_hold_freq()), with its exceptions -- not when a clamp keeps the
 * CPU from what it asks for, not when sched_ext has all the fair tasks, and not when the limits
 * have just changed.
 */
static bool sx_hold_while_busy(struct sx_cpu *sg_cpu)
{
	/*
	 * (Not with "refined": its busy time has the idle time a CPU has between two looks in it,
	 * and "busy" here is no idle that stopped the tick -- under an idle governor that keeps the
	 * tick for short idle times, a CPU woken every few milliseconds would never come down.)
	 */
	if (READ_ONCE(sg_cpu->sg_policy->tunables->refined) || scx_switched_all())
		return false;
	return !uclamp_rq_is_capped(cpu_rq(sg_cpu->cpu)) && sx_cpu_is_busy(sg_cpu);
}

/*
 * "refined": for react_us after the governor starts -- at boot, after a change of governor, and
 * after every suspend, which stops and starts it -- its records are empty, and what the CPUs are
 * doing does not show yet. The frequency is not lowered until then.
 */
static unsigned int sx_warm(struct sx_policy *sg_policy, u64 time, unsigned int next_f)
{
	const struct sx_tunables *tunables = sg_policy->tunables;
	unsigned int cur = sg_policy->policy->cur;

	if (!READ_ONCE(tunables->refined))
		return next_f;
	if (!sg_policy->warm_until)
		sg_policy->warm_until = (time + (u64)READ_ONCE(tunables->react_us) * NSEC_PER_USEC) ?: 1;
	if ((s64)(time - sg_policy->warm_until) >= 0 || next_f >= cur)
		return next_f;
	/* (and nothing of it kept for the requests after: the cache would answer them with it) */
	sg_policy->cached_raw_freq = SX_NO_RAW_FREQ;
	return cur;
}

/*
 * Is the frequency a busy CPU would keep above the governor's own ceiling? It is not kept then:
 * the ceiling is over that rule as the policy's maximum is (which cpufreq itself enforces).
 */
static bool sx_above_ceiling(struct sx_policy *sg_policy)
{
	unsigned int ceiling = sg_policy->caps_now;

	return ceiling && sg_policy->next_freq > ceiling;
}

/*
 * Make sx_should_update_freq() ignore the rate limit when DL has increased the utilization
 * (6.18's ignore_dl_rate_limit(); the deadline servers' reservation is not that, sx_dl_bw()).
 */
static inline void sx_ignore_dl_rate_limit(struct sx_cpu *sg_cpu)
{
	if (sx_dl_bw(cpu_rq(sg_cpu->cpu)) > sg_cpu->bw_dl)
		sg_cpu->sg_policy->need_freq_update = true;
}

/*
 * Updates that can change nothing, turned away before anything is read or locked:
 *  - one from a CPU outside the policy. Qualcomm's cpufreq driver lets any CPU set a cluster's
 *    frequency, so schedutil works a cluster out from wherever a task is put on one of its run
 *    queues: on the 13 Pro the UFS interrupt's prime core did, for every writer of a random-write
 *    test it woke on a busy middle core -- 130 000 times a second, ~4 300 instructions each and the
 *    middle cluster's lock and cache lines. The CPU the task was put on works the cluster out at its
 *    own next look, its next switch of task or tick -- as on platforms where a CPU cannot set
 *    another's frequency at all.
 *  - (@pinned) a policy held at one frequency (its minimum is its maximum: a benchmark mode can
 *    hold every cluster so) and at that frequency already: the rate limits count from a change,
 *    and with none every update worked it all out for the same answer.
 * Not while schedutil's I/O boost is on: it counts wake-ups after I/O at every update, a remote
 * one too.
 */
static __always_inline bool sx_skip(const struct sx_policy *sg_policy, bool pinned)
{
	const struct sx_tunables *tunables = sg_policy->tunables;
	const struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int max;

	if (READ_ONCE(tunables->iowait_boost))
		return false;
	if (!cpumask_test_cpu(raw_smp_processor_id(), policy->cpus))
		return true;
	if (!pinned)
		return false;
	max = READ_ONCE(policy->max);
	return READ_ONCE(policy->min) == max && READ_ONCE(sg_policy->next_freq) == max &&
	       !READ_ONCE(sg_policy->limits_changed);
}

/*
 * caps_ui_ms: the cpuset top-app, as the cgroup core names it to sx_front_probe() -- compared, never
 * followed (Android makes it when it boots and keeps it). NULL: no task has been moved there since
 * the probe was put in.
 */
static struct cgroup *sx_top_cgrp;

/* "RenderThread" as it lies in a task's name, compared as a word and a half */
#define SX_RENDER_LO	0x68547265646e6552ULL	/* "RenderTh" */
#define SX_RENDER_HI	0x64616572U		/* "read" */

/*
 * Is @p a thread that draws the app in front: its main thread or HWUI's RenderThread, in @top or in a
 * cpuset made inside it (MIUI gives top-app's threads cpusets of their own: main, render, other,
 * trashy)? With preemption off, as the scheduler has it.
 */
static __always_inline bool sx_draws(struct task_struct *p, struct cgroup *top)
{
	struct cgroup *cgrp;

	if (!thread_group_leader(p) &&
	    (get_unaligned_le64(p->comm) != SX_RENDER_LO || get_unaligned_le32(p->comm + 8) != SX_RENDER_HI))
		return false;
	cgrp = task_css(p, cpuset_cgrp_id)->cgroup;
	return cgrp == top || cgroup_parent(cgrp) == top;
}

/*
 * caps_ui_ms: a look of the CPU's own at the task it runs -- with the run queue's lock, the one place
 * the task's run time is known to the last moment: what it ran since it was last accounted for is
 * added (one of another CPU is up to a tick behind). A thread that draws is in a burst while it has
 * run three in four of the time since the look that began it -- through its short sleeps, a lock
 * waited for, a call; another task on the CPU or a sleep of its own long enough make a burst begin
 * anew at its next look. One that has had caps_ui_ms of CPU time in its burst lifts the policy's
 * ceiling until SX_UI_HOLD after the look: a busy CPU looks every tick (4 ms on the 13 Pro) at the
 * latest. (Under the policy's lock, or the run queue's.)
 */
#define SX_UI_HOLD	(8 * NSEC_PER_MSEC)

static noinline void sx_ui_burst(struct sx_policy *sg_policy, struct sx_cpu *sg_cpu, u64 time, unsigned int ms,
				 struct cgroup *top)
{
	struct rq *rq = cpu_rq(sg_cpu->cpu);
	struct task_struct *p = rcu_dereference_sched(rq->curr);
	u64 sum, start, clock, ran;

	if (!sx_draws(p, top))
		return;
	sum = READ_ONCE(p->se.sum_exec_runtime);
	start = READ_ONCE(p->se.exec_start);
	clock = READ_ONCE(rq->clock_task);
	/*
	 * (6.18 books run time to the task that runs, curr, and moves its exec_start with it; one that
	 * runs for another's scheduling context -- proxy execution -- may have an older exec_start:
	 * its sum alone then)
	 */
	if (clock > start && p == rcu_dereference_sched(rq->donor))
		sum += clock - start;
	ran = sum - sg_cpu->ui_sum;
	/* (another thread, or this one after a while away or one that ran less: a burst from now on) */
	if (p != sg_cpu->ui_task || (s64)ran < 0 || clock < sg_cpu->ui_since || ran * 4 < (clock - sg_cpu->ui_since) * 3) {
		sg_cpu->ui_task = p;
		sg_cpu->ui_sum = sum;
		sg_cpu->ui_since = clock;
		return;
	}
	if (ran < (u64)ms * NSEC_PER_MSEC)
		return;
	if ((s64)(time - sg_policy->ui_until) >= 0)
		sg_policy->ui_lifts++;
	sg_policy->ui_until = time + SX_UI_HOLD;
}

static __always_inline void sx_ui_look(struct sx_policy *sg_policy, struct sx_cpu *sg_cpu, u64 time)
{
	unsigned int ms = READ_ONCE(sg_policy->tunables->caps_ui_ms);
	struct cgroup *top;

	if (!ms || !READ_ONCE(sg_policy->tunables->caps))
		return;
	top = READ_ONCE(sx_top_cgrp);
	if (top)
		sx_ui_burst(sg_policy, sg_cpu, time, ms, top);
}

/* see sugov_update_single_common() */
static inline bool sx_update_single_common(struct sx_cpu *sg_cpu, u64 time, unsigned long max_cap,
					   unsigned int flags)
{
	unsigned long boost;

	sx_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	sx_ignore_dl_rate_limit(sg_cpu);

	if (!sx_should_update_freq(sg_cpu->sg_policy, time))
		return false;

	boost = sx_iowait_apply(sg_cpu, time, max_cap);
	sx_get_util(sg_cpu, time, max_cap, boost);

	return true;
}

static noinline void sx_single_freq_work(struct sx_cpu *sg_cpu, u64 time, unsigned int flags)
{
	struct sx_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int cached_freq = sg_policy->cached_raw_freq;
	unsigned long max_cap = arch_scale_cpu_capacity(sg_cpu->cpu);
	unsigned int next_f;

	if (!sx_update_single_common(sg_cpu, time, max_cap, flags))
		return;

	sx_ui_look(sg_policy, sg_cpu, time);
	next_f = sx_warm(sg_policy, time, sx_get_next_freq(sg_policy, sg_cpu->util, sg_cpu->max, time, sg_cpu->saturated));

	if (sx_hold_while_busy(sg_cpu) && next_f < sg_policy->next_freq &&
	    !sg_policy->need_freq_update && !sx_above_ceiling(sg_policy)) {
		next_f = sg_policy->next_freq;

		/* Restore cached freq as next_freq has changed */
		sg_policy->cached_raw_freq = cached_freq;
	}

	if (!sx_update_next_freq(sg_policy, time, next_f))
		return;

	/* see sugov_update_single_freq() */
	if (sg_policy->policy->fast_switch_enabled) {
		sx_switch(sg_policy, next_f);
	} else {
		raw_spin_lock(&sg_policy->update_lock);
		sx_deferred_update(sg_policy);
		raw_spin_unlock(&sg_policy->update_lock);
	}
}

/*
 * (The hooks: an update sx_skip() turns away costs a few loads and no stack frame -- the work is a
 * function of its own, which saves and restores the registers the whole look needs.)
 */
static void sx_update_single_freq(struct update_util_data *hook, u64 time,
				  unsigned int flags)
{
	struct sx_cpu *sg_cpu = container_of(hook, struct sx_cpu, update_util);

	if (!sx_skip(sg_cpu->sg_policy, true))
		sx_single_freq_work(sg_cpu, time, flags);
}

/* see sugov_update_single_perf() */
static void sx_update_single_perf(struct update_util_data *hook, u64 time,
				  unsigned int flags)
{
	struct sx_cpu *sg_cpu = container_of(hook, struct sx_cpu, update_util);
	unsigned long prev_util = sg_cpu->util;
	unsigned long max_cap;

	if (!arch_scale_freq_invariant()) {
		sx_update_single_freq(hook, time, flags);
		return;
	}

	max_cap = arch_scale_cpu_capacity(sg_cpu->cpu);

	if (sx_skip(sg_cpu->sg_policy, false) || !sx_update_single_common(sg_cpu, time, max_cap, flags))
		return;

	if (sx_hold_while_busy(sg_cpu) && sg_cpu->util < prev_util)
		sg_cpu->util = prev_util;

	cpufreq_driver_adjust_perf(sg_cpu->cpu, sg_cpu->bw_min, sg_cpu->util, max_cap);

	sg_cpu->sg_policy->last_freq_update_time = time;
}

/* one CPU of a shared policy into the policy's request: the highest utilisation for its capacity */
static inline void sx_take_cpu(struct sx_cpu *j_sg_cpu, u64 time, unsigned long *util, unsigned long *max,
			       bool *saturated)
{
	unsigned long j_util, j_max, cap = arch_scale_cpu_capacity(j_sg_cpu->cpu);

	sx_get_util(j_sg_cpu, time, cap, sx_iowait_apply(j_sg_cpu, time, cap));
	j_util = j_sg_cpu->util;
	j_max = j_sg_cpu->max;
	*saturated |= j_sg_cpu->saturated;

	if (j_util * *max > j_max * *util) {
		*util = j_util;
		*max = j_max;
	}
}

static unsigned int sx_next_freq_shared(struct sx_cpu *sg_cpu, u64 time)
{
	struct sx_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util = 0, max = 1;
	bool saturated = false;
	unsigned int j;

	if (sg_policy->nr_cpus) {
		for (j = 0; j < sg_policy->nr_cpus; j++)
			sx_take_cpu(&per_cpu(sx_cpu, sg_policy->cpus[j]), time, &util, &max, &saturated);
	} else {
		for_each_cpu(j, sg_policy->policy->cpus)
			sx_take_cpu(&per_cpu(sx_cpu, j), time, &util, &max, &saturated);
	}

	return sx_get_next_freq(sg_policy, util, max, time, saturated);
}

static noinline void sx_shared_work(struct sx_cpu *sg_cpu, u64 time, unsigned int flags)
{
	struct sx_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int next_f;

	raw_spin_lock(&sg_policy->update_lock);

	sx_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	sx_ignore_dl_rate_limit(sg_cpu);

	if (sx_should_update_freq(sg_policy, time)) {
		sx_ui_look(sg_policy, sg_cpu, time);
		next_f = sx_warm(sg_policy, time, sx_next_freq_shared(sg_cpu, time));

		if (!sx_update_next_freq(sg_policy, time, next_f))
			goto unlock;

		if (sg_policy->policy->fast_switch_enabled)
			sx_switch(sg_policy, next_f);
		else
			sx_deferred_update(sg_policy);
	}
unlock:
	raw_spin_unlock(&sg_policy->update_lock);
}

static void
sx_update_shared(struct update_util_data *hook, u64 time, unsigned int flags)
{
	struct sx_cpu *sg_cpu = container_of(hook, struct sx_cpu, update_util);

	if (!sx_skip(sg_cpu->sg_policy, true))
		sx_shared_work(sg_cpu, time, flags);
}

static void sx_work(struct kthread_work *work)
{
	struct sx_policy *sg_policy = container_of(work, struct sx_policy, work);
	unsigned int freq;
	unsigned long flags;

	/* see sugov_work() */
	raw_spin_lock_irqsave(&sg_policy->update_lock, flags);
	freq = sg_policy->next_freq;
	sg_policy->work_in_progress = false;
	raw_spin_unlock_irqrestore(&sg_policy->update_lock, flags);

	mutex_lock(&sg_policy->work_lock);
	__cpufreq_driver_target(sg_policy->policy, freq, CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);
}

static void sx_irq_work(struct irq_work *irq_work)
{
	struct sx_policy *sg_policy;

	sg_policy = container_of(irq_work, struct sx_policy, irq_work);

	kthread_queue_work(&sg_policy->worker, &sg_policy->work);
}

/************************** sysfs interface ************************/

static struct sx_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);
/* who holds a reference on slim WALT: started policies that use it */
static DEFINE_MUTEX(sx_slim_lock);

static inline struct sx_tunables *to_sx_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sx_tunables, attr_set);
}

/* a tunable that is one number: bounds, and what a new value needs done for each policy */
#define SX_NUMBER(_name, _min, _max, _apply)						\
static ssize_t _name##_show(struct gov_attr_set *attr_set, char *buf)			\
{											\
	return sysfs_emit(buf, "%u\n", (unsigned int)to_sx_tunables(attr_set)->_name);	\
}											\
static ssize_t _name##_store(struct gov_attr_set *attr_set, const char *buf, size_t count)	\
{											\
	struct sx_tunables *tunables = to_sx_tunables(attr_set);			\
	struct sx_policy *sg_policy;							\
	unsigned int value;								\
											\
	if (kstrtouint(buf, 10, &value))						\
		return -EINVAL;								\
	if (value < (_min) || value > (_max))						\
		return -ERANGE;								\
	WRITE_ONCE(tunables->_name, value);						\
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook)		\
		_apply(sg_policy);							\
	return count;									\
}											\
static struct governor_attr _name = __ATTR_RW(_name)

static void sx_nothing(struct sx_policy *sg_policy)
{
}

/* a started policy holds slim WALT while it is read (use_slim_walt, and not refined) */
static void sx_slim_follow(struct sx_policy *sg_policy)
{
	const struct sx_tunables *tunables = sg_policy->tunables;
	bool want;

	mutex_lock(&sx_slim_lock);
	want = sg_policy->started && tunables->use_slim_walt && !tunables->refined;
	if (want != sg_policy->slim) {
		if (want)
			slim_walt_get();
		else
			slim_walt_put();
		sg_policy->slim = want;
	}
	mutex_unlock(&sx_slim_lock);
}

/*
 * touch_ms: the screen's events. Whatever reports the position of fingers is listened to
 * (keys, sensors and the fingerprint reader report none), from the first time a policy asks
 * for it on; before that nothing of this is in the input path. A passive observer: the handle
 * does not power a device up by itself (6.18's passive_observer); it hears what the device's own
 * users -- Android's input reader -- have it report.
 */
static unsigned int sx_touch_events(struct input_handle *handle, struct input_value *vals, unsigned int count)
{
	unsigned int i;

	WRITE_ONCE(sx_touch_stamp, sched_clock() ?: 1);
	/* (caps: a finger put down -- a new contact, or the first one -- not one that moves) */
	for (i = 0; i < count; i++)
		if ((vals[i].type == EV_ABS && vals[i].code == ABS_MT_TRACKING_ID && (int)vals[i].value >= 0) ||
		    (vals[i].type == EV_KEY && vals[i].code == BTN_TOUCH && vals[i].value)) {
			WRITE_ONCE(sx_touch_down_stamp, ktime_get_ns() ?: 1);
			break;
		}
	/* (all of them on to the next handlers: nothing is filtered) */
	return count;
}

static int sx_touch_connect(struct input_handler *handler, struct input_dev *dev,
			    const struct input_device_id *id)
{
	struct input_handle *handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	int ret;

	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "sugov_ext";

	ret = input_register_handle(handle);
	if (ret)
		goto free;
	ret = input_open_device(handle);
	if (!ret)
		return 0;

	input_unregister_handle(handle);
free:
	kfree(handle);
	return ret;
}

static void sx_touch_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id sx_touch_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] = BIT_MASK(ABS_MT_POSITION_X) | BIT_MASK(ABS_MT_POSITION_Y) },
	},
	{ },
};

static struct input_handler sx_touch_handler = {
	.events			= sx_touch_events,
	.connect		= sx_touch_connect,
	.disconnect		= sx_touch_disconnect,
	.passive_observer	= true,
	.name			= "sugov_ext",
	.id_table		= sx_touch_ids,
};

/*
 * caps_front_ms: a process moved into the cpuset top-app -- what Android does with an app that is
 * started or switched to (libprocessgroup writes its pid to cgroup.procs: the whole thread group).
 * Not one thread moved on its own (the tasks file): that is not an app come to the front but a
 * service or the app itself moving a thread as it works, and it would take the ceiling off for
 * caps_front_ms as if an app had started. The cgroup core's own tracepoint, after the move has
 * been made, with the cgroup's path in its hierarchy; probed from the first time a policy asks for
 * it on, before that nothing of this is in the path. Any move there says which group top-app is
 * (caps_ui_ms).
 */
static void sx_front_probe(void *data, struct cgroup *dst_cgrp, const char *path, struct task_struct *task,
			   bool threadgroup)
{
	if (!(dst_cgrp->root->subsys_mask & (1U << cpuset_cgrp_id)) || strcmp(path, "/top-app"))
		return;
	if (READ_ONCE(sx_top_cgrp) != dst_cgrp)
		WRITE_ONCE(sx_top_cgrp, dst_cgrp);
	if (threadgroup)
		WRITE_ONCE(sx_front_stamp, ktime_get_ns() ?: 1);
}

/*
 * What the rules listen to -- the touch screen, the cgroup core's tracepoint -- is put in by a work
 * item, never where it is asked for: a governor is started from CPU hotplug as well, with the hotplug
 * lock held for writing, and the first probe of a tracepoint switches its static key on, which takes
 * that lock for reading. (The input handler would not need that; it goes the same way, one place for
 * both.) Once in, each stays: listening costs every touch event a stamp, every move between cgroups
 * the path of the group it goes to. A registration that fails is tried again at the next request.
 */
static bool sx_touch_wanted, sx_front_wanted;
static bool sx_touch_hooked, sx_front_hooked;

static void sx_hooks(struct work_struct *work)
{
	if (READ_ONCE(sx_touch_wanted) && !READ_ONCE(sx_touch_hooked))
		WRITE_ONCE(sx_touch_hooked, !input_register_handler(&sx_touch_handler));
	if (READ_ONCE(sx_front_wanted) && !READ_ONCE(sx_front_hooked))
		WRITE_ONCE(sx_front_hooked, !register_trace_cgroup_attach_task(sx_front_probe, NULL));
}
static DECLARE_WORK(sx_hooks_work, sx_hooks);

static void sx_touch_follow(struct sx_policy *sg_policy)
{
	const struct sx_tunables *tunables = sg_policy->tunables;

	if (!tunables->touch_ms && !(tunables->caps && tunables->caps_touch_ms))
		return;
	if (READ_ONCE(sx_touch_hooked))
		return;
	WRITE_ONCE(sx_touch_wanted, true);
	schedule_work(&sx_hooks_work);
}

static void sx_front_follow(struct sx_policy *sg_policy)
{
	const struct sx_tunables *tunables = sg_policy->tunables;

	if (!tunables->caps || !(tunables->caps_front_ms || tunables->caps_ui_ms) || READ_ONCE(sx_front_hooked))
		return;
	WRITE_ONCE(sx_front_wanted, true);
	schedule_work(&sx_hooks_work);
}

/* what the ceiling that follows the load needs listened to: the screen, the cpuset top-app */
static void sx_caps_follow(struct sx_policy *sg_policy)
{
	sx_touch_follow(sg_policy);
	sx_front_follow(sg_policy);
}

SX_NUMBER(up_rate_limit_us, 0, UINT_MAX, sx_update_rate_limits);
SX_NUMBER(down_rate_limit_us, 0, UINT_MAX, sx_update_rate_limits);
/* more than a minute after a touch nobody is still waiting for what it started */
SX_NUMBER(touch_ms, 0, 60 * MSEC_PER_SEC, sx_touch_follow);
/* (a floor above the policy's maximum asks for that maximum) */
SX_NUMBER(floor, 0, UINT_MAX, sx_nothing);
SX_NUMBER(touch_floor, 0, UINT_MAX, sx_nothing);
SX_NUMBER(ceiling, 0, UINT_MAX, sx_nothing);
SX_NUMBER(iowait_boost, 0, 1, sx_nothing);
SX_NUMBER(enabled, 0, 1, sx_nothing);
SX_NUMBER(use_slim_walt, 0, 1, sx_slim_follow);
SX_NUMBER(refined, 0, 1, sx_slim_follow);
/* (beyond ravg_window_us it is the whole window) */
SX_NUMBER(react_us, 1000, 1000000, sx_nothing);
/* a window of nothing divides by zero; one of more than a second is no measure of "now" */
SX_NUMBER(ravg_window_us, 1000, 1000000, sx_nothing);
SX_NUMBER(target_load_thresh, 0, UINT_MAX, sx_nothing);
SX_NUMBER(target_load_shift, 0, 31, sx_nothing);
SX_NUMBER(caps, 0, 1, sx_caps_follow);
SX_NUMBER(caps_boost, 0, 100, sx_nothing);
SX_NUMBER(caps_floor, 0, UINT_MAX, sx_nothing);
SX_NUMBER(caps_max, 0, UINT_MAX, sx_nothing);
SX_NUMBER(caps_burst, 0, UINT_MAX, sx_nothing);
SX_NUMBER(caps_burst_ms, 0, 60 * MSEC_PER_SEC, sx_nothing);
SX_NUMBER(caps_front_ms, 0, 60 * MSEC_PER_SEC, sx_caps_follow);
SX_NUMBER(caps_front_pct, 0, 100, sx_nothing);
SX_NUMBER(caps_touch_ms, 0, 60 * MSEC_PER_SEC, sx_caps_follow);
SX_NUMBER(caps_touch, 0, UINT_MAX, sx_nothing);
/* (a burst of a second is long whatever it is) */
SX_NUMBER(caps_ui_ms, 0, MSEC_PER_SEC, sx_caps_follow);

/*
 * caps_state (read only): for each policy of these tunables, the smoothed demand of its busiest
 * CPU (of 1024; "none" before a first sample), the ceiling of its last request (kHz, 0: none),
 * the top step the rule goes by, how long its CPUs have been busy look after look by the last
 * look (ms; caps_burst) and how often a burst of a thread that draws lifted the ceiling
 * (caps_ui_ms) -- what a look from user space wants to see.
 */
static ssize_t caps_state_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sx_policy *sg_policy;
	ssize_t n = 0;

	/*
	 * (the list as a store sees it, under the set's lock and while it is in use: a policy that
	 * leaves the governor is taken off it there, and a read does not otherwise hold that off)
	 */
	mutex_lock(&attr_set->update_lock);
	if (attr_set->usage_count) {
		list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
			u32 smooth = READ_ONCE(sg_policy->caps_smooth);
			u64 since, look;

			if (smooth == SX_CAPS_NONE)
				n += sysfs_emit_at(buf, n, "policy%u smooth none", sg_policy->policy->cpu);
			else
				n += sysfs_emit_at(buf, n, "policy%u smooth %u", sg_policy->policy->cpu, smooth);
			n += sysfs_emit_at(buf, n, " ceiling %u top %u", READ_ONCE(sg_policy->caps_now), sg_policy->caps_fmax);
			since = READ_ONCE(sg_policy->caps_burst_since);
			look = READ_ONCE(sg_policy->caps_look);
			n += sysfs_emit_at(buf, n, " busy_ms %llu ui_lifts %lu\n", since && look > since ? div_u64(look - since, NSEC_PER_MSEC) : 0,
					   READ_ONCE(sg_policy->ui_lifts));
		}
	}
	mutex_unlock(&attr_set->update_lock);
	return n;
}
static struct governor_attr caps_state = __ATTR_RO(caps_state);

/*
 * boost: percent more of the utilisation, or less (0 is the governor as it comes). The one number
 * a mode is: what user space knows -- the user's choice, the app in front -- handed over in one
 * write, with nothing running to hand it.
 */
static ssize_t boost_show(struct gov_attr_set *attr_set, char *buf)
{
	return sysfs_emit(buf, "%d\n", to_sx_tunables(attr_set)->boost);
}

static ssize_t boost_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	int value;

	if (kstrtoint(buf, 10, &value))
		return -EINVAL;
	/* (half of what a CPU does, or twice: beyond that it is no margin) */
	if (value < -50 || value > 100)
		return -ERANGE;
	WRITE_ONCE(to_sx_tunables(attr_set)->boost, value);
	return count;
}

static struct governor_attr boost = __ATTR_RW(boost);

/* "kHz percent" pairs with a colon between them */
static ssize_t target_loads_show(struct gov_attr_set *attr_set, char *buf)
{
	const struct sx_loads *loads;
	ssize_t len = 0;
	unsigned int i;

	rcu_read_lock();
	loads = rcu_dereference(to_sx_tunables(attr_set)->loads);
	for (i = 0; loads && i < loads->nr; i++)
		len += sysfs_emit_at(buf, len, "%s%u %u", i ? ":" : "", loads->step[i].freq, loads->step[i].pct);
	rcu_read_unlock();
	/* none: the one that changes nothing */
	return len ? len + sysfs_emit_at(buf, len, "\n") : sysfs_emit(buf, "0 0\n");
}

static ssize_t target_loads_store(struct gov_attr_set *attr_set, const char *buf, size_t count)
{
	struct sx_tunables *tunables = to_sx_tunables(attr_set);
	struct sx_loads *loads, *old;
	struct sx_policy *sg_policy;
	const char *p = skip_spaces(buf);

	loads = kzalloc(sizeof(*loads), GFP_KERNEL);
	if (!loads)
		return -ENOMEM;
	while (*p) {
		struct sx_load_step *step = &loads->step[loads->nr];
		unsigned long long freq, pct;
		char *end;

		if (loads->nr == SX_MAX_STEPS)
			goto bad;
		freq = simple_strtoull(p, &end, 10);
		if (end == p || !isspace(*end))
			goto bad;
		p = skip_spaces(end);
		pct = simple_strtoull(p, &end, 10);
		if (end == p || freq > UINT_MAX)
			goto bad;
		/* lowest frequency first, each once: of the steps a CPU has reached the highest counts */
		if (loads->nr && freq <= step[-1].freq)
			goto bad;
		step->freq = freq;
		step->pct = min(pct, 1000ULL);
		loads->nr++;
		p = skip_spaces(end);
		if (*p == ':')
			p = skip_spaces(p + 1);
		else if (*p)
			goto bad;
	}
	if (!loads->nr)
		goto bad;

	old = rcu_dereference_protected(tunables->loads, lockdep_is_held(&attr_set->update_lock));
	rcu_assign_pointer(tunables->loads, loads);
	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook)
		WRITE_ONCE(sg_policy->cached_raw_freq, SX_NO_RAW_FREQ);
	if (old)
		kfree_rcu(old, rcu);
	return count;
bad:
	kfree(loads);
	return -EINVAL;
}

static struct governor_attr target_loads = __ATTR_RW(target_loads);

static struct attribute *sx_attrs[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	&boost.attr,
	&floor.attr,
	&touch_floor.attr,
	&ceiling.attr,
	&iowait_boost.attr,
	&touch_ms.attr,
	&enabled.attr,
	&use_slim_walt.attr,
	&ravg_window_us.attr,
	&target_loads.attr,
	&target_load_thresh.attr,
	&target_load_shift.attr,
	&refined.attr,
	&react_us.attr,
	&caps.attr,
	&caps_boost.attr,
	&caps_floor.attr,
	&caps_max.attr,
	&caps_burst.attr,
	&caps_burst_ms.attr,
	&caps_front_ms.attr,
	&caps_front_pct.attr,
	&caps_touch_ms.attr,
	&caps_touch.attr,
	&caps_ui_ms.attr,
	&caps_state.attr,
	NULL
};
ATTRIBUTE_GROUPS(sx);

/*
 * The tunables of policies that went offline with the governor, by the policy's first CPU:
 * what is left of the object when its last policy has gone and its files are no more. The
 * next tunables made for that policy take the values over.
 */
static struct sx_tunables *sx_kept[NR_CPUS];
static DEFINE_MUTEX(sx_kept_lock);

static void sx_tunables_drop(struct sx_tunables *tunables)
{
	if (!tunables)
		return;
	/* no policy has these any more, and no update hook of one is still running */
	kfree(rcu_dereference_protected(tunables->loads, 1));
	kfree(tunables);
}

static void sx_tunables_free(struct kobject *kobj)
{
	struct gov_attr_set *attr_set = to_gov_attr_set(kobj);
	struct sx_tunables *tunables = to_sx_tunables(attr_set);
	struct sx_tunables *old;

	mutex_lock(&sx_kept_lock);
	old = sx_kept[tunables->home];
	sx_kept[tunables->home] = tunables->keep ? tunables : NULL;
	mutex_unlock(&sx_kept_lock);

	sx_tunables_drop(old);
	if (!tunables->keep)
		sx_tunables_drop(tunables);
}

/* @tunables, new and not yet anybody's, get what was kept for their policy */
static void sx_tunables_restore(struct sx_tunables *tunables)
{
	struct sx_tunables *kept;

	mutex_lock(&sx_kept_lock);
	kept = sx_kept[tunables->home];
	sx_kept[tunables->home] = NULL;
	mutex_unlock(&sx_kept_lock);
	if (!kept)
		return;

	tunables->up_rate_limit_us = kept->up_rate_limit_us;
	tunables->down_rate_limit_us = kept->down_rate_limit_us;
	tunables->boost = kept->boost;
	tunables->floor = kept->floor;
	tunables->touch_floor = kept->touch_floor;
	tunables->ceiling = kept->ceiling;
	tunables->iowait_boost = kept->iowait_boost;
	tunables->touch_ms = kept->touch_ms;
	tunables->target_load_thresh = kept->target_load_thresh;
	tunables->target_load_shift = kept->target_load_shift;
	tunables->ravg_window_us = kept->ravg_window_us;
	tunables->enabled = kept->enabled;
	tunables->use_slim_walt = kept->use_slim_walt;
	tunables->refined = kept->refined;
	tunables->react_us = kept->react_us;
	tunables->caps = kept->caps;
	tunables->caps_boost = kept->caps_boost;
	tunables->caps_floor = kept->caps_floor;
	tunables->caps_max = kept->caps_max;
	tunables->caps_burst = kept->caps_burst;
	tunables->caps_burst_ms = kept->caps_burst_ms;
	tunables->caps_front_ms = kept->caps_front_ms;
	tunables->caps_front_pct = kept->caps_front_pct;
	tunables->caps_touch_ms = kept->caps_touch_ms;
	tunables->caps_touch = kept->caps_touch;
	tunables->caps_ui_ms = kept->caps_ui_ms;
	rcu_assign_pointer(tunables->loads, rcu_dereference_protected(kept->loads, 1));
	kfree(kept);
}

static const struct kobj_type sx_tunables_ktype = {
	.default_groups = sx_groups,
	.sysfs_ops = &governor_sysfs_ops,
	.release = &sx_tunables_free,
};

/********************** cpufreq governor interface *********************/

static struct sx_policy *sx_policy_alloc(struct cpufreq_policy *policy)
{
	struct sx_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

/* see sugov_kthread_create() */
static int sx_kthread_create(struct sx_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_attr attr = {
		.size		= sizeof(struct sched_attr),
		.sched_policy	= SCHED_DEADLINE,
		.sched_flags	= SCHED_FLAG_SUGOV,
		.sched_nice	= 0,
		.sched_priority	= 0,
		/*
		 * Fake (unused) bandwidth; workaround to "fix"
		 * priority inheritance.
		 */
		.sched_runtime	= NSEC_PER_MSEC,
		.sched_deadline = 10 * NSEC_PER_MSEC,
		.sched_period	= 10 * NSEC_PER_MSEC,
	};
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	/* kthread only required for slow path */
	if (policy->fast_switch_enabled)
		return 0;

	kthread_init_work(&sg_policy->work, sx_work);
	kthread_init_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"sugov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create sugov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setattr_nocheck(thread, &attr);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_DEADLINE\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;
	if (policy->dvfs_possible_from_any_cpu)
		set_cpus_allowed_ptr(thread, policy->related_cpus);
	else
		kthread_bind_mask(thread, policy->related_cpus);

	init_irq_work(&sg_policy->irq_work, sx_irq_work);
	mutex_init(&sg_policy->work_lock);

	wake_up_process(thread);

	return 0;
}

static void sx_kthread_stop(struct sx_policy *sg_policy)
{
	/* kthread only required for slow path */
	if (sg_policy->policy->fast_switch_enabled)
		return;

	kthread_flush_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
	mutex_destroy(&sg_policy->work_lock);
}

/*
 * The policy's place among the frequency domains, by their first CPU: 0 for the first (the little
 * cores of the 13 Pro), 1 for the second (the middle cores), 2 and more for the others (the prime
 * core). The domains of lower CPUs are up by now: cpufreq brings policies up in CPU order, and the
 * governor is chosen for them later still.
 */
static unsigned int sx_rank(struct cpufreq_policy *policy)
{
	unsigned int first = cpumask_first(policy->related_cpus), cpu, rank = 0;

	for_each_possible_cpu(cpu) {
		struct cpufreq_policy *p;

		if (cpu >= first)
			break;
		p = cpufreq_cpu_get(cpu);
		if (!p)
			continue;
		/* (each domain once: at its first CPU that is online) */
		if (p != policy && cpu == cpumask_first(p->cpus))
			rank++;
		cpufreq_cpu_put(p);
	}
	return rank;
}

/*
 * The built-in values: what the 5.15 kernel's user-space tuning module wrote once it had selected
 * this governor, so that the kernel alone runs as the phone did there. User space can write every
 * file as before.
 *  - everywhere: the rate limits 500 / 1000 us, enabled, use_slim_walt, a window of 20 ms, target
 *    loads none, threshold 1024, shift 4, react_us a tick of 250 Hz -- the governor's own -- and
 *    refined 1, boost -20 (the mode "balance"); for the load-following ceiling caps_boost 5,
 *    caps_front_ms 2000 at caps_front_pct 85, caps_burst_ms 1000, no lift for a touch, none for a
 *    thread that draws (caps_ui_ms 0: off unless turned on there too)
 *  - the first cluster (little): no load-following ceiling (its floor 1344000 written all the same),
 *    and a floor of 1344000 kHz for 1500 ms after a touch -- what the module had WALT's input boost
 *    do on that phone, there being no WALT here
 *  - the second (middle): the load-following ceiling, held at 1536000 kHz until a load has lasted
 *    caps_burst_ms (the module's "mid_burst")
 *  - the others (prime): the load-following ceiling
 */
static void sx_tunables_defaults(struct sx_tunables *tunables, struct cpufreq_policy *policy)
{
	tunables->up_rate_limit_us = 500;
	tunables->down_rate_limit_us = 1000;
	tunables->enabled = true;
	tunables->use_slim_walt = true;
	tunables->ravg_window_us = 20000;
	tunables->target_load_thresh = 1024;
	tunables->target_load_shift = 4;
	tunables->react_us = 4000;
	tunables->refined = true;
	tunables->boost = -20;
	tunables->caps_boost = 5;
	tunables->caps_front_ms = 2000;
	tunables->caps_front_pct = 85;
	tunables->caps_burst_ms = 1000;

	switch (sx_rank(policy)) {
	case 0:
		tunables->touch_floor = 1344000;
		tunables->touch_ms = 1500;
		tunables->caps_floor = 1344000;
		break;
	case 1:
		tunables->caps = true;
		tunables->caps_burst = 1536000;
		break;
	default:
		tunables->caps = true;
		break;
	}
}

static struct sx_tunables *sx_tunables_alloc(struct sx_policy *sg_policy)
{
	struct sx_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables)
		return NULL;

	gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
	sx_tunables_defaults(tunables, sg_policy->policy);

	if (have_governor_per_policy())
		tunables->home = cpumask_first(sg_policy->policy->related_cpus);
	else
		global_tunables = tunables;
	/* (or as they were when this policy last went offline with the governor) */
	sx_tunables_restore(tunables);
	return tunables;
}

static void sx_clear_global_tunables(void)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;
}

static int sx_init(struct cpufreq_policy *policy)
{
	struct sx_policy *sg_policy;
	struct sx_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = sx_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	ret = sx_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = sx_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto stop_kthread;
	}

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &sx_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   policy->governor->name);
	if (ret)
		goto fail;

out:
	/*
	 * EAS takes this governor as it takes schedutil (sugov_is_governor()): the scheduling domains
	 * are built again, so that the scheduler knows -- as sugov_init() has them built.
	 */
	em_rebuild_sched_domains();
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	kobject_put(&tunables->attr_set.kobj);
	policy->governor_data = NULL;
	sx_clear_global_tunables();

stop_kthread:
	sx_kthread_stop(sg_policy);
	mutex_unlock(&global_tunables_lock);

free_sg_policy:
	kfree(sg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void sx_exit(struct cpufreq_policy *policy)
{
	struct sx_policy *sg_policy = policy->governor_data;
	struct sx_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	/*
	 * Its last CPU has gone offline, and when one is back the policy gets this governor
	 * again (cpufreq_offline(), cpufreq_init_policy()): the tunables are kept for then.
	 * Any other end is a change of governor.
	 */
	tunables->keep = policy_is_inactive(policy);
	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		sx_clear_global_tunables();

	mutex_unlock(&global_tunables_lock);

	sx_kthread_stop(sg_policy);
	kfree(sg_policy);
	cpufreq_disable_fast_switch(policy);

	em_rebuild_sched_domains();
}

static int sx_start(struct cpufreq_policy *policy)
{
	struct sx_policy *sg_policy = policy->governor_data;
	void (*uu)(struct update_util_data *data, u64 time, unsigned int flags);
	unsigned int cpu;

	sx_update_rate_limits(sg_policy);
	sg_policy->last_freq_update_time	= 0;
	sg_policy->next_freq			= 0;
	sg_policy->work_in_progress		= false;
	sg_policy->limits_changed		= false;
	sg_policy->cached_raw_freq		= SX_NO_RAW_FREQ;
	sg_policy->warm_until			= 0;
	/*
	 * (caps: the smoothed demand is kept -- a governor stopped and started again is a system
	 * suspend, mostly; the CPUs' windows start afresh with them, below)
	 */
	sg_policy->caps_look			= 0;
	sg_policy->caps_now			= 0;
	sg_policy->caps_fmax			= sx_caps_top_step(policy);
	sg_policy->ui_until			= 0;
	sg_policy->invariant			= arch_scale_freq_invariant();
	sx_lists(sg_policy);

	sg_policy->need_freq_update = cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS);

	for_each_cpu(cpu, policy->cpus) {
		struct sx_cpu *sg_cpu = &per_cpu(sx_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->cpu			= cpu;
		sg_cpu->sg_policy		= sg_policy;
		sg_cpu->fair_max		= SCHED_CAPACITY_SCALE;
	}

	sg_policy->started = true;
	sx_slim_follow(sg_policy);
	sx_caps_follow(sg_policy);

	if (policy_is_shared(policy))
		uu = sx_update_shared;
	else if (policy->fast_switch_enabled && cpufreq_driver_has_adjust_perf())
		uu = sx_update_single_perf;
	else
		uu = sx_update_single_freq;

	for_each_cpu(cpu, policy->cpus) {
		struct sx_cpu *sg_cpu = &per_cpu(sx_cpu, cpu);

		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util, uu);
	}
	return 0;
}

static void sx_stop(struct cpufreq_policy *policy)
{
	struct sx_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_rcu();

	if (!policy->fast_switch_enabled) {
		irq_work_sync(&sg_policy->irq_work);
		kthread_cancel_work_sync(&sg_policy->work);
	}

	sg_policy->started = false;
	sx_slim_follow(sg_policy);
}

static void sx_limits(struct cpufreq_policy *policy)
{
	struct sx_policy *sg_policy = policy->governor_data;

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	}

	/* pairs with the memory barrier in sx_should_update_freq() */
	smp_wmb();

	WRITE_ONCE(sg_policy->limits_changed, true);
}

static struct cpufreq_governor schedutilmium_gov = {
	.name			= "schedutilmium",
	.owner			= THIS_MODULE,
	.flags			= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.init			= sx_init,
	.exit			= sx_exit,
	.start			= sx_start,
	.stop			= sx_stop,
	.limits			= sx_limits,
};

bool sugov_ext_is_governor(struct cpufreq_policy *policy)
{
	return policy->governor == &schedutilmium_gov;
}

cpufreq_governor_init(schedutilmium_gov);
