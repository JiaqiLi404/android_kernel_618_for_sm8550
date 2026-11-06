// SPDX-License-Identifier: GPL-2.0
/*
 * k618_sugov_test: the schedutilmium cpufreq governor at work on the 6.18 GKI (kernel-6.18/k618_sched.py),
 * on a machine whose cpufreq driver is the kernel's cpufreq_testbench: a QEMU guest booted with
 * cpufreq_testbench.enable=1 and at least three CPUs. CPU 0 then has a policy to itself (a governor's
 * single-CPU path), the others share one, and "the hardware" is a record of the table entry the driver
 * was told last -- told the way Qualcomm's driver takes it, by the entry the cpufreq core resolved last.
 * kernel-6.18/k618_sugov_qemu.sh builds it and boots it as /init (no shell, no Android: it mounts what it
 * needs, writes its lines to the kernel log and powers the machine off).
 *
 * The governor's boot test from the 5.15 kernel, with these changes for 6.18:
 *
 *   built-in values   6.18's governor comes with the values the 5.15 tuning module wrote (refined 1,
 *                     boost -20, and by the policy's place among the frequency domains: on the first a
 *                     touch floor and no load-following ceiling, on the second that ceiling with
 *                     caps_burst): checked on both policies first. Then both get the 5.15 kernel's
 *                     values, which the checks after were written for (as_515()), and every switch to
 *                     the governor does that again (set_governor(); set_governor_raw() does not)
 *   "walt"            written to scaling_governor selects schedutilmium (cpufreq.walt_governor)
 *   the multiplier    5.15's cpu.uclamp.min.multiplier is a patch of that kernel's own, not on 6.18: not
 *                     checked; the group ceilings and the PELT multiplier are
 *   after a change    of governor the tunables are 6.18's built-in values (check_hotplug)
 *   EAS               "k618_sugov_test eas" (or sugov_mode=eas on the command line), a boot of its own
 *                     with a device tree that gives CPU 0 half the others' capacity, the test bench's
 *                     energy model (cpufreq_testbench.em=1), sched_verbose and
 *                     cpufreq.walt_governor=schedutil: EAS stays possible and running with
 *                     schedutilmium on every policy, stops with another governor on one and starts again
 *                     with schedutilmium back; "walt" then selects schedutil (the way back)
 *
 * Nothing runs faster or slower there, so a thread that is busy half of the time stays busy
 * half of the time at any frequency. That is enough to tell the rules apart:
 *
 *   the governor      can be selected, has its files with the built-in values; under a busy loop the
 *                     policy reaches the top frequency and the driver is then left alone;
 *                     back at rest it reaches the lowest
 *   the rate limits   with an up limit of 300 ms the frequency climbs no faster; with a down
 *                     limit of 600 ms it stays up that long after its last change
 *   the limits        a lowered scaling_max_freq is obeyed at once, whatever the down limit
 *   the busy share    a thread busy half of the time: high frequency with the busy share
 *                     (slim WALT, or the run-time windows alone), low with enabled at 0 and
 *                     with refined at 1 and react_us a window long (with a tick, the default,
 *                     its bursts count as such: shown, not judged -- react_us's rule is checked
 *                     on the build host)
 *   a busy CPU read   from another: a busy loop on CPU 1 keeps the shared policy at the top
 *   from elsewhere    with refined at 1 and a boost of -30 while a thread on CPU 2 that wakes
 *                     every millisecond has it worked out over and over
 *   two threads       that hand CPU 1 to each other every 200 us keep it busy: with refined at
 *                     1 and a boost of -30 the policy goes to the top as under a busy loop
 *                     (the run time of the thread that ran at two looks in a row sees next to
 *                     nothing of it)
 *   new processes     a child forked every 50 ms onto an idle CPU of the shared policy, 3 ms of
 *                     work each, another CPU of it asking every millisecond: with refined at 1
 *                     and a boost of -30 the policy never goes past the step above its lowest,
 *                     and is there a tenth of the time at most (a new task's starting utilisation,
 *                     taken as it is, took it up each time, to 1.1-1.5 GHz)
 *   the list          target_loads reads back as written; what is not a list, not in order or
 *                     out of range is refused
 *   slim WALT         its files are there; the tick is sampled while the governor runs with
 *                     use_slim_walt and not otherwise
 *   the shared policy a busy loop on one of its CPUs takes it to the top, CPU 0's stays down
 *   the driver        was never called with a frequency other than that of the table entry
 *                     it went to (a step the governor picks has to be resolved)
 *   with the governor kernel.sched_pelt_multiplier takes 2
 *   a group's ceiling a busy loop of a group with cpu.uclamp.max at 20 % stays low, although
 *                     the busy share has replaced the utilisation -- also with a real-time
 *                     thread that wakes on the same CPU every 3 ms and has no ceiling
 *   boost             with -50 a busy loop no longer gets the top frequency; with 100 a
 *                     half-busy thread gets more
 *   the floors        the policy rests at the step above "floor"; with "touch_floor" it goes
 *                     there when the screen (one made through uinput) is touched, for touch_ms
 *   caps              the ceiling that follows the load: a busy loop from rest held at the
 *                     floor's step, let up over seconds; lifted by a finger put down and by a
 *                     process moved into a cpuset group top-app (not by a thread on its own);
 *                     caps_max, caps_burst for a load that has not lasted, and "ceiling" over it all
 *   caps_ui_ms        a burst of a thread that draws -- the main thread of a process in the cpuset
 *                     top-app, or its RenderThread -- lifts that ceiling to caps_front_pct % of the
 *                     top, through short sleeps too; another thread of it, a process elsewhere and
 *                     short bursts do not, and the ceiling is held again soon after the burst
 *   I/O waits        a thread that waits for I/O on CPU 0 four times every 4 ms leaves it low,
 *                    unless iowait_boost is 1 (schedutil's boost, off by default here)
 *   a cluster offline the shared policy's CPUs are all taken offline and brought back: its
 *                     governor and what was written to its tunables are as they were; after
 *                     a change of governor they are the built-in values
 *
 * The governors and limits that were set before are put back.
 * Prints a line per check, then "SUGOV TEST OK: ..." (exit 0) or "SUGOV TEST FAILED: n"; the EAS
 * boot "SUGOV EAS OK: ..." or "SUGOV EAS FAILED: n".
 * build: aarch64-linux-gnu-gcc -O2 -static -Wall -pthread -o k618_sugov_test kernel-6.18/k618_sugov_test.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/loop.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CPUFREQ "/sys/devices/system/cpu/cpufreq"
#define RT_DEFAULT "/proc/sys/kernel/sched_util_clamp_min_rt_default"
#define WALT_GOV "/sys/module/cpufreq/parameters/walt_governor"
#define EAS "/proc/sys/kernel/sched_energy_aware"
#define LOWEST 300000
#define TOP 2000000

static int failures, checks;

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000l };

	clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

static void pin(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}

static const char *path(int policy, const char *file)
{
	static char buf[4][200];
	static int at;
	char *p = buf[at++ & 3];

	snprintf(p, sizeof(buf[0]), CPUFREQ "/policy%d/%s", policy, file);
	return p;
}

static const char *get(const char *file, char *buf, size_t size)
{
	int fd = open(file, O_RDONLY);
	ssize_t n = fd < 0 ? -1 : read(fd, buf, size - 1);

	if (fd >= 0)
		close(fd);
	if (n < 0)
		n = 0;
	buf[n] = 0;
	buf[strcspn(buf, "\n")] = 0;
	return buf;
}

static long number(const char *file)
{
	char buf[64];

	return strtol(get(file, buf, sizeof(buf)), NULL, 10);
}

/* 0, or the error of the write */
static int put(const char *file, const char *text)
{
	int fd = open(file, O_WRONLY), err = 0;

	if (fd < 0)
		return errno;
	if (write(fd, text, strlen(text)) < 0)
		err = errno;
	close(fd);
	return err;
}

static void ok(int good, const char *fmt, ...)
{
	va_list ap;

	checks++;
	if (!good)
		failures++;
	printf("sugov %s", good ? "" : "FAILED: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

/* --- something for a CPU to do --- */

enum { BUSY, HALF, TICKLE, RT_TICKLE, EVERY_MS };

struct load {
	int cpu, mode;
	const char *join;	/* the "tasks" file of a cgroup the thread puts itself in */
	volatile int joined;	/* 0, or the error of that */
	volatile int stop;
	pthread_t thread;
};

static void spin_until(long long until, volatile int *stop)
{
	while (now_ns() < until && !*stop)
		;
}

static void *load_main(void *arg)
{
	struct load *l = arg;

	if (l->join) {
		char tid[16];

		snprintf(tid, sizeof(tid), "%ld", syscall(SYS_gettid));
		l->joined = put(l->join, tid);
	}
	pin(l->cpu);
	prctl(PR_SET_TIMERSLACK, 1UL);
	if (l->mode == RT_TICKLE) {
		struct sched_param param = { .sched_priority = 1 };

		l->joined = sched_setscheduler(0, SCHED_FIFO, &param) ? errno : 0;
	}
	while (!l->stop) {
		if (l->mode == BUSY) {
			spin_until(now_ns() + 2000000, &l->stop);
		} else if (l->mode == HALF) {
			/* three ticks on, three off */
			spin_until(now_ns() + 12000000, &l->stop);
			sleep_ms(12);
		} else if (l->mode == RT_TICKLE) {
			/* next to nothing every 3 ms, as a real-time thread (it was made one before it came here) */
			sleep_ms(3);
		} else if (l->mode == EVERY_MS) {
			/* next to nothing every millisecond: the shared policy is asked each time */
			sleep_ms(1);
		} else {
			/*
			 * Nothing to speak of, but the governor is asked every 10 ms. (Longer than a
			 * tick on purpose: schedutil's rule for a single CPU, "not down while the
			 * CPU has not been idle", counts an idle period only if the tick was stopped
			 * for it. Woken every 3 ms a CPU with nothing to do is busy by that count.)
			 */
			sleep_ms(10);
		}
	}
	return NULL;
}

static void start_in(struct load *l, int cpu, int mode, const char *join)
{
	l->cpu = cpu;
	l->mode = mode;
	l->join = join;
	l->joined = 0;
	l->stop = 0;
	pthread_create(&l->thread, NULL, load_main, l);
}

static void start(struct load *l, int cpu, int mode)
{
	start_in(l, cpu, mode, NULL);
}

static void stop(struct load *l)
{
	l->stop = 1;
	pthread_join(l->thread, NULL);
}

/* --- looking at a policy --- */

static long cur(int policy)
{
	return number(path(policy, "scaling_cur_freq"));
}

/*
 * How often the driver was called with another frequency than that of the table entry it
 * went to -- the entry the cpufreq core resolved last. A governor that works out a step by
 * itself and does not have it resolved shows up here.
 */
static long mismatches(int policy)
{
	return number(path(policy, "testbench_mismatches"));
}

/* waits until the policy is at @freq; the milliseconds it took, or -1 after @limit_ms */
static int wait_for(int policy, long freq, int limit_ms)
{
	long long t0 = now_ns();

	while (cur(policy) != freq) {
		if (now_ns() - t0 > limit_ms * 1000000ll)
			return -1;
		sleep_ms(4);
	}
	return (int)((now_ns() - t0) / 1000000);
}

struct trace {
	int n;
	int ms[1200];
	long freq[1200];
};

/* the policy's frequency every 4 ms for @ms milliseconds */
static void watch(int policy, int ms, struct trace *t)
{
	long long t0 = now_ns();

	memset(t, 0, sizeof(*t));
	while (t->n < 1200 && now_ns() - t0 < ms * 1000000ll) {
		t->ms[t->n] = (int)((now_ns() - t0) / 1000000);
		t->freq[t->n++] = cur(policy);
		sleep_ms(4);
	}
}

/* when the frequency first was above @freq, or -1 */
static int first_above(const struct trace *t, long freq)
{
	for (int i = 0; i < t->n; i++)
		if (t->freq[i] > freq)
			return t->ms[i];
	return -1;
}

static int compare_long(const void *a, const void *b)
{
	long x = *(const long *)a, y = *(const long *)b;

	return x < y ? -1 : x > y;
}

/* the frequency that three in ten of the looks after @from_ms are at or above */
static long upper_after(const struct trace *t, int from_ms)
{
	long v[1200];
	int n = 0;

	for (int i = 0; i < t->n; i++)
		if (t->ms[i] >= from_ms)
			v[n++] = t->freq[i];
	if (!n)
		return -1;
	qsort(v, n, sizeof(v[0]), compare_long);
	return v[n * 7 / 10];
}

/* a trace as its runs of equal frequency: what a check that failed had looked at */
static void show(const struct trace *t, const char *what)
{
	int from = 0, lines = 0;

	for (int i = 1; i <= t->n && lines < 14; i++)
		if (i == t->n || t->freq[i] != t->freq[from]) {
			printf("sugov   %s: at %4d ms %7ld for %d ms\n", what, t->ms[from], t->freq[from],
			       (i == t->n ? t->ms[t->n - 1] + 4 : t->ms[i]) - t->ms[from]);
			from = i;
			lines++;
		}
}

static int set_governor_raw(int policy, const char *governor)
{
	char buf[64];

	return !put(path(policy, "scaling_governor"), governor) && !strcmp(get(path(policy, "scaling_governor"), buf, sizeof(buf)), governor);
}

static const char *tunable(int policy, const char *governor, const char *name)
{
	static char buf[4][200];
	static int at;
	char *p = buf[at++ & 3];

	snprintf(p, sizeof(buf[0]), CPUFREQ "/policy%d/%s/%s", policy, governor, name);
	return p;
}

static int is(int policy, const char *governor, const char *name, const char *want)
{
	char buf[200];

	return !strcmp(get(tunable(policy, governor, name), buf, sizeof(buf)), want);
}

/* --- the checks --- */

#define GOV "schedutilmium"

/*
 * The values of the 5.15 kernel's governor where 6.18's built-in ones differ (k618_sched.py): the
 * checks were written for them. With these the governor is what the 5.15 test saw.
 */
static void as_515(int policy)
{
	static const char *const name[] = { "refined", "boost", "touch_floor", "touch_ms", "caps", "caps_floor", "caps_burst" };

	for (unsigned int i = 0; i < sizeof(name) / sizeof(name[0]); i++)
		put(tunable(policy, GOV, name[i]), "0");
}

/* (a switch to the governor gives it the 5.15 kernel's values, as the checks expect them) */
static int set_governor(int policy, const char *governor)
{
	char buf[64];
	int switched = strcmp(get(path(policy, "scaling_governor"), buf, sizeof(buf)), governor) != 0;

	if (!set_governor_raw(policy, governor))
		return 0;
	if (switched && !strcmp(governor, GOV))
		as_515(policy);
	return 1;
}

static struct load tickle;

/* brings CPU 0's policy to rest at the lowest frequency */
static int rest(void)
{
	return wait_for(0, LOWEST, 4000);
}

static void check_governor(const char *governor, const char *const *files, const char *const *defaults)
{
	struct load busy;
	long switches = number(path(0, "testbench_switches"));
	int up, down, all = 1;

	if (!set_governor_raw(0, governor)) {
		ok(0, "%s cannot be made policy0's governor", governor);
		return;
	}
	for (int i = 0; files[i]; i++)
		if (!is(0, governor, files[i], defaults[i])) {
			char buf[200];

			ok(0, "%s/%s reads \"%s\", not \"%s\"", governor, files[i], get(tunable(0, governor, files[i]), buf, sizeof(buf)), defaults[i]);
			all = 0;
		}
	if (all)
		ok(1, "%s: selected, its files are there with the built-in values of the first cluster (refined 1, boost -20, a touch floor, no caps)", governor);

	down = rest();
	start(&busy, 0, BUSY);
	up = wait_for(0, TOP, 4000);
	/*
	 * At the top nothing changes: the driver is not called over and over. (Half a second
	 * first: the top may have been a moment's request, of a real-time thread that woke,
	 * before the loop's own utilisation had got there.)
	 */
	sleep_ms(500);
	switches = number(path(0, "testbench_switches"));
	sleep_ms(300);
	switches = number(path(0, "testbench_switches")) - switches;
	if (cur(0) != TOP)
		up = -1;
	stop(&busy);
	ok(down >= 0 && up >= 0 && !switches,
	   "%s: a busy loop takes CPU 0 from the lowest frequency to the top one in %d ms, and in 300 ms there the driver is called %ld times", governor, up, switches);
	down = rest();
	ok(down >= 0, "%s: at rest again the frequency is back at the lowest after %d ms", governor, down);
}

static void check_rate_limits(void)
{
	struct load busy;
	struct trace t;
	int shortest = 1 << 30, changes = 0, last = -1, fell = -1, top;

	/*
	 * Without a limit a busy loop is at the top within tens of ms (a new thread starts with
	 * half a CPU's utilisation: one step from rest, the next when it has run a little). With
	 * an up limit of 300 ms the second step has to wait that long for the first. (The limits
	 * are the skeleton's, whatever is mapped to a frequency: looked at with enabled at 0,
	 * where the scheduler's utilisation makes those two steps.)
	 */
	set_governor(0, GOV);
	put(tunable(0, GOV, "enabled"), "0");
	put(tunable(0, GOV, "up_rate_limit_us"), "300000");
	rest();
	sleep_ms(350);
	start(&busy, 0, BUSY);
	watch(0, 3000, &t);
	top = first_above(&t, TOP - 1);
	for (int i = 1; i < t.n; i++) {
		if (t.freq[i] == t.freq[i - 1])
			continue;
		if (last >= 0 && t.ms[i] - last < shortest)
			shortest = t.ms[i] - last;
		last = t.ms[i];
		changes++;
	}
	ok(changes >= 1 && (changes < 2 || shortest >= 280) && top >= 280 && t.freq[t.n - 1] == TOP,
	   "an up limit of 300 ms: a busy loop is at the top after %d ms, in %d steps seen (the two closest %d ms apart)", top, changes,
	   changes < 2 ? 0 : shortest);

	/* the busy loop goes on; the frequency last changed some time ago. Now it may not drop for 600 ms after a change */
	put(tunable(0, GOV, "up_rate_limit_us"), "500");
	put(tunable(0, GOV, "down_rate_limit_us"), "600000");
	put(path(0, "scaling_max_freq"), "1700000");
	wait_for(0, 1700000, 1000);
	put(path(0, "scaling_max_freq"), "2000000");
	last = wait_for(0, TOP, 2000);
	stop(&busy);
	watch(0, 1500, &t);
	for (int i = 0; i < t.n && fell < 0; i++)
		if (t.freq[i] < TOP)
			fell = t.ms[i];
	ok(last >= 0 && fell >= 550, "a down limit of 600 ms: the load gone right after the last step up, the frequency fell after %d ms", fell);
	put(tunable(0, GOV, "down_rate_limit_us"), "1000");
	put(tunable(0, GOV, "enabled"), "1");
}

static void check_limits(void)
{
	struct load busy;
	int up, down;

	set_governor(0, "schedutilmium");
	put(tunable(0, "schedutilmium", "down_rate_limit_us"), "3000000");
	start(&busy, 0, BUSY);
	up = wait_for(0, TOP, 4000);
	put(path(0, "scaling_max_freq"), "1100000");
	down = wait_for(0, 1100000, 1000);
	ok(up >= 0 && down >= 0 && down < 300 && number(path(0, "testbench_hw_freq")) == 1100000,
	   "a maximum lowered to 1.1 GHz under a busy loop is obeyed after %d ms, with a down limit of 3 s (the driver is at %ld)", down,
	   number(path(0, "testbench_hw_freq")));
	put(path(0, "scaling_max_freq"), "2000000");
	stop(&busy);
	put(tunable(0, "schedutilmium", "down_rate_limit_us"), "1000");
}

/* the mean of the looks after @from_ms (-1: none) */
static long mean_after(const struct trace *t, int from_ms)
{
	long long sum = 0;
	int n = 0;

	for (int i = 0; i < t->n; i++)
		if (t->ms[i] >= from_ms) {
			sum += t->freq[i];
			n++;
		}
	return n ? (long)(sum / n) : -1;
}

/*
 * a thread busy half of the time on CPU 1: where the shared policy's frequency is while it runs
 * (and, with @mean, its mean)
 */
static long half_busy(long *mean)
{
	struct load half;
	struct trace t;

	wait_for(1, LOWEST, 4000);
	start(&half, 1, HALF);
	watch(1, 1800, &t);
	stop(&half);
	if (mean)
		*mean = mean_after(&t, 700);
	return upper_after(&t, 700);
}

/*
 * Half of the time at any frequency. The scheduler's utilisation is that share of what the
 * CPU does at the frequency it runs at, and a request of 1.25 times it ends at the lowest
 * step. The share of time alone asks for 1.5625 * 0.5 of the top frequency or more -- while
 * the thread runs: with nothing to run the CPU's utilisation is the scheduler's again. So
 * the frequency looked at is the one that three looks in ten are at or above.
 *
 * On the policy the CPUs share: schedutilmium keeps schedutil's rule that a single CPU's
 * frequency does not drop while the CPU has not been idle, and on CPU 0 whatever takes the
 * request up for a moment then holds it there for as long as the load runs.
 */
static void check_half_busy(void)
{
	long share, windows, off, refined, whole, bursts;

	set_governor(1, GOV);
	share = half_busy(NULL);
	put(tunable(1, GOV, "use_slim_walt"), "0");
	windows = half_busy(NULL);
	put(tunable(1, GOV, "use_slim_walt"), "1");
	put(tunable(1, GOV, "enabled"), "0");
	off = half_busy(NULL);
	put(tunable(1, GOV, "enabled"), "1");
	put(tunable(1, GOV, "refined"), "1");
	put(tunable(1, GOV, "react_us"), "20000");
	refined = half_busy(&whole);
	put(tunable(1, GOV, "react_us"), "4000");
	half_busy(&bursts);
	put(tunable(1, GOV, "refined"), "0");
	ok(off <= 700000, "busy half of the time: %ld with enabled at 0 (utilisation that follows the frequency: low)", off);
	/*
	 * (How high depends on how the thread's 24 ms fall into the windows of 20, and on how often
	 * the governor is asked while the thread sleeps -- by whatever else wakes on these CPUs:
	 * 1.1 to 2 GHz have been seen. Low is 0.5.)
	 */
	ok(share >= 900000 && windows >= 900000, "busy half of the time: %ld with slim WALT, %ld with the run-time windows alone (a share of time: high)",
	   share, windows);
	/* (it settles where the share makes 64 % of what the frequency offers: a step or two up, here, where no frequency offers more) */
	ok(refined <= 1300000 && refined < windows,
	   "busy half of the time: %ld with refined at 1 and react_us a window long (the busy time taken at the frequency it was measured at: in between)",
	   refined);
	/*
	 * (12 ms on, 12 off: with the window under way read from a tick on, an on stretch is all of
	 * what has passed of a window that began with it, and the frequency goes up while it runs.
	 * How much depends on how the stretches fall into the windows: 5-23 % more in the mean.)
	 */
	printf("sugov   (react_us a tick: a mean of %ld, %ld with react_us a window long)\n", bursts, whole);
}

/*
 * A busy loop on CPU 1, and on CPU 2 a thread that wakes every millisecond: each of its wake-ups
 * has the shared policy worked out, on CPU 2, with CPU 1's busy share read from there. A read that
 * took CPU 1 for idle since it last looked at itself -- at its ticks, and only where the rate
 * limits let it -- would have the policy fall below the top between them.
 */
static int share_above(const struct trace *t, long freq, int from_ms);

static void check_remote(void)
{
	struct load busy, waker;
	struct trace t;
	int up, top;

	set_governor(1, GOV);
	put(tunable(1, GOV, "refined"), "1");
	put(tunable(1, GOV, "boost"), "-30");
	wait_for(1, LOWEST, 4000);
	start(&busy, 1, BUSY);
	start(&waker, 2, EVERY_MS);
	up = wait_for(1, TOP, 3000);
	watch(1, 1000, &t);
	stop(&waker);
	stop(&busy);
	top = share_above(&t, TOP - 1, 0);
	put(tunable(1, GOV, "boost"), "0");
	put(tunable(1, GOV, "refined"), "0");
	ok(up >= 0 && top >= 90,
	   "a busy loop on CPU 1 read from CPU 2, which wakes every millisecond: with refined at 1 and a boost of -30 the policy is at the top after %d ms and there %d %% of a second",
	   up, top);
	if (up < 0 || top < 90)
		show(&t, "busy CPU 1, CPU 2 waking");
	wait_for(1, LOWEST, 4000);
}

/*
 * Two threads that hand CPU 1 to each other through two pipes, 200 us of work a turn: the CPU is
 * busy all the time, and at nearly every look of the governor another thread runs than at the
 * look before. Powersave's boost, -30: the busy share has to be all of the CPU for the frequency
 * to go up at all (1.5625 * 0.7 = 1.09 times the frequency it was measured at).
 */
struct pingpong {
	int in, out;
	volatile int *stop;
	long turns;
	pthread_t thread;
};

static void *pingpong_main(void *arg)
{
	struct pingpong *p = arg;
	char c;

	pin(1);
	while (!*p->stop && read(p->in, &c, 1) == 1) {
		spin_until(now_ns() + 200000, p->stop);
		if (write(p->out, &c, 1) != 1)
			break;
		p->turns++;
	}
	return NULL;
}

static void check_pingpong(void)
{
	volatile int stop = 0;
	struct pingpong a = { .stop = &stop }, b = { .stop = &stop };
	struct trace t;
	int ab[2], ba[2], up;
	char c = 0;

	set_governor(1, GOV);
	if (pipe(ab) || pipe(ba)) {
		ok(0, "two threads: no pipes (%s)", strerror(errno));
		return;
	}
	put(tunable(1, GOV, "refined"), "1");
	put(tunable(1, GOV, "boost"), "-30");
	wait_for(1, LOWEST, 4000);
	a.in = ba[0];
	a.out = ab[1];
	b.in = ab[0];
	b.out = ba[1];
	pthread_create(&a.thread, NULL, pingpong_main, &a);
	pthread_create(&b.thread, NULL, pingpong_main, &b);
	if (write(ba[1], &c, 1) != 1)
		stop = 1;
	up = wait_for(1, TOP, 3000);
	watch(1, 400, &t);
	stop = 1;
	/* (a thread that waits for the other one's byte gets the end of its pipe) */
	close(ab[1]);
	close(ba[1]);
	pthread_join(a.thread, NULL);
	pthread_join(b.thread, NULL);
	close(ab[0]);
	close(ba[0]);
	put(tunable(1, GOV, "boost"), "0");
	put(tunable(1, GOV, "refined"), "0");
	ok(up >= 0 && upper_after(&t, 0) == TOP,
	   "two threads that hand CPU 1 to each other every 200 us (%ld and %ld turns): with refined at 1 and a boost of -30 the policy is at the top after %d ms and stays there",
	   a.turns, b.turns, up);
	if (up < 0 || upper_after(&t, 0) != TOP)
		show(&t, "two threads");
	wait_for(1, LOWEST, 4000);
}

/*
 * Short-lived processes started on an idle CPU, as a script's "sleep" or "resetprop" on the phone:
 * a task just forked starts with half of its CPU's spare capacity as its utilisation, and a
 * governor that took that whenever the windows held nothing had the 13 Pro's middle cluster, left
 * alone, go from its floor to 1.4 GHz 76 times in 31 s. A thread allowed CPUs 1
 * and 2 forks a child every 50 ms (put on the idle one of the two), which spins 3 ms and exits:
 * with refined at 1 and a boost of -30 the policy stays at rest.
 */
static volatile int forks_stop;
static volatile long forks_made;

static void *forks_main(void *arg)
{
	cpu_set_t set;

	(void)arg;
	CPU_ZERO(&set);
	CPU_SET(1, &set);
	CPU_SET(2, &set);
	sched_setaffinity(0, sizeof(set), &set);
	while (!forks_stop) {
		pid_t child = fork();

		if (!child) {
			long long until = now_ns() + 3000000;

			while (now_ns() < until)
				;
			_exit(0);
		}
		if (child > 0) {
			waitpid(child, NULL, 0);
			forks_made++;
		}
		sleep_ms(50);
	}
	return NULL;
}

/* the policy's step above @freq (its table), @freq if there is none */
static long step_above(int policy, long freq)
{
	char buf[512], *s = buf, *end;
	long best = 0;

	get(path(policy, "scaling_available_frequencies"), buf, sizeof(buf));
	for (long f = strtol(s, &end, 10); end != s; s = end, f = strtol(s, &end, 10))
		if (f > freq && (!best || f < best))
			best = f;
	return best ? best : freq;
}

static void check_forks(void)
{
	struct load waker;
	pthread_t forker;
	struct trace t;
	int above = 0;
	long highest = 0, one_up = step_above(1, LOWEST);

	set_governor(1, GOV);
	put(tunable(1, GOV, "refined"), "1");
	put(tunable(1, GOV, "boost"), "-30");
	/*
	 * (another CPU of the policy has it worked out every millisecond, as the phone's cluster
	 * always has -- and what the record kept from the checks before, when refined was last on,
	 * rolls out first)
	 */
	start(&waker, 3, EVERY_MS);
	sleep_ms(200);
	wait_for(1, LOWEST, 4000);
	forks_stop = 0;
	forks_made = 0;
	pthread_create(&forker, NULL, forks_main, NULL);
	watch(1, 2000, &t);
	forks_stop = 1;
	pthread_join(forker, NULL);
	stop(&waker);
	for (int i = 0; i < t.n; i++) {
		above += t.freq[i] > LOWEST;
		highest = t.freq[i] > highest ? t.freq[i] : highest;
	}
	put(tunable(1, GOV, "boost"), "0");
	put(tunable(1, GOV, "refined"), "0");
	/*
	 * (a child that, with the fork before it, keeps a CPU busy all the last react_us is
	 * "saturated" and may take the policy a step up for a few ms: one step, not the scheduler's
	 * guess of half a CPU, which went to 1.1-1.5 GHz)
	 */
	ok(forks_made >= 20 && highest <= one_up && above * 10 <= t.n,
	   "%ld processes started on the shared policy's idle CPUs, 3 ms of work each: with refined at 1 and a boost of -30 never above %ld (highest %ld), %d of %d looks above the lowest frequency",
	   forks_made, one_up, highest, above, t.n);
	if (forks_made < 20 || highest > one_up || above * 10 > t.n)
		show(&t, "processes started");
	wait_for(1, LOWEST, 4000);
}

static void check_lists(void)
{
	static const struct { const char *file, *text; int error; } bad[] = {
		{ "target_loads", "1500000 50:900000 20", EINVAL },
		{ "target_loads", "900000 20:900000 50", EINVAL },
		{ "target_loads", "900000", EINVAL },
		{ "target_loads", "900000:20", EINVAL },
		{ "target_loads", "nothing", EINVAL },
		{ "target_loads", "\n", EINVAL },
		{ "target_loads", "1 1:2 1:3 1:4 1:5 1:6 1:7 1:8 1:9 1:10 1:11 1:12 1:13 1:14 1:15 1:16 1:17 1", EINVAL },
		{ "ravg_window_us", "0", ERANGE },
		{ "ravg_window_us", "999", ERANGE },
		{ "target_load_shift", "32", ERANGE },
		{ "enabled", "2", ERANGE },
		{ "up_rate_limit_us", "soon", EINVAL },
		{ "touch_ms", "60001", ERANGE },
		{ "floor", "low", EINVAL },
		{ "react_us", "999", ERANGE },
		{ "react_us", "1000001", ERANGE },
	};
	int wrong = 0, got = 0;
	const char *what = "";

	set_governor(1, GOV);
	for (unsigned int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		int err = put(tunable(1, GOV, bad[i].file), bad[i].text);

		if (err != bad[i].error && !wrong++) {
			what = bad[i].text;
			got = err;
		}
	}
	ok(!wrong, "%d writes that are not a list, not in order or out of range are refused (%d were not: \"%s\" gave error %d)",
	   (int)(sizeof(bad) / sizeof(bad[0])), wrong, what, got);
	ok(is(1, GOV, "target_loads", "0 0") && is(1, GOV, "touch_ms", "0") && is(1, GOV, "enabled", "1"), "and they left the list and the numbers as they were");
	ok(!put(tunable(1, GOV, "target_loads"), "900000 20:1500000 50\n") && is(1, GOV, "target_loads", "900000 20:1500000 50") &&
	   !put(tunable(1, GOV, "target_loads"), " 700000   2000 : 1100000 5 ") && is(1, GOV, "target_loads", "700000 1000:1100000 5"),
	   "target_loads: pairs with a colon between them are taken and read back, a percentage above 1000 is 1000");
	put(tunable(1, GOV, "target_loads"), "0 0");
}

/* --- what the 5.15 tuning module set along with the governor --- */

#define CPUCTL "/dev/cpuctl-sugov"
#define PELT "/proc/sys/kernel/sched_pelt_multiplier"

static int file_is(const char *file, const char *want)
{
	char buf[64];

	return !strcmp(get(file, buf, sizeof(buf)), want);
}

#define TRACING "/tmp/sugov-tracing"

/*
 * For a ceiling check that failed: 40 ms of what CPU 0 ran, and who was woken from there, while
 * a loop of the group with the ceiling keeps it busy once more.
 */
static void trace_capped(void)
{
	struct load busy;
	char line[400];
	FILE *f;
	int n = 0;

	mkdir(TRACING, 0755);
	if (mount("tracefs", TRACING, "tracefs", 0, NULL) && errno != EBUSY) {
		printf("sugov   (no tracefs: %s)\n", strerror(errno));
		return;
	}
	put(TRACING "/tracing_on", "0");
	put(TRACING "/events/sched/sched_switch/enable", "1");
	put(TRACING "/events/sched/sched_waking/enable", "1");
	put(TRACING "/trace", "\n");
	start_in(&busy, 0, BUSY, CPUCTL "/capped/tasks");
	sleep_ms(300);
	put(TRACING "/tracing_on", "1");
	sleep_ms(40);
	put(TRACING "/tracing_on", "0");
	stop(&busy);
	f = fopen(TRACING "/per_cpu/cpu0/trace", "r");
	while (f && fgets(line, sizeof(line), f) && n < 150)
		if (line[0] != '#') {
			line[strcspn(line, "\n")] = 0;
			printf("sugov   cpu0: %s\n", line);
			n++;
		}
	if (f)
		fclose(f);
	put(TRACING "/events/sched/sched_switch/enable", "0");
	put(TRACING "/events/sched/sched_waking/enable", "0");
}

/*
 * A group's ceiling, in a cpu cgroup mounted for this; and the scheduler's PELT multiplier, which the
 * 5.15 tuning module raised to 2 with the governor. (5.15's cpu.uclamp.min.multiplier is a patch of
 * that kernel's own, not on 6.18: not checked here.)
 */
static void check_uclamp(void)
{
	mkdir(CPUCTL, 0755);
	if (mount("cpu", CPUCTL, "cgroup", 0, "cpu") && errno != EBUSY) {
		ok(0, "the cpu cgroup cannot be mounted (%s)", strerror(errno));
		return;
	}

	/*
	 * A group's ceiling under schedutilmium, whose busy share replaces the utilisation the
	 * ceiling had been applied to. (Without the thread that keeps CPU 0's governor asked:
	 * whenever that one is runnable the run queue has no ceiling.)
	 */
	mkdir(CPUCTL "/capped", 0755);
	if (put(CPUCTL "/capped/cpu.uclamp.max", "20") || !set_governor(0, "schedutilmium")) {
		ok(0, "a group with a ceiling cannot be made, or schedutilmium not selected");
	} else {
		struct load busy;
		struct trace t;
		struct load rt;
		long capped, with_rt;
		int up, joined;

		stop(&tickle);
		start_in(&busy, 0, BUSY, CPUCTL "/capped/tasks");
		watch(0, 1500, &t);
		capped = upper_after(&t, 500);
		stop(&busy);
		joined = busy.joined;
		if (joined || capped > 700000) {
			printf("sugov   the loop's move into the group: error %d; what CPU 0's policy did meanwhile:\n", joined);
			show(&t, "capped");
			trace_capped();
		}
		/*
		 * The same with a real-time thread that wakes on that CPU every 3 ms. It has no
		 * ceiling, and a run queue's is the highest of its runnable tasks': each of its
		 * wake-ups is a moment the loop's ceiling is not there.
		 */
		start(&rt, 0, RT_TICKLE);
		start_in(&busy, 0, BUSY, CPUCTL "/capped/tasks");
		watch(0, 1500, &t);
		with_rt = upper_after(&t, 500);
		stop(&busy);
		stop(&rt);
		if (with_rt > 700000)
			show(&t, "capped, with a real-time thread");
		start(&busy, 0, BUSY);
		up = wait_for(0, TOP, 4000);
		stop(&busy);
		start(&tickle, 0, TICKLE);
		ok(capped > 0 && capped <= 700000 && up >= 0,
		   "schedutilmium keeps to a group's ceiling: a busy loop of a group with cpu.uclamp.max at 20 %% runs at %ld, one outside it at the top", capped);
		ok(!rt.joined && with_rt > 0 && with_rt <= 700000,
		   "and with a real-time thread that wakes on the same CPU every 3 ms: %ld (the thread was made real-time: error %d)", with_rt, rt.joined);
		rest();
	}

	ok(file_is(PELT, "1") && !put(PELT, "2") && file_is(PELT, "2") && put(PELT, "3") == EINVAL && !put(PELT, "1") && file_is(PELT, "1"),
	   "kernel.sched_pelt_multiplier takes 2 (the utilisation's half-life 16 ms instead of 32) and 1 again");
}

/*
 * boost: percent more of the utilisation, or less, before it becomes a frequency -- what a mode
 * is. With half of it taken away a busy loop no longer gets the top frequency; a half-busy
 * thread gets more with it doubled.
 */
static void check_boost(void)
{
	struct load busy;
	struct trace t;
	long low, high, more, plain;

	set_governor(0, GOV);
	ok(put(tunable(0, GOV, "boost"), "101") == ERANGE && put(tunable(0, GOV, "boost"), "-51") == ERANGE &&
	   put(tunable(0, GOV, "boost"), "more") == EINVAL && is(0, GOV, "boost", "0"),
	   "boost: more than 100, less than -50 and what is no number are refused");
	put(tunable(0, GOV, "boost"), "-50");
	rest();
	start(&busy, 0, BUSY);
	watch(0, 1200, &t);
	low = upper_after(&t, 600);
	put(tunable(0, GOV, "boost"), "0");
	high = wait_for(0, TOP, 3000) >= 0 ? TOP : cur(0);
	stop(&busy);
	rest();
	/* 1.25 * 1.25 * 0.5 of the top frequency */
	ok(low >= 1500000 && low <= 1700000 && high == TOP, "with a boost of -50 a busy loop runs at %ld, and at the top again with 0 (%ld)", low, high);
	/*
	 * a thread busy half of the time (on the shared policy, as above): more with twice the
	 * utilisation. (With enabled at 0: its busy share has such a thread near the top as it is.)
	 */
	set_governor(1, GOV);
	put(tunable(1, GOV, "enabled"), "0");
	plain = half_busy(NULL);
	put(tunable(1, GOV, "boost"), "100");
	more = half_busy(NULL);
	put(tunable(1, GOV, "boost"), "0");
	put(tunable(1, GOV, "enabled"), "1");
	ok(more > plain, "with a boost of 100 a thread busy half of the time gets %ld (with enabled at 0), %ld without", more, plain);
	rest();
}

/* a screen to touch: a device that reports the position of fingers, made through uinput */
static int touch_open(void)
{
	struct uinput_setup setup = { .id = { .bustype = BUS_VIRTUAL }, .name = "sugov-test-touch" };
	struct uinput_abs_setup abs = { .code = ABS_MT_POSITION_X, .absinfo = { .maximum = 4095 } };
	int fd;

	mknod("/dev/uinput", S_IFCHR | 0600, makedev(10, 223));
	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	if (ioctl(fd, UI_SET_EVBIT, EV_ABS) || ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X) || ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y) ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID) || ioctl(fd, UI_ABS_SETUP, &abs))
		goto bad;
	abs.code = ABS_MT_POSITION_Y;
	if (ioctl(fd, UI_ABS_SETUP, &abs))
		goto bad;
	abs.code = ABS_MT_TRACKING_ID;
	abs.absinfo.minimum = -1;
	abs.absinfo.maximum = 65535;
	if (ioctl(fd, UI_ABS_SETUP, &abs) || ioctl(fd, UI_DEV_SETUP, &setup) || ioctl(fd, UI_DEV_CREATE))
		goto bad;
	return fd;
bad:
	close(fd);
	return -1;
}

/* a finger put down on the screen (a new contact, @id) at @at, or lifted again (@id -1) */
static int touch_contact(int fd, int id, int at)
{
	struct input_event ev[4] = {
		{ .type = EV_ABS, .code = ABS_MT_TRACKING_ID, .value = id },
		{ .type = EV_ABS, .code = ABS_MT_POSITION_X, .value = at },
		{ .type = EV_ABS, .code = ABS_MT_POSITION_Y, .value = at },
		{ .type = EV_SYN, .code = SYN_REPORT },
	};

	return write(fd, ev, sizeof(ev)) == sizeof(ev);
}

static int touch(int fd, int at)
{
	struct input_event ev[3] = {
		{ .type = EV_ABS, .code = ABS_MT_POSITION_X, .value = at },
		{ .type = EV_ABS, .code = ABS_MT_POSITION_Y, .value = at },
		{ .type = EV_SYN, .code = SYN_REPORT },
	};

	return write(fd, ev, sizeof(ev)) == sizeof(ev);
}

/*
 * The two floors: no request is below "floor"; for touch_ms after a touch none is below
 * "touch_floor" either, and with that over the policy is back at rest.
 */
static void check_floors(void)
{
	int fd = touch_open();
	int up, down, held, touched, over;
	struct trace t;

	set_governor(0, GOV);
	rest();
	put(tunable(0, GOV, "floor"), "1000000");
	up = wait_for(0, 1100000, 1000);
	put(tunable(0, GOV, "floor"), "0");
	down = rest();
	ok(up >= 0 && down >= 0, "with floor at 1000000 the policy rests at the step above it (there after %d ms), and at the lowest again with 0", up);
	if (fd < 0) {
		ok(0, "no screen to touch (uinput)");
		return;
	}
	put(tunable(0, GOV, "touch_floor"), "1500000");
	put(tunable(0, GOV, "touch_ms"), "600");
	/*
	 * (200 ms without a touch: never at the touch floor. Not "at the lowest": a moment's work of
	 * the kernel's own on CPU 0 can take it a step up, the busy share counting all that runs there)
	 */
	sleep_ms(100);
	watch(0, 200, &t);
	held = first_above(&t, 1400000) < 0;
	touch(fd, 300);
	touched = wait_for(0, 1500000, 300);
	over = wait_for(0, LOWEST, 4000);
	ok(held && touched >= 0 && over >= 350 && over <= 2000,
	   "with touch_floor at 1500000 nothing is asked for until the screen (one made through uinput) is touched; then the policy is there after %d ms and at rest again %d ms later (touch_ms 600)%s",
	   touched, over, held ? "" : "; before the touch:");
	if (!held)
		show(&t, "before the touch");
	put(tunable(0, GOV, "touch_floor"), "0");
	put(tunable(0, GOV, "touch_ms"), "0");
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	rest();
}

/*
 * The governor's own ceiling: under a busy loop no request is above it -- one between two steps
 * gets the step under it --, and the policy's maximum stays where it was (that is the
 * difference from scaling_max_freq: the scheduler takes the CPUs' sizes from the maximum).
 * With 0 the policy is at the top again.
 */
static void check_ceiling(void)
{
	struct load busy;
	int up, down, back;
	long max;

	set_governor(0, GOV);
	start(&busy, 0, BUSY);
	up = wait_for(0, TOP, 4000);
	put(tunable(0, GOV, "ceiling"), "1150000");
	down = wait_for(0, 1100000, 1000);
	max = number(path(0, "scaling_max_freq"));
	put(tunable(0, GOV, "ceiling"), "0");
	back = wait_for(0, TOP, 1000);
	stop(&busy);
	ok(up >= 0 && down >= 0 && back >= 0 && max == TOP,
	   "a ceiling of 1150000 under a busy loop has the policy at the step under it after %d ms, its maximum still %ld; at the top again %d ms after 0",
	   down, max, back);
	rest();
}

/*
 * caps, the ceiling that follows the load (a rule first run in a user-space program on the 5.15
 * kernel): a busy loop from rest is held at the floor's step for the first windows of 320 ms and
 * let up step by step over seconds; a finger put down lifts the ceiling to caps_touch for
 * caps_touch_ms, a process moved into a cpuset group named top-app to caps_front_pct % of the top
 * for caps_front_ms; caps_max holds the rule's ceiling but not a lift, caps_burst holds it for a
 * load that has not lasted caps_burst_ms; "ceiling", written, takes the
 * place of all of it; caps_state says where the rule stands.
 */
#define CPUSET "/dev/cpuset-sugov"

/* the highest frequency of the looks from @from_ms to @to_ms */
static long highest(const struct trace *t, int from_ms, int to_ms)
{
	long top = 0;

	for (int i = 0; i < t->n; i++)
		if (t->ms[i] >= from_ms && t->ms[i] < to_ms && t->freq[i] > top)
			top = t->freq[i];
	return top;
}

/* a busy loop on CPU 0 from rest, held by the rule (the ceilings' smoothed demand at nothing first) */
static void caps_from_rest(struct load *busy)
{
	rest();
	/* (at rest a few seconds: the windows that end meanwhile are samples of nothing) */
	sleep_ms(3000);
	start(busy, 0, BUSY);
}

static void check_caps(void)
{
	int fd = touch_open(), id = 1, held, lifted;
	long late, back, capped, over, lasted, busy_ms;
	char state[256];
	struct load busy, front;
	struct trace t;

	set_governor(0, GOV);
	put(tunable(0, GOV, "caps_floor"), "900000");
	put(tunable(0, GOV, "caps_front_ms"), "0");
	put(tunable(0, GOV, "caps"), "1");

	/* held: nothing above the floor's step for a while, then up as the windows say "busy" */
	caps_from_rest(&busy);
	watch(0, 4500, &t);
	held = first_above(&t, 900000);
	late = highest(&t, 4000, 4500);
	get(tunable(0, GOV, "caps_state"), state, sizeof(state));
	stop(&busy);
	state[strcspn(state, "\n")] = 0;
	ok((held < 0 || held >= 300) && late >= 1500000 && !strncmp(state, "policy0 smooth ", 15) && strstr(state, " top 2000000"),
	   "caps: a busy loop from rest stays at the floor's step (900000) until %d ms, and the ceiling opens over seconds: at %ld in its last half second (caps_state \"%s\")",
	   held, late, state);
	if (!((held < 0 || held >= 300) && late >= 1500000))
		show(&t, "caps from rest");

	/*
	 * A finger put down: lifted for caps_touch_ms -- here to the top, where nothing else takes
	 * it within that second (the smoothed demand of the loop before has not gone down all the
	 * way in 3 s, and the rule lets it up to 1300000 at most by then) -- then held again.
	 */
	if (fd < 0) {
		ok(0, "caps: no screen to touch (uinput)");
	} else {
		put(tunable(0, GOV, "caps_touch"), "2000000");
		put(tunable(0, GOV, "caps_touch_ms"), "300");
		caps_from_rest(&busy);
		sleep_ms(150);
		touch_contact(fd, id++, 500);
		watch(0, 700, &t);
		touch_contact(fd, -1, 500);
		lifted = first_above(&t, 1700000);
		back = highest(&t, 450, 700);
		stop(&busy);
		ok(lifted >= 0 && lifted <= 80 && back < TOP,
		   "caps: a finger put down lifts the ceiling to the top after %d ms; 450 ms later it is held again (%ld at most; caps_touch_ms 300)", lifted,
		   back);
		if (!(lifted >= 0 && lifted <= 80 && back < TOP))
			show(&t, "caps touched");
		put(tunable(0, GOV, "caps_touch_ms"), "0");
	}

	/*
	 * a process moved into the cpuset top-app (its pid to cgroup.procs, as Android brings an app
	 * to the front): lifted to caps_front_pct % of the top (here 100: none); one thread moved on
	 * its own (the tasks file) first, which is not. The rule's own ceiling is held at 1100000
	 * meanwhile (caps_burst, for longer than the loop lasts: what goes above it is the lift, which
	 * caps_burst does not hold -- and the smoothed demand of a loop half a second old, which can
	 * be at the top by then, does not count). Mounted as Android and the UX rules' test mount it
	 * (noprefix): a hierarchy that is still there when that test mounts its own is taken as it is,
	 * the new options ignored.
	 */
	mkdir(CPUSET, 0755);
	if (mount("cpuset", CPUSET, "cgroup", 0, "cpuset,noprefix")) {
		ok(0, "caps: the cpuset cgroup cannot be mounted (%s)", strerror(errno));
	} else {
		char buf[32];
		int i, err = 0, moved = -1;
		long thread_high;
		pid_t child;

		mkdir(CPUSET "/top-app", 0755);
		put(CPUSET "/top-app/cpus", get(CPUSET "/cpus", buf, sizeof(buf)));
		put(CPUSET "/top-app/mems", get(CPUSET "/mems", buf, sizeof(buf)));
		put(tunable(0, GOV, "caps_front_pct"), "100");
		put(tunable(0, GOV, "caps_front_ms"), "300");
		put(tunable(0, GOV, "caps_burst_ms"), "60000");
		put(tunable(0, GOV, "caps_burst"), "1100000");
		caps_from_rest(&busy);
		sleep_ms(150);
		/* a thread on its own: no lift */
		start_in(&front, 1, TICKLE, CPUSET "/top-app/tasks");
		watch(0, 400, &t);
		stop(&front);
		thread_high = highest(&t, 0, 400);
		ok(!front.joined && thread_high > 0 && thread_high <= 1100000,
		   "caps: a thread moved into cpuset top-app on its own does not lift the ceiling (%ld at most in 400 ms, the rule's held at 1100000; joined: %s)",
		   thread_high, front.joined ? strerror(front.joined) : "yes");
		if (!(thread_high > 0 && thread_high <= 1100000))
			show(&t, "caps front, a thread");
		/* a process: lifted, and held again caps_front_ms after */
		child = fork();
		if (child == 0) {
			char me[16];

			snprintf(me, sizeof(me), "%d", (int)getpid());
			if (put(CPUSET "/top-app/cgroup.procs", me))
				_exit(1);
			pin(1);
			for (;;)
				sleep_ms(10);
		}
		watch(0, 700, &t);
		if (child > 0) {
			kill(child, SIGKILL);
			waitpid(child, &moved, 0);
		}
		lifted = first_above(&t, 1700000);
		back = highest(&t, 450, 700);
		stop(&busy);
		put(tunable(0, GOV, "caps_front_ms"), "0");
		put(tunable(0, GOV, "caps_front_pct"), "85");
		put(tunable(0, GOV, "caps_burst"), "0");
		put(tunable(0, GOV, "caps_burst_ms"), "1000");
		/*
		 * gone again before the UX rules' test makes a cpuset of its own: a thread that has been
		 * joined can be in its group a moment longer (it leaves when it is released), and a group
		 * that cannot be removed keeps the hierarchy after the umount
		 */
		for (i = 0; rmdir(CPUSET "/top-app"); i++) {
			if (errno != EBUSY || i == 200) {
				err = errno;
				break;
			}
			sleep_ms(10);
		}
		if (!err && umount(CPUSET))
			err = errno;
		rmdir(CPUSET);
		/* (killed, not exited: a child that could not join would have exited 1 before) */
		ok(child > 0 && WIFSIGNALED(moved) && lifted >= 0 && lifted <= 80 && back > 0 && back <= 1100000 && !err,
		   "caps: a process moved into cpuset top-app lifts the ceiling to the top after %d ms, past caps_burst; 450 ms later it is held again (%ld at most; caps_front_ms 300; joined: %s; taken down again: %s)",
		   lifted, back, child > 0 && WIFSIGNALED(moved) ? "yes" : "no", err ? strerror(err) : "yes");
		if (!(lifted >= 0 && lifted <= 80 && back > 0 && back <= 1100000))
			show(&t, "caps front");
	}

	/*
	 * caps_max: the rule's ceiling no higher (a loop that has opened it is held at 1100000), but a
	 * lift goes past it; "ceiling", written, takes the place of all of it, the lift included
	 */
	start(&busy, 0, BUSY);
	sleep_ms(4000);
	put(tunable(0, GOV, "caps_max"), "1100000");
	watch(0, 300, &t);
	capped = highest(&t, 100, 300);
	over = -1;
	if (fd >= 0) {
		put(tunable(0, GOV, "caps_touch"), "1500000");
		put(tunable(0, GOV, "caps_touch_ms"), "300");
		touch_contact(fd, id++, 500);
		watch(0, 200, &t);
		touch_contact(fd, -1, 500);
		over = highest(&t, 0, 200);
		sleep_ms(400);
		put(tunable(0, GOV, "ceiling"), "700000");
		touch_contact(fd, id++, 500);
		watch(0, 200, &t);
		touch_contact(fd, -1, 500);
		late = highest(&t, 20, 200);
		put(tunable(0, GOV, "ceiling"), "0");
		put(tunable(0, GOV, "caps_touch_ms"), "0");
	}
	stop(&busy);
	put(tunable(0, GOV, "caps_max"), "0");
	ok(capped == 1100000 && over >= 1500000 && late == 700000,
	   "caps: with caps_max 1100000 an opened ceiling holds there (%ld), a finger put down takes it past (%ld); a ceiling of 700000 written holds even then (%ld)",
	   capped, over, late);

	/*
	 * caps_burst: a load that has lasted goes where the rule lets it; one that has only begun --
	 * the loop again after 150 ms of rest, the rule's ceiling still open -- is held at caps_burst
	 * for caps_burst_ms (here 400) and let up after
	 */
	put(tunable(0, GOV, "caps_burst_ms"), "400");
	put(tunable(0, GOV, "caps_burst"), "1100000");
	start(&busy, 0, BUSY);
	sleep_ms(4000);
	watch(0, 200, &t);
	lasted = highest(&t, 0, 200);
	get(tunable(0, GOV, "caps_state"), state, sizeof(state));
	stop(&busy);
	state[strcspn(state, "\n")] = 0;
	busy_ms = strstr(state, " busy_ms ") ? atol(strstr(state, " busy_ms ") + 9) : -1;
	sleep_ms(150);
	start(&busy, 0, BUSY);
	watch(0, 800, &t);
	stop(&busy);
	capped = highest(&t, 60, 380);
	over = highest(&t, 560, 800);
	put(tunable(0, GOV, "caps_burst"), "0");
	put(tunable(0, GOV, "caps_burst_ms"), "1000");
	ok(lasted > 1100000 && busy_ms >= 3000 && capped > 0 && capped <= 1100000 && over > 1100000,
	   "caps: with caps_burst 1100000 a load that has lasted is let up (%ld; busy for %ld ms by caps_state), one that has only begun is held there (%ld at most from 60 to 380 ms) and let up after caps_burst_ms 400 (%ld from 560 ms)",
	   lasted, busy_ms, capped, over);
	if (!(lasted > 1100000 && capped > 0 && capped <= 1100000 && over > 1100000))
		show(&t, "caps burst");

	/* off: the loop at the top as without */
	put(tunable(0, GOV, "caps"), "0");
	rest();
	start(&busy, 0, BUSY);
	lifted = wait_for(0, TOP, 1000);
	stop(&busy);
	ok(lifted >= 0, "caps: turned off, a busy loop is at the top after %d ms", lifted);
	put(tunable(0, GOV, "caps_floor"), "0");
	put(tunable(0, GOV, "caps_touch"), "0");
	put(tunable(0, GOV, "caps_front_ms"), "2000");
	if (fd >= 0) {
		ioctl(fd, UI_DEV_DESTROY);
		close(fd);
	}
	rest();
}

/*
 * I/O waits. A thread on CPU 0 reads 4 KB at a time, O_DIRECT, from a loop device over a file in
 * /tmp: four reads every 4 ms, and each read ends in a wake-up after an I/O wait. The loop
 * device's worker copies the data on the other CPUs -- the unbound workqueues are kept off CPU 0
 * meanwhile. Under QEMU a read costs a few hundred microseconds of CPU 0's time, a quarter of it
 * in all: its busy share alone takes it to 1.1 or 1.3 GHz at times, seldom higher (0-6 % of the
 * time above 1.3 GHz). With iowait_boost at 0 (the default) that is all; at 1 schedutil's boost, doubled at each such
 * wake-up within a tick, has it at 1.7 GHz or the top a good part of the time (how far up it
 * gets depends on how the wake-ups fall between the governor's updates: two runs had 19 % and
 * 70 % of the time at the top).
 */
#define IO_FILE		(1 << 20)
#define WQ_MASK		"/sys/devices/virtual/workqueue/cpumask"

struct io_reader {
	pthread_t thread;
	volatile int stop;
	int fd, failed;
	long reads;
	char *buf;
};

static void *io_main(void *arg)
{
	struct io_reader *r = arg;
	off_t off = 0;

	pin(0);
	while (!r->stop) {
		for (int i = 0; i < 4; i++) {
			if (pread(r->fd, r->buf, 4096, off) != 4096) {
				r->failed = errno ? errno : EIO;
				return NULL;
			}
			r->reads++;
			off = (off + 4096) % IO_FILE;
		}
		sleep_ms(4);
	}
	return NULL;
}

/* how many of the looks after @from_ms were above @freq, in percent (-1: none) */
static int share_above(const struct trace *t, long freq, int from_ms)
{
	int n = 0, above = 0;

	for (int i = 0; i < t->n; i++)
		if (t->ms[i] >= from_ms) {
			n++;
			above += t->freq[i] > freq;
		}
	return n ? above * 100 / n : -1;
}

/*
 * caps_ui_ms: what a process forked for it does on CPU 0 -- in the cpuset top-app or not, its main
 * thread or a thread of the name given, spinning @spin_us and sleeping @sleep_us in turn for
 * @total_ms
 */
struct ui_run {
	int top;
	const char *name;
	int spin_us, sleep_us, total_ms;
};

static void *ui_loop(void *arg)
{
	const struct ui_run *r = arg;
	long long end = now_ns() + r->total_ms * 1000000ll;
	volatile int never = 0;

	if (r->name)
		prctl(PR_SET_NAME, r->name);
	pin(0);
	while (now_ns() < end) {
		spin_until(now_ns() + r->spin_us * 1000ll, &never);
		if (r->sleep_us) {
			struct timespec ts = { 0, r->sleep_us * 1000l };

			nanosleep(&ts, NULL);
		}
	}
	return NULL;
}

/* the run in a child of its own while CPU 0's frequency is watched for @watch_ms; the child's status */
static int ui_watch(const struct ui_run *r, int watch_ms, struct trace *t)
{
	pid_t child = fork();
	int status = -1;

	if (child == 0) {
		char me[16];
		pthread_t th;

		snprintf(me, sizeof(me), "%d", (int)getpid());
		if (r->top && put(CPUSET "/top-app/cgroup.procs", me))
			_exit(2);
		if (r->name) {
			pin(1);
			if (pthread_create(&th, NULL, ui_loop, (void *)r))
				_exit(3);
			pthread_join(th, NULL);
		} else {
			ui_loop((void *)r);
		}
		_exit(0);
	}
	watch(0, watch_ms, t);
	if (child > 0)
		waitpid(child, &status, 0);
	return status;
}

/* how often a burst lifted policy 0's ceiling, by caps_state */
static long ui_lifts(void)
{
	char state[256];
	const char *at;

	get(tunable(0, GOV, "caps_state"), state, sizeof(state));
	at = strstr(state, " ui_lifts ");
	return at ? atol(at + 10) : -1;
}

static void check_ui(void)
{
	static const struct ui_run main_busy = { 1, NULL, 2000, 0, 300 }, render = { 1, "RenderThread", 2000, 0, 300 },
				   worker = { 1, "worker", 2000, 0, 300 }, elsewhere = { 0, NULL, 2000, 0, 300 },
				   short_ = { 1, NULL, 5000, 5000, 300 }, sleepy = { 1, NULL, 3000, 300, 300 };
	struct load busy;
	struct trace t, after;
	char buf[32];
	long l0, l1, l2, high, held, rt_high, w_high, e_high, s_high, z_high;
	int first, rt_first, z_first, st[6], err = 0, i;

	set_governor(0, GOV);
	put(tunable(0, GOV, "caps_floor"), "900000");
	put(tunable(0, GOV, "caps_front_ms"), "0");
	put(tunable(0, GOV, "caps_front_pct"), "85");
	put(tunable(0, GOV, "caps_burst_ms"), "60000");
	put(tunable(0, GOV, "caps_burst"), "1100000");
	put(tunable(0, GOV, "caps"), "1");
	mkdir(CPUSET, 0755);
	if (mount("cpuset", CPUSET, "cgroup", 0, "cpuset,noprefix")) {
		ok(0, "caps_ui_ms: the cpuset cgroup cannot be mounted (%s)", strerror(errno));
		goto out;
	}
	mkdir(CPUSET "/top-app", 0755);
	put(CPUSET "/top-app/cpus", get(CPUSET "/cpus", buf, sizeof(buf)));
	put(CPUSET "/top-app/mems", get(CPUSET "/mems", buf, sizeof(buf)));

	/* off (0, the default): the main thread of a process in top-app held as any other */
	rest();
	st[0] = ui_watch(&main_busy, 350, &t);
	z_first = first_above(&t, 1100000);
	z_high = highest(&t, 0, 350);
	ok(st[0] == 0 && z_high > 0 && z_high <= 1100000,
	   "caps_ui_ms: 0, a busy main thread of a process in top-app is held at caps_burst (%ld at most, above it from %d ms; the child %d)", z_high,
	   z_first, st[0]);

	/* 8: lifted to caps_front_pct % of the top (the step 1700000), and held again soon after */
	put(tunable(0, GOV, "caps_ui_ms"), "8");
	rest();
	l0 = ui_lifts();
	st[1] = ui_watch(&main_busy, 320, &t);
	start(&busy, 0, BUSY);
	watch(0, 150, &after);
	stop(&busy);
	l1 = ui_lifts();
	first = first_above(&t, 1100000);
	high = highest(&t, 0, 320);
	held = highest(&after, 40, 150);
	ok(st[1] == 0 && first >= 0 && first <= 80 && high == 1700000 && held > 0 && held <= 1100000 && l1 > l0,
	   "caps_ui_ms: 8, a busy main thread of a process in top-app lifts the ceiling after %d ms, to %ld (caps_front_pct 85); "
	   "an ordinary thread after it is held again (%ld at most from 40 ms; lifts %ld -> %ld; the child %d)",
	   first, high, held, l0, l1, st[1]);
	if (!(first >= 0 && first <= 80 && high == 1700000))
		show(&t, "caps_ui_ms main");
	if (!(held > 0 && held <= 1100000))
		show(&after, "caps_ui_ms after");

	/* HWUI's RenderThread of it too; another thread of it, or a process elsewhere, not */
	rest();
	st[2] = ui_watch(&render, 350, &t);
	rt_first = first_above(&t, 1100000);
	rt_high = highest(&t, 0, 350);
	rest();
	st[3] = ui_watch(&worker, 350, &t);
	w_high = highest(&t, 0, 350);
	rest();
	st[4] = ui_watch(&elsewhere, 350, &t);
	e_high = highest(&t, 0, 350);
	l2 = ui_lifts();
	ok(st[2] == 0 && rt_first >= 0 && rt_first <= 80 && rt_high == 1700000 && st[3] == 0 && w_high > 0 && w_high <= 1100000 && st[4] == 0 &&
	   e_high > 0 && e_high <= 1100000,
	   "caps_ui_ms: a busy RenderThread lifts it too (after %d ms, to %ld); a thread of another name in top-app does not (%ld at most), nor the main thread of a process outside it (%ld)",
	   rt_first, rt_high, w_high, e_high);

	/* bursts of 5 ms every 10 ms are none (nothing above caps_burst); 3 ms with sleeps of 0.3 ms between are one */
	rest();
	st[5] = ui_watch(&short_, 350, &t);
	s_high = highest(&t, 0, 350);
	rest();
	l1 = ui_lifts();
	i = ui_watch(&sleepy, 350, &t);
	first = first_above(&t, 1100000);
	high = highest(&t, 0, 350);
	ok(st[5] == 0 && s_high > 0 && s_high <= 1100000 && i == 0 && first >= 0 && first <= 100 && high == 1700000 && ui_lifts() > l1 && l2 >= 0,
	   "caps_ui_ms: a main thread in top-app that runs 5 ms of every 10 is held (%ld at most); one that runs 3 ms and sleeps 0.3 ms in turn lifts it after %d ms, to %ld",
	   s_high, first, high);
	if (!(first >= 0 && first <= 100 && high == 1700000))
		show(&t, "caps_ui_ms sleepy");

	for (i = 0; rmdir(CPUSET "/top-app"); i++) {
		if (errno != EBUSY || i == 200) {
			err = errno;
			break;
		}
		sleep_ms(10);
	}
	if (!err && umount(CPUSET))
		err = errno;
	rmdir(CPUSET);
	ok(!err, "caps_ui_ms: the cpuset taken down again (%s)", err ? strerror(err) : "yes");
out:
	put(tunable(0, GOV, "caps_ui_ms"), "0");
	put(tunable(0, GOV, "caps"), "0");
	put(tunable(0, GOV, "caps_floor"), "0");
	put(tunable(0, GOV, "caps_burst"), "0");
	put(tunable(0, GOV, "caps_burst_ms"), "1000");
	put(tunable(0, GOV, "caps_front_ms"), "2000");
}

static void check_iowait(int cpus)
{
	static const char img[] = "/tmp/iowait.img", node[] = "/dev/loop7";
	struct io_reader r = { .fd = -1 };
	struct trace t;
	char was_mask[64] = "", mask[32], *data;
	int file, loop = -1, share[2] = { -1, -1 };
	long reads[2] = { 0, 0 };

	set_governor(0, GOV);
	ok(is(0, GOV, "iowait_boost", "0") && put(tunable(0, GOV, "iowait_boost"), "2") == ERANGE,
	   "iowait_boost: 0 unless written, and 2 is refused");
	/* the file, and the loop device over it (no devtmpfs here: the node is made) */
	file = open(img, O_RDWR | O_CREAT | O_TRUNC, 0600);
	data = malloc(IO_FILE);
	if (file >= 0 && data) {
		memset(data, 0x5a, IO_FILE);
		if (write(file, data, IO_FILE) != IO_FILE) {
			close(file);
			file = -1;
		}
	}
	free(data);
	if (file >= 0 && access(node, F_OK))
		mknod(node, S_IFBLK | 0600, makedev(7, 7));
	if (file >= 0)
		loop = open(node, O_RDWR);
	if (loop < 0 || ioctl(loop, LOOP_SET_FD, file)) {
		ok(0, "I/O waits: no loop device over %s (%s)", img, strerror(errno));
		goto out;
	}
	r.fd = open(node, O_RDONLY | O_DIRECT);
	if (r.fd < 0 || posix_memalign((void **)&r.buf, 4096, 4096)) {
		ok(0, "I/O waits: %s cannot be read with O_DIRECT (%s)", node, strerror(errno));
		goto out;
	}
	/* the unbound workqueues -- the loop device's worker among them -- off CPU 0 */
	get(WQ_MASK, was_mask, sizeof(was_mask));
	snprintf(mask, sizeof(mask), "%x", ((1u << cpus) - 1) & ~1u);
	put(WQ_MASK, mask);
	for (int on = 0; on < 2 && !r.failed; on++) {
		put(tunable(0, GOV, "iowait_boost"), on ? "1" : "0");
		rest();
		r.stop = 0;
		r.reads = 0;
		pthread_create(&r.thread, NULL, io_main, &r);
		watch(0, 2000, &t);
		r.stop = 1;
		pthread_join(r.thread, NULL);
		share[on] = share_above(&t, 1300000, 200);
		reads[on] = r.reads;
		show(&t, on ? "iowait_boost 1" : "iowait_boost 0");
	}
	put(tunable(0, GOV, "iowait_boost"), "0");
	if (was_mask[0])
		put(WQ_MASK, was_mask);
	/* (with it on, 22 to 39 % in four runs -- QEMU's timing; with it off 0 or 1) */
	ok(!r.failed && share[0] >= 0 && share[0] <= 12 && share[1] >= 15,
	   "a thread on CPU 0 that waits for I/O four times every 4 ms (%ld and %ld reads%s%s): above 1.3 GHz %d %% of the time with iowait_boost 0, %d %% with 1",
	   reads[0], reads[1], r.failed ? ", then " : "", r.failed ? strerror(r.failed) : "", share[0], share[1]);
out:
	if (r.fd >= 0)
		close(r.fd);
	free(r.buf);
	if (loop >= 0) {
		ioctl(loop, LOOP_CLR_FD, 0);
		close(loop);
	}
	if (file >= 0)
		close(file);
	unlink(img);
	rest();
}

/*
 * A cluster that loses all its CPUs and gets them back: the shared policy's. The governor's
 * ->exit() runs with the last CPU and its ->init() with the first one back; the tunables are
 * to be what was written, not the defaults.
 */
static void check_hotplug(int cpus)
{
	static const char *const name[] = { "target_loads", "touch_floor", "touch_ms", "up_rate_limit_us", "boost", "react_us", "caps_burst",
					    "caps_burst_ms" };
	static const char *const value[] = { "900000 20:1500000 50", "1100000", "250", "700", "-15", "6000", "1100000", "400" };
	static const char *const fresh[] = { "0 0", "0", "0", "500", "-20", "4000", "1536000", "1000" };
	const int names = (int)(sizeof(name) / sizeof(name[0]));
	char file[64], buf[64];
	int set = 1, gone, back = 1, same = 1, anew = 1;

	/* (this thread looks from the last CPU, which is one of them) */
	pin(0);
	set_governor(1, GOV);
	for (int i = 0; i < names; i++)
		set &= !put(tunable(1, GOV, name[i]), value[i]);
	for (int cpu = 1; cpu < cpus; cpu++) {
		snprintf(file, sizeof(file), "/sys/devices/system/cpu/cpu%d/online", cpu);
		set &= !put(file, "0");
	}
	gone = access(tunable(1, GOV, "target_loads"), F_OK) != 0;
	for (int cpu = 1; cpu < cpus; cpu++) {
		snprintf(file, sizeof(file), "/sys/devices/system/cpu/cpu%d/online", cpu);
		back &= !put(file, "1");
	}
	back &= !strcmp(get(path(1, "scaling_governor"), buf, sizeof(buf)), GOV);
	for (int i = 0; i < names; i++)
		same &= is(1, GOV, name[i], value[i]);
	ok(set && gone && back && same,
	   "the shared policy's CPUs all offline and back: its governor is " GOV " again and its tunables are what was written (set %d, files gone meanwhile %d, back %d, the same %d)",
	   set, gone, back, same);
	/* a change of governor is not that */
	set_governor_raw(1, "performance");
	set_governor_raw(1, GOV);
	for (int i = 0; i < names; i++)
		anew &= is(1, GOV, name[i], fresh[i]);
	ok(anew, "after a change of governor they are the built-in values of the second cluster");
	pin(cpus - 1);
}

/* the windows CPU @cpu has completed, from /proc/slim_sched/walt_busy; -1 if its line is not there */
static long long slim_windows(int cpu, int *lines, long *percent)
{
	FILE *f = fopen("/proc/slim_sched/walt_busy", "r");
	long long windows = -1, w;
	unsigned long share;
	unsigned int avg, most;
	long pct;
	int c;

	*lines = 0;
	if (!f)
		return -1;
	while (fscanf(f, "%d %ld %lu %u %u %lld", &c, &pct, &share, &avg, &most, &w) == 6) {
		++*lines;
		if (c == cpu) {
			windows = w;
			*percent = pct;
		}
	}
	fclose(f);
	return windows;
}

/* the windows CPU 0 completes in 300 ms of a busy loop (a CPU at rest has no tick to sample) */
static long long slim_windows_busy(long *percent)
{
	struct load busy;
	long long before, after;
	int lines;

	sleep_ms(60);
	before = slim_windows(0, &lines, percent);
	start(&busy, 0, BUSY);
	sleep_ms(300);
	after = slim_windows(0, &lines, percent);
	stop(&busy);
	return after - before;
}

static void check_slim(int cpus)
{
	long long with, without, again, refined, other;
	long percent = 0, unused;
	int lines;
	char buf[32];

	/* (the shared policy with another governor: nobody else holds the sampling) */
	set_governor(0, GOV);
	set_governor(1, "performance");
	slim_windows(0, &lines, &unused);
	ok(lines == cpus && !strcmp(get("/proc/slim_sched/walt_ctrl", buf, sizeof(buf)), "1") && number("/proc/slim_sched/window_ms") == 20 &&
	   number("/sys/module/slim_walt/parameters/slim_walt_window_ms") == 20,
	   "slim WALT: its files are there, a line for each of %d CPUs, a window of 20 ms", cpus);
	with = slim_windows_busy(&percent);
	ok(with >= 8 && with <= 16 && percent >= 80, "slim WALT: %lld windows of CPU 0 in 300 ms of a busy loop under schedutilmium, the last %ld %% busy", with, percent);
	/* nobody reads it: no tick is looked at */
	put(tunable(0, "schedutilmium", "use_slim_walt"), "0");
	without = slim_windows_busy(&unused);
	put(tunable(0, "schedutilmium", "use_slim_walt"), "1");
	again = slim_windows_busy(&unused);
	put(tunable(0, "schedutilmium", "refined"), "1");
	refined = slim_windows_busy(&unused);
	put(tunable(0, "schedutilmium", "refined"), "0");
	set_governor(0, "performance");
	other = slim_windows_busy(&unused);
	ok(!without && again >= 8 && !refined && !other,
	   "slim WALT: the tick is sampled only while it is read: %lld windows with use_slim_walt at 0, %lld with 1 again, %lld with refined at 1, %lld under another governor",
	   without, again, refined, other);
}

static void check_shared(const char *governor)
{
	struct load busy;
	struct trace t;
	long low;
	int up;

	set_governor(0, governor);
	set_governor(1, governor);
	rest();
	start(&busy, 2, BUSY);
	up = wait_for(1, TOP, 4000);
	/* (300 ms of looks, not one: a moment's work of the kernel's own on CPU 0 is not the point) */
	watch(0, 300, &t);
	low = upper_after(&t, 0);
	ok(up >= 0 && low <= 700000, "%s on the policy the other CPUs share: a busy loop on CPU 2 takes it to the top in %d ms, CPU 0's stays at %ld",
	   governor, up, low);
	if (low > 700000)
		show(&t, "CPU 0 meanwhile");
	stop(&busy);
}

/* --- 6.18 --- */

/* the governor's files, and what they read with the built-in values: the first cluster, the second */
static const char *const k618_files[] = { "up_rate_limit_us", "down_rate_limit_us", "enabled", "use_slim_walt", "ravg_window_us",
					  "target_loads", "target_load_thresh", "target_load_shift", "refined", "boost", "floor", "touch_floor",
					  "touch_ms", "ceiling", "iowait_boost", "react_us", "caps", "caps_boost", "caps_floor", "caps_max",
					  "caps_burst", "caps_burst_ms", "caps_front_ms", "caps_front_pct", "caps_touch_ms", "caps_touch", "caps_ui_ms",
					  NULL };
static const char *const k618_first[] = { "500", "1000", "1", "1", "20000", "0 0", "1024", "4", "1", "-20", "0", "1344000", "1500", "0", "0",
					  "4000", "0", "5", "1344000", "0", "0", "1000", "2000", "85", "0", "0", "0" };
static const char *const k618_second[] = { "500", "1000", "1", "1", "20000", "0 0", "1024", "4", "1", "-20", "0", "0", "0", "0", "0",
					   "4000", "1", "5", "0", "0", "1536000", "1000", "2000", "85", "0", "0", "0" };

/*
 * The second policy's built-in values (the first's are checked with the selection), and "walt" --
 * what the vendor's post-boot script writes -- selecting the governor. Then both policies get the
 * 5.15 kernel's values, which the checks after were written for.
 */
static void check_618(void)
{
	char buf[200], walt[64];
	int all = 1, selected;

	if (!set_governor_raw(1, GOV)) {
		ok(0, GOV " cannot be made policy1's governor");
	} else {
		for (int i = 0; k618_files[i]; i++)
			if (!is(1, GOV, k618_files[i], k618_second[i])) {
				ok(0, "policy1: %s/%s reads \"%s\", not \"%s\"", GOV, k618_files[i], get(tunable(1, GOV, k618_files[i]), buf, sizeof(buf)),
				   k618_second[i]);
				all = 0;
			}
		if (all)
			ok(1, GOV ": the second policy has the second cluster's built-in values (caps 1, caps_burst 1536000, no touch floor)");
	}
	get(WALT_GOV, walt, sizeof(walt));
	selected = set_governor_raw(0, "performance") && !put(path(0, "scaling_governor"), "walt") &&
		   !strcmp(get(path(0, "scaling_governor"), buf, sizeof(buf)), GOV);
	ok(selected && !strcmp(walt, GOV), "\"walt\" written to scaling_governor selects %s (policy0 now \"%s\"; cpufreq.walt_governor \"%s\")", GOV, buf,
	   walt);
	as_515(0);
	as_515(1);
}

/*
 * The EAS boot (k618_sugov_qemu.sh): EAS is possible while every policy runs a governor it takes --
 * /proc/sys/kernel/sched_energy_aware reads a value then and nothing otherwise
 * (sched_energy_aware_handler()) -- and with sched_verbose the kernel says "starting EAS" and
 * "stopping EAS" when the domains are built again with or without it.
 */
static int kmsg_fd = -1;

/* the kernel's log from now on */
static void kmsg_from_now(void)
{
	kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (kmsg_fd >= 0)
		lseek(kmsg_fd, 0, SEEK_END);
}

/* of the records since the last call, those with @a in *@na and those with @b in *@nb */
static void kmsg_count(const char *a, int *na, const char *b, int *nb)
{
	char rec[2048];

	*na = *nb = 0;
	while (kmsg_fd >= 0) {
		ssize_t n = read(kmsg_fd, rec, sizeof(rec) - 1);

		if (n < 0 && errno == EPIPE)	/* (records overwritten meanwhile: on with the next) */
			continue;
		if (n <= 0)
			break;
		rec[n] = 0;
		*na += strstr(rec, a) != NULL;
		*nb += strstr(rec, b) != NULL;
	}
}

static int eas_main(void)
{
	char buf[200], walt[64], gov[2][64];
	int start, stop, back, both;

	get(path(0, "scaling_driver"), buf, sizeof(buf));
	printf("sugov: EAS: driver \"%s\", governors %s, sched_energy_aware \"%s\"\n", buf,
	       get(path(0, "scaling_governor"), gov[0], sizeof(gov[0])), get(EAS, walt, sizeof(walt)));
	if (strcmp(buf, "testbench") || number(path(1, "affected_cpus")) != 1) {
		printf("SUGOV EAS FAILED: not a machine booted with cpufreq_testbench.enable=1 and three CPUs or more\n");
		return 1;
	}
	/*
	 * (the domains are built again once the policies are up -- arch_topology's capacities, the
	 * governors' own work items: a moment for those)
	 */
	sleep_ms(500);
	get(path(0, "scaling_governor"), gov[0], sizeof(gov[0]));
	get(path(1, "scaling_governor"), gov[1], sizeof(gov[1]));
	/* (booted with schedutil, the kernel's default: EAS on from the start) */
	ok(!strcmp(gov[0], "schedutil") && !strcmp(gov[1], "schedutil") && file_is(EAS, "1"),
	   "EAS: booted with schedutil on both policies, EAS is possible (sched_energy_aware \"%s\")", get(EAS, buf, sizeof(buf)));
	kmsg_from_now();

	/* the way back: cpufreq.walt_governor=schedutil */
	get(WALT_GOV, walt, sizeof(walt));
	back = set_governor_raw(0, "performance") && !put(path(0, "scaling_governor"), "walt") &&
	       !strcmp(get(path(0, "scaling_governor"), buf, sizeof(buf)), "schedutil");
	sleep_ms(400);
	kmsg_count("starting EAS", &start, "stopping EAS", &stop);
	ok(back && !strcmp(walt, "schedutil"), "EAS: with cpufreq.walt_governor=%s \"walt\" selects the stock governor (policy0 \"%s\")", walt, buf);

	/* schedutilmium on both: EAS goes on, not stopped by the change */
	both = set_governor_raw(0, GOV) && set_governor_raw(1, GOV);
	sleep_ms(400);
	kmsg_count("starting EAS", &start, "stopping EAS", &stop);
	ok(both && file_is(EAS, "1") && !stop, "EAS: %s on both policies, EAS still possible (sched_energy_aware \"%s\"; %d stops, %d starts)", GOV,
	   get(EAS, buf, sizeof(buf)), stop, start);

	/* another governor on one policy: EAS stops */
	set_governor_raw(0, "performance");
	sleep_ms(400);
	kmsg_count("starting EAS", &start, "stopping EAS", &stop);
	ok(file_is(EAS, "") && stop >= 1, "EAS: performance on policy0 -- EAS no longer possible (sched_energy_aware \"%s\"), stopped (%d)",
	   get(EAS, buf, sizeof(buf)), stop);

	/* schedutilmium back: EAS starts again -- the domains are built again at the governor's init */
	set_governor_raw(0, GOV);
	sleep_ms(400);
	kmsg_count("starting EAS", &start, "stopping EAS", &stop);
	ok(file_is(EAS, "1") && start >= 1, "EAS: %s on policy0 again -- EAS possible (sched_energy_aware \"%s\") and started again (%d)", GOV,
	   get(EAS, buf, sizeof(buf)), start);

	set_governor_raw(0, gov[0]);
	set_governor_raw(1, gov[1]);
	if (failures) {
		printf("SUGOV EAS FAILED: %d of %d\n", failures, checks);
		return 1;
	}
	printf("SUGOV EAS OK: %d checks of EAS with %s\n", checks, GOV);
	return 0;
}

/*
 * As /init: proc and sys mounted, the lines into the kernel's log -- the console of a QEMU boot of
 * the GKI is the serial one, /dev/console the null console its built-in command line names last.
 */
static void as_init(void)
{
	int fd;

	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	fd = open("/dev/kmsg", O_WRONLY);
	if (fd >= 0) {
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
}

/*
 * sugov_test trace <governor> [tunable=value ...]: CPU 0's frequency every 4 ms for two
 * seconds of the thread that is busy half of the time, as the runs of equal frequency
 * (to look at a governor, not a test)
 */
static int trace_main(int argc, char **argv)
{
	struct load half;
	struct trace t;
	int from = 0;

	pin((int)sysconf(_SC_NPROCESSORS_ONLN) - 1);
	start(&tickle, 0, TICKLE);
	if (!set_governor(0, argv[2]))
		printf("trace: %s cannot be made the governor\n", argv[2]);
	for (int i = 3; i < argc; i++) {
		char *eq = strchr(argv[i], '=');

		if (eq) {
			*eq = 0;
			printf("trace: %s = %s: error %d\n", argv[i], eq + 1, put(tunable(0, argv[2], argv[i]), eq + 1));
		}
	}
	printf("trace: at rest after %d ms\n", rest());
	start(&half, 0, HALF);
	watch(0, 2000, &t);
	stop(&half);
	for (int i = 1; i <= t.n; i++)
		if (i == t.n || t.freq[i] != t.freq[from]) {
			printf("trace: %4d ms %7ld for %d ms\n", t.ms[from], t.freq[from], (i == t.n ? 2000 : t.ms[i]) - t.ms[from]);
			from = i;
		}
	printf("trace: the driver was called %ld times\n", number(path(0, "testbench_switches")));
	stop(&tickle);
	return 0;
}

static int run(int argc, char **argv)
{
	char was[2][64], buf[256], rt_default[32];
	int cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
	const char *mode = getenv("sugov_mode");

	if (argc > 2 && !strcmp(argv[1], "trace"))
		return trace_main(argc, argv);
	if ((argc > 1 && !strcmp(argv[1], "eas")) || (mode && !strcmp(mode, "eas")))
		return eas_main();
	get(CPUFREQ "/policy0/scaling_driver", buf, sizeof(buf));
	printf("sugov: driver \"%s\", %d CPUs, governors: %s\n", buf, cpus, get(path(0, "scaling_available_governors"), was[0], sizeof(was[0])));
	if (strcmp(buf, "testbench") || cpus < 3 || number(path(1, "affected_cpus")) != 1) {
		printf("SUGOV TEST FAILED: not a machine booted with cpufreq_testbench.enable=1 and three CPUs or more\n");
		return 1;
	}
	get(path(0, "scaling_governor"), was[0], sizeof(was[0]));
	get(path(1, "scaling_governor"), was[1], sizeof(was[1]));
	/*
	 * A real-time thread that is runnable asks for the top frequency, whatever it needs,
	 * until a clamp says otherwise (effective_cpu_util()): the kernel's own threads of that
	 * kind wake a few hundred times a second. Their default clamp is set to nothing here, as
	 * the vendor's post-boot script sets it on the phone (and the 5.15 tuning module did).
	 */
	get(RT_DEFAULT, rt_default, sizeof(rt_default));
	ok(!put(RT_DEFAULT, "0") && number(RT_DEFAULT) == 0, "real-time threads no longer ask for the top frequency by default (sched_util_clamp_min_rt_default was %s, is 0)",
	   rt_default);
	/* this thread looks from the last CPU; CPU 0 has something that keeps its governor asked */
	pin(cpus - 1);
	start(&tickle, 0, TICKLE);

	check_governor(GOV, k618_files, k618_first);
	check_618();
	check_rate_limits();
	check_limits();
	check_half_busy();
	check_pingpong();
	check_remote();
	check_forks();
	check_lists();
	check_uclamp();
	check_boost();
	check_floors();
	check_ceiling();
	check_caps();
	check_ui();
	check_iowait(cpus);
	check_slim(cpus);
	check_shared(GOV);
	ok(!mismatches(0) && !mismatches(1) && number(path(1, "testbench_switches")) > 0,
	   "through all of it the driver went to the table entry of the frequency it was called with (%ld and %ld calls on the two policies, %ld and %ld with another)",
	   number(path(0, "testbench_switches")), number(path(1, "testbench_switches")), mismatches(0), mismatches(1));
	/* (last: with three of four CPUs gone for a moment, what the kernel itself runs is on CPU 0 afterwards) */
	check_hotplug(cpus);

	stop(&tickle);
	set_governor(0, was[0]);
	set_governor(1, was[1]);
	put(RT_DEFAULT, rt_default);
	if (failures) {
		printf("SUGOV TEST FAILED: %d of %d\n", failures, checks);
		return 1;
	}
	printf("SUGOV TEST OK: %d checks of schedutilmium and slim WALT on the test bench's two policies\n", checks);
	return 0;
}

int main(int argc, char **argv)
{
	int init = getpid() == 1, ret;

	if (init)
		as_init();
	setvbuf(stdout, NULL, _IOLBF, 0);
	ret = run(argc, argv);
	if (init) {
		printf("sugov: done (%d), powering off\n", ret);
		fflush(stdout);
		sync();
		reboot(RB_POWER_OFF);
	}
	return ret;
}
