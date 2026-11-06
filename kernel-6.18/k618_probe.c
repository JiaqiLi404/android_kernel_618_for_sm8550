// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#include <drm/drm_atomic.h>
#include <drm/drm_device.h>
#include <drm/drm_mode_object.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_print.h>
#include <drm/drm_property.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/device/bus.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kdebug.h>
#include <linux/kmsg_dump.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/pid.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/debug.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/sched/task.h>
#include <linux/security.h>
#include <linux/seq_file.h>
#include <linux/stacktrace.h>
#include <linux/string.h>
#include <linux/suspend.h>
#include <linux/sysctl.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>

/* a write to a block device has to be a whole number of blocks, and 4 KB covers UFS */
#define BLK 4096
/* room kept at the front of the buffer for the copy's own header line */
#define HEAD 160
#include <linux/workqueue.h>
#include <asm/syscall.h>
#include <asm/unistd.h>
#include <net/net_namespace.h>

/* probe_ms=0: do not look at all (only write the log, if that is asked for) */
static unsigned int probe_ms = 45000;
module_param(probe_ms, uint, 0444);
static unsigned int force_code;
module_param(force_code, uint, 0444);
static char *log_part = "";
module_param(log_part, charp, 0444);
/* log_path= writes somewhere else than /dev/block/by-name/<log_part> (a file, to check this in QEMU) */
static char *log_path = "";
module_param(log_path, charp, 0444);
/*
 * log_major/log_minor= take the partition by its device numbers instead of its by-name link, which
 * first-stage init only makes once it walks the block uevents, well after the disk itself appears.
 * The partition's own name still has to be log_part, so the numbers cannot send a write elsewhere.
 */
static unsigned int log_major, log_minor;
module_param(log_major, uint, 0444);
module_param(log_minor, uint, 0444);
/* the partition whose by-name link leads to the disk that log_part is on (init mounts this one) */
static char *log_from = "metadata";
module_param(log_from, charp, 0444);
/*
 * the first write at log_start_ms, then one every log_every_ms (0: only one). Until the partition is
 * there -- the disk takes a second or two to show up -- it is tried again every log_poll_ms, which
 * has to be short: the open itself has to be done before anything goes wrong, because afterwards it
 * would want locks that a stopped CPU may be holding. What it writes then is the newest log_buf_kb
 * of the log, into one of two slots of log_slot_kb from log_offset_kb into the partition (the ROM's
 * own mtdoops writes a 2 MB record of its own at the start of this one on every boot).
 */
static unsigned int log_start_ms = 1500;
module_param(log_start_ms, uint, 0444);
static unsigned int log_poll_ms = 200;
module_param(log_poll_ms, uint, 0444);
static unsigned int log_every_ms;
module_param(log_every_ms, uint, 0444);
static unsigned int log_offset_kb;
module_param(log_offset_kb, uint, 0444);
static unsigned int log_slot_kb = 2048;
module_param(log_slot_kb, uint, 0444);
static unsigned int log_buf_kb = 1024;
module_param(log_buf_kb, uint, 0444);

/* only the probe work runs here, one file at a time: the buffer can be static */
static char buf[8192];

static int read_file(const char *path)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t r;

	buf[0] = '\0';
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	r = kernel_read(f, buf, sizeof(buf) - 1, &pos);
	filp_close(f, NULL);
	if (r < 0)
		return r;
	buf[r] = '\0';
	/* a line of its own in the log, without the trailing newline */
	while (r > 0 && buf[r - 1] == '\n')
		buf[--r] = '\0';
	return r;
}

static void count_sb(struct super_block *sb, void *arg)
{
	if (!sb->s_bdev)
		return;
	(*(unsigned int *)arg)++;
	pr_emerg("k618_probe: mounted %s on %s\n", sb->s_type->name, sb->s_id);
}

/*
 * how many of the disk's file systems the kernel has mounted. Not from /proc/mounts: that list is
 * filtered by whose namespace and root we read it from, and first-stage init may have switched root
 * into /first_stage_ramdisk by then (it does when androidboot.force_normal_boot is set), which also
 * moves /dev and /proc out of our reach. The superblocks are the kernel's own, with no such view.
 */
static unsigned int block_mounts(void)
{
	static const char * const types[] = { "f2fs", "erofs", "ext4" };
	unsigned int i, n = 0;
	bool any = false;

	for (i = 0; i < ARRAY_SIZE(types); i++) {
		struct file_system_type *t = get_fs_type(types[i]);

		if (!t)
			continue;
		any = true;
		iterate_supers_type(t, count_sb, &n);
		/* put_filesystem is not exported: the reference leaks, and we panic right after */
	}
	return any ? n : UINT_MAX;
}

enum { SC_OTHER, SC_POLL, SC_IOCTL, SC_MOUNT, SC_IO, SC_WAIT, SC_SLEEP };

static unsigned int syscall_class(long nr)
{
	switch (nr) {
	case __NR_ppoll:
	case __NR_epoll_pwait:
		return SC_POLL;
	case __NR_ioctl:
		return SC_IOCTL;
	case __NR_mount:
	case __NR_umount2:
	case __NR_move_mount:
	case __NR_fsopen:
	case __NR_fsconfig:
	case __NR_fsmount:
		return SC_MOUNT;
	case __NR_read:
	case __NR_pread64:
	case __NR_write:
	case __NR_openat:
	case __NR_newfstatat:
	case __NR_statx:
	case __NR_getdents64:
	case __NR_faccessat:
	case __NR_readlinkat:
	case __NR_fsync:
	case __NR_syncfs:
		return SC_IO;
	case __NR_wait4:
	case __NR_waitid:
		return SC_WAIT;
	case __NR_nanosleep:
	case __NR_clock_nanosleep:
		return SC_SLEEP;
	}
	return SC_OTHER;
}

/*
 * which syscall init (pid 1) is in, from its task: first-stage init mounts /proc with hidepid=2, and
 * a kernel thread is in neither its group nor gid 3009, so nothing under /proc/1 is visible to us.
 * The registers of a task on a CPU say nothing, so a running init counts as "anywhere else".
 */
static unsigned int pid1_class(void)
{
	struct task_struct *t;
	unsigned int state;
	struct pid *pid;
	long nr = -1;

	pid = find_get_pid(1);
	t = pid ? get_pid_task(pid, PIDTYPE_PID) : NULL;
	put_pid(pid);
	if (!t) {
		pr_emerg("k618_probe: no pid 1\n");
		return SC_OTHER;
	}
	state = READ_ONCE(t->__state);
	if (state)
		nr = syscall_get_nr(t, task_pt_regs(t));
	pr_emerg("k618_probe: pid 1 (%s) state 0x%x, syscall %ld\n", t->comm, state, nr);
	put_task_struct(t);
	return nr < 0 ? SC_OTHER : syscall_class(nr);
}

static struct file *log_file;
static unsigned int log_seq;
static char *log_text;
/* what the look reports when the log cannot be read back: did this work, and if not, why */
static unsigned int log_writes;
static int log_err = -EAGAIN;
/* the writer's whereabouts for the stall watch: its pid, its step, when it last finished a copy */
static pid_t log_worker;
static unsigned int log_phase;
static unsigned long log_done_at;
static unsigned int log_stall_test;
module_param(log_stall_test, uint, 0444);

static void log_name(char *path, size_t size)
{
	if (*log_path)
		strscpy(path, log_path, size);
	else if (log_major)
		snprintf(path, size, "%u:%u", log_major, log_minor);
	else
		snprintf(path, size, "/dev/block/by-name/%s", log_part);
}

/*
 * the partition to write to, opened once and then kept open. Its by-name link only exists once
 * first-stage init has walked the block uevents, which it does after loading us, so the first
 * attempts can fail and are simply tried again. A partition is written to only when the partition
 * table's own name for it is the one that was asked for.
 */
static const char *part_name(struct block_device *bdev)
{
	return bdev->bd_meta_info ? (const char *)bdev->bd_meta_info->volname : "";
}

/*
 * the partition by the name in the partition table, since first-stage init only makes by-name links
 * for the partitions it mounts itself. It asks the disk that holds log_from (which init does link,
 * because it mounts it) for the partition named log_part: device numbers alone would not do it --
 * a disk's 16th and later partitions take numbers from the extended major's one pool, which all the
 * disks share in whatever order they are scanned, so the number is not the one 5.15 showed.
 */
static dev_t part_on(struct file *disk)
{
	struct block_device *part;
	unsigned long i;

	xa_for_each(&file_bdev(disk)->bd_disk->part_tbl, i, part)
		if (!strcmp(part_name(part), log_part))
			return part->bd_dev;
	return 0;
}

/* Device compatibility. */
static struct file *open_disk(unsigned int n)
{
	char path[32], num[16];
	unsigned int maj, mi;
	struct file *f;
	loff_t pos = 0;
	ssize_t r;

	snprintf(path, sizeof(path), "/sys/block/%s%c/dev", n < 16 ? "sd" : "vd", 'a' + n % 16);
	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f))
		return NULL;
	r = kernel_read(f, num, sizeof(num) - 1, &pos);
	filp_close(f, NULL);
	if (r <= 0)
		return NULL;
	num[r] = '\0';
	if (sscanf(num, "%u:%u", &maj, &mi) != 2)
		return NULL;
	f = bdev_file_open_by_dev(MKDEV(maj, mi), BLK_OPEN_READ, NULL, NULL);
	return IS_ERR(f) ? NULL : f;
}

static struct file *find_part(void)
{
	char path[80];
	struct file *f;
	unsigned int n;
	dev_t dev = 0;

	snprintf(path, sizeof(path), "/dev/block/by-name/%s", log_from);
	f = bdev_file_open_by_path(path, BLK_OPEN_READ, NULL, NULL);
	if (!IS_ERR(f)) {
		dev = part_on(f);
		fput(f);
		if (!dev)
			pr_emerg("k618_probe: no partition '%s' on the disk of '%s'\n", log_part, log_from);
	} else {
		/* a disk that is there but not scanned yet has no partitions: the next try sees them */
		for (n = 0; n < 32 && !dev; n++) {
			f = open_disk(n);
			if (f) {
				dev = part_on(f);
				fput(f);
			}
		}
	}
	if (!dev)
		return NULL;
	pr_emerg("k618_probe: '%s' is %u:%u\n", log_part, MAJOR(dev), MINOR(dev));
	f = bdev_file_open_by_dev(dev, BLK_OPEN_WRITE, NULL, NULL);
	return IS_ERR(f) ? NULL : f;
}

static struct file *open_log_dev(void)
{
	static unsigned int tries;
	struct block_device *bdev;
	const char *name;
	char path[80];
	struct file *f;

	log_name(path, sizeof(path));
	if (*log_path) {
		f = filp_open(path, O_WRONLY | O_CREAT | O_LARGEFILE, 0600);
		if (IS_ERR(f)) {
			log_err = PTR_ERR(f);
			return NULL;
		}
		/*
		 * Before init mounts it, a path like /metadata is only a directory of the ramdisk,
		 * and a file written there is lost with the RAM. Wait for a file system that has a
		 * disk under it, and look again later.
		 */
		if (!file_inode(f)->i_sb->s_bdev) {
			pr_emerg("k618_probe: %s is not on a disk yet (%s)\n", path,
				 file_inode(f)->i_sb->s_type->name);
			fput(f);
			log_err = -EXDEV;
			return NULL;
		}
		pr_emerg("k618_probe: writing the log to %s on %s\n", path,
			 file_inode(f)->i_sb->s_id);
		return f;
	}
	if (log_major) {
		f = bdev_file_open_by_dev(MKDEV(log_major, log_minor), BLK_OPEN_WRITE, NULL, NULL);
	} else {
		f = bdev_file_open_by_path(path, BLK_OPEN_WRITE, NULL, NULL);
		/* no by-name link of its own (init only links what it mounts): ask its disk */
		if (IS_ERR(f)) {
			f = find_part();
			if (f)
				return f;
			f = ERR_PTR(-ENODEV);
		}
	}
	if (IS_ERR(f)) {
		/* the first tries can be too early for the disk: say so once, then keep trying */
		log_err = PTR_ERR(f);
		if (tries++ < 3)
			pr_emerg("k618_probe: no '%s' yet (%s: %d)\n", log_part, path, log_err);
		return NULL;
	}
	bdev = file_bdev(f);
	name = bdev->bd_meta_info ? (const char *)bdev->bd_meta_info->volname : "";
	if (!*log_part || strcmp(name, log_part)) {
		pr_emerg("k618_probe: %s is '%s', not the partition '%s'\n", path, name, log_part);
		fput(f);
		return NULL;
	}
	pr_emerg("k618_probe: writing the log to %s (%s)\n", path, name);
	return f;
}

/*
 * the kernel log onto that partition, for a later 5.15 boot to read back with dd. Every write is a
 * whole copy of the log and goes into one of two slots in turn, so a write that is cut short cannot
 * take the last good one with it: the reader takes the slot with the highest seq that ends in
 * K618END. (The log the phone keeps in RAM does not survive its hard reset; the disk does.)
 */
static void write_log(void)
{
	struct kmsg_dump_iter iter;
	size_t len = 0, total;
	loff_t pos;
	int n;

	if (!log_file)
		log_file = open_log_dev();
	if (!log_file || !log_text)
		return;
	/* where the writer is, for the stall watch (log_stall_ms) */
	WRITE_ONCE(log_worker, task_pid_nr(current));
	WRITE_ONCE(log_phase, 1);
	log_seq++;
	pos = ((loff_t)log_offset_kb + (loff_t)(log_seq & 1) * log_slot_kb) * 1024;
	n = snprintf(log_text, HEAD, "K618LOG seq %u %s at %llu ms\n", log_seq,
		     init_utsname()->release, div_u64(local_clock(), NSEC_PER_MSEC));
	/* the newest part of the log, so that a slot too small loses the start and not the end */
	kmsg_dump_rewind(&iter);
	kmsg_dump_get_buffer(&iter, true, log_text + n, (size_t)log_buf_kb * 1024 - n - 8 - BLK,
		     &len);
	/* Device compatibility. */
	memcpy(log_text + n + len, "K618END\n", 8);
	/* Device compatibility. */
	total = ALIGN(n + len + 8, BLK);
	memset(log_text + n + len + 8, 0, total - (n + len + 8));
	WRITE_ONCE(log_phase, 2);
	{
		ssize_t w = __kernel_write(log_file, log_text, total, &pos);

		if (w != (ssize_t)total) {
			/* said out loud: the copies after this one can carry it, if any get through */
			pr_emerg("k618_probe: log seq %u FAILED: wrote %zd of %zu\n", log_seq, w, total);
			log_err = -EIO;
			goto out;
		}
	}
	WRITE_ONCE(log_phase, 3);
	/* QEMU check of the stall watch: one flush that takes 3 s */
	if (log_stall_test && log_seq == log_stall_test)
		msleep(3000);
	vfs_fsync(log_file, 1);
	log_writes++;
	log_err = 0;
	WRITE_ONCE(log_done_at, jiffies);
out:
	WRITE_ONCE(log_phase, 0);
	pr_emerg("k618_probe: log seq %u: %zu bytes of log, to %llu\n", log_seq, len, pos);
}

static void k618_log_work(struct work_struct *w);
static DECLARE_DELAYED_WORK(k618_log_dw, k618_log_work);

static void k618_log_work(struct work_struct *w)
{
	write_log();
	/* Device compatibility. */
	if (!log_file)
		queue_delayed_work(system_unbound_wq, &k618_log_dw, msecs_to_jiffies(log_poll_ms));
	else if (log_every_ms)
		queue_delayed_work(system_unbound_wq, &k618_log_dw, msecs_to_jiffies(log_every_ms));
}

/* Device compatibility. */
static unsigned int log_now(unsigned int spin_ms)
{
	unsigned int was = log_writes, ms;

	mod_delayed_work(system_unbound_wq, &k618_log_dw, 0);
	for (ms = 0; ms < spin_ms && log_writes == was; ms += 10)
		mdelay(10);
	return log_writes;
}

/* Device compatibility. */
static unsigned int log_stall_ms = 1500;
module_param(log_stall_ms, uint, 0444);
static const char * const log_phase_name[] = { "idle", "reading the log", "writing", "flushing" };
static struct task_struct *log_wd;
static char *log_text2;

/*
 * O_DIRECT | O_DSYNC on a handle we already hold: the flags a write really goes by are the cached
 * f_iocb_flags, set when the file was opened, so changing f_flags alone does nothing -- the writes
 * stay buffered and reach the disk only when something else flushes the device (the copies' own
 * fsync, every 2 s). That is exactly what the panic's block must not depend on: the QEMU check had
 * it return "1 block written" with the block still in RAM, and the reboot lost it.
 */
static void log_make_direct(struct file *f)
{
	f->f_flags |= O_DIRECT | O_DSYNC;
	f->f_iocb_flags = iocb_flags(f);
}

/* a task found under rcu may be on its way out; its reference count says so */
static bool get_task_struct_safe(struct task_struct *t)
{
	return refcount_inc_not_zero(&t->usage);
}

static void log_stall_stacks(pid_t writer)
{
	struct task_struct *g, *t, *task[16];
	unsigned long ents[24];
	unsigned int n = 0, i, j, nr, st;

	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (n >= ARRAY_SIZE(task))
			goto taken;
		st = READ_ONCE(t->__state);
		/* idle kernel threads sleep as TASK_IDLE (uninterruptible + no load): not waiters */
		if (task_pid_nr(t) != writer &&
		    (!(st & TASK_UNINTERRUPTIBLE) || (st & TASK_NOLOAD)))
			continue;
		if (!get_task_struct_safe(t))
			continue;
		task[n++] = t;
	}
taken:
	rcu_read_unlock();
	for (j = 0; j < n; j++) {
		t = task[j];
		nr = stack_trace_save_tsk(t, ents, ARRAY_SIZE(ents), 0);
		pr_emerg("k618_stall: %s[%d]%s state %u, %u frames\n", t->comm, task_pid_nr(t),
			 task_pid_nr(t) == writer ? " (the writer)" : "", READ_ONCE(t->__state), nr);
		for (i = 0; i < nr; i++)
			pr_emerg("k618_stall:   %pS\n", (void *)ents[i]);
		put_task_struct(t);
	}
}

static void log_stall_copy(void)
{
	struct kmsg_dump_iter iter;
	size_t len = 0, total;
	struct file *f;
	loff_t pos;
	ssize_t w;
	int n;

	if (!log_text2 || !log_file)
		return;
	if (*log_path) {
		/* a file (the QEMU check): the same handle */
		f = get_file(log_file);
	} else {
		f = bdev_file_open_by_dev(file_bdev(log_file)->bd_dev, BLK_OPEN_WRITE, NULL, NULL);
		if (IS_ERR(f)) {
			pr_emerg("k618_stall: no second handle: %ld\n", PTR_ERR(f));
			return;
		}
		log_make_direct(f);
	}
	n = snprintf(log_text2, HEAD, "K618LOG seq %u %s at %llu ms\n", log_seq + 1,
		     init_utsname()->release, div_u64(local_clock(), NSEC_PER_MSEC));
	kmsg_dump_rewind(&iter);
	kmsg_dump_get_buffer(&iter, true, log_text2 + n, (size_t)log_buf_kb * 1024 - n - 8 - BLK,
			     &len);
	memcpy(log_text2 + n + len, "K618END\n", 8);
	total = ALIGN(n + len + 8, BLK);
	memset(log_text2 + n + len + 8, 0, total - (n + len + 8));
	pos = ((loff_t)log_offset_kb + 2 * (loff_t)log_slot_kb) * 1024;
	w = __kernel_write(f, log_text2, total, &pos);
	pr_emerg("k618_stall: copy into the third slot: wrote %zd of %zu\n", w, total);
	fput(f);
}

static int log_wd_fn(void *unused)
{
	bool told = false;

	/* real time, so that ordinary work being starved is something it can still report */
	sched_set_fifo_low(current);
	while (!kthread_should_stop()) {
		unsigned long done;
		unsigned int ms;

		schedule_timeout_idle(msecs_to_jiffies(500));
		done = READ_ONCE(log_done_at);
		if (!log_file || !done)
			continue;
		ms = jiffies_to_msecs(jiffies - done);
		if (ms < log_stall_ms) {
			told = false;
			continue;
		}
		if (told)
			continue;
		told = true;
		pr_emerg("k618_stall: no copy for %u ms; the writer is %s (seq %u, pid %d)\n", ms,
			 log_phase_name[READ_ONCE(log_phase) & 3], log_seq, READ_ONCE(log_worker));
		log_stall_stacks(READ_ONCE(log_worker));
		log_stall_copy();
	}
	return 0;
}

/* Device compatibility. */
static unsigned int log_ring_kb = 16384;
module_param(log_ring_kb, uint, 0444);
static unsigned int log_ring_ms = 20;
module_param(log_ring_ms, uint, 0444);
/* blocks one tick may write: early boot makes ~150 KB/s of log, in bursts */
static unsigned int log_ring_batch = 24;
module_param(log_ring_batch, uint, 0444);
/* Device compatibility. */
static unsigned int log_ring_keep_kb = 32768;
module_param(log_ring_keep_kb, uint, 0444);
/* Device compatibility. */
static char *log_ring_skip = "";
module_param(log_ring_skip, charp, 0444);
#define RING_HEAD 96
#define SKIP_MAX 8
static struct task_struct *log_ring_task;
static char *log_ring_buf;		/* one page: a direct write wants a block-aligned buffer */
static struct file *log_ring_file;	/* O_DIRECT | O_DSYNC: a write of its own needs no flush */
static struct kmsg_dump_iter log_ring_iter;
/* where log_ring_iter stood after the last block that reached the disk */
static struct kmsg_dump_iter log_ring_done;
static unsigned int log_ring_idx, log_ring_blocks, log_ring_keep, log_ring_err;
static u32 log_ring_run;
static char skip_pat[SKIP_MAX][40];
static unsigned int skip_n, skip_count[SKIP_MAX];

/* where block idx goes: the kept part once, the rest of the ring in turn */
static loff_t log_ring_pos(unsigned int idx)
{
	unsigned int at = idx;

	if (idx >= log_ring_keep)
		at = log_ring_keep + (idx - log_ring_keep) % (log_ring_blocks - log_ring_keep);
	return ((loff_t)log_offset_kb + 3 * (loff_t)log_slot_kb) * 1024 + (loff_t)at * BLK;
}

static void log_skip_parse(void)
{
	const char *p = log_ring_skip;

	while (*p && skip_n < SKIP_MAX) {
		size_t l = strcspn(p, ",");

		if (l && l < sizeof(skip_pat[0]))
			memcpy(skip_pat[skip_n++], p, l);
		p += l;
		if (*p == ',')
			p++;
	}
}

static bool log_skip(const char *line, size_t len)
{
	unsigned int i;

	for (i = 0; i < skip_n; i++) {
		if (strnstr(line, skip_pat[i], len)) {
			skip_count[i]++;
			return true;
		}
	}
	return false;
}

/* once a minute at most, and only when something more was left out */
static void log_skip_report(void)
{
	static unsigned long next;
	static unsigned int told;
	unsigned int i, total = 0;
	char line[320];
	int n = 0;

	for (i = 0; i < skip_n; i++)
		total += skip_count[i];
	if (total == told || (next && time_before(jiffies, next)))
		return;
	told = total;
	next = jiffies + 60 * HZ;
	for (i = 0; i < skip_n; i++)
		n += scnprintf(line + n, sizeof(line) - n, "%s %u '%s'", i ? "," : "", skip_count[i],
			       skip_pat[i]);
	pr_info("k618_probe: left out of the ring so far:%s\n", line);
}

/*
 * /proc/net/k618_log reads the ring back through a handle of its own, opened here by the ring's
 * thread: the writer's handle is write-only.
 */
static struct file *log_view_file;

/* the direct handle, onto the same disk the copies have already opened */
static void log_ring_open(void)
{
	struct file *f, *v;
	char path[80];

	if (log_ring_file || !log_file || !log_ring_buf)
		return;
	if (*log_path)
		f = get_file(log_file);		/* a file (the QEMU check): the same handle */
	else
		f = bdev_file_open_by_dev(file_bdev(log_file)->bd_dev, BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(f)) {
		pr_emerg("k618_probe: log ring has no handle: %ld\n", PTR_ERR(f));
		log_ring_err++;
		return;
	}
	log_make_direct(f);
	log_ring_blocks = max(1u, (unsigned int)(log_ring_kb * 1024 / BLK));
	log_ring_keep = min(log_ring_keep_kb * 1024 / BLK, log_ring_blocks / 2);
	log_skip_parse();
	log_ring_run = get_random_u32();
	kmsg_dump_rewind(&log_ring_iter);
	log_ring_done = log_ring_iter;
	log_ring_file = f;
	/* and one to read it back with, for /proc/net/k618_log */
	if (*log_path) {
		log_name(path, sizeof(path));
		v = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
	} else {
		v = bdev_file_open_by_dev(file_bdev(log_file)->bd_dev, BLK_OPEN_READ, NULL, NULL);
		if (!IS_ERR(v))
			log_make_direct(v);
	}
	if (!IS_ERR(v))
		WRITE_ONCE(log_view_file, v);
	pr_emerg("k618_probe: log ring on: %u blocks at +%u KB (the first %u kept), every %u ms, %u a tick, run %08x; %u texts left out; read-back %s\n",
		 log_ring_blocks, log_offset_kb + 3 * log_slot_kb, log_ring_keep, log_ring_ms,
		 log_ring_batch, log_ring_run, skip_n, IS_ERR(v) ? "none" : "on");
}

/*
 * At most max blocks of whatever printk has written since the last call. Each block says its own
 * index, so the reader can order them after the ring has wrapped, and ends in K618END with the rest
 * zeroed. Returns how many blocks were written.
 */
static unsigned int log_ring_drain(unsigned int max)
{
	unsigned int done = 0;
	loff_t pos;

	if (!log_ring_file)
		return 0;
	while (done < max) {
		struct kmsg_dump_iter from = log_ring_iter;
		size_t room = BLK - RING_HEAD - 8, used = 0, len;
		char *text = log_ring_buf + RING_HEAD;
		int n;

		/* whole records: one that would not fit waits for the next block */
		while (room - used >= 256 &&
		       kmsg_dump_get_line(&log_ring_iter, true, text + used, room - used, &len)) {
			if (!log_skip(text + used, len))
				used += len;
		}
		if (!used)
			break;
		n = snprintf(log_ring_buf, RING_HEAD, "K618RNG %u at %llu ms, %zu bytes, run %08x\n",
			     log_ring_idx, div_u64(local_clock(), NSEC_PER_MSEC), used, log_ring_run);
		n = min(n, RING_HEAD - 1);
		memset(log_ring_buf + n, '\n', RING_HEAD - n);
		memcpy(text + used, "K618END\n", 8);
		memset(text + used + 8, 0, room - used);
		pos = log_ring_pos(log_ring_idx);
		if (__kernel_write(log_ring_file, log_ring_buf, BLK, &pos) != BLK) {
			/* Device compatibility. */
			log_ring_iter = from;
			log_ring_err++;
			break;
		}
		log_ring_done = log_ring_iter;
		log_ring_idx++;
		done++;
	}
	return done;
}

static int log_ring_fn(void *unused)
{
	/* the lowest real-time priority: above every ordinary thread, below the frame pipeline's */
	sched_set_fifo_low(current);
	while (!kthread_should_stop()) {
		schedule_timeout_idle(msecs_to_jiffies(log_ring_ms));
		log_ring_open();
		log_ring_drain(log_ring_batch);
		log_skip_report();
	}
	return 0;
}

/*
 * /proc/net/k618_log: the ring read back from the disk as plain text, for an adb shell on 6.18 --
 * there is no root there to read the partition with, and dmesg is not shell's to read. /proc/net
 * because of the phone's policy: a file of a module's own anywhere else in /proc or /sys gets the
 * generic proc or sysfs label, which shell may not even stat (tried on the phone under 5.15:
 * /proc/mi_display, /sys/kernel/fscaps), while one under /proc/net gets proc_net (genfscon "/net"),
 * which shell reads, as it reads /proc/net/dev. This run's blocks only, in index order, as they
 * were when the file was opened: the kept start of the boot, then whatever of the rest the ring
 * still holds. The read asks the policy nothing, like the writes (__kernel_read is not exported,
 * so its body is here).
 */
struct log_view {
	char *blk;		/* a page: a direct read wants an aligned buffer */
	unsigned int end;	/* blocks written when the file was opened */
	unsigned int kept;	/* of those, the ones in the kept part */
	unsigned int lo;	/* the oldest index the wrapping part still holds */
	unsigned int at;	/* the record show() is on: 0 the header, then blocks and the gap */
};

static unsigned int log_view_records(const struct log_view *v)
{
	return 1 + v->kept + (v->lo > v->kept) + (v->end - v->lo);
}

static ssize_t log_view_read(char *blk, unsigned int idx)
{
	struct kvec iov = { .iov_base = blk, .iov_len = BLK };
	struct file *f = READ_ONCE(log_view_file);
	struct iov_iter iter;
	struct kiocb kiocb;

	if (!f)
		return -ENODEV;
	init_sync_kiocb(&kiocb, f);
	kiocb.ki_pos = log_ring_pos(idx);
	iov_iter_kvec(&iter, ITER_DEST, &iov, 1, BLK);
	return f->f_op->read_iter(&kiocb, &iter);
}

static int log_view_show(struct seq_file *m, void *p)
{
	struct log_view *v = p;
	unsigned int r = v->at, idx, got, run;
	unsigned long long ms;
	size_t len;
	ssize_t n;

	if (!r) {
		seq_printf(m, "k618_log: run %08x, %u blocks written: %u kept from the start, %u overwritten, %u after them; %u write errors\n",
			   log_ring_run, v->end, v->kept, v->lo - v->kept, v->end - v->lo, log_ring_err);
		return 0;
	}
	r--;
	if (r < v->kept) {
		idx = r;
	} else if (v->lo > v->kept && r == v->kept) {
		seq_printf(m, "\n[k618_log: blocks %u..%u were overwritten]\n\n", v->kept, v->lo - 1);
		return 0;
	} else {
		idx = v->lo + (r - v->kept - (v->lo > v->kept));
	}
	n = log_view_read(v->blk, idx);
	if (n != BLK) {
		seq_printf(m, "\n[k618_log: block %u could not be read: %zd]\n", idx, n);
		return 0;
	}
	if (sscanf(v->blk, "K618RNG %u at %llu ms, %zu bytes, run %x", &got, &ms, &len, &run) != 4 ||
	    got != idx || run != log_ring_run || len > BLK - RING_HEAD - 8) {
		seq_printf(m, "\n[k618_log: block %u was rewritten during this read]\n", idx);
		return 0;
	}
	seq_write(m, v->blk + RING_HEAD, len);
	return 0;
}

static void *log_view_start(struct seq_file *m, loff_t *pos)
{
	struct log_view *v = m->private;

	if (*pos >= log_view_records(v))
		return NULL;
	v->at = *pos;
	return v;
}

static void *log_view_next(struct seq_file *m, void *p, loff_t *pos)
{
	++*pos;
	return log_view_start(m, pos);
}

static void log_view_stop(struct seq_file *m, void *p)
{
}

static const struct seq_operations log_view_seq = {
	.start = log_view_start,
	.next = log_view_next,
	.stop = log_view_stop,
	.show = log_view_show,
};

static int log_view_open(struct inode *inode, struct file *file)
{
	struct log_view *v = __seq_open_private(file, &log_view_seq, sizeof(*v));
	unsigned int wrap;

	if (!v)
		return -ENOMEM;
	v->blk = (char *)__get_free_page(GFP_KERNEL);
	if (!v->blk) {
		seq_release_private(inode, file);
		return -ENOMEM;
	}
	v->end = READ_ONCE(log_ring_idx);
	wrap = log_ring_blocks - log_ring_keep;
	v->kept = min(v->end, log_ring_keep);
	v->lo = v->end - v->kept > wrap ? v->end - wrap : v->kept;
	return 0;
}

static int log_view_release(struct inode *inode, struct file *file)
{
	struct log_view *v = ((struct seq_file *)file->private_data)->private;

	free_page((unsigned long)v->blk);
	return seq_release_private(inode, file);
}

/* read_iter, not read: kernel_read() (log_view_test) takes only a file that has one */
static const struct proc_ops log_view_ops = {
	.proc_open = log_view_open,
	.proc_read_iter = seq_read_iter,
	.proc_lseek = seq_lseek,
	.proc_release = log_view_release,
};
static bool log_view_on;

/* log_view_test=<ms>: read /proc/net/k618_log back once at that time, to see it work in QEMU */
static unsigned int log_view_test;
module_param(log_view_test, uint, 0444);

static void k618_vtest_work(struct work_struct *w)
{
	loff_t pos = 0;
	size_t total = 0;
	char *b, *nl;
	struct file *f;
	ssize_t n;

	b = kvmalloc(65536, GFP_KERNEL);
	f = b ? filp_open("/proc/net/k618_log", O_RDONLY, 0) : ERR_PTR(-ENOMEM);
	if (IS_ERR(f)) {
		pr_emerg("k618_probe: log_view_test: open: %ld\n", PTR_ERR(f));
		kvfree(b);
		return;
	}
	while ((n = kernel_read(f, b, 65536 - 1, &pos)) > 0) {
		if (!total) {
			b[n] = '\0';
			nl = strchr(b, '\n');
			if (nl)
				*nl = '\0';
			pr_emerg("k618_probe: log_view_test: first line: %s\n", b);
		}
		total += n;
	}
	filp_close(f, NULL);
	kvfree(b);
	pr_emerg("k618_probe: log_view_test: read %zu bytes (last read %zd)\n", total, n);
}
static DECLARE_DELAYED_WORK(k618_vtest_dw, k618_vtest_work);

/*
 * A panic stops the other CPUs and never schedules again, so nothing of ours runs after it: the
 * notifier itself has to write. Interrupts come back on for the write (the disk's completion
 * interrupt on this CPU is what it waits for) and go off again afterwards, and the number of blocks
 * is bounded, so a disk that no longer answers cannot make this worse than it already is. An oops
 * is not written from here (k618_die_event says why): the die notifier only calls this from
 * interrupt context, where it writes nothing either and waits for the ring's thread.
 */
static unsigned int log_panic_blocks = 4096;
module_param(log_panic_blocks, uint, 0444);
static atomic_t log_panic_busy = ATOMIC_INIT(0);
static unsigned int log_panic_runs;

static void log_panic_drain(const char *why, struct pt_regs *regs)
{
	unsigned long flags;
	unsigned int n, i, from;

	if (!log_ring_file || log_panic_runs >= 3 || atomic_cmpxchg(&log_panic_busy, 0, 1))
		return;
	log_panic_runs++;
	if (in_interrupt()) {
		/* Interrupt-safe diagnostic capture. */
		pr_emerg("k618_probe: %s in interrupt context: the ring's thread writes it\n", why);
		if (regs)
			show_regs(regs);
		from = READ_ONCE(log_ring_idx);
		if (log_ring_task)
			wake_up_process(log_ring_task);
		for (i = 0; i < 300; i++) {
			mdelay(1);
			if (i >= 60 && READ_ONCE(log_ring_idx) > from + 1)
				break;
		}
		atomic_set(&log_panic_busy, 0);
		return;
	}
	/* Device compatibility. */
	pr_emerg("k618_probe: %s: writing the rest of the log (%u blocks so far, %u errors)\n", why,
		 log_ring_idx, log_ring_err);
	/* Device compatibility. */
	log_ring_iter = log_ring_done;
	local_irq_save(flags);
	local_irq_enable();
	n = log_ring_drain(log_panic_blocks);
	local_irq_restore(flags);
	pr_emerg("k618_probe: %s: %u blocks written\n", why, n);
	atomic_set(&log_panic_busy, 0);
}

static int k618_panic_event(struct notifier_block *nb, unsigned long ev, void *p)
{
	log_panic_drain("panic", NULL);
	return NOTIFY_DONE;
}

static int k618_die_event(struct notifier_block *nb, unsigned long ev, void *p)
{
	struct die_args *args = p;

	if (ev != DIE_OOPS)
		return NOTIFY_DONE;
	if (in_interrupt()) {
		log_panic_drain("oops", args ? args->regs : NULL);
		return NOTIFY_DONE;
	}
	/* Device compatibility. */
	pr_emerg("k618_probe: oops in task context: the ring's thread writes it\n");
	if (log_ring_task)
		wake_up_process(log_ring_task);
	return NOTIFY_DONE;
}

/* before panicsig's own notifier, which waits for its grid and would be reset away first */
static struct notifier_block k618_panic_nb = {
	.notifier_call = k618_panic_event,
	.priority = 100,
};
static struct notifier_block k618_die_nb = {
	.notifier_call = k618_die_event,
	.priority = 100,
};
static bool panic_nb_on, die_nb_on;

/* log_panic_test=<ms>: panic on purpose, to see in QEMU that the ring really carries it. Never set
 * on the phone -- the image's own options leave it at 0.
 */
static unsigned int log_panic_test;
module_param(log_panic_test, uint, 0444);

static void k618_ptest_work(struct work_struct *w)
{
	pr_emerg("k618_probe: log_panic_test: a panic on purpose at %llu ms\n",
		 div_u64(local_clock(), NSEC_PER_MSEC));
	panic("k618_probe: log_panic_test");
}
static DECLARE_DELAYED_WORK(k618_ptest_dw, k618_ptest_work);

/* Interrupt-safe diagnostic capture. */
static unsigned int log_oops_irq_test;
module_param(log_oops_irq_test, uint, 0444);
static struct timer_list k618_otest_timer;

static void k618_otest_fn(struct timer_list *t)
{
	int *volatile p = NULL;

	pr_emerg("k618_probe: log_oops_irq_test: an oops on purpose in interrupt context at %llu ms\n",
		 div_u64(local_clock(), NSEC_PER_MSEC));
	*p = 1;
}

/* Device compatibility. */
static unsigned int log_oops_task_test;
module_param(log_oops_task_test, uint, 0444);
static unsigned long k618_otask_delay;

static int k618_otask_fn(void *unused)
{
	int *volatile p = NULL;

	schedule_timeout_uninterruptible(k618_otask_delay);
	pr_emerg("k618_probe: log_oops_task_test: an oops on purpose in task context at %llu ms\n",
		 div_u64(local_clock(), NSEC_PER_MSEC));
	*p = 1;
	return 0;
}

/* Device compatibility. */
static char watch_comm[160];
module_param_string(watch_comm, watch_comm, sizeof(watch_comm), 0444);
/* how many lines to allow it, so a loop cannot fill the log */
static unsigned int watch_max = 600;
module_param(watch_max, uint, 0444);
static unsigned int watched;

/*
 * watch_block=<part of a path>: an ioctl that never returns leaves no line at all, because the
 * return is where the one above logs. For a path holding this text both ends are logged, whether
 * the call succeeds or not -- so a call still inside the kernel is an "ioctl ... ->" with no answer
 * after it, and the ones that did return read as a trace of what the process asked of that device.
 */
static char watch_block[32];
module_param_string(watch_block, watch_block, sizeof(watch_block), 0444);

/* does this name start with one of the list's names? */
static bool name_listed(const char *p, const char *name)
{
	if (!*p)
		return false;
	while (*p) {
		size_t l = strcspn(p, ",");

		if (l && !strncmp(name, p, min_t(size_t, l, TASK_COMM_LEN - 1)))
			return true;
		p += l;
		if (*p == ',')
			p++;
	}
	return false;
}

/*
 * by the thread's own name or its process's: the composer does its DRM work in binder threads
 * ("binder:2324_2"), which only the process's name gives away
 */
static bool comm_watched(void)
{
	return name_listed(watch_comm, current->comm) ||
	       name_listed(watch_comm, current->group_leader->comm);
}

static bool watch_this_task(void)
{
	return watched < watch_max && comm_watched();
}

/* Device compatibility. */
static unsigned int watch_skip;
module_param(watch_skip, uint, 0444);
static unsigned int skipped;

/* Device compatibility. */
unsigned int k618_probe_flush(unsigned int spin_ms);	/* 6.18: -Wmissing-prototypes is an error */
unsigned int k618_probe_flush(unsigned int spin_ms)
{
	if (!READ_ONCE(log_file))
		return 0;
	return log_now(spin_ms);
}
EXPORT_SYMBOL_GPL(k618_probe_flush);

/* 1 the log is written (go and read it), 2..7 why it is not */
static unsigned int log_state(void)
{
	if (log_writes)
		return 1;
	switch (log_err) {
	case 0:
		return 2;	/* open, nothing written yet */
	case -EAGAIN:
		return 3;	/* the work never ran */
	case -ENOENT:
		return 4;	/* nothing of that name */
	case -EXDEV:
		return 5;	/* there, but not on a disk: the mount never happened */
	case -EACCES:
	case -EPERM:
	case -EROFS:
		return 6;
	}
	return 7;
}

/* how far the first stage got, by the mounts and by what init is waiting in */
static unsigned int first_stage_state(void)
{
	unsigned int blk, cls;

	/* for the log alone: how many tasks there are and how many run */
	if (read_file("/proc/loadavg") >= 0)
		pr_emerg("k618_probe: loadavg: %s\n", buf);

	blk = block_mounts();
	if (blk == UINT_MAX)
		return 9;
	cls = pid1_class();
	pr_emerg("k618_probe: %u block mounts, pid 1 class %u\n", blk, cls);
	if (!blk)
		return 1;
	if (blk > 1)
		return 8;
	switch (cls) {
	case SC_POLL:
		return 2;
	case SC_IOCTL:
		return 3;
	case SC_MOUNT:
		return 4;
	case SC_IO:
		return 5;
	case SC_WAIT:
		return 6;
	}
	return 7;
}

/* Device compatibility. */
static unsigned int k618_probe_look(void)
{
	if (*log_part || *log_path) {
		unsigned int s = log_state();

		pr_emerg("k618_probe: log state %u (%u writes, err %d)\n", s, log_writes, log_err);
		return s;
	}
	return first_stage_state();
}

static void k618_probe_work(struct work_struct *w)
{
	unsigned int code = force_code ? force_code : k618_probe_look();

	panic("k618_probe: code %u", code);
}

static DECLARE_DELAYED_WORK(k618_probe_dw, k618_probe_work);

/*
 * stop_at_write_open: let the second stage run on past the SELinux policy load and stop it at the
 * first open for writing of a UFS LUN or one of its partitions (an "sd*" disk) other than the one
 * the log is on -- before e2fsck checks persist, before persist is mounted, before dm or vold open
 * userdata, before anything writes misc. A kprobe on bdev_open sends the opener to k618_stop_open
 * instead, so bdev_open never runs: there it says who opened what, writes the log while it still
 * can, and panics with a message k618_panicsig reads as its own stop (kind 6 -> top_ms).
 */
static bool stop_at_write_open;
module_param(stop_at_write_open, bool, 0444);
/* how long to spin for the log when the stop cannot write it itself */
static unsigned int stop_log_ms = 700;
module_param(stop_log_ms, uint, 0444);
static bool write_armed, kprobe_on, watch_on;

static int k618_stop_open(struct block_device *bdev, blk_mode_t open_mode)
{
	const char *vol = bdev->bd_meta_info ? (const char *)bdev->bd_meta_info->volname : "";
	struct task_struct *parent;
	unsigned int n;

	rcu_read_lock();
	parent = rcu_dereference(current->real_parent);
	pr_emerg("k618_probe: stopping at a write open (mode %#x) of %pg (%s) by %s[%d], parent %s[%d]; preemptible %d, irqs_disabled %d\n",
		 open_mode, bdev, vol, current->comm, task_pid_nr(current),
		 parent ? parent->comm : "?", parent ? task_pid_nr(parent) : -1,
		 preemptible(), irqs_disabled());
	rcu_read_unlock();
	dump_stack();
	/* twice: the second copy carries the first one's own result line, and so this one's state */
	n = log_now(stop_log_ms);
	pr_emerg("k618_probe: %u log copies written by the stop's first wait (err %d)\n", n, log_err);
	log_now(stop_log_ms);
	panic("k618_panicsig: stop at a write open of %pg (%s) by %s[%d]", bdev, vol, current->comm,
	      task_pid_nr(current));
}

/* Device compatibility. */
static bool deny_write_open;
module_param(deny_write_open, bool, 0444);
/* refusals to print one by one; past that they go on silently, counted */
static unsigned int deny_max = 24;
module_param(deny_max, uint, 0444);
/* of those, how many with a stack */
static unsigned int deny_stacks = 4;
module_param(deny_stacks, uint, 0444);
/* when to end the run after the first refusal; 0 leaves the end to probe_ms */
static unsigned int deny_panic_ms = 10000;
module_param(deny_panic_ms, uint, 0444);
static atomic_t denied = ATOMIC_INIT(0);
/* Device compatibility. */
static bool deny_code_panic;
module_param(deny_code_panic, bool, 0444);
/*
 * ro_write_open: nothing is refused and nothing is written either -- the write bit is taken out
 * of the open before bdev_open runs, so every partition the second stage opens comes up
 * read-only and the boot carries on as far as read-only takes it (ext4 mounts a cleanly
 * unmounted persist ro; a dirty journal gives EROFS). Each one is printed, and with the log
 * written by __kernel_write the whole run comes back in /metadata.
 */
static bool ro_write_open;
module_param(ro_write_open, bool, 0444);
/*
 * allow_vols: the GPT names this test is allowed to write, comma separated -- their write opens go
 * through untouched while every other one is still refused, so one run shows the whole second stage
 * with only the named partitions at risk. ro_write_open is not a substitute: taking the write bit
 * out of the open stops a write(2) from userspace, but not a filesystem's own writes --
 * setup_bdev_super (fs/super.c) only refuses a mount when the *device* is marked read-only, and
 * submit_bio_noacct checks the same flag, so ext4 would mount read-write over our read-only open
 * and write anyway.
 */
static char allow_vols[128];
module_param_string(allow_vols, allow_vols, sizeof(allow_vols), 0444);
static atomic_t allowed = ATOMIC_INIT(0);
/* Device compatibility. */
static char repair_comm[192] =
	"fsck.f2fs,make_f2fs,mkfs.f2fs,sload_f2fs,dump.f2fs,resize.f2fs,defrag.f2fs,"
	"e2fsck,mke2fs,make_ext4fs,fsck.ext4,mkfs.ext4,fsck,mkfs,sgdisk,wipe";
module_param_string(repair_comm, repair_comm, sizeof(repair_comm), 0444);
static char repair_vols[128] = "userdata";
module_param_string(repair_vols, repair_vols, sizeof(repair_vols), 0444);
static atomic_t repaired = ATOMIC_INIT(0);
/* Device compatibility. */
static char nv_vols[64];
module_param_string(nv_vols, nv_vols, sizeof(nv_vols), 0444);
static char nv_comm[64] = "rmt_storage";
module_param_string(nv_comm, nv_comm, sizeof(nv_comm), 0444);

/* Device compatibility. */
static char raw_disks[64];
module_param_string(raw_disks, raw_disks, sizeof(raw_disks), 0444);
static char raw_comm[64] = "boot@1.2-servic";
module_param_string(raw_comm, raw_comm, sizeof(raw_comm), 0444);

/* is this name one of the comma-separated list? (whole names) */
static bool name_in(const char *list, const char *name, size_t n)
{
	if (!name || !n || !*list)
		return false;
	while (*list) {
		size_t l = strcspn(list, ",");

		if (l == n && !strncmp(list, name, n))
			return true;
		list += l;
		if (*list == ',')
			list++;
	}
	return false;
}

/* this process's name, and a volume's */
static bool comm_in(const char *list)
{
	return name_in(list, current->comm, strnlen(current->comm, TASK_COMM_LEN));
}

static bool vol_in(const char *list, const char *vol)
{
	return name_in(list, vol, vol ? strlen(vol) : 0);
}

/* is this partition one of allow_vols? */
static bool vol_allowed(const char *vol)
{
	const char *p = allow_vols;
	size_t n;

	if (!vol || !*vol || !*p)
		return false;
	n = strlen(vol);
	while (*p) {
		size_t l = strcspn(p, ",");

		if (l == n && !strncmp(p, vol, n))
			return true;
		p += l;
		while (*p == 0x2c)
			p++;
	}
	return false;
}
static unsigned int deny_wait_ms = 20000;
module_param(deny_wait_ms, uint, 0444);
static unsigned int deny_net_ms = 3000;
module_param(deny_net_ms, uint, 0444);
static unsigned int deny_panic_code;

/* the partition names the second stage could want to write first, by code */
static const struct {
	const char *name;
	unsigned int code;
} deny_codes[] = {
	{ "persist", 1 },					/* e2fsck, then the mount */
	{ "misc", 2 },						/* the bootloader control block */
	{ "userdata", 3 },					/* dm-default-key, fsck, the mount */
	{ "super", 4 },						/* dynamic partitions, snapshots */
	{ "modemst1", 5 }, { "modemst2", 5 },			/* modem NV */
	{ "fsg", 5 }, { "fsc", 5 },
	{ "frp", 6 }, { "devinfo", 6 },				/* small vendor blobs */
	{ "ssd", 6 }, { "keystore", 6 },
	{ "logfs", 7 }, { "minidump", 7 },			/* the ROM's own log areas */
	{ "rawdump", 7 }, { "crash_history", 7 },
	{ "oops", 7 }, { "dbg", 7 },
};

/* 8: some other name (the log has it), 9: no name at all, so a whole LUN */
static unsigned int deny_code(const char *vol)
{
	int i;

	if (!vol || !*vol)
		return 9;
	for (i = 0; i < ARRAY_SIZE(deny_codes); i++)
		if (!strcmp(vol, deny_codes[i].name))
			return deny_codes[i].code;
	return 8;
}

static void k618_deny_work(struct work_struct *w);
static DECLARE_DELAYED_WORK(k618_deny_dw, k618_deny_work);

static void k618_deny_work(struct work_struct *w)
{
	unsigned int code = READ_ONCE(deny_panic_code);
	int n = atomic_read(&denied);

	if (code)	/* the refusal asked for this and did not come back from writing the log */
		panic("k618_probe: code %u (%d refused, the refusal's own ending never came)", code, n);
	write_log();
	if (!n)
		panic("k618_panicsig: stop: nothing asked to write a disk by %u ms", deny_wait_ms);
	if (ro_write_open)
		panic("k618_panicsig: stop at %u ms: %d write opens made read-only",
			      deny_wait_ms, n);
	panic("k618_panicsig: stop after %d refused write opens", n);
}

static int k618_deny_open(struct block_device *bdev, blk_mode_t open_mode)
{
	const char *vol = bdev->bd_meta_info ? (const char *)bdev->bd_meta_info->volname : "";
	struct task_struct *parent;
	int n = atomic_inc_return(&denied);

	if (n <= deny_max) {
		rcu_read_lock();
		parent = rcu_dereference(current->real_parent);
		pr_emerg("k618_probe: refusing write open %d (mode %#x) of %pg (%s) to %s[%d], parent %s[%d]\n",
			 n, open_mode, bdev, vol, current->comm, task_pid_nr(current),
			 parent ? parent->comm : "?", parent ? task_pid_nr(parent) : -1);
		rcu_read_unlock();
		if (n <= deny_stacks)
			dump_stack();
	}
	if (n == 1 && deny_code_panic) {
		unsigned int code = deny_code(vol);

		/* the net first, so a log write that never comes back still leaves the reading */
		WRITE_ONCE(deny_panic_code, code);
		mod_delayed_work(system_unbound_wq, &k618_deny_dw, msecs_to_jiffies(deny_net_ms));
		write_log();
		panic("k618_probe: code %u (refused a write open of %pg (%s) to %s[%d])", code, bdev,
		      vol, current->comm, task_pid_nr(current));
	}
	if (n == 1 && deny_panic_ms)
		mod_delayed_work(system_unbound_wq, &k618_deny_dw, msecs_to_jiffies(deny_panic_ms));
	return -EPERM;
}

/*
 * a repair or format tool on a volume whose data must survive: refused, and a copy of the log goes
 * out at once. The instruction pointer was moved here, so this runs in the caller's own context
 * (as k618_deny_open does) and may wait for the copy.
 */
static int k618_deny_repair_open(struct block_device *bdev, blk_mode_t open_mode)
{
	pr_emerg("k618_probe: %s[%d] may not rewrite %pg (%s): refused %d (the data stays as it is)\n",
		 current->comm, task_pid_nr(current), bdev,
		 bdev->bd_meta_info ? (const char *)bdev->bd_meta_info->volname : "",
		 atomic_inc_return(&repaired));
	log_now(200);
	return -EPERM;
}

/* the partition the log goes to: the file's own (log_path) or the one opened by name (log_part) */
static bool is_log_bdev(struct block_device *bdev)
{
	struct super_block *sb;

	if (!log_file) {
		/*
		 * Not open yet: if the log goes to a partition, that one has to stay writable or
		 * we would refuse our own open and never write a line.
		 */
		return !*log_path && *log_part && !strcmp(part_name(bdev), log_part);
	}
	if (!*log_path)
		return file_bdev(log_file) == bdev;
	sb = file_inode(log_file)->i_sb;
	return sb && sb->s_bdev == bdev;
}

/* bdev_open(bdev, mode, ...), before it takes any lock of the disk */
static int k618_bdev_open_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct block_device *bdev = (struct block_device *)regs->regs[0];
	blk_mode_t open_mode = (blk_mode_t)regs->regs[1];

	if (!READ_ONCE(write_armed) || !(open_mode & BLK_OPEN_WRITE) || !bdev || !bdev->bd_disk)
		return 0;
	if (strncmp(bdev->bd_disk->disk_name, "sd", 2))
		return 0;
	/* the log's own partition keeps being written */
	if (is_log_bdev(bdev))
		return 0;
	/* the ones this test may write go through as they are */
	if (bdev->bd_meta_info &&
	    vol_allowed((const char *)bdev->bd_meta_info->volname)) {
		const char *vol = (const char *)bdev->bd_meta_info->volname;

		/* ...except a repair or format tool on a volume whose data must survive */
		if (vol_in(repair_vols, vol) && comm_in(repair_comm)) {
			instruction_pointer_set(regs, (unsigned long)k618_deny_repair_open);
			return 1;
		}
		if (atomic_inc_return(&allowed) <= deny_max)
			pr_emerg("k618_probe: write open of %pg (%s) to %s[%d] allowed\n", bdev,
				 (const char *)bdev->bd_meta_info->volname, current->comm,
				 task_pid_nr(current));
		return 0;
	}
	/* the modem's own volumes, to the program that serves them and nobody else */
	if (bdev->bd_meta_info && *nv_vols &&
	    vol_in(nv_vols, (const char *)bdev->bd_meta_info->volname) && comm_in(nv_comm)) {
		if (atomic_inc_return(&allowed) <= deny_max)
			pr_emerg("k618_probe: write open of %pg (%s) to %s[%d] allowed (nv_vols)\n", bdev,
				 (const char *)bdev->bd_meta_info->volname, current->comm,
				 task_pid_nr(current));
		return 0;
	}
	/* a whole disk to the one program that owns what is written there (the GPT's slot flags) */
	if (*raw_disks && !bdev_is_partition(bdev) &&
	    name_in(raw_disks, bdev->bd_disk->disk_name, strlen(bdev->bd_disk->disk_name)) &&
	    comm_in(raw_comm)) {
		if (atomic_inc_return(&allowed) <= deny_max)
			pr_emerg("k618_probe: write open of %pg to %s[%d] allowed (raw_disks)\n",
				 bdev, current->comm, task_pid_nr(current));
		return 0;
	}
	if (ro_write_open) {
		blk_mode_t ro = open_mode & ~(blk_mode_t)BLK_OPEN_WRITE;
		int n = atomic_inc_return(&denied);

		/* bdev_open runs, only without the write bit: x1 is its mode argument */
		regs->regs[1] = ro;
		if (n <= deny_max)
			pr_emerg("k618_probe: write open %d of %pg (%s) to %s[%d] made read-only (mode %#x -> %#x)\n",
				 n, bdev, bdev->bd_meta_info ?
				 (const char *)bdev->bd_meta_info->volname : "",
				 current->comm, task_pid_nr(current), open_mode, ro);
		return 0;
	}
	/* on to ours instead of single-stepping bdev_open: bdev_open never runs */
	instruction_pointer_set(regs, deny_write_open ? (unsigned long)k618_deny_open
						     : (unsigned long)k618_stop_open);
	return 1;
}

static struct kprobe k618_bdev_open_kp = {
	.symbol_name = "bdev_open",
	.pre_handler = k618_bdev_open_pre,
};

/* the watched process's opens: do_filp_open(dfd, struct filename *, open_flags *) */
struct k618_open {
	char name[96];
};

static int k618_filp_open_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct k618_open *d = (struct k618_open *)ri->data;
	struct filename *fn = (struct filename *)regs->regs[1];

	d->name[0] = '\0';
	if (!watch_this_task())
		return 1;			/* no return handler for this call */
	if (fn && fn->name)
		strscpy(d->name, fn->name, sizeof(d->name));
	return 0;
}

static int k618_filp_open_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct k618_open *d = (struct k618_open *)ri->data;
	long ret = regs_return_value(regs);

	if (!d->name[0])
		return 0;
	/* everything that failed, and every device it did open -- but not the linker's search */
	if (IS_ERR((void *)ret) && PTR_ERR((void *)ret) == -ENOENT &&
	    (strstr(d->name, "/lib64/") || strstr(d->name, "/lib/")))
		return 0;
	if (IS_ERR((void *)ret)) {
		watched++;
		pr_emerg("k618_watch: %s[%d] open %s: %ld\n", current->comm, task_pid_nr(current),
			 d->name, PTR_ERR((void *)ret));
	} else if (!strncmp(d->name, "/dev/", 5) && strncmp(d->name, "/dev/__properties__/", 20)) {
		watched++;
		pr_emerg("k618_watch: %s[%d] open %s: ok\n", current->comm, task_pid_nr(current),
			 d->name);
	}
	return 0;
}

static struct kretprobe k618_filp_open_krp = {
	.kp.symbol_name = "do_filp_open",
	.entry_handler = k618_filp_open_entry,
	.handler = k618_filp_open_ret,
	.data_size = sizeof(struct k618_open),
	.maxactive = 32,
};

/* and its ioctls: __arm64_sys_ioctl(const struct pt_regs *user) -- fd in x0, request in x1 */
struct k618_ioctl {
	char path[80];
	unsigned int cmd;
	bool both;
};

static int k618_ioctl_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct k618_ioctl *d = (struct k618_ioctl *)ri->data;
	struct pt_regs *user = (struct pt_regs *)regs->regs[0];
	struct file *f;
	const char *p;
	char buf[80];

	d->path[0] = '\0';
	d->both = false;
	if (!watch_this_task() || !user)
		return 1;
	d->cmd = (unsigned int)user->regs[1];
	if (watch_skip && d->cmd == watch_skip) {
		skipped++;
		return 1;
	}
	f = fget_raw((int)user->regs[0]);
	if (!f) {
		strscpy(d->path, "?", sizeof(d->path));
		return 0;
	}
	p = d_path(&f->f_path, buf, sizeof(buf));
	strscpy(d->path, IS_ERR(p) ? "?" : p, sizeof(d->path));
	fput(f);
	if (*watch_block && strstr(d->path, watch_block)) {
		d->both = true;
		watched++;
		pr_emerg("k618_watch: %s[%d] ioctl %#x on %s ->\n", current->comm,
			 task_pid_nr(current), d->cmd, d->path);
	}
	return 0;
}

static int k618_ioctl_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct k618_ioctl *d = (struct k618_ioctl *)ri->data;
	long ret = regs_return_value(regs);

	if (!d->path[0] || (ret >= 0 && !d->both))
		return 0;
	watched++;
	pr_emerg("k618_watch: %s[%d] ioctl %#x on %s: %ld\n", current->comm, task_pid_nr(current),
		 d->cmd, d->path, ret);
	return 0;
}

static struct kretprobe k618_ioctl_krp = {
	.kp.symbol_name = "__arm64_sys_ioctl",
	.entry_handler = k618_ioctl_entry,
	.handler = k618_ioctl_ret,
	.data_size = sizeof(struct k618_ioctl),
	.maxactive = 32,
};

/* the first SELinux policy load arms the stop (selinux_policy_commit's AVC reset notification) */
static int k618_probe_lsm(struct notifier_block *nb, unsigned long event, void *data)
{
	if (event != LSM_POLICY_CHANGE || READ_ONCE(write_armed))
		return NOTIFY_DONE;
	WRITE_ONCE(write_armed, true);
	pr_emerg("k618_probe: policy loaded at %llu ms: the next write open of a disk stops the boot\n",
		 div_u64(local_clock(), NSEC_PER_MSEC));
	/* the policy load itself can sleep, so this copy is certain */
	write_log();
	return NOTIFY_DONE;
}

static struct notifier_block k618_probe_lsm_nb = {
	.notifier_call = k618_probe_lsm,
};

/* Device compatibility. */
static bool watch_log;
module_param(watch_log, bool, 0444);
static char watch_log_tag[96];
module_param_string(watch_log_tag, watch_log_tag, sizeof(watch_log_tag), 0444);
static unsigned int watch_log_max = 1500;
module_param(watch_log_max, uint, 0444);
/* Device compatibility. */
static char watch_log_comm[160];
module_param_string(watch_log_comm, watch_log_comm, sizeof(watch_log_comm), 0444);
static unsigned int logged, log_dropped;

/* the priority liblog puts in front of the tag */
static char k618_prio(unsigned char p)
{
	static const char l[] = "??VDIWEF";

	return p < sizeof(l) - 1 ? l[p] : '?';
}

static bool tag_wanted(const char *tag)
{
	const char *p = watch_log_tag;
	char one[32];

	if (!*p)
		return true;
	while (*p) {
		size_t l = strcspn(p, ",");

		if (l && l < sizeof(one)) {
			memcpy(one, p, l);
			one[l] = '\0';
			if (strstr(tag, one))
				return true;
		}
		p += l;
		if (*p == ',')
			p++;
	}
	return false;
}

struct k618_uiov {
	unsigned long base;
	unsigned long len;
};

static int k618_writev_pre(struct kprobe *kp, struct pt_regs *regs)
{
	struct pt_regs *user = (struct pt_regs *)regs->regs[0];
	struct k618_uiov iov[4];
	unsigned char hdr[11], prio;
	char tag[40], msg[220];
	unsigned long nr;
	size_t l;

	if (!user || logged >= watch_log_max ||
	    !(*watch_log_comm ? name_listed(watch_log_comm, current->comm) ||
				name_listed(watch_log_comm, current->group_leader->comm) :
				comm_watched()))
		return 0;
	nr = user->regs[2];
	if (nr != 4)
		return 0;
	if (copy_from_user_nofault(iov, (void __user *)user->regs[1], sizeof(iov)))
		return 0;
	if (iov[0].len != sizeof(hdr) || iov[1].len != 1 || !iov[2].len || !iov[3].len)
		return 0;
	if (copy_from_user_nofault(hdr, (void __user *)iov[0].base, sizeof(hdr)) ||
	    copy_from_user_nofault(&prio, (void __user *)iov[1].base, 1)) {
		log_dropped++;
		return 0;
	}
	if (hdr[0] > 7 || prio > 7)		/* not a text log message after all */
		return 0;
	l = min(iov[2].len, sizeof(tag) - 1);
	if (copy_from_user_nofault(tag, (void __user *)iov[2].base, l)) {
		log_dropped++;
		return 0;
	}
	tag[l] = '\0';
	if (!tag_wanted(tag))
		return 0;
	l = min(iov[3].len, sizeof(msg) - 1);
	if (copy_from_user_nofault(msg, (void __user *)iov[3].base, l)) {
		log_dropped++;
		return 0;
	}
	msg[l] = '\0';
	logged++;
	pr_emerg("k618_log: %s[%d] %c %s: %s\n", current->comm, task_pid_nr(current),
		 k618_prio(prio), tag, msg);
	return 0;
}

static struct kprobe k618_writev_kp = {
	.symbol_name = "__arm64_sys_writev",
	.pre_handler = k618_writev_pre,
};
static bool writev_on;

/* Device compatibility. */
static char stack_comm[64];
module_param_string(stack_comm, stack_comm, sizeof(stack_comm), 0444);
static unsigned int stack_at_ms;
module_param(stack_at_ms, uint, 0444);
static unsigned int stack_max = 48;
module_param(stack_max, uint, 0444);

static bool comm_starts_in(const char *list, const char *comm)
{
	while (*list) {
		size_t l = strcspn(list, ",");

		if (l && !strncmp(comm, list, min_t(size_t, l, TASK_COMM_LEN - 1)))
			return true;
		list += l;
		if (*list == ',')
			list++;
	}
	return false;
}

static void k618_stacks_work(struct work_struct *w)
{
	static unsigned long ents[24];
	struct task_struct **task, *g, *t;
	char *cmd;
	unsigned int n = 0, i, j, nr;

	task = kvmalloc_array(stack_max, sizeof(*task), GFP_KERNEL);
	cmd = kmalloc(200, GFP_KERNEL);
	if (!task || !cmd) {
		kvfree(task);
		kfree(cmd);
		return;
	}
	/*
	 * the tasks are taken first, under rcu and with a reference each, and looked at afterwards:
	 * reading another process's command line takes its mmap lock and may sleep, which a
	 * read-side critical section may not do.
	 */
	rcu_read_lock();
	for_each_process_thread(g, t) {
		if (n >= stack_max)
			break;
		if (!comm_starts_in(stack_comm, t->comm))
			continue;
		if (!get_task_struct_safe(t))
			continue;
		task[n++] = t;
	}
	rcu_read_unlock();
	pr_emerg("k618_stack: %u threads of '%s' at %u ms\n", n, stack_comm, stack_at_ms);
	for (j = 0; j < n; j++) {
		t = task[j];
		/*
		 * several of this phone's services are called vendor.qti.hardware.something, and a
		 * name is cut to fifteen characters, so the command line is what tells them apart --
		 * printed once per process, for the thread that leads it.
		 */
		if (t == t->group_leader) {
			int l = get_cmdline(t, cmd, 199);

			cmd[l > 0 ? l : 0] = '\0';
			for (i = 0; i + 1 < (unsigned int)max(l, 0); i++)
				if (!cmd[i])
					cmd[i] = ' ';
			pr_emerg("k618_stack: %s[%d] is '%s'\n", t->comm, task_pid_nr(t),
				 l > 0 ? cmd : "?");
		}
		if (task_is_running(t)) {
			pr_emerg("k618_stack: %s[%d] of [%d]: running\n", t->comm, task_pid_nr(t),
				 task_pid_nr(t->group_leader));
			put_task_struct(t);
			continue;
		}
		nr = stack_trace_save_tsk(t, ents, ARRAY_SIZE(ents), 0);
		pr_emerg("k618_stack: %s[%d] of [%d]: state %u, %u frames\n", t->comm,
			 task_pid_nr(t), task_pid_nr(t->group_leader), READ_ONCE(t->__state), nr);
		for (i = 0; i < nr; i++)
			pr_emerg("k618_stack:   %pS\n", (void *)ents[i]);
		put_task_struct(t);
	}
	kvfree(task);
	kfree(cmd);
	pr_emerg("k618_stack: %u log lines (%u dropped), %u ioctls of %#x not logged\n",
		 logged, log_dropped, skipped, watch_skip);
	log_now(200);
}
static DECLARE_DELAYED_WORK(k618_stacks_dw, k618_stacks_work);

/* Device compatibility. */
static bool suspend_veto;
module_param(suspend_veto, bool, 0444);
static unsigned int suspend_max = 20;
module_param(suspend_max, uint, 0444);
static unsigned int suspend_seen;

static int k618_pm_event(struct notifier_block *nb, unsigned long event, void *unused)
{
	const char *what = event == PM_SUSPEND_PREPARE ? "suspend" :
			   event == PM_HIBERNATION_PREPARE ? "hibernation" :
			   event == PM_RESTORE_PREPARE ? "restore" :
			   event == PM_POST_SUSPEND ? "after suspend" : "other";

	if (event == PM_SUSPEND_PREPARE || event == PM_HIBERNATION_PREPARE ||
	    event == PM_RESTORE_PREPARE) {
		suspend_seen++;
		if (suspend_seen <= suspend_max)
			pr_emerg("k618_probe: %s %u asked by %s[%d] at %llu ms -- %s\n", what,
				 suspend_seen, current->comm, task_pid_nr(current),
				 div_u64(local_clock(), NSEC_PER_MSEC),
				 suspend_veto ? "refused (suspend_veto)" : "let through, log written first");
		/* the log onto the disk while nothing is frozen yet: a suspended system writes nothing */
		if (suspend_seen <= suspend_max)
			log_now(300);
		return suspend_veto ? NOTIFY_BAD : NOTIFY_OK;
	}
	if (event == PM_POST_SUSPEND && !suspend_veto && suspend_seen <= suspend_max)
		pr_emerg("k618_probe: resumed at %llu ms\n", div_u64(local_clock(), NSEC_PER_MSEC));
	return NOTIFY_OK;
}

static struct notifier_block k618_pm_nb = {
	.notifier_call = k618_pm_event,
};
static bool pm_nb_on;

/* ms until a time since boot, 0 if it has gone by */
static unsigned long delay_until(unsigned int ms)
{
	u64 now = div_u64(local_clock(), NSEC_PER_MSEC);

	return msecs_to_jiffies(ms > now ? ms - now : 0);
}

/* Interrupt-safe diagnostic capture. */
static char dump_files[256];
module_param_string(dump_files, dump_files, sizeof(dump_files), 0444);
static char dump_drm[64];
module_param_string(dump_drm, dump_drm, sizeof(dump_drm), 0444);
static char dump_at_ms[64];
module_param_string(dump_at_ms, dump_at_ms, sizeof(dump_at_ms), 0444);
static unsigned int dump_max = 8192;
module_param(dump_max, uint, 0444);
static unsigned int dump_times[4], dump_n, dump_i;

static void k618_dump_drm(void)
{
	struct device *d = bus_find_device_by_name(&platform_bus_type, NULL, dump_drm);
	struct drm_device *ddev = d ? dev_get_drvdata(d) : NULL;
	struct drm_printer p;

	/* msm_drm's drvdata is its drm_device once bound (msm_drm_init); anything else is not used */
	if (!ddev || ddev->dev != d) {
		pr_emerg("k618_dump: %s: %s\n", dump_drm, d ? "no DRM device bound" : "no such device");
		put_device(d);
		return;
	}
	p = drm_info_printer(ddev->dev);
	drm_modeset_lock_all(ddev);
	drm_state_dump(ddev, &p);
	drm_modeset_unlock_all(ddev);
	pr_emerg("k618_dump: %s: state printed\n", dump_drm);
	put_device(d);
}

static void k618_dump_file(const char *path)
{
	static char chunk[1024], line[256];
	struct file *f;
	loff_t pos = 0;
	size_t total = 0, ll = 0;
	ssize_t r, i;

	f = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_emerg("k618_dump: %s: %ld\n", path, PTR_ERR(f));
		return;
	}
	/* a seq_file hands out a page at a time, so read until it says no more */
	while (total < dump_max) {
		r = kernel_read(f, chunk, min_t(size_t, sizeof(chunk), dump_max - total), &pos);
		if (r <= 0) {
			if (r < 0)
				pr_emerg("k618_dump: %s: read %zd\n", path, r);
			break;
		}
		total += r;
		for (i = 0; i < r; i++) {
			if (chunk[i] == '\n' || ll == sizeof(line) - 1) {
				line[ll] = '\0';
				pr_emerg("k618_dump: %s: %s\n", path, line);
				ll = 0;
				if (chunk[i] == '\n')
					continue;
			}
			line[ll++] = chunk[i] == '\t' ? ' ' : chunk[i];
		}
	}
	if (ll) {
		line[ll] = '\0';
		pr_emerg("k618_dump: %s: %s\n", path, line);
	}
	pr_emerg("k618_dump: %s: %zu bytes%s\n", path, total, total >= dump_max ? " (cut)" : "");
	filp_close(f, NULL);
}

static void k618_dump_work(struct work_struct *w);
static DECLARE_DELAYED_WORK(k618_dump_dw, k618_dump_work);

static void k618_dump_work(struct work_struct *w)
{
	char path[96];
	const char *p = dump_files;

	pr_emerg("k618_dump: at %llu ms (%u of %u)\n", div_u64(local_clock(), NSEC_PER_MSEC),
		 dump_i + 1, dump_n);
	if (*dump_drm)
		k618_dump_drm();
	while (*p) {
		size_t l = strcspn(p, ",");

		if (l && l < sizeof(path)) {
			memcpy(path, p, l);
			path[l] = '\0';
			k618_dump_file(path);
		}
		p += l;
		if (*p == ',')
			p++;
	}
	log_now(200);
	if (++dump_i < dump_n)
		queue_delayed_work(system_unbound_wq, &k618_dump_dw, delay_until(dump_times[dump_i]));
}

static void k618_dump_start(void)
{
	const char *p = dump_at_ms;

	while (*p && dump_n < ARRAY_SIZE(dump_times)) {
		size_t l = strcspn(p, ",");
		char num[16];

		if (l && l < sizeof(num)) {
			memcpy(num, p, l);
			num[l] = '\0';
			if (!kstrtouint(num, 10, &dump_times[dump_n]))
				dump_n++;
		}
		p += l;
		if (*p == ',')
			p++;
	}
	if ((!*dump_files && !*dump_drm) || !dump_n)
		return;
	pr_emerg("k618_dump: '%s' + '%s' at %u time(s) from %u ms\n", dump_drm, dump_files, dump_n,
		 dump_times[0]);
	queue_delayed_work(system_unbound_wq, &k618_dump_dw, delay_until(dump_times[0]));
}

/* Device compatibility. */
static char ret_watch[512];
module_param_string(ret_watch, ret_watch, sizeof(ret_watch), 0444);
static unsigned int ret_watch_at_ms = 15000;
module_param(ret_watch_at_ms, uint, 0444);
static unsigned int ret_watch_max = 600;
module_param(ret_watch_max, uint, 0444);
static unsigned long drm_dbg_mask;
module_param(drm_dbg_mask, ulong, 0444);
static unsigned int drm_dbg_frames = 3;
module_param(drm_dbg_frames, uint, 0444);
static unsigned int drm_dbg_windows = 2;
module_param(drm_dbg_windows, uint, 0444);
static unsigned int drm_dbg_gap = 600;
module_param(drm_dbg_gap, uint, 0444);

#define K618_RW_MAX 24
struct k618_rw {
	struct kretprobe krp;
	char name[48];
	u8 kind;		/* 0 plain, 1 drm_atomic_set_property, 2 drm_mode_atomic_ioctl */
};
static struct k618_rw k618_rws[K618_RW_MAX];
static atomic_t rw_logged = ATOMIC_INIT(0), rw_refused = ATOMIC_INIT(0);
static unsigned long drm_dbg_saved;

/* drm_atomic_set_property(state, file_priv, obj, prop, prop_value, async_flip) */
struct k618_sp {
	struct drm_mode_object *obj;
	struct drm_property *prop;
	u64 val;
};

static int k618_rw_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct k618_sp *sp = (struct k618_sp *)ri->data;

	sp->obj = (struct drm_mode_object *)regs->regs[2];
	sp->prop = (struct drm_property *)regs->regs[3];
	sp->val = regs->regs[4];
	return 0;
}

static void k618_rw_drm_dbg(void)
{
	unsigned int gap = max(drm_dbg_gap, 1U);
	unsigned int n = atomic_inc_return(&rw_refused) - 1;

	if (!drm_dbg_mask || n / gap >= drm_dbg_windows)
		return;
	if (n % gap == 0) {
		drm_dbg_saved = __drm_debug;
		__drm_debug |= drm_dbg_mask;
		pr_emerg("k618_ret: refused commit %u: DRM debug %#lx on\n", n + 1, drm_dbg_mask);
	} else if (n % gap == drm_dbg_frames) {
		__drm_debug = drm_dbg_saved;
		pr_emerg("k618_ret: refused commit %u: DRM debug off\n", n + 1);
	}
}

static int k618_rw_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct k618_rw *rw = container_of(get_kretprobe(ri), struct k618_rw, krp);
	unsigned long v = regs_return_value(regs);
	struct k618_sp *sp = (struct k618_sp *)ri->data;
	long ret;

	/* Device compatibility. */
	if (IS_ERR_VALUE(v))
		ret = (long)v;
	else if (!(v >> 32) && IS_ERR_VALUE((unsigned long)(long)(int)v))
		ret = (int)v;
	else
		return 0;
	if (rw->kind == 2)
		k618_rw_drm_dbg();
	/*
	 * the rest only once a commit has been refused: a normal commit has errors of its own that
	 * mean nothing (a property that is not this handler's), and the refusal repeats every frame
	 */
	else if (drm_dbg_mask && !atomic_read(&rw_refused))
		return 0;
	if (atomic_inc_return(&rw_logged) > ret_watch_max)
		return 0;
	if (rw->kind == 1)
		pr_emerg("k618_ret: %s -> %ld: '%s' = %llu on %s %u (%s[%d])\n", rw->name, ret,
			 sp->prop ? sp->prop->name : "?", sp->val,
			 !sp->obj ? "?" : sp->obj->type == DRM_MODE_OBJECT_CONNECTOR ? "connector" :
			 sp->obj->type == DRM_MODE_OBJECT_CRTC ? "crtc" :
			 sp->obj->type == DRM_MODE_OBJECT_PLANE ? "plane" : "object",
			 sp->obj ? sp->obj->id : 0, current->comm, task_pid_nr(current));
	else
		pr_emerg("k618_ret: %s -> %ld (%s[%d])\n", rw->name, ret, current->comm,
			 task_pid_nr(current));
	return 0;
}

static void k618_rw_work(struct work_struct *w)
{
	const char *p = ret_watch;
	unsigned int n = 0, ok = 0;

	while (*p && n < K618_RW_MAX) {
		size_t l = strcspn(p, ",");
		struct k618_rw *rw = &k618_rws[n];
		int e;

		if (l && l < sizeof(rw->name)) {
			memcpy(rw->name, p, l);
			rw->name[l] = '\0';
			rw->kind = !strcmp(rw->name, "drm_atomic_set_property") ? 1 :
				   !strcmp(rw->name, "drm_mode_atomic_ioctl") ? 2 : 0;
			rw->krp.kp.symbol_name = rw->name;
			rw->krp.handler = k618_rw_ret;
			if (rw->kind == 1) {
				rw->krp.entry_handler = k618_rw_entry;
				rw->krp.data_size = sizeof(struct k618_sp);
			}
			rw->krp.maxactive = 32;
			e = register_kretprobe(&rw->krp);
			if (e)
				pr_emerg("k618_ret: no kretprobe on %s (%d)\n", rw->name, e);
			else
				ok++;
			n++;
		}
		p += l;
		if (*p == ',')
			p++;
	}
	pr_emerg("k618_ret: %u of %u functions watched, %u lines; DRM debug %#lx for %u refusals, %u times\n",
		 ok, n, ret_watch_max, drm_dbg_mask, drm_dbg_frames, drm_dbg_windows);
}
static DECLARE_DELAYED_WORK(k618_rw_dw, k618_rw_work);

/* Device compatibility. */
static unsigned int walt_compat;
module_param(walt_compat, uint, 0444);
static int walt_up[2] = { 95, 95 }, walt_down[2] = { 85, 85 };
static const struct ctl_table walt_compat_table[] = {
	{
		.procname	= "sched_upmigrate",
		.data		= walt_up,
		.maxlen		= sizeof(walt_up),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "sched_downmigrate",
		.data		= walt_down,
		.maxlen		= sizeof(walt_down),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
};
static struct ctl_table_header *walt_compat_hdr;

/*
 * in_recovery (read-only, the decision): a recovery boot gets no write guard. The 13 Pro has a
 * recovery partition whose image carries no kernel (kernel_size 0), so the bootloader starts the
 * boot partition's kernel -- ours -- with the recovery ramdisk, the ROM's or TWRP's. That ramdisk
 * is unpacked with our modules over it, so this probe loads there as well; and a recovery is there
 * to write partitions -- flash, wipe, format -- which the guard (GUARD=1 images only; armed by the
 * first policy load, which a recovery does too) would refuse. The decision is logged either way,
 * and readable in /sys/module/k618_probe/parameters/in_recovery.
 * The test is first-stage init's own: the ramdisk
 * has /system/bin/recovery and nothing says androidboot.force_normal_boot=1 (/proc is mounted
 * before the first module loads; the value is in the bootconfig on this phone's generation, on the
 * command line on older ones).
 */
static bool in_recovery;
module_param(in_recovery, bool, 0444);

static bool force_normal(const char *s)
{
	static const char key[] = "androidboot.force_normal_boot";

	while ((s = strstr(s, key))) {
		s += sizeof(key) - 1;
		while (*s == ' ' || *s == '=' || *s == '"')
			s++;
		if (*s == '1')
			return true;
	}
	return false;
}

static bool __init recovery_boot(void)
{
	static const char * const src[] = { "/proc/cmdline", "/proc/bootconfig" };
	bool forced = false;
	struct path p;
	char *b;
	int i;

	if (kern_path("/system/bin/recovery", 0, &p))
		return false;
	path_put(&p);
	b = kmalloc(8192, GFP_KERNEL);
	if (!b)
		return true;
	for (i = 0; i < ARRAY_SIZE(src) && !forced; i++) {
		struct file *f = filp_open(src[i], O_RDONLY, 0);
		loff_t pos = 0;
		ssize_t r;

		if (IS_ERR(f))
			continue;
		r = kernel_read(f, b, 8192 - 1, &pos);
		filp_close(f, NULL);
		if (r <= 0)
			continue;
		b[r] = '\0';
		forced = force_normal(b);
	}
	kfree(b);
	return !forced;
}

static int __init k618_probe_init(void)
{
	pr_info("k618_probe: look at %u ms, force_code %u, log '%s%s' from %u ms every %u ms (poll %u), %u KB slots at +%u KB\n",
		probe_ms, force_code, log_part, log_path, log_start_ms, log_every_ms, log_poll_ms,
		log_slot_kb, log_offset_kb);
	in_recovery = recovery_boot();
	if (in_recovery) {
		pr_emerg("k618_probe: recovery boot (/system/bin/recovery, no force_normal_boot)%s\n",
			 stop_at_write_open || deny_write_open || ro_write_open ?
			 ": no write guard, a recovery writes partitions" : "");
		stop_at_write_open = deny_write_open = ro_write_open = false;
	}
	if (*allow_vols) {
		pr_emerg("k618_probe: allow_vols '%s': persist %d, userdata %d, persist1 %d, none %d\n",
			 allow_vols, vol_allowed("persist"), vol_allowed("userdata"),
			 vol_allowed("persist1"), vol_allowed(""));
		pr_emerg("k618_probe: no repair or format tool may write '%s' (%s)\n",
			 repair_vols, *repair_comm ? repair_comm : "off");
		if (*nv_vols)
			pr_emerg("k618_probe: '%s' may be written by '%s' only\n", nv_vols, nv_comm);
		/* the decision itself, on names it will meet: a tool yes, a mounting program no */
		pr_emerg("k618_probe: guard self-test: fsck.f2fs %d, make_f2fs %d, e2fsck %d, vold %d, init %d; userdata %d, metadata %d, persist %d\n",
			 name_in(repair_comm, "fsck.f2fs", 9), name_in(repair_comm, "make_f2fs", 9),
			 name_in(repair_comm, "e2fsck", 6), name_in(repair_comm, "vold", 4),
			 name_in(repair_comm, "init", 4), vol_in(repair_vols, "userdata"),
			 vol_in(repair_vols, "metadata"), vol_in(repair_vols, "persist"));
	}
	if (stop_at_write_open || deny_write_open || ro_write_open) {
		int ret = register_kprobe(&k618_bdev_open_kp);

		kprobe_on = !ret;
		if (ret)
			pr_emerg("k618_probe: no kprobe on bdev_open (%d): nothing will stop the second stage\n",
				 ret);
		else
			register_blocking_lsm_notifier(&k618_probe_lsm_nb);
	}
	if (*watch_comm) {
		int o = register_kretprobe(&k618_filp_open_krp);
		int i = register_kretprobe(&k618_ioctl_krp);

		if (watch_log) {
			int w = register_kprobe(&k618_writev_kp);

			writev_on = !w;
			pr_emerg("k618_probe: their log messages too (tags '%s', at most %u lines)%s\n",
				 watch_log_tag, watch_log_max, w ? ": NO kprobe on writev" : "");
		}
		watch_on = !o && !i;
		pr_emerg("k618_probe: watching '%s*' (opens %s, ioctls %s), at most %u lines\n",
			 watch_comm, o ? "no" : "yes", i ? "no" : "yes", watch_max);
		if (o)
			pr_emerg("k618_probe: no kretprobe on do_filp_open (%d)\n", o);
		if (i)
			pr_emerg("k618_probe: no kretprobe on the ioctl syscall (%d)\n", i);
	}
	if ((deny_write_open || ro_write_open) && deny_wait_ms)
		queue_delayed_work(system_unbound_wq, &k618_deny_dw, delay_until(deny_wait_ms));
	if (probe_ms)
		queue_delayed_work(system_unbound_wq, &k618_probe_dw, delay_until(probe_ms));
	if (*stack_comm && stack_at_ms)
		queue_delayed_work(system_unbound_wq, &k618_stacks_dw, delay_until(stack_at_ms));
	k618_dump_start();
	if (*ret_watch)
		queue_delayed_work(system_unbound_wq, &k618_rw_dw, delay_until(ret_watch_at_ms));
	if (walt_compat) {
		walt_compat_hdr = register_sysctl("walt", walt_compat_table);
		pr_emerg("k618_probe: walt_compat: /proc/sys/walt/sched_upmigrate, sched_downmigrate %s\n",
			 walt_compat_hdr ? "there" : "NOT there");
	}
	/* every suspend attempt, logged (and refused with suspend_veto=1) */
	pm_nb_on = !register_pm_notifier(&k618_pm_nb);
	pr_emerg("k618_probe: suspend attempts %s%s\n", pm_nb_on ? "watched" : "NOT watched",
		 suspend_veto ? " and refused" : "");
	if (*log_part || *log_path) {
		/* the buffer for the log comes now, while asking for memory is still safe */
		log_text = vmalloc((size_t)log_buf_kb * 1024);
		if (!log_text)
			return -ENOMEM;
		queue_delayed_work(system_unbound_wq, &k618_log_dw, delay_until(log_start_ms));
		if (log_ring_kb && log_ring_ms) {
			/* a page, not vmalloc: a direct write takes the page behind the address */
			log_ring_buf = (char *)__get_free_page(GFP_KERNEL);
			log_ring_task = log_ring_buf ?
				kthread_run(log_ring_fn, NULL, "k618_logring") : NULL;
			if (IS_ERR(log_ring_task))
				log_ring_task = NULL;
			if (log_ring_task) {
				panic_nb_on = !atomic_notifier_chain_register(&panic_notifier_list,
									     &k618_panic_nb);
				die_nb_on = !register_die_notifier(&k618_die_nb);
				/* the ring read back as text, for a shell without root */
				log_view_on = !!proc_create("k618_log", 0444, init_net.proc_net,
							    &log_view_ops);
				if (log_view_on && log_view_test)
					queue_delayed_work(system_unbound_wq, &k618_vtest_dw,
							   delay_until(log_view_test));
			}
			pr_emerg("k618_probe: log ring %s (%u KB), panic dump %s, oops dump %s, /proc/net/k618_log %s\n",
				 log_ring_task ? "asked for" : "NOT on", log_ring_kb,
				 panic_nb_on ? "on" : "NOT on", die_nb_on ? "on" : "NOT on",
				 log_view_on ? "on" : "NOT on");
		}
		if (log_panic_test)
			queue_delayed_work(system_unbound_wq, &k618_ptest_dw,
					   delay_until(log_panic_test));
		if (log_oops_irq_test) {
			timer_setup(&k618_otest_timer, k618_otest_fn, 0);
			mod_timer(&k618_otest_timer, jiffies + delay_until(log_oops_irq_test));
		}
		if (log_oops_task_test) {
			k618_otask_delay = delay_until(log_oops_task_test);
			kthread_run(k618_otask_fn, NULL, "k618_otask");
		}
		if (log_stall_ms && log_every_ms) {
			log_text2 = vmalloc((size_t)log_buf_kb * 1024);
			log_wd = log_text2 ? kthread_run(log_wd_fn, NULL, "k618_logwd") : NULL;
			if (IS_ERR(log_wd))
				log_wd = NULL;
			pr_emerg("k618_probe: stall watch %s (%u ms)\n", log_wd ? "on" : "NOT on",
				 log_stall_ms);
		}
	}
	return 0;
}
module_init(k618_probe_init);

static void __exit k618_probe_exit(void)
{
	if (writev_on)
		unregister_kprobe(&k618_writev_kp);
	if (watch_on) {
		unregister_kretprobe(&k618_filp_open_krp);
		unregister_kretprobe(&k618_ioctl_krp);
	}
	if (kprobe_on) {
		unregister_blocking_lsm_notifier(&k618_probe_lsm_nb);
		unregister_kprobe(&k618_bdev_open_kp);
	}
	if (pm_nb_on)
		unregister_pm_notifier(&k618_pm_nb);
	if (walt_compat_hdr)
		unregister_sysctl_table(walt_compat_hdr);
	if (panic_nb_on)
		atomic_notifier_chain_unregister(&panic_notifier_list, &k618_panic_nb);
	if (die_nb_on)
		unregister_die_notifier(&k618_die_nb);
	cancel_delayed_work_sync(&k618_vtest_dw);
	if (log_view_on)
		remove_proc_entry("k618_log", init_net.proc_net);
	if (log_ring_task)
		kthread_stop(log_ring_task);
	if (log_view_file)
		fput(log_view_file);
	if (log_ring_file)
		fput(log_ring_file);
	if (log_ring_buf)
		free_page((unsigned long)log_ring_buf);
	cancel_delayed_work_sync(&k618_ptest_dw);
	if (log_oops_irq_test)
		timer_delete_sync(&k618_otest_timer);
	cancel_delayed_work_sync(&k618_probe_dw);
	cancel_delayed_work_sync(&k618_stacks_dw);
	cancel_delayed_work_sync(&k618_deny_dw);
	if (log_wd)
		kthread_stop(log_wd);
	cancel_delayed_work_sync(&k618_log_dw);
	if (log_file)
		fput(log_file);
	vfree(log_text);
	vfree(log_text2);
}
module_exit(k618_probe_exit);

MODULE_DESCRIPTION("6.12 bring-up: what the first stage is doing, as a panic code");
/* 6.18 exports the VFS's own calls into a namespace a module has to name */
MODULE_IMPORT_NS("ANDROID_GKI_VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
MODULE_LICENSE("GPL");
