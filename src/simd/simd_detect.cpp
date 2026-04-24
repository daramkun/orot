#include "simd_dispatch.hpp"
#include "../deflate/lz77.hpp"  /* for match_length_scalar */

#include <cstring>

namespace orot { namespace deflate {

/* =========================================================================
 * Scalar fallbacks
 * ========================================================================= */

int scalar_match_length(const uint8_t* a, const uint8_t* b, int max_len) {
    return match_length_scalar(a, b, max_len);
}

uint32_t scalar_crc32(uint32_t crc, const uint8_t* data, size_t len) {
    /* Sarwate table-driven CRC-32 (IEEE 0xEDB88320) */
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c >> 1) ^ (0xEDB88320U & -(c & 1));
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; ++i)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
    return ~crc;
}

uint32_t scalar_adler32(uint32_t adler, const uint8_t* data, size_t len) {
    uint32_t s1 = adler & 0xFFFF;
    uint32_t s2 = (adler >> 16) & 0xFFFF;
    static constexpr uint32_t MOD_ADLER = 65521;
    static constexpr size_t   NMAX      = 5552;  /* max before overflow */

    while (len > 0) {
        const size_t chunk = (len < NMAX) ? len : NMAX;
        for (size_t i = 0; i < chunk; ++i) {
            s1 += data[i];
            s2 += s1;
        }
        s1 %= MOD_ADLER;
        s2 %= MOD_ADLER;
        data += chunk;
        len  -= chunk;
    }
    return (s2 << 16) | s1;
}

void scalar_hash_insert_bulk(
    const uint8_t* data, size_t len,
    uint16_t* head, uint16_t* prev, int hash_bits)
{
    if (len < 4) return;
    const uint32_t hash_mask = (1U << hash_bits) - 1;
    const size_t   win_mask  = 0x7FFF; /* 32KB - 1 */
    const size_t   limit     = len - 3;
    for (size_t i = 0; i < limit; ++i) {
        uint32_t v;
        std::memcpy(&v, data + i, 4);
        const uint32_t h  = ((v * 0x1E35A7BDU) >> (32 - hash_bits)) & hash_mask;
        const uint16_t pos = static_cast<uint16_t>(i & win_mask);
        prev[pos] = head[h];
        head[h]   = pos;
    }
}

/* =========================================================================
 * Dispatch table initialization
 * ========================================================================= */

static SIMDDispatch build_dispatch() {
    SIMDDispatch d;
    d.match_length    = scalar_match_length;
    d.hash_insert_bulk = scalar_hash_insert_bulk;
    d.crc32           = scalar_crc32;
    d.adler32         = scalar_adler32;

#if defined(DEFLATE_HAS_NEON)
    d.match_length    = neon_match_length;
    d.hash_insert_bulk = neon_hash_insert_bulk;
    d.adler32         = neon_adler32;
#endif
#if defined(DEFLATE_HAS_CRC_ARM)
    d.crc32           = arm_crc32;
#endif

#if defined(DEFLATE_HAS_SSE42)
    d.match_length    = sse42_match_length;
    d.crc32           = sse42_crc32;
#endif
#if defined(DEFLATE_HAS_AVX2)
    d.adler32         = avx2_adler32;
    d.hash_insert_bulk = avx2_hash_insert_bulk;
#elif defined(DEFLATE_HAS_SSE2)
    d.hash_insert_bulk = sse2_hash_insert_bulk;
#endif

    return d;
}

const SIMDDispatch& get_simd_dispatch() {
    static const SIMDDispatch dispatch = build_dispatch();
    return dispatch;
}

} } /* namespace orot::deflate */
