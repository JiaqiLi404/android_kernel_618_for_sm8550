// SPDX-License-Identifier: GPL-2.0
/*
 * k618_zramtest: QEMU only, never in a phone image. Checks k618_ports.py's zram_tiered on a boot
 * with no user space to set zram up: zram0's algorithms (lz4, and zstd as the second), that
 * disksize_mb decides the size (16384 by default; 64 here, so that a round scans 16384 slots, not
 * 4 million, in the seconds an emulated boot lives), then 4 MB written through the block device,
 * two rounds of the
 * cold tier asked for with recomp_now (the first marks, the second recompresses), the compressed
 * size before and after, and every page read back after the page cache is dropped. One line per
 * step, "k618_zramtest: PASS" or "k618_zramtest: FAIL ..." at the end.
 */
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#define NR_PAGES	1024	/* 4 MB: an emulated boot lives some 5 s past this point */
#define SYS		"/sys/block/zram0/"
#define PAR		"/sys/module/zram/parameters/"

static int fails;

static void bad(const char *what)
{
	fails++;
	pr_emerg("k618_zramtest: FAIL %s\n", what);
}

static ssize_t rd(const char *path, char *buf, size_t len)
{
	struct file *f = filp_open(path, O_RDONLY, 0);
	loff_t pos = 0;
	ssize_t n;

	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_read(f, buf, len - 1, &pos);
	filp_close(f, NULL);
	buf[n > 0 ? n : 0] = 0;
	return n;
}

static ssize_t wr(const char *path, const char *val)
{
	struct file *f = filp_open(path, O_WRONLY, 0);
	loff_t pos = 0;
	ssize_t n;

	if (IS_ERR(f))
		return PTR_ERR(f);
	n = kernel_write(f, val, strlen(val), &pos);
	filp_close(f, NULL);
	return n;
}

/* the idx-th number in a file of numbers separated by blanks (mm_stat, a parameter) */
static unsigned long long field(const char *path, int idx)
{
	char buf[256], *p = buf, *t;
	unsigned long long v = 0;
	int i = 0;

	if (rd(path, buf, sizeof(buf)) <= 0)
		return 0;
	while ((t = strsep(&p, " \t\n"))) {
		if (!*t)
			continue;
		if (i++ == idx) {
			if (kstrtoull(t, 10, &v))
				v = 0;
			break;
		}
	}
	return v;
}

/* text-like content that lz4 and zstd both compress, zstd further: records of words */
static void fill(u32 *page, u32 n)
{
	static const char *const words[] = {
		"android.view", "RenderThread", "binder:", "surfaceflinger", "com.miui.home",
		"layout", "texture", "0x7f", "null", "true", "false", "systemui", "measure", "draw",
	};
	char *p = (char *)page;
	u32 seed = n * 2654435761u + 12345, off = 0;

	while (off + 32 < PAGE_SIZE) {
		const char *w;

		seed = seed * 1103515245 + 12345;
		w = words[(seed >> 16) % ARRAY_SIZE(words)];
		off += scnprintf(p + off, PAGE_SIZE - off, "%u:%s=%u;", n, w, (seed >> 8) & 0xfff);
	}
	memset(p + off, 0, PAGE_SIZE - off);
}

static int rounds_since(unsigned long long before, int timeout_s)
{
	int i;

	for (i = 0; i < timeout_s * 10; i++) {
		if (field(PAR "recomp_rounds", 0) > before)
			return 0;
		msleep(100);
	}
	return -ETIMEDOUT;
}

static void k618_zramtest_work(struct work_struct *w)
{
	unsigned long long size, orig, before, after, r0;
	char buf[256];
	struct file *f = ERR_PTR(-ENODEV);
	u32 *page, *got;
	loff_t pos;
	int i, bad_pages = 0;

	for (i = 0; i < 300 && rd(SYS "disksize", buf, sizeof(buf)) < 0; i++)
		msleep(100);
	if (i == 300) {
		bad("no zram0");
		return;
	}
	rd(SYS "comp_algorithm", buf, sizeof(buf));
	pr_emerg("k618_zramtest: comp_algorithm %s", buf);
	if (!strstr(buf, "[lz4]"))
		bad("lz4 is not the default");
	rd(SYS "recomp_algorithm", buf, sizeof(buf));
	pr_emerg("k618_zramtest: recomp_algorithm %s", buf);
	if (!strstr(buf, "[zstd]"))
		bad("zstd is not the second algorithm");

	size = field(PAR "disksize_mb", 0);
	pr_emerg("k618_zramtest: disksize_mb %llu by default\n", size);
	if (size != 16384)
		bad("disksize_mb is not 16384 by default");
	if (wr(PAR "disksize_mb", "64") < 0 || wr(SYS "disksize", "1G") < 0) {
		bad("disksize write");
		return;
	}
	size = field(SYS "disksize", 0);
	pr_emerg("k618_zramtest: disksize_mb 64, wrote 1G to disksize, got %llu\n", size);
	if (size != 64ULL << 20)
		bad("disksize_mb did not decide the size");

	page = vmalloc(PAGE_SIZE);
	got = vmalloc(PAGE_SIZE);
	/* no ueventd here, so no node in /dev: open the block device by its number */
	f = ERR_PTR(-ENODEV);
	if (rd(SYS "dev", buf, sizeof(buf)) > 0) {
		unsigned int maj, min;

		if (sscanf(buf, "%u:%u", &maj, &min) == 2)
			f = bdev_file_open_by_dev(MKDEV(maj, min), BLK_OPEN_READ | BLK_OPEN_WRITE,
						  &fails, NULL);
	}
	if (!page || !got || IS_ERR(f)) {
		bad("cannot open zram0");
		goto out;
	}
	for (i = 0, pos = 0; i < NR_PAGES; i++) {
		fill(page, i);
		if (kernel_write(f, page, PAGE_SIZE, &pos) != PAGE_SIZE) {
			bad("write");
			goto out_f;
		}
	}
	vfs_fsync(f, 0);
	invalidate_mapping_pages(f->f_mapping, 0, -1);
	orig = field(SYS "mm_stat", 0);
	before = field(SYS "mm_stat", 1);
	pr_emerg("k618_zramtest: %d pages written: %llu bytes stored as %llu (lz4)\n",
		 NR_PAGES, orig, before);

	/* the first round marks what it finds, the second recompresses what nothing touched since */
	for (i = 0; i < 2; i++) {
		r0 = field(PAR "recomp_rounds", 0);
		if (wr(PAR "recomp_now", "1") < 0 || rounds_since(r0, 60)) {
			bad("round did not run");
			goto out_f;
		}
	}
	after = field(SYS "mm_stat", 1);
	pr_emerg("k618_zramtest: after two rounds: %llu bytes (%llu%% of lz4), recomp_ms %llu\n",
		 after, before ? after * 100 / before : 0, field(PAR "recomp_ms", 0));
	if (!before || after >= before * 95 / 100)
		bad("recompression did not make the data smaller");

	invalidate_mapping_pages(f->f_mapping, 0, -1);
	for (i = 0, pos = 0; i < NR_PAGES; i++) {
		fill(page, i);
		if (kernel_read(f, got, PAGE_SIZE, &pos) != PAGE_SIZE || memcmp(page, got, PAGE_SIZE))
			bad_pages++;
	}
	pr_emerg("k618_zramtest: %d of %d pages read back intact\n", NR_PAGES - bad_pages, NR_PAGES);
	if (bad_pages)
		bad("pages read back differ");
out_f:
	fput(f);
out:
	vfree(page);
	vfree(got);
	if (!fails)
		pr_emerg("k618_zramtest: PASS\n");
}
static DECLARE_WORK(k618_zramtest_w, k618_zramtest_work);

static int __init k618_zramtest_init(void)
{
	queue_work(system_unbound_wq, &k618_zramtest_w);
	return 0;
}
module_init(k618_zramtest_init);

MODULE_DESCRIPTION("k618: QEMU check of zram's two tiers");
MODULE_LICENSE("GPL");
/* filp_open() and kernel_read() / kernel_write() are GKI's file-system-only exports */
MODULE_IMPORT_NS("ANDROID_GKI_VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
