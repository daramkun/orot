#pragma once

#include <cstdint>
#include <cstddef>

namespace orot { namespace lzma {

/* Parse LZMA alone header and return uncompressed size.
 * Returns (uint64_t)-1 on invalid header.
 * src must be at least 13 bytes. */
uint64_t lzma_header_uncompressed_size(const uint8_t* src) noexcept;

/* Decompress LZMA alone format.
 * Returns bytes written to dst, or 0 on error. */
size_t lzma_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_cap) noexcept;

} } /* namespace orot::lzma */
