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
    /*
     * Hoist all hot state to local variables so the compiler can keep them
     * in registers.  The critical issue: out_ptr is uint8_t*& (reference),
     * so every *out_ptr++ forces a memory load+store through the reference.
     * Similarly, br methods access member fields through a reference, and
     * the compiler must assume that writing through a uint8_t* can alias any
     * type (C++ strict aliasing: unsigned char exemption), so it would reload
     * br.bits_ / br.bit_count_ / br.ptr_ after every *out++ store.
     * Local copies break the alias chain → registers throughout the loop.
     */
    const uint8_t* src     = br.current_ptr();
    const uint8_t* src_end = br.end_ptr();
    uint64_t       bits    = br.raw_bits();
    int            bit_cnt = br.raw_bit_count();
    uint8_t*       out     = out_ptr;

    const uint8_t* safe_out_end = out_end - 258;

    bool ended = false;

    for (;;) {
        /*
         * Unconditional fast refill — safe because:
         *  1. Loop entry: caller checked avail_in >= 10.
         *  2. Loop bottom: exits when src + 8 > src_end.
         * One refill gives ≤56 bits of new data; total in accumulator ≤63 bits.
         * That covers: litlen(≤15) + len_extra(≤5) + dist(≤15) + dist_extra(≤13) = 48 bits max.
         */
        {
            uint64_t word;
            std::memcpy(&word, src, 8);
            bits    |= word << bit_cnt;
            int nb   = (63 - bit_cnt) >> 3;
            src     += nb;
            bit_cnt += nb << 3;
        }

        /* ── Decode literal/length symbol ──────────────────────────── */
        uint32_t entry = tables.litlen[static_cast<uint32_t>(bits) & ((1u << LITLEN_DECODE_BITS) - 1)];

        if (__builtin_expect(entry & HUFF_SUBTABLE_FLAG, 0)) {
            /* Secondary table lookup (codes > LITLEN_DECODE_BITS bits).
             * Extract secondary index WITHOUT consuming primary bits first —
             * shift right by LITLEN_DECODE_BITS to see beyond the primary.
             * Then the final `bits >>= ebits` below consumes the FULL code
             * length in one step, avoiding the double-consume bug
             * (consume_primary + consume_full_len = primary + full ≠ full). */
            const int sec_bits   = static_cast<int>((entry >> 16) & 0xFF);
            const int sec_offset = static_cast<int>(entry & 0xFFFF);
            entry = tables.litlen[sec_offset +
                ((static_cast<uint32_t>(bits) >> LITLEN_DECODE_BITS) & ((1u << sec_bits) - 1))];
        }

        const int ebits = static_cast<int>((entry >> 16) & 0xFF);
        bits    >>= ebits;
        bit_cnt -= ebits;

        if (__builtin_expect(entry & HUFF_LITERAL_FLAG, 1)) {
            /* Literal — most common path; sym in bits[7:0] */
            *out++ = static_cast<uint8_t>(entry);
            if (__builtin_expect(out > safe_out_end, 0)) goto done;
            continue;
        }

        {
            const int sym = static_cast<int>(entry & 0xFFFF);

            if (sym == 256) {
                /* End-of-block */
                ended = true;
                goto done;
            }

            /* Length code (sym 257-285) */
            const int li        = sym - 257;
            int       match_len = LENGTH_BASE[li];
            const int len_extra = LENGTH_EXTRA_BITS[li];
            if (len_extra > 0) {
                match_len += static_cast<int>(static_cast<uint32_t>(bits) & ((1u << len_extra) - 1));
                bits    >>= len_extra;
                bit_cnt -= len_extra;
            }

            /* ── Decode distance symbol ── */
            uint32_t dentry = tables.dist[static_cast<uint32_t>(bits) & ((1u << DIST_DECODE_BITS) - 1)];
            if (__builtin_expect(dentry & HUFF_SUBTABLE_FLAG, 0)) {
                /* Same fix as litlen: extract secondary index without consuming
                 * primary bits — shift right by DIST_DECODE_BITS instead.    */
                const int sec_bits   = static_cast<int>((dentry >> 16) & 0xFF);
                const int sec_offset = static_cast<int>(dentry & 0xFFFF);
                dentry = tables.dist[sec_offset +
                    ((static_cast<uint32_t>(bits) >> DIST_DECODE_BITS) & ((1u << sec_bits) - 1))];
            }

            const int di    = static_cast<int>(dentry & 0xFFFF);
            const int dbits = static_cast<int>((dentry >> 16) & 0xFF);
            bits    >>= dbits;
            bit_cnt -= dbits;

            int dist = DIST_BASE[di];
            const int dist_extra = DIST_EXTRA_BITS[di];
            if (dist_extra > 0) {
                dist    += static_cast<int>(static_cast<uint32_t>(bits) & ((1u << dist_extra) - 1));
                bits    >>= dist_extra;
                bit_cnt -= dist_extra;
            }

            /* Bounds check */
            if (out + match_len > out_end) goto done;
            if (static_cast<ptrdiff_t>(dist) > out - out_buf) goto done;

            copy_match(out, static_cast<size_t>(dist), static_cast<size_t>(match_len));
            out += match_len;
        }

        /* Stop if no longer in "safe" region */
        if (out > safe_out_end || src + 8 > src_end)
            goto done;
    }

done:
    br.restore(src, bits, bit_cnt);
    out_ptr = out;
    return ended;
}

} /* namespace deflate */
