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
 * Dictionary APIs accept raw content dictionaries. Dictionary IDs are carried
 * in the frame header when requested, but orot does not implement zstd trained
 * dictionary entropy tables.
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

/**
 * Compress using a raw content dictionary and optionally emit dict_id in the
 * frame header. Current encoder still uses raw/RLE blocks, so the dictionary is
 * used for frame metadata compatibility rather than entropy coding.
 */
int orot_zstd_compress_dict(
    const void* src, int src_size,
    const void* dict, int dict_size, unsigned dict_id,
    void*       dst, int dst_cap,
    int         level);

/**
 * Decompress a complete Zstandard frame using a raw content dictionary.
 * expected_dict_id may be 0 to accept any non-zero frame dictionary id.
 */
int orot_zstd_decompress_dict(
    const void* src, int src_size,
    const void* dict, int dict_size, unsigned expected_dict_id,
    void*       dst, int dst_cap);

typedef struct orot_zstd_cstream orot_zstd_cstream;
typedef struct orot_zstd_dstream orot_zstd_dstream;

orot_zstd_cstream* orot_zstd_compress_stream_new(int level);
void orot_zstd_compress_stream_free(orot_zstd_cstream* ctx);
int orot_zstd_compress_stream_set_dict(
    orot_zstd_cstream* ctx,
    const void* dict, int dict_size, unsigned dict_id);
int orot_zstd_compress_stream_update(
    orot_zstd_cstream* ctx,
    const void* src, int src_size);
int orot_zstd_compress_stream_finish(
    orot_zstd_cstream* ctx,
    void* dst, int dst_cap);

orot_zstd_dstream* orot_zstd_decompress_stream_new(void);
void orot_zstd_decompress_stream_free(orot_zstd_dstream* ctx);
int orot_zstd_decompress_stream_set_dict(
    orot_zstd_dstream* ctx,
    const void* dict, int dict_size, unsigned expected_dict_id);
int orot_zstd_decompress_stream_update(
    orot_zstd_dstream* ctx,
    const void* src, int src_size);
int orot_zstd_decompress_stream_finish(
    orot_zstd_dstream* ctx,
    void* dst, int dst_cap);

#ifdef __cplusplus
} /* extern "C" */
#endif
