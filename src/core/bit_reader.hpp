#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace deflate {

/**
 * Bit-stream input using a 64-bit accumulator.
 *
 * Bits are consumed LSB-first (DEFLATE convention).
 * Refills 8 bytes at a time via memcpy.
 *
 * Designed for the decompressor state machine: tracks whether
 * the stream is near the end (< 8 bytes remaining) to avoid
 * unsafe bulk refills.
 */
class BitReader {
public:
    BitReader(const uint8_t* src, size_t size) noexcept
        : src_(src), end_(src + size), ptr_(src) {}

    /* ── Refill ──────────────────────────────────────────────────────────── */

    /**
     * Fast refill: load up to 8 bytes at once.
     * Only safe when at least 8 bytes remain in input.
     */
    void refill_fast() noexcept {
        assert(ptr_ + 8 <= end_);
        uint64_t word;
        std::memcpy(&word, ptr_, 8);
        bits_      |= word << bit_count_;
        const int bytes = (63 - bit_count_) >> 3;
        ptr_       += bytes;
        bit_count_ += bytes << 3;
    }

    /**
     * Safe refill: load one byte at a time until accumulator has >= 56 bits
     * or input is exhausted.
     */
    void refill_safe() noexcept {
        while (bit_count_ < 56 && ptr_ < end_) {
            bits_       |= static_cast<uint64_t>(*ptr_++) << bit_count_;
            bit_count_  += 8;
        }
    }

    /* ── Peek / consume ──────────────────────────────────────────────────── */

    /** Peek at the low `count` bits without consuming. count <= 32. */
    uint32_t peek_bits(int count) const noexcept {
        assert(count >= 0 && count <= 32);
        return static_cast<uint32_t>(bits_) & ((1U << count) - 1);
    }

    /** Consume `count` bits (must have been peeked first). */
    void consume_bits(int count) noexcept {
        assert(count >= 0 && count <= bit_count_);
        bits_      >>= count;
        bit_count_ -= count;
    }

    /** Read and consume `count` bits. */
    uint32_t read_bits(int count) noexcept {
        const uint32_t v = peek_bits(count);
        consume_bits(count);
        return v;
    }

    /** Read 1 bit. */
    int read_bit() noexcept { return static_cast<int>(read_bits(1)); }

    /* ── Byte-aligned I/O ────────────────────────────────────────────────── */

    /** Skip to next byte boundary. */
    void align_to_byte() noexcept {
        const int pad = bit_count_ & 7;
        if (pad) consume_bits(pad);
    }

    /** Read a little-endian 16-bit value (must be byte-aligned). */
    uint16_t read_u16_le() noexcept {
        assert(bit_count_ == 0 || (bit_count_ & 7) == 0);
        uint16_t v;
        std::memcpy(&v, ptr_, 2);
        ptr_       += 2;
        bit_count_  = 0;
        bits_       = 0;
        return v;
    }

    /* ── State ───────────────────────────────────────────────────────────── */

    int    bits_available()  const noexcept { return bit_count_; }
    size_t bytes_remaining() const noexcept {
        return static_cast<size_t>(end_ - ptr_) + (bit_count_ >> 3);
    }
    size_t bytes_consumed()  const noexcept {
        return static_cast<size_t>(ptr_ - src_) - (bit_count_ >> 3);
    }
    bool   is_exhausted()    const noexcept {
        return ptr_ >= end_ && bit_count_ == 0;
    }

    const uint8_t* current_ptr() const noexcept { return ptr_; }
    const uint8_t* end_ptr()     const noexcept { return end_; }

    /** True when >= 8 bytes of raw input remain (safe for refill_fast). */
    bool can_refill_fast() const noexcept { return ptr_ + 8 <= end_; }

private:
    const uint8_t* const src_;
    const uint8_t* const end_;
    const uint8_t*       ptr_;
    uint64_t             bits_      = 0;
    int                  bit_count_ = 0;
};

} /* namespace deflate */
