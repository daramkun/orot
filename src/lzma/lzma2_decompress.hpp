#pragma once

#include <cstdint>
#include <cstddef>

namespace orot { namespace lzma {

/* Decompress LZMA2 chunk stream.
 * Returns bytes written to dst, or 0 on error. */
size_t lzma2_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_cap) noexcept;

} } /* namespace orot::lzma */
