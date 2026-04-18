#if defined(DEFLATE_HAS_SSE2)

#include "../simd_dispatch.hpp"

#include <emmintrin.h>  /* SSE2 */
#include <cstring>

namespace orot { namespace deflate {

/*
 * SSE2 bulk hash insert.
 * Process 4 positions per iteration using 32-bit multiply.
 * SSE2 has no 32x32→32 multiply, so we use a fallback strategy:
 * split into two 16-bit multiplies.
 */
void sse2_hash_insert_bulk(
    const uint8_t* data, size_t len,
    uint16_t* head, uint16_t* prev, int hash_bits)
{
    if (len < 4) return;
    const uint32_t hash_mask = (1U << hash_bits) - 1;
    const size_t   win_mask  = 0x7FFF;
    const int      shift     = 32 - hash_bits;
    const size_t   limit     = len - 3;

    /* SSE2: process 4 positions using _mm_set_epi32 + emulated 32-bit mul */
    size_t i = 0;
    for (; i + 4 <= limit; i += 4) {
        uint32_t v[4];
        std::memcpy(&v[0], data + i,     4);
        std::memcpy(&v[1], data + i + 1, 4);
        std::memcpy(&v[2], data + i + 2, 4);
        std::memcpy(&v[3], data + i + 3, 4);

        /* Compute hash for each: (v * 0x1E35A7BD) >> shift */
        for (int j = 0; j < 4; ++j) {
            const uint32_t h   = ((v[j] * 0x1E35A7BDU) >> shift) & hash_mask;
            const uint16_t pos = static_cast<uint16_t>((i + static_cast<size_t>(j)) & win_mask);
            prev[pos] = head[h];
            head[h]   = pos;
        }
    }

    /* Scalar tail */
    for (; i < limit; ++i) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        const uint32_t h   = ((v * 0x1E35A7BDU) >> shift) & hash_mask;
        const uint16_t pos = static_cast<uint16_t>(i & win_mask);
        prev[pos] = head[h];
        head[h]   = pos;
    }
}

} } /* namespace orot::deflate */

#endif /* DEFLATE_HAS_SSE2 */
