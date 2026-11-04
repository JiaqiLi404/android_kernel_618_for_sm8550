// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/security.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

static int mode;
module_param(mode, int, 0444);
/* mode 12: how long to keep init alive after the refused open */
static unsigned int stay_ms = 8000;
module_param(stay_ms, uint, 0444);
static char *say = "";
module_param(say, charp, 0444);

static int *volatile nullp;

static void tfn(struct timer_list *t)
{
	pr_emerg("k618_panictest: timer\n");
	*nullp = 1;
}
static DEFINE_TIMER(ttimer, tfn);

static void wfn(struct work_struct *w)
{
	pr_emerg("k618_panictest: work\n");
	*nullp = 2;
}
static DECLARE_WORK(twork, wfn);

/* modes 10, 11: reads give zeros, writes are dropped */
static void k618t_submit_bio(struct bio *bio)
{
	if (bio_op(bio) == REQ_OP_READ)
		zero_fill_bio(bio);
	bio_endio(bio);
}

static const struct block_device_operations k618t_fops = {
	.owner = THIS_MODULE,
	.submit_bio = k618t_submit_bio,
};

static int k618t_write_open(void)
{
	struct queue_limits lim = {};
	struct gendisk *disk;
	struct file *f;
	int ret;

	disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(disk))
		return PTR_ERR(disk);
	disk->fops = &k618t_fops;
	strscpy(disk->disk_name, "sdz", DISK_NAME_LEN);
	set_capacity(disk, 2048);
	ret = add_disk(disk);
	if (ret) {
		put_disk(disk);
		return ret;
	}
	pr_emerg("k618_panictest: opening %s for writing\n", disk->disk_name);
	f = bdev_file_open_by_dev(disk_devt(disk), BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
	pr_emerg("k618_panictest: the write open of %s returned %ld\n", disk->disk_name,
		 IS_ERR(f) ? PTR_ERR(f) : 0L);
	if (!IS_ERR(f))
		fput(f);
	return 0;
}

static int __init k618_panictest_init(void)
{
	void __iomem *p;

	pr_emerg("k618_panictest: mode %d\n", mode);
	switch (mode) {
	case 1:
		*nullp = 0;
		break;
	case 2:
		mod_timer(&ttimer, jiffies + HZ / 10);
		msleep(2000);
		break;
	case 3:
		schedule_work(&twork);
		msleep(2000);
		break;
	case 4:
		panic("k618 panictest");
		break;
	case 5:
		p = ioremap(0x0b000000, 0x1000);
		if (p)
			pr_emerg("k618_panictest: read %x\n", readl(p));
		break;
	case 6:
		emergency_restart();
		break;
	case 7:
		return -EINVAL;
	case 8:
		pr_info("%s\n", say);
		break;
	case 9:
		call_blocking_lsm_notifier(LSM_POLICY_CHANGE, NULL);
		break;
	case 10:
		call_blocking_lsm_notifier(LSM_POLICY_CHANGE, NULL);
		return k618t_write_open();
	case 11:
		return k618t_write_open();
	case 12:
		call_blocking_lsm_notifier(LSM_POLICY_CHANGE, NULL);
		k618t_write_open();
		msleep(stay_ms);
		break;
	}
	return 0;
}
module_init(k618_panictest_init);

MODULE_DESCRIPTION("QEMU only: panics on purpose to check k618_panicsig");
MODULE_LICENSE("GPL");
