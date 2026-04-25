#pragma once

#include <cstdint>
#include <cstddef>

namespace orot { namespace lzma {

/* Upper bound on LZMA2 compressed output. */
size_t lzma2_compress_bound(size_t src_len) noexcept;

/* Compress src into LZMA2 chunk stream.
 * Returns bytes written to dst, or 0 on error. */
size_t lzma2_compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_cap,
    int            level) noexcept;

} } /* namespace orot::lzma */
