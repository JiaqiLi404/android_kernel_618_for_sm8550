# port_618_rd.py: fs/readdir.c's part of port_618.py (exec'd by it; `edit` is port_618.py's).
#
# SUSFS hides a path from directory listings the same way in every getdents variant: the callback
# struct carries the directory's super_block and idmap, the syscall fills them in before
# iterate_dir(), and the actor looks the entry's inode up and skips it if it is a hidden path. In
# 6.18 the syscalls hold the file by CLASS(fd_pos), the count moved into dir_context, and the
# actors mask d_type first -- so the struct and actor hunks of fillonedir/compat_fillonedir
# applied but every syscall hunk failed. All four parts must go in together: a struct whose sb is
# never filled in has the actor call ilookup(NULL, ino).

SB_FIELDS = ("#ifdef CONFIG_KSU_SUSFS_SUS_PATH\n"
             "\tstruct super_block *sb;\n"
             "\tstruct mnt_idmap *idmap;\n"
             "#endif\n")
INODE_DECL = ("#ifdef CONFIG_KSU_SUSFS_SUS_PATH\n"
              "\tstruct inode *inode;\n"
              "#endif\n")
SKIP_SUS = ("#ifdef CONFIG_KSU_SUSFS_SUS_PATH\n"
            "\tinode = ilookup(buf->sb, ino);\n"
            "\tif (!inode) {\n"
            "\t\tgoto orig_flow;\n"
            "\t}\n"
            "\tif (susfs_is_inode_sus_path(buf->idmap, inode)) {\n"
            "\t\tiput(inode);\n"
            "\t\treturn true;\n"
            "\t}\n"
            "\tiput(inode);\n"
            "orig_flow:\n"
            "#endif\n")
SET_SB = ("#ifdef CONFIG_KSU_SUSFS_SUS_PATH\n"
          "\tbuf.sb = fd_file(f)->f_inode->i_sb;\n"
          "\tbuf.idmap = mnt_idmap(fd_file(f)->f_path.mnt);\n"
          "#endif\n\n")
EBADF = "\tif (fd_empty(f))\n\t\treturn -EBADF;\n\n"

# ---- old_readdir: its struct and fillonedir applied; the syscall fills the two fields in
edit("fs/readdir.c",
     "\t\t.ctx.actor = fillonedir,\n\t\t.ctx.count = 1, /* Hint to fs: just one entry. */\n"
     "\t\t.dirent = dirent\n\t};\n\n" + EBADF,
     "\t\t.ctx.actor = fillonedir,\n\t\t.ctx.count = 1, /* Hint to fs: just one entry. */\n"
     "\t\t.dirent = dirent\n\t};\n\n" + EBADF + SET_SB)

# ---- getdents: struct, filldir (declaration, then the check right before the entry is written,
# after the signal test -- where 6.12 has it), syscall
edit("fs/readdir.c",
     "struct getdents_callback {\n\tstruct dir_context ctx;\n\tstruct linux_dirent __user * current_dir;\n",
     "struct getdents_callback {\n\tstruct dir_context ctx;\n\tstruct linux_dirent __user * current_dir;\n" + SB_FIELDS)
edit("fs/readdir.c",
     "\tint reclen = ALIGN(offsetof(struct linux_dirent, d_name) + namlen + 2,\n"
     "\t\tsizeof(long));\n\tint prev_reclen;\n\tunsigned int flags = d_type;\n",
     "\tint reclen = ALIGN(offsetof(struct linux_dirent, d_name) + namlen + 2,\n"
     "\t\tsizeof(long));\n\tint prev_reclen;\n\tunsigned int flags = d_type;\n" + INODE_DECL)
edit("fs/readdir.c",
     "\tdirent = buf->current_dir;\n\tprev = (void __user *) dirent - prev_reclen;\n"
     "\tif (!user_write_access_begin(prev, reclen + prev_reclen))\n\t\tgoto efault;\n\n"
     "\t/* This might be 'dirent->d_off', but if so it will get overwritten */\n",
     SKIP_SUS +
     "\tdirent = buf->current_dir;\n\tprev = (void __user *) dirent - prev_reclen;\n"
     "\tif (!user_write_access_begin(prev, reclen + prev_reclen))\n\t\tgoto efault;\n\n"
     "\t/* This might be 'dirent->d_off', but if so it will get overwritten */\n")
edit("fs/readdir.c",
     "\t\t.ctx.actor = filldir,\n\t\t.ctx.count = count,\n\t\t.ctx.dt_flags_mask = FILLDIR_FLAG_NOINTR,\n"
     "\t\t.current_dir = dirent\n\t};\n\tint error;\n\n" + EBADF,
     "\t\t.ctx.actor = filldir,\n\t\t.ctx.count = count,\n\t\t.ctx.dt_flags_mask = FILLDIR_FLAG_NOINTR,\n"
     "\t\t.current_dir = dirent\n\t};\n\tint error;\n\n" + EBADF + SET_SB)

# ---- getdents64
edit("fs/readdir.c",
     "struct getdents_callback64 {\n\tstruct dir_context ctx;\n\tstruct linux_dirent64 __user * current_dir;\n",
     "struct getdents_callback64 {\n\tstruct dir_context ctx;\n\tstruct linux_dirent64 __user * current_dir;\n" + SB_FIELDS)
edit("fs/readdir.c",
     "\tint reclen = ALIGN(offsetof(struct linux_dirent64, d_name) + namlen + 1,\n"
     "\t\tsizeof(u64));\n\tint prev_reclen;\n\tunsigned int flags = d_type;\n",
     "\tint reclen = ALIGN(offsetof(struct linux_dirent64, d_name) + namlen + 1,\n"
     "\t\tsizeof(u64));\n\tint prev_reclen;\n\tunsigned int flags = d_type;\n" + INODE_DECL)
edit("fs/readdir.c",
     "\tdirent = buf->current_dir;\n\tprev = (void __user *)dirent - prev_reclen;\n",
     SKIP_SUS + "\tdirent = buf->current_dir;\n\tprev = (void __user *)dirent - prev_reclen;\n")
edit("fs/readdir.c",
     "\t\t.ctx.actor = filldir64,\n\t\t.ctx.count = count,\n\t\t.ctx.dt_flags_mask = FILLDIR_FLAG_NOINTR,\n"
     "\t\t.current_dir = dirent\n\t};\n\tint error;\n\n" + EBADF,
     "\t\t.ctx.actor = filldir64,\n\t\t.ctx.count = count,\n\t\t.ctx.dt_flags_mask = FILLDIR_FLAG_NOINTR,\n"
     "\t\t.current_dir = dirent\n\t};\n\tint error;\n\n" + EBADF + SET_SB)

# ---- compat old_readdir: its struct and compat_fillonedir applied
edit("fs/readdir.c",
     "\t\t.ctx.actor = compat_fillonedir,\n\t\t.ctx.count = 1, /* Hint to fs: just one entry. */\n"
     "\t\t.dirent = dirent\n\t};\n\n" + EBADF,
     "\t\t.ctx.actor = compat_fillonedir,\n\t\t.ctx.count = 1, /* Hint to fs: just one entry. */\n"
     "\t\t.dirent = dirent\n\t};\n\n" + EBADF + SET_SB)

# ---- compat getdents (the same lines as filldir's around the check, but no comment after them)
edit("fs/readdir.c",
     "struct compat_getdents_callback {\n\tstruct dir_context ctx;\n\tstruct compat_linux_dirent __user *current_dir;\n",
     "struct compat_getdents_callback {\n\tstruct dir_context ctx;\n\tstruct compat_linux_dirent __user *current_dir;\n" + SB_FIELDS)
edit("fs/readdir.c",
     "\tint reclen = ALIGN(offsetof(struct compat_linux_dirent, d_name) +\n"
     "\t\tnamlen + 2, sizeof(compat_long_t));\n\tint prev_reclen;\n\tunsigned int flags = d_type;\n",
     "\tint reclen = ALIGN(offsetof(struct compat_linux_dirent, d_name) +\n"
     "\t\tnamlen + 2, sizeof(compat_long_t));\n\tint prev_reclen;\n\tunsigned int flags = d_type;\n" + INODE_DECL)
edit("fs/readdir.c",
     "\tdirent = buf->current_dir;\n\tprev = (void __user *) dirent - prev_reclen;\n"
     "\tif (!user_write_access_begin(prev, reclen + prev_reclen))\n\t\tgoto efault;\n\n"
     "\tunsafe_put_user(offset, &prev->d_off, efault_end);\n",
     SKIP_SUS +
     "\tdirent = buf->current_dir;\n\tprev = (void __user *) dirent - prev_reclen;\n"
     "\tif (!user_write_access_begin(prev, reclen + prev_reclen))\n\t\tgoto efault;\n\n"
     "\tunsafe_put_user(offset, &prev->d_off, efault_end);\n")
edit("fs/readdir.c",
     "\t\t.ctx.actor = compat_filldir,\n\t\t.ctx.count = count,\n\t\t.ctx.dt_flags_mask = FILLDIR_FLAG_NOINTR,\n"
     "\t\t.current_dir = dirent,\n\t};\n\tint error;\n\n" + EBADF,
     "\t\t.ctx.actor = compat_filldir,\n\t\t.ctx.count = count,\n\t\t.ctx.dt_flags_mask = FILLDIR_FLAG_NOINTR,\n"
     "\t\t.current_dir = dirent,\n\t};\n\tint error;\n\n" + EBADF + SET_SB)
