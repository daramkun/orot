#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace orot { namespace deflate {

/**
 * Bit-stream output using a 64-bit accumulator.
 *
 * Bits are packed LSB-first (DEFLATE convention).
 * Flush 8 bytes at a time via memcpy — avoids alignment faults.
 *
 * Caller is responsible for ensuring sufficient space.
 * Use pending_bytes() to check how much has been written.
 */
class BitWriter {
public:
    BitWriter(uint8_t* dst, size_t capacity) noexcept
        : dst_(dst), cap_(capacity), ptr_(dst) {}

    /* ── Bit output ──────────────────────────────────────────────────────── */

    /** Write the low `count` bits of `value`. count must be 1..32. */
    void write_bits(uint32_t value, int count) noexcept {
        assert(count > 0 && count <= 32);
        assert(bit_count_ + count <= 64);
        bits_      |= (static_cast<uint64_t>(value) & ((1ULL << count) - 1)) << bit_count_;
        bit_count_ += count;
        /* Flush exactly 4 bytes when the accumulator has at least 32 bits.
         * Fixed-size store is cheaper than the variable-length flush. */
        if (bit_count_ >= 32) {
            assert(ptr_ + 4 <= dst_ + cap_);
            std::memcpy(ptr_, &bits_, 4);
            ptr_       += 4;
            bits_      >>= 32;
            bit_count_ -= 32;
        }
    }

    /** Write a single bit (0 or 1). */
    void write_bit(int b) noexcept { write_bits(static_cast<uint32_t>(b), 1); }

    /** Write a full byte. */
    void write_byte(uint8_t b) noexcept { write_bits(b, 8); }

    /* ── Alignment ───────────────────────────────────────────────────────── */

    /** Pad to next byte boundary with zero bits. */
    void align_to_byte() noexcept {
        if (bit_count_ & 7) {
            const int pad = 8 - (bit_count_ & 7);
            write_bits(0, pad);
        }
    }

    /** Flush all remaining pending bits (zero-padded to byte). */
    void flush() noexcept {
        align_to_byte();
        flush_full_bytes();
        assert(bit_count_ == 0);
    }

    /* ── Large block output (bypasses bit accumulator) ───────────────────── */

    /** Write raw bytes (must be byte-aligned first). */
    void write_bytes(const uint8_t* src, size_t len) noexcept {
        if (len == 0) return;
        assert((bit_count_ & 7) == 0 && "must be byte-aligned before write_bytes");
        flush_full_bytes();  /* drain any buffered complete bytes first */
        assert(bit_count_ == 0);
        assert(ptr_ + len <= dst_ + cap_);
        std::memcpy(ptr_, src, len);
        ptr_ += len;
    }

    /* ── Introspection ───────────────────────────────────────────────────── */

    /** Total bytes committed to output buffer so far (excludes pending bits). */
    size_t bytes_written()  const noexcept { return static_cast<size_t>(ptr_ - dst_); }
    /** Pending bits not yet flushed (0-7). */
    int    pending_bits()   const noexcept { return bit_count_;  }
    /** Total bytes including partially-filled byte. */
    size_t total_bytes()    const noexcept {
        return bytes_written() + (bit_count_ > 0 ? 1 : 0);
    }

    uint8_t* current_ptr() const noexcept { return ptr_; }
    size_t   remaining()   const noexcept {
        return cap_ - static_cast<size_t>(ptr_ - dst_);
    }

private:
    void flush_full_bytes() noexcept {
        /* Drain any remaining full bytes (used by flush() only). */
        while (bit_count_ >= 8) {
            assert(ptr_ + 1 <= dst_ + cap_);
            *ptr_++ = static_cast<uint8_t>(bits_);
            bits_      >>= 8;
            bit_count_  -= 8;
        }
    }

    uint8_t* const dst_;
    const size_t   cap_;
    uint8_t*       ptr_;
    uint64_t       bits_      = 0;
    int            bit_count_ = 0;
};

} } /* namespace orot::deflate */
