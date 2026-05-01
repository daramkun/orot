#include "orot/brotli.h"
#include "brotli/brotli_compress.hpp"
#include "brotli/brotli_decompress.hpp"

#if defined(OROT_HAS_BROTLI_BACKEND)
#include <brotli/decode.h>
#include <brotli/encode.h>
#endif

#include <climits>

using namespace orot::brotli;

namespace {

#if defined(OROT_HAS_BROTLI_BACKEND)
static bool high_entropy_sample(const void* src, size_t src_size) noexcept {
    if (!src || src_size < 256 * 1024) return false;
    const auto* bytes = static_cast<const uint8_t*>(src);
    uint8_t seen[256] = {};
    const size_t sample = src_size < 4096 ? src_size : 4096;
    for (size_t i = 0; i < sample; ++i) seen[bytes[i]] = 1;
    int distinct = 0;
    for (int i = 0; i < 256; ++i) distinct += seen[i];
    return distinct >= 240;
}
#endif

} // namespace

size_t orot_brotli_compress_bound(size_t src_size) {
#if defined(OROT_HAS_BROTLI_BACKEND)
    const size_t n = BrotliEncoderMaxCompressedSize(src_size);
    const size_t fallback = brotli_compress_bound(src_size);
    if (n != 0) return n > fallback ? n : fallback;
#endif
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

#if defined(OROT_HAS_BROTLI_BACKEND)
    if (!high_entropy_sample(src, src_size)) {
        size_t encoded = dst_cap;
        if (BrotliEncoderCompress(
                quality, lgwin, BROTLI_MODE_GENERIC,
                src_size, static_cast<const uint8_t*>(src),
                &encoded, static_cast<uint8_t*>(dst)) &&
            encoded <= static_cast<size_t>(INT_MAX)) {
            return static_cast<int>(encoded);
        }
    }
#endif

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

    if (src_size * 100 > dst_cap * 95) {
        size_t fast_n = 0;
        BrotliDecodeStatus fast_status = brotli_decompress(
            static_cast<const uint8_t*>(src), src_size,
            static_cast<uint8_t*>(dst), dst_cap,
            &fast_n);
        if (fast_status == BrotliDecodeStatus::Ok) {
            if (fast_n > (size_t)INT_MAX) return -1;
            if (uncompressed_size_out) *uncompressed_size_out = fast_n;
            return (int)fast_n;
        }
    }

#if defined(OROT_HAS_BROTLI_BACKEND)
    size_t decoded = dst_cap;
    const BrotliDecoderResult br = BrotliDecoderDecompress(
        src_size, static_cast<const uint8_t*>(src),
        &decoded, static_cast<uint8_t*>(dst));
    if (br == BROTLI_DECODER_RESULT_SUCCESS) {
        if (decoded > static_cast<size_t>(INT_MAX)) return -1;
        if (uncompressed_size_out) *uncompressed_size_out = decoded;
        return static_cast<int>(decoded);
    }
    if (br == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) return -2;
#endif

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
