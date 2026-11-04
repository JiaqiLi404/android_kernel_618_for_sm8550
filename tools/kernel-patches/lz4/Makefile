# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -O3

# Upstream LZ4 v1.10.1 (tools/kernel-patches/lz4_upgrade.py): upstream/ compiled as one unit by
# lz4_up.c, the kernel's LZ4 API (include/linux/lz4.h) on top of it in lz4_kernel.c -- one object
# for whichever of the three options asks for it (this directory is entered only then).
# lz4_decompress.c and lz4defs.h are left for lib/decompress_unlz4.c's pre-boot build.
lz4_lib-y := lz4_kernel.o lz4_up.o
ifneq ($(filter y,$(CONFIG_LZ4_COMPRESS) $(CONFIG_LZ4HC_COMPRESS) $(CONFIG_LZ4_DECOMPRESS)),)
obj-y += lz4_lib.o
else
obj-m += lz4_lib.o
endif

# upstream's files as they are: C99, declarations after statements, the functions of its API
# that the kernel does not call (static there), <stddef.h> <stdint.h> <string.h> <limits.h> from
# shim/; on arm64 no 64-byte copies -- they write further past a copy than LZ4's own in-place
# margin leaves free, and EROFS decompresses in place with that margin
CFLAGS_lz4_up.o += -std=gnu11 -I$(srctree)/$(src)/shim -DLZ4_NO_WILDCOPY64 \
	-Wno-declaration-after-statement -Wno-unused-function
