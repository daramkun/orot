#if defined(DEFLATE_HAS_NEON)

#include "../simd_dispatch.hpp"

#include <arm_neon.h>
#include <cstring>

namespace orot { namespace deflate {

/*
 * NEON bulk hash insert.
 * Process 8 positions per iteration — mirrors AVX2 alignr+256-bit approach.
 * Two 128-bit loads + vextq_u8 shifts produce 8 overlapping 4-byte windows.
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

    /* Process 8 positions at a time; need data[i..i+19] (20 bytes). */
    for (; i + 20 <= len; i += 8) {
        /* Two 128-bit loads covering data[i..i+19].
         * c0 = data[i..i+15], c1 = data[i+4..i+19].
         * vextq_u8(a, b, n) → bytes [a[n..15], b[0..n-1]].
         * Lane 0 of each reinterpreted uint32x4_t is the desired 4-byte input. */
        const uint8x16_t c0 = vld1q_u8(data + i);
        const uint8x16_t c1 = vld1q_u8(data + i + 4);

        const uint32x4_t vals0 = vreinterpretq_u32_u8(c0);
        const uint32x4_t vals1 = vreinterpretq_u32_u8(vextq_u8(c0, c1, 1));
        const uint32x4_t vals2 = vreinterpretq_u32_u8(vextq_u8(c0, c1, 2));
        const uint32x4_t vals3 = vreinterpretq_u32_u8(vextq_u8(c0, c1, 3));
        const uint32x4_t vals4 = vreinterpretq_u32_u8(c1);
        const uint32x4_t vals5 = vreinterpretq_u32_u8(vextq_u8(c1, c1, 1));
        const uint32x4_t vals6 = vreinterpretq_u32_u8(vextq_u8(c1, c1, 2));
        const uint32x4_t vals7 = vreinterpretq_u32_u8(vextq_u8(c1, c1, 3));

        /* Pack lane 0 of each vals* into two uint32x4_t for vectorized multiply. */
        const uint32x2_t p01 = vzip1_u32(vget_low_u32(vals0), vget_low_u32(vals1));
        const uint32x2_t p23 = vzip1_u32(vget_low_u32(vals2), vget_low_u32(vals3));
        const uint32x4_t lo4 = vcombine_u32(p01, p23);
        const uint32x2_t p45 = vzip1_u32(vget_low_u32(vals4), vget_low_u32(vals5));
        const uint32x2_t p67 = vzip1_u32(vget_low_u32(vals6), vget_low_u32(vals7));
        const uint32x4_t hi4 = vcombine_u32(p45, p67);

        /* Multiply and extract all 8 hashes to stack; apply variable shift+mask. */
        alignas(16) uint32_t h[8];
        vst1q_u32(h,     vmulq_u32(lo4, MULT));
        vst1q_u32(h + 4, vmulq_u32(hi4, MULT));
        for (int j = 0; j < 8; ++j)
            h[j] = (h[j] >> shift) & hash_mask;

#if !defined(__APPLE__)
        for (int j = 0; j < 8; ++j)
            __builtin_prefetch(&head[h[j]], 0, 1);
#endif

        /* Read old head values into aligned buffer for contiguous prev[] store. */
        alignas(16) uint16_t old_head[8];
        for (int j = 0; j < 8; ++j)
            old_head[j] = head[h[j]];

        const uint16_t base_pos = static_cast<uint16_t>(i & win_mask);
        if (base_pos + 8 <= static_cast<uint16_t>(win_mask + 1)) {
            vst1q_u16(&prev[base_pos], vld1q_u16(old_head));
        } else {
            for (int j = 0; j < 8; ++j)
                prev[(base_pos + j) & win_mask] = old_head[j];
        }

        for (int j = 0; j < 8; ++j)
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
