# port_618_ns.py: fs/namespace.c's part of port_618.py (exec'd by it; edit / replace_between are
# port_618.py's).

# ---- the include list ends with pidfs.h/nstree.h now
edit("fs/namespace.c",
     "#include <linux/nstree.h>\n\n#include \"pnode.h\"\n#include \"internal.h\"\n#include <trace/hooks/blk.h>\n\n",
     "#include <linux/nstree.h>\n"
     "#ifdef CONFIG_KSU_SUSFS\n#include <linux/susfs_def.h>\n#endif // #ifdef CONFIG_KSU_SUSFS\n\n"
     "#include \"pnode.h\"\n#include \"internal.h\"\n#include <trace/hooks/blk.h>\n\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "extern bool susfs_is_current_ksu_domain(void);\n"
     "extern struct static_key_true susfs_is_sdcard_android_data_not_decrypted;\n\n"
     "#define CL_COPY_MNT_NS BIT(25) /* used by copy_mnt_ns() */\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n\n")

# ---- mnt_id_ctr is a plain u64 (under mnt_id_xa's lock) now
edit("fs/namespace.c",
     "static u64 mnt_id_ctr = MNT_UNIQUE_ID_OFFSET;\n",
     "static u64 mnt_id_ctr = MNT_UNIQUE_ID_OFFSET;\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "static atomic64_t sus_mnt_id_ctr = ATOMIC64_INIT(0);\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n")

# ---- mount IDs live in an XArray now (6.12: an IDA). A KernelSU unshare copy carries the
# original's ID and is not in the XArray: erasing its ID would free the original's slot.
edit("fs/namespace.c",
     "static void mnt_free_id(struct mount *mnt)\n{\n\txa_erase(&mnt_id_xa, mnt->mnt_id);\n}\n",
     "static void mnt_free_id(struct mount *mnt)\n{\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "\tif (unlikely(mnt->mnt.mnt_flags & VFSMOUNT_MNT_FLAGS_KSU_UNSHARED_MNT))\n"
     "\t\treturn;\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "\txa_erase(&mnt_id_xa, mnt->mnt_id);\n}\n")

# ---- SUSFS's two copies of alloc_vfsmnt(), redone from 6.18's: they applied as 6.12's (IDA calls,
# mnt_slave_list/mnt_slave as list_heads, mnt_umounting, a NULL devname)
NS_HELPERS = r"""#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
/*
 * k618: SUSFS's two copies of alloc_vfsmnt(), redone from 6.18's alloc_vfsmnt(): mount IDs come
 * from the mnt_id_xa XArray (6.12: an IDA) and mnt_id_ctr is a plain u64 under its lock, a mount
 * without a name gets "none" (6.18 reads mnt_devname without a NULL check), mnt_slave_list and
 * mnt_slave are hlists and mnt_umounting is gone. What SUSFS does differently is kept: a mount a
 * KernelSU process unshares carries the original's ID (it is not in the XArray, and
 * mnt_free_id() leaves it alone), one a KernelSU process mounts or clones gets an ID from
 * DEFAULT_KSU_MNT_ID up, and neither takes a number from mnt_id_ctr.
 */

/* A copy of alloc_vfsmnt() but allocates the fake mnt_id for mounts
 * that are unshared by ksu process
 */
static struct mount *susfs_alloc_unshare_ksu_vfsmnt(const char *name, int old_mnt_id)
{
	struct mount *mnt = kmem_cache_zalloc(mnt_cache, GFP_KERNEL);

	if (mnt) {
		mnt->mnt_id = old_mnt_id;
		// - Do not inc the original mnt_id_ctr, instead, we do
		//   "mnt_id_ctr + atomic64_inc_return(&sus_mnt_id_ctr);"
		// - It will not do any harm for now even if the next few legit mounts
		//   collide with the same mnt_id_unique as the previous sus mounts.
		mnt->mnt_id_unique = READ_ONCE(mnt_id_ctr) + atomic64_inc_return(&sus_mnt_id_ctr);

		if (name)
			mnt->mnt_devname = kstrdup_const(name,
							 GFP_KERNEL_ACCOUNT);
		else
			mnt->mnt_devname = "none";
		if (!mnt->mnt_devname)
			goto out_free_cache;

#ifdef CONFIG_SMP
		mnt->mnt_pcp = alloc_percpu(struct mnt_pcp);
		if (!mnt->mnt_pcp)
			goto out_free_devname;

		this_cpu_add(mnt->mnt_pcp->mnt_count, 1);
#else
		mnt->mnt_count = 1;
		mnt->mnt_writers = 0;
#endif

		INIT_HLIST_NODE(&mnt->mnt_hash);
		INIT_LIST_HEAD(&mnt->mnt_child);
		INIT_LIST_HEAD(&mnt->mnt_mounts);
		INIT_LIST_HEAD(&mnt->mnt_list);
		INIT_LIST_HEAD(&mnt->mnt_expire);
		INIT_LIST_HEAD(&mnt->mnt_share);
		INIT_HLIST_HEAD(&mnt->mnt_slave_list);
		INIT_HLIST_NODE(&mnt->mnt_slave);
		INIT_HLIST_NODE(&mnt->mnt_mp_list);
		INIT_HLIST_HEAD(&mnt->mnt_stuck_children);
		RB_CLEAR_NODE(&mnt->mnt_node);
		mnt->mnt.mnt_idmap = &nop_mnt_idmap;
	}
	return mnt;

#ifdef CONFIG_SMP
out_free_devname:
	kfree_const(mnt->mnt_devname);
#endif
out_free_cache:
	kmem_cache_free(mnt_cache, mnt);
	return NULL;
}

/* A copy of alloc_vfsmnt() but allocates the fake mnt_id for mount
 * that is mounted or single cloned by ksu process
 */
static struct mount *susfs_alloc_non_unshare_ksu_vfsmnt(const char *name)
{
	struct mount *mnt = kmem_cache_zalloc(mnt_cache, GFP_KERNEL);
	int res;

	if (mnt) {
		xa_lock(&mnt_id_xa);
		res = __xa_alloc(&mnt_id_xa, &mnt->mnt_id, mnt,
				 XA_LIMIT(DEFAULT_KSU_MNT_ID, INT_MAX), GFP_KERNEL);
		// - Do not inc the original mnt_id_ctr, instead, we do
		//   "mnt_id_ctr + atomic64_inc_return(&sus_mnt_id_ctr);"
		// - It will not do any harm for now even if the next few legit mounts
		//   collide with the same mnt_id_unique as the previous sus mounts.
		if (!res)
			mnt->mnt_id_unique = mnt_id_ctr + atomic64_inc_return(&sus_mnt_id_ctr);
		xa_unlock(&mnt_id_xa);
		if (res)
			goto out_free_cache;

		if (name)
			mnt->mnt_devname = kstrdup_const(name,
							 GFP_KERNEL_ACCOUNT);
		else
			mnt->mnt_devname = "none";
		if (!mnt->mnt_devname)
			goto out_free_id;

#ifdef CONFIG_SMP
		mnt->mnt_pcp = alloc_percpu(struct mnt_pcp);
		if (!mnt->mnt_pcp)
			goto out_free_devname;

		this_cpu_add(mnt->mnt_pcp->mnt_count, 1);
#else
		mnt->mnt_count = 1;
		mnt->mnt_writers = 0;
#endif

		INIT_HLIST_NODE(&mnt->mnt_hash);
		INIT_LIST_HEAD(&mnt->mnt_child);
		INIT_LIST_HEAD(&mnt->mnt_mounts);
		INIT_LIST_HEAD(&mnt->mnt_list);
		INIT_LIST_HEAD(&mnt->mnt_expire);
		INIT_LIST_HEAD(&mnt->mnt_share);
		INIT_HLIST_HEAD(&mnt->mnt_slave_list);
		INIT_HLIST_NODE(&mnt->mnt_slave);
		INIT_HLIST_NODE(&mnt->mnt_mp_list);
		INIT_HLIST_HEAD(&mnt->mnt_stuck_children);
		RB_CLEAR_NODE(&mnt->mnt_node);
		mnt->mnt.mnt_idmap = &nop_mnt_idmap;
	}
	return mnt;

#ifdef CONFIG_SMP
out_free_devname:
	kfree_const(mnt->mnt_devname);
#endif
out_free_id:
	mnt_free_id(mnt);
out_free_cache:
	kmem_cache_free(mnt_cache, mnt);
	return NULL;
}
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT

"""
replace_between("fs/namespace.c",
                "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n/* A copy of alloc_vfsmnt() but allocates the fake mnt_id for mounts\n",
                "static struct mount *alloc_vfsmnt(const char *name)\n",
                NS_HELPERS)

# ---- vfs_create_mount hands fc->source to alloc_vfsmnt() as it is now (NULL is "none" there, and
# in the copy above)
edit("fs/namespace.c",
     "\tif (!fc->root)\n\t\treturn ERR_PTR(-EINVAL);\n\n"
     "\tmnt = alloc_vfsmnt(fc->source);\n\tif (!mnt)\n\t\treturn ERR_PTR(-ENOMEM);\n",
     "\tif (!fc->root)\n\t\treturn ERR_PTR(-EINVAL);\n\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "\t// - We will just stop checking for ksu process if /sdcard/Android is accessible,\n"
     "\t//   for the sake of performance\n"
     "\tif (static_branch_unlikely(&susfs_is_sdcard_android_data_not_decrypted)) {\n"
     "\t\tif (susfs_is_current_ksu_domain()) {\n"
     "\t\t\tmnt = susfs_alloc_non_unshare_ksu_vfsmnt(fc->source);\n"
     "\t\t\tgoto bypass_orig_flow;\n"
     "\t\t}\n"
     "\t}\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n\n"
     "\tmnt = alloc_vfsmnt(fc->source);\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "bypass_orig_flow:\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "\tif (!mnt)\n\t\treturn ERR_PTR(-ENOMEM);\n")

# ---- clone_mnt: the s_active count moved to setup_mnt() and the flags are copied with
# MNT_INTERNAL_FLAGS masked off now
CLONE_SUSFS = (
    "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
    "\tbool is_mnt_ksu_unshared = false;\n\n"
    "\t// - We will just stop checking for ksu process if /sdcard/Android is accessible,\n"
    "\t//   for the sake of performance\n"
    "\tif (static_branch_unlikely(&susfs_is_sdcard_android_data_not_decrypted)) {\n"
    "\t\t// - If /sdcard/Android is still not accessible, we keep checking for mounts\n"
    "\t\t//   mounted by ksu process\n"
    "\t\tif (susfs_is_current_ksu_domain()) {\n"
    "\t\t\t// - If it is unsharing, we re-use the old->mnt_id assign it for mnt->mnt_id directly\n"
    "\t\t\t//   without going thru ida, but we need to set a bit VFSMOUNT_MNT_FLAGS_KSU_UNSHARED_MNT\n"
    "\t\t\t//   on mnt->mnt.mnt_flags below, otherwise we find no other ways to identify if this\n"
    "\t\t\t//   mnt->mnt_id is assigned without ida when it is being freed in mnt_free_id().\n"
    "\t\t\tif (flag & CL_COPY_MNT_NS) {\n"
    "\t\t\t\tmnt = susfs_alloc_unshare_ksu_vfsmnt(old->mnt_devname, old->mnt_id);\n"
    "\t\t\t\tis_mnt_ksu_unshared = true;\n"
    "\t\t\t\tgoto bypass_orig_flow;\n"
    "\t\t\t}\n"
    "\t\t\t// else we just go assign fake mnt_id starting with DEFAULT_KSU_MNT_ID\n"
    "\t\t\tmnt = susfs_alloc_non_unshare_ksu_vfsmnt(old->mnt_devname);\n"
    "\t\t\tgoto bypass_orig_flow;\n"
    "\t\t}\n"
    "\t}\n\n"
    "\t// - We keep checking all processes and if old->mnt_id >= DEFAULT_KSU_MNT_ID,\n"
    "\t//   go assign fake mnt_id starting with DEFAULT_KSU_MNT_ID\n"
    "\tif (old->mnt_id >= DEFAULT_KSU_MNT_ID) {\n"
    "\t\tmnt = susfs_alloc_non_unshare_ksu_vfsmnt(old->mnt_devname);\n"
    "\t\tgoto bypass_orig_flow;\n"
    "\t}\n"
    "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n\n")
CLONE_FLAG = (
    "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
    "\t/*\n"
    "\t * k618: the flag means \"this mount's ID was not allocated\", so a clone carries it only when\n"
    "\t * it is itself a KernelSU unshare copy. 6.12 let every clone of such a mount inherit it\n"
    "\t * (MNT_INTERNAL_FLAGS does not cover it): the clone's own ID was then never freed, and\n"
    "\t * here its XArray slot would keep pointing at the freed mount.\n"
    "\t */\n"
    "\tmnt->mnt.mnt_flags &= ~VFSMOUNT_MNT_FLAGS_KSU_UNSHARED_MNT;\n"
    "\tif (unlikely(is_mnt_ksu_unshared))\n"
    "\t\tmnt->mnt.mnt_flags |= VFSMOUNT_MNT_FLAGS_KSU_UNSHARED_MNT;\n"
    "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n")
edit("fs/namespace.c",
     "\tstruct mount *mnt;\n\tint err;\n\n"
     "\tmnt = alloc_vfsmnt(old->mnt_devname);\n\tif (!mnt)\n\t\treturn ERR_PTR(-ENOMEM);\n\n"
     "\tmnt->mnt.mnt_flags = READ_ONCE(old->mnt.mnt_flags) &\n"
     "\t\t\t     ~MNT_INTERNAL_FLAGS;\n",
     "\tstruct mount *mnt;\n\tint err;\n"
     + CLONE_SUSFS +
     "\tmnt = alloc_vfsmnt(old->mnt_devname);\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "bypass_orig_flow:\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "\tif (!mnt)\n\t\treturn ERR_PTR(-ENOMEM);\n\n"
     "\tmnt->mnt.mnt_flags = READ_ONCE(old->mnt.mnt_flags) &\n"
     "\t\t\t     ~MNT_INTERNAL_FLAGS;\n"
     + CLONE_FLAG)

# ---- fs/proc_namespace.c: SUSFS's show functions for processes that must not see KernelSU's
# mounts are whole copies of 6.12's show_vfsmnt/show_mountinfo/show_vfsstat with a skip at the
# top. As wrappers the skip is all they add and every other line is printed by 6.18's own code --
# a copy prints what 6.12 printed, and any difference from what a process allowed to see
# everything reads is itself a tell.
replace_between("fs/proc_namespace.c",
                "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\nstatic int susfs_show_vfsmnt(",
                "static int mounts_open_common(",
                """#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
/*
 * k618: SUSFS's show functions, as wrappers of 6.18's own: a KernelSU mount is skipped, every
 * other line is printed exactly as show_vfsmnt/show_mountinfo/show_vfsstat print it (upstream
 * carries whole copies of 6.12's three).
 */
static int susfs_show_vfsmnt(struct seq_file *m, struct vfsmount *mnt)
{
	if (real_mount(mnt)->mnt_id >= DEFAULT_KSU_MNT_ID)
		return 0;
	return show_vfsmnt(m, mnt);
}

static int susfs_show_mountinfo(struct seq_file *m, struct vfsmount *mnt)
{
	if (real_mount(mnt)->mnt_id >= DEFAULT_KSU_MNT_ID)
		return 0;
	return show_mountinfo(m, mnt);
}

static int susfs_show_vfsstat(struct seq_file *m, struct vfsmount *mnt)
{
	if (real_mount(mnt)->mnt_id >= DEFAULT_KSU_MNT_ID)
		return 0;
	return show_vfsstat(m, mnt);
}
#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT

""")

# ---- copy_mnt_ns: CL_SHARED_TO_SLAVE is CL_SLAVE now
edit("fs/namespace.c",
     "\t\tcopy_flags |= CL_SLAVE;\n\tnew = copy_tree(old, old->mnt.mnt_root, copy_flags);\n",
     "\t\tcopy_flags |= CL_SLAVE;\n"
     "#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "\tcopy_flags |= CL_COPY_MNT_NS;\n"
     "#endif // #ifdef CONFIG_KSU_SUSFS_SUS_MOUNT\n"
     "\tnew = copy_tree(old, old->mnt.mnt_root, copy_flags);\n")
