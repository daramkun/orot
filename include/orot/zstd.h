#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * orot Zstandard (zstd) API
 *
 * Whole-buffer Zstandard support. The compressor emits valid zstd frames using
 * raw blocks and whole-block RLE blocks; the decompressor handles raw/RLE blocks
 * and the implemented compressed-block decode path.
 *
 * Return convention: non-negative = byte count written; negative = error.
 *   -1: unsupported or invalid input
 *   -2: output buffer too small
 *   -3: checksum mismatch
 */

/**
 * Conservative upper bound on compressed output for orot_zstd_compress.
 */
int orot_zstd_compress_bound(int src_size);

/**
 * Compress src_size bytes from src into dst using Zstandard frame format.
 *
 * level: 1 (fastest) ... 9 (best compression).
 *
 * Returns bytes written to dst, or a negative error code.
 */
int orot_zstd_compress(
    const void* src, int src_size,
    void*       dst, int dst_cap,
    int         level);

/**
 * Decompress a complete Zstandard frame from src into dst.
 *
 * Returns bytes written to dst, or a negative error code.
 */
int orot_zstd_decompress(
    const void* src, int src_size,
    void*       dst, int dst_cap);

#ifdef __cplusplus
} /* extern "C" */
#endif
