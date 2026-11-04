/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Between lz4_up.c (upstream LZ4, its API static there) and lz4_kernel.c (the kernel's LZ4 API,
 * include/linux/lz4.h): plain functions, and the sizes both sides check their types against.
 * Made by tools/kernel-patches/lz4_upgrade.py.
 */
#ifndef __LZ4K_H__
#define __LZ4K_H__

#include <linux/types.h>

/* upstream's LZ4_STREAM_MINSIZE (LZ4_MEMORY_USAGE 14), LZ4_STREAMHC_MINSIZE, LZ4_STREAMDECODE_MINSIZE */
#define LZ4K_STREAM_SIZE	16416
#define LZ4K_STREAMHC_SIZE	262200
#define LZ4K_STREAMDECODE_SIZE	32

int lz4k_compress_fast(void *state, const char *src, char *dst, int srcSize, int dstCapacity,
		       int acceleration);
int lz4k_compress_destSize(void *state, const char *src, char *dst, int *srcSizePtr,
			   int targetDstSize);
int lz4k_decompress_safe(const char *src, char *dst, int compressedSize, int dstCapacity);
int lz4k_decompress_safe_partial(const char *src, char *dst, int compressedSize,
				 int targetOutputSize, int dstCapacity);
int lz4k_decompress_fast(const char *src, char *dst, int originalSize);
int lz4k_compress_HC(void *state, const char *src, char *dst, int srcSize, int dstCapacity,
		     int compressionLevel);
void lz4k_resetStreamHC(void *stream, int compressionLevel);
int lz4k_loadDictHC(void *stream, const char *dictionary, int dictSize);
int lz4k_compress_HC_continue(void *stream, const char *src, char *dst, int srcSize,
			      int dstCapacity);
int lz4k_saveDictHC(void *stream, char *safeBuffer, int maxDictSize);
void lz4k_resetStream(void *stream);
int lz4k_loadDict(void *stream, const char *dictionary, int dictSize);
int lz4k_saveDict(void *stream, char *safeBuffer, int dictSize);
int lz4k_compress_fast_continue(void *stream, const char *src, char *dst, int srcSize,
				int dstCapacity, int acceleration);
int lz4k_setStreamDecode(void *stream, const char *dictionary, int dictSize);
int lz4k_decompress_safe_continue(void *stream, const char *src, char *dst, int compressedSize,
				  int dstCapacity);
int lz4k_decompress_fast_continue(void *stream, const char *src, char *dst, int originalSize);
int lz4k_decompress_safe_usingDict(const char *src, char *dst, int compressedSize, int dstCapacity,
				   const char *dictStart, int dictSize);
int lz4k_decompress_fast_usingDict(const char *src, char *dst, int originalSize,
				   const char *dictStart, int dictSize);

#endif
