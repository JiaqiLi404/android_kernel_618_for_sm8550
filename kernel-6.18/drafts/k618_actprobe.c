// SPDX-License-Identifier: GPL-2.0
/* Device compatibility. */
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

static bool act = true, mem = true, ois = true;
module_param(act, bool, 0444);
module_param(mem, bool, 0444);
module_param(ois, bool, 0444);
static unsigned long pkt_parse_size = 0xcd8;	/* llvm-nm -S camera.ko: cam_actuator_i2c_pkt_parse */
module_param(pkt_parse_size, ulong, 0444);
static unsigned int max_mem = 600;
module_param(max_mem, uint, 0444);

static unsigned int off_act_state = 0xe88, off_act_index = 0x74;
static unsigned int off_ois_state = 0x1198, off_ois_name = 0x119c;
module_param(off_act_state, uint, 0444);
module_param(off_act_index, uint, 0444);
module_param(off_ois_state, uint, 0444);
module_param(off_ois_name, uint, 0444);

#define K618_CAM_CONFIG_DEV	0x105

static unsigned long pkt_parse_addr;
static atomic_t mem_lines = ATOMIC_INIT(0);

/* the last CONFIG_DEV seen per task, for the packet header dump */
static DEFINE_SPINLOCK(cfg_lock);
static struct {
	pid_t pid;
	u64 offset;
	u64 packet_handle;
} last_cfg[8];

static void stash_cfg(u64 offset, u64 handle)
{
	unsigned long flags;
	int i, slot = 0;

	spin_lock_irqsave(&cfg_lock, flags);
	for (i = 0; i < ARRAY_SIZE(last_cfg); i++)
		if (last_cfg[i].pid == current->pid || !last_cfg[i].pid) {
			slot = i;
			break;
		}
	last_cfg[slot].pid = current->pid;
	last_cfg[slot].offset = offset;
	last_cfg[slot].packet_handle = handle;
	spin_unlock_irqrestore(&cfg_lock, flags);
}

static bool find_cfg(u64 *offset, u64 *handle)
{
	unsigned long flags;
	bool found = false;
	int i;

	spin_lock_irqsave(&cfg_lock, flags);
	for (i = 0; i < ARRAY_SIZE(last_cfg); i++)
		if (last_cfg[i].pid == current->pid) {
			*offset = last_cfg[i].offset;
			*handle = last_cfg[i].packet_handle;
			found = true;
			break;
		}
	spin_unlock_irqrestore(&cfg_lock, flags);
	return found;
}

/* --- cam_actuator_driver_cmd(a_ctrl, cmd) */
static int act_cmd_pre(struct kprobe *p, struct pt_regs *regs)
{
	char *a_ctrl = (char *)regs->regs[0];
	u32 *cmd = (u32 *)regs->regs[1];
	u32 ctl[6] = {0};	/* op_code, size, handle_type, reserved, handle lo, handle hi */
	char name[24] = {0};
	u32 state = 0, index = 0;
	struct {
		s32 session_handle;
		s32 dev_handle;
		u64 offset;
		u64 packet_handle;
	} cfg = {0};
	u64 uptr;
	long ret;

	if (!a_ctrl || !cmd || copy_from_kernel_nofault(ctl, cmd, sizeof(ctl)))
		return 0;
	copy_from_kernel_nofault(name, a_ctrl, sizeof(name) - 1);
	copy_from_kernel_nofault(&state, a_ctrl + off_act_state, sizeof(state));
	copy_from_kernel_nofault(&index, a_ctrl + off_act_index, sizeof(index));
	uptr = ((u64)ctl[5] << 32) | ctl[4];
	if (ctl[0] != K618_CAM_CONFIG_DEV) {
		pr_info("k618_actprobe: %s[%u] %s/%d op 0x%x size %u htype %u state %u\n",
			name, index, current->comm, current->pid, ctl[0], ctl[1], ctl[2], state);
		return 0;
	}
	ret = copy_from_user_nofault(&cfg, (void __user *)uptr, sizeof(cfg));
	pr_info("k618_actprobe: %s[%u] %s/%d CONFIG_DEV state %u size %u: %s sess 0x%x dev 0x%x off 0x%llx pkt 0x%llx (idx %llu fd %llu)\n",
		name, index, current->comm, current->pid, state, ctl[1],
		ret ? "UNREADABLE" : "cfg", cfg.session_handle, cfg.dev_handle, cfg.offset,
		cfg.packet_handle, cfg.packet_handle & 0x7fff, (cfg.packet_handle >> 16) & 0xffff);
	if (!ret)
		stash_cfg(cfg.offset, cfg.packet_handle);
	return 0;
}

/* --- cam_mem_get_cpu_buf(handle, &vaddr, &len), only from cam_actuator_i2c_pkt_parse */
struct cpubuf_data {
	s32 handle;
	uintptr_t *vaddr_ptr;
	size_t *len_ptr;
};

static int cpubuf_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct cpubuf_data *d = (struct cpubuf_data *)ri->data;
	unsigned long lr = regs->regs[30];

	if (!pkt_parse_addr || lr - pkt_parse_addr >= pkt_parse_size)
		return 1;	/* not the actuator's: no return probe */
	d->handle = (s32)regs->regs[0];
	d->vaddr_ptr = (uintptr_t *)regs->regs[1];
	d->len_ptr = (size_t *)regs->regs[2];
	return 0;
}

static int cpubuf_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct cpubuf_data *d = (struct cpubuf_data *)ri->data;
	int rc = (int)regs_return_value(regs);
	uintptr_t vaddr = 0;
	size_t len = 0;
	u64 offset = 0, handle = 0;
	u32 hdr[6] = {0};	/* op_code, size, request_id lo/hi, flags, padding */

	if (rc) {
		pr_info("k618_actprobe: cam_mem_get_cpu_buf(0x%x idx %u) from the actuator: %d\n",
			d->handle, d->handle & 0x7fff, rc);
		return 0;
	}
	copy_from_kernel_nofault(&vaddr, d->vaddr_ptr, sizeof(vaddr));
	copy_from_kernel_nofault(&len, d->len_ptr, sizeof(len));
	if (find_cfg(&offset, &handle) && (s32)handle == d->handle && vaddr &&
		offset + sizeof(hdr) <= len &&
		!copy_from_kernel_nofault(hdr, (void *)(vaddr + offset), sizeof(hdr)))
		pr_info("k618_actprobe: packet 0x%x: len %zu off 0x%llx op_code 0x%x size %u req %llu flags 0x%x pad 0x%x\n",
			d->handle, len, offset, hdr[0], hdr[1],
			((u64)hdr[3] << 32) | hdr[2], hdr[4], hdr[5]);
	else
		pr_info("k618_actprobe: cmd buffer 0x%x: len %zu\n", d->handle, len);
	return 0;
}

static struct kretprobe krp_cpubuf = {
	.kp.symbol_name = "cam_mem_get_cpu_buf",
	.entry_handler = cpubuf_entry,
	.handler = cpubuf_ret,
	.data_size = sizeof(struct cpubuf_data),
	.maxactive = 32,
};

static struct kprobe kp_act_cmd = {
	.symbol_name = "cam_actuator_driver_cmd",
	.pre_handler = act_cmd_pre,
};

/* --- cam_mem_mgr_alloc_and_map(cmd_v2) / cam_mem_mgr_map(cmd_v2) */
struct memcmd_data {
	char *cmd;
};

static int memcmd_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	((struct memcmd_data *)ri->data)->cmd = (char *)regs->regs[0];
	return 0;
}

static void memcmd_log(const char *what, char *cmd, int rc, unsigned int off_flags,
	unsigned int off_handle, unsigned int off_len, bool len64)
{
	u32 flags = 0, handle = 0;
	u64 len = 0;

	if (copy_from_kernel_nofault(&flags, cmd + off_flags, sizeof(flags)) ||
		copy_from_kernel_nofault(&handle, cmd + off_handle, sizeof(handle)))
		return;
	copy_from_kernel_nofault(&len, cmd + off_len, len64 ? 8 : 4);
	if (rc || !(flags & BIT(3)))	/* CAM_MEM_FLAG_KMD_ACCESS */
		return;
	if (atomic_inc_return(&mem_lines) > max_mem)
		return;
	pr_info("k618_actprobe: %s %s/%d -> handle 0x%x (idx %u fd %u) flags 0x%x len 0x%llx\n",
		what, current->comm, current->pid, handle, handle & 0x7fff, handle >> 16, flags, len);
}

static int alloc_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	memcmd_log("alloc", ((struct memcmd_data *)ri->data)->cmd,
		(int)regs_return_value(regs), 0xe0, 0x100, 0x48, true);
	return 0;
}

static int map_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	memcmd_log("map", ((struct memcmd_data *)ri->data)->cmd,
		(int)regs_return_value(regs), 0x4c, 0xf0, 0xf4, false);
	return 0;
}

static struct kretprobe krp_alloc = {
	.kp.symbol_name = "cam_mem_mgr_alloc_and_map",
	.entry_handler = memcmd_entry,
	.handler = alloc_ret,
	.data_size = sizeof(struct memcmd_data),
	.maxactive = 16,
};

static struct kretprobe krp_map = {
	.kp.symbol_name = "cam_mem_mgr_map",
	.entry_handler = memcmd_entry,
	.handler = map_ret,
	.data_size = sizeof(struct memcmd_data),
	.maxactive = 16,
};

/* --- cam_ois_driver_cmd(o_ctrl, cmd) */
struct oiscmd_data {
	char *o_ctrl;
	u32 op_code;
};

static int oiscmd_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct oiscmd_data *d = (struct oiscmd_data *)ri->data;

	d->o_ctrl = (char *)regs->regs[0];
	d->op_code = 0;
	copy_from_kernel_nofault(&d->op_code, (void *)regs->regs[1], sizeof(d->op_code));
	return 0;
}

static int oiscmd_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct oiscmd_data *d = (struct oiscmd_data *)ri->data;
	char name[33] = {0};
	u32 state = 0;

	copy_from_kernel_nofault(name, d->o_ctrl + off_ois_name, 32);
	copy_from_kernel_nofault(&state, d->o_ctrl + off_ois_state, sizeof(state));
	pr_info("k618_actprobe: OIS %s %s/%d op 0x%x -> %d, state %u\n", name,
		current->comm, current->pid, d->op_code, (int)regs_return_value(regs), state);
	return 0;
}

static struct kretprobe krp_ois = {
	.kp.symbol_name = "cam_ois_driver_cmd",
	.entry_handler = oiscmd_entry,
	.handler = oiscmd_ret,
	.data_size = sizeof(struct oiscmd_data),
	.maxactive = 8,
};

static bool on_act, on_cpubuf, on_alloc, on_map, on_ois;

static int __init actprobe_init(void)
{
	struct kprobe kp_parse = { .symbol_name = "cam_actuator_i2c_pkt_parse" };
	int ret;

	if (act) {
		ret = register_kprobe(&kp_parse);	/* only to resolve its address */
		if (!ret) {
			pkt_parse_addr = (unsigned long)kp_parse.addr;
			unregister_kprobe(&kp_parse);
		}
		ret = register_kprobe(&kp_act_cmd);
		on_act = !ret;
		pr_info("k618_actprobe: %s: %d\n", kp_act_cmd.symbol_name, ret);
		ret = pkt_parse_addr ? register_kretprobe(&krp_cpubuf) : -ENOENT;
		on_cpubuf = !ret;
		pr_info("k618_actprobe: %s (callers in cam_actuator_i2c_pkt_parse %#lx, %#lx bytes): %d\n",
			krp_cpubuf.kp.symbol_name, pkt_parse_addr, pkt_parse_size, ret);
	}
	if (mem) {
		ret = register_kretprobe(&krp_alloc);
		on_alloc = !ret;
		pr_info("k618_actprobe: %s: %d\n", krp_alloc.kp.symbol_name, ret);
		ret = register_kretprobe(&krp_map);
		on_map = !ret;
		pr_info("k618_actprobe: %s: %d\n", krp_map.kp.symbol_name, ret);
	}
	if (ois) {
		ret = register_kretprobe(&krp_ois);
		on_ois = !ret;
		pr_info("k618_actprobe: %s: %d\n", krp_ois.kp.symbol_name, ret);
	}
	return 0;
}

static void __exit actprobe_exit(void)
{
	if (on_act)
		unregister_kprobe(&kp_act_cmd);
	if (on_cpubuf)
		unregister_kretprobe(&krp_cpubuf);
	if (on_alloc)
		unregister_kretprobe(&krp_alloc);
	if (on_map)
		unregister_kretprobe(&krp_map);
	if (on_ois)
		unregister_kretprobe(&krp_ois);
	pr_info("k618_actprobe: removed (%d memory lines)\n", atomic_read(&mem_lines));
}

module_init(actprobe_init);
module_exit(actprobe_exit);
MODULE_DESCRIPTION("6.18 bring-up: log what CamX hands the actuator at stream on (read-only)");
MODULE_LICENSE("GPL");
