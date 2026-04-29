#pragma once

#include <cstddef>
#include <cstdint>

namespace orot { namespace brotli {

enum class BrotliDecodeStatus {
    Ok,
    NeedOutput,
    DataError,
    Unsupported,
};

BrotliDecodeStatus brotli_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    size_t* actual_out) noexcept;

} } /* namespace orot::brotli */
