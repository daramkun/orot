#include "brotli_compress.hpp"
#include "brotli_bit.hpp"

#include <algorithm>

namespace orot { namespace brotli {

static constexpr size_t kMaxMetaBlockSize = 1u << 24;

size_t brotli_compress_bound(size_t src_len) noexcept {
    size_t blocks = (src_len + kMaxMetaBlockSize - 1) / kMaxMetaBlockSize;
    return src_len + 16 + blocks * 8;
}

static bool write_wbits(BitWriter& bw, int lgwin) noexcept {
    switch (lgwin) {
    case 10: return bw.write_bits(0b0100001u, 7);
    case 11: return bw.write_bits(0b0110001u, 7);
    case 12: return bw.write_bits(0b1000001u, 7);
    case 13: return bw.write_bits(0b1010001u, 7);
    case 14: return bw.write_bits(0b1100001u, 7);
    case 15: return bw.write_bits(0b1110001u, 7);
    case 16: return bw.write_bits(0u, 1);
    case 17: return bw.write_bits(0b0000001u, 7);
    case 18: return bw.write_bits(0b0011u, 4);
    case 19: return bw.write_bits(0b0101u, 4);
    case 20: return bw.write_bits(0b0111u, 4);
    case 21: return bw.write_bits(0b1001u, 4);
    case 22: return bw.write_bits(0b1011u, 4);
    case 23: return bw.write_bits(0b1101u, 4);
    case 24: return bw.write_bits(0b1111u, 4);
    default: return false;
    }
}

static bool write_meta_len(BitWriter& bw, size_t len) noexcept {
    if (len == 0 || len > kMaxMetaBlockSize) return false;
    size_t encoded = len - 1;
    int mnibbles = (encoded <= 0xFFFFu) ? 4 :
                   (encoded <= 0xFFFFFu) ? 5 : 6;
    uint32_t mnibbles_code = (mnibbles == 4) ? 0u :
                             (mnibbles == 5) ? 1u : 2u;
    return bw.write_bits(mnibbles_code, 2)
        && bw.write_bits((uint32_t)encoded, mnibbles * 4);
}

size_t brotli_compress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    int quality, int lgwin) noexcept
{
    (void)quality;

    BitWriter bw;
    bw.init(dst, dst_cap);
    const uint8_t* input = src ? src : reinterpret_cast<const uint8_t*>("");

    if (!write_wbits(bw, lgwin))
        return 0;

    size_t pos = 0;
    while (pos < src_len) {
        size_t chunk = std::min(src_len - pos, kMaxMetaBlockSize);

        /* ISLAST=0, non-empty uncompressed meta-block. */
        if (!bw.write_bits(0, 1)) return 0;
        if (!write_meta_len(bw, chunk)) return 0;
        if (!bw.write_bits(1, 1)) return 0; /* ISUNCOMPRESSED */
        if (!bw.align_zero()) return 0;
        if (!bw.write_bytes(input + pos, chunk)) return 0;

        pos += chunk;
    }

    /* Final empty meta-block: ISLAST=1, ISLASTEMPTY=1. */
    if (!bw.write_bits(1, 1)) return 0;
    if (!bw.write_bits(1, 1)) return 0;
    if (!bw.finish_zero()) return 0;

    return bw.overflow ? 0 : bw.bytes_written(dst);
}

} } /* namespace orot::brotli */
