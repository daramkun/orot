#pragma once

#include <cstddef>
#include <cstdint>
#include "../deflate_fwd.hpp"

namespace orot { namespace deflate {

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

/* Extended variant: also returns incremental adler32 when adler_exact is true.
 * out_adler is set only when *out_adler_exact == true (STORED-only streams). */
deflate_result raw_decompress_ex(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    size_t*        actual_out_size,
    uint32_t*      out_adler,
    bool*          out_adler_exact);

size_t raw_compress_bound(size_t src_len);

} } /* namespace orot::deflate */
