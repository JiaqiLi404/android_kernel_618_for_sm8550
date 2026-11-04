/* QEMU-only /init for the synx v1 user interface (k618_tp_fix.py SYNX_V1): loads the modules listed
 * in mods.h, then drives /dev/synx_device the way this ROM's libsynx does (no SYNX_INITIALIZE, v1
 * structures, commands 0-11), plus a v2 client for comparison, and writes one line per check to
 * /dev/kmsg ("synx_q: ... PASS/FAIL"). Static, no libc. Built by k618_synx_v1_qemu.sh. */
#define SYS_mknodat	33
#define SYS_mkdirat	34
#define SYS_mount	40
#define SYS_openat	56
#define SYS_close	57
#define SYS_read	63
#define SYS_write	64
#define SYS_ioctl	29
#define SYS_ppoll	73
#define SYS_nanosleep	101
#define SYS_finit_module 273
#define SYS_reboot	142
#define AT_FDCWD	-100
#define O_RDWR		2
#define O_WRONLY	1
#define POLLPRI		2

#define SYNX_IOCTL	0xc0187382u	/* _IOWR('s', 130, struct synx_private_ioctl_arg) */
#define V1_CREATE 0
#define V1_RELEASE 1
#define V1_SIGNAL 2
#define V1_MERGE 3
#define V1_REGISTER_PAYLOAD 4
#define V1_DEREGISTER_PAYLOAD 5
#define V1_WAIT 6
#define V1_BIND 7
#define V1_ADDREFCOUNT 8
#define V1_GETSTATUS 9
#define V1_IMPORT 10
#define V1_EXPORT 11
#define V2_INITIALIZE 14

typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;

struct synx_private_ioctl_arg { u32 id; u32 size; u32 result; u32 reserved; u64 ioctl_ptr; };
/* v1 (5.15's uapi/media/synx.h) */
struct synx_info { char name[64]; s32 synx_obj; };
struct synx_userpayload_info { s32 synx_obj; u32 reserved; u64 payload[4]; };
struct synx_signal { s32 synx_obj; u32 synx_state; };
struct synx_merge { u64 synx_objs; u32 num_objs; s32 merged; };
struct synx_wait { s32 synx_obj; u32 reserved; u64 timeout_ms; };
struct synx_external_desc { u32 type; u32 reserved; s32 id[2]; };
struct synx_bind { s32 synx_obj; u32 reserved; struct synx_external_desc ext_sync_desc; };
struct synx_addrefcount { s32 synx_obj; u32 count; };
struct synx_id_info { s32 synx_obj; u32 secure_key; s32 new_synx_obj; u32 padding; };
/* v2 (6.12's synx_header.h) */
struct synx_initialize_v2 { char name[64]; u32 id; u32 flags; u32 reserved; };
struct synx_create_v2 { char name[64]; u32 synx_obj; u32 flags; u32 type; u32 res; s32 id[2]; };
struct synx_signal_v2 { u32 synx_obj; u32 synx_state; u64 reserved; };
struct pollfd { int fd; short events; short revents; };
struct timespec { long tv_sec; long tv_nsec; };

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory");
	return x0;
}
#define sys(n, a, b, c, d) sys6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

static long kfd = -1;
static int npass, nfail;

static void put(char *buf, int *n, const char *s) { while (*s && *n < 240) buf[(*n)++] = *s++; }
static void putnum(char *buf, int *n, long v)
{
	char num[24];
	int k = 0;
	unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;

	do { num[k++] = '0' + u % 10; u /= 10; } while (u);
	if (v < 0) buf[(*n)++] = '-';
	while (k) buf[(*n)++] = num[--k];
}

static void say(const char *what, long a, long b)
{
	char buf[256];
	int n = 0;

	put(buf, &n, "synx_q: ");
	put(buf, &n, what);
	put(buf, &n, " ");
	putnum(buf, &n, a);
	put(buf, &n, " ");
	putnum(buf, &n, b);
	buf[n++] = '\n';
	sys(SYS_write, kfd, buf, n, 0);
}

/* one check: got == want */
static void check(const char *what, long got, long want)
{
	char buf[256];
	int n = 0;
	int ok = got == want;

	put(buf, &n, "synx_q: ");
	put(buf, &n, what);
	put(buf, &n, ": got ");
	putnum(buf, &n, got);
	put(buf, &n, " want ");
	putnum(buf, &n, want);
	put(buf, &n, ok ? " PASS" : " FAIL");
	buf[n++] = '\n';
	sys(SYS_write, kfd, buf, n, 0);
	if (ok)
		npass++;
	else
		nfail++;
}

static long cmd(long fd, u32 id, void *p, u32 size, u32 *result)
{
	struct synx_private_ioctl_arg a = { id, size, 0, 0, (u64)(unsigned long)p };
	long rc = sys(SYS_ioctl, fd, SYNX_IOCTL, &a, 0);

	if (result)
		*result = a.result;
	return rc;
}

static void zero(void *p, int n) { char *c = p; while (n--) *c++ = 0; }
static void copy(char *d, const char *s) { while ((*d++ = *s++)) ; }

static s32 create(long fd, const char *name, long *rc)
{
	struct synx_info i;

	zero(&i, sizeof(i));
	copy(i.name, name);
	*rc = cmd(fd, V1_CREATE, &i, sizeof(i), 0);
	return i.synx_obj;
}

static long status(long fd, s32 h)
{
	struct synx_signal s = { h, 0xdead };
	long rc = cmd(fd, V1_GETSTATUS, &s, sizeof(s), 0);

	return rc ? rc : (long)s.synx_state;
}

static long sig(long fd, s32 h, u32 state)
{
	struct synx_signal s = { h, state };

	return cmd(fd, V1_SIGNAL, &s, sizeof(s), 0);
}

static long release(long fd, s32 h)
{
	struct synx_info i;

	zero(&i, sizeof(i));
	i.synx_obj = h;
	return cmd(fd, V1_RELEASE, &i, sizeof(i), 0);
}

static long payload(long fd, u32 id, s32 h, u64 data, u64 p2)
{
	struct synx_userpayload_info u;

	zero(&u, sizeof(u));
	u.synx_obj = h;
	u.payload[0] = data;
	u.payload[2] = p2;
	return cmd(fd, id, &u, sizeof(u), 0);
}

static long waitv1(long fd, s32 h, u64 ms, u32 *result)
{
	struct synx_wait w = { h, 0, ms };

	return cmd(fd, V1_WAIT, &w, sizeof(w), result);
}

static long pollpri(long fd, long ms)
{
	struct pollfd p = { (int)fd, POLLPRI, 0 };
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };

	return sys6(SYS_ppoll, (long)&p, 1, (long)&t, 0, 8, 0);
}

static void nap(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };

	sys(SYS_nanosleep, &t, 0, 0, 0);
}

static long readint(const char *path)
{
	char b[32];
	long fd = sys(SYS_openat, AT_FDCWD, path, 0, 0), n, v = 0, neg = 0, i;

	if (fd < 0)
		return -100000 + fd;
	n = sys(SYS_read, fd, b, sizeof(b) - 1, 0);
	sys(SYS_close, fd, 0, 0, 0);
	for (i = 0; i < n && (b[i] == '-' || (b[i] >= '0' && b[i] <= '9')); i++) {
		if (b[i] == '-')
			neg = 1;
		else
			v = v * 10 + b[i] - '0';
	}
	return neg ? -v : v;
}

static long writestr(const char *path, const char *s)
{
	long fd = sys(SYS_openat, AT_FDCWD, path, O_WRONLY, 0), rc;
	int n = 0;

	if (fd < 0)
		return fd;
	while (s[n])
		n++;
	rc = sys(SYS_write, fd, s, n, 0);
	sys(SYS_close, fd, 0, 0, 0);
	return rc;
}

static const char *mods[] = {
#include "mods.h"
	0
};

#define DEV "/dev/synx_device"
#define BT "/sys/module/k618_synx_bindtest/parameters/"

void qinit_main(void)
{
	long fd1, fd2, fd3, fd4, rc, i;
	s32 a, b, c, d, e, f, m, h;
	u32 res;
	struct synx_userpayload_info ev;

	kfd = sys(SYS_openat, AT_FDCWD, "/dev/kmsg", O_WRONLY, 0);
	for (i = 0; mods[i]; i++) {
		long fd = sys(SYS_openat, AT_FDCWD, mods[i], 0, 0);

		rc = sys(SYS_finit_module, fd, "", 0, 0);
		sys(SYS_close, fd, 0, 0, 0);
		check(mods[i], rc, 0);
	}
	sys(SYS_mkdirat, AT_FDCWD, "/sys", 0755, 0);
	check("mount sysfs", sys6(SYS_mount, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0, 0), 0);
	{
		/* no devtmpfs in this kernel: the node from synx's sysfs "MAJ:MIN" (its class is
		 * literally named SYNX_DEVICE_NAME) */
		char b[32];
		long fd = sys(SYS_openat, AT_FDCWD, "/sys/class/SYNX_DEVICE_NAME/synx_device/dev", 0, 0);
		long n = fd < 0 ? 0 : sys(SYS_read, fd, b, sizeof(b) - 1, 0), maj = 0, min = 0, k = 0;

		if (fd >= 0)
			sys(SYS_close, fd, 0, 0, 0);
		for (; k < n && b[k] != ':'; k++)
			maj = maj * 10 + b[k] - '0';
		for (k++; k < n && b[k] >= '0' && b[k] <= '9'; k++)
			min = min * 10 + b[k] - '0';
		check("synx device number read", n > 2, 1);
		check("mknod " DEV, sys(SYS_mknodat, AT_FDCWD, DEV, 020000 | 0600, (maj << 8) | min), 0);
	}

	fd1 = sys(SYS_openat, AT_FDCWD, DEV, O_RDWR, 0);
	fd2 = sys(SYS_openat, AT_FDCWD, DEV, O_RDWR, 0);
	check("open fd1 >= 0", fd1 >= 0, 1);
	check("open fd2 >= 0", fd2 >= 0, 1);

	/* a fresh fd polls on a real session (5.15 made it in open) */
	check("poll before any call (no event)", pollpri(fd1, 50), 0);

	a = create(fd1, "k618-a", &rc);
	check("CREATE a rc", rc, 0);
	check("CREATE a handle > 0", a > 0, 1);
	b = create(fd1, "k618-b", &rc);
	check("CREATE b rc", rc, 0);
	check("CREATE b != a", b != a && b > 0, 1);
	check("GETSTATUS a (active)", status(fd1, a), 1);

	/* REGISTER_PAYLOAD: payload[2] is not a timeout for a v1 client */
	check("REGISTER_PAYLOAD a", payload(fd1, V1_REGISTER_PAYLOAD, a, 0x1234, 5), 0);
	nap(60);
	check("no event 60 ms later (payload[2]=5 not a timeout)", pollpri(fd1, 0), 0);
	check("SIGNAL a success", sig(fd1, a, 2), 0);
	check("poll after signal", pollpri(fd1, 1000), 1);
	zero(&ev, sizeof(ev));
	check("read event size", sys(SYS_read, fd1, &ev, sizeof(ev), 0), sizeof(ev));
	check("event handle", ev.synx_obj, a);
	check("event status", ev.reserved, 2);
	check("event payload[0]", (long)ev.payload[0], 0x1234);
	check("GETSTATUS a (signalled)", status(fd1, a), 2);
	rc = waitv1(fd1, a, 10, &res);
	check("WAIT a rc", rc, 0);
	check("WAIT a result (v1: 0 for success)", res, 0);
	check("SIGNAL a again (already)", sig(fd1, a, 2) < 0, 1);

	/* MERGE */
	c = create(fd1, "k618-c", &rc);
	{
		s32 hs[2] = { b, c };
		struct synx_merge mg = { (u64)(unsigned long)hs, 2, 0 };

		check("MERGE b+c", cmd(fd1, V1_MERGE, &mg, sizeof(mg), 0), 0);
		m = mg.merged;
	}
	check("merged handle > 0", m > 0, 1);
	check("GETSTATUS merged (active)", status(fd1, m), 1);
	check("SIGNAL b", sig(fd1, b, 2), 0);
	check("GETSTATUS merged (c still active)", status(fd1, m), 1);
	check("SIGNAL c", sig(fd1, c, 2), 0);
	check("GETSTATUS merged (all signalled)", status(fd1, m), 2);
	rc = waitv1(fd1, m, 10, &res);
	check("WAIT merged result", res, 0);

	/* an error signal: WAIT's result is the state, as in 5.15 */
	d = create(fd1, "k618-d", &rc);
	check("SIGNAL d error", sig(fd1, d, 3), 0);
	check("GETSTATUS d (error)", status(fd1, d), 3);
	waitv1(fd1, d, 10, &res);
	check("WAIT d result (error state)", res, 3);

	/* ADDREFCOUNT: two more releases needed */
	check("ADDREFCOUNT a 2", ({ struct synx_addrefcount r = { a, 2 }; cmd(fd1, V1_ADDREFCOUNT, &r, sizeof(r), 0); }), 0);
	check("RELEASE a 1/3", release(fd1, a), 0);
	check("RELEASE a 2/3", release(fd1, a), 0);
	check("GETSTATUS a after 2 releases", status(fd1, a), 2);
	check("RELEASE a 3/3", release(fd1, a), 0);
	check("RELEASE a 4th (gone)", release(fd1, a) < 0, 1);
	check("GETSTATUS a gone (v1: 0)", status(fd1, a), 0);
	check("ADDREFCOUNT bad handle", ({ struct synx_addrefcount r = { 0x7ffff, 1 }; cmd(fd1, V1_ADDREFCOUNT, &r, sizeof(r), 0); }) < 0, 1);
	check("ADDREFCOUNT b 5000 (> 2048)", ({ struct synx_addrefcount r = { b, 5000 }; cmd(fd1, V1_ADDREFCOUNT, &r, sizeof(r), 0); }) < 0, 1);

	/* EXPORT / IMPORT */
	{
		struct synx_id_info x = { b, 0x55, 0, 0 };

		check("EXPORT b", cmd(fd1, V1_EXPORT, &x, sizeof(x), 0), 0);
		check("EXPORT keeps secure_key", x.secure_key, 0x55);
		x.padding = 1;
		check("EXPORT global refused", cmd(fd1, V1_EXPORT, &x, sizeof(x), 0) < 0, 1);
		x.synx_obj = 0x7ffff;
		x.padding = 0;
		check("EXPORT bad handle", cmd(fd1, V1_EXPORT, &x, sizeof(x), 0) < 0, 1);
	}
	{
		struct synx_id_info x = { b, 0x55, 0, 0 };

		check("IMPORT b into fd2", cmd(fd2, V1_IMPORT, &x, sizeof(x), 0), 0);
		h = x.new_synx_obj;
		check("imported handle > 0", h > 0, 1);
		check("GETSTATUS imported (signalled)", status(fd2, h), 2);
		check("RELEASE imported", release(fd2, h), 0);
	}
	{
		struct synx_id_info x = { b, 0, 0, 0 };

		check("IMPORT b into its own session", cmd(fd1, V1_IMPORT, &x, sizeof(x), 0), 0);
		check("same-session import = same handle", x.new_synx_obj, b);
		check("RELEASE b 1/2", release(fd1, b), 0);
		check("RELEASE b 2/2", release(fd1, b), 0);
		check("RELEASE b 3rd (gone)", release(fd1, b) < 0, 1);
	}

	/* BIND to a "CSL fence" (k618_synx_bindtest's ops), external signal -> user event */
	e = create(fd1, "k618-e", &rc);
	{
		struct synx_bind bd = { e, 0, { 0 /* SYNX_TYPE_CSL */, 0, { 5, 0 } } };

		check("BIND e to CSL 5", cmd(fd1, V1_BIND, &bd, sizeof(bd), 0), 0);
	}
	check("bind ops got sync_obj 5", readint(BT "last_sync_obj"), 5);
	check("REGISTER_PAYLOAD e", payload(fd1, V1_REGISTER_PAYLOAD, e, 0x99, 0), 0);
	check("fire the CSL fence", writestr(BT "fire", "1"), 1);
	check("poll after external signal", pollpri(fd1, 1000), 1);
	zero(&ev, sizeof(ev));
	sys(SYS_read, fd1, &ev, sizeof(ev), 0);
	check("event handle e", ev.synx_obj, e);
	check("event status e", ev.reserved, 2);
	check("event payload e", (long)ev.payload[0], 0x99);
	check("GETSTATUS e", status(fd1, e), 2);

	/* DEREGISTER_PAYLOAD: no event afterwards */
	f = create(fd1, "k618-f", &rc);
	check("REGISTER_PAYLOAD f", payload(fd1, V1_REGISTER_PAYLOAD, f, 0x77, 0), 0);
	check("DEREGISTER_PAYLOAD f", payload(fd1, V1_DEREGISTER_PAYLOAD, f, 0x77, 0), 0);
	check("SIGNAL f", sig(fd1, f, 2), 0);
	check("no event for f", pollpri(fd1, 200), 0);

	/* v2 sizes on a v1 session are refused, as 5.15 did */
	{
		struct synx_create_v2 c2;

		zero(&c2, sizeof(c2));
		c2.flags = 1;
		check("v2 CREATE size on a v1 session", cmd(fd1, V1_CREATE, &c2, sizeof(c2), 0) < 0, 1);
	}

	/* a v2 client: SYNX_INITIALIZE replaces the session open made */
	fd3 = sys(SYS_openat, AT_FDCWD, DEV, O_RDWR, 0);
	{
		struct synx_initialize_v2 in;
		struct synx_create_v2 c2;
		struct synx_signal_v2 s2;

		zero(&in, sizeof(in));
		copy(in.name, "k618-v2");
		check("v2 INITIALIZE", cmd(fd3, V2_INITIALIZE, &in, sizeof(in), 0), 0);
		zero(&c2, sizeof(c2));
		copy(c2.name, "k618-v2-obj");
		c2.flags = 1;	/* SYNX_CREATE_LOCAL_FENCE */
		check("v2 CREATE", cmd(fd3, V1_CREATE, &c2, sizeof(c2), 0), 0);
		check("v2 handle > 0", c2.synx_obj > 0, 1);
		zero(&s2, sizeof(s2));
		s2.synx_obj = c2.synx_obj;
		check("v2 GETSTATUS", cmd(fd3, V1_GETSTATUS, &s2, sizeof(s2), 0), 0);
		check("v2 state active", s2.synx_state, 1);
		/* the error state from a v2 (or kernel) client, as 5.15 took it (synx v1 6) */
		s2.synx_state = 3;
		check("v2 SIGNAL error state", cmd(fd3, V1_SIGNAL, &s2, sizeof(s2), 0), 0);
		zero(&s2, sizeof(s2));
		s2.synx_obj = c2.synx_obj;
		cmd(fd3, V1_GETSTATUS, &s2, sizeof(s2), 0);
		check("v2 state error", s2.synx_state, 3);
		check("v1 CREATE on the v2 session refused", create(fd3, "x", &rc) || rc < 0, 1);
	}

	/* open and close without a call */
	fd4 = sys(SYS_openat, AT_FDCWD, DEV, O_RDWR, 0);
	check("close fd4", sys(SYS_close, fd4, 0, 0, 0), 0);
	check("close fd1", sys(SYS_close, fd1, 0, 0, 0), 0);
	check("close fd2", sys(SYS_close, fd2, 0, 0, 0), 0);
	check("close fd3", sys(SYS_close, fd3, 0, 0, 0), 0);
	nap(200);
	say("done: passed, failed", npass, nfail);
	sys(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0);
	for (;;)
		;
}

__asm__(".globl _start\n_start:\n bl qinit_main\n");
