#include "orot/bzip2.h"
#include "bzip2/bzip2_compress.hpp"
#include "bzip2/bzip2_decompress.hpp"

#if defined(OROT_HAS_BZ2_BACKEND)
#include <bzlib.h>
#endif

#include <climits>
#include <cstdint>

using namespace orot::bzip2;

namespace {

#if defined(OROT_HAS_BZ2_BACKEND)
static int bz2_backend_compress(
    const void* src, size_t src_size,
    void* dst, size_t dst_cap,
    int level)
{
    if (src_size > static_cast<size_t>(UINT_MAX) || dst_cap > static_cast<size_t>(UINT_MAX))
        return -1;
    unsigned int out_len = static_cast<unsigned int>(dst_cap);
    const int r = BZ2_bzBuffToBuffCompress(
        static_cast<char*>(dst), &out_len,
        const_cast<char*>(static_cast<const char*>(src)),
        static_cast<unsigned int>(src_size),
        level, 0, 0);
    if (r == BZ_OK && out_len <= static_cast<unsigned int>(INT_MAX))
        return static_cast<int>(out_len);
    if (r == BZ_OUTBUFF_FULL) return -2;
    return -1;
}

static int bz2_backend_decompress(
    const void* src, size_t src_size,
    void* dst, size_t dst_cap,
    size_t* uncompressed_size_out)
{
    if (src_size > static_cast<size_t>(UINT_MAX) || dst_cap > static_cast<size_t>(UINT_MAX))
        return -1;
    unsigned int out_len = static_cast<unsigned int>(dst_cap);
    const int r = BZ2_bzBuffToBuffDecompress(
        static_cast<char*>(dst), &out_len,
        const_cast<char*>(static_cast<const char*>(src)),
        static_cast<unsigned int>(src_size),
        0, 0);
    if (r == BZ_OK && out_len <= static_cast<unsigned int>(INT_MAX)) {
        if (uncompressed_size_out) *uncompressed_size_out = out_len;
        return static_cast<int>(out_len);
    }
    if (r == BZ_OUTBUFF_FULL) return -2;
    return -3;
}
#endif

} // namespace

size_t orot_bzip2_compress_bound(size_t src_size) {
    return bzip2_compress_bound(src_size);
}

int orot_bzip2_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         level)
{
    if (src_size > 0 && !src) return -1;
    if (!dst) return -1;
    if (level < 1 || level > 9) return -1;

    size_t need = bzip2_compress_bound(src_size);
    if (dst_cap < need) return -2;

#if defined(OROT_HAS_BZ2_BACKEND)
    {
        const int n = bz2_backend_compress(src, src_size, dst, dst_cap, level);
        if (n > 0 || src_size == 0) return n;
    }
#endif

    size_t n = bzip2_compress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        level);

    if (n == 0) return -1;
    if (n > (size_t)INT_MAX) return -1;
    return (int)n;
}

int orot_bzip2_compress_parallel(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         level,
    int         n_threads)
{
    if (src_size > 0 && !src) return -1;
    if (!dst) return -1;
    if (level < 1 || level > 9) return -1;

    size_t need = bzip2_compress_bound(src_size);
    if (dst_cap < need) return -2;

    size_t n = bzip2_compress_parallel(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        level, n_threads);

    if (n == 0) return -1;
    if (n > (size_t)INT_MAX) return -1;
    return (int)n;
}

int orot_bzip2_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    size_t*     uncompressed_size_out)
{
    if (!src) return -1;
    if (src_size < 10) return -3;
    if (!dst) return -1;

#if defined(OROT_HAS_BZ2_BACKEND)
    if (dst_cap > 0 && src_size * 100 < dst_cap * 90) {
        return bz2_backend_decompress(src, src_size, dst, dst_cap, uncompressed_size_out);
    }
#endif

    size_t n = bzip2_decompress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap);

    if (n == 0) return -3;
    if (n > (size_t)INT_MAX) return -1;

    if (uncompressed_size_out) *uncompressed_size_out = n;
    return (int)n;
}
