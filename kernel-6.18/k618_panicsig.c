// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kmsg_dump.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/panic_notifier.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/security.h>
#include <linux/string.h>

static unsigned int kind_ms = 24000;
module_param(kind_ms, uint, 0444);
static unsigned int context_ms = 8000;
module_param(context_ms, uint, 0444);
static unsigned int restart_ms = 12000;
module_param(restart_ms, uint, 0444);

static unsigned int mode;
module_param(mode, uint, 0444);
static unsigned int pos_base_ms = 30000;
module_param(pos_base_ms, uint, 0444);
static unsigned int pos_step_ms = 2000;
module_param(pos_step_ms, uint, 0444);
static unsigned int other_ms = 230000;
module_param(other_ms, uint, 0444);
static unsigned int other_step_ms = 15000;
module_param(other_step_ms, uint, 0444);
static unsigned int restart_at_ms = 295000;
module_param(restart_at_ms, uint, 0444);
static unsigned int pos_from = 83;
module_param(pos_from, uint, 0444);
static unsigned int pos_low_ms = 40000;
module_param(pos_low_ms, uint, 0444);
static unsigned int total;
module_param(total, uint, 0444);
static unsigned int all_ms = 140000;
module_param(all_ms, uint, 0444);
static unsigned int stage_ms = 20000;
module_param(stage_ms, uint, 0444);

static char *node = "/soc/ufshc@1d84000";
module_param(node, charp, 0444);
static char *nodes =
	"/soc/dma_dev,/soc/apps-smmu@15000000/anoc_1_qtb@16f0000,/soc/apps-smmu@15000000,/soc/ufshc@1d84000";
module_param(nodes, charp, 0444);
static unsigned int code_base_ms = 70000;
module_param(code_base_ms, uint, 0444);
static unsigned int code_step_ms = 16000;
module_param(code_step_ms, uint, 0444);
static unsigned int late_ms = 400000;
module_param(late_ms, uint, 0444);
static char *smmu_node = "/soc/apps-smmu@15000000";
module_param(smmu_node, charp, 0444);
static char *smmu_dev = "15000000.apps-smmu";
module_param(smmu_dev, charp, 0444);
static bool stop_at_policy;
module_param(stop_at_policy, bool, 0444);
/* mode 6: the highest rung (the policy loaded) gets the first, shortest slot */
static bool ladder_desc;
module_param(ladder_desc, bool, 0444);
/* mode 6: the highest rung's own wait, until top_ms (0: its slot on the grid) */
static unsigned int top_ms;
module_param(top_ms, uint, 0444);

static bool policy_seen;

static bool notified;

/* mode 1: the modules that came in after this one, and how the last of them ended */
static unsigned int coming;
static bool last_live;
static char last_name[MODULE_NAME_LEN];

static int k618_panicsig_module(struct notifier_block *nb, unsigned long state, void *data)
{
	struct module *mod = data;

	if (state == MODULE_STATE_COMING) {
		coming++;
		last_live = false;
		strscpy(last_name, mod->name, sizeof(last_name));
	} else if (state == MODULE_STATE_LIVE) {
		last_live = true;
	}
	return NOTIFY_DONE;
}

static struct notifier_block k618_panicsig_module_nb = {
	.notifier_call = k618_panicsig_module,
};

/* busy-wait until ms since boot (local_clock, as printk's time stamps) */
static void wait_until(u64 ms)
{
	u64 now = div_u64(local_clock(), NSEC_PER_MSEC);

	if (ms > now)
		mdelay(ms - now);
}

/* mode 7: the code k618_probe panicked with */
#define PROBE_TAG "k618_probe: code "

static unsigned int probe_code(const char *msg)
{
	const char *p = strstr(msg, PROBE_TAG);
	unsigned int v = 0;

	if (!p)
		return 0;
	for (p += sizeof(PROBE_TAG) - 1; *p >= '0' && *p <= '9'; p++)
		v = v * 10 + (*p - '0');
	return v;
}

static unsigned int panic_kind(const char *msg)
{
	/* mode 6's own stop at the policy load (stop_at_policy) */
	if (strstr(msg, "k618_panicsig: stop"))
		return 6;
	if (strstr(msg, "Attempted to kill init"))
		return 1;
	if (strstr(msg, "Fatal exception in interrupt"))
		return 2;
	if (strstr(msg, "external abort") || strstr(msg, "SError"))
		return 3;
	if (strstr(msg, "Fatal exception"))
		return 4;
	return 5;
}

/* only the panicking CPU runs here: the line buffer can be static */
static char line[512];

static unsigned int panic_context(char *comm, size_t size)
{
	struct kmsg_dump_iter iter;
	size_t len;

	comm[0] = '\0';
	kmsg_dump_rewind(&iter);
	while (kmsg_dump_get_line(&iter, false, line, sizeof(line) - 1, &len)) {
		char *p;

		line[len] = '\0';
		p = strstr(line, "Comm: ");
		if (p) {
			strscpy(comm, p + 6, size);
			comm[strcspn(comm, " \n")] = '\0';
		}
	}
	if (!strncmp(comm, "swapper", 7))
		return 0;
	if (!strncmp(comm, "kworker", 7))
		return 1;
	return 2;
}

/* mode 2: how far the boot got after the modules, by the kernel's own messages */
static unsigned int boot_stage(void)
{
	struct kmsg_dump_iter iter;
	unsigned int stage = 0;
	size_t len;

	kmsg_dump_rewind(&iter);
	while (kmsg_dump_get_line(&iter, false, line, sizeof(line) - 1, &len)) {
		line[len] = '\0';
		if (strstr(line, "policy capability"))
			stage = max(stage, 3U);
		else if (strstr(line, "mounted with root inode") || strstr(line, "mounted filesystem"))
			stage = max(stage, 2U);
		else if (strstr(line, "[sda]"))
			stage = max(stage, 1U);
	}
	return stage;
}

/* mode 3: which kind of device a node belongs to, by the first compatible of it or of an ancestor */
static unsigned int k618_sup_class(struct device_node *np)
{
	struct device_node *p;

	for (p = np; p; p = p->parent) {
		const char *c;

		if (of_property_read_string(p, "compatible", &c))
			continue;
		if (strstr(c, "ufs-phy"))
			return 0;
		if (strstr(c, "gcc"))
			return 1;
		if (strstr(c, "rpmh-clk"))
			return 2;
		if (strstr(c, "smmu"))
			return 3;
		if (strstr(c, "noc") || strstr(c, "virt"))
			return 4;
		if (strstr(c, "bcm-voter"))
			return 5;
		if (strstr(c, "gdsc"))
			return 6;
		if (strstr(c, "regulator"))
			return 7;
		if (strstr(c, "pinctrl"))
			return 8;
	}
	return 9;
}

/* mode 3: why the node has no driver; only called from the panic notifier (other CPUs stopped) */
static unsigned int k618_inspect(void)
{
	struct device_node *np, *sup;
	struct platform_device *pdev;
	struct fwnode_handle *fwnode;
	struct fwnode_link *flink;
	struct device_link *link;
	struct device *dev;
	unsigned int code;

	np = of_find_node_by_path(node);
	if (!np)
		return 1;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return 2;
	dev = &pdev->dev;
	if (dev->driver) {
		code = boot_stage() >= 1 ? 3 : 4;
		goto out;
	}
	if (!dev->can_match) {
		code = 5;
		goto out;
	}
	code = 6;
	/* a supplier whose device does not exist yet shows as an unresolved firmware-node link */
	fwnode = dev_fwnode(dev);
	if (fwnode) {
		list_for_each_entry(flink, &fwnode->suppliers, c_hook) {
			sup = to_of_node(flink->supplier);
			if (!sup)
				continue;
			pr_emerg("k618_panicsig: %pOF waits for %pOF (no device)\n", dev_of_node(dev),
				 sup);
			code = 7 + k618_sup_class(sup);
			goto out;
		}
	}
	/* else a device link to a supplier that has no driver (and that the consumer waits for) */
	list_for_each_entry(link, &dev->links.suppliers, c_node) {
		if (link->flags & DL_FLAG_SYNC_STATE_ONLY)
			continue;
		if (link->supplier->driver)
			continue;
		sup = dev_of_node(link->supplier);
		pr_emerg("k618_panicsig: %pOF waits for %s (%pOF, not bound)\n", dev_of_node(dev),
			 dev_name(link->supplier), sup);
		code = 7 + (sup ? k618_sup_class(sup) : 9);
		goto out;
	}
out:
	put_device(dev);
	return code;
}

/* mode 4: 0 bound, 1 no device, 2 no driver matched, 3 its own probe defers or fails, 4 a supplier */
static unsigned int k618_state(const char *path)
{
	struct device_node *np, *sup;
	struct platform_device *pdev;
	struct fwnode_handle *fwnode;
	struct fwnode_link *flink;
	struct device_link *link;
	struct device *dev;
	unsigned int s;

	np = of_find_node_by_path(path);
	if (!np)
		return 1;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return 1;
	dev = &pdev->dev;
	if (dev->driver) {
		s = 0;
		goto out;
	}
	if (!dev->can_match) {
		s = 2;
		goto out;
	}
	s = 3;
	fwnode = dev_fwnode(dev);
	if (fwnode) {
		list_for_each_entry(flink, &fwnode->suppliers, c_hook) {
			sup = to_of_node(flink->supplier);
			if (!sup)
				continue;
			pr_emerg("k618_panicsig: %s waits for %pOF (no device)\n", path, sup);
			s = 4;
			goto out;
		}
	}
	list_for_each_entry(link, &dev->links.suppliers, c_node) {
		if (link->flags & DL_FLAG_SYNC_STATE_ONLY)
			continue;
		if (link->supplier->driver)
			continue;
		pr_emerg("k618_panicsig: %s waits for %s (class %u, not bound)\n", path,
			 dev_name(link->supplier),
			 k618_sup_class(dev_of_node(link->supplier)));
		s = 4;
		goto out;
	}
out:
	put_device(dev);
	pr_emerg("k618_panicsig: %s state %u\n", path, s);
	return s;
}

/* mode 4: the first node of the chain that has no driver, and in what way */
static char k618_chain_buf[1024];

static unsigned int k618_chain(unsigned int states)
{
	char *rest, *path;
	unsigned int i = 0;

	strscpy(k618_chain_buf, nodes, sizeof(k618_chain_buf));
	rest = k618_chain_buf;
	while ((path = strsep(&rest, ","))) {
		unsigned int s;

		if (!*path)
			continue;
		s = k618_state(path);
		if (s)
			return 1 + i * states + (states > 1 ? s - 1 : 0);
		i++;
	}
	return 1 + i * states;
}

/*
 * mode 6: the highest rung the boot reached, by the device model and the kernel's own messages;
 * rung 0 = rung 1 with an SMMU TLB sync timeout in the log as well
 */
static unsigned int k618_ladder(void)
{
	bool smmu_failed = false, tlb_timeout = false, disk = false, mounted = false;
	bool policy = policy_seen;
	struct kmsg_dump_iter iter;
	size_t len;

	kmsg_dump_rewind(&iter);
	while (kmsg_dump_get_line(&iter, false, line, sizeof(line) - 1, &len)) {
		line[len] = '\0';
		if (strstr(line, "TLB sync timed out"))
			tlb_timeout = true;
		if (strstr(line, "policy capability"))
			policy = true;
		else if (strstr(line, "mounted with root inode") || strstr(line, "mounted filesystem"))
			mounted = true;
		else if (strstr(line, "[sda]"))
			disk = true;
		else if (strstr(line, "Failed to register iommu") && strstr(line, smmu_dev))
			smmu_failed = true;
	}
	pr_emerg("k618_panicsig: smmu failed %d, tlb timeout %d, disk %d, mounted %d, policy %d\n",
		 smmu_failed, tlb_timeout, disk, mounted, policy);
	if (policy)
		return 7;
	if (mounted)
		return 6;
	if (disk)
		return 5;
	if (!k618_state(node))
		return 4;
	if (!k618_state(smmu_node))
		return 3;
	if (!smmu_failed)
		return 2;
	return tlb_timeout ? 0 : 1;
}

static int k618_panicsig_notify(struct notifier_block *nb, unsigned long event, void *data)
{
	char comm[TASK_COMM_LEN + 8];
	unsigned int kind = panic_kind(data ? data : "");
	unsigned int context;

	notified = true;
	if (mode == 7) {
		unsigned int code = probe_code(data ? data : "");
		u64 until;

		if (code) {
			until = (u64)code_base_ms + (u64)(code - 1) * code_step_ms;
			/* the panic came after this code's slot: say so instead of reading as a lower one */
			if (div_u64(local_clock(), NSEC_PER_MSEC) > until)
				until = late_ms;
		} else if (kind == 6) {
			until = top_ms;
		} else if (kind == 1) {
			until = pos_base_ms;
		} else {
			until = (u64)other_ms + (u64)(kind - 2) * other_step_ms;
		}
		wait_until(until);
		pr_emerg("k618_panicsig: kind %u, probe code %u: until %llu ms\n", kind, code, until);
		return NOTIFY_DONE;
	}
	if (mode >= 3 && mode <= 6) {
		unsigned int code = 0;
		u64 until;

		if (mode == 6 && kind == 6)
			kind = 1;
		if (kind != 1) {
			until = (u64)other_ms + (u64)(kind - 2) * other_step_ms;
		} else {
			unsigned int slot;

			code = mode == 3 ? k618_inspect() : mode == 6 ? k618_ladder() :
			       k618_chain(mode == 4 ? 4 : 1);
			/* mode 6: rungs 7..0 -> slots 1..8 reversed, else 7 -> 7 ... 1 -> 1 and 0 -> 8 */
			slot = mode != 6 ? code : ladder_desc ? 8 - code : code ? code : 8;
			until = (u64)code_base_ms + (u64)(slot - 1) * code_step_ms;
			if (mode == 6 && code == 7 && top_ms)
				until = top_ms;
			/* the panic came after this code's slot: say so instead of reading as a lower one */
			if (div_u64(local_clock(), NSEC_PER_MSEC) > until)
				until = late_ms;
		}
		wait_until(until);
		pr_emerg("k618_panicsig: kind %u, %s code %u: until %llu ms\n", kind,
			 mode == 4 || mode == 5 ? nodes : node, code, until);
		return NOTIFY_DONE;
	}
	if (mode == 2) {
		unsigned int pos = coming + (last_live ? 1 : 0), stage = 0;
		u64 until;

		if (kind != 1)
			until = (u64)other_ms + (u64)(kind - 2) * other_step_ms;
		else if (pos > total)
			until = (u64)all_ms + (u64)(stage = boot_stage()) * stage_ms;
		else if (pos > pos_from)
			until = (u64)pos_low_ms + (u64)(pos - pos_from) * pos_step_ms;
		else
			until = pos_low_ms;
		wait_until(until);
		pr_emerg("k618_panicsig: kind %u, module position %u of %u (last: %s, %s), stage %u: until %llu ms\n",
			 kind, pos, total, last_name, last_live ? "live" : "not live", stage, until);
		return NOTIFY_DONE;
	}
	if (mode == 1) {
		unsigned int pos = coming + (last_live ? 1 : 0);
		u64 until = kind == 1 ? (u64)pos_base_ms + (u64)pos * pos_step_ms :
					(u64)other_ms + (u64)(kind - 2) * other_step_ms;

		wait_until(until);
		pr_emerg("k618_panicsig: kind %u, module position %u (last: %s, %s): until %llu ms\n",
			 kind, pos, last_name, last_live ? "live" : "not live", until);
		return NOTIFY_DONE;
	}
	mdelay(kind * kind_ms);
	context = panic_context(comm, sizeof(comm));
	mdelay(context * context_ms);
	pr_emerg("k618_panicsig: kind %u, context %u (%s): waited %u ms\n", kind, context, comm,
		 kind * kind_ms + context * context_ms);
	return NOTIFY_DONE;
}

/* first in the chain, before anything that might reset or stop */
static struct notifier_block k618_panicsig_nb = {
	.notifier_call = k618_panicsig_notify,
	.priority = INT_MAX,
};

/*
 * rmode is the reboot mode, not ours: on this phone it is REBOOT_COLD (0) whatever init asks for, so
 * the wait has to come from this module's mode parameter.
 */
static int k618_panicsig_restart(struct notifier_block *nb, unsigned long rmode, void *cmd)
{
	if (!notified) {
		if (mode)
			wait_until(restart_at_ms);
		else
			mdelay(restart_ms);
		pr_emerg("k618_panicsig: restart without a panic notifier (reboot mode %lu, %s)\n",
			 rmode, cmd ? (char *)cmd : "");
	}
	return NOTIFY_DONE;
}

/* the first restart handler, ahead of PSCI's (129) */
static struct notifier_block k618_panicsig_restart_nb = {
	.notifier_call = k618_panicsig_restart,
	.priority = 255,
};

/* stop at the first SELinux policy load (selinux_policy_commit's AVC reset notification) */
static int k618_panicsig_lsm(struct notifier_block *nb, unsigned long event, void *data)
{
	if (event == LSM_POLICY_CHANGE && stop_at_policy && !policy_seen) {
		policy_seen = true;
		panic("k618_panicsig: stop at the SELinux policy load");
	}
	return NOTIFY_DONE;
}

static struct notifier_block k618_panicsig_lsm_nb = {
	.notifier_call = k618_panicsig_lsm,
};

static int __init k618_panicsig_init(void)
{
	register_module_notifier(&k618_panicsig_module_nb);
	atomic_notifier_chain_register(&panic_notifier_list, &k618_panicsig_nb);
	register_restart_handler(&k618_panicsig_restart_nb);
	if (stop_at_policy)
		register_blocking_lsm_notifier(&k618_panicsig_lsm_nb);
	pr_info("k618_panicsig: mode %u; kind %u ms, context %u ms, restart %u ms\n", mode, kind_ms,
		context_ms, restart_ms);
	return 0;
}
module_init(k618_panicsig_init);

static void __exit k618_panicsig_exit(void)
{
	if (stop_at_policy)
		unregister_blocking_lsm_notifier(&k618_panicsig_lsm_nb);
	unregister_module_notifier(&k618_panicsig_module_nb);
	unregister_restart_handler(&k618_panicsig_restart_nb);
	atomic_notifier_chain_unregister(&panic_notifier_list, &k618_panicsig_nb);
}
module_exit(k618_panicsig_exit);

MODULE_DESCRIPTION("6.12 bring-up: the kind of a panic as a wait before the reset");
MODULE_LICENSE("GPL");
