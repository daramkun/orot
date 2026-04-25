#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

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
} // namespace detail

inline uint32_t crc32_update(uint32_t crc, uint8_t byte) {
    return (crc << 8) ^ detail::CRC32_TABLE[((crc >> 24) ^ byte) & 0xFF];
}

inline uint32_t crc32_block(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = crc32_update(crc, data[i]);
    return crc ^ 0xFFFFFFFFu;
}

inline uint32_t crc32_combine(uint32_t combined, uint32_t block_crc) {
    return ((combined << 1) | (combined >> 31)) ^ block_crc;
}

} // namespace orot::bzip2
