// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */

/* arm64 syscall numbers (arch/arm64/include/asm/unistd32.h order is irrelevant here: these are the
 * generic ones, the only set arm64 has) */
#define SYS_openat	56
#define SYS_finit_module 273
#define SYS_exit_group	94

#define AT_FDCWD	-100
#define O_RDONLY	0

static long sys4(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;

	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
			 : "memory");
	return x0;
}

static void die(int code)
{
	sys4(SYS_exit_group, code, 0, 0, 0);
	__builtin_unreachable();
}

/* the stack as the kernel hands it over: argc, then argv[] */
void k618_umh_main(long *sp)
{
	char **argv = (char **)(sp + 1);
	long fd, ret;

	if (sp[0] < 2)
		die(2);
	fd = sys4(SYS_openat, AT_FDCWD, (long)argv[1], O_RDONLY, 0);
	if (fd < 0)
		die(3);
	/* argv[2], when there is one, is this module's parameters -- a user pointer, as the
	 * syscall wants; otherwise the empty string, which is what init passes for "no parameters" */
	ret = sys4(SYS_finit_module, fd, (long)(sp[0] > 2 ? argv[2] : ""), 0, 0);
	die(ret == 0 ? 0 : 1);
}

__attribute__((naked, noreturn)) void _start(void)
{
	__asm__ volatile("mov x0, sp\n\tb k618_umh_main");
}
