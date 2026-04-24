#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * orot LZW API
 *
 * Variable-width Lempel-Ziv-Welch compression (9 to max_bits code width).
 * The compressed stream includes a 1-byte header storing max_bits so that
 * orot_lzw_decompress requires no additional parameters.
 *
 * Return convention: non-negative = byte count written; negative = error.
 *   -1: general error (bad input, output buffer too small, invalid stream)
 *   -2: output buffer too small
 */

/**
 * Conservative upper bound on compressed output for orot_lzw_compress
 * when using the default max_bits (12).
 */
int orot_lzw_compress_bound(int src_size);

/**
 * Compress src_size bytes from src into dst using LZW with variable-width codes.
 *
 * max_bits: maximum code width, 9..16. Pass 0 to use the default (12).
 *   Higher values allow a larger dictionary and better compression at a
 *   memory cost of (1 << max_bits) * 6 bytes during decompression.
 *
 * Returns bytes written to dst, or -1 on error.
 */
int orot_lzw_compress(
    const void* src, int src_size,
    void*       dst, int dst_cap,
    int         max_bits);

/**
 * Decompress LZW-compressed data from src into dst.
 * max_bits is read from the embedded 1-byte header in src.
 *
 * Returns bytes written to dst, or -1/-2 on error.
 */
int orot_lzw_decompress(
    const void* src, int src_size,
    void*       dst, int dst_cap);

#ifdef __cplusplus
} /* extern "C" */
#endif
