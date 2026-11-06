// SPDX-License-Identifier: GPL-2.0
/*
 * k618_reclaimtest: QEMU only, never in a phone image. The /init of a ramdisk of its own
 * (k618_reclaimtest.sh builds it: this program, static, with GKI's zsmalloc.ko and zram.ko), on
 * QEMU's virt board given the 13 Pro's three CPU capacities in its device tree. It checks
 * k618_ports.py's look_around, kcompressd and kswapd_mid under memory pressure with zram swap,
 * as the phone runs: the classic LRU (lru_gen off), zram0 (640 MB) as the only swap,
 * watermark_scale_factor 200. Three rounds; in each a child process fills more anonymous memory
 * than is available with pages it can check, keeps reading a part of it (so that reclaim finds
 * young PTEs on the inactive list), then reads every page back and compares:
 *   A  as built: look_around 1, kcompressd 2, kswapd_mid 1, reclaim_cpus 1 (middle cluster)
 *   B  all three off: their counters stand still, kswapd goes back to all CPUs
 *   C  on again, reclaim_cpus 2: kswapd and the kcompressd threads on the little cluster
 * One line per check, "k618_reclaimtest: PASS" or "k618_reclaimtest: FAIL ..." at the end, then
 * the machine powers off. "k618rt.harness" on the command line: only the pressure and the data
 * (for a kernel without the three ports).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE		4096UL
#define WORDS		(PAGE / 8)
#define ZRAM_MB		640
#define HOT_MB		64
#define VM		"/proc/sys/vm/"

static int fails, harness;

static void out(const char *pre, const char *fmt, va_list ap)
{
	char buf[640];
	int n = snprintf(buf, sizeof(buf), "k618_reclaimtest: %s", pre);

	n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	if (write(1, buf, n) < 0)
		return;
}

static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	out("", fmt, ap);
	va_end(ap);
}

static void bad(const char *fmt, ...)
{
	va_list ap;

	fails++;
	va_start(ap, fmt);
	out("FAIL ", fmt, ap);
	va_end(ap);
}

static ssize_t rd(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = read(fd, buf, len - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	return n;
}

static int wr(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = write(fd, val, strlen(val));
	close(fd);
	return n < 0 ? -errno : 0;
}

/* the first number in a file; -1 when it cannot be read */
static long long num(const char *path)
{
	char buf[128];

	if (rd(path, buf, sizeof(buf)) <= 0)
		return -1;
	return strtoll(buf, NULL, 0);
}

/* "name value" lines (/proc/vmstat) or "name: value kB" (/proc/meminfo) */
static long long field(const char *path, const char *name)
{
	static char buf[16384];
	size_t l = strlen(name);
	char *p;

	if (rd(path, buf, sizeof(buf)) <= 0)
		return -1;
	for (p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
		if (!strncmp(p, name, l) && (p[l] == ' ' || p[l] == ':'))
			return strtoll(p + l + 1 + (p[l] == ':'), NULL, 10);
	return -1;
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* the pid of the first task whose comm starts with @prefix (exactly @prefix when @exact) */
static int find_task(const char *prefix, int exact, int *count)
{
	char path[300], comm[64];
	struct dirent *e;
	int pid = 0, n = 0;
	DIR *d = opendir("/proc");

	while (d && (e = readdir(d))) {
		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		if (rd(path, comm, sizeof(comm)) <= 0)
			continue;
		comm[strcspn(comm, "\n")] = 0;
		if (exact ? strcmp(comm, prefix) : strncmp(comm, prefix, strlen(prefix)))
			continue;
		n++;
		/* the thread without a ":<n>" is the one that takes every folio */
		if (!pid || !strchr(comm, ':'))
			pid = atoi(e->d_name);
	}
	if (d)
		closedir(d);
	if (count)
		*count = n;
	return pid;
}

static void cpus_allowed(int pid, char *buf, size_t len)
{
	char path[64], st[4096], *p;

	snprintf(path, sizeof(path), "/proc/%d/status", pid);
	snprintf(buf, len, "?");
	if (rd(path, st, sizeof(st)) <= 0 || !(p = strstr(st, "Cpus_allowed_list:")))
		return;
	p += strlen("Cpus_allowed_list:");
	p += strspn(p, " \t");
	snprintf(buf, len, "%.*s", (int)strcspn(p, "\n"), p);
}

/* user + system time of a task, in clock ticks */
static long long cpu_ticks(int pid)
{
	char path[64], st[1024], *p;
	long long ut = 0, stime = 0;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	if (rd(path, st, sizeof(st)) <= 0 || !(p = strrchr(st, ')')))
		return -1;
	/* after "pid (comm)": state is field 3, utime 14, stime 15 */
	if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lld %lld", &ut, &stime) != 2)
		return -1;
	return ut + stime;
}

/* a CPU mask as a list, as the kernel prints Cpus_allowed_list */
static void cpulist(uint64_t mask, char *buf, size_t len)
{
	int cpu, start = -1, n = 0;

	buf[0] = 0;
	for (cpu = 0; cpu <= 64; cpu++) {
		int on = cpu < 64 && (mask >> cpu & 1);

		if (on && start < 0)
			start = cpu;
		if (!on && start >= 0) {
			n += snprintf(buf + n, len - n, "%s%d", n ? "," : "", start);
			if (cpu - 1 > start)
				n += snprintf(buf + n, len - n, "-%d", cpu - 1);
			start = -1;
		}
	}
}

/* the CPUs as the kernel groups them by capacity: all, the middle ones, the slowest ones */
static int ncpus;
static char cpus_all[64], cpus_mid[64], cpus_little[64];

static void cpu_groups(void)
{
	long long cap[64], lo = -1, hi = -1;
	uint64_t all = 0, mid = 0, little = 0;
	char path[80];
	int cpu;

	for (cpu = 0; cpu < 64; cpu++) {
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
		cap[cpu] = num(path);
		if (cap[cpu] < 0)
			break;
		all |= 1ULL << cpu;
		lo = lo < 0 || cap[cpu] < lo ? cap[cpu] : lo;
		hi = cap[cpu] > hi ? cap[cpu] : hi;
	}
	ncpus = cpu;
	for (cpu = 0; cpu < ncpus; cpu++) {
		if (cap[cpu] > lo && cap[cpu] < hi)
			mid |= 1ULL << cpu;
		if (cap[cpu] == lo && lo < hi)
			little |= 1ULL << cpu;
	}
	cpulist(all, cpus_all, sizeof(cpus_all));
	/* no such cluster: no preference, every CPU */
	cpulist(mid ? mid : all, cpus_mid, sizeof(cpus_mid));
	cpulist(little ? little : all, cpus_little, sizeof(cpus_little));
	snprintf(path, sizeof(path), "%lld..%lld", lo, hi);
	say("%d CPUs, capacities %s: middle cluster %s, little cluster %s", ncpus, path,
	    cpus_mid, cpus_little);
	if (!mid)
		say("no three kinds of CPU (no capacity-dmips-mhz in the device tree?): placement "
		    "checks expect every CPU");
}

static int load_module(const char *path, const char *params)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), ret;

	if (fd < 0)
		return -errno;
	ret = syscall(SYS_finit_module, fd, params, 0);
	ret = ret < 0 ? -errno : 0;
	close(fd);
	return ret;
}

/* zram0 as swap: a swap header by hand (no mkswap here), then swapon */
static int zram_swap(void)
{
	unsigned int maj, min;
	unsigned char hdr[PAGE];
	uint32_t *info = (uint32_t *)(hdr + 1024);
	char buf[64];
	long long size;
	int fd, i;

	for (i = 0; i < 50 && rd("/sys/block/zram0/disksize", buf, sizeof(buf)) < 0; i++)
		usleep(100000);
	if (wr("/sys/block/zram0/disksize", "640M"))
		return -1;
	size = num("/sys/block/zram0/disksize");
	if (rd("/sys/block/zram0/dev", buf, sizeof(buf)) <= 0 || sscanf(buf, "%u:%u", &maj, &min) != 2)
		return -2;
	unlink("/dev/zram0");
	if (mknod("/dev/zram0", S_IFBLK | 0600, makedev(maj, min)))
		return -3;
	memset(hdr, 0, sizeof(hdr));
	info[0] = 1;				/* version */
	info[1] = size / PAGE - 1;		/* last_page */
	info[2] = 0;				/* nr_badpages */
	memcpy(hdr + PAGE - 10, "SWAPSPACE2", 10);
	fd = open("/dev/zram0", O_WRONLY | O_CLOEXEC);
	if (fd < 0 || pwrite(fd, hdr, PAGE, 0) != (ssize_t)PAGE || fsync(fd)) {
		if (fd >= 0)
			close(fd);
		return -4;
	}
	close(fd);
	if (swapon("/dev/zram0", 0))
		return -errno;
	say("zram0: %lld MB as swap, %s", size >> 20,
	    rd("/sys/block/zram0/comp_algorithm", buf, sizeof(buf)) > 0 ? buf : "?");
	return 0;
}

/* page i of round r: 1 KB that compresses badly, then 16 words over and over */
static void fill(uint64_t *p, uint64_t i, unsigned int r)
{
	uint64_t x = (i * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)r << 56) ^ 0x5DEECE66DULL;
	uint64_t base = i * 0x100000001B3ULL ^ r;
	unsigned int j;

	for (j = 0; j < 128; j++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		p[j] = x;
	}
	for (; j < WORDS; j++)
		p[j] = base ^ ((uint64_t)(j & 15) << 40);
}

static int same(const uint64_t *p, uint64_t i, unsigned int r)
{
	uint64_t want[WORDS];

	fill(want, i, r);
	return !memcmp(p, want, PAGE);
}

/* the child of a round: @bytes of anonymous memory, the first HOT_MB read again and again */
static int pressure(size_t bytes, unsigned int r)
{
	size_t n = bytes / PAGE, hot = (size_t)HOT_MB << 20 >> 12, i, k, bad_pages = 0;
	volatile uint64_t sink = 0;
	uint64_t *m;
	int pass;

	m = mmap(NULL, n * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED)
		return 2;
	/* folios of one page, whatever the THP setting: large ones are not kcompressd's */
	madvise(m, n * PAGE, MADV_NOHUGEPAGE);
	for (i = 0; i < n; i++) {
		fill(m + i * WORDS, i, r);
		if ((i & 255) != 255)
			continue;
		/* every 8 MB the hot part is read again; every MB a pause, for kswapd to keep up */
		if ((i & 2047) == 2047)
			for (k = 0; k < hot && k < i; k++)
				sink += m[k * WORDS];
		usleep(300);
	}
	for (pass = 0; pass < 3; pass++)
		for (k = 0; k < hot && k < n; k++)
			sink += m[k * WORDS];
	for (i = 0; i < n; i++)
		if (!same(m + i * WORDS, i, r))
			bad_pages++;
	say("  child: %zu pages written and read back, %zu differ", n, bad_pages);
	return bad_pages ? 1 : 0;
}

struct snap {
	long long pswpout, pswpin, scan_k, steal_k, scan_d, steal_d, rotated, skipped, kc_pages,
		  kc_freed, kc_ticks;
};

static int kc_pid, kswapd_pid;

static void snap(struct snap *s)
{
	s->pswpout = field("/proc/vmstat", "pswpout");
	s->pswpin = field("/proc/vmstat", "pswpin");
	s->scan_k = field("/proc/vmstat", "pgscan_kswapd");
	s->steal_k = field("/proc/vmstat", "pgsteal_kswapd");
	s->scan_d = field("/proc/vmstat", "pgscan_direct");
	s->steal_d = field("/proc/vmstat", "pgsteal_direct");
	s->rotated = field("/proc/vmstat", "pgrotated");
	s->skipped = num(VM "look_around_skipped");
	s->kc_pages = num(VM "kcompressd_pages");
	s->kc_freed = num(VM "kcompressd_freed");
	s->kc_ticks = kc_pid ? cpu_ticks(kc_pid) : -1;
}

static void set_vm(const char *name, int val)
{
	char path[96], v[16];

	snprintf(path, sizeof(path), VM "%s", name);
	snprintf(v, sizeof(v), "%d", val);
	if (wr(path, v) || num(path) != val)
		bad("%s: cannot be set to %d (reads %lld)", path, val, num(path));
}

static void round_(const char *name, unsigned int r, long extra_mb, int on, int reclaim_cpus,
		   const char *want)
{
	long long avail = field("/proc/meminfo", "MemAvailable");
	size_t bytes = ((size_t)avail + extra_mb * 1024) * 1024;
	struct snap a, b;
	char cpus[64], kcpus[64];
	double t0;
	int status = 0;
	pid_t pid;

	if (!harness) {
		set_vm("look_around", on);
		set_vm("kcompressd", on ? 2 : 0);
		set_vm("kswapd_mid", on);
		set_vm("reclaim_cpus", reclaim_cpus);
		sleep(1);	/* what the round before queued is written by now */
	}
	if (harness)
		say("round %s: %zu MB (MemAvailable %lld MB + %ld)", name, bytes >> 20, avail >> 10,
		    extra_mb);
	else
		say("round %s: look_around %d, kcompressd %d, kswapd_mid %d, reclaim_cpus %d; %zu MB "
		    "(MemAvailable %lld MB + %ld)", name, on, on ? 2 : 0, on, reclaim_cpus,
		    bytes >> 20, avail >> 10, extra_mb);
	snap(&a);
	t0 = now();
	pid = fork();
	if (pid == 0) {
		wr("/proc/self/oom_score_adj", "1000");	/* an OOM kill takes the child, not init */
		_exit(pressure(bytes, r));
	}
	if (pid < 0 || waitpid(pid, &status, 0) != pid) {
		bad("round %s: no child", name);
		return;
	}
	snap(&b);
	say("  %.1f s; pswpout +%lld, pswpin +%lld, kswapd scanned +%lld stole +%lld, direct "
	    "scanned +%lld stole +%lld, rotated +%lld", now() - t0, b.pswpout - a.pswpout,
	    b.pswpin - a.pswpin, b.scan_k - a.scan_k, b.steal_k - a.steal_k, b.scan_d - a.scan_d,
	    b.steal_d - a.steal_d, b.rotated - a.rotated);
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		bad("round %s: the child %s %d (data not intact, or no memory)", name,
		    WIFEXITED(status) ? "exited with" : "was killed by signal",
		    WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
	if (b.pswpout <= a.pswpout)
		bad("round %s: nothing was swapped out", name);
	if (harness)
		return;
	cpus_allowed(kswapd_pid, cpus, sizeof(cpus));
	cpus_allowed(kc_pid, kcpus, sizeof(kcpus));
	say("  look_around_skipped +%lld; kcompressd_pages +%lld, kcompressd_freed +%lld, "
	    "kcompressd0 CPU +%lld ticks; kswapd0 on %s, kcompressd0 on %s", b.skipped - a.skipped,
	    b.kc_pages - a.kc_pages, b.kc_freed - a.kc_freed, b.kc_ticks - a.kc_ticks, cpus, kcpus);
	if (on ? b.skipped <= a.skipped : b.skipped != a.skipped)
		bad("round %s: look_around_skipped %s", name, on ? "did not move" : "moved while off");
	if (on ? b.kc_pages <= a.kc_pages : b.kc_pages != a.kc_pages)
		bad("round %s: kcompressd_pages %s", name, on ? "did not move" : "moved while off");
	if (strcmp(cpus, want))
		bad("round %s: kswapd0 on CPUs %s, not %s", name, cpus, want);
	if (on && strcmp(kcpus, want))
		bad("round %s: kcompressd0 on CPUs %s, not %s", name, kcpus, want);
}

int main(void)
{
	char buf[1024];
	int n, ret;

	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mkdir("/dev", 0755);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	if (rd("/proc/cmdline", buf, sizeof(buf)) > 0 && strstr(buf, "k618rt.harness"))
		harness = 1;
	say("start%s; %s", harness ? " (harness only: the pressure and the data)" : "",
	    rd("/proc/version", buf, sizeof(buf)) > 0 ? buf : "?");

	ret = load_module("/zsmalloc.ko", "");
	if (!ret)
		ret = load_module("/zram.ko", "num_devices=1 disksize_mb=" "640");
	if (ret) {
		bad("zsmalloc.ko / zram.ko not loaded (%d)", ret);
		goto end;
	}
	/* the classic LRU, as the phone runs */
	if (wr("/sys/kernel/mm/lru_gen/enabled", "0") ||
	    rd("/sys/kernel/mm/lru_gen/enabled", buf, sizeof(buf)) <= 0 || strtol(buf, NULL, 0))
		bad("lru_gen still on (%s)", buf);
	ret = zram_swap();
	if (ret) {
		bad("zram0 not set up as swap (%d)", ret);
		goto end;
	}
	wr(VM "swappiness", "100");
	wr(VM "watermark_scale_factor", "200");
	wr(VM "page-cluster", "0");
	cpu_groups();
	kswapd_pid = find_task("kswapd0", 1, NULL);
	kc_pid = find_task("kcompressd", 0, &n);
	if (!kswapd_pid)
		bad("no kswapd0");
	if (!harness) {
		say("defaults: look_around %lld, kcompressd %lld, kswapd_mid %lld, reclaim_cpus %lld; "
		    "%d kcompressd threads", num(VM "look_around"), num(VM "kcompressd"),
		    num(VM "kswapd_mid"), num(VM "reclaim_cpus"), n);
		if (num(VM "look_around") != 1 || num(VM "kcompressd") != 2 ||
		    num(VM "kswapd_mid") != 1 || num(VM "reclaim_cpus") != 1)
			bad("defaults are not 1, 2, 1, 1 (a kernel without the three ports?)");
		if (n != 4)
			bad("%d kcompressd threads, not 4", n);
	}

	round_("A", 1, 300, 1, 1, cpus_mid);
	round_("B", 2, 300, 0, 1, cpus_all);
	round_("C", 3, 150, 1, 2, cpus_little);
	if (!harness)
		set_vm("reclaim_cpus", 1);

	if (rd("/sys/block/zram0/mm_stat", buf, sizeof(buf)) > 0)
		say("zram0 mm_stat %.*s", (int)strcspn(buf, "\n"), buf);
end:
	if (fails)
		say("FAIL: %d check(s) failed", fails);
	else
		say("PASS%s", harness ? " (harness only)" : "");
	sync();
	reboot(RB_POWER_OFF);
	for (;;)
		pause();
}
