// SPDX-License-Identifier: GPL-2.0
/*
 * k618_place_test: a QEMU smoke test of k618_place (never for the phone). Loaded right after
 * k618_place at the end of the boot batch; its init runs every check and returns, so the run takes
 * ~15 s longer. QEMU's virt board has no cpufreq and no energy model and gives every CPU the same
 * capacity, so EAS is off there; what can be checked:
 *
 *   em     with 8 CPUs, three energy-model domains like the 13 Pro's (CPUs 0-2 / 3-6 / 7, the phone's
 *          OPP frequencies, a C * V^2 * f model) are registered here; k618_place must find them,
 *          install the measured table (cost = power * 10 / performance everywhere, three spot costs),
 *          keep the registered inefficiency marks, put the registered table back on em=0, and mark
 *          the measured model's inefficient OPPs with em_ineff=1
 *   slice  a thread named k618pt_draw (k618_place.test_comm makes it a draw thread) gets the marked
 *          slice, loses it on slice=0, gets it back on slice=1
 *   pull   that thread, queued behind a busy thread on CPU 1 with CPU 2 idle and allowed, is pulled
 *          (the "pulls" counter moves; with pull=0 it does not)
 *   rt     an RT thread whose previous CPU runs the draw thread is placed elsewhere (rt=1) instead of
 *          preempting it (rt=0, the kernel's own placement)
 *   sync   only that its hook went in: the rule acts inside EAS, which QEMU does not run
 *
 * Results: "k618_place_test: PASS ..." / "FAIL ..." lines and a summary in the kernel log.
 */
#define pr_fmt(fmt) "k618_place_test: " fmt

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/energy_model.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>

static int k618pt_pass, k618pt_fail;

#define CHECK(cond, fmt, ...) do {						\
	if (cond) {								\
		k618pt_pass++;							\
		pr_info("PASS " fmt "\n", ##__VA_ARGS__);			\
	} else {								\
		k618pt_fail++;							\
		pr_err("FAIL " fmt "\n", ##__VA_ARGS__);			\
	}									\
} while (0)

/* ---------------------------------------------------------------- k618_place's parameters */

#define K618PT_PARAMS	"/sys/module/k618_place/parameters/"

static int k618pt_write(const char *name, const char *val)
{
	char path[96];
	struct file *f;
	loff_t pos = 0;
	ssize_t n;

	snprintf(path, sizeof(path), K618PT_PARAMS "%s", name);
	f = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_write(f, val, strlen(val), &pos);
	filp_close(f, NULL);
	return n < 0 ? n : 0;
}

static char k618pt_buf[PAGE_SIZE];

static const char *k618pt_read(const char *name)
{
	char path[96];
	struct file *f;
	loff_t pos = 0;
	ssize_t n;

	snprintf(path, sizeof(path), K618PT_PARAMS "%s", name);
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return "";
	n = kernel_read(f, k618pt_buf, sizeof(k618pt_buf) - 1, &pos);
	filp_close(f, NULL);
	k618pt_buf[n > 0 ? n : 0] = '\0';
	return k618pt_buf;
}

/* a counter of k618_place's state file */
static long k618pt_stat(const char *key)
{
	const char *s = k618pt_read("state"), *p;
	char pat[40];
	long v = -1;

	snprintf(pat, sizeof(pat), "\n%s ", key);
	p = strstr(s, pat);
	if (p && sscanf(p + strlen(pat), "%ld", &v) == 1)
		return v;
	return -1;
}

static bool k618pt_state_has(const char *text)
{
	return strstr(k618pt_read("state"), text);
}

/* ---------------------------------------------------------------- em */

static const unsigned long k618pt_khz[3][21] = {
	{ 307200, 441600, 556800, 672000, 787200, 902400, 1017600, 1113600, 1228800, 1344000, 1459200,
	  1555200, 1670400, 1785600, 1900800, 2016000 },
	{ 499200, 614400, 729600, 844800, 940800, 1056000, 1171200, 1286400, 1401600, 1536000, 1651200,
	  1785600, 1920000, 2054400, 2188800, 2323200, 2457600, 2592000, 2707200, 2803200 },
	{ 595200, 729600, 864000, 998400, 1132800, 1248000, 1363200, 1478400, 1593600, 1708800, 1843200,
	  1977600, 2092800, 2227200, 2342400, 2476800, 2592000, 2726400, 2841600, 2956800, 3187200 },
};
static const int k618pt_nr[3] = { 16, 20, 21 };
static const unsigned int k618pt_coef[3] = { 100, 270, 588 };
static const int k618pt_first[3] = { 0, 3, 7 };
static unsigned long k618pt_reg_power[3][21], k618pt_reg_flags[3][21];

static int k618pt_domain(struct device *dev)
{
	int i;

	for (i = 2; i >= 0; i--)
		if (dev->id >= k618pt_first[i])
			return i;
	return 0;
}

/* em_create_perf_table()'s callback: the lowest state at or above *khz, C * V^2 * f with V rising */
static int k618pt_power(struct device *dev, unsigned long *uw, unsigned long *khz)
{
	int d = k618pt_domain(dev), i;

	for (i = 0; i < k618pt_nr[d]; i++)
		if (k618pt_khz[d][i] >= *khz) {
			unsigned long mv = 560 + 300 * i / (k618pt_nr[d] - 1);

			*khz = k618pt_khz[d][i];
			*uw = (unsigned long)k618pt_coef[d] * mv * mv * (*khz / 1000) / 1000000;
			return 0;
		}
	return -EINVAL;
}

static struct em_perf_state *k618pt_states(int d)
{
	struct em_perf_domain *pd = em_cpu_get(k618pt_first[d]);

	return pd ? rcu_dereference_raw(pd->em_table)->state : NULL;
}

static bool k618pt_em_register(void)
{
	struct em_data_callback cb = EM_DATA_CB(k618pt_power);
	struct cpumask m;
	int d, i, cpu, ret;

	if (num_possible_cpus() != 8) {
		pr_info("SKIP em: %u CPUs, the three domains need 8 (QEMU_SMP=8)\n", num_possible_cpus());
		return false;
	}
	for (d = 0; d < 3; d++) {
		if (em_cpu_get(k618pt_first[d])) {
			pr_info("SKIP em: an energy model is there already\n");
			return false;
		}
		cpumask_clear(&m);
		for (cpu = k618pt_first[d]; cpu < (d < 2 ? k618pt_first[d + 1] : 8); cpu++)
			cpumask_set_cpu(cpu, &m);
		ret = em_dev_register_pd_no_update(get_cpu_device(k618pt_first[d]), k618pt_nr[d], &cb, &m, true);
		if (ret) {
			pr_err("FAIL em: cannot register domain %d (%d)\n", d, ret);
			k618pt_fail++;
			return false;
		}
		for (i = 0; i < k618pt_nr[d]; i++) {
			k618pt_reg_power[d][i] = k618pt_states(d)[i].power;
			k618pt_reg_flags[d][i] = k618pt_states(d)[i].flags;
		}
	}
	return true;
}

static bool k618pt_wait_em(const char *what, int secs)
{
	int i;

	for (i = 0; i < secs * 10; i++) {
		if (k618pt_state_has(what))
			return true;
		msleep(100);
	}
	return false;
}

static int k618pt_find(int d, unsigned long khz)
{
	int i;

	for (i = 0; i < k618pt_nr[d]; i++)
		if (k618pt_khz[d][i] == khz)
			return i;
	return -1;
}

static void k618pt_em_checks(void)
{
	/* the module's cost = 10 * K * E, K = 588: E 1000 (little 1228.8), 1142 (middle 499.2), 3161
	 * (prime 3187.2), less what integer performance and power drop */
	static const struct { int d; unsigned long khz, cost; } spot[] = {
		{ 0, 1228800, 5880 }, { 1, 499200, 6714 }, { 2, 3187200, 18586 },
	};
	bool costs_ok = true, flags_ok = true, back_ok = true;
	struct em_perf_state *st;
	int d, i;

	CHECK(k618pt_wait_em("\nem measured", 10), "em: k618_place installed the measured model");
	for (d = 0; d < 3; d++) {
		st = k618pt_states(d);
		for (i = 0; i < k618pt_nr[d]; i++) {
			if (st[i].cost != st[i].power * 10 / st[i].performance)
				costs_ok = false;
			if (st[i].flags != k618pt_reg_flags[d][i])
				flags_ok = false;
		}
	}
	CHECK(costs_ok, "em: cost = power * 10 / performance at every OPP");
	CHECK(flags_ok, "em: em_ineff=0 keeps the registered inefficiency marks");
	for (i = 0; i < ARRAY_SIZE(spot); i++) {
		int j = k618pt_find(spot[i].d, spot[i].khz);
		unsigned long c = k618pt_states(spot[i].d)[j].cost;

		CHECK(c + 1 >= spot[i].cost && c <= spot[i].cost + 1, "em: domain %d at %lu kHz costs %lu (want %lu)",
		      spot[i].d, spot[i].khz, c, spot[i].cost);
	}

	k618pt_write("em", "0");
	CHECK(k618pt_wait_em("\nem device tree", 5), "em: em=0 reported");
	for (d = 0; d < 3; d++) {
		st = k618pt_states(d);
		for (i = 0; i < k618pt_nr[d]; i++)
			if (st[i].power != k618pt_reg_power[d][i] || st[i].flags != k618pt_reg_flags[d][i])
				back_ok = false;
	}
	CHECK(back_ok, "em: em=0 puts the registered table back, power and marks");

	k618pt_write("em_ineff", "1");
	k618pt_write("em", "1");
	k618pt_wait_em("\nem measured", 5);
	st = k618pt_states(0);
	CHECK((st[4].flags & EM_PERF_STATE_INEFFICIENT) && !(st[5].flags & EM_PERF_STATE_INEFFICIENT),
	      "em: em_ineff=1 marks little 787.2 MHz inefficient, 902.4 MHz not");
	st = k618pt_states(2);
	CHECK((st[1].flags & EM_PERF_STATE_INEFFICIENT) && !(st[2].flags & EM_PERF_STATE_INEFFICIENT),
	      "em: em_ineff=1 marks prime 729.6 MHz inefficient, 864 MHz not");
	k618pt_write("em_ineff", "0");
	flags_ok = true;
	for (d = 0; d < 3; d++) {
		st = k618pt_states(d);
		for (i = 0; i < k618pt_nr[d]; i++)
			if (st[i].flags != k618pt_reg_flags[d][i])
				flags_ok = false;
	}
	CHECK(flags_ok, "em: em_ineff=0 again restores the registered marks");
}

/* ---------------------------------------------------------------- threads */

static int k618pt_spin(void *unused)
{
	while (!kthread_should_stop()) {
		udelay(200);
		cond_resched();
	}
	return 0;
}

static struct task_struct *k618pt_draw_task;
static atomic_t k618pt_rt_runs, k618pt_rt_hits;
static int k618pt_rt_cpu;

/* wakes every 2 ms; counts the wake-ups that ran on the draw thread's CPU */
static int k618pt_rt_loop(void *unused)
{
	while (!kthread_should_stop()) {
		usleep_range(2000, 2200);
		atomic_inc(&k618pt_rt_runs);
		if (raw_smp_processor_id() == k618pt_rt_cpu)
			atomic_inc(&k618pt_rt_hits);
	}
	return 0;
}

/* runs a little, sleeps half a millisecond: its CPU goes idle (newidle) every ~0.5 ms */
static int k618pt_nap(void *unused)
{
	while (!kthread_should_stop()) {
		udelay(30);
		usleep_range(400, 500);
	}
	return 0;
}

/* the RT thread back onto CPU 4 once (its previous CPU then), free again; wake-ups there in 1 s */
static void k618pt_rt_measure(struct task_struct *rtt, int *runs, int *hits)
{
	set_cpus_allowed_ptr(rtt, cpumask_of(4));
	msleep(20);
	set_cpus_allowed_ptr(rtt, cpu_possible_mask);
	atomic_set(&k618pt_rt_runs, 0);
	atomic_set(&k618pt_rt_hits, 0);
	msleep(1000);
	*runs = atomic_read(&k618pt_rt_runs);
	*hits = atomic_read(&k618pt_rt_hits);
}

static struct task_struct *k618pt_thread(int (*fn)(void *), const char *name, int cpu)
{
	struct task_struct *t = kthread_create(fn, NULL, "%s", name);

	if (IS_ERR(t))
		return NULL;
	set_cpus_allowed_ptr(t, cpumask_of(cpu));
	wake_up_process(t);
	return t;
}

static bool k618pt_slice_marked(struct task_struct *t)
{
	return t->se.custom_slice && (t->se.slice & 0x3ff) == 0x2a5;
}

static void k618pt_thread_checks(void)
{
	struct task_struct *draw, *hog, *rtt, *nap, *busy[8];
	struct cpumask m;
	long before, after;
	int i, runs, hits;

	if (num_online_cpus() < 5) {
		pr_info("SKIP threads: %u CPUs online, need 5\n", num_online_cpus());
		return;
	}
	k618pt_write("test_comm", "k618pt_draw");
	draw = k618pt_thread(k618pt_spin, "k618pt_draw", 1);
	hog = k618pt_thread(k618pt_spin, "k618pt_hog", 1);
	if (!draw || !hog) {
		pr_err("FAIL threads: cannot start\n");
		k618pt_fail++;
		return;
	}
	k618pt_draw_task = draw;
	/* the hog outweighs the draw thread 87:1, so the draw thread stays queued across ticks */
	set_user_nice(hog, -20);
	/* a parameter write walks every thread; the new one is named after its creation */
	k618pt_write("test_comm", "k618pt_draw");
	msleep(50);
	CHECK(k618pt_slice_marked(draw), "slice: the draw thread has the marked slice (%llu ns)", draw->se.slice);
	k618pt_write("slice", "0");
	CHECK(!draw->se.custom_slice, "slice: slice=0 takes it off");
	k618pt_write("slice", "1");
	CHECK(k618pt_slice_marked(draw), "slice: slice=1 puts it back");
	CHECK(!hog->se.custom_slice, "slice: a thread of another name keeps the base slice");

	/* pull: queued behind the hog on CPU 1, CPU 2 idle and allowed */
	cpumask_clear(&m);
	cpumask_set_cpu(1, &m);
	cpumask_set_cpu(2, &m);
	before = k618pt_stat("pulls");
	for (i = 0; i < 20; i++) {
		set_cpus_allowed_ptr(draw, cpumask_of(1));
		msleep(30);
		set_cpus_allowed_ptr(draw, &m);
		msleep(70);
	}
	after = k618pt_stat("pulls");
	CHECK(after > before, "pull: the waiting draw thread was pulled (%ld times in 20 rounds, kicks %ld)",
	      after - before, k618pt_stat("pull_kicks"));
	k618pt_write("pull", "0");
	before = k618pt_stat("pulls");
	for (i = 0; i < 10; i++) {
		set_cpus_allowed_ptr(draw, cpumask_of(1));
		msleep(30);
		set_cpus_allowed_ptr(draw, &m);
		msleep(70);
	}
	after = k618pt_stat("pulls");
	CHECK(after == before, "pull: pull=0 pulls nothing (%ld)", after - before);
	k618pt_write("pull", "1");

	/* pull by a CPU going idle: CPU 2 runs a thread that sleeps every half millisecond */
	nap = k618pt_thread(k618pt_nap, "k618pt_nap", 2);
	before = k618pt_stat("pulls_newidle");
	for (i = 0; nap && i < 20; i++) {
		set_cpus_allowed_ptr(draw, cpumask_of(1));
		msleep(30);
		set_cpus_allowed_ptr(draw, &m);
		msleep(70);
	}
	after = k618pt_stat("pulls_newidle");
	CHECK(nap && after > before, "pull: a CPU going idle took it (%ld times in 20 rounds)", after - before);
	if (nap)
		kthread_stop(nap);
	kthread_stop(hog);

	/* rt: the RT thread's previous CPU is the draw thread's (CPU 4, the draw thread alone there) */
	set_cpus_allowed_ptr(draw, cpumask_of(4));
	k618pt_rt_cpu = 4;
	rtt = kthread_create(k618pt_rt_loop, NULL, "k618pt_rt");
	if (IS_ERR(rtt)) {
		pr_err("FAIL rt: cannot start\n");
		k618pt_fail++;
		goto out;
	}
	sched_set_fifo_low(rtt);
	set_cpus_allowed_ptr(rtt, cpumask_of(4));
	wake_up_process(rtt);
	msleep(100);
	before = k618pt_stat("rt_picks");
	k618pt_rt_measure(rtt, &runs, &hits);
	CHECK(runs > 50 && hits * 10 <= runs, "rt: rt=1 kept off the draw thread's CPU (%d of %d wake-ups there, %ld picks)",
	      hits, runs, k618pt_stat("rt_picks") - before);
	k618pt_write("rt", "0");
	k618pt_rt_measure(rtt, &runs, &hits);
	CHECK(runs > 50 && hits * 2 >= runs, "rt: with rt=0 the kernel's own placement preempts it (%d of %d there)",
	      hits, runs);
	k618pt_write("rt", "1");
	/* an RT thread that is a draw thread itself (here: by the test's name) is left to the kernel */
	k618pt_write("test_comm", "k618pt_rt");
	before = k618pt_stat("rt_picks");
	k618pt_rt_measure(rtt, &runs, &hits);
	/*
	 * Only where this thread went counts: rt_picks is every RT wake-up of the system, and with the
	 * phone's full module set other RT threads (the 18 Pro Max modules' own) wake meanwhile
	 */
	CHECK(runs > 50 && hits * 2 >= runs,
	      "rt: an RT draw thread keeps the kernel's placement (%d of %d on its previous CPU; %ld RT picks"
	      " of other threads meanwhile)", hits, runs, k618pt_stat("rt_picks") - before);
	k618pt_write("test_comm", "k618pt_draw");

	/* rt with no idle CPU: every other CPU runs a busy thread, so only "not the draw thread's" helps */
	for (i = 0; i < 8; i++)
		busy[i] = i == 4 || !cpu_online(i) ? NULL : k618pt_thread(k618pt_spin, "k618pt_busy", i);
	msleep(50);
	before = k618pt_stat("rt_draw_skips");
	k618pt_rt_measure(rtt, &runs, &hits);
	CHECK(runs > 50 && hits * 10 <= runs && k618pt_stat("rt_draw_skips") > before,
	      "rt: all CPUs busy, rt=1 still kept off the draw thread's CPU (%d of %d there, %ld skips)",
	      hits, runs, k618pt_stat("rt_draw_skips") - before);
	k618pt_write("rt", "0");
	k618pt_rt_measure(rtt, &runs, &hits);
	CHECK(runs > 50 && hits * 2 >= runs, "rt: all CPUs busy, rt=0 preempts it (%d of %d there)", hits, runs);
	k618pt_write("rt", "1");
	for (i = 0; i < 8; i++)
		if (busy[i])
			kthread_stop(busy[i]);
	kthread_stop(rtt);
out:
	kthread_stop(draw);
	/* (a zero-length write never reaches the store: a lone newline empties it) */
	k618pt_write("test_comm", "\n");
}

static int __init k618pt_init(void)
{
	struct path path;
	bool em_ok;

	if (kern_path(K618PT_PARAMS "state", 0, &path)) {
		pr_err("FAIL k618_place is not loaded\n");
		return 0;
	}
	path_put(&path);
	CHECK(k618pt_state_has("\nhooks rt 1 pull 1 slice 1 sync 1\n"), "hooks: all nine went in");
	/* whatever the module was loaded with: every part on, as the checks expect */
	k618pt_write("rt", "1");
	k618pt_write("pull", "1");
	k618pt_write("slice", "1");
	k618pt_write("sync", "1");
	k618pt_write("em_ineff", "0");
	k618pt_write("em", "1");
	em_ok = k618pt_em_register();
	if (em_ok)
		k618pt_em_checks();
	k618pt_thread_checks();
	pr_info("sync: hook in, not exercised (no EAS on QEMU); drops %ld\n", k618pt_stat("sync_drops"));
	pr_info("state at the end:\n%s", k618pt_read("state"));
	pr_info("%d passed, %d failed\n", k618pt_pass, k618pt_fail);
	return 0;
}
module_init(k618pt_init);

/* filp_open for k618_place's parameter files: the GKI keeps it for filesystems; a QEMU-only test may
 * say it means to use it */
MODULE_IMPORT_NS("ANDROID_GKI_VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
MODULE_DESCRIPTION("k618: QEMU smoke test of k618_place");
MODULE_LICENSE("GPL v2");
