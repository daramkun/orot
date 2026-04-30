#pragma once

#include <cstddef>
#include <cstdint>

namespace orot { namespace brotli {

bool decode_static_dictionary_word(
    int distance,
    size_t max_distance,
    int copy_len,
    uint8_t* dst,
    size_t dst_cap,
    size_t& written) noexcept;

} } /* namespace orot::brotli */
