#!/usr/bin/env python3
'Device support and compatibility helpers.'
import sys

tree = sys.argv[1]
EDITS = []


def edit(path, old, new):
    EDITS.append((path, old, new))


def replace_between(path, start, end, new):
    """the text from `start` (included) up to `end` (not included) becomes `new`; each marker once"""
    EDITS.append((path, ("__between__", start, end), new))


# ---- fs/Makefile: the obj-y list gained file_attr.o
edit("fs/Makefile",
     "\t\tkernel_read_file.o mnt_idmapping.o remap_range.o pidfs.o \\\n\t\tfile_attr.o\n\n",
     "\t\tkernel_read_file.o mnt_idmapping.o remap_range.o pidfs.o \\\n\t\tfile_attr.o\n\n"
     "obj-$(CONFIG_KSU_SUSFS) += susfs.o\n\n")

# ---- fs/exec.c: an Android include (dma-buf.h) now follows ksm.h
edit("fs/exec.c",
     "#include <linux/ksm.h>\n#include <linux/dma-buf.h>\n",
     "#include <linux/ksm.h>\n#include <linux/dma-buf.h>\n"
     "#ifdef CONFIG_KSU_SUSFS\n#include <linux/susfs_def.h>\n#endif\n")

# ---- fs/open.c: do_faccessat returns int now (was long)
edit("fs/open.c",
     "\nstatic int do_faccessat(int dfd, const char __user *filename, int mode, int flags)\n",
     "\n#ifdef CONFIG_KSU_SUSFS\n"
     "extern struct static_key_true ksu_su_compat_enabled;\n"
     "extern bool __ksu_is_allow_uid_for_current(uid_t uid);\n"
     "extern int ksu_handle_faccessat(int *dfd, struct filename **filename, int *mode, int *__unused_flags);\n"
     "#endif\n\n"
     "static int do_faccessat(int dfd, const char __user *filename, int mode, int flags)\n")

# ---- mm/memory.c: __access_remote_vm declares a folio now; the hunk as 6.12 has it (its check
# runs on a vma just set to NULL, so it never fires there either -- kept as upstream has it)
edit("mm/memory.c",
     "\t\tstruct folio *folio;\n\t\tstruct vm_area_struct *vma = NULL;\n"
     "\t\tstruct page *page = get_user_page_vma_remote(mm, addr,\n"
     "\t\t\t\t\t\t\t     gup_flags, &vma);\n",
     "\t\tstruct folio *folio;\n\t\tstruct vm_area_struct *vma = NULL;\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_MAP\n"
     "\t\tstruct page *page = NULL;\n\n"
     "\t\tif (vma && vma->vm_file && SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n"
     "\t\t\tbreak;\n\n"
     "\t\tpage = get_user_page_vma_remote(mm, addr,\n"
     "\t\t\t\t\t\t\t     gup_flags, &vma);\n"
     "#else\n"
     "\t\tstruct page *page = get_user_page_vma_remote(mm, addr,\n"
     "\t\t\t\t\t\t\t     gup_flags, &vma);\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MAP\n")

# ---- security/selinux/hooks.c: the line after selinux_state is another comment now
edit("security/selinux/hooks.c",
     "struct selinux_state selinux_state;\n\n/* SECMARK reference count */\n",
     "struct selinux_state selinux_state;\n"
     "#ifdef CONFIG_KSU_SUSFS\n"
     "extern struct selinux_policy *backup_sepolicy;\n"
     "extern bool ksu_selinux_hide_running __read_mostly;\n"
     "extern int security_context_to_sid_with_policy(struct selinux_policy *policy, const char *scontext, u32 scontext_len,\n"
     "                                               u32 *sid, u32 def_sid, gfp_t gfp_flags);\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS\n"
     "\n/* SECMARK reference count */\n")

# ---- fs/proc/base.c: the include list after cpufreq_times.h changed
edit("fs/proc/base.c",
     "#include <linux/cpufreq_times.h>\n#include <uapi/linux/lsm.h>\n",
     "#include <linux/cpufreq_times.h>\n"
     "#if defined(CONFIG_KSU_SUSFS_SUS_MAP) || defined(CONFIG_KSU_SUSFS_OPEN_REDIRECT)\n"
     "#include <linux/susfs_def.h>\n"
     "#endif // #if defined(CONFIG_KSU_SUSFS_SUS_MAP) || defined(CONFIG_KSU_SUSFS_OPEN_REDIRECT)\n\n"
     "#include <uapi/linux/lsm.h>\n")

# ---- fs/proc/task_mmu.c: show_smap checks vma_data_pages() now
edit("fs/proc/task_mmu.c",
     "\tstruct mem_size_stats mss = {};\n\n\tif (!vma_data_pages(vma))\n\t\tgoto show_pad;\n",
     "\tstruct mem_size_stats mss = {};\n\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_MAP\n"
     "\tif (vma->vm_file) {\n"
     "\t\tif (SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n"
     "\t\t\treturn 0;\n"
     "\t}\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MAP\n\n"
     "\tif (!vma_data_pages(vma))\n\t\tgoto show_pad;\n")

# ---- fs/proc/task_mmu.c: show_smaps_rollup walks VMAs under per-VMA locks now, and 6.12's
# "Case 4" (a VMA that grew while the lock was dropped) is the vm_start < last_vma_end branch. As
# 6.12 does on its main path: a hidden mapping's stats are skipped and last_vma_end moves on.
edit("fs/proc/task_mmu.c",
     "\t\tif (vma->vm_start < last_vma_end) {\n"
     "\t\t\t/*\n"
     "\t\t\t * After retaking the lock, already reported VMA grew\n",
     "#ifdef CONFIG_KSU_SUSFS_SUS_MAP\n"
     "\t\tif (vma->vm_file && SUSFS_IS_INODE_SUS_MAP(file_inode(vma->vm_file)))\n"
     "\t\t\tgoto bypass_orig_flow;\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MAP\n"
     "\t\tif (vma->vm_start < last_vma_end) {\n"
     "\t\t\t/*\n"
     "\t\t\t * After retaking the lock, already reported VMA grew\n")
edit("fs/proc/task_mmu.c",
     "\t\t\tsmap_gather_stats(priv, vma, &mss);\n\t\t}\n\t\tlast_vma_end = vma->vm_end;\n",
     "\t\t\tsmap_gather_stats(priv, vma, &mss);\n\t\t}\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_MAP\n"
     "bypass_orig_flow:\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MAP\n"
     "\t\tlast_vma_end = vma->vm_end;\n")

# ---- fs/stat.c: a timestamp trace header sits between asm/unistd.h and internal.h now
edit("fs/stat.c",
     "#include <linux/iversion.h>\n\n#include <linux/uaccess.h>\n#include <asm/unistd.h>\n",
     "#include <linux/iversion.h>\n\n"
     "#ifdef CONFIG_KSU_SUSFS\n#include <linux/susfs_def.h>\n#endif // #ifdef CONFIG_KSU_SUSFS\n"
     "#include <linux/uaccess.h>\n#include <asm/unistd.h>\n")
edit("fs/stat.c",
     "#include \"internal.h\"\n#include \"mount.h\"\n\n",
     "#include \"internal.h\"\n#include \"mount.h\"\n\n"
     "#ifdef CONFIG_KSU_SUSFS\n"
     "extern struct static_key_true ksu_is_init_rc_hook_enabled;\n"
     "extern void ksu_handle_vfs_fstat(int fd, loff_t *kstat_size_ptr);\n"
     "extern struct static_key_true ksu_su_compat_enabled;\n"
     "extern bool __ksu_is_allow_uid_for_current(uid_t uid);\n"
     "extern int ksu_handle_stat(int *dfd, struct filename **filename, int *flags);\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\n"
     "extern bool susfs_is_inode_sus_kstat(struct inode *inode, bool *out_is_fuse);\n"
     "extern void susfs_sus_kstat_spoof_generic_fillattr(struct inode *inode, struct kstat *stat, u32 result_mask);\n"
     "extern u64 susfs_get_non_sus_mnt_id_unique_from_mnt(struct mount *orig_mnt);\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\n\n")

# ---- fs/stat.c: vfs_getattr_nosec is an if/else now, with block devices' bdev_statx() after it
# and no AT_GETATTR_NOSEC. 6.12's SUSFS marks a spoofed inode before the getattr and, once the
# attributes are filled (by the fs or generic_fillattr), spoofs them and returns -- kept, on both
# branches; a spoofed inode returns before bdev_statx as it returned before everything in 6.12.
edit("fs/stat.c",
     "\tidmap = mnt_idmap(path->mnt);\n"
     "\tif (inode->i_op->getattr) {\n"
     "\t\tint ret;\n\n"
     "\t\tret = inode->i_op->getattr(idmap, path, stat, request_mask,\n"
     "\t\t\t\tquery_flags);\n"
     "\t\tif (ret)\n"
     "\t\t\treturn ret;\n"
     "\t} else {\n"
     "\t\tgeneric_fillattr(idmap, request_mask, inode, stat);\n"
     "\t}\n",
     "\tidmap = mnt_idmap(path->mnt);\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\n"
     "\tif (susfs_is_current_app_uid()) {\n"
     "\t\tbool is_fuse = false;\n"
     "\t\tif (susfs_is_inode_sus_kstat(d_backing_inode(path->dentry), &is_fuse)) {\n"
     "\t\t\tif (!is_fuse) {\n"
     "\t\t\t\tstat->mnt_id = real_mount(path->mnt)->mnt_id;\n"
     "\t\t\t\tstat->result_mask |= STATX_SUS_KSTAT;\n"
     "\t\t\t}\n"
     "\t\t\tstat->mnt_id = real_mount(path->mnt)->mnt_id;\n"
     "\t\t\tstat->result_mask |= STATX_SUS_KSTAT_FUSE;\n"
     "\t\t}\n"
     "\t}\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\n"
     "\tif (inode->i_op->getattr) {\n"
     "\t\tint ret;\n\n"
     "\t\tret = inode->i_op->getattr(idmap, path, stat, request_mask,\n"
     "\t\t\t\tquery_flags);\n"
     "\t\tif (ret)\n"
     "\t\t\treturn ret;\n"
     "\t} else {\n"
     "\t\tgeneric_fillattr(idmap, request_mask, inode, stat);\n"
     "\t}\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\n"
     "\tif (stat->result_mask & STATX_SUS_KSTAT) {\n"
     "\t\tsusfs_sus_kstat_spoof_generic_fillattr(inode, stat, STATX_SUS_KSTAT);\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "\tif (stat->result_mask & STATX_SUS_KSTAT_FUSE) {\n"
     "\t\tsusfs_sus_kstat_spoof_generic_fillattr(inode, stat, STATX_SUS_KSTAT_FUSE);\n"
     "\t\treturn 0;\n"
     "\t}\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\n")

# ---- fs/stat.c: vfs_fstat holds the fd by CLASS(fd_raw) now and returns vfs_getattr() directly.
# KernelSU (in SUSFS mode its init.rc injection has no hook of its own) reports init.rc's size
# with the lines it adds -- after the getattr, whatever it returned, as 6.12 does.
edit("fs/stat.c",
     "int vfs_fstat(int fd, struct kstat *stat)\n{\n"
     "\tCLASS(fd_raw, f)(fd);\n"
     "\tif (fd_empty(f))\n"
     "\t\treturn -EBADF;\n"
     "\treturn vfs_getattr(&fd_file(f)->f_path, stat, STATX_BASIC_STATS, 0);\n}\n",
     "int vfs_fstat(int fd, struct kstat *stat)\n{\n"
     "#ifdef CONFIG_KSU_SUSFS\n"
     "\tint error;\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS\n"
     "\tCLASS(fd_raw, f)(fd);\n"
     "\tif (fd_empty(f))\n"
     "\t\treturn -EBADF;\n"
     "#ifdef CONFIG_KSU_SUSFS\n"
     "\terror = vfs_getattr(&fd_file(f)->f_path, stat, STATX_BASIC_STATS, 0);\n"
     "\tif (static_branch_unlikely(&ksu_is_init_rc_hook_enabled))\n"
     "\t\tksu_handle_vfs_fstat(fd, &stat->size);\n"
     "\treturn error;\n"
     "#else\n"
     "\treturn vfs_getattr(&fd_file(f)->f_path, stat, STATX_BASIC_STATS, 0);\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS\n}\n")

# ---- fs/stat.c: vfs_statx_path returns on a getattr error first now; the spoofed mount id
# after it (the bypass_orig_flow label below applied as it is)
edit("fs/stat.c",
     "\tint error = vfs_getattr(path, stat, request_mask, flags);\n"
     "\tif (error)\n"
     "\t\treturn error;\n\n"
     "\tif (request_mask & STATX_MNT_ID_UNIQUE) {\n",
     "\tint error = vfs_getattr(path, stat, request_mask, flags);\n"
     "\tif (error)\n"
     "\t\treturn error;\n\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\n"
     "\tif (stat->result_mask & (STATX_SUS_KSTAT_FUSE | STATX_SUS_KSTAT)) {\n"
     "\t\tstat->result_mask &= ~(STATX_SUS_KSTAT_FUSE | STATX_SUS_KSTAT);\n"
     "\t\tif (request_mask & STATX_MNT_ID_UNIQUE) {\n"
     "\t\t\tstat->mnt_id = susfs_get_non_sus_mnt_id_unique_from_mnt(real_mount(path->mnt));\n"
     "\t\t\tstat->result_mask |= STATX_MNT_ID_UNIQUE;\n"
     "\t\t} else {\n"
     "\t\t\tstat->result_mask |= STATX_MNT_ID;\n"
     "\t\t}\n"
     "\t\tgoto bypass_orig_flow;\n"
     "\t}\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\n\n"
     "\tif (request_mask & STATX_MNT_ID_UNIQUE) {\n")

# ---- fs/stat.c: vfs_statx computes its lookup flags with statx_lookup_flags() now
edit("fs/stat.c",
     "\tunsigned int lookup_flags = statx_lookup_flags(flags);\n"
     "\tint error;\n\n"
     "\tif (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH |\n",
     "\tunsigned int lookup_flags = statx_lookup_flags(flags);\n"
     "\tint error;\n\n"
     "#ifdef CONFIG_KSU_SUSFS\n"
     "\tif (likely(susfs_is_current_proc_no_su()))\n"
     "\t\tgoto orig_flow;\n\n"
     "\tif (static_branch_likely(&ksu_su_compat_enabled)) {\n"
     "\t\tif (unlikely(__ksu_is_allow_uid_for_current(current_uid().val)))\n"
     "\t\t\tksu_handle_stat(&dfd, &filename, &flags);\n"
     "\t}\n\n"
     "orig_flow:\n"
     "#endif\n"
     "\tif (flags & ~(AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH |\n")

# ---- fs/namei.c: lookup_one_qstr_excl goes to a common `found:` tail now, also from a dcache hit,
# and only sets `dir` on the slow path. 6.12 checks for a hidden path on the slow path only (a
# dcache hit returned before it) and retries the lookup in `dir`: the check goes right after the
# slow lookup, before `found:` -- on a dcache hit `dir` is unset and the retry would use it.
edit("fs/namei.c",
     "\tstruct dentry *dentry;\n\tstruct dentry *old;\n\tstruct inode *dir;\n\n"
     "\tdentry = lookup_dcache(name, base, flags);\n\tif (dentry)\n\t\tgoto found;\n",
     "\tstruct dentry *dentry;\n\tstruct dentry *old;\n\tstruct inode *dir;\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_PATH\n\tbool found_sus_path = false;\n#endif\n\n"
     "\tdentry = lookup_dcache(name, base, flags);\n\tif (dentry)\n\t\tgoto found;\n")
edit("fs/namei.c",
     "\t\tdentry = old;\n\t}\nfound:\n\tif (IS_ERR(dentry))\n\t\treturn dentry;\n",
     "\t\tdentry = old;\n\t}\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_PATH\n"
     "\tif (unlikely(dentry) && !IS_ERR(dentry) && dentry->d_inode && !found_sus_path && susfs_is_inode_sus_path(&nop_mnt_idmap, dentry->d_inode)) {\n"
     "\t\tif (d_in_lookup(dentry))\n"
     "\t\t\td_lookup_done(dentry);\n"
     "\t\tif (!(flags & LOOKUP_RCU))\n"
     "\t\t\tdput(dentry);\n"
     "\t\tdentry = d_alloc(base, &susfs_fake_qstr_name);\n"
     "\t\tfound_sus_path = true;\n"
     "\t\tgoto retry;\n"
     "\t}\n"
     "#endif\n"
     "found:\n\tif (IS_ERR(dentry))\n\t\treturn dentry;\n")

# ---- fs/namei.c: lookup_fast's non-RCU branch -- d_revalidate() takes the parent inode and the
# name now
edit("fs/namei.c",
     "\t} else {\n\t\tdentry = __d_lookup(parent, &nd->last);\n\t\tif (unlikely(!dentry))\n\t\t\treturn NULL;\n",
     "\t} else {\n\t\tdentry = __d_lookup(parent, &nd->last);\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_PATH\n"
     "\t\tif (is_nd_state_lookup_last_and_open_last && dentry && !IS_ERR(dentry) && dentry->d_inode &&\n"
     "\t\t\tsusfs_is_inode_sus_path(&nop_mnt_idmap, dentry->d_inode))\n"
     "\t\t{\n"
     "\t\t\tif (d_in_lookup(dentry))\n"
     "\t\t\t\td_lookup_done(dentry);\n"
     "\t\t\tdput(dentry);\n"
     "\t\t\tdentry = NULL;\n"
     "\t\t}\n"
     "#endif\n"
     "\t\tif (unlikely(!dentry))\n\t\t\treturn NULL;\n")

# ---- fs/namei.c: vfs_readlink -- readlink_copy() takes the length now, and 6.14+ returns a cached
# link (IOP_CACHED_LINK) before anything else: the spoof goes in front of that return as well as
# before the final copy, so an open-redirected symlink is spoofed whichever way its link is read
edit("fs/namei.c",
     "\tif (inode->i_opflags & IOP_CACHED_LINK)\n"
     "\t\treturn readlink_copy(buffer, buflen, inode->i_link, inode->i_linklen);\n",
     "#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT\n"
     "\tif ((inode->i_opflags & IOP_CACHED_LINK) && SUSFS_IS_INODE_OPEN_REDIRECT(inode)) {\n"
     "\t\tres = susfs_open_redirect_spoof_vfs_readlink(inode, buffer, buflen);\n"
     "\t\tif (!res)\n"
     "\t\t\treturn res;\n"
     "\t}\n"
     "#endif\n"
     "\tif (inode->i_opflags & IOP_CACHED_LINK)\n"
     "\t\treturn readlink_copy(buffer, buflen, inode->i_link, inode->i_linklen);\n")
edit("fs/namei.c",
     "\t\t\treturn PTR_ERR(link);\n\t}\n"
     "\tres = readlink_copy(buffer, buflen, link, strlen(link));\n\tdo_delayed_call(&done);\n",
     "\t\t\treturn PTR_ERR(link);\n\t}\n"
     "#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT\n"
     "\tif (SUSFS_IS_INODE_OPEN_REDIRECT(inode)) {\n"
     "\t\tres = susfs_open_redirect_spoof_vfs_readlink(inode, buffer, buflen);\n"
     "\t\tif (!res) {\n"
     "\t\t\tdo_delayed_call(&done);\n"
     "\t\t\treturn res;\n"
     "\t\t}\n"
     "\t}\n"
     "#endif\n"
     "\tres = readlink_copy(buffer, buflen, link, strlen(link));\n\tdo_delayed_call(&done);\n")

# ---- include/linux/susfs_def.h (copied from susfs4ksu): ND_FLAGS_LOOKUP_LAST is set in nd->flags
# for the last component and never cleared. 6.12 had nothing at 0x2000000; 6.18 numbers the LOOKUP_
# flags by bit and BIT(25) is LOOKUP_NO_MAGICLINKS -- with it set, nd_jump_link() refuses every
# /proc/<pid>/fd/N, exe, cwd... reached through a slow lookup (-ELOOP). BIT(15) is one of the five
# spare pathwalk bits include/linux/namei.h leaves (11-15), the top one.
edit("include/linux/susfs_def.h",
     "#define ND_FLAGS_LOOKUP_LAST 0x2000000\n",
     "#define ND_FLAGS_LOOKUP_LAST BIT(15) /* k618: 0x2000000 is LOOKUP_NO_MAGICLINKS on 6.18 */\n")

# ---- fs/namespace.c and fs/readdir.c: in files of their own
import os  # noqa: E402
_here = os.path.dirname(os.path.abspath(__file__))
for _part in ("port_618_ns.py", "port_618_rd.py", "port_618_fix.py"):
    _f = os.path.join(_here, _part)
    if os.path.exists(_f):
        exec(open(_f, encoding="utf-8").read())


def main():
    bad = 0
    for path, old, new in EDITS:
        p = f"{tree}/{path}"
        s = open(p, encoding="utf-8").read()
        if isinstance(old, tuple):
            _, start, end = old
            if s.count(start) != 1 or s.count(end) != 1 or s.index(start) > s.index(end):
                print(f"NOT APPLIED {path}: block markers occur {s.count(start)} and {s.count(end)} times")
                bad += 1
                continue
            i, j = s.index(start), s.index(end)
            open(p, "w", encoding="utf-8").write(s[:i] + new + s[j:])
            print(f"ok  {path} (block)")
            continue
        n = s.count(old)
        if n != 1:
            print(f"NOT APPLIED {path}: the text to change occurs {n} times")
            bad += 1
            continue
        open(p, "w", encoding="utf-8").write(s.replace(old, new, 1))
        print(f"ok  {path}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
