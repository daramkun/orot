#include "orot/bzip2.h"
#include "bzip2/bzip2_compress.hpp"
#include "bzip2/bzip2_decompress.hpp"

#include <climits>

using namespace orot::bzip2;

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

    size_t n = bzip2_decompress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap);

    if (n == 0) return -3;
    if (n > (size_t)INT_MAX) return -1;

    if (uncompressed_size_out) *uncompressed_size_out = n;
    return (int)n;
}
