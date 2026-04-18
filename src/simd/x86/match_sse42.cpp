#if defined(DEFLATE_HAS_SSE42)

#include "../simd_dispatch.hpp"

#include <nmmintrin.h>  /* SSE4.2 */
#include <cstring>

namespace orot { namespace deflate {

/*
 * SSE4.2 match length using PCMPISTRM / compare 16 bytes at once.
 * We use a simple 16-byte-at-a-time loop with _mm_cmpeq_epi8 + movemask.
 */
int sse42_match_length(const uint8_t* a, const uint8_t* b, int max_len) {
    int len = 0;

    /* 16-byte chunks via SSE4.2 */
    while (len + 16 <= max_len) {
        __m128i va = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + len));
        __m128i vb = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + len));
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(va, vb));
        if (mask == 0xFFFF) {
            len += 16;
            continue;
        }
        /* Find first mismatch bit */
#if defined(__GNUC__) || defined(__clang__)
        return len + __builtin_ctz(~mask);
#else
        int mismatch = 0;
        while ((mask >> mismatch) & 1) ++mismatch;
        return len + mismatch;
#endif
    }

    /* 8-byte chunk */
    if (len + 8 <= max_len) {
        __m128i va = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(a + len));
        __m128i vb = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(b + len));
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(va, vb)) & 0xFF;
        if (mask == 0xFF) {
            len += 8;
        } else {
#if defined(__GNUC__) || defined(__clang__)
            return len + __builtin_ctz(~mask);
#else
            int m = 0;
            while ((mask >> m) & 1) ++m;
            return len + m;
#endif
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

#endif /* DEFLATE_HAS_SSE42 */
