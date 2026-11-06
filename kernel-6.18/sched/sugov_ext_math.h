/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The arithmetic of the schedutilmium cpufreq governor and of slim WALT.
 *
 * Nothing in here touches kernel state: each function works on what it is handed. That keeps
 * the rules in one place where they can be read, and lets a host-side test compile this file
 * on the build host and run it against a transliteration of the code the rules stand for. The
 * code is that of the governor's 5.15 version statement for statement.
 *
 * Times are nanoseconds on one clock, frequencies kHz, utilisation and capacity are on the
 * scheduler's scale (1024 = the biggest CPU at its top frequency).
 */
#ifndef _KERNEL_SCHED_SUGOV_EXT_MATH_H
#define _KERNEL_SCHED_SUGOV_EXT_MATH_H

#ifdef __KERNEL__
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/types.h>
#endif

/*
 * 1. Between two steps of a frequency table.
 *
 * A request that falls between two steps is normally served by the step above it. It is
 * served by the step below while it lies in the lowest fifth of the gap: a CPU that needs
 * 3 % more than a step offers is not worth the whole next step.
 */
static inline unsigned int sx_pick_step(unsigned int raw, unsigned int below, unsigned int above)
{
	if (above <= below)
		return above;
	if (raw <= below)
		return below;
	return 5ULL * (raw - below) < above - below ? below : above;
}

/*
 * 2. A margin on the utilisation (the tunable "boost"): @boost percent more of it, or with a
 * negative @boost less, before it is mapped to a frequency. What a mode -- save power,
 * balanced, performance -- comes to in one number.
 */
static inline unsigned long sx_boost(unsigned long util, int boost)
{
	if (boost > 0)
		return util + (unsigned long)div64_u64((u64)util * (unsigned int)boost, 100);
	if (boost < 0)
		return util - (unsigned long)div64_u64((u64)util * (unsigned int)-boost, 100);
	return util;
}

/*
 * 3. From utilisation to a frequency.
 *
 * Stock schedutil asks for 1.25 * max_freq * util / capacity: the top frequency is reached at
 * 80 % of the capacity. This one is handed a utilisation that has that 1.25 in it already
 * and multiplies by 1.25 once more, which moves the point to 64 %. Two refinements:
 *
 *  - target loads: steps of {frequency, percent}, lowest frequency first. A CPU that is
 *    at least 80 % busy at a step's frequency has its utilisation raised by that step's
 *    percentage; of the steps reached, the highest counts.
 *  - above @thresh the slope is 1 + 2^-@shift instead of 1.25, but never below what the
 *    steeper slope gives at @thresh itself. (With the defaults, 1024 and 4, everything
 *    above the threshold lands above the top frequency.)
 */
struct sx_load_step {
	u32 freq;
	u32 pct;
};

/* the utilisation at which a CPU of capacity @cap is 80 % busy when it runs at @freq */
static inline unsigned long sx_load_threshold(unsigned long cap, u32 freq, unsigned long max_freq)
{
	if (!max_freq)
		return 0;
	return (unsigned long)(div64_u64((u64)cap * freq, max_freq) * 80 / 100);
}

static inline unsigned long sx_mium_freq(unsigned long max_freq, unsigned long util, unsigned long cap,
					 const struct sx_load_step *steps, unsigned int nr_steps,
					 unsigned long thresh, unsigned int shift)
{
	u64 base = (u64)max_freq + (max_freq >> 2);
	u64 raw;
	unsigned int i, reached = 0;

	for (i = 0; i < nr_steps && util >= sx_load_threshold(cap, steps[i].freq, max_freq); i++)
		reached = i + 1;
	if (reached)
		util = (unsigned long)((u64)util * (steps[reached - 1].pct + 100) / 100);

	if (util < thresh)
		raw = base * util;
	else
		raw = max(((u64)max_freq + (shift < 64 ? (u64)max_freq >> shift : 0)) * util, base * thresh);
	return cap ? (unsigned long)div64_u64(raw, cap) : 0;
}

/*
 * 4. How busy a CPU was, from the run time of what it ran.
 *
 * Time is cut into windows of @win. A sample credits the run time the running task has
 * gained since the sample before it to the window the sample falls in -- if it is the same
 * task; what ran in between two samples of different tasks is not seen. The value is the
 * larger of the current and the previous window, as a share of a window on a scale of 1024.
 * It is a share of time: it says nothing of the frequency the CPU ran at.
 */
struct sx_runwin {
	u64 start;		/* where the current window began */
	u64 curr;		/* run time credited to it */
	u64 prev;		/* and to the window before */
	u64 last_sum;		/* the running task's total run time at the last sample */
	u64 stamp;		/* when the last sample was taken; 0: never */
	unsigned long util;	/* the value at that sample */
	int pid;		/* the task that ran at the last sample */
};

/* @pid 0: the CPU is idle, nothing is credited and the windows stay where they are */
static inline void sx_runwin_sample(struct sx_runwin *rw, u64 now, u64 win, int pid, u64 sum_exec)
{
	if (!win)
		return;
	if (pid) {
		u64 delta = now - rw->start, ran = 0;

		if (delta >= win) {
			u64 n = div64_u64(delta, win);

			rw->prev = n <= 1 ? rw->curr : 0;
			rw->start += n * win;
			rw->curr = 0;
		}
		if (rw->pid == pid && rw->last_sum && sum_exec >= rw->last_sum)
			ran = sum_exec - rw->last_sum;
		rw->last_sum = sum_exec;
		rw->curr += ran;
		rw->pid = pid;
	}
	rw->stamp = now;
	rw->util = (unsigned long)div64_u64(max(rw->curr, rw->prev) << 10, win);
}

/* the last sample's value, halved for each whole window that has passed since */
static inline unsigned long sx_runwin_read(const struct sx_runwin *rw, u64 now, u64 win)
{
	u64 n;

	if (!rw->stamp || !rw->util || !win)
		return 0;
	n = now > rw->stamp ? div64_u64(now - rw->stamp, win) : 0;
	return n < 64 ? rw->util >> n : 0;
}

/*
 * 4b. How busy a CPU was, and how much it did, from its idle time ("refined").
 *
 * The busy time of a CPU is the time that passed less what it spent idle, waiting for I/O
 * included: the kernel books that at every entry to and exit from idle, and what interrupts
 * take counts as busy. So all that ran between two samples is seen -- of the run time of tasks
 * (4.) only what the task that runs at both samples ran is. A sample credits the busy time since
 * the one before, spread evenly over that stretch of time, and the same busy time weighed by
 * @scale -- the CPU's frequency as a share of its top, 1024 the top -- as work: the top
 * frequency's worth of it. (In refined mode every change of the frequency comes right after a
 * sample of all the CPUs of the policy, so @scale is the frequency the CPU had since the sample
 * before.) The share of time alone, priced at the frequency of the moment, would after a step
 * down have the faster past cut the new frequency once more, and would have a CPU busy all the
 * time climb a step at a time for a window after its work had ended.
 *
 * Two records of it:
 *  - windows of @win: the one under way and the one before, of time and of work;
 *  - the last @react in buckets of a quarter of it (the one under way and four before), so that a
 *    burst after a rest reads as one once it has lasted that long, wherever in a window it
 *    starts.
 *
 * The value (sx_busyrec_read): the work as a share of the time on a scale of 1024, the largest
 * of the last @react, the window under way as a share of the part of it that has passed -- but at
 * least of @react -- and the window before as a share of a whole one; and the share of the last
 * @react that the CPU was busy, which says whether it was busy all the time.
 */
#define SX_BUCKETS 5

struct sx_busyrec {
	u64 start;		/* where the current window began */
	u64 curr, prev;		/* busy time credited to it and to the window before */
	u64 curr_w, prev_w;	/* work credited to them */
	u64 bstart;		/* where the current bucket began */
	u64 bt[SX_BUCKETS];	/* busy time credited to the buckets, [0] the current one */
	u64 bw[SX_BUCKETS];	/* and work */
	u64 busy;		/* the CPU's busy time at the last sample */
	u64 stamp;		/* when the last sample was taken; 0: never */
};

struct sx_busyval {
	unsigned long work;	/* the top frequency's worth of work, 1024: all the time */
	unsigned long busy;	/* the share of the last react span the CPU was busy, 1024: all */
};

/* the part of @ran that falls in @part of @len (as if spread evenly over it) */
static inline u64 sx_part(u64 ran, u64 part, u64 len)
{
	return len ? mul_u64_u64_div_u64(ran, part, len) : 0;
}

/*
 * A pair of windows (the one under way, the one before) moved on to @now from @start, with what
 * was not credited yet: @ran over the @len before @now. Returns where the window under way begins.
 */
static inline u64 sx_win_move(u64 start, u64 *curr, u64 *prev, u64 now, u64 win, u64 ran, u64 len)
{
	u64 n, from;

	if (now - start < win) {
		*curr += ran;
		return start;
	}
	n = div64_u64(now - start, win);
	start += n * win;
	/* of @ran, the part that falls in the window before the new one */
	from = now - len;
	if (from < start) {
		u64 before = start - max(from, start - win);

		*prev = (n == 1 ? *curr : 0) + sx_part(ran, before, len);
		*curr = sx_part(ran, now - start, len);
	} else {
		*prev = n == 1 ? *curr : 0;
		*curr = ran;
	}
	return start;
}

/* the length of a bucket for a react span of @react */
static inline u64 sx_bucket_len(u64 react)
{
	return max(div64_u64(react, SX_BUCKETS - 1), 1ULL);
}

/*
 * The buckets moved on to @now, with @ran and @work over the @len before @now credited to those it
 * falls in. A bucket that falls out of the record takes its part with it.
 */
static inline void sx_buckets_move(struct sx_busyrec *br, u64 now, u64 bl, u64 ran, u64 work, u64 len)
{
	u64 n = now >= br->bstart ? div64_u64(now - br->bstart, bl) : 0, from = now - len;
	int k;

	if (n) {
		for (k = SX_BUCKETS - 1; k >= 0; k--) {
			br->bt[k] = (u64)k >= n ? br->bt[k - n] : 0;
			br->bw[k] = (u64)k >= n ? br->bw[k - n] : 0;
		}
		br->bstart += n * bl;
	}
	if (!len)
		return;
	for (k = 0; k < SX_BUCKETS; k++) {
		u64 lo = br->bstart >= (u64)k * bl ? br->bstart - (u64)k * bl : 0;
		u64 hi = k ? lo + bl : now, a = max(from, lo), b = min(now, hi);

		if (b > a) {
			br->bt[k] += sx_part(ran, b - a, len);
			br->bw[k] += sx_part(work, b - a, len);
		}
		if (lo <= from)
			break;
	}
}

static inline unsigned long sx_win_value(u64 curr, u64 prev, u64 into, u64 win, u64 span)
{
	u64 part = max(into, min(span, win));

	if (!part)
		part = 1;
	return (unsigned long)max(div64_u64(min(curr, part) << 10, part), div64_u64(min(prev, win) << 10, win));
}

/*
 * A sample at @now, by the windows' clock, with the CPU's busy time @busy (in ns, any origin) and
 * its frequency @scale (of 1024). One that is no later than the last (the clock of another run
 * queue) changes nothing: the next sample has what passed. A busy time that went back (a count
 * read half done, from another CPU) is taken as the new base with nothing credited -- kept, the
 * old one would have the CPU read idle until its busy time had made up for the difference. The
 * first sample starts the window and the bucket under way where it is taken.
 */
static inline void sx_busyrec_sample(struct sx_busyrec *br, u64 now, u64 win, u64 react, u64 busy, unsigned int scale)
{
	u64 ran = 0, len = 0, start;

	if (!win)
		return;
	if (!br->stamp) {
		br->start = br->bstart = now;
	} else {
		if (now <= br->stamp)
			return;
		len = now - br->stamp;
		if (busy >= br->busy)
			ran = min(busy - br->busy, len);
	}
	start = br->start;
	br->start = sx_win_move(start, &br->curr, &br->prev, now, win, ran, len);
	sx_win_move(start, &br->curr_w, &br->prev_w, now, win, (ran * min(scale, 1024U)) >> 10, len);
	sx_buckets_move(br, now, sx_bucket_len(react), ran, (ran * min(scale, 1024U)) >> 10, len);
	br->stamp = now ?: 1;
	br->busy = busy;
}

/*
 * The value at @now: the record as the last sample left it, moved on to @now with nothing busy
 * since -- which is what a CPU that has not sampled itself since has mostly been. Read from
 * another CPU as well (whose clock may be a little behind).
 */
static inline struct sx_busyval sx_busyrec_read(const struct sx_busyrec *rec, u64 now, u64 win, u64 react)
{
	struct sx_busyval v = { 0, 0 };
	struct sx_busyrec br;
	u64 bl = sx_bucket_len(react), into, t, w, span;
	int k;

	if (!rec->stamp || !win)
		return v;
	br = *rec;
	now = max(now, br.stamp);
	br.start = sx_win_move(br.start, &br.curr, &br.prev, now, win, 0, 0);
	sx_win_move(rec->start, &br.curr_w, &br.prev_w, now, win, 0, 0);
	sx_buckets_move(&br, now, bl, 0, 0, 0);
	/* the last @react: the buckets since the one four back, and of that one what @react still covers */
	into = min(now - br.bstart, bl);
	t = 0;
	w = 0;
	for (k = 0; k < SX_BUCKETS - 1; k++) {
		t += br.bt[k];
		w += br.bw[k];
	}
	t += sx_part(br.bt[SX_BUCKETS - 1], bl - into, bl);
	w += sx_part(br.bw[SX_BUCKETS - 1], bl - into, bl);
	span = (SX_BUCKETS - 1) * bl;
	v.busy = (unsigned long)div64_u64(min(t, span) << 10, span);
	v.work = (unsigned long)max(div64_u64(min(w, span) << 10, span),
				    (u64)sx_win_value(br.curr_w, br.prev_w, now - br.start, win, react));
	return v;
}

/*
 * 5. slim WALT: how busy a CPU was, sampled at the scheduler tick.
 *
 * At each tick the whole time since the tick before counts as busy if anything is runnable
 * at that moment, and as idle if not. A window ends at the first tick at or after its
 * length; the busy share of the last complete window is the load. A CPU whose tick was
 * stopped is not sampled: a gap of more than 8 windows empties the window, a shorter one
 * is booked whole, as busy or as idle, by the tick that ends it.
 */
struct sx_slim {
	u64 win_start;		/* where the current window began */
	u64 last;		/* the last tick seen; 0: none yet */
	u64 busy, total;	/* of the current window */
	u64 nr_sum;		/* runnable tasks, summed over its ticks */
	u32 nr_max, samples;
	u64 prev_busy, prev_total;	/* the last complete window */
	u32 prev_nr_avg;	/* runnable tasks per tick, times 1024 */
	u32 prev_nr_max;
	u64 nr_windows;		/* complete windows so far */
};

static inline void sx_slim_reset(struct sx_slim *sw)
{
	sw->busy = sw->total = sw->nr_sum = 0;
	sw->nr_max = sw->samples = 0;
}

static inline void sx_slim_tick(struct sx_slim *sw, u64 now, unsigned int nr_running, u64 win)
{
	u64 delta;

	if (!sw->last) {
		sw->win_start = sw->last = now;
		return;
	}
	delta = now - sw->last;
	if (delta > 8 * win) {
		delta = 0;
		sx_slim_reset(sw);
	}
	if (nr_running)
		sw->busy += delta;
	sw->total += delta;
	sw->nr_sum += nr_running;
	sw->samples++;
	sw->nr_max = max(sw->nr_max, nr_running);
	sw->last = now;
	if (now - sw->win_start < win)
		return;

	sw->prev_busy = sw->busy;
	sw->prev_total = sw->total;
	sw->prev_nr_avg = (u32)div64_u64(sw->nr_sum << 10, sw->samples);
	sw->prev_nr_max = sw->nr_max;
	sw->win_start = now;
	sw->nr_windows++;
	sx_slim_reset(sw);
}

/*
 * The busy share of the last complete window on a scale of @scale (1024 for the load, 100
 * for a percentage), halved for each whole window since the last tick, nothing after 9.
 */
static inline unsigned long sx_slim_share(const struct sx_slim *sw, u64 now, u64 win, unsigned int scale)
{
	u64 n = 0;

	if (!sw->prev_total || !sw->last || !win)
		return 0;
	if (now > sw->last && now - sw->last >= win) {
		n = div64_u64(now - sw->last, win);
		if (n > 9)
			return 0;
	}
	return (unsigned long)div64_u64((sw->prev_busy >> n) * scale, sw->prev_total);
}

/*
 * 6. A ceiling that follows the load (the tunable "caps").
 *
 * This rule ran in user space first, on the 5.15 kernel (a daemon's "caps"); it is taken from
 * there statement by statement: each CPU's busy share of a window of SX_CAPS_WINDOW, of 1024 in
 * steps of 16, and one sample early in a window, once SX_CAPS_EARLY of it has passed with
 * SX_CAPS_EARLY_BUSY of that busy; a sample that repeats the CPU's last one within SX_CAPS_REPEAT
 * is no news; the cluster's demand is the highest of its CPUs' last samples that are not older
 * than SX_CAPS_STALE, and its smoothed demand moves a quarter of the way to that at every sample
 * that is news. The ceiling is the table's step at or above the smoothed demand x (100 + boost) %
 * of the top step (sx_caps_raw).
 *
 * In user space the CPUs were looked at every 40 ms while anything ran and every 250 to 350 ms
 * while nothing did: the end of a window was seen SX_CAPS_LATE after it at the latest. Here they
 * are looked at when the governor works the policy out, which an idle cluster does not ask for: a
 * window that ended longer ago than that with nobody looking was an idle one, and the next look
 * takes it as a sample of nothing at the moment it ended (SX_CAPS_GAP_MAX of them at most; the
 * smoothed demand has long reached nothing by then); what the CPU did in all that time goes to the
 * window the look ends.
 */
#define SX_CAPS_WINDOW		320000000ull
#define SX_CAPS_EARLY		16000000ull
#define SX_CAPS_EARLY_BUSY	4000000ull
#define SX_CAPS_REPEAT		480000000ull
#define SX_CAPS_STALE		960000000ull
#define SX_CAPS_LATE		350000000ull
#define SX_CAPS_GAP_MAX		16
#define SX_CAPS_NONE		0xffffffffu

struct sx_caps_cpu {
	u64 start;	/* when the window began; 0: not looked at yet */
	u64 busy0;	/* the CPU's busy time then */
	u64 last;	/* and at the last look */
	u64 at;		/* when the last sample that was news was taken */
	u32 util;	/* that sample, of 1024; SX_CAPS_NONE: none yet */
	bool early;	/* the window's early sample has been taken */
};

/* the highest sample of the cluster's @n CPUs that still counts at @now */
static inline u32 sx_caps_demand(struct sx_caps_cpu *const *cpus, int n, u64 now)
{
	u32 demand = 0;
	int i;

	for (i = 0; i < n; i++)
		if (cpus[i]->util != SX_CAPS_NONE && now - cpus[i]->at <= SX_CAPS_STALE && cpus[i]->util > demand)
			demand = cpus[i]->util;
	return demand;
}

/* a sample @util of @c at @now: unless it is a repeat, it moves the smoothed demand */
static inline void sx_caps_note(struct sx_caps_cpu *c, struct sx_caps_cpu *const *cpus, int n, u64 now, u32 util,
				u32 *smooth)
{
	u32 demand;

	if (util == c->util && now - c->at < SX_CAPS_REPEAT)
		return;
	c->util = util;
	c->at = now;
	demand = sx_caps_demand(cpus, n, now);
	*smooth = *smooth == SX_CAPS_NONE ? demand : (u32)((3ull * *smooth + demand) >> 2);
}

/*
 * One CPU of a cluster looked at, at @now, with its busy time so far @busy: its samples and what
 * they do to the cluster's @smooth. @cpus are the cluster's @n CPUs, @c among them.
 */
static inline void sx_caps_look(struct sx_caps_cpu *c, struct sx_caps_cpu *const *cpus, int n, u64 now, u64 busy,
				u32 *smooth)
{
	u64 span, ran;

	u64 last = c->last;

	if (!c->start) {
		c->start = now;
		c->busy0 = busy;
		c->last = busy;
		c->util = SX_CAPS_NONE;
		c->at = 0;
		c->early = false;
		return;
	}
	if (now <= c->start)
		return;
	/* (a count read from elsewhere can go back a little: the time is the most it has said) */
	if (busy < last)
		busy = last;
	c->last = busy;
	span = now - c->start;
	/* (a count that went back is a new base) */
	ran = busy > c->busy0 ? busy - c->busy0 : 0;
	if (span >= SX_CAPS_WINDOW) {
		int k;

		/*
		 * (the windows that ended long before anybody looked: idle ones -- but for the first,
		 * which had what was seen at the last look; what came after it goes to this one)
		 */
		for (k = 0; span >= SX_CAPS_WINDOW + SX_CAPS_LATE && k < SX_CAPS_GAP_MAX; k++) {
			u64 part = 0;

			if (!k) {
				part = last > c->busy0 ? min(last - c->busy0, SX_CAPS_WINDOW) : 0;
				ran = busy > last ? busy - last : 0;
			}
			c->start += SX_CAPS_WINDOW;
			span -= SX_CAPS_WINDOW;
			sx_caps_note(c, cpus, n, c->start, (u32)div64_u64(part << 10, SX_CAPS_WINDOW) & ~15u, smooth);
		}
		c->start = now;
		c->busy0 = busy;
		c->early = false;
	} else if (!c->early && span >= SX_CAPS_EARLY && ran >= SX_CAPS_EARLY_BUSY) {
		c->early = true;
	} else {
		return;
	}
	if (ran > span)
		ran = span;
	sx_caps_note(c, cpus, n, now, (u32)div64_u64(ran << 10, span) & ~15u, smooth);
}

/*
 * The ceiling the smoothed demand asks for, before it becomes a step of the table: @smooth x
 * (100 + @boost) % of @fmax (the table's top step). (No sample yet -- SX_CAPS_NONE -- is no
 * ceiling at all: the caller tells it apart from a demand of nothing, which is the floor.)
 */
static inline u64 sx_caps_raw(u32 smooth, unsigned int boost, unsigned int fmax)
{
	return ((u64)smooth * (100 + boost) / 100 * fmax) >> 10;
}

/*
 * 8. A step of a frequency table for a request.
 *
 * cpufreq_frequency_table_target() walks all of a table's entries at every lookup, and the
 * governor makes three to five lookups each time it works a policy out. @steps is the table's
 * valid frequencies in ascending order without repeats (@n of them). Of those within [@min, @max]:
 * @high false (cpufreq's CPUFREQ_RELATION_L) the lowest at or above @khz, else the highest under
 * it; @high true (CPUFREQ_RELATION_H) the highest at or below @khz, else the lowest above it.
 * False where no step lies within the limits: cpufreq warns then and takes the table's first entry.
 * (6.18's lookup is the same where it is not asked for "efficient" steps only.)
 */
static inline unsigned int sx_steps_index(const unsigned int *steps, unsigned int n, unsigned int khz)
{
	unsigned int lo = 0, hi = n;

	while (lo < hi) {
		unsigned int mid = (lo + hi) / 2;

		if (steps[mid] < khz)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* the steps within [@min, @max]: [*@lo, *@hi) -- the part that changes only with the limits */
static inline void sx_steps_limits(const unsigned int *steps, unsigned int n, unsigned int min,
				   unsigned int max, unsigned int *lo, unsigned int *hi)
{
	*lo = sx_steps_index(steps, n, min);
	*hi = sx_steps_index(steps, n, max);
	if (*hi < n && steps[*hi] == max)
		(*hi)++;
}

/* the step for @khz among those in [@lo, @hi), which must not be empty */
static inline unsigned int sx_steps_pick(const unsigned int *steps, unsigned int n, unsigned int lo,
					 unsigned int hi, unsigned int khz, bool high)
{
	unsigned int i = sx_steps_index(steps, n, khz);

	if (!high)
		return i < hi ? steps[i > lo ? i : lo] : steps[hi - 1];
	if (i >= lo && i < hi && steps[i] == khz)
		return khz;
	return i > lo ? steps[(i < hi ? i : hi) - 1] : steps[lo];
}

static inline bool sx_steps_find(const unsigned int *steps, unsigned int n, unsigned int min,
				 unsigned int max, unsigned int khz, bool high, unsigned int *step)
{
	unsigned int lo, hi;

	sx_steps_limits(steps, n, min, max, &lo, &hi);
	if (lo >= hi)
		return false;
	*step = sx_steps_pick(steps, n, lo, hi, khz, high);
	return true;
}

/*
 * 9. A ceiling for bursts (the tunables "caps_burst" and "caps_burst_ms").
 *
 * The smoothed demand of 6 takes a burst for a long load. A CPU that ran through the first tens of
 * milliseconds of its window has a sample near all of it at its next look, which stands for the
 * cluster's demand for SX_CAPS_STALE -- and every sample of the cluster's other CPUs in that time
 * moves the smoothed demand a quarter of the way towards it. On the 13 Pro (four middle CPUs, up to
 * two samples a window each) a few hundred milliseconds of an app's main thread took the middle
 * cores' ceiling from 1.4 GHz to 2.6-2.7 GHz while a feed was flicked.
 *
 * So a load is a long one only once it has lasted: the ceiling stays at caps_burst until the
 * cluster's busiest CPU has been busy SX_BURST_BUSY of the time from one look to the next, look
 * after look, for caps_burst_ms. (The busiest CPU may be another at each look: the frequency is the
 * cluster's. A cluster that nothing asks about is not looked at: the first look after it is one
 * of a long stretch, not a busy one.)
 */
#define SX_BURST_BUSY	922	/* of 1024: 90 % */

/*
 * From when the cluster has been busy look after look (0: it is not), after a look at @now that
 * follows the last at @last (0: none); @most is the most that any of its CPUs ran in between.
 */
static inline u64 sx_burst_since(u64 since, u64 last, u64 now, u64 most)
{
	if (!last || now <= last || (most << 10) < (now - last) * SX_BURST_BUSY)
		return 0;
	return since ?: last;
}

/* is the ceiling held at caps_burst at @now, the cluster busy from @since (0: not) on? */
static inline bool sx_burst_held(u64 since, u64 now, unsigned int ms)
{
	return !since || now - since < (u64)ms * 1000000ull;
}

#endif /* _KERNEL_SCHED_SUGOV_EXT_MATH_H */
