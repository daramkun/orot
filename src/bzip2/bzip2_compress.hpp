#pragma once
#include <cstdint>
#include <cstddef>

namespace orot::bzip2 {

size_t bzip2_compress_bound(size_t src_size);

/* Returns bytes written, or 0 on failure. */
size_t bzip2_compress(const uint8_t* src, size_t src_size,
                      uint8_t* dst, size_t dst_cap,
                      int level);

} // namespace orot::bzip2
