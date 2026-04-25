#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace orot { namespace lzma {

/* ── Probability type ────────────────────────────────────────────────────── */

using Prob = uint16_t;

static constexpr int    kNumBitModelTotalBits = 11;
static constexpr uint32_t kBitModelTotal      = 1u << kNumBitModelTotalBits;  // 2048
static constexpr int    kNumMoveBits          = 5;
static constexpr Prob   kProbInit             = kBitModelTotal >> 1;           // 1024

inline void prob_init(Prob* probs, size_t count) noexcept {
    for (size_t i = 0; i < count; ++i)
        probs[i] = kProbInit;
}

/* ── Range Encoder ───────────────────────────────────────────────────────── */

struct RangeEncoder {
    uint64_t low        = 0;
    uint32_t range      = 0xFFFFFFFFu;
    uint8_t  cache      = 0;
    int64_t  cache_size = 1;
    uint8_t* out        = nullptr;
    uint8_t* out_end    = nullptr;
    bool     overflow   = false;

    void reset(uint8_t* dst, size_t cap) noexcept {
        out       = dst;
        out_end   = dst + cap;
        low       = 0;
        range     = 0xFFFFFFFFu;
        cache     = 0;
        cache_size = 1;
        overflow  = false;
    }

    void shift_low() noexcept {
        if ((uint32_t)low < (uint32_t)0xFF000000u || (low >> 32) != 0) {
            uint8_t carry = (uint8_t)(low >> 32);
            uint8_t tmp   = cache;
            int64_t n     = cache_size;
            /* Reset before loop: new pending byte will be set below */
            cache_size = 1;
            while (n-- > 0) {
                if (out < out_end)
                    *out++ = (uint8_t)(tmp + carry);
                else
                    overflow = true;
                tmp = 0xFF;
            }
            cache = (uint8_t)((uint32_t)low >> 24);
        } else {
            cache_size++;
        }
        low = (uint32_t)(low << 8);
    }

    void normalize() noexcept {
        if (range < (1u << 24)) {
            range <<= 8;
            shift_low();  /* shift_low handles low internally */
        }
    }

    void encode_bit(Prob* prob, int bit) noexcept {
        uint32_t bound = (range >> kNumBitModelTotalBits) * (uint32_t)(*prob);
        if (bit == 0) {
            range  = bound;
            *prob += (kBitModelTotal - *prob) >> kNumMoveBits;
        } else {
            low   += bound;
            range -= bound;
            *prob -= *prob >> kNumMoveBits;
        }
        normalize();
    }

    void encode_direct_bits(uint32_t value, int count) noexcept {
        while (count-- > 0) {
            range >>= 1;
            if ((value >> count) & 1)
                low += range;
            if (range < (1u << 24)) {
                range <<= 8;
                shift_low();
            }
        }
    }

    /* Bit tree: MSB first, symbol in [0, (1<<num_bits)) */
    void encode_bit_tree(Prob* probs, int num_bits, uint32_t symbol) noexcept {
        uint32_t m = 1;
        for (int i = num_bits - 1; i >= 0; --i) {
            int bit = (symbol >> i) & 1;
            encode_bit(&probs[m], bit);
            m = (m << 1) | bit;
        }
    }

    /* Reversed bit tree: LSB first (used for distance extra bits) */
    void encode_bit_tree_reverse(Prob* probs, int num_bits, uint32_t symbol) noexcept {
        uint32_t m = 1;
        for (int i = 0; i < num_bits; ++i) {
            int bit = symbol & 1;
            symbol >>= 1;
            encode_bit(&probs[m], bit);
            m = (m << 1) | bit;
        }
    }

    bool flush() noexcept {
        for (int i = 0; i < 5; ++i) {
            range <<= 8;
            shift_low();
        }
        return !overflow;
    }

    size_t bytes_written(const uint8_t* dst_start) const noexcept {
        return (size_t)(out - dst_start);
    }
};

/* ── Range Decoder ───────────────────────────────────────────────────────── */

struct RangeDecoder {
    uint32_t       range     = 0xFFFFFFFFu;
    uint32_t       code      = 0;
    const uint8_t* in        = nullptr;
    const uint8_t* in_end    = nullptr;
    bool           corrupted = false;

    bool init(const uint8_t* src, size_t size) noexcept {
        in        = src;
        in_end    = src + size;
        corrupted = false;
        if (size < 5 || *in++ != 0x00) {
            corrupted = true;
            return false;
        }
        code = 0;
        for (int i = 0; i < 4; ++i)
            code = (code << 8) | *in++;
        range = 0xFFFFFFFFu;
        if (code == range) {
            corrupted = true;
            return false;
        }
        return true;
    }

    uint8_t read_byte() noexcept {
        if (in < in_end)
            return *in++;
        corrupted = true;
        return 0;
    }

    void normalize() noexcept {
        if (range < (1u << 24)) {
            range <<= 8;
            code = (code << 8) | read_byte();
        }
    }

    int decode_bit(Prob* prob) noexcept {
        uint32_t bound = (range >> kNumBitModelTotalBits) * (uint32_t)(*prob);
        if (code < bound) {
            range  = bound;
            *prob += (kBitModelTotal - *prob) >> kNumMoveBits;
            normalize();
            return 0;
        } else {
            code  -= bound;
            range -= bound;
            *prob -= *prob >> kNumMoveBits;
            normalize();
            return 1;
        }
    }

    uint32_t decode_direct_bits(int count) noexcept {
        uint32_t result = 0;
        while (count-- > 0) {
            range >>= 1;
            result <<= 1;
            if (code >= range) {
                code -= range;
                result |= 1;
            }
            normalize();
        }
        return result;
    }

    uint32_t decode_bit_tree(Prob* probs, int num_bits) noexcept {
        uint32_t m = 1;
        for (int i = 0; i < num_bits; ++i)
            m = (m << 1) | decode_bit(&probs[m]);
        return m - (1u << num_bits);
    }

    uint32_t decode_bit_tree_reverse(Prob* probs, int num_bits) noexcept {
        uint32_t m = 1, symbol = 0;
        for (int i = 0; i < num_bits; ++i) {
            int bit = decode_bit(&probs[m]);
            m = (m << 1) | bit;
            symbol |= ((uint32_t)bit << i);
        }
        return symbol;
    }

    bool is_finished_ok() const noexcept {
        return code == 0 && !corrupted;
    }
};

} } /* namespace orot::lzma */
