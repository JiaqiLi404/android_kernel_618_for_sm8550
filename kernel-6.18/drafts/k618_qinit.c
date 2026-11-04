/* QEMU-only /init for k618_tlmm: loads /k618_tlmm.ko with each parameter set in turn, writes each
 * result to /dev/kmsg, unloads after every successful load, then powers off. Static, no libc. */
#define SYS_openat	56
#define SYS_close	57
#define SYS_write	64
#define SYS_finit_module 273
#define SYS_delete_module 106
#define SYS_reboot	142
#define AT_FDCWD	-100
#ifndef KO
#define KO "/k618_tlmm.ko"
#define MODNAME "k618_tlmm"
#endif

static long sys4(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;

	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static int len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void say(long kfd, const char *a, const char *b, long ret)
{
	char buf[256];
	int n = 0, i;
	char num[24];
	int k = 0;
	long v = ret < 0 ? -ret : ret;

	for (i = 0; a[i] && n < 200; i++) buf[n++] = a[i];
	for (i = 0; b[i] && n < 240; i++) buf[n++] = b[i];
	buf[n++] = ' '; buf[n++] = '-'; buf[n++] = '>'; buf[n++] = ' ';
	do { num[k++] = '0' + v % 10; v /= 10; } while (v);
	if (ret < 0) buf[n++] = '-';
	while (k) buf[n++] = num[--k];
	buf[n++] = '\n';
	sys4(SYS_write, kfd, (long)buf, n, 0);
}

static const char *runs[] = {
#include "tlmm_runs.h"
	0
};

void qinit_main(void)
{
	long kfd = sys4(SYS_openat, AT_FDCWD, (long)"/dev/kmsg", 1, 0);
	int i;

	for (i = 0; runs[i]; i++) {
		long fd = sys4(SYS_openat, AT_FDCWD, (long)KO, 0, 0);
		long ret = sys4(SYS_finit_module, fd, (long)runs[i], 0, 0);

		sys4(SYS_close, fd, 0, 0, 0);
		say(kfd, "tlmm_q: load with '", runs[i], ret);
		if (ret == 0) {
			ret = sys4(SYS_delete_module, (long)MODNAME, 2048 /* O_NONBLOCK */, 0, 0);
			say(kfd, "tlmm_q: unload", "", ret);
		}
	}
	say(kfd, "tlmm_q: all runs done", "", 0);
	sys4(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0);
	for (;;) ;
}

__asm__(".globl _start\n_start:\n bl qinit_main\n");
