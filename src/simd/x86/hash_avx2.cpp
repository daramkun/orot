#if defined(DEFLATE_HAS_AVX2)

#include "../simd_dispatch.hpp"

#include <immintrin.h>  /* AVX2 */
#include <cstring>

namespace orot { namespace deflate {

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
    /* Need data[i..i+19] for the two 128-bit loads; scalar tail covers the rest. */
    for (; i + 20 <= len; i += 8) {
        /* Load two overlapping 128-bit chunks covering data[i..i+10].
         * Use SSSE3 _mm_alignr_epi8 to produce 8 consecutive 4-byte inputs
         * instead of 8 individual scalar loads via _mm256_set_epi32. */
        const __m128i c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
        const __m128i c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i + 4));

        /* 8 overlapping 4-byte windows via byte-aligned shifts */
        const __m128i a0 = c0;
        const __m128i a1 = _mm_alignr_epi8(c1, c0, 1);
        const __m128i a2 = _mm_alignr_epi8(c1, c0, 2);
        const __m128i a3 = _mm_alignr_epi8(c1, c0, 3);
        const __m128i a4 = c1;
        const __m128i a5 = _mm_alignr_epi8(c1, c1, 1);
        const __m128i a6 = _mm_alignr_epi8(c1, c1, 2);
        const __m128i a7 = _mm_alignr_epi8(c1, c1, 3);

        /* Pack low 32-bit lanes into 256-bit vector */
        const __m128i lo128 = _mm_unpacklo_epi64(
            _mm_unpacklo_epi32(a0, a1), _mm_unpacklo_epi32(a2, a3));
        const __m128i hi128 = _mm_unpacklo_epi64(
            _mm_unpacklo_epi32(a4, a5), _mm_unpacklo_epi32(a6, a7));
        const __m256i vals  = _mm256_set_m128i(hi128, lo128);

        __m256i hashes = _mm256_and_si256(
            _mm256_srli_epi32(_mm256_mullo_epi32(vals, MULT), shift),
            MASK);

        /* Store hash results; prefetch head[] entries before reading */
        alignas(32) uint32_t h[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(h), hashes);

        for (int j = 0; j < 8; ++j)
            __builtin_prefetch(&head[h[j]], 0, 1);

        /* Read old head values, write prev[] as contiguous 16-byte store */
        alignas(16) uint16_t old_head[8];
        for (int j = 0; j < 8; ++j)
            old_head[j] = head[h[j]];

        const uint16_t base_pos = static_cast<uint16_t>(i & win_mask);
        if (base_pos + 8 <= static_cast<uint16_t>(win_mask + 1)) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(&prev[base_pos]),
                _mm_load_si128(reinterpret_cast<const __m128i*>(old_head)));
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
            /* Cross-block contribution: each of the 32 new bytes adds vs1 once to s2
             * (equivalent to: s2 += old_s1 for each of the 32 bytes, before byte is added). */
            vs2 = _mm256_add_epi32(vs2, _mm256_slli_epi32(vs1, 5));  /* vs2 += vs1 * 32 */

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

} } /* namespace orot::deflate */

#endif /* DEFLATE_HAS_AVX2 */
