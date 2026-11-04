// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/module.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/string.h>

static char list_path[128] = "/lib/modules/k618_batch.load";
module_param_string(list, list_path, sizeof(list_path), 0444);
static char mod_dir[128] = "/lib/modules";
module_param_string(dir, mod_dir, sizeof(mod_dir), 0444);
static char helper[128] = "/k618_umh";
module_param_string(helper, helper, sizeof(helper), 0444);
static unsigned int delay_ms;
module_param(delay_ms, uint, 0444);
static unsigned int stop_after;
module_param(stop_after, uint, 0444);
/* Device compatibility. */
static unsigned int flush_ms = 300;
module_param(flush_ms, uint, 0444);
extern unsigned int k618_probe_flush(unsigned int spin_ms);
static unsigned int (*flush)(unsigned int);

static unsigned int n_loaded, n_refused, n_died, n_noloader;

static unsigned long long now_ms(void)
{
	return div_u64(local_clock(), NSEC_PER_MSEC);
}

/* the file, as one buffer: the list is a few kilobytes and this runs once */
static char *read_all(const char *path, size_t *len)
{
	struct file *f = filp_open(path, O_RDONLY, 0);
	char *buf;
	loff_t pos = 0;
	ssize_t r;

	if (IS_ERR(f))
		return ERR_CAST(f);
	buf = kmalloc(SZ_64K, GFP_KERNEL);
	if (!buf) {
		filp_close(f, NULL);
		return ERR_PTR(-ENOMEM);
	}
	r = kernel_read(f, buf, SZ_64K - 1, &pos);
	filp_close(f, NULL);
	if (r < 0) {
		kfree(buf);
		return ERR_PTR(r);
	}
	buf[r] = '\0';
	*len = r;
	return buf;
}

/*
 * A module's parameters, the two ways init's own loader finds them: the "options <name> ..." line
 * of modules.options, and <name>.<param>=<value> on the kernel command line (that is how the phone's
 * bootloader hands msm_drm the panel it is to use -- without it the display driver binds no panel).
 * Both files are read once; this builds one string per module, or NULL when it has no parameters.
 */
static char *opts_text, *cmdline_text;

static void norm(char *dst, size_t size, const char *name)
{
	size_t i;

	for (i = 0; i + 1 < size && name[i] && name[i] != ' ' && name[i] != '\n'; i++)
		dst[i] = name[i] == '-' ? '_' : name[i];
	dst[i] = '\0';
	if (i > 3 && !strcmp(dst + i - 3, ".ko"))
		dst[i - 3] = '\0';
}

static char *params_for(const char *name)
{
	char base[80], want[88], *out = NULL, *p, *tok;
	size_t n;

	norm(base, sizeof(base), name);
	/* modules.options: "options <base> a=1 b=2" */
	if (opts_text) {
		snprintf(want, sizeof(want), "options %s ", base);
		n = strlen(want);
		for (p = opts_text; p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
			char line[256];
			size_t l = strcspn(p, "\n");

			if (l >= sizeof(line))
				continue;
			memcpy(line, p, l);
			line[l] = '\0';
			/* the name in the file may be spelled with '-' */
			for (char *c = line + 8; *c && *c != ' '; c++)
				if (*c == '-')
					*c = '_';
			if (!strncmp(line, want, n))
				out = kasprintf(GFP_KERNEL, "%s", line + n);
			if (out)
				break;
		}
	}
	/* the command line: "<base>.<param>=<value>", in the order they appear */
	if (cmdline_text) {
		snprintf(want, sizeof(want), "%s.", base);
		n = strlen(want);
		for (tok = cmdline_text; *tok; ) {
			size_t l = strcspn(tok, " \n");
			char one[192];

			if (l && l < sizeof(one) && !strncmp(tok, want, n)) {
				char *add;

				memcpy(one, tok + n, l - n);
				one[l - n] = '\0';
				add = out ? kasprintf(GFP_KERNEL, "%s %s", out, one) :
					    kasprintf(GFP_KERNEL, "%s", one);
				kfree(out);
				out = add;
			}
			tok += l;
			while (*tok == ' ' || *tok == '\n')
				tok++;
		}
	}
	return out;
}

static void load_one(const char *name, unsigned int n, unsigned int total)
{
	char *path = kasprintf(GFP_KERNEL, "%s/%s", mod_dir, name);
	char *args = params_for(name);
	char *argv[] = { helper, path, args, NULL };
	static char * const envp[] = { "PATH=/", NULL };
	unsigned long long t0;
	int ret;

	if (!path) {
		pr_emerg("k618_load: %u/%u %s: no memory\n", n, total, name);
		return;
	}
	pr_emerg("k618_load: %u/%u %s: trying%s%s\n", n, total, name, args ? " with " : "",
		 args ?: "");
	if (flush && flush_ms)
		flush(flush_ms);
	t0 = now_ms();
	/*
	 * UMH_WAIT_PROC: the child is waited for, so the modules go in one at a time and in order.
	 * ret is the child's exit status (0 loaded, 1 finit_module refused it, 2..4 it could not
	 * try), or -ECHILD/-EINTR and the like when the child died of a signal -- which is what a
	 * module that faults in its init() looks like from here.
	 */
	ret = call_usermodehelper(helper, argv, (char **)envp, UMH_WAIT_PROC);
	/* a wait status: (exit code << 8) | the signal that killed it; or < 0, nothing ran */
	if (ret == 0) {
		n_loaded++;
		pr_emerg("k618_load: %u/%u %s: loaded (%llu ms)%s%s\n", n, total, name, now_ms() - t0,
			 args ? " with " : "", args ?: "");
	} else if (ret > 0 && (ret & 0x7f)) {
		/* killed by a signal: its init() faulted (SIGSEGV 11, SIGBUS 7, SIGILL 4...) and the
		 * oops took the child, not us -- which is the whole point of loading it this way */
		n_died++;
		pr_emerg("k618_load: %u/%u %s: FAULTED in its init() (signal %d, %llu ms)%s%s\n",
			 n, total, name, ret & 0x7f, now_ms() - t0, args ? " with " : "", args ?: "");
	} else if (ret == (1 << 8)) {		/* exit(1): the kernel refused the module */
		n_refused++;
		pr_emerg("k618_load: %u/%u %s: refused (%llu ms)%s%s\n", n, total, name,
			 now_ms() - t0, args ? " with " : "", args ?: "");
	} else if (ret > 0) {			/* exit(2..4): no file, or no argument */
		n_noloader++;
		pr_emerg("k618_load: %u/%u %s: no loader (status %#x, %llu ms)\n", n, total, name,
			 ret, now_ms() - t0);
	} else {
		n_noloader++;
		pr_emerg("k618_load: %u/%u %s: could not run %s (%d, %llu ms)\n", n, total, name,
			 helper, ret, now_ms() - t0);
	}
	kfree(args);
	kfree(path);
}

static int __init k618_load_init(void)
{
	unsigned int total = 0, n = 0;
	char *buf, *p, *line;
	size_t len = 0;

	p = kasprintf(GFP_KERNEL, "%s/modules.options", mod_dir);
	if (p) {
		opts_text = read_all(p, &len);
		if (IS_ERR(opts_text))
			opts_text = NULL;
		kfree(p);
	}
	cmdline_text = read_all("/proc/cmdline", &len);
	if (IS_ERR(cmdline_text))
		cmdline_text = NULL;
	flush = symbol_get(k618_probe_flush);
	buf = read_all(list_path, &len);
	if (IS_ERR(buf)) {
		pr_emerg("k618_load: no list %s (%ld)\n", list_path, PTR_ERR(buf));
		return 0;
	}
	for (p = buf; *p; p++)
		if (*p == '\n')
			total++;
	pr_emerg("k618_load: %u modules from %s, by %s, at %llu ms\n", total, list_path, helper,
		 now_ms());
	if (delay_ms)
		msleep(delay_ms);
	p = buf;
	while ((line = strsep(&p, "\n"))) {
		if (!*line)
			continue;
		load_one(line, ++n, total);
		if (stop_after && n_died >= stop_after) {
			pr_emerg("k618_load: stopping: %u of them died\n", n_died);
			break;
		}
	}
	pr_emerg("k618_load: done: %u loaded, %u refused, %u died, %u not tried of %u, at %llu ms\n",
		 n_loaded, n_refused, n_died, n_noloader, total, now_ms());
	kfree(buf);
	kfree(opts_text);
	kfree(cmdline_text);
	opts_text = cmdline_text = NULL;
	if (flush) {
		flush(flush_ms);	/* the summary on disk as well */
		symbol_put(k618_probe_flush);
		flush = NULL;
	}
	return 0;
}
module_init(k618_load_init);

MODULE_DESCRIPTION("6.12 bring-up: a batch of modules, a child process each");
/* 6.18 exports the VFS's own calls into a namespace a module has to name (filp_open here) */
MODULE_IMPORT_NS("ANDROID_GKI_VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
MODULE_LICENSE("GPL");
