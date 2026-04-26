#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * orot Zstandard (zstd) API
 *
 * Initial public API for whole-buffer Zstandard support. The entry points are
 * present so consumers can compile against the zstd surface while the frame,
 * block, entropy, and sequence decoders are implemented in later stages.
 *
 * Return convention: non-negative = byte count written; negative = error.
 *   -1: unsupported or invalid input
 *   -2: output buffer too small
 */

/**
 * Conservative upper bound on compressed output for orot_zstd_compress.
 */
int orot_zstd_compress_bound(int src_size);

/**
 * Compress src_size bytes from src into dst using Zstandard frame format.
 *
 * level: 1 (fastest) ... 9 (best compression). Level handling is reserved for
 * the compressor implementation stage.
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
