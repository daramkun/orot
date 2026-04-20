/* arm_crc32: hardware CRC requires DEFLATE_HAS_CRC_ARM */
#if defined(DEFLATE_HAS_CRC_ARM)

#include "../simd_dispatch.hpp"

#include <arm_acle.h>
#include <cstring>

namespace orot { namespace deflate {

uint32_t arm_crc32(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;

    while (len >= 32) {
        uint64_t v0, v1, v2, v3;
        std::memcpy(&v0, data,      8);
        std::memcpy(&v1, data +  8, 8);
        std::memcpy(&v2, data + 16, 8);
        std::memcpy(&v3, data + 24, 8);
        crc  = __crc32d(crc, v0);
        crc  = __crc32d(crc, v1);
        crc  = __crc32d(crc, v2);
        crc  = __crc32d(crc, v3);
        data += 32;
        len  -= 32;
    }
    while (len >= 8) {
        uint64_t v;
        std::memcpy(&v, data, 8);
        crc  = __crc32d(crc, v);
        data += 8;
        len  -= 8;
    }
    if (len >= 4) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        crc  = __crc32w(crc, v);
        data += 4;
        len  -= 4;
    }
    if (len >= 2) {
        uint16_t v;
        std::memcpy(&v, data, 2);
        crc  = __crc32h(crc, v);
        data += 2;
        len  -= 2;
    }
    if (len >= 1) crc = __crc32b(crc, *data);

    return ~crc;
}

} } /* namespace orot::deflate */

#endif /* DEFLATE_HAS_CRC_ARM */

/* neon_adler32: NEON vectorized Adler-32 */
#if defined(DEFLATE_HAS_NEON)

#include <arm_neon.h>
#include <cstring>

namespace orot { namespace deflate {

uint32_t neon_adler32(uint32_t adler, const uint8_t* data, size_t len) {
    static constexpr uint32_t MOD_ADLER = 65521;
    static constexpr size_t BLOCK = 5504;  /* multiple of 64 */

    uint32_t s1 = adler & 0xFFFF;
    uint32_t s2 = (adler >> 16) & 0xFFFF;

    /*
     * 64B/iter 5-accumulator design: breaks the 9-step serial vs2 recurrence.
     *
     * Old: single vs2 with 9 sequential vaddq_u32 per iter → 18-cycle recurrence.
     * New: vs2_c (carry) + vs2_h0..h3 (one per 16-byte group) — 5 independent
     *      recurrence chains, each ≤2 steps → 4-cycle recurrence per chain.
     *
     * Critical path: max(vs1 chain 8 cycles, vs2_c chain 8 cycles) = 8 cycles/iter
     * vs 26 cycles before → ~3× throughput improvement for adler32.
     */
    static const uint8_t w64_hi_arr[16] = {
        64,63,62,61,60,59,58,57,56,55,54,53,52,51,50,49
    };
    static const uint8_t w64_lo_arr[16] = {
        48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33
    };
    static const uint8_t w32_hi_arr[16] = {
        32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17
    };
    static const uint8_t w32_lo_arr[16] = {
        16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1
    };

    while (len > 0) {
        const size_t chunk = (len < BLOCK) ? len : BLOCK;
        const uint8_t* end = data + chunk;

        const uint8x16_t W64_HI = vld1q_u8(w64_hi_arr);
        const uint8x16_t W64_LO = vld1q_u8(w64_lo_arr);
        const uint8x16_t W32_HI = vld1q_u8(w32_hi_arr);
        const uint8x16_t W32_LO = vld1q_u8(w32_lo_arr);
        uint32x4_t vs1 = vdupq_n_u32(0);
        uint32x4_t vs2 = vdupq_n_u32(0);
        const uint8_t* p = data;

        /* 64B main loop: 5 independent vs2 accumulators break the serial chain.
         * vs2_c: carry (vs1*64), vs2_h0..h3: per-group weighted sums.
         * Each has a 2-step recurrence (4 cycles) vs the old 9-step (18 cycles). */
        {
            uint32x4_t vs2_c  = vdupq_n_u32(0);
            uint32x4_t vs2_h0 = vdupq_n_u32(0);
            uint32x4_t vs2_h1 = vdupq_n_u32(0);
            uint32x4_t vs2_h2 = vdupq_n_u32(0);
            uint32x4_t vs2_h3 = vdupq_n_u32(0);
            while (p + 64 <= end) {
                vs2_c = vaddq_u32(vs2_c, vshlq_n_u32(vs1, 6));  /* carry: vs2_c += vs1*64 */

                uint8x16_t b0 = vld1q_u8(p);      /* weights [64..49] */
                uint8x16_t b1 = vld1q_u8(p + 16); /* weights [48..33] */
                uint8x16_t b2 = vld1q_u8(p + 32); /* weights [32..17] */
                uint8x16_t b3 = vld1q_u8(p + 48); /* weights [16..1]  */

                /* s1: sum all 64 bytes (4-step serial chain) */
                uint16x8_t s16_0 = vaddl_u8(vget_low_u8(b0), vget_high_u8(b0));
                uint16x8_t s16_1 = vaddl_u8(vget_low_u8(b1), vget_high_u8(b1));
                uint16x8_t s16_2 = vaddl_u8(vget_low_u8(b2), vget_high_u8(b2));
                uint16x8_t s16_3 = vaddl_u8(vget_low_u8(b3), vget_high_u8(b3));
                vs1 = vaddq_u32(vs1, vaddl_u16(vget_low_u16(s16_0), vget_high_u16(s16_0)));
                vs1 = vaddq_u32(vs1, vaddl_u16(vget_low_u16(s16_1), vget_high_u16(s16_1)));
                vs1 = vaddq_u32(vs1, vaddl_u16(vget_low_u16(s16_2), vget_high_u16(s16_2)));
                vs1 = vaddq_u32(vs1, vaddl_u16(vget_low_u16(s16_3), vget_high_u16(s16_3)));

                /* s2: each group accumulates into its own independent register chain */
                uint16x8_t h0l = vmull_u8(vget_low_u8(b0),  vget_low_u8(W64_HI));
                uint16x8_t h0h = vmull_u8(vget_high_u8(b0), vget_high_u8(W64_HI));
                vs2_h0 = vaddq_u32(vs2_h0, vaddl_u16(vget_low_u16(h0l), vget_high_u16(h0l)));
                vs2_h0 = vaddq_u32(vs2_h0, vaddl_u16(vget_low_u16(h0h), vget_high_u16(h0h)));

                uint16x8_t h1l = vmull_u8(vget_low_u8(b1),  vget_low_u8(W64_LO));
                uint16x8_t h1h = vmull_u8(vget_high_u8(b1), vget_high_u8(W64_LO));
                vs2_h1 = vaddq_u32(vs2_h1, vaddl_u16(vget_low_u16(h1l), vget_high_u16(h1l)));
                vs2_h1 = vaddq_u32(vs2_h1, vaddl_u16(vget_low_u16(h1h), vget_high_u16(h1h)));

                uint16x8_t h2l = vmull_u8(vget_low_u8(b2),  vget_low_u8(W32_HI));
                uint16x8_t h2h = vmull_u8(vget_high_u8(b2), vget_high_u8(W32_HI));
                vs2_h2 = vaddq_u32(vs2_h2, vaddl_u16(vget_low_u16(h2l), vget_high_u16(h2l)));
                vs2_h2 = vaddq_u32(vs2_h2, vaddl_u16(vget_low_u16(h2h), vget_high_u16(h2h)));

                uint16x8_t h3l = vmull_u8(vget_low_u8(b3),  vget_low_u8(W32_LO));
                uint16x8_t h3h = vmull_u8(vget_high_u8(b3), vget_high_u8(W32_LO));
                vs2_h3 = vaddq_u32(vs2_h3, vaddl_u16(vget_low_u16(h3l), vget_high_u16(h3l)));
                vs2_h3 = vaddq_u32(vs2_h3, vaddl_u16(vget_low_u16(h3h), vget_high_u16(h3h)));

                p += 64;
            }
            /* Combine 5 independent chains into vs2 */
            vs2 = vaddq_u32(
                vaddq_u32(vs2_c, vs2_h0),
                vaddq_u32(vaddq_u32(vs2_h1, vs2_h2), vs2_h3));
        }

        /* 32B tail within chunk */
        while (p + 32 <= end) {
            vs2 = vaddq_u32(vs2, vshlq_n_u32(vs1, 5));  /* vs2 += vs1 * 32 */

            uint8x16_t b0 = vld1q_u8(p);
            uint8x16_t b1 = vld1q_u8(p + 16);

            uint16x8_t s16_0 = vaddl_u8(vget_low_u8(b0), vget_high_u8(b0));
            uint16x8_t s16_1 = vaddl_u8(vget_low_u8(b1), vget_high_u8(b1));
            vs1 = vaddq_u32(vs1, vaddl_u16(vget_low_u16(s16_0), vget_high_u16(s16_0)));
            vs1 = vaddq_u32(vs1, vaddl_u16(vget_low_u16(s16_1), vget_high_u16(s16_1)));

            uint16x8_t h0l = vmull_u8(vget_low_u8(b0),  vget_low_u8(W32_HI));
            uint16x8_t h0h = vmull_u8(vget_high_u8(b0), vget_high_u8(W32_HI));
            vs2 = vaddq_u32(vs2, vaddl_u16(vget_low_u16(h0l), vget_high_u16(h0l)));
            vs2 = vaddq_u32(vs2, vaddl_u16(vget_low_u16(h0h), vget_high_u16(h0h)));

            uint16x8_t h1l = vmull_u8(vget_low_u8(b1),  vget_low_u8(W32_LO));
            uint16x8_t h1h = vmull_u8(vget_high_u8(b1), vget_high_u8(W32_LO));
            vs2 = vaddq_u32(vs2, vaddl_u16(vget_low_u16(h1l), vget_high_u16(h1l)));
            vs2 = vaddq_u32(vs2, vaddl_u16(vget_low_u16(h1h), vget_high_u16(h1h)));

            p += 32;
        }

        /* 16B tail within chunk */
        while (p + 16 <= end) {
            vs2 = vaddq_u32(vs2, vshlq_n_u32(vs1, 4));
            uint8x16_t bytes = vld1q_u8(p);
            uint16x8_t lo = vmull_u8(vget_low_u8(bytes),  vget_low_u8(W32_LO));
            uint16x8_t hi = vmull_u8(vget_high_u8(bytes), vget_high_u8(W32_LO));
            vs2 = vaddq_u32(vs2, vaddl_u16(vget_low_u16(lo), vget_high_u16(lo)));
            vs2 = vaddq_u32(vs2, vaddl_u16(vget_low_u16(hi), vget_high_u16(hi)));
            uint16x8_t sum16 = vaddl_u8(vget_low_u8(bytes), vget_high_u8(bytes));
            vs1 = vaddq_u32(vs1, vaddl_u16(vget_low_u16(sum16), vget_high_u16(sum16)));
            p += 16;
        }

        s2 += s1 * static_cast<uint32_t>(p - data);
        s1 += vaddvq_u32(vs1);
        s2 += vaddvq_u32(vs2);

        while (p < end) { s1 += *p++; s2 += s1; }

        s1 %= MOD_ADLER;
        s2 %= MOD_ADLER;
        data += chunk;
        len  -= chunk;
    }
    return (s2 << 16) | s1;
}

} } /* namespace orot::deflate */

#endif /* DEFLATE_HAS_NEON */
