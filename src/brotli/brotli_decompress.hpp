#pragma once

#include <cstddef>
#include <cstdint>

namespace orot { namespace brotli {

size_t brotli_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap) noexcept;

} } /* namespace orot::brotli */
