#include "orot/deflate.h"
#include "formats/raw_deflate.hpp"
#include "formats/zlib_wrapper.hpp"
#include "formats/gzip_wrapper.hpp"
#include "simd/simd_dispatch.hpp"
#if defined(DEFLATE_THREADS_ENABLED)
#include "decompress/parallel_decompress.hpp"
#endif

#include <cstring>
#include <cstdlib>

namespace {

/* Default allocator (system malloc/free) */
deflate_allocator g_alloc = {
    [](void*, size_t size, size_t align) -> void* {
#if defined(_MSC_VER)
        return _aligned_malloc(size, align);
#else
        void* p = nullptr;
        ::posix_memalign(&p, align, size);
        return p;
#endif
    },
    [](void*, void* ptr) {
#if defined(_MSC_VER)
        _aligned_free(ptr);
#else
        ::free(ptr);
#endif
    },
    nullptr
};

} /* anonymous namespace */

extern "C" {

/* ── Allocator ───────────────────────────────────────────────────────────── */

void deflate_set_allocator(const deflate_allocator* alloc) {
    if (alloc) g_alloc = *alloc;
}

/* ── Whole-buffer API ────────────────────────────────────────────────────── */

size_t deflate_compress_bound(size_t in_size, deflate_format format) {
    size_t n = orot::deflate::raw_compress_bound(in_size);
    switch (format) {
    case DEFLATE_FORMAT_ZLIB: n += 6;  break;
    case DEFLATE_FORMAT_GZIP: n += 18; break;
    default: break;
    }
    return n;
}

size_t deflate_compress(
    const void* in,  size_t in_size,
    void*       out, size_t out_capacity,
    int         level,
    deflate_format format)
{
    const auto* src = static_cast<const uint8_t*>(in);
    auto*       dst = static_cast<uint8_t*>(out);

    switch (format) {
    case DEFLATE_FORMAT_RAW:
        return orot::deflate::raw_compress(src, in_size, dst, out_capacity, level);
    case DEFLATE_FORMAT_ZLIB:
        return orot::deflate::zlib_compress(src, in_size, dst, out_capacity, level);
    case DEFLATE_FORMAT_GZIP:
        return orot::deflate::gzip_compress(src, in_size, dst, out_capacity, level);
    }
    return 0;
}

deflate_result deflate_decompress(
    const void* in,  size_t in_size,
    void*       out, size_t out_capacity,
    size_t*     actual_out_size,
    deflate_format format)
{
    const auto* src = static_cast<const uint8_t*>(in);
    auto*       dst = static_cast<uint8_t*>(out);

    switch (format) {
    case DEFLATE_FORMAT_RAW:
        return orot::deflate::raw_decompress(src, in_size, dst, out_capacity, actual_out_size);
    case DEFLATE_FORMAT_ZLIB:
        return orot::deflate::zlib_decompress(src, in_size, dst, out_capacity, actual_out_size);
    case DEFLATE_FORMAT_GZIP: {
#if defined(DEFLATE_THREADS_ENABLED)
        static constexpr size_t PARALLEL_GZIP_THRESHOLD = 256 * 1024;
        if (in_size >= PARALLEL_GZIP_THRESHOLD) {
            const deflate_result pr = orot::deflate::parallel_gzip_decompress(
                src, in_size, dst, out_capacity, actual_out_size);
            if (pr != DEFLATE_DATA_ERROR) return pr;
        }
#endif
        return orot::deflate::gzip_decompress(src, in_size, dst, out_capacity, actual_out_size);
    }}
    return DEFLATE_PARAM_ERROR;
}

/* ── Checksum utilities ──────────────────────────────────────────────────── */

uint32_t deflate_adler32(uint32_t initial, const void* data, size_t len) {
    return orot::deflate::simd_adler32_fn()(initial,
        static_cast<const uint8_t*>(data), len);
}

uint32_t deflate_crc32(uint32_t initial, const void* data, size_t len) {
    return orot::deflate::simd_crc32_fn()(initial,
        static_cast<const uint8_t*>(data), len);
}

} /* extern "C" */
