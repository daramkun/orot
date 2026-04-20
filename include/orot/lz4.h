#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * orot LZ4 API
 *
 * Two variants:
 *   orot_lz4_*  — raw LZ4 block (no frame header, matches lz4-block interop)
 *   orot_lz4f_* — LZ4 frame format (magic + header + blocks + checksum,
 *                 compatible with the lz4 CLI and liblz4 frame API)
 *
 * Return convention: non-negative = byte count written; negative = error.
 *   -1: general error (bad input, output too small)
 *   -2: output buffer too small
 *   -3: checksum mismatch (lz4f only)
 */

/* ── Raw LZ4 block ───────────────────────────────────────────────────────── */

/**
 * Conservative upper bound on compressed output for orot_lz4_compress.
 */
int orot_lz4_compress_bound(int src_size);

/**
 * Compress src_size bytes from src into dst using raw LZ4 block format.
 *
 * level: 1 (fastest) … 9 (best compression)
 * Returns bytes written to dst, or -1 on error (dst too small).
 */
int orot_lz4_compress(
    const void* src, int src_size,
    void*       dst, int dst_cap,
    int level);

/**
 * Decompress a raw LZ4 block from src into dst.
 *
 * src_size must be the exact compressed block size.
 * Returns bytes written to dst, or -1/-2 on error.
 */
int orot_lz4_decompress(
    const void* src, int src_size,
    void*       dst, int dst_cap);

/* ── LZ4 frame ───────────────────────────────────────────────────────────── */

/**
 * Conservative upper bound on compressed output for orot_lz4f_compress.
 */
int orot_lz4f_compress_bound(int src_size);

/**
 * Compress src_size bytes from src into a complete LZ4 frame.
 * Output is compatible with the lz4 CLI and liblz4 LZ4F_decompress.
 *
 * level: 1 (fastest) … 9 (best compression)
 * Returns bytes written to dst, or -1 on error.
 */
int orot_lz4f_compress(
    const void* src, int src_size,
    void*       dst, int dst_cap,
    int level);

/**
 * Decompress a complete LZ4 frame from src into dst.
 * Validates magic, header checksum, and content checksum.
 *
 * Returns bytes written to dst, or -1/-2/-3 on error.
 */
int orot_lz4f_decompress(
    const void* src, int src_size,
    void*       dst, int dst_cap);

#ifdef __cplusplus
} /* extern "C" */
#endif
