// SPDX-License-Identifier: GPL-2.0
/*
 * extend_reclaim (built as extend_reclaim.ko from k618_extend_reclaim.c): Xiaomi's 5.15
 * drivers/staging/extend_reclaim/extend_reclaim.c (fuxi tree, chenzhiwei@xiaomi.com) on 6.18.
 *
 *   /sys/kernel/extend_reclaim/reclaim   write "<type> <pages>": type 0 file, 1 anon, 2 both;
 *                                        reclaims that many pages system-wide.
 *                                        Read: "reclaim_show".
 *   /sys/kernel/extend_reclaim/debug     non-zero: one log line per request
 *
 * What changed for 6.18:
 *  - try_to_free_mem_cgroup_pages() takes reclaim options and an optional swappiness.
 *  - "anon": 5.15 forced SCAN_ANON through android_vh_tune_scan_type when the writing process was
 *    called "extend_reclaim". That hook sits in get_scan_count(), which 6.18 does not reach with
 *    MGLRU on (CONFIG_LRU_GEN_ENABLED=y here). 6.18 has the same thing built in for any caller:
 *    swappiness SWAPPINESS_ANON_ONLY (memory.reclaim's "swappiness=max"), honoured by both LRUs.
 *    So no vendor hook, and "anon" means anon for every writer, not only one named extend_reclaim.
 *  - the out-of-range type is checked before it is used as an index (5.15 printed
 *    reclaim_type_str[reclaim_type] for a bad type first).
 *
 * Nothing on this ROM writes it (odm build.prop: persist.sys.lmkd.extend_reclaim.enable=false;
 * OS4's lmkd talks to mi_reclaim instead); kept for parity with the phone's 5.15 module set.
 */
#define pr_fmt(fmt) "extend_reclaim: " fmt

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/memcontrol.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/sysfs.h>

enum extend_reclaim_type {
	EXTEND_RECLAIM_TYPE_FILE = 0,
	EXTEND_RECLAIM_TYPE_ANON,
	EXTEND_RECLAIM_TYPE_ALL,
	EXTEND_RECLAIM_TYPE_COUNT
};

static const char * const reclaim_type_str[EXTEND_RECLAIM_TYPE_COUNT] = { "file", "anon", "all" };
static int extend_reclaim_debug;
static struct kobject *kobj;

static ssize_t debug_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d", extend_reclaim_debug);
}

static ssize_t debug_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	int v;

	if (sscanf(buf, "%d", &v) != 1) {
		pr_err("invalid input: %s", buf);
		return -EINVAL;
	}
	WRITE_ONCE(extend_reclaim_debug, v);
	return count;
}

static ssize_t reclaim_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "reclaim_show");
}

static ssize_t reclaim_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	struct mem_cgroup *target = mem_cgroup_disabled() ? NULL : root_mem_cgroup;
	int anon_only = SWAPPINESS_ANON_ONLY;
	unsigned int options = MEMCG_RECLAIM_PROACTIVE;
	unsigned long nr_reclaimed;
	long nr_to_reclaim;
	int type;

	if (sscanf(buf, "%d %ld", &type, &nr_to_reclaim) != 2) {
		pr_err("invalid input: %s", buf);
		return -EINVAL;
	}
	if (type < 0 || type >= EXTEND_RECLAIM_TYPE_COUNT || nr_to_reclaim < 0) {
		pr_err("invalid input: reclaim type = %d, nr_to_reclaim = %ld", type, nr_to_reclaim);
		return -EINVAL;
	}
	if (type != EXTEND_RECLAIM_TYPE_FILE)
		options |= MEMCG_RECLAIM_MAY_SWAP;

	nr_reclaimed = try_to_free_mem_cgroup_pages(target, nr_to_reclaim, GFP_KERNEL, options,
						    type == EXTEND_RECLAIM_TYPE_ANON ?
						    &anon_only : NULL);

	if (READ_ONCE(extend_reclaim_debug))
		pr_info("current->comm = %s, reclaim type = %s, nr_to_reclaim = %ld, nr_reclaimed = %lu",
			current->comm, reclaim_type_str[type], nr_to_reclaim, nr_reclaimed);
	return count;
}

static struct kobj_attribute reclaim_attr = __ATTR(reclaim, 0644, reclaim_show, reclaim_store);
static struct kobj_attribute debug_attr = __ATTR(debug, 0644, debug_show, debug_store);

static struct attribute *extend_reclaim_attrs[] = {
	&reclaim_attr.attr,
	&debug_attr.attr,
	NULL,
};

static const struct attribute_group extend_reclaim_attr_group = {
	.attrs = extend_reclaim_attrs,
};

static int __init extend_reclaim_init(void)
{
	int error;

	kobj = kobject_create_and_add("extend_reclaim", kernel_kobj);
	if (!kobj) {
		error = -ENOMEM;
		goto exit;
	}
	error = sysfs_create_group(kobj, &extend_reclaim_attr_group);
	if (error)
		goto kobj_put;

	pr_info("module init!");
	return 0;

kobj_put:
	kobject_put(kobj);
exit:
	pr_err("module init failed error = %d!", error);
	return error;
}

static void __exit extend_reclaim_exit(void)
{
	if (kobj) {
		sysfs_remove_group(kobj, &extend_reclaim_attr_group);
		kobject_put(kobj);
	}
	pr_info("module exit!");
}

module_init(extend_reclaim_init);
module_exit(extend_reclaim_exit);

MODULE_AUTHOR("chenzhiwei<chenzhiwei@xiaomi.com>");
MODULE_DESCRIPTION("global page reclaim interface");
MODULE_LICENSE("GPL");
