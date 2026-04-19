#if defined(DEFLATE_HAS_NEON)

#include "../simd_dispatch.hpp"

#include <arm_neon.h>
#include <cstring>

namespace orot { namespace deflate {

/*
 * NEON bulk hash insert — 16 positions per iteration.
 *
 * Four 128-bit loads: a=data[i], b=data[i+4], c=data[i+8], d=data[i+12].
 * Group 1: vextq(a, b, 0..7) → 4-byte windows at positions i+0..i+7.
 * Group 2: vextq(c, d, 0..7) → 4-byte windows at positions i+8..i+15.
 * Covers data[i..i+18], loop guard: i+28 <= len.
 */
void neon_hash_insert_bulk(
    const uint8_t* data, size_t len,
    uint16_t* head, uint16_t* prev, int hash_bits)
{
    if (len < 4) return;
    const uint32_t hash_mask = (1U << hash_bits) - 1;
    const int      shift     = 32 - hash_bits;
    const size_t   win_mask  = 0x7FFF;
    const size_t   limit     = len - 3;

    const uint32x4_t MULT = vdupq_n_u32(0x1E35A7BDU);

    size_t i = 0;

    /* 16 positions per iteration; need data[i..i+27] (28 bytes). */
    for (; i + 28 <= len; i += 16) {
        const uint8x16_t a = vld1q_u8(data + i);        /* data[i..i+15]    */
        const uint8x16_t b = vld1q_u8(data + i +  4);   /* data[i+4..i+19]  */
        const uint8x16_t c = vld1q_u8(data + i +  8);   /* data[i+8..i+23]  */
        const uint8x16_t d = vld1q_u8(data + i + 12);   /* data[i+12..i+27] */

        /* Group 1: positions i+0..i+7 */
        const uint32x4_t g1v0 = vreinterpretq_u32_u8(a);
        const uint32x4_t g1v1 = vreinterpretq_u32_u8(vextq_u8(a, b, 1));
        const uint32x4_t g1v2 = vreinterpretq_u32_u8(vextq_u8(a, b, 2));
        const uint32x4_t g1v3 = vreinterpretq_u32_u8(vextq_u8(a, b, 3));
        const uint32x4_t g1v4 = vreinterpretq_u32_u8(b);
        const uint32x4_t g1v5 = vreinterpretq_u32_u8(vextq_u8(b, c, 1));
        const uint32x4_t g1v6 = vreinterpretq_u32_u8(vextq_u8(b, c, 2));
        const uint32x4_t g1v7 = vreinterpretq_u32_u8(vextq_u8(b, c, 3));

        /* Group 2: positions i+8..i+15 */
        const uint32x4_t g2v0 = vreinterpretq_u32_u8(c);
        const uint32x4_t g2v1 = vreinterpretq_u32_u8(vextq_u8(c, d, 1));
        const uint32x4_t g2v2 = vreinterpretq_u32_u8(vextq_u8(c, d, 2));
        const uint32x4_t g2v3 = vreinterpretq_u32_u8(vextq_u8(c, d, 3));
        const uint32x4_t g2v4 = vreinterpretq_u32_u8(d);
        const uint32x4_t g2v5 = vreinterpretq_u32_u8(vextq_u8(d, d, 1));
        const uint32x4_t g2v6 = vreinterpretq_u32_u8(vextq_u8(d, d, 2));
        const uint32x4_t g2v7 = vreinterpretq_u32_u8(vextq_u8(d, d, 3));

        /* Pack lane 0 of each v* into four uint32x4_t for vectorized multiply */
        const uint32x2_t p01 = vzip1_u32(vget_low_u32(g1v0), vget_low_u32(g1v1));
        const uint32x2_t p23 = vzip1_u32(vget_low_u32(g1v2), vget_low_u32(g1v3));
        const uint32x2_t p45 = vzip1_u32(vget_low_u32(g1v4), vget_low_u32(g1v5));
        const uint32x2_t p67 = vzip1_u32(vget_low_u32(g1v6), vget_low_u32(g1v7));

        const uint32x2_t q01 = vzip1_u32(vget_low_u32(g2v0), vget_low_u32(g2v1));
        const uint32x2_t q23 = vzip1_u32(vget_low_u32(g2v2), vget_low_u32(g2v3));
        const uint32x2_t q45 = vzip1_u32(vget_low_u32(g2v4), vget_low_u32(g2v5));
        const uint32x2_t q67 = vzip1_u32(vget_low_u32(g2v6), vget_low_u32(g2v7));

        const uint32x4_t lo4a = vcombine_u32(p01, p23);
        const uint32x4_t lo4b = vcombine_u32(p45, p67);
        const uint32x4_t hi4a = vcombine_u32(q01, q23);
        const uint32x4_t hi4b = vcombine_u32(q45, q67);

        /* Multiply and extract all 16 hashes */
        alignas(16) uint32_t h[16];
        vst1q_u32(h,      vmulq_u32(lo4a, MULT));
        vst1q_u32(h +  4, vmulq_u32(lo4b, MULT));
        vst1q_u32(h +  8, vmulq_u32(hi4a, MULT));
        vst1q_u32(h + 12, vmulq_u32(hi4b, MULT));
        for (int j = 0; j < 16; ++j)
            h[j] = (h[j] >> shift) & hash_mask;

        for (int j = 0; j < 16; ++j)
            __builtin_prefetch(&head[h[j]], 0, 0);

        alignas(16) uint16_t old_head[16];
        for (int j = 0; j < 16; ++j)
            old_head[j] = head[h[j]];

        const uint16_t base_pos = static_cast<uint16_t>(i & win_mask);
        if (base_pos + 16 <= static_cast<uint16_t>(win_mask + 1)) {
            vst1q_u16(&prev[base_pos],     vld1q_u16(old_head));
            vst1q_u16(&prev[base_pos + 8], vld1q_u16(old_head + 8));
        } else {
            for (int j = 0; j < 16; ++j)
                prev[(base_pos + j) & win_mask] = old_head[j];
        }

        for (int j = 0; j < 16; ++j)
            head[h[j]] = static_cast<uint16_t>((i + static_cast<size_t>(j)) & win_mask);
    }

    /* Scalar tail */
    for (; i < limit; ++i) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        const uint32_t hv  = ((v * 0x1E35A7BDU) >> shift) & hash_mask;
        const uint16_t pos = static_cast<uint16_t>(i & win_mask);
        prev[pos] = head[hv];
        head[hv]  = pos;
    }
}

} } /* namespace orot::deflate */

#endif /* DEFLATE_HAS_NEON */
