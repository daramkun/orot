#pragma once
#include <cstdint>
#include <cstddef>

namespace orot::bzip2 {

/* MSB-first CRC32 — NOT the same as gzip/zlib CRC32.
   Same polynomial (0x04C11DB7) but reflected differently. */

inline uint32_t crc32_update(uint32_t crc, uint8_t byte) {
    for (int i = 0; i < 8; ++i) {
        if ((crc ^ ((uint32_t)byte << 24)) & 0x80000000u)
            crc = (crc << 1) ^ 0x04C11DB7u;
        else
            crc = (crc << 1);
        byte <<= 1;
    }
    return crc;
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
