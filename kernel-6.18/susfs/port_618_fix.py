# port_618_fix.py: what 6.18's build needs on top of the placement (exec'd by port_618.py).

# ---- 6.18 builds with -Wmissing-prototypes and CONFIG_WERROR. SUSFS declares the helpers fs/susfs.c
# gives the kernel only where each is used (extern), so the definitions have no prototype. They go
# into susfs.h, which susfs.c includes -- every use site's extern was compared with these by hand.
edit("include/linux/susfs.h",
     "/* susfs_init */\nvoid susfs_init(void);\n\n#endif\n",
     "/* susfs_init */\nvoid susfs_init(void);\n\n"
     "/*\n"
     " * k618: the helpers fs/susfs.c gives the kernel's own files, each declared extern where it is\n"
     " * used; their prototypes here, for 6.18's -Wmissing-prototypes (with CONFIG_WERROR)\n"
     " */\n"
     "struct mnt_idmap;\nstruct inode;\nstruct path;\nstruct kstat;\nstruct kstatfs;\nstruct seq_file;\nstruct filename;\n"
     "bool susfs_is_inode_sus_path(struct mnt_idmap* idmap, struct inode *inode);\n"
     "int susfs_get_data_path(struct path *path);\n"
     "bool susfs_is_inode_sus_kstat(struct inode *inode, bool *out_is_fuse);\n"
     "void susfs_sus_kstat_spoof_generic_fillattr(struct inode *inode, struct kstat *stat, u32 result_mask);\n"
     "void susfs_sus_kstat_spoof_show_map_vma(struct inode *inode, dev_t *out_dev, unsigned long *out_ino);\n"
     "int susfs_sus_kstat_spoof_vfs_statfs(struct inode *inode, struct kstatfs *buf, bool *is_fuse);\n"
     "void susfs_sus_kstat_spoof_inotify_fdinfo(unsigned long *out_target_ino, dev_t *out_target_dev);\n"
     "void susfs_sus_kstat_spoof_proc_fd_seq_show(int *out_target_mnt_id, unsigned long *out_target_ino, dev_t target_dev);\n"
     "void susfs_spoof_cmdline_or_bootconfig(struct seq_file *m);\n"
     "struct filename *susfs_open_redirect_spoof_do_sys_openat(struct inode *inode);\n"
     "int susfs_open_redirect_spoof_vfs_readlink(struct inode *inode, char __user *buffer, int buflen);\n"
     "int susfs_open_redirect_spoof_do_proc_readlink(struct inode *inode, char *tmp_buf, int buflen);\n"
     "int susfs_open_redirect_spoof_show_map_vma_srcu(struct inode *inode, unsigned long *out_ino, dev_t *out_dev, char **out_spoofed_name);\n"
     "\n#endif\n")

# ---- the same for the SUSFS helpers defined in fs/namespace.c, fs/statfs.c and the two SELinux
# wrappers KernelSU's selinux_hide.c calls (each declared extern where it is used)
edit("fs/namespace.c",
     "#ifdef CONFIG_KSU_SUSFS\n/* - To retrieve the non sus mnt_id from mount */\n",
     "#ifdef CONFIG_KSU_SUSFS\n"
     "/* k618: prototypes for 6.18's -Wmissing-prototypes; the users declare them extern */\n"
     "int susfs_get_non_sus_mnt_id_from_mnt(struct mount *orig_mnt);\n"
     "u64 susfs_get_non_sus_mnt_id_unique_from_mnt(struct mount *orig_mnt);\n"
     "struct vfsmount *susfs_get_non_sus_vfsmnt_from_vfsmnt(struct vfsmount *vfsmnt);\n\n"
     "/* - To retrieve the non sus mnt_id from mount */\n")
edit("fs/statfs.c",
     "#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\nint statfs_by_dentry_wrapper(struct dentry *dentry, struct kstatfs *buf){\n",
     "#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT\n"
     "/* k618: prototypes for 6.18's -Wmissing-prototypes */\n"
     "int statfs_by_dentry_wrapper(struct dentry *dentry, struct kstatfs *buf);\n"
     "int calculate_f_flags_wrapper(struct vfsmount *mnt);\n"
     "int statfs_by_dentry_wrapper(struct dentry *dentry, struct kstatfs *buf){\n")
edit("security/selinux/ss/services.c",
     "#ifdef CONFIG_KSU_SUSFS\nvoid security_dump_masked_av_fn(struct policydb *policydb,\n",
     "#ifdef CONFIG_KSU_SUSFS\n"
     "/* k618: KernelSU's selinux_hide.c calls these two; prototypes for 6.18's -Wmissing-prototypes */\n"
     "void security_dump_masked_av_fn(struct policydb *policydb, struct context *scontext,\n"
     "\t\t\t\tstruct context *tcontext, u16 tclass, u32 permissions,\n"
     "\t\t\t\tconst char *reason);\n"
     "void context_struct_compute_av_fn(struct policydb *policydb, struct context *scontext,\n"
     "\t\t\t\t  struct context *tcontext, u16 tclass,\n"
     "\t\t\t\t  struct av_decision *avd, struct extended_perms *xperms);\n"
     "void security_dump_masked_av_fn(struct policydb *policydb,\n")

# ---- fs/proc/task_mmu.c: the line SUSFS prints for an open-redirected file in /proc/<pid>/maps
# copies 6.12 Android's show_map_vma(), with its page-size-compat steps (__VM_NO_COMPAT,
# __fold_filemap_fixup_entry) -- gone in 6.18, where show_map() handles the padding around
# show_map_vma(). The line is printed as 6.18's own show_map_vma() prints one (the pad only
# before a name), so a spoofed line looks like every other.
edit("fs/proc/task_mmu.c",
     "\t\t\t\tend = VMA_PAD_START(vma);\n"
     "\t\t\t\tif (flags & __VM_NO_COMPAT) {\n"
     "\t\t\t\t\tsrcu_read_unlock(&susfs_srcu_open_redirect, srcu_idx);\n"
     "\t\t\t\t\treturn;\n"
     "\t\t\t\t}\n"
     "\t\t\t\t__fold_filemap_fixup_entry(&((struct proc_maps_private *)m->private)->iter, &end);\n"
     "\t\t\t\tshow_vma_header_prefix(m, start, end, flags, pgoff, dev, ino);\n"
     "\t\t\t\tseq_pad(m, ' ');\n"
     "\t\t\t\tif (spoofed_redirected_name)\n"
     "\t\t\t\t\tseq_puts(m, spoofed_redirected_name);\n"
     "\t\t\t\tseq_putc(m, '\\n');\n",
     "\t\t\t\tend = VMA_PAD_START(vma);\n"
     "\t\t\t\t/* k618: as 6.18's show_map_vma() prints a line (no page-size-compat steps here) */\n"
     "\t\t\t\tshow_vma_header_prefix(m, start, end, flags, pgoff, dev, ino);\n"
     "\t\t\t\tif (spoofed_redirected_name) {\n"
     "\t\t\t\t\tseq_pad(m, ' ');\n"
     "\t\t\t\t\tseq_puts(m, spoofed_redirected_name);\n"
     "\t\t\t\t}\n"
     "\t\t\t\tseq_putc(m, '\\n');\n")
