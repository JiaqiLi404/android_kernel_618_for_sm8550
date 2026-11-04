// SPDX-License-Identifier: GPL-2.0-only
/*
 * Upstream LZ4 v1.10.1 -- lib/lz4.c and lib/lz4hc.c of github.com/lz4/lz4, in upstream/ as they
 * are but for the switch for the 64-byte copies (LZ4_NO_WILDCOPY64, set by the Makefile) --
 * compiled as one unit with every function of upstream's API static, and the plain functions
 * lz4_kernel.c builds the kernel's LZ4 API from (lz4k.h).
 * Made by tools/kernel-patches/lz4_upgrade.py.
 */
#include <linux/slab.h>
#include "lz4k.h"

/* names of the kernel's headers that upstream has its own of: the current task, MIN and MAX */
#undef current
#undef MIN
#undef MAX

/* upstream's API, static in this unit (the "static linking only" part as well) */
#define LZ4LIB_VISIBILITY static
#define LZ4_PUBLISH_STATIC_FUNCTIONS

/*
 * Memory: upstream allocates only in functions not used here and for LZ4HC levels 10 to 12
 * (64 KB of scratch for its optimal parser), never on the stack (the heap modes) -- and never
 * sleeps for it here: a compression that does not get its memory fails, and its caller keeps
 * the data as it is.
 */
#define LZ4_HEAPMODE 1
#define LZ4HC_HEAPMODE 1
#define LZ4_USER_MEMORY_FUNCTIONS
static void *LZ4_malloc(size_t s)
{
	return kmalloc(s, GFP_NOWAIT | __GFP_NOWARN);
}

static void *LZ4_calloc(size_t n, size_t s)
{
	return kcalloc(n, s, GFP_NOWAIT | __GFP_NOWARN);
}

static void LZ4_free(void *p)
{
	kfree(p);
}

#include "upstream/lz4.c"
#include "upstream/lz4hc.c"

_Static_assert(sizeof(LZ4_stream_t) == LZ4K_STREAM_SIZE, "LZ4_stream_t");
_Static_assert(sizeof(LZ4_streamHC_t) == LZ4K_STREAMHC_SIZE, "LZ4_streamHC_t");
_Static_assert(sizeof(LZ4_streamDecode_t) == LZ4K_STREAMDECODE_SIZE, "LZ4_streamDecode_t");
_Static_assert(_Alignof(LZ4_stream_t) <= 8 && _Alignof(LZ4_streamHC_t) <= 8 &&
	       _Alignof(LZ4_streamDecode_t) <= 8, "the kernel's stream types are 8-byte aligned");

int lz4k_compress_fast(void *state, const char *src, char *dst, int srcSize, int dstCapacity,
		       int acceleration)
{
	return LZ4_compress_fast_extState(state, src, dst, srcSize, dstCapacity, acceleration);
}

int lz4k_compress_destSize(void *state, const char *src, char *dst, int *srcSizePtr,
			   int targetDstSize)
{
	return LZ4_compress_destSize_extState(state, src, dst, srcSizePtr, targetDstSize, 1);
}

int lz4k_decompress_safe(const char *src, char *dst, int compressedSize, int dstCapacity)
{
	return LZ4_decompress_safe(src, dst, compressedSize, dstCapacity);
}

int lz4k_decompress_safe_partial(const char *src, char *dst, int compressedSize,
				 int targetOutputSize, int dstCapacity)
{
	return LZ4_decompress_safe_partial(src, dst, compressedSize, targetOutputSize, dstCapacity);
}

int lz4k_decompress_fast(const char *src, char *dst, int originalSize)
{
	return LZ4_decompress_fast(src, dst, originalSize);
}

int lz4k_compress_HC(void *state, const char *src, char *dst, int srcSize, int dstCapacity,
		     int compressionLevel)
{
	return LZ4_compress_HC_extStateHC(state, src, dst, srcSize, dstCapacity, compressionLevel);
}

void lz4k_resetStreamHC(void *stream, int compressionLevel)
{
	LZ4_resetStreamHC(stream, compressionLevel);
}

int lz4k_loadDictHC(void *stream, const char *dictionary, int dictSize)
{
	return LZ4_loadDictHC(stream, dictionary, dictSize);
}

int lz4k_compress_HC_continue(void *stream, const char *src, char *dst, int srcSize,
			      int dstCapacity)
{
	return LZ4_compress_HC_continue(stream, src, dst, srcSize, dstCapacity);
}

int lz4k_saveDictHC(void *stream, char *safeBuffer, int maxDictSize)
{
	return LZ4_saveDictHC(stream, safeBuffer, maxDictSize);
}

void lz4k_resetStream(void *stream)
{
	LZ4_resetStream(stream);
}

int lz4k_loadDict(void *stream, const char *dictionary, int dictSize)
{
	return LZ4_loadDict(stream, dictionary, dictSize);
}

int lz4k_saveDict(void *stream, char *safeBuffer, int dictSize)
{
	return LZ4_saveDict(stream, safeBuffer, dictSize);
}

int lz4k_compress_fast_continue(void *stream, const char *src, char *dst, int srcSize,
				int dstCapacity, int acceleration)
{
	return LZ4_compress_fast_continue(stream, src, dst, srcSize, dstCapacity, acceleration);
}

int lz4k_setStreamDecode(void *stream, const char *dictionary, int dictSize)
{
	return LZ4_setStreamDecode(stream, dictionary, dictSize);
}

int lz4k_decompress_safe_continue(void *stream, const char *src, char *dst, int compressedSize,
				  int dstCapacity)
{
	return LZ4_decompress_safe_continue(stream, src, dst, compressedSize, dstCapacity);
}

int lz4k_decompress_fast_continue(void *stream, const char *src, char *dst, int originalSize)
{
	return LZ4_decompress_fast_continue(stream, src, dst, originalSize);
}

int lz4k_decompress_safe_usingDict(const char *src, char *dst, int compressedSize, int dstCapacity,
				   const char *dictStart, int dictSize)
{
	return LZ4_decompress_safe_usingDict(src, dst, compressedSize, dstCapacity, dictStart,
					     dictSize);
}

int lz4k_decompress_fast_usingDict(const char *src, char *dst, int originalSize,
				   const char *dictStart, int dictSize)
{
	return LZ4_decompress_fast_usingDict(src, dst, originalSize, dictStart, dictSize);
}
