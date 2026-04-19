#include "inflate_fast.hpp"

#include <algorithm>
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
 * NEON fast paths (non-overlapping only — dist >= len):
 *   len ≤ 128, dist ≥ 128 → 8× 128-bit ops
 *   len ≤  64, dist ≥  64 → 4× 128-bit ops
 *   len ≤  32, dist ≥  32 → 2× 128-bit ops
 *   len ≤  16, dist ≥  16 → 1× 128-bit op (most common)
 *
 * SSE2 fast paths:
 *   len ≤  32, dist ≥  32 → 2× 128-bit ops
 *   len ≤  16, dist ≥  16 → 1× 128-bit op
 *
 * Scalar fast path: len ≤ 16, dist ≥ 16 → two word memcpy.
 *
 * Non-overlapping (len ≤ dist): single memcpy.
 * dist == 1: RLE memset.
 * Overlapping (dist < len, dist > 1): doubling memcpy, O(log(len/dist)).
 */
static inline void copy_match(
    uint8_t* out, size_t dist, size_t len) noexcept
{
#if defined(DEFLATE_HAS_NEON)
    if (__builtin_expect(len <= 16 && dist >= 16, 1)) {
        vst1q_u8(out, vld1q_u8(out - dist));
        return;
    }
    if (__builtin_expect(len <= 32 && dist >= 32, 0)) {
        const uint8_t* s = out - dist;
        vst1q_u8(out,      vld1q_u8(s));
        vst1q_u8(out + 16, vld1q_u8(s + 16));
        return;
    }
    if (__builtin_expect(len <= 64 && dist >= 64, 0)) {
        const uint8_t* s = out - dist;
        vst1q_u8(out,      vld1q_u8(s));
        vst1q_u8(out + 16, vld1q_u8(s + 16));
        vst1q_u8(out + 32, vld1q_u8(s + 32));
        vst1q_u8(out + 48, vld1q_u8(s + 48));
        return;
    }
    if (__builtin_expect(len <= 128 && dist >= 128, 0)) {
        const uint8_t* s = out - dist;
        for (int k = 0; k < 8; ++k)
            vst1q_u8(out + k * 16, vld1q_u8(s + k * 16));
        return;
    }
#elif defined(DEFLATE_HAS_SSE2)
    if (__builtin_expect(len <= 16 && dist >= 16, 1)) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(out - dist)));
        return;
    }
    if (__builtin_expect(len <= 32 && dist >= 32, 0)) {
        const uint8_t* s = out - dist;
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(s)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16),
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16)));
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
#if defined(__aarch64__)
    /* dist 2..7 overlapping: splat the pattern via vqtbl1q_u8 and store 16B at a time. */
    if (__builtin_expect(dist <= 7, 0)) {
        static const uint8_t tbl_idx[6][16] = {
            {0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},       /* dist=2 */
            {0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},       /* dist=3 */
            {0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3},       /* dist=4 */
            {0,1,2,3,4,0,1,2,3,4,0,1,2,3,4,0},       /* dist=5 */
            {0,1,2,3,4,5,0,1,2,3,4,5,0,1,2,3},       /* dist=6 */
            {0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1},       /* dist=7 */
        };
        const uint8x16_t src = vld1q_u8(out - dist);
        const uint8x16_t idx = vld1q_u8(tbl_idx[dist - 2]);
        const uint8x16_t pat = vqtbl1q_u8(src, idx);
        size_t done = 0;
        for (; done + 16 <= len; done += 16)
            vst1q_u8(out + done, pat);
        if (done < len) {
            uint8_t tmp[16];
            vst1q_u8(tmp, pat);
            std::memcpy(out + done, tmp, len - done);
        }
        return;
    }
#endif
    std::memcpy(out, out - dist, dist);
    size_t filled = dist;
    while (filled < len) {
        const size_t chunk = std::min(filled, len - filled);
        std::memcpy(out + filled, out, chunk);
        filled += chunk;
    }
}

/*
 * 128-bit bit accumulator helpers.
 *
 * State: bits_lo (bits 0..63), bits_hi (bits 64..127), bit_cnt (0..128).
 * Invariant: bit i is in lo[i] if i < 64, in hi[i-64] if i >= 64.
 * bits[0..bit_cnt-1] are guaranteed valid stream bits.
 *
 * acc_extract: extract LITLEN_DECODE_BITS bits at offset `off`.
 * acc_consume: discard the low n bits (right-shift the 128-bit register).
 */

static constexpr uint32_t ACC_MASK11 = (1u << LITLEN_DECODE_BITS) - 1;

static inline uint32_t acc_extract(uint64_t lo, uint64_t hi, int off) noexcept {
    if (__builtin_expect(off + LITLEN_DECODE_BITS <= 64, 1))
        return static_cast<uint32_t>(lo >> off) & ACC_MASK11;
    if (off >= 64)
        return static_cast<uint32_t>(hi >> (off - 64)) & ACC_MASK11;
    /* off in [64-LITLEN_DECODE_BITS .. 63]: straddles lo/hi boundary */
    return static_cast<uint32_t>((lo >> off) | (hi << (64 - off))) & ACC_MASK11;
}

static inline void acc_consume(uint64_t& lo, uint64_t& hi, int& cnt, int n) noexcept {
    if (__builtin_expect(n == 0, 0)) return;
    if (__builtin_expect(n < 64, 1)) {
        /* n in [1..63]: no undefined shift (64-n in [1..63], n in [1..63]) */
        lo   = (lo >> n) | (hi << (64 - n));
        hi >>= n;
    } else {
        /* n in [64..127]: lo gets hi shifted right, hi zeroed */
        lo   = (n < 128) ? (hi >> (n - 64)) : 0ULL;
        hi   = 0;
    }
    cnt -= n;
}

bool inflate_fast(
    BitReader&           br,
    uint8_t*             out_buf,
    uint8_t*&            out_ptr,
    const uint8_t*       out_end,
    const InflateTables& tables)
{
    const uint8_t* src     = br.current_ptr();
    const uint8_t* src_end = br.end_ptr();
    uint64_t       bits_lo = br.raw_bits();
    uint64_t       bits_hi = 0;
    int            bit_cnt = br.raw_bit_count();
    uint8_t*       out     = out_ptr;

    const uint8_t* safe_out_end = out_end - 258;

    bool ended = false;

    /* Minimum bits needed at decode_symbol to decode any symbol without refill.
     * Worst case: 15 (litlen) + 5 (len_extra) + 15 (dist) + 13 (dist_extra) = 48.
     * Use 56 for alignment with the 7-byte standard refill quantum. */
    static constexpr int MIN_DECODE_BITS = 56;

    for (;;) {
        /*
         * 128-bit refill — two sequential 8-byte loads.
         *
         * Load 1 (fill lo to ~56 bits):
         *   Standard 64-bit refill: advance src by (63-bit_cnt)/8 bytes.
         *   After Load 1: bit_cnt in [56..63], lo holds ~56-63 bits.
         *
         * Load 2 (fill hi to get ~120 bits total):
         *   Places the next word starting at bit position bit_cnt.
         *   After Load 2: bit_cnt in [120..127], hi holds ~56-63 bits.
         *
         * At the top of the loop, bit_cnt is always <= 63 (invariant: see notes
         * on goto decode_symbol below).
         */
        if (__builtin_expect(src + 8 > src_end, 0)) goto done;

        /* Load 1: fill bits_lo */
        {
            uint64_t w;
            std::memcpy(&w, src, 8);
            bits_lo |= w << bit_cnt;
            const int nb = (63 - bit_cnt) >> 3;
            src     += nb;
            bit_cnt += nb << 3;
        }

        /* Load 2: fill bits_hi (only if there's room and input available) */
        if (__builtin_expect(src + 8 <= src_end && bit_cnt < 120, 1)) {
            uint64_t w;
            std::memcpy(&w, src, 8);
            const int pos = bit_cnt;
            /* pos is in [56..63]: always < 64, so use the split formula */
            bits_lo |= w << pos;                          /* fills lo[pos..63]: 1 byte max */
            bits_hi  = (pos > 0) ? (w >> (64 - pos))     /* fills hi[0..63-pos] from w */
                                 : 0ULL;
            const int nb = (127 - pos) >> 3;
            src     += nb;
            bit_cnt += nb << 3;
        }

        /* ── Decode literal/length symbol ──────────────────────────── */
decode_symbol:;
        {
            const uint32_t e0_idx =
                static_cast<uint32_t>(bits_lo) & ACC_MASK11;
            uint32_t e0 = tables.litlen[e0_idx];

            const bool had_secondary = !!(e0 & HUFF_SUBTABLE_FLAG);
            if (__builtin_expect(had_secondary, 0)) {
                const int sec_bits   = static_cast<int>((e0 >> 16) & 0xFF);
                const int sec_offset = static_cast<int>(e0 & 0xFFFF);
                e0 = tables.litlen[sec_offset +
                    ((static_cast<uint32_t>(bits_lo) >> LITLEN_DECODE_BITS) &
                     ((1u << sec_bits) - 1))];
            }

            const int ebits0 = static_cast<int>((e0 >> 16) & 0xFF);

#if defined(DEFLATE_HAS_NEON)
            /*
             * NEON speculative literal batch: 1+8 (9 symbols) with 1+5 (6)
             * and 1+3 (4) fallbacks.
             *
             * Requires bit_cnt >= 9 * LITLEN_DECODE_BITS (99 bits with 11-bit table).
             * Uses the 128-bit accumulator to supply all 99 bits in one pass.
             *
             * The 1+8 batch stores 9 bytes as one 8-byte + 1-byte write.
             * Falls through to 1+5 or 1+3 when any symbol fails verification.
             */
            if (__builtin_expect(
                    !had_secondary &&
                    (e0 & HUFF_LITERAL_FLAG) &&
                    bit_cnt >= 9 * LITLEN_DECODE_BITS,
                    1)) {
                const uint32_t CHECK = HUFF_LITERAL_FLAG | HUFF_SUBTABLE_FLAG | (0xFFu << 16);
                const uint32_t OK    = HUFF_LITERAL_FLAG | (static_cast<uint32_t>(ebits0) << 16);

                const uint32_t e1 = tables.litlen[acc_extract(bits_lo, bits_hi,   ebits0)];
                const uint32_t e2 = tables.litlen[acc_extract(bits_lo, bits_hi, 2*ebits0)];
                const uint32_t e3 = tables.litlen[acc_extract(bits_lo, bits_hi, 3*ebits0)];
                const uint32_t e4 = tables.litlen[acc_extract(bits_lo, bits_hi, 4*ebits0)];
                const uint32_t e5 = tables.litlen[acc_extract(bits_lo, bits_hi, 5*ebits0)];
                const uint32_t e6 = tables.litlen[acc_extract(bits_lo, bits_hi, 6*ebits0)];
                const uint32_t e7 = tables.litlen[acc_extract(bits_lo, bits_hi, 7*ebits0)];
                const uint32_t e8 = tables.litlen[acc_extract(bits_lo, bits_hi, 8*ebits0)];

                const uint32x4_t mask_v = vdupq_n_u32(CHECK);
                const uint32x4_t ok_v   = vdupq_n_u32(OK);
                const uint32x4_t ev0 = vcombine_u32(
                    vcreate_u32((uint64_t)e1 | ((uint64_t)e2 << 32)),
                    vcreate_u32((uint64_t)e3 | ((uint64_t)e4 << 32)));
                const uint32x4_t ev1 = vcombine_u32(
                    vcreate_u32((uint64_t)e5 | ((uint64_t)e6 << 32)),
                    vcreate_u32((uint64_t)e7 | ((uint64_t)e8 << 32)));
                const uint32x4_t cmp = vandq_u32(
                    vceqq_u32(vandq_u32(ev0, mask_v), ok_v),
                    vceqq_u32(vandq_u32(ev1, mask_v), ok_v));

                if (__builtin_expect(vminvq_u32(cmp) == UINT32_MAX, 1)) {
                    const uint64_t pack8 =
                        (uint64_t)(uint8_t)e0 | ((uint64_t)(uint8_t)e1 <<  8) |
                        ((uint64_t)(uint8_t)e2 << 16) | ((uint64_t)(uint8_t)e3 << 24) |
                        ((uint64_t)(uint8_t)e4 << 32) | ((uint64_t)(uint8_t)e5 << 40) |
                        ((uint64_t)(uint8_t)e6 << 48) | ((uint64_t)(uint8_t)e7 << 56);
                    std::memcpy(out, &pack8, 8);
                    out[8] = static_cast<uint8_t>(e8);
                    out += 9;
                    acc_consume(bits_lo, bits_hi, bit_cnt, 9 * ebits0);
                    if (__builtin_expect(out > safe_out_end, 0)) goto done;
                    if (__builtin_expect(bit_cnt >= MIN_DECODE_BITS, 1))
                        goto decode_symbol;
                    continue;
                }

                /* 1+8 failed: try 1+5 */
                if (__builtin_expect(bit_cnt >= 6 * LITLEN_DECODE_BITS, 1)) {
                    /* ev0 == {e1,e2,e3,e4} — reuse instead of rebuilding ev2 */
                    const uint32x4_t ev3 = vcombine_u32(
                        vcreate_u32((uint64_t)e5 | ((uint64_t)OK << 32)),
                        vdup_n_u32(OK));
                    const uint32x4_t cmp2 = vandq_u32(
                        vceqq_u32(vandq_u32(ev0, mask_v), ok_v),
                        vceqq_u32(vandq_u32(ev3, mask_v), ok_v));
                    if (__builtin_expect(vminvq_u32(cmp2) == UINT32_MAX, 1)) {
                        const uint64_t pack6 =
                            (uint64_t)(uint8_t)e0 | ((uint64_t)(uint8_t)e1 <<  8) |
                            ((uint64_t)(uint8_t)e2 << 16) | ((uint64_t)(uint8_t)e3 << 24) |
                            ((uint64_t)(uint8_t)e4 << 32) | ((uint64_t)(uint8_t)e5 << 40);
                        std::memcpy(out, &pack6, 8);  /* safe: 258-byte headroom */
                        out += 6;
                        acc_consume(bits_lo, bits_hi, bit_cnt, 6 * ebits0);
                        if (__builtin_expect(out > safe_out_end, 0)) goto done;
                        if (__builtin_expect(bit_cnt >= MIN_DECODE_BITS, 1))
                            goto decode_symbol;
                        continue;
                    }
                }

                /* 1+3 fallback */
                {
                    const uint32x4_t ev4 = vcombine_u32(
                        vcreate_u32((uint64_t)e1 | ((uint64_t)e2 << 32)),
                        vcreate_u32((uint64_t)e3 | ((uint64_t)OK << 32)));
                    const uint32x4_t cmp3 = vceqq_u32(
                        vandq_u32(ev4, vdupq_n_u32(CHECK)), vdupq_n_u32(OK));
                    if (__builtin_expect(vminvq_u32(cmp3) == UINT32_MAX, 1)) {
                        const uint32_t pack4 =
                            (uint32_t)(uint8_t)e0 | ((uint32_t)(uint8_t)e1 <<  8) |
                            ((uint32_t)(uint8_t)e2 << 16) | ((uint32_t)(uint8_t)e3 << 24);
                        std::memcpy(out, &pack4, 4);
                        out += 4;
                        acc_consume(bits_lo, bits_hi, bit_cnt, 4 * ebits0);
                        if (__builtin_expect(out > safe_out_end, 0)) goto done;
                        if (__builtin_expect(bit_cnt >= MIN_DECODE_BITS, 1))
                            goto decode_symbol;
                        continue;
                    }
                }
            }
#elif defined(DEFLATE_HAS_SSE2)
            /*
             * x86 1+8 literal batch (scalar AND verification, OOO-friendly).
             */
            if (__builtin_expect(
                    !had_secondary &&
                    (e0 & HUFF_LITERAL_FLAG) &&
                    bit_cnt >= 9 * LITLEN_DECODE_BITS,
                    1)) {
                const uint32_t CHECK = HUFF_LITERAL_FLAG | HUFF_SUBTABLE_FLAG | (0xFFu << 16);
                const uint32_t OK    = HUFF_LITERAL_FLAG | (static_cast<uint32_t>(ebits0) << 16);

                const uint32_t e1 = tables.litlen[acc_extract(bits_lo, bits_hi,   ebits0)];
                const uint32_t e2 = tables.litlen[acc_extract(bits_lo, bits_hi, 2*ebits0)];
                const uint32_t e3 = tables.litlen[acc_extract(bits_lo, bits_hi, 3*ebits0)];
                const uint32_t e4 = tables.litlen[acc_extract(bits_lo, bits_hi, 4*ebits0)];
                const uint32_t e5 = tables.litlen[acc_extract(bits_lo, bits_hi, 5*ebits0)];
                const uint32_t e6 = tables.litlen[acc_extract(bits_lo, bits_hi, 6*ebits0)];
                const uint32_t e7 = tables.litlen[acc_extract(bits_lo, bits_hi, 7*ebits0)];
                const uint32_t e8 = tables.litlen[acc_extract(bits_lo, bits_hi, 8*ebits0)];

                if (__builtin_expect(
                        ((e1 & CHECK) == OK) & ((e2 & CHECK) == OK) &
                        ((e3 & CHECK) == OK) & ((e4 & CHECK) == OK) &
                        ((e5 & CHECK) == OK) & ((e6 & CHECK) == OK) &
                        ((e7 & CHECK) == OK) & ((e8 & CHECK) == OK), 1)) {
                    const uint64_t pack8 =
                        (uint64_t)(uint8_t)e0 | ((uint64_t)(uint8_t)e1 <<  8) |
                        ((uint64_t)(uint8_t)e2 << 16) | ((uint64_t)(uint8_t)e3 << 24) |
                        ((uint64_t)(uint8_t)e4 << 32) | ((uint64_t)(uint8_t)e5 << 40) |
                        ((uint64_t)(uint8_t)e6 << 48) | ((uint64_t)(uint8_t)e7 << 56);
                    std::memcpy(out, &pack8, 8);
                    out[8] = static_cast<uint8_t>(e8);
                    out += 9;
                    acc_consume(bits_lo, bits_hi, bit_cnt, 9 * ebits0);
                    if (__builtin_expect(out > safe_out_end, 0)) goto done;
                    if (__builtin_expect(bit_cnt >= MIN_DECODE_BITS, 1))
                        goto decode_symbol;
                    continue;
                }
                /* 1+5 fallback */
                if (__builtin_expect(
                        ((e1 & CHECK) == OK) & ((e2 & CHECK) == OK) &
                        ((e3 & CHECK) == OK) & ((e4 & CHECK) == OK) &
                        ((e5 & CHECK) == OK), 1)) {
                    const uint64_t pack6 =
                        (uint64_t)(uint8_t)e0 | ((uint64_t)(uint8_t)e1 <<  8) |
                        ((uint64_t)(uint8_t)e2 << 16) | ((uint64_t)(uint8_t)e3 << 24) |
                        ((uint64_t)(uint8_t)e4 << 32) | ((uint64_t)(uint8_t)e5 << 40);
                    std::memcpy(out, &pack6, 8);
                    out += 6;
                    acc_consume(bits_lo, bits_hi, bit_cnt, 6 * ebits0);
                    if (__builtin_expect(out > safe_out_end, 0)) goto done;
                    if (__builtin_expect(bit_cnt >= MIN_DECODE_BITS, 1))
                        goto decode_symbol;
                    continue;
                }
                /* 1+3 fallback */
                if (__builtin_expect(
                        ((e1 & CHECK) == OK) & ((e2 & CHECK) == OK) &
                        ((e3 & CHECK) == OK), 1)) {
                    const uint32_t pack4 =
                        (uint32_t)(uint8_t)e0 | ((uint32_t)(uint8_t)e1 <<  8) |
                        ((uint32_t)(uint8_t)e2 << 16) | ((uint32_t)(uint8_t)e3 << 24);
                    std::memcpy(out, &pack4, 4);
                    out += 4;
                    acc_consume(bits_lo, bits_hi, bit_cnt, 4 * ebits0);
                    if (__builtin_expect(out > safe_out_end, 0)) goto done;
                    if (__builtin_expect(bit_cnt >= MIN_DECODE_BITS, 1))
                        goto decode_symbol;
                    continue;
                }
            }
#endif /* DEFLATE_HAS_NEON / DEFLATE_HAS_SSE2 */

            /* Scalar path: consume e0 and handle it alone. */
            acc_consume(bits_lo, bits_hi, bit_cnt, ebits0);

            if (__builtin_expect(e0 & HUFF_LITERAL_FLAG, 1)) {
                *out++ = static_cast<uint8_t>(e0);
                if (__builtin_expect(out > safe_out_end, 0)) goto done;
                if (__builtin_expect(bit_cnt >= MIN_DECODE_BITS, 1))
                    goto decode_symbol;
                continue;
            }

            {
                const int sym = static_cast<int>(e0 & 0xFFFF);

                if (sym == 256) {
                    ended = true;
                    goto done;
                }

                const int li        = sym - 257;
                int       match_len = LENGTH_BASE[li];
                const int len_extra = LENGTH_EXTRA_BITS[li];
                if (len_extra > 0) {
                    match_len += static_cast<int>(
                        static_cast<uint32_t>(bits_lo) & ((1u << len_extra) - 1));
                    acc_consume(bits_lo, bits_hi, bit_cnt, len_extra);
                }

                /* Decode distance symbol */
                uint32_t dentry = tables.dist[
                    static_cast<uint32_t>(bits_lo) & ((1u << DIST_DECODE_BITS) - 1)];
                if (__builtin_expect(dentry & HUFF_SUBTABLE_FLAG, 0)) {
                    const int sec_bits   = static_cast<int>((dentry >> 16) & 0xFF);
                    const int sec_offset = static_cast<int>(dentry & 0xFFFF);
                    dentry = tables.dist[sec_offset +
                        ((static_cast<uint32_t>(bits_lo) >> DIST_DECODE_BITS) &
                         ((1u << sec_bits) - 1))];
                }

                const int di    = static_cast<int>(dentry & 0xFFFF);
                const int dbits = static_cast<int>((dentry >> 16) & 0xFF);
                acc_consume(bits_lo, bits_hi, bit_cnt, dbits);

                int dist = DIST_BASE[di];
                const int dist_extra = DIST_EXTRA_BITS[di];
                if (dist_extra > 0) {
                    dist += static_cast<int>(
                        static_cast<uint32_t>(bits_lo) & ((1u << dist_extra) - 1));
                    acc_consume(bits_lo, bits_hi, bit_cnt, dist_extra);
                }

                if (out + match_len > out_end) goto done;
                if (static_cast<ptrdiff_t>(dist) > out - out_buf) goto done;

                copy_match(out, static_cast<size_t>(dist), static_cast<size_t>(match_len));
                out += match_len;

                /* After match, bit_cnt may still be >> 63 (from 128-bit accumulator).
                 * Falling through to the for-loop top with bit_cnt > 63 causes Load 1
                 * to execute `bits_lo |= w << bit_cnt` with UB shift (ARM64: wrong).
                 * Skip Load 1 if we still have enough bits for another full decode. */
                if (__builtin_expect(out > safe_out_end, 0)) goto done;
                if (__builtin_expect(bit_cnt >= MIN_DECODE_BITS, 1)) goto decode_symbol;
            }
        }

        if (out > safe_out_end || src + 8 > src_end)
            goto done;
    }

done:
    /*
     * Compact 128-bit accumulator back to ≤56 bits for BitReader restore.
     *
     * If bit_cnt > 56: bits [56..bit_cnt-1] were pre-loaded but not consumed.
     * Rewind src by ceil((bit_cnt-56)/8) bytes so the slow path re-reads them.
     * Then bit_cnt' = bit_cnt - rewind_bytes*8 is in [48..56].
     *
     * bits_lo[0..bit_cnt'-1] retain the correct stream bits (the low bits of the
     * 128-bit accumulator after all consume operations).  bits [bit_cnt'..63] of
     * bits_lo hold overflow from the load ops — they match the bytes that will be
     * reloaded from the rewound src, so FILL_BITS_MAX's OR is idempotent.
     */
    if (bit_cnt > 56) {
        const int excess      = bit_cnt - 56;
        const int rewind_bytes = (excess + 7) >> 3;
        src     -= rewind_bytes;
        bit_cnt -= rewind_bytes << 3;
    }
    /* Clamp to 64-bit BitReader range (bit_cnt should be ≤56 after above) */
    if (bit_cnt > 63) bit_cnt = 63;
    if (bit_cnt < 0)  bit_cnt = 0;

    br.restore(src, bits_lo, bit_cnt);
    out_ptr = out;
    return ended;
}

} } /* namespace orot::deflate */
