#pragma once

#include <cstddef>
#include <cstdint>

namespace deflate {

/* ── Function pointer types ──────────────────────────────────────────────── */

/** Match length: compare a[] vs b[], return common prefix length (max max_len). */
using MatchLengthFn = int (*)(const uint8_t* a, const uint8_t* b, int max_len);

/** Bulk hash insert: populate head[]/prev[] for input[0..len). */
using HashInsertBulkFn = void (*)(
    const uint8_t* data, size_t len,
    uint16_t* head, uint16_t* prev,
    int hash_bits);

/** Compute CRC-32 (IEEE polynomial). */
using Crc32Fn = uint32_t (*)(uint32_t crc, const uint8_t* data, size_t len);

/** Compute Adler-32. */
using Adler32Fn = uint32_t (*)(uint32_t adler, const uint8_t* data, size_t len);

/* ── Dispatch table ──────────────────────────────────────────────────────── */

struct SIMDDispatch {
    MatchLengthFn   match_length;
    HashInsertBulkFn hash_insert_bulk;
    Crc32Fn         crc32;
    Adler32Fn       adler32;
};

/**
 * Returns the global dispatch table, initialized once at first call.
 * Thread-safe (function-local static, C++11 guarantee).
 */
const SIMDDispatch& get_simd_dispatch();

/**
 * Convenience: return the best available match_length function.
 * Inlined for use in the LZ77 hot path.
 */
inline MatchLengthFn simd_match_length_fn() {
    return get_simd_dispatch().match_length;
}

inline Crc32Fn simd_crc32_fn() {
    return get_simd_dispatch().crc32;
}

inline Adler32Fn simd_adler32_fn() {
    return get_simd_dispatch().adler32;
}

/* ── Scalar fallbacks (always available) ─────────────────────────────────── */

int      scalar_match_length (const uint8_t* a, const uint8_t* b, int max_len);
uint32_t scalar_crc32        (uint32_t crc,   const uint8_t* data, size_t len);
uint32_t scalar_adler32      (uint32_t adler, const uint8_t* data, size_t len);
void     scalar_hash_insert_bulk(
    const uint8_t* data, size_t len,
    uint16_t* head, uint16_t* prev, int hash_bits);

/* ── SIMD implementations (declared here, defined in platform files) ─────── */

#if defined(DEFLATE_HAS_NEON) || defined(DEFLATE_HAS_CRC_ARM)
int      neon_match_length   (const uint8_t* a, const uint8_t* b, int max_len);
uint32_t arm_crc32           (uint32_t crc,   const uint8_t* data, size_t len);
uint32_t neon_adler32        (uint32_t adler, const uint8_t* data, size_t len);
void     neon_hash_insert_bulk(
    const uint8_t* data, size_t len,
    uint16_t* head, uint16_t* prev, int hash_bits);
#endif

#if defined(DEFLATE_HAS_SSE42)
int      sse42_match_length  (const uint8_t* a, const uint8_t* b, int max_len);
uint32_t sse42_crc32         (uint32_t crc,   const uint8_t* data, size_t len);
#endif

#if defined(DEFLATE_HAS_AVX2)
uint32_t avx2_adler32        (uint32_t adler, const uint8_t* data, size_t len);
void     avx2_hash_insert_bulk(
    const uint8_t* data, size_t len,
    uint16_t* head, uint16_t* prev, int hash_bits);
#endif

#if defined(DEFLATE_HAS_SSE2)
void     sse2_hash_insert_bulk(
    const uint8_t* data, size_t len,
    uint16_t* head, uint16_t* prev, int hash_bits);
#endif

} /* namespace deflate */
