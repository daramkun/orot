#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <cstring>

#if defined(DEFLATE_HAS_NEON)
#include <arm_neon.h>
#elif defined(DEFLATE_HAS_SSE2)
#include <emmintrin.h>
#endif

namespace orot::bzip2 {

/* MSB-first CRC32 — NOT the same as gzip/zlib CRC32.
   Same polynomial (0x04C11DB7) but reflected differently. */

namespace detail {
constexpr std::array<uint32_t, 256> make_crc32_table() {
    std::array<uint32_t, 256> t{};
    for (int i = 0; i < 256; ++i) {
        uint32_t c = (uint32_t)i << 24;
        for (int j = 0; j < 8; ++j)
            c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
        t[i] = c;
    }
    return t;
}
static constexpr auto CRC32_TABLE = make_crc32_table();

constexpr std::array<std::array<uint32_t, 256>, 8> make_crc32_tables() {
    std::array<std::array<uint32_t, 256>, 8> tables{};
    tables[0] = CRC32_TABLE;
    for (int n = 1; n < 8; ++n) {
        for (int i = 0; i < 256; ++i) {
            uint32_t c = tables[n - 1][i];
            tables[n][i] = (c << 8) ^ tables[0][c >> 24];
        }
    }
    return tables;
}
static constexpr auto CRC32_TABLES = make_crc32_tables();
} // namespace detail

inline uint32_t crc32_update(uint32_t crc, uint8_t byte) {
    return (crc << 8) ^ detail::CRC32_TABLE[((crc >> 24) ^ byte) & 0xFF];
}

inline uint32_t crc32_update_bytes(uint32_t crc, const uint8_t* data, size_t len) {
    while (len >= 8) {
        crc =
            detail::CRC32_TABLES[7][((crc >> 24) ^ data[0]) & 0xFF] ^
            detail::CRC32_TABLES[6][((crc >> 16) ^ data[1]) & 0xFF] ^
            detail::CRC32_TABLES[5][((crc >> 8)  ^ data[2]) & 0xFF] ^
            detail::CRC32_TABLES[4][( crc        ^ data[3]) & 0xFF] ^
            detail::CRC32_TABLES[3][data[4]] ^
            detail::CRC32_TABLES[2][data[5]] ^
            detail::CRC32_TABLES[1][data[6]] ^
            detail::CRC32_TABLES[0][data[7]];
        data += 8;
        len -= 8;
    }
    for (size_t i = 0; i < len; ++i)
        crc = crc32_update(crc, data[i]);
    return crc;
}

inline uint32_t crc32_update_repeat(uint32_t crc, uint8_t byte, size_t len) {
    alignas(16) uint8_t chunk[64];

#if defined(DEFLATE_HAS_NEON)
    const uint8x16_t v = vdupq_n_u8(byte);
    vst1q_u8(chunk +  0, v);
    vst1q_u8(chunk + 16, v);
    vst1q_u8(chunk + 32, v);
    vst1q_u8(chunk + 48, v);
#elif defined(DEFLATE_HAS_SSE2)
    const __m128i v = _mm_set1_epi8((char)byte);
    _mm_store_si128((__m128i*)(chunk +  0), v);
    _mm_store_si128((__m128i*)(chunk + 16), v);
    _mm_store_si128((__m128i*)(chunk + 32), v);
    _mm_store_si128((__m128i*)(chunk + 48), v);
#else
    std::memset(chunk, byte, sizeof(chunk));
#endif

    while (len >= sizeof(chunk)) {
        crc = crc32_update_bytes(crc, chunk, sizeof(chunk));
        len -= sizeof(chunk);
    }
    if (len > 0) {
        crc = crc32_update_bytes(crc, chunk, len);
    }
    return crc;
}

inline uint32_t crc32_finalize(uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}

inline uint32_t crc32_block(const uint8_t* data, size_t len) {
    return crc32_finalize(crc32_update_bytes(0xFFFFFFFFu, data, len));
}

inline uint32_t crc32_combine(uint32_t combined, uint32_t block_crc) {
    return ((combined << 1) | (combined >> 31)) ^ block_crc;
}

} // namespace orot::bzip2
