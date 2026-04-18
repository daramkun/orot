#if defined(DEFLATE_HAS_NEON)

#include "../simd_dispatch.hpp"

#include <arm_neon.h>
#include <cstring>

namespace orot { namespace deflate {

/*
 * NEON match length comparison.
 * Compare 16 bytes at a time using vceqq_u8 + vminvq_u8.
 */
int neon_match_length(const uint8_t* a, const uint8_t* b, int max_len) {
    int len = 0;

    /* Process 16 bytes per iteration */
    while (len + 16 <= max_len) {
        uint8x16_t va = vld1q_u8(a + len);
        uint8x16_t vb = vld1q_u8(b + len);
        uint8x16_t eq = vceqq_u8(va, vb);

        /* Find first mismatch: if all equal, vminvq returns 0xFF */
        uint8_t min_val = vminvq_u8(eq);
        if (min_val == 0xFF) {
            len += 16;
            continue;
        }

        /* Mismatch in this chunk. Use bitmask to find the first differing byte. */
        {
            /* Weights: bit j set for position j within each 8-byte half */
            static const uint8_t kBits[8] = {1,2,4,8,16,32,64,128};
            const uint8x8_t bits  = vld1_u8(kBits);
            const uint8x8_t elo   = vget_low_u8(eq);
            const uint8x8_t ehi   = vget_high_u8(eq);
            /* mismatch bits: 0 where equal, bit-weight where not equal */
            uint8x8_t mlo = vbic_u8(bits, elo);
            uint8x8_t mhi = vbic_u8(bits, ehi);
            /* combine into two bytes: low=first 8, high=second 8 */
            uint8_t blo = vaddv_u8(mlo);
            uint8_t bhi = vaddv_u8(mhi);
            if (blo) return len + __builtin_ctz(blo);
            return len + 8 + __builtin_ctz(bhi);
        }
    }

    /* Process 8 bytes */
    if (len + 8 <= max_len) {
        uint8x8_t va = vld1_u8(a + len);
        uint8x8_t vb = vld1_u8(b + len);
        uint8x8_t eq = vceq_u8(va, vb);
        uint8_t min_val = vminv_u8(eq);
        if (min_val == 0xFF) {
            len += 8;
        } else {
            static const uint8_t kBits[8] = {1,2,4,8,16,32,64,128};
            const uint8x8_t bits = vld1_u8(kBits);
            uint8_t mask = vaddv_u8(vbic_u8(bits, eq));
            return len + __builtin_ctz(mask);
        }
    }

    /* 4-byte chunk before byte-by-byte tail */
    if (len + 4 <= max_len) {
        uint32_t va, vb;
        std::memcpy(&va, a + len, 4);
        std::memcpy(&vb, b + len, 4);
        const uint32_t diff = va ^ vb;
        if (diff == 0) {
            len += 4;
        } else {
#if defined(__GNUC__) || defined(__clang__)
            return len + static_cast<int>(__builtin_ctz(diff) >> 3);
#else
            while (len < max_len && a[len] == b[len]) ++len;
            return len;
#endif
        }
    }
    /* Scalar tail (0-3 bytes) */
    while (len < max_len && a[len] == b[len]) ++len;
    return len;
}

} } /* namespace orot::deflate */

#endif /* DEFLATE_HAS_NEON */
