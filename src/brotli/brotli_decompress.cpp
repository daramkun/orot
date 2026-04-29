#include "brotli_decompress.hpp"
#include "brotli_bit.hpp"
#include "brotli_compress.hpp"

#include <cstring>

namespace orot { namespace brotli {

static int read_wbits(BitReader& br) noexcept {
    if (br.read_bits(1) == 0) return 16;
    uint32_t suffix = br.read_bits(3);
    if (br.error) return 0;
    switch (suffix) {
    case 0b001: {
        uint32_t top = br.read_bits(3);
        if (br.error) return 0;
        if (top == 0) return 17;
        if (top <= 6) return 9 + (int)top;
        return 0;
    }
    case 0b011: return 18;
    case 0b101: return 19;
    case 0b111: return 20;
    case 0b100: return 21;
    case 0b110: return 22;
    case 0b010: return 23;
    case 0b000: return 24;
    default: return 0;
    }
}

static int decode_mnibbles(uint32_t code) noexcept {
    switch (code) {
    case 0: return 4;
    case 1: return 5;
    case 2: return 6;
    default: return 0;
    }
}

BrotliDecodeStatus brotli_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    size_t* actual_out) noexcept
{
    if (actual_out) *actual_out = 0;

    BitReader br;
    br.init(src, src_len);

    int wbits = read_wbits(br);
    if (wbits < kLgWinMin || wbits > kLgWinMax || br.error)
        return BrotliDecodeStatus::DataError;

    size_t out_pos = 0;

    for (;;) {
        bool is_last = br.read_bits(1) != 0;
        if (br.error) return BrotliDecodeStatus::DataError;

        if (is_last) {
            bool is_last_empty = br.read_bits(1) != 0;
            if (br.error) return BrotliDecodeStatus::DataError;
            if (is_last_empty) {
                if (!br.remaining_zero()) return BrotliDecodeStatus::DataError;
                if (actual_out) *actual_out = out_pos;
                return BrotliDecodeStatus::Ok;
            }
        }

        int mnibbles = decode_mnibbles(br.read_bits(2));
        if (br.error) return BrotliDecodeStatus::DataError;

        if (mnibbles == 0) {
            if (br.read_bits(1) != 0) return BrotliDecodeStatus::DataError;
            uint32_t mskipbytes = br.read_bits(2);
            if (br.error) return BrotliDecodeStatus::DataError;
            size_t skip_len = 0;
            if (mskipbytes > 0) {
                uint32_t raw = br.read_bits((int)mskipbytes * 8);
                if (br.error) return BrotliDecodeStatus::DataError;
                if (mskipbytes > 1 && ((raw >> ((mskipbytes - 1) * 8)) & 0xFFu) == 0)
                    return BrotliDecodeStatus::DataError;
                skip_len = (size_t)raw + 1;
            }
            if (!br.align_zero()) return BrotliDecodeStatus::DataError;
            if (!br.consume_bytes(skip_len)) return BrotliDecodeStatus::DataError;
            continue;
        }

        uint32_t raw_len = br.read_bits(mnibbles * 4);
        if (br.error) return BrotliDecodeStatus::DataError;
        if (mnibbles > 4 && ((raw_len >> ((mnibbles - 1) * 4)) & 0xFu) == 0)
            return BrotliDecodeStatus::DataError;
        size_t meta_len = (size_t)raw_len + 1;

        if (!is_last) {
            bool is_uncompressed = br.read_bits(1) != 0;
            if (br.error) return BrotliDecodeStatus::DataError;
            if (is_uncompressed) {
                if (!br.align_zero()) return BrotliDecodeStatus::DataError;
                if (out_pos + meta_len > dst_cap) return BrotliDecodeStatus::NeedOutput;
                if ((size_t)(br.end - br.byte_ptr()) < meta_len)
                    return BrotliDecodeStatus::DataError;
                std::memcpy(dst + out_pos, br.byte_ptr(), meta_len);
                if (!br.consume_bytes(meta_len)) return BrotliDecodeStatus::DataError;
                out_pos += meta_len;
                continue;
            }
        }

        return BrotliDecodeStatus::Unsupported;
    }
}

} } /* namespace orot::brotli */
