#include "../simd_dispatch.hpp"

#if defined(DEFLATE_HAS_AVX2)

#include <immintrin.h>  /* AVX2 */
#include <cstring>

namespace deflate {

/*
 * AVX2 bulk hash insert.
 * Process 8 positions per iteration using _mm256_mullo_epi32.
 */
void avx2_hash_insert_bulk(
    const uint8_t* data, size_t len,
    uint16_t* head, uint16_t* prev, int hash_bits)
{
    if (len < 4) return;
    const uint32_t hash_mask = (1U << hash_bits) - 1;
    const size_t   win_mask  = 0x7FFF;
    const int      shift     = 32 - hash_bits;
    const size_t   limit     = len - 3;

    const __m256i MULT = _mm256_set1_epi32(static_cast<int>(0x1E35A7BDU));
    const __m256i MASK = _mm256_set1_epi32(static_cast<int>(hash_mask));

    size_t i = 0;
    for (; i + 8 <= limit; i += 8) {
        /* Load 8 4-byte values at consecutive positions */
        __m256i vals = _mm256_set_epi32(
            *reinterpret_cast<const int*>(data + i + 7),
            *reinterpret_cast<const int*>(data + i + 6),
            *reinterpret_cast<const int*>(data + i + 5),
            *reinterpret_cast<const int*>(data + i + 4),
            *reinterpret_cast<const int*>(data + i + 3),
            *reinterpret_cast<const int*>(data + i + 2),
            *reinterpret_cast<const int*>(data + i + 1),
            *reinterpret_cast<const int*>(data + i)
        );

        __m256i hashes = _mm256_and_si256(
            _mm256_srli_epi32(_mm256_mullo_epi32(vals, MULT), shift),
            MASK);

        /* Store results and insert into hash chains (scalar loop) */
        alignas(32) uint32_t h[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(h), hashes);

        for (int j = 0; j < 8; ++j) {
            const uint16_t pos = static_cast<uint16_t>((i + static_cast<size_t>(j)) & win_mask);
            prev[pos]  = head[h[j]];
            head[h[j]] = pos;
        }
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

/*
 * AVX2 Adler-32.
 * Process 32 bytes per iteration.
 */
uint32_t avx2_adler32(uint32_t adler, const uint8_t* data, size_t len) {
    static constexpr uint32_t MOD_ADLER = 65521;
    static constexpr size_t   BLOCK     = 5536;  /* ~5552, multiple of 32 */

    uint32_t s1 = adler & 0xFFFF;
    uint32_t s2 = (adler >> 16) & 0xFFFF;

    while (len > 0) {
        const size_t chunk = (len < BLOCK) ? len : BLOCK;
        const uint8_t* end = data + chunk;

        /* Weights: 32,31,...,1 for bytes within each 32-byte vector */
        const __m256i WEIGHTS = _mm256_set_epi8(
            1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
            17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32);

        __m256i vs1 = _mm256_setzero_si256();
        __m256i vs2 = _mm256_setzero_si256();
        const uint8_t* p = data;

        while (p + 32 <= end) {
            __m256i bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            /* Unpack to 16-bit to avoid saturation */
            __m256i lo = _mm256_unpacklo_epi8(bytes, _mm256_setzero_si256());
            __m256i hi = _mm256_unpackhi_epi8(bytes, _mm256_setzero_si256());
            vs1 = _mm256_add_epi32(vs1, _mm256_madd_epi16(
                _mm256_set1_epi16(1), _mm256_add_epi16(lo, hi)));

            __m256i wlo = _mm256_unpacklo_epi8(WEIGHTS, _mm256_setzero_si256());
            __m256i whi = _mm256_unpackhi_epi8(WEIGHTS, _mm256_setzero_si256());
            vs2 = _mm256_add_epi32(vs2,
                _mm256_add_epi32(
                    _mm256_madd_epi16(lo, wlo),
                    _mm256_madd_epi16(hi, whi)));
            p += 32;
        }

        /* Horizontal sum of SIMD accumulators */
        alignas(32) uint32_t sv1[8], sv2[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(sv1), vs1);
        _mm256_store_si256(reinterpret_cast<__m256i*>(sv2), vs2);

        s2 += s1 * static_cast<uint32_t>(p - data);
        for (int j = 0; j < 8; ++j) { s1 += sv1[j]; s2 += sv2[j]; }

        /* Scalar tail */
        while (p < end) {
            s1 += *p++;
            s2 += s1;
        }

        s1 %= MOD_ADLER;
        s2 %= MOD_ADLER;
        data += chunk;
        len  -= chunk;
    }
    return (s2 << 16) | s1;
}

} /* namespace deflate */

#endif /* DEFLATE_HAS_AVX2 */
