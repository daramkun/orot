#include "../simd_dispatch.hpp"

/* arm_crc32: hardware CRC requires DEFLATE_HAS_CRC_ARM */
#if defined(DEFLATE_HAS_CRC_ARM)

#include <arm_acle.h>
#include <cstring>

namespace deflate {

uint32_t arm_crc32(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;

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

} /* namespace deflate */

#endif /* DEFLATE_HAS_CRC_ARM */

/* neon_adler32: NEON vectorized Adler-32 */
#if defined(DEFLATE_HAS_NEON)

#include <arm_neon.h>
#include <cstring>

namespace deflate {

uint32_t neon_adler32(uint32_t adler, const uint8_t* data, size_t len) {
    static constexpr uint32_t MOD_ADLER = 65521;
    static constexpr size_t BLOCK = 5552;

    uint32_t s1 = adler & 0xFFFF;
    uint32_t s2 = (adler >> 16) & 0xFFFF;

    static const uint8_t weights_arr[16] = {
        16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1
    };

    while (len > 0) {
        const size_t chunk = (len < BLOCK) ? len : BLOCK;
        const uint8_t* end = data + chunk;

        const uint8x16_t WEIGHTS = vld1q_u8(weights_arr);
        uint32x4_t vs1 = vdupq_n_u32(0);
        uint32x4_t vs2 = vdupq_n_u32(0);
        const uint8_t* p = data;

        while (p + 16 <= end) {
            /* Cross-group: vs1 accumulates bytes from all prior groups;
             * each of the 16 new bytes will add those prior bytes to s2.
             * Equivalent to the scalar: s2 += s1 for each new byte (16 times). */
            vs2 = vaddq_u32(vs2, vshlq_n_u32(vs1, 4));  /* vs2 += vs1 * 16 */

            uint8x16_t bytes = vld1q_u8(p);
            /* Within-group weighted s2: byte[i] contributes (16-i) times */
            uint16x8_t lo = vmull_u8(vget_low_u8 (bytes), vget_low_u8 (WEIGHTS));
            uint16x8_t hi = vmull_u8(vget_high_u8(bytes), vget_high_u8(WEIGHTS));
            vs2 = vaddq_u32(vs2, vaddl_u16(vget_low_u16(lo), vget_high_u16(lo)));
            vs2 = vaddq_u32(vs2, vaddl_u16(vget_low_u16(hi), vget_high_u16(hi)));
            /* s1: simple byte sum */
            uint16x8_t sum16 = vaddl_u8(vget_low_u8(bytes), vget_high_u8(bytes));
            vs1 = vaddq_u32(vs1,
                vaddl_u16(vget_low_u16(sum16), vget_high_u16(sum16)));
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

} /* namespace deflate */

#endif /* DEFLATE_HAS_NEON */
