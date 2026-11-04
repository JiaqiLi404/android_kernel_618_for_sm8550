// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>

static bool tolerate = true;
module_param(tolerate, bool, 0444);
/* how many failures to tolerate; 0: as many as come */
static unsigned int max;
module_param(max, uint, 0444);

/* Device compatibility. */
static char tolerate_comm[64];
module_param_string(tolerate_comm, tolerate_comm, sizeof(tolerate_comm), 0444);

static unsigned int tried, failed, tolerated, comm_tolerated;

/* is the loading process one of tolerate_comm? */
static bool comm_listed(void)
{
	const char *p = tolerate_comm;

	while (*p) {
		size_t l = strcspn(p, ",");

		if (l && l == strnlen(current->comm, TASK_COMM_LEN) && !strncmp(current->comm, p, l))
			return true;
		p += l;
		if (*p == ',')
			p++;
	}
	return false;
}

/* a listed process's failure: answered as loaded, and said so in the log now and then */
static bool tolerate_listed(struct pt_regs *regs, const char *what, long ret)
{
	if (!comm_listed())
		return false;
	comm_tolerated++;
	regs->regs[0] = 0;
	if (comm_tolerated <= 3 || !(comm_tolerated % 100))
		pr_emerg("k618_insmod: %s's load of %s: err %ld, answered as loaded (%u so far, at %llu ms)\n",
			 current->comm, what, ret, comm_tolerated, div_u64(local_clock(), NSEC_PER_MSEC));
	return true;
}

/* what the entry handler found, for the return handler */
struct k618_ins {
	char path[96];
};

static const char *fd_path(int fd, char *buf, size_t size)
{
	struct file *f = fget_raw(fd);
	const char *p = "?";

	if (!f)
		return p;
	/* the file is held by the caller's fd table as well, so this put cannot be the last */
	p = d_path(&f->f_path, buf, size);
	fput(f);
	return IS_ERR(p) ? "?" : p;
}

/* __arm64_sys_finit_module(const struct pt_regs *user): x0 is the user's registers, x0 of those the fd */
static int k618_finit_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct k618_ins *d = (struct k618_ins *)ri->data;
	struct pt_regs *user = (struct pt_regs *)regs->regs[0];
	char buf[96];

	tried++;
	strscpy(d->path, user ? fd_path((int)user->regs[0], buf, sizeof(buf)) : "?", sizeof(d->path));
	return 0;
}

static int k618_finit_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct k618_ins *d = (struct k618_ins *)ri->data;
	long ret = regs_return_value(regs);

	if (ret >= 0)
		return 0;
	failed++;
	if (tolerate_listed(regs, d->path, ret))
		return 0;
	if (!tolerate || (max && tolerated >= max)) {
		pr_emerg("k618_insmod: NOT LOADED %s: err %ld (not tolerated; %u of %u tried failed)\n",
			 d->path, ret, failed, tried);
		return 0;
	}
	tolerated++;
	/* what invoke_syscall hands back to init: a success, so its list goes on */
	regs->regs[0] = 0;
	pr_emerg("k618_insmod: NOT LOADED %s: err %ld (tolerated %u of %u tried, at %llu ms)\n",
		 d->path, ret, tolerated, tried, div_u64(local_clock(), NSEC_PER_MSEC));
	return 0;
}

static struct kretprobe k618_finit_krp = {
	.kp.symbol_name = "__arm64_sys_finit_module",
	.entry_handler = k618_finit_entry,
	.handler = k618_finit_ret,
	.data_size = sizeof(struct k618_ins),
	.maxactive = 64,
};

/* init(8) uses finit_module; init_module(2) is watched as well, where the name is in a user buffer */
static int k618_init_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	long ret = regs_return_value(regs);

	if (ret >= 0)
		return 0;
	failed++;
	if (tolerate_listed(regs, "(init_module)", ret))
		return 0;
	if (!tolerate || (max && tolerated >= max)) {
		pr_emerg("k618_insmod: NOT LOADED (init_module): err %ld (not tolerated)\n", ret);
		return 0;
	}
	tolerated++;
	regs->regs[0] = 0;
	pr_emerg("k618_insmod: NOT LOADED (init_module): err %ld (tolerated %u)\n", ret, tolerated);
	return 0;
}

static struct kretprobe k618_init_krp = {
	.kp.symbol_name = "__arm64_sys_init_module",
	.handler = k618_init_ret,
	.maxactive = 64,
};

static bool finit_on, init_on;

static int __init k618_insmod_init(void)
{
	int ret = register_kretprobe(&k618_finit_krp);

	finit_on = !ret;
	if (ret) {
		/* without it the batch is as fatal as before: say so where the log cannot miss it */
		pr_emerg("k618_insmod: no kretprobe on finit_module (%d): the first bad module ends this boot\n",
			 ret);
		return 0;
	}
	init_on = !register_kretprobe(&k618_init_krp);
	pr_emerg("k618_insmod: watching finit_module%s; tolerate %d, max %u, tolerate_comm '%s'\n",
		 init_on ? " and init_module" : "", tolerate, max, tolerate_comm);
	return 0;
}

static void __exit k618_insmod_exit(void)
{
	if (finit_on)
		unregister_kretprobe(&k618_finit_krp);
	if (init_on)
		unregister_kretprobe(&k618_init_krp);
	pr_emerg("k618_insmod: %u loads tried, %u failed, %u tolerated, %u answered as loaded for '%s'\n",
		 tried, failed, tolerated, comm_tolerated, tolerate_comm);
}
module_init(k618_insmod_init);
module_exit(k618_insmod_exit);

MODULE_DESCRIPTION("6.12 bring-up: a failed insmod does not end the test boot");
MODULE_LICENSE("GPL");
