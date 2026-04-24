#pragma once

/* Internal-only forward types for src/ implementation files.
 * External users include <orot/deflate.hpp> instead. */

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum deflate_result {
    DEFLATE_OK            =  0,
    DEFLATE_STREAM_END    =  1,
    DEFLATE_NEED_INPUT    =  2,
    DEFLATE_NEED_OUTPUT   =  3,
    DEFLATE_DATA_ERROR    = -1,
    DEFLATE_MEM_ERROR     = -2,
    DEFLATE_BUF_ERROR     = -3,
    DEFLATE_VERSION_ERROR = -4,
    DEFLATE_PARAM_ERROR   = -5,
} deflate_result;

typedef enum deflate_format {
    DEFLATE_FORMAT_RAW  = 0,
    DEFLATE_FORMAT_ZLIB = 1,
    DEFLATE_FORMAT_GZIP = 2,
} deflate_format;

#define DEFLATE_LEVEL_STORE   0
#define DEFLATE_LEVEL_FAST    1
#define DEFLATE_LEVEL_DEFAULT 6
#define DEFLATE_LEVEL_BETTER  9
#define DEFLATE_LEVEL_MAX    12

typedef enum deflate_flush {
    DEFLATE_NO_FLUSH   = 0,
    DEFLATE_SYNC_FLUSH = 1,
    DEFLATE_FULL_FLUSH = 2,
    DEFLATE_FINISH     = 3,
} deflate_flush;

typedef struct deflate_allocator {
    void* (*alloc)(void* opaque, size_t size, size_t alignment);
    void  (*free )(void* opaque, void*  ptr);
    void* opaque;
} deflate_allocator;

#ifdef __cplusplus
} /* extern "C" */
#endif
