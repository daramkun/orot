#include "../simd_dispatch.hpp"

#if defined(DEFLATE_HAS_NEON)

#include <arm_neon.h>
#include <cstring>

namespace deflate {

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

        /* There's a mismatch in this 16-byte chunk.
         * Find which byte using a byte-by-byte scan
         * (only 16 iterations max, branch predictor friendly). */
        for (int j = 0; j < 16; ++j) {
            if (a[len + j] != b[len + j])
                return len + j;
        }
        return len + 16;  /* should not reach */
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
            for (int j = 0; j < 8; ++j) {
                if (a[len + j] != b[len + j])
                    return len + j;
            }
            return len + 8;
        }
    }

    /* Scalar tail */
    while (len < max_len && a[len] == b[len]) ++len;
    return len;
}

} /* namespace deflate */

#endif /* DEFLATE_HAS_NEON */
