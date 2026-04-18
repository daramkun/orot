#if defined(DEFLATE_HAS_NEON)

#include "../simd_dispatch.hpp"

#include <arm_neon.h>
#include <cstring>

namespace orot { namespace deflate {

/*
 * NEON bulk hash insert.
 * Process 4 positions per iteration using 32-bit multiply hash.
 */
void neon_hash_insert_bulk(
    const uint8_t* data, size_t len,
    uint16_t* head, uint16_t* prev, int hash_bits)
{
    if (len < 4) return;
    const uint32_t hash_mask = (1U << hash_bits) - 1;
    const size_t   win_mask  = 0x7FFF;
    const size_t   limit     = len - 3;

    /* Multiplicative hash constant */
    static const uint32x4_t MULT = vdupq_n_u32(0x1E35A7BDU);
    /* LZ77_HASH_BITS is always 16, so shift = 32-16 = 16 (must be compile-time constant) */
    static_assert(true, "shift hardcoded to 16 for LZ77_HASH_BITS=16");
    const uint32x4_t MASK = vdupq_n_u32(hash_mask);

    size_t i = 0;

    /* Process 4 positions at a time */
    for (; i + 4 <= limit; i += 4) {
        /* Load 7 bytes (covers 4 starting positions of 4-byte reads) */
        uint32_t v0, v1, v2, v3;
        std::memcpy(&v0, data + i,     4);
        std::memcpy(&v1, data + i + 1, 4);
        std::memcpy(&v2, data + i + 2, 4);
        std::memcpy(&v3, data + i + 3, 4);

        uint32x4_t vals = {v0, v1, v2, v3};
        /* hash = (val * MULT) >> shift */
        uint32x4_t hashes = vshrq_n_u32(vmulq_u32(vals, MULT), 16);
        hashes = vandq_u32(hashes, MASK);

        /* Insert each position into hash chain */
        for (int j = 0; j < 4; ++j) {
            const uint32_t h   = vgetq_lane_u32(hashes, 0);
            const uint16_t pos = static_cast<uint16_t>((i + static_cast<size_t>(j)) & win_mask);
            prev[pos] = head[h];
            head[h]   = pos;
            /* Shift for next lane */
            hashes = vextq_u32(hashes, hashes, 1);
        }
    }

    /* Tail: scalar */
    for (; i < limit; ++i) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        const uint32_t h  = ((v * 0x1E35A7BDU) >> 16) & hash_mask;
        const uint16_t pos = static_cast<uint16_t>(i & win_mask);
        prev[pos] = head[h];
        head[h]   = pos;
    }
}

} } /* namespace orot::deflate */

#endif /* DEFLATE_HAS_NEON */
