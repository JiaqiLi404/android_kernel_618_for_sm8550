// SPDX-License-Identifier: GPL-2.0
/*
 * k618_mi_exception_log: /dev/mi_exception_log on 6.18 -- a rewrite of the 13 Pro's own 5.15
 * mi_exception_log.ko ("mi exception log device driver", closed source; written from its machine
 * code), built under the stock name.
 *
 * A ring of 100 messages of 1024 bytes: write() puts one in and wakes the readers, read() hands the
 * oldest out and drops it, poll() says readable while the ring is not empty. HyperOS's framework
 * (miui-framework.jar) opens the node and the vendor's init.qcom.rc gives it mode 0666; the 18 Pro
 * Max has no such device, so its system side coped without one -- this is for the 13 Pro's own
 * users of it.
 *
 * As the original: character device "mi_exception_log" (dynamic major, 256 minors) with a class and
 * one device of that name; one spinlock (taken with interrupts off, so a message can be written
 * from any context), one wait queue; a message of at most 1022 bytes; a read of an empty ring gives
 * "empty\n" and does not advance; write() returns the length taken, read() the length handed out.
 * The ring drops the oldest message when it is full, as the original's read index does.
 *
 * Different, on purpose: the original wrote into the ring at its *old* write index and then only
 * moved the read index when full, so a full ring kept overwriting one slot and the messages after
 * it were stale; and its read of an empty ring wrote "empty\n" into the ring itself. Here the write
 * index always moves on, and the empty read is answered without touching the ring.
 */
#define pr_fmt(fmt) "MI_EXCEPTION_LOG: " fmt

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/wait.h>

#define MEL_NAME	"mi_exception_log"
#define MEL_SLOTS	100
#define MEL_SLOT_SIZE	1024
#define MEL_MSG_MAX	(MEL_SLOT_SIZE - 2)	/* 1022, as stock */
#define MEL_EMPTY	"empty\n"

static struct {
	char log[MEL_SLOTS][MEL_SLOT_SIZE];
	int read_idx;
	int write_idx;
	wait_queue_head_t not_empty;
} mel;

static DEFINE_SPINLOCK(mel_lock);
static int mel_major;
static struct class *mel_class;

static ssize_t exception_log_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	char msg[MEL_SLOT_SIZE];
	unsigned long flags;
	size_t len;

	spin_lock_irqsave(&mel_lock, flags);
	if (mel.read_idx == mel.write_idx) {
		spin_unlock_irqrestore(&mel_lock, flags);
		len = min(count, strlen(MEL_EMPTY));
		if (len && copy_to_user(buf, MEL_EMPTY, len))
			return -EFAULT;
		return len;
	}
	len = strnlen(mel.log[mel.read_idx], MEL_SLOT_SIZE - 1);
	memcpy(msg, mel.log[mel.read_idx], len);
	mel.read_idx = (mel.read_idx + 1) % MEL_SLOTS;
	spin_unlock_irqrestore(&mel_lock, flags);

	/* a message is handed out with the newline the original appends */
	if (len < MEL_SLOT_SIZE - 1)
		msg[len++] = '\n';
	if (len > count)
		len = count;
	if (len && copy_to_user(buf, msg, len))
		return -EFAULT;
	return len;
}

static ssize_t exception_log_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	char msg[MEL_SLOT_SIZE] = {};
	size_t len = min(iov_iter_count(from), (size_t)MEL_MSG_MAX);
	unsigned long flags;

	if (copy_from_iter(msg, len, from) != len) {
		iov_iter_revert(from, len);
		return -EFAULT;
	}
	msg[len] = '\0';

	spin_lock_irqsave(&mel_lock, flags);
	memcpy(mel.log[mel.write_idx], msg, len + 1);
	mel.write_idx = (mel.write_idx + 1) % MEL_SLOTS;
	if (mel.write_idx == mel.read_idx)	/* full: the oldest goes */
		mel.read_idx = (mel.read_idx + 1) % MEL_SLOTS;
	spin_unlock_irqrestore(&mel_lock, flags);

	wake_up_interruptible(&mel.not_empty);
	return len;
}

static __poll_t exception_log_poll(struct file *file, poll_table *wait)
{
	unsigned long flags;
	bool ready;

	poll_wait(file, &mel.not_empty, wait);
	spin_lock_irqsave(&mel_lock, flags);
	ready = mel.read_idx != mel.write_idx;
	spin_unlock_irqrestore(&mel_lock, flags);
	return ready ? EPOLLIN | EPOLLRDNORM : 0;
}

static int exception_log_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations exception_log_fops = {
	.owner		= THIS_MODULE,
	.read		= exception_log_read,
	.write_iter	= exception_log_write_iter,
	.poll		= exception_log_poll,
	.release	= exception_log_release,
	.llseek		= noop_llseek,
};

static int __init mi_exception_log_init(void)
{
	struct device *dev;

	init_waitqueue_head(&mel.not_empty);
	mel_major = __register_chrdev(0, 0, 256, MEL_NAME, &exception_log_fops);
	if (mel_major < 0) {
		pr_err("error while trying to create %s\n", MEL_NAME);
		return mel_major;
	}
	mel_class = class_create(MEL_NAME);
	if (IS_ERR(mel_class)) {
		__unregister_chrdev(mel_major, 0, 256, MEL_NAME);
		pr_warn("Failed to create class.\n");
		return PTR_ERR(mel_class);
	}
	dev = device_create(mel_class, NULL, MKDEV(mel_major, 0), NULL, MEL_NAME);
	if (IS_ERR(dev)) {
		class_destroy(mel_class);
		__unregister_chrdev(mel_major, 0, 256, MEL_NAME);
		pr_err("error while trying to create %s\n", MEL_NAME);
		return PTR_ERR(dev);
	}
	pr_info("mi_exception_log init ok\n");
	return 0;
}

static void __exit mi_exception_log_exit(void)
{
	device_destroy(mel_class, MKDEV(mel_major, 0));
	class_destroy(mel_class);
	__unregister_chrdev(mel_major, 0, 256, MEL_NAME);
}

module_init(mi_exception_log_init);
module_exit(mi_exception_log_exit);
MODULE_DESCRIPTION("mi exception log device driver");
MODULE_LICENSE("GPL");
