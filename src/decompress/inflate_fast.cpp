#include "inflate_fast.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace deflate {

/*
 * Inline back-copy helper: copy `len` bytes from `dist` bytes back in output.
 * Handles overlapping copies (e.g., run-length expansions).
 */
static inline void copy_match(
    uint8_t* out, size_t dist, size_t len) noexcept
{
    const uint8_t* src = out - dist;
    if (dist >= 16 && len <= dist) {
        /* Non-overlapping: bulk copy */
        std::memcpy(out, src, len);
    } else {
        /* Overlapping: byte-by-byte (handles RLE patterns) */
        while (len-- > 0) *out++ = *src++;
    }
}

bool inflate_fast(
    BitReader&           br,
    uint8_t*             out_buf,
    uint8_t*&            out_ptr,
    const uint8_t*       out_end,
    const InflateTables& tables)
{
    /* Safety margins */
    const uint8_t* safe_out_end = out_end - 258;
    (void)safe_out_end;

    for (;;) {
        /* Ensure bits available */
        if (br.can_refill_fast())
            br.refill_fast();
        else
            br.refill_safe();

        /* ── Decode literal/length symbol ──────────────────────────── */
        uint32_t entry = tables.litlen[br.peek_bits(LITLEN_DECODE_BITS)];

        if (entry & HUFF_SUBTABLE_FLAG) {
            /* Secondary table lookup */
            const int primary_bits  = LITLEN_DECODE_BITS;
            const int secondary_bits = static_cast<int>((entry >> 16) & 0xFF);
            const int sec_offset     = static_cast<int>(entry & 0xFFFF);
            br.consume_bits(primary_bits);
            entry = tables.litlen[sec_offset + br.peek_bits(secondary_bits)];
        }

        const int sym  = static_cast<int>(entry & 0xFFFF);
        const int bits = static_cast<int>((entry >> 16) & 0xFF);
        br.consume_bits(bits);

        if (sym < 256) {
            /* Literal byte */
            if (out_ptr >= out_end) return false;
            *out_ptr++ = static_cast<uint8_t>(sym);
            continue;
        }

        if (sym == 256) {
            /* End-of-block */
            return false;
        }

        /* Length code (sym 257-285) */
        const int li        = sym - 257;
        int match_len       = LENGTH_BASE[li];
        const int len_extra = LENGTH_EXTRA_BITS[li];
        if (len_extra > 0) {
            if (br.bits_available() < len_extra) br.refill_safe();
            match_len += static_cast<int>(br.read_bits(len_extra));
        }

        /* ── Decode distance symbol ─────────────────────────────────── */
        if (br.bits_available() < DIST_DECODE_BITS + 13)
            br.refill_safe();

        uint32_t dentry = tables.dist[br.peek_bits(DIST_DECODE_BITS)];
        if (dentry & HUFF_SUBTABLE_FLAG) {
            const int primary_bits   = DIST_DECODE_BITS;
            const int secondary_bits = static_cast<int>((dentry >> 16) & 0xFF);
            const int sec_offset     = static_cast<int>(dentry & 0xFFFF);
            br.consume_bits(primary_bits);
            dentry = tables.dist[sec_offset + br.peek_bits(secondary_bits)];
        }

        const int di         = static_cast<int>(dentry & 0xFFFF);
        const int dbits      = static_cast<int>((dentry >> 16) & 0xFF);
        br.consume_bits(dbits);

        int dist = DIST_BASE[di];
        const int dist_extra = DIST_EXTRA_BITS[di];
        if (dist_extra > 0) {
            if (br.bits_available() < dist_extra) br.refill_safe();
            dist += static_cast<int>(br.read_bits(dist_extra));
        }

        /* Bounds check */
        if (out_ptr + match_len > out_end) return false;
        if (static_cast<ptrdiff_t>(dist) > out_ptr - out_buf) return false;

        copy_match(out_ptr, static_cast<size_t>(dist),
                   static_cast<size_t>(match_len));
        out_ptr += match_len;

        /* Stop if no longer in "safe" region */
        if (out_ptr + 258 > out_end || !br.can_refill_fast())
            return false;
    }
}

} /* namespace deflate */
