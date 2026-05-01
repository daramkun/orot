#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace orot { namespace brotli {

struct BitReader {
    const uint8_t* src = nullptr;
    const uint8_t* end = nullptr;
    uint64_t bits = 0;
    int bit_count = 0;
    bool error = false;

    void init(const uint8_t* data, size_t size) noexcept {
        src = data;
        end = data + size;
        bits = 0;
        bit_count = 0;
        error = false;
    }

    bool fill(int need) noexcept {
        while (bit_count < need && src < end) {
            bits |= (uint64_t)(*src++) << bit_count;
            bit_count += 8;
        }
        if (bit_count < need) {
            error = true;
            return false;
        }
        return true;
    }

    uint32_t read_bits(int n) noexcept {
        if (n == 0) return 0;
        if (!fill(n)) return 0;
        uint32_t v = (uint32_t)(bits & ((1ull << n) - 1));
        bits >>= n;
        bit_count -= n;
        return v;
    }

    bool align_zero() noexcept {
        int skip = bit_count & 7;
        if (skip == 0) return true;
        uint32_t v = read_bits(skip);
        return !error && v == 0;
    }

    bool remaining_zero() noexcept {
        if ((bits & ((bit_count == 64) ? ~0ull : ((1ull << bit_count) - 1))) != 0)
            return false;
        while (src < end) {
            if (*src++ != 0) return false;
        }
        return true;
    }

    const uint8_t* byte_ptr() const noexcept {
        return src;
    }

    bool consume_bytes(size_t n) noexcept {
        if ((bit_count & 7) != 0) {
            error = true;
            return false;
        }
        if ((size_t)(end - src) < n) {
            error = true;
            return false;
        }
        src += n;
        return true;
    }
};

struct BitWriter {
    uint8_t* dst = nullptr;
    uint8_t* end = nullptr;
    uint64_t bits = 0;
    int bit_count = 0;
    bool overflow = false;

    void init(uint8_t* data, size_t cap) noexcept {
        dst = data;
        end = data + cap;
        bits = 0;
        bit_count = 0;
        overflow = false;
    }

    bool write_bits(uint32_t value, int n) noexcept {
        if (n == 0) return true;
        bits |= (uint64_t)(value & ((1u << n) - 1)) << bit_count;
        bit_count += n;
        while (bit_count >= 8) {
            if (dst >= end) {
                overflow = true;
                return false;
            }
            *dst++ = (uint8_t)(bits & 0xFFu);
            bits >>= 8;
            bit_count -= 8;
        }
        return true;
    }

    bool align_zero() noexcept {
        int pad = (8 - (bit_count & 7)) & 7;
        return write_bits(0, pad);
    }

    bool write_bytes(const uint8_t* src, size_t n) noexcept {
        if ((bit_count & 7) != 0 && !align_zero())
            return false;
        if ((size_t)(end - dst) < n) {
            overflow = true;
            return false;
        }
        std::memcpy(dst, src, n);
        dst += n;
        return true;
    }

    bool finish_zero() noexcept {
        return align_zero();
    }

    size_t bytes_written(const uint8_t* start) const noexcept {
        return (size_t)(dst - start);
    }
};

} } /* namespace orot::brotli */
