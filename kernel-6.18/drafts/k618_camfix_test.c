// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>

static bool ubwc = true, cci = true, csiphy = true;
module_param(ubwc, bool, 0444);
module_param(cci, bool, 0444);
module_param(csiphy, bool, 0444);
/* Device compatibility. */
static unsigned long cfg_size = 0x1d10;
module_param(cfg_size, ulong, 0444);
static unsigned int off_hw_info = 34976, off_num_client = 496, off_mask = 8928;
module_param(off_hw_info, uint, 0444);
module_param(off_num_client, uint, 0444);
module_param(off_mask, uint, 0444);

static atomic_t masks_set = ATOMIC_INIT(0), redirects = ATOMIC_INIT(0), refgen_skips = ATOMIC_INIT(0);
static unsigned long dq_addr, cfg_addr;
static bool ubwc_on, cci_on, csiphy_on;

static int poll_pre(struct kprobe *p, struct pt_regs *regs)
{
	unsigned long lr = regs->regs[30];
	void *addr = (void *)regs->regs[0];
	u32 *status = (u32 *)regs->regs[5];

	if (regs->regs[3] != BIT(7) || regs->regs[4] != BIT(7) || lr - cfg_addr >= cfg_size)
		return 0;
	if (status)
		*status = BIT(7);
	regs->regs[0] = 0;
	instruction_pointer_set(regs, lr);
	if (atomic_inc_return(&refgen_skips) <= 8)
		pr_info("k618_camfix: CSIPHY REFGEN poll at %p skipped (%d)\n",
			addr, atomic_read(&refgen_skips));
	return 1;	/* back in cam_csiphy_core_cfg with 0, as if the bit had come */
}

static int ubwc_pre(struct kprobe *p, struct pt_regs *regs)
{
	char *bus_priv = (char *)regs->regs[0];
	char *hw = NULL;
	u32 num_client = 0;
	u64 mask = 1, *m;

	if (!bus_priv ||
	    copy_from_kernel_nofault(&hw, bus_priv + off_hw_info, sizeof(hw)) || !hw ||
	    copy_from_kernel_nofault(&num_client, hw + off_num_client, sizeof(num_client)) ||
	    copy_from_kernel_nofault(&mask, hw + off_mask, sizeof(mask)))
		return 0;
	if (mask || !num_client || num_client > 32)
		return 0;
	m = (u64 *)(hw + off_mask);
	WRITE_ONCE(*m, ~0ULL);
	atomic_inc(&masks_set);
	pr_info("k618_camfix: bus description %p (%u clients): ubwc_clients_mask 0 -> all\n",
		hw, num_client);
	return 0;
}

static int burst_pre(struct kprobe *p, struct pt_regs *regs)
{
	if (atomic_inc_return(&redirects) <= 8)
		pr_info("k618_camfix: cam_cci_data_queue_burst -> cam_cci_data_queue (%d)\n",
			atomic_read(&redirects));
	instruction_pointer_set(regs, dq_addr);
	return 1;	/* no single step: resume at cam_cci_data_queue, x0..x3 and lr as they are */
}

static struct kprobe kp_ubwc = {
	.symbol_name = "__cam_vfe_bus_ver3_process_cmd",
	.pre_handler = ubwc_pre,
};
static struct kprobe kp_burst = {
	.symbol_name = "cam_cci_data_queue_burst",
	.pre_handler = burst_pre,
};
static struct kprobe kp_poll = {
	.symbol_name = "cam_common_read_poll_timeout",
	.pre_handler = poll_pre,
};

static int __init camfix_init(void)
{
	struct kprobe kp_dq = { .symbol_name = "cam_cci_data_queue" };
	int ret;

	if (ubwc) {
		ret = register_kprobe(&kp_ubwc);
		ubwc_on = !ret;
		pr_info("k618_camfix: ubwc probe on %s: %d\n", kp_ubwc.symbol_name, ret);
	}
	if (cci) {
		ret = register_kprobe(&kp_dq);	/* only to resolve its address */
		if (!ret) {
			dq_addr = (unsigned long)kp_dq.addr;
			unregister_kprobe(&kp_dq);
			ret = register_kprobe(&kp_burst);
			cci_on = !ret;
		}
		pr_info("k618_camfix: cci redirect %s -> %s: %d\n", kp_burst.symbol_name,
			kp_dq.symbol_name, ret);
	}
	if (csiphy) {
		struct kprobe kp_cfg = { .symbol_name = "cam_csiphy_core_cfg" };

		ret = register_kprobe(&kp_cfg);	/* only to resolve its address */
		if (!ret) {
			cfg_addr = (unsigned long)kp_cfg.addr;
			unregister_kprobe(&kp_cfg);
			ret = register_kprobe(&kp_poll);
			csiphy_on = !ret;
		}
		pr_info("k618_camfix: csiphy refgen skip on %s (callers in %s, %#lx bytes): %d\n",
			kp_poll.symbol_name, kp_cfg.symbol_name, cfg_size, ret);
	}
	return 0;
}

static void __exit camfix_exit(void)
{
	if (ubwc_on)
		unregister_kprobe(&kp_ubwc);
	if (cci_on)
		unregister_kprobe(&kp_burst);
	if (csiphy_on)
		unregister_kprobe(&kp_poll);
	pr_info("k618_camfix: removed after %d mask(s) set, %d burst write(s) redirected, %d refgen poll(s) skipped\n",
		atomic_read(&masks_set), atomic_read(&redirects), atomic_read(&refgen_skips));
}

module_init(camfix_init);
module_exit(camfix_exit);
MODULE_DESCRIPTION("6.18 camera diagnostics: UBWC mask and CCI burst writes");
MODULE_LICENSE("GPL");
