#include "orot/brotli.h"
#include "brotli/brotli_compress.hpp"
#include "brotli/brotli_decompress.hpp"

#include <climits>

using namespace orot::brotli;

size_t orot_brotli_compress_bound(size_t src_size) {
    return brotli_compress_bound(src_size);
}

int orot_brotli_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         quality,
    int         lgwin)
{
    if (src_size > 0 && !src) return -1;
    if (!dst) return -1;
    if (quality < kQualityMin || quality > kQualityMax) return -1;
    if (lgwin < kLgWinMin || lgwin > kLgWinMax) return -1;

    const size_t need = brotli_compress_bound(src_size);
    if (dst_cap < need) return -2;

    size_t n = brotli_compress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        quality, lgwin);

    if (n == 0) return -1;
    if (n > (size_t)INT_MAX) return -1;
    return (int)n;
}

int orot_brotli_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    size_t*     uncompressed_size_out)
{
    if (!src) return -1;
    if (src_size == 0) return -3;
    if (!dst) return -1;

    size_t n = 0;
    BrotliDecodeStatus status = brotli_decompress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        &n);

    switch (status) {
    case BrotliDecodeStatus::Ok:
        break;
    case BrotliDecodeStatus::NeedOutput:
        return -2;
    case BrotliDecodeStatus::DataError:
        return -3;
    case BrotliDecodeStatus::Unsupported:
        return -4;
    }

    if (n > (size_t)INT_MAX) return -1;

    if (uncompressed_size_out) *uncompressed_size_out = n;
    return (int)n;
}
