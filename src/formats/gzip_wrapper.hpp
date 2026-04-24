#pragma once

#include <cstddef>
#include <cstdint>
#include "../deflate_fwd.hpp"

namespace orot { namespace deflate {

/* RFC 1952 gzip framing: 10-byte header, CRC-32 + ISIZE trailer */

size_t gzip_compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    int level,
    const char*    filename  = nullptr,
    uint32_t       mtime     = 0);

deflate_result gzip_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    size_t*        actual_out_size);

size_t gzip_compress_bound(size_t src_len);

} } /* namespace orot::deflate */
