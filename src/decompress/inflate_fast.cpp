#include "inflate_fast.hpp"

#include <cassert>
#include <cstring>

#if defined(DEFLATE_HAS_NEON)
#include <arm_neon.h>
#elif defined(DEFLATE_HAS_SSE2)
#include <emmintrin.h>
#endif

namespace orot { namespace deflate {

/*
 * copy_match: back-copy `len` bytes from `dist` bytes back in output.
 *
 * NEON fast paths:
 *   len ≤ 32, dist ≥ 32 → two 128-bit load+store (always writes 32 bytes).
 *   len ≤ 16, dist ≥ 16 → single 128-bit load+store (always writes 16 bytes).
 * Both are safe because the caller ensures out + 258 ≤ out_end; the extra
 * bytes written past `len` will be overwritten by subsequent decode ops.
 *
 * SSE2 fast path: len ≤ 16, dist ≥ 16 → single 128-bit load+store.
 * Scalar fast path: same condition, two word-sized memcpy calls.
 *
 * Non-overlapping (len ≤ dist): single memcpy.
 * dist == 1: RLE memset.
 * Overlapping (dist < len, dist > 1): doubling memcpy, O(log(len/dist)).
 */
static inline void copy_match(
    uint8_t* out, size_t dist, size_t len) noexcept
{
#if defined(DEFLATE_HAS_NEON)
    if (__builtin_expect(len <= 32 && dist >= 32, 1)) {
        vst1q_u8(out,      vld1q_u8(out - dist));
        vst1q_u8(out + 16, vld1q_u8(out - dist + 16));
        return;
    }
    if (__builtin_expect(len <= 16 && dist >= 16, 1)) {
        vst1q_u8(out, vld1q_u8(out - dist));
        return;
    }
#elif defined(DEFLATE_HAS_SSE2)
    if (__builtin_expect(len <= 16 && dist >= 16, 1)) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(out - dist)));
        return;
    }
#else
    if (__builtin_expect(len <= 16 && dist >= 16, 1)) {
        uint64_t w0, w1;
        std::memcpy(&w0, out - dist,     8);
        std::memcpy(&w1, out - dist + 8, 8);
        std::memcpy(out,     &w0, 8);
        std::memcpy(out + 8, &w1, 8);
        return;
    }
#endif
    if (len <= dist) {
        std::memcpy(out, out - dist, len);
        return;
    }
    if (dist == 1) {
        std::memset(out, out[-1], len);
        return;
    }
    /* Overlapping RLE expansion: doubling memcpy, O(log(len/dist)) calls. */
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
        if (__builtin_expect(src + 8 > src_end, 0)) goto done;
        {
            uint64_t word;
            std::memcpy(&word, src, 8);
            /* Prefetch compressed input ~3 refills ahead to hide load latency. */
            __builtin_prefetch(src + 24, 0, 0);
            bits    |= word << bit_cnt;
            int nb   = (63 - bit_cnt) >> 3;
            src     += nb;
            bit_cnt += nb << 3;
        }

        /* ── Decode literal/length symbol ──────────────────────────── */
        /*
         * decode_symbol: decode the next symbol using the current bit accumulator
         * without refilling.  When we decode a literal we jump back here if we
         * still have >= LITLEN_DECODE_BITS bits available, avoiding one 8-byte
         * load per literal — roughly halves the refill rate on literal-heavy
         * streams.  The jump is always backward, so C++ initialization rules
         * are not violated.
         */
decode_symbol:;
        {
            uint32_t e0 = tables.litlen[static_cast<uint32_t>(bits) & ((1u << LITLEN_DECODE_BITS) - 1)];

            /* Track whether secondary lookup is needed so NEON batch can skip it. */
            const bool had_secondary = !!(e0 & HUFF_SUBTABLE_FLAG);
            if (__builtin_expect(had_secondary, 0)) {
                /* Secondary table lookup (codes > LITLEN_DECODE_BITS bits).
                 * Extract secondary index WITHOUT consuming primary bits first —
                 * shift right by LITLEN_DECODE_BITS to see beyond the primary.
                 * Then the final `bits >>= ebits` below consumes the FULL code
                 * length in one step, avoiding the double-consume bug
                 * (consume_primary + consume_full_len = primary + full ≠ full). */
                const int sec_bits   = static_cast<int>((e0 >> 16) & 0xFF);
                const int sec_offset = static_cast<int>(e0 & 0xFFFF);
                e0 = tables.litlen[sec_offset +
                    ((static_cast<uint32_t>(bits) >> LITLEN_DECODE_BITS) & ((1u << sec_bits) - 1))];
            }

            const int ebits0 = static_cast<int>((e0 >> 16) & 0xFF);

#if defined(DEFLATE_HAS_NEON)
            /*
             * Nested speculative NEON literal batch: 1+5 (6 symbols) with 1+3 (4) fallback.
             *
             * e0 is on the dependency chain (already decoded).  Using ebits0 as stride,
             * speculatively issue e1..e5 loads — all independent of each other and of
             * future bit consumption, so the OOO core executes them in parallel.
             *
             * 1+5 attempt (bit_cnt ≥ 54): verifies e1..e5 with two uint32x4_t; uses
             *   vcreate_u32/vcombine_u32 to build vectors from integer registers without
             *   a stack round-trip; vminvq_u32 (single UMINV) checks all lanes.
             *   Outputs 6 bytes as one 64-bit store (safe: 258-byte headroom ensures
             *   8-byte write is within bounds; advance is only 6).
             *
             * 1+3 fallback (bit_cnt ≥ 36): reuses e1..e3 already in flight;
             *   single uint32x4_t + vminvq_u32; 4-byte store.
             */
            if (__builtin_expect(
                    !had_secondary &&
                    (e0 & HUFF_LITERAL_FLAG) &&
                    bit_cnt >= 4 * LITLEN_DECODE_BITS,
                    1)) {
                static constexpr uint32_t MASK9 = (1u << LITLEN_DECODE_BITS) - 1;
                const uint32_t CHECK = HUFF_LITERAL_FLAG | HUFF_SUBTABLE_FLAG | (0xFFu << 16);
                const uint32_t OK    = HUFF_LITERAL_FLAG | (static_cast<uint32_t>(ebits0) << 16);

                /* e1..e3 needed by both 1+5 and 1+3 paths — load them unconditionally. */
                const uint32_t e1 = tables.litlen[static_cast<uint32_t>(bits >> ebits0)         & MASK9];
                const uint32_t e2 = tables.litlen[static_cast<uint32_t>(bits >> (2 * ebits0))   & MASK9];
                const uint32_t e3 = tables.litlen[static_cast<uint32_t>(bits >> (3 * ebits0))   & MASK9];

                if (bit_cnt >= 6 * LITLEN_DECODE_BITS) {
                    /* 1+5: additionally load e4, e5 */
                    const uint32_t e4 = tables.litlen[static_cast<uint32_t>(bits >> (4 * ebits0)) & MASK9];
                    const uint32_t e5 = tables.litlen[static_cast<uint32_t>(bits >> (5 * ebits0)) & MASK9];

                    const uint32x4_t mask_v = vdupq_n_u32(CHECK);
                    const uint32x4_t ok_v   = vdupq_n_u32(OK);
                    /* Build vectors directly from integer registers: no stack store/load */
                    const uint32x4_t ev0 = vcombine_u32(
                        vcreate_u32((uint64_t)e1 | ((uint64_t)e2 << 32)),
                        vcreate_u32((uint64_t)e3 | ((uint64_t)e4 << 32)));
                    /* Pad lanes 6-8 with OK so they always pass verification */
                    const uint32x4_t ev1 = vcombine_u32(
                        vcreate_u32((uint64_t)e5 | ((uint64_t)OK << 32)),
                        vdup_n_u32(OK));
                    const uint32x4_t cmp = vandq_u32(
                        vceqq_u32(vandq_u32(ev0, mask_v), ok_v),
                        vceqq_u32(vandq_u32(ev1, mask_v), ok_v));

                    if (__builtin_expect(vminvq_u32(cmp) == UINT32_MAX, 1)) {
                        /* Pack 6 literal bytes into one 64-bit store (upper 2 bytes = 0,
                         * overwritten by next decode; safe within 258-byte headroom). */
                        const uint64_t pack =
                            (uint64_t)(uint8_t)e0 | ((uint64_t)(uint8_t)e1 <<  8) |
                            ((uint64_t)(uint8_t)e2 << 16) | ((uint64_t)(uint8_t)e3 << 24) |
                            ((uint64_t)(uint8_t)e4 << 32) | ((uint64_t)(uint8_t)e5 << 40);
                        std::memcpy(out, &pack, 8);
                        out     += 6;
                        bits    >>= 6 * ebits0;
                        bit_cnt -=  6 * ebits0;
                        if (__builtin_expect(out > safe_out_end, 0)) goto done;
                        if (__builtin_expect(bit_cnt >= LITLEN_DECODE_BITS, 1))
                            goto decode_symbol;
                        continue;
                    }
                    /* 1+5 failed: fall through to 1+3 with e1..e3 already in registers */
                }

                {
                    /* 1+3 fallback: verify e1..e3; pad lane 4 with OK (always passes). */
                    const uint32x4_t ev = vcombine_u32(
                        vcreate_u32((uint64_t)e1 | ((uint64_t)e2 << 32)),
                        vcreate_u32((uint64_t)e3 | ((uint64_t)OK << 32)));
                    const uint32x4_t cmp = vceqq_u32(
                        vandq_u32(ev, vdupq_n_u32(CHECK)), vdupq_n_u32(OK));

                    if (__builtin_expect(vminvq_u32(cmp) == UINT32_MAX, 1)) {
                        uint32_t pack4 =
                            (uint32_t)(uint8_t)e0 | ((uint32_t)(uint8_t)e1 <<  8) |
                            ((uint32_t)(uint8_t)e2 << 16) | ((uint32_t)(uint8_t)e3 << 24);
                        std::memcpy(out, &pack4, 4);
                        out     += 4;
                        bits    >>= 4 * ebits0;
                        bit_cnt -=  4 * ebits0;
                        if (__builtin_expect(out > safe_out_end, 0)) goto done;
                        if (__builtin_expect(bit_cnt >= LITLEN_DECODE_BITS, 1))
                            goto decode_symbol;
                        continue;
                    }
                }
            }
#elif defined(DEFLATE_HAS_SSE2)
            /*
             * x86 nested literal batch: same 1+5 + 1+3 structure as NEON,
             * using scalar bitwise AND for verification (OOO executes in parallel).
             * Packed 32/64-bit stores replace individual byte writes.
             */
            if (__builtin_expect(
                    !had_secondary &&
                    (e0 & HUFF_LITERAL_FLAG) &&
                    bit_cnt >= 4 * LITLEN_DECODE_BITS,
                    1)) {
                static constexpr uint32_t MASK9 = (1u << LITLEN_DECODE_BITS) - 1;
                const uint32_t CHECK = HUFF_LITERAL_FLAG | HUFF_SUBTABLE_FLAG | (0xFFu << 16);
                const uint32_t OK    = HUFF_LITERAL_FLAG | (static_cast<uint32_t>(ebits0) << 16);

                const uint32_t e1 = tables.litlen[static_cast<uint32_t>(bits >> ebits0)         & MASK9];
                const uint32_t e2 = tables.litlen[static_cast<uint32_t>(bits >> (2 * ebits0))   & MASK9];
                const uint32_t e3 = tables.litlen[static_cast<uint32_t>(bits >> (3 * ebits0))   & MASK9];

                if (bit_cnt >= 6 * LITLEN_DECODE_BITS) {
                    const uint32_t e4 = tables.litlen[static_cast<uint32_t>(bits >> (4 * ebits0)) & MASK9];
                    const uint32_t e5 = tables.litlen[static_cast<uint32_t>(bits >> (5 * ebits0)) & MASK9];
                    if (__builtin_expect(
                            ((e1 & CHECK) == OK) & ((e2 & CHECK) == OK) & ((e3 & CHECK) == OK) &
                            ((e4 & CHECK) == OK) & ((e5 & CHECK) == OK), 1)) {
                        const uint64_t pack =
                            (uint64_t)(uint8_t)e0 | ((uint64_t)(uint8_t)e1 <<  8) |
                            ((uint64_t)(uint8_t)e2 << 16) | ((uint64_t)(uint8_t)e3 << 24) |
                            ((uint64_t)(uint8_t)e4 << 32) | ((uint64_t)(uint8_t)e5 << 40);
                        std::memcpy(out, &pack, 8);
                        out     += 6;
                        bits    >>= 6 * ebits0;
                        bit_cnt -=  6 * ebits0;
                        if (__builtin_expect(out > safe_out_end, 0)) goto done;
                        if (__builtin_expect(bit_cnt >= LITLEN_DECODE_BITS, 1))
                            goto decode_symbol;
                        continue;
                    }
                }
                if (__builtin_expect(
                        ((e1 & CHECK) == OK) & ((e2 & CHECK) == OK) & ((e3 & CHECK) == OK), 1)) {
                    uint32_t pack4 =
                        (uint32_t)(uint8_t)e0 | ((uint32_t)(uint8_t)e1 <<  8) |
                        ((uint32_t)(uint8_t)e2 << 16) | ((uint32_t)(uint8_t)e3 << 24);
                    std::memcpy(out, &pack4, 4);
                    out     += 4;
                    bits    >>= 4 * ebits0;
                    bit_cnt -=  4 * ebits0;
                    if (__builtin_expect(out > safe_out_end, 0)) goto done;
                    if (__builtin_expect(bit_cnt >= LITLEN_DECODE_BITS, 1))
                        goto decode_symbol;
                    continue;
                }
            }
#endif /* DEFLATE_HAS_NEON / DEFLATE_HAS_SSE2 */

            /* Scalar fallback: consume e0's bits and handle it alone. */
            bits    >>= ebits0;
            bit_cnt -= ebits0;

            if (__builtin_expect(e0 & HUFF_LITERAL_FLAG, 1)) {
                /* Literal — most common path; sym in bits[7:0] */
                *out++ = static_cast<uint8_t>(e0);
                if (__builtin_expect(out > safe_out_end, 0)) goto done;
                if (__builtin_expect(bit_cnt >= LITLEN_DECODE_BITS, 1))
                    goto decode_symbol;
                continue;
            }

            {
                const int sym = static_cast<int>(e0 & 0xFFFF);

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

} } /* namespace orot::deflate */
