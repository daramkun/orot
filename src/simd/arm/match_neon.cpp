#if defined(DEFLATE_HAS_NEON)

#include "../simd_dispatch.hpp"

#include <arm_neon.h>
#include <cstring>

namespace orot { namespace deflate {

/*
 * NEON match length comparison.
 * Process 32 bytes per iteration (two vceqq_u8 + vandq) to hide vminvq latency.
 * kBits hoisted to a single static array used by both the 16-byte and 8-byte paths.
 */
int neon_match_length(const uint8_t* a, const uint8_t* b, int max_len) {
    int len = 0;

    /* Process 32 bytes per iteration to expose ILP */
    while (len + 32 <= max_len) {
        uint8x16_t va0 = vld1q_u8(a + len);
        uint8x16_t vb0 = vld1q_u8(b + len);
        uint8x16_t va1 = vld1q_u8(a + len + 16);
        uint8x16_t vb1 = vld1q_u8(b + len + 16);
        uint8x16_t eq0 = vceqq_u8(va0, vb0);
        uint8x16_t eq1 = vceqq_u8(va1, vb1);
        uint8x16_t all = vandq_u8(eq0, eq1);

        if (__builtin_expect(vminvq_u8(all) == 0xFF, 1)) {
            len += 32;
            continue;
        }

        /* Mismatch in this 32-byte window: determine which 16-byte half */
        {
            static const uint8_t kBits[8] = {1,2,4,8,16,32,64,128};
            const uint8x8_t bits = vld1_u8(kBits);

            if (__builtin_expect(vminvq_u8(eq0) == 0xFF, 1)) {
                /* Mismatch is in second 16-byte chunk */
                len += 16;
                const uint8x8_t elo = vget_low_u8(eq1);
                const uint8x8_t ehi = vget_high_u8(eq1);
                uint8_t blo = vaddv_u8(vbic_u8(bits, elo));
                uint8_t bhi = vaddv_u8(vbic_u8(bits, ehi));
                if (blo) return len + __builtin_ctz(blo);
                return len + 8 + __builtin_ctz(bhi);
            } else {
                /* Mismatch is in first 16-byte chunk */
                const uint8x8_t elo = vget_low_u8(eq0);
                const uint8x8_t ehi = vget_high_u8(eq0);
                uint8_t blo = vaddv_u8(vbic_u8(bits, elo));
                uint8_t bhi = vaddv_u8(vbic_u8(bits, ehi));
                if (blo) return len + __builtin_ctz(blo);
                return len + 8 + __builtin_ctz(bhi);
            }
        }
    }

    /* Process 16 bytes */
    if (len + 16 <= max_len) {
        uint8x16_t va = vld1q_u8(a + len);
        uint8x16_t vb = vld1q_u8(b + len);
        uint8x16_t eq = vceqq_u8(va, vb);

        if (vminvq_u8(eq) == 0xFF) {
            len += 16;
        } else {
            static const uint8_t kBits[8] = {1,2,4,8,16,32,64,128};
            const uint8x8_t bits  = vld1_u8(kBits);
            const uint8x8_t elo   = vget_low_u8(eq);
            const uint8x8_t ehi   = vget_high_u8(eq);
            uint8_t blo = vaddv_u8(vbic_u8(bits, elo));
            uint8_t bhi = vaddv_u8(vbic_u8(bits, ehi));
            if (blo) return len + __builtin_ctz(blo);
            return len + 8 + __builtin_ctz(bhi);
        }
    }

    /* Process 8 bytes */
    if (len + 8 <= max_len) {
        uint8x8_t va = vld1_u8(a + len);
        uint8x8_t vb = vld1_u8(b + len);
        uint8x8_t eq = vceq_u8(va, vb);
        if (vminv_u8(eq) == 0xFF) {
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
