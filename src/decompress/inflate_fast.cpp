#include "inflate_fast.hpp"

#include <cassert>
#include <cstring>

namespace deflate {

/*
 * copy_match: back-copy `len` bytes from `dist` bytes back in output.
 *
 * Fast path: len <= 16, dist >= 16 → two word loads, no overlap possible.
 * Non-overlapping (len <= dist): single memcpy.
 * dist == 1: RLE memset.
 * Overlapping (dist < len, dist > 1): doubling memcpy, O(log(len/dist)) calls.
 */
static inline void copy_match(
    uint8_t* out, size_t dist, size_t len) noexcept
{
    /* Fast path: short match, guaranteed non-overlapping */
    if (__builtin_expect(len <= 16 && dist >= 16, 1)) {
        uint64_t w0, w1;
        std::memcpy(&w0, out - dist,     8);
        std::memcpy(&w1, out - dist + 8, 8);
        std::memcpy(out,     &w0, 8);
        std::memcpy(out + 8, &w1, 8);
        return;
    }
    if (len <= dist) {
        /* Non-overlapping: always safe to memcpy */
        std::memcpy(out, out - dist, len);
        return;
    }
    if (dist == 1) {
        /* RLE: single-byte repeat */
        std::memset(out, out[-1], len);
        return;
    }
    /* Overlapping RLE expansion: doubling memcpy.
     * O(log(len/dist)) calls instead of O(len) byte loop. */
    std::memcpy(out, out - dist, dist);
    size_t filled = dist;
    while (filled + dist <= len) {
        std::memcpy(out + filled, out + filled - dist, dist);
        filled += dist;
    }
    if (filled < len)
        std::memcpy(out + filled, out + filled - dist, len - filled);
}

bool inflate_fast(
    BitReader&           br,
    uint8_t*             out_buf,
    uint8_t*&            out_ptr,
    const uint8_t*       out_end,
    const InflateTables& tables)
{
    /* Safety margins: caller guarantees out_ptr + 258 <= out_end on entry */
    const uint8_t* safe_out_end = out_end - 258;

    for (;;) {
        /*
         * Unconditional fast refill — safe because:
         *  1. Loop entry: caller checked avail_in >= 10.
         *  2. Loop bottom: exits when !can_refill_fast().
         *  So at top of every iteration can_refill_fast() is guaranteed true.
         *
         * One refill gives 56 bits — enough for the full symbol pair:
         *   litlen(≤15) + len_extra(≤5) + dist(≤15) + dist_extra(≤13) = 48 bits max.
         * No intermediate refill_safe() calls needed inside the loop.
         */
        br.refill_fast();

        /* ── Decode literal/length symbol ──────────────────────────── */
        uint32_t entry = tables.litlen[br.peek_bits(LITLEN_DECODE_BITS)];

        if (__builtin_expect(entry & HUFF_SUBTABLE_FLAG, 0)) {
            /* Secondary table lookup (codes > LITLEN_DECODE_BITS bits) */
            const int secondary_bits = static_cast<int>((entry >> 16) & 0xFF);
            const int sec_offset     = static_cast<int>(entry & 0xFFFF);
            br.consume_bits(LITLEN_DECODE_BITS);
            entry = tables.litlen[sec_offset + br.peek_bits(secondary_bits)];
        }

        const int bits = static_cast<int>((entry >> 16) & 0xFF);
        br.consume_bits(bits);

        if (__builtin_expect(entry & HUFF_LITERAL_FLAG, 1)) {
            /* Literal byte — most common path; sym is in bits[7:0] */
            *out_ptr++ = static_cast<uint8_t>(entry);
            if (__builtin_expect(out_ptr > safe_out_end, 0)) return false;
            continue;
        }

        const int sym = static_cast<int>(entry & 0xFFFF);

        if (sym == 256) {
            /* End-of-block: signal caller that the block is complete. */
            return true;
        }

        /* Length code (sym 257-285) — 56 bits guaranteed, no refill needed */
        const int li        = sym - 257;
        int match_len       = LENGTH_BASE[li];
        const int len_extra = LENGTH_EXTRA_BITS[li];
        if (len_extra > 0)
            match_len += static_cast<int>(br.read_bits(len_extra));

        /* ── Decode distance symbol — still have ≥ 41 bits remaining ── */
        uint32_t dentry = tables.dist[br.peek_bits(DIST_DECODE_BITS)];
        if (__builtin_expect(dentry & HUFF_SUBTABLE_FLAG, 0)) {
            const int secondary_bits = static_cast<int>((dentry >> 16) & 0xFF);
            const int sec_offset     = static_cast<int>(dentry & 0xFFFF);
            br.consume_bits(DIST_DECODE_BITS);
            dentry = tables.dist[sec_offset + br.peek_bits(secondary_bits)];
        }

        const int di         = static_cast<int>(dentry & 0xFFFF);
        const int dbits      = static_cast<int>((dentry >> 16) & 0xFF);
        br.consume_bits(dbits);

        int dist = DIST_BASE[di];
        const int dist_extra = DIST_EXTRA_BITS[di];
        if (dist_extra > 0)
            dist += static_cast<int>(br.read_bits(dist_extra));

        /* Bounds check */
        if (out_ptr + match_len > out_end) return false;
        if (static_cast<ptrdiff_t>(dist) > out_ptr - out_buf) return false;

        copy_match(out_ptr, static_cast<size_t>(dist),
                   static_cast<size_t>(match_len));
        out_ptr += match_len;

        /* Stop if no longer in "safe" region */
        if (out_ptr > safe_out_end || !br.can_refill_fast())
            return false;
    }
}

} /* namespace deflate */
