#pragma once

#include "deflate_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version ─────────────────────────────────────────────────────────────── */
uint32_t deflate_version(void);
const char* deflate_version_string(void);

/* ── Custom allocator (global, set before first use) ─────────────────────── */
void deflate_set_allocator(const deflate_allocator* alloc);

/* =========================================================================
 * Whole-buffer API  (libdeflate-style: fast, no state, no streaming)
 * ========================================================================= */

/**
 * Returns upper bound on compressed output size (safe to allocate).
 */
size_t deflate_compress_bound(size_t in_size, deflate_format format);

/**
 * Compress entire buffer in one call.
 * Returns compressed size, or 0 if out_capacity < deflate_compress_bound().
 */
size_t deflate_compress(
    const void* in,  size_t in_size,
    void*       out, size_t out_capacity,
    int         level,
    deflate_format format
);

/**
 * Decompress entire buffer in one call.
 * actual_out_size set to decompressed size on DEFLATE_OK.
 */
deflate_result deflate_decompress(
    const void* in,  size_t in_size,
    void*       out, size_t out_capacity,
    size_t*     actual_out_size,
    deflate_format format
);

/* =========================================================================
 * Streaming API  (zlib-style: incremental, resumable)
 * ========================================================================= */

typedef struct deflate_stream deflate_stream;

/** Create streaming compressor. Returns NULL on allocation failure. */
deflate_stream* deflate_stream_new(int level, deflate_format format);
void            deflate_stream_free(deflate_stream* s);

/**
 * Feed input, consume output. Call repeatedly until all input consumed.
 * On DEFLATE_FINISH flush: returns DEFLATE_STREAM_END when done.
 * next_in / avail_in / next_out / avail_out updated in place.
 */
deflate_result deflate_stream_compress(
    deflate_stream*  s,
    const uint8_t**  next_in,  size_t* avail_in,
    uint8_t**        next_out, size_t* avail_out,
    deflate_flush    flush
);

/** Create streaming decompressor. Returns NULL on allocation failure. */
deflate_stream* inflate_stream_new(deflate_format format);
void            inflate_stream_free(deflate_stream* s);

deflate_result deflate_stream_decompress(
    deflate_stream*  s,
    const uint8_t**  next_in,  size_t* avail_in,
    uint8_t**        next_out, size_t* avail_out
);

/* =========================================================================
 * Parallel API  (pigz-style: multi-threaded whole-buffer compression)
 * ========================================================================= */

typedef struct deflate_parallel_ctx deflate_parallel_ctx;

/**
 * Create parallel compressor context.
 * num_threads: 0 = auto-detect (hardware_concurrency)
 * block_size:  0 = auto (128KB..1MB based on level)
 */
deflate_parallel_ctx* deflate_parallel_new(
    int    level,
    deflate_format format,
    int    num_threads,
    size_t block_size
);
void deflate_parallel_free(deflate_parallel_ctx* ctx);

/**
 * Compress entire buffer using multiple threads.
 * Returns compressed size, or 0 on failure.
 */
size_t deflate_parallel_compress(
    deflate_parallel_ctx* ctx,
    const void* in,  size_t in_size,
    void*       out, size_t out_capacity
);

/* =========================================================================
 * Checksum utilities
 * ========================================================================= */

uint32_t deflate_adler32(uint32_t initial, const void* data, size_t len);
uint32_t deflate_crc32  (uint32_t initial, const void* data, size_t len);

/* =========================================================================
 * zlib-compatible aliases  (only when DEFLATE_ZLIB_COMPAT defined)
 * ========================================================================= */

#ifdef DEFLATE_ZLIB_COMPAT

#define Z_OK            DEFLATE_OK
#define Z_STREAM_END    DEFLATE_STREAM_END
#define Z_DATA_ERROR    DEFLATE_DATA_ERROR
#define Z_MEM_ERROR     DEFLATE_MEM_ERROR
#define Z_BUF_ERROR     DEFLATE_BUF_ERROR
#define Z_NO_FLUSH      DEFLATE_NO_FLUSH
#define Z_SYNC_FLUSH    DEFLATE_SYNC_FLUSH
#define Z_FINISH        DEFLATE_FINISH
#define Z_BEST_SPEED    DEFLATE_LEVEL_FAST
#define Z_BEST_COMPRESSION DEFLATE_LEVEL_BETTER
#define Z_DEFAULT_COMPRESSION DEFLATE_LEVEL_DEFAULT

#endif /* DEFLATE_ZLIB_COMPAT */

#ifdef __cplusplus
} /* extern "C" */
#endif
