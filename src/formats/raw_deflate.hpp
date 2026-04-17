#pragma once

#include <cstddef>
#include <cstdint>
#include "../../include/deflate/deflate_types.h"

namespace deflate {

/* Raw DEFLATE (RFC 1951): no framing, no checksum */

size_t raw_compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    int level,
    bool is_last = true);

deflate_result raw_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    size_t*        actual_out_size);

size_t raw_compress_bound(size_t src_len);

} /* namespace deflate */
