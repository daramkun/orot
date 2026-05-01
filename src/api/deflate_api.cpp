#include "../deflate_fwd.hpp"
#include "formats/raw_deflate.hpp"
#include "formats/zlib_wrapper.hpp"
#include "formats/gzip_wrapper.hpp"
#include "simd/simd_dispatch.hpp"
#if defined(DEFLATE_THREADS_ENABLED)
#include "decompress/parallel_decompress.hpp"
#endif
#if defined(OROT_HAS_LIBDEFLATE_BACKEND)
#include <libdeflate.h>
#endif
#if defined(OROT_HAS_ZLIB_BACKEND)
#include <zlib.h>
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

#if defined(OROT_HAS_LIBDEFLATE_BACKEND)
static libdeflate_compressor* backend_compressor(int level) noexcept {
    int lv = level;
    if (lv < 0) lv = 0;
    if (lv > 12) lv = 12;
    thread_local libdeflate_compressor* compressors[13] = {};
    if (!compressors[lv])
        compressors[lv] = libdeflate_alloc_compressor(lv);
    return compressors[lv];
}

static libdeflate_decompressor* backend_decompressor() noexcept {
    thread_local libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
    return decompressor;
}

static deflate_result map_backend_result(libdeflate_result result) noexcept {
    switch (result) {
    case LIBDEFLATE_SUCCESS: return DEFLATE_OK;
    case LIBDEFLATE_BAD_DATA: return DEFLATE_DATA_ERROR;
    case LIBDEFLATE_SHORT_OUTPUT: return DEFLATE_NEED_OUTPUT;
    case LIBDEFLATE_INSUFFICIENT_SPACE: return DEFLATE_NEED_OUTPUT;
    }
    return DEFLATE_DATA_ERROR;
}
#endif

} /* anonymous namespace */

extern "C" {

/* ── Allocator ───────────────────────────────────────────────────────────── */

void deflate_set_allocator(const deflate_allocator* alloc) {
    if (alloc) g_alloc = *alloc;
}

/* ── Whole-buffer API ────────────────────────────────────────────────────── */

size_t deflate_compress_bound(size_t in_size, deflate_format format) {
#if defined(OROT_HAS_LIBDEFLATE_BACKEND)
    if (auto* c = backend_compressor(6)) {
        switch (format) {
        case DEFLATE_FORMAT_RAW:  return libdeflate_deflate_compress_bound(c, in_size);
        case DEFLATE_FORMAT_ZLIB: return libdeflate_zlib_compress_bound(c, in_size);
        case DEFLATE_FORMAT_GZIP: return libdeflate_gzip_compress_bound(c, in_size);
        }
    }
#endif
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

#if defined(OROT_HAS_LIBDEFLATE_BACKEND)
    if (auto* c = backend_compressor(level)) {
        size_t n = 0;
        switch (format) {
        case DEFLATE_FORMAT_RAW:
            n = libdeflate_deflate_compress(c, src, in_size, dst, out_capacity);
            break;
        case DEFLATE_FORMAT_ZLIB:
            n = libdeflate_zlib_compress(c, src, in_size, dst, out_capacity);
            break;
        case DEFLATE_FORMAT_GZIP:
            n = libdeflate_gzip_compress(c, src, in_size, dst, out_capacity);
            break;
        }
        if (n != 0) return n;
    }
#endif

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

#if defined(OROT_HAS_ZLIB_BACKEND)
    if (format == DEFLATE_FORMAT_ZLIB &&
        in_size >= 2 &&
        out_capacity > 256 * 1024 &&
        out_capacity <= 640 * 1024 &&
        (src[1] >> 6) == 0) {
        uLongf actual = static_cast<uLongf>(out_capacity);
        if (static_cast<size_t>(actual) == out_capacity) {
            const int zr = uncompress(dst, &actual, src, static_cast<uLong>(in_size));
            if (zr == Z_OK) {
                if (actual_out_size) *actual_out_size = static_cast<size_t>(actual);
                return DEFLATE_OK;
            }
            if (zr == Z_BUF_ERROR) return DEFLATE_NEED_OUTPUT;
            if (zr == Z_MEM_ERROR) return DEFLATE_MEM_ERROR;
        }
    }
#endif

#if defined(OROT_HAS_LIBDEFLATE_BACKEND)
    if (auto* d = backend_decompressor()) {
        size_t actual = 0;
        libdeflate_result r = LIBDEFLATE_BAD_DATA;
        switch (format) {
        case DEFLATE_FORMAT_RAW:
            r = libdeflate_deflate_decompress(d, src, in_size, dst, out_capacity, &actual);
            break;
        case DEFLATE_FORMAT_ZLIB:
            r = libdeflate_zlib_decompress(d, src, in_size, dst, out_capacity, &actual);
            break;
        case DEFLATE_FORMAT_GZIP:
            r = libdeflate_gzip_decompress(d, src, in_size, dst, out_capacity, &actual);
            break;
        }
        if (r == LIBDEFLATE_SUCCESS && actual_out_size)
            *actual_out_size = actual;
        return map_backend_result(r);
    }
#endif

#if defined(OROT_HAS_ZLIB_BACKEND)
    if (format == DEFLATE_FORMAT_ZLIB) {
        uLongf actual = static_cast<uLongf>(out_capacity);
        if (static_cast<size_t>(actual) == out_capacity) {
            const int zr = uncompress(dst, &actual, src, static_cast<uLong>(in_size));
            if (zr == Z_OK) {
                if (actual_out_size) *actual_out_size = static_cast<size_t>(actual);
                return DEFLATE_OK;
            }
            if (zr == Z_BUF_ERROR) return DEFLATE_NEED_OUTPUT;
            if (zr == Z_MEM_ERROR) return DEFLATE_MEM_ERROR;
        }
    }
#endif

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
