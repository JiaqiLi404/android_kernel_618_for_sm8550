// SPDX-License-Identifier: GPL-2.0-only
/*
 * The kernel's LZ4 API (include/linux/lz4.h) on upstream LZ4 v1.10.1 (lz4_up.c, through lz4k.h),
 * with the exports lib/lz4 had before. Made by tools/kernel-patches/lz4_upgrade.py.
 */
#include <linux/build_bug.h>
#include <linux/lz4.h>
#include <linux/module.h>
#include "lz4k.h"

static_assert(LZ4_MEM_COMPRESS == LZ4K_STREAM_SIZE && sizeof(LZ4_stream_t) == LZ4K_STREAM_SIZE);
static_assert(LZ4HC_MEM_COMPRESS == LZ4K_STREAMHC_SIZE &&
	      sizeof(LZ4_streamHC_t) == LZ4K_STREAMHC_SIZE);
static_assert(sizeof(LZ4_streamDecode_t) == LZ4K_STREAMDECODE_SIZE);
static_assert(__alignof__(LZ4_stream_t) >= 8 && __alignof__(LZ4_streamHC_t) >= 8 &&
	      __alignof__(LZ4_streamDecode_t) >= 8);

int LZ4_compress_default(const char *source, char *dest, int inputSize, int maxOutputSize,
			 void *wrkmem)
{
	return lz4k_compress_fast(wrkmem, source, dest, inputSize, maxOutputSize,
				  LZ4_ACCELERATION_DEFAULT);
}
EXPORT_SYMBOL(LZ4_compress_default);

int LZ4_compress_fast(const char *source, char *dest, int inputSize, int maxOutputSize,
		      int acceleration, void *wrkmem)
{
	return lz4k_compress_fast(wrkmem, source, dest, inputSize, maxOutputSize, acceleration);
}
EXPORT_SYMBOL(LZ4_compress_fast);

int LZ4_compress_destSize(const char *source, char *dest, int *sourceSizePtr, int targetDestSize,
			  void *wrkmem)
{
	return lz4k_compress_destSize(wrkmem, source, dest, sourceSizePtr, targetDestSize);
}
EXPORT_SYMBOL(LZ4_compress_destSize);

int LZ4_decompress_fast(const char *source, char *dest, int originalSize)
{
	return lz4k_decompress_fast(source, dest, originalSize);
}
EXPORT_SYMBOL(LZ4_decompress_fast);

int LZ4_decompress_safe(const char *source, char *dest, int compressedSize,
			int maxDecompressedSize)
{
	return lz4k_decompress_safe(source, dest, compressedSize, maxDecompressedSize);
}
EXPORT_SYMBOL(LZ4_decompress_safe);

int LZ4_decompress_safe_partial(const char *source, char *dest, int compressedSize,
				int targetOutputSize, int maxDecompressedSize)
{
	return lz4k_decompress_safe_partial(source, dest, compressedSize, targetOutputSize,
					    maxDecompressedSize);
}
EXPORT_SYMBOL(LZ4_decompress_safe_partial);

int LZ4_compress_HC(const char *src, char *dst, int srcSize, int dstCapacity,
		    int compressionLevel, void *wrkmem)
{
	return lz4k_compress_HC(wrkmem, src, dst, srcSize, dstCapacity, compressionLevel);
}
EXPORT_SYMBOL(LZ4_compress_HC);

void LZ4_resetStreamHC(LZ4_streamHC_t *streamHCPtr, int compressionLevel)
{
	lz4k_resetStreamHC(streamHCPtr, compressionLevel);
}

int LZ4_loadDictHC(LZ4_streamHC_t *streamHCPtr, const char *dictionary, int dictSize)
{
	return lz4k_loadDictHC(streamHCPtr, dictionary, dictSize);
}
EXPORT_SYMBOL(LZ4_loadDictHC);

int LZ4_compress_HC_continue(LZ4_streamHC_t *streamHCPtr, const char *src, char *dst,
			     int srcSize, int maxDstSize)
{
	return lz4k_compress_HC_continue(streamHCPtr, src, dst, srcSize, maxDstSize);
}
EXPORT_SYMBOL(LZ4_compress_HC_continue);

int LZ4_saveDictHC(LZ4_streamHC_t *streamHCPtr, char *safeBuffer, int maxDictSize)
{
	return lz4k_saveDictHC(streamHCPtr, safeBuffer, maxDictSize);
}
EXPORT_SYMBOL(LZ4_saveDictHC);

void LZ4_resetStream(LZ4_stream_t *LZ4_stream)
{
	lz4k_resetStream(LZ4_stream);
}

int LZ4_loadDict(LZ4_stream_t *streamPtr, const char *dictionary, int dictSize)
{
	return lz4k_loadDict(streamPtr, dictionary, dictSize);
}
EXPORT_SYMBOL(LZ4_loadDict);

int LZ4_saveDict(LZ4_stream_t *streamPtr, char *safeBuffer, int dictSize)
{
	return lz4k_saveDict(streamPtr, safeBuffer, dictSize);
}
EXPORT_SYMBOL(LZ4_saveDict);

int LZ4_compress_fast_continue(LZ4_stream_t *streamPtr, const char *src, char *dst, int srcSize,
			       int maxDstSize, int acceleration)
{
	return lz4k_compress_fast_continue(streamPtr, src, dst, srcSize, maxDstSize, acceleration);
}
EXPORT_SYMBOL(LZ4_compress_fast_continue);

int LZ4_setStreamDecode(LZ4_streamDecode_t *LZ4_streamDecode, const char *dictionary,
			int dictSize)
{
	return lz4k_setStreamDecode(LZ4_streamDecode, dictionary, dictSize);
}
EXPORT_SYMBOL(LZ4_setStreamDecode);

int LZ4_decompress_safe_continue(LZ4_streamDecode_t *LZ4_streamDecode, const char *source,
				 char *dest, int compressedSize, int maxDecompressedSize)
{
	return lz4k_decompress_safe_continue(LZ4_streamDecode, source, dest, compressedSize,
					     maxDecompressedSize);
}
EXPORT_SYMBOL(LZ4_decompress_safe_continue);

int LZ4_decompress_fast_continue(LZ4_streamDecode_t *LZ4_streamDecode, const char *source,
				 char *dest, int originalSize)
{
	return lz4k_decompress_fast_continue(LZ4_streamDecode, source, dest, originalSize);
}
EXPORT_SYMBOL(LZ4_decompress_fast_continue);

int LZ4_decompress_safe_usingDict(const char *source, char *dest, int compressedSize,
				  int maxDecompressedSize, const char *dictStart, int dictSize)
{
	return lz4k_decompress_safe_usingDict(source, dest, compressedSize, maxDecompressedSize,
					      dictStart, dictSize);
}
EXPORT_SYMBOL(LZ4_decompress_safe_usingDict);

int LZ4_decompress_fast_usingDict(const char *source, char *dest, int originalSize,
				  const char *dictStart, int dictSize)
{
	return lz4k_decompress_fast_usingDict(source, dest, originalSize, dictStart, dictSize);
}
EXPORT_SYMBOL(LZ4_decompress_fast_usingDict);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("LZ4 compression and decompression (upstream LZ4 v1.10.1)");
