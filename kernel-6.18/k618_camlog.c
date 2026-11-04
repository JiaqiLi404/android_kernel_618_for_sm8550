// SPDX-License-Identifier: GPL-2.0
/*
 * k618_camlog: /dev/camlog on 6.18 -- a rewrite of the 13 Pro's own 5.15 cameralog.ko ("camera log
 * device driver", closed source; written from its machine code), built under the stock name so the
 * vendor's modprobe of the 5.15 file finds it loaded.
 *
 * A mailbox for one message: write() puts a string in and wakes the readers, poll() says readable
 * while it is not empty, read() hands it out and empties it. The 13 Pro's camera HAL opens it (odm
 * libcamlog.so, com.xiaomi.camdfx.so), and so does the port's libcameraopt; the vendor's
 * init.qcom.rc gives the node 0666 and its vendor_file_contexts labels it. The 18 Pro Max has no
 * such device (its system side copes without one).
 *
 * As the original: a character device "camlog" (dynamic major, 256 minors) with a class "camlog"
 * and one device "camlog"; a 1024-byte buffer under one mutex, one wait queue; ioctl, open and
 * release log at debug level and succeed; camlog_send_message() is exported and only logs (it takes
 * no message in the original either, and no module calls it).
 *
 * Different, on purpose -- the original's buffer handling, which trusted user space:
 *  - write() copies at most 1023 bytes and ends the string there; the original copied `count`
 *    bytes into its 1024-byte buffer unchecked, and did not end it, so a shorter message written
 *    over an unread longer one kept the longer one's tail;
 *  - read() copies at most `count` bytes; the original copied the whole message whatever the
 *    reader's buffer size;
 *  - as the original, a read of an empty mailbox puts "\n" in the reader's buffer and returns 0,
 *    and a write returns the length of what is now in the mailbox (0 if the copy failed).
 */
#define pr_fmt(fmt) "cameralog: " fmt

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define CAMLOG_NAME	"camlog"
#define CAMLOG_MSG_SIZE	1024

static struct {
	char message[CAMLOG_MSG_SIZE];
	struct mutex lock;
	wait_queue_head_t not_empty;
} camlog;

static int camlog_major;
static struct class *camlog_class;

/* the original's: logs, takes no message (exported, no header declares it) */
void camlog_send_message(void);
void camlog_send_message(void)
{
	pr_debug(" camlog_send_message ss\n");
}
EXPORT_SYMBOL(camlog_send_message);

static ssize_t cameralog_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	size_t len;
	ssize_t ret;

	mutex_lock(&camlog.lock);
	len = strnlen(camlog.message, CAMLOG_MSG_SIZE);
	if (!len) {
		/* as the original: an empty mailbox reads as "\n" with a return value of 0 */
		ret = (count && copy_to_user(buf, "\n", 1)) ? -EFAULT : 0;
		mutex_unlock(&camlog.lock);
		return ret;
	}
	if (len > count)
		len = count;
	ret = copy_to_user(buf, camlog.message, len) ? 0 : len;
	memset(camlog.message, 0, sizeof(camlog.message));
	mutex_unlock(&camlog.lock);
	return ret;
}

static ssize_t cameralog_write(struct file *file, const char __user *buf, size_t count,
			       loff_t *ppos)
{
	size_t len = min_t(size_t, count, CAMLOG_MSG_SIZE - 1);
	ssize_t ret;

	mutex_lock(&camlog.lock);
	if (copy_from_user(camlog.message, buf, len)) {
		memset(camlog.message, 0, sizeof(camlog.message));
		ret = 0;
	} else {
		camlog.message[len] = '\0';
		ret = strlen(camlog.message);
	}
	mutex_unlock(&camlog.lock);
	wake_up_interruptible(&camlog.not_empty);
	return ret;
}

static __poll_t cameralog_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &camlog.not_empty, wait);
	return READ_ONCE(camlog.message[0]) ? EPOLLIN | EPOLLRDNORM : 0;
}

static long cameralog_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	pr_debug(" cameralog_ioctl!\n");
	return 0;
}

static int cameralog_open(struct inode *inode, struct file *file)
{
	pr_debug(" cameralog_open!\n");
	return 0;
}

static int cameralog_release(struct inode *inode, struct file *file)
{
	pr_debug(" cameralog_release!\n");
	return 0;
}

static const struct file_operations cameralog_fops = {
	.owner		= THIS_MODULE,
	.read		= cameralog_read,
	.write		= cameralog_write,
	.poll		= cameralog_poll,
	.unlocked_ioctl	= cameralog_ioctl,
	.compat_ioctl	= cameralog_ioctl,
	.open		= cameralog_open,
	.release	= cameralog_release,
	.llseek		= noop_llseek,
};

static int __init cameralog_init(void)
{
	struct device *dev;

	mutex_init(&camlog.lock);
	init_waitqueue_head(&camlog.not_empty);
	camlog_major = __register_chrdev(0, 0, 256, CAMLOG_NAME, &cameralog_fops);
	if (camlog_major < 0) {
		pr_err("error while trying to create %s\n", CAMLOG_NAME);
		return camlog_major;
	}
	camlog_class = class_create(CAMLOG_NAME);
	if (IS_ERR(camlog_class)) {
		__unregister_chrdev(camlog_major, 0, 256, CAMLOG_NAME);
		pr_warn("Failed to create class.\n");
		return PTR_ERR(camlog_class);
	}
	dev = device_create(camlog_class, NULL, MKDEV(camlog_major, 0), NULL, CAMLOG_NAME);
	if (IS_ERR(dev)) {
		class_destroy(camlog_class);
		__unregister_chrdev(camlog_major, 0, 256, CAMLOG_NAME);
		pr_err("error while trying to create %s\n", CAMLOG_NAME);
		return PTR_ERR(dev);
	}
	return 0;
}

static void __exit cameralog_exit(void)
{
	device_destroy(camlog_class, MKDEV(camlog_major, 0));
	class_destroy(camlog_class);
	__unregister_chrdev(camlog_major, 0, 256, CAMLOG_NAME);
}

module_init(cameralog_init);
module_exit(cameralog_exit);
MODULE_DESCRIPTION("camera log device driver");
MODULE_LICENSE("GPL");
