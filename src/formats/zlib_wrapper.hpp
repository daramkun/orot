#pragma once

#include <cstddef>
#include <cstdint>
#include "../../include/orot/deflate_types.h"

namespace orot { namespace deflate {

/* RFC 1950 zlib framing: CMF+FLG header, Adler-32 trailer */

size_t zlib_compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    int level);

deflate_result zlib_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    size_t*        actual_out_size);

size_t zlib_compress_bound(size_t src_len);

} } /* namespace orot::deflate */
