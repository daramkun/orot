#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ─────────────────────────────────────────────────────────── */
typedef enum deflate_result {
    DEFLATE_OK            =  0,  /* success                                 */
    DEFLATE_STREAM_END    =  1,  /* end of stream reached                   */
    DEFLATE_NEED_INPUT    =  2,  /* need more input data                    */
    DEFLATE_NEED_OUTPUT   =  3,  /* need more output space                  */
    DEFLATE_DATA_ERROR    = -1,  /* invalid/corrupt input data              */
    DEFLATE_MEM_ERROR     = -2,  /* memory allocation failure               */
    DEFLATE_BUF_ERROR     = -3,  /* output buffer too small (whole-buffer)  */
    DEFLATE_VERSION_ERROR = -4,  /* version mismatch                        */
    DEFLATE_PARAM_ERROR   = -5,  /* invalid parameter                       */
} deflate_result;

/* ── Wire format ─────────────────────────────────────────────────────────── */
typedef enum deflate_format {
    DEFLATE_FORMAT_RAW  = 0,  /* RFC 1951: raw DEFLATE bitstream           */
    DEFLATE_FORMAT_ZLIB = 1,  /* RFC 1950: zlib framing (Adler-32)         */
    DEFLATE_FORMAT_GZIP = 2,  /* RFC 1952: gzip framing (CRC-32)           */
} deflate_format;

/* ── Compression levels ──────────────────────────────────────────────────── */
#define DEFLATE_LEVEL_STORE   0   /* store only, no compression             */
#define DEFLATE_LEVEL_FAST    1   /* fastest, lowest ratio                  */
#define DEFLATE_LEVEL_DEFAULT 6   /* good balance                           */
#define DEFLATE_LEVEL_BETTER  9   /* better ratio, slower                   */
#define DEFLATE_LEVEL_MAX    12   /* maximum ratio, slowest                 */

/* ── Flush modes ─────────────────────────────────────────────────────────── */
typedef enum deflate_flush {
    DEFLATE_NO_FLUSH   = 0,  /* accumulate data, no forced output          */
    DEFLATE_SYNC_FLUSH = 1,  /* flush to byte boundary (Z_SYNC_FLUSH)      */
    DEFLATE_FULL_FLUSH = 2,  /* flush + reset LZ77 dict (random access)    */
    DEFLATE_FINISH     = 3,  /* finalize stream                            */
} deflate_flush;

/* ── Custom allocator ────────────────────────────────────────────────────── */
typedef struct deflate_allocator {
    void* (*alloc)(void* opaque, size_t size, size_t alignment);
    void  (*free )(void* opaque, void*  ptr);
    void* opaque;
} deflate_allocator;

/* ── Version ─────────────────────────────────────────────────────────────── */
#define DEFLATE_VERSION_MAJOR 1
#define DEFLATE_VERSION_MINOR 0
#define DEFLATE_VERSION_PATCH 0

#define DEFLATE_VERSION \
    ((DEFLATE_VERSION_MAJOR << 16) | \
     (DEFLATE_VERSION_MINOR <<  8) | \
      DEFLATE_VERSION_PATCH)

#ifdef __cplusplus
} /* extern "C" */
#endif
