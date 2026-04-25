#pragma once
#include <cstdint>
#include <cstddef>

namespace orot::bzip2 {

/* Returns bytes written, or 0 on failure. */
size_t bzip2_decompress(const uint8_t* src, size_t src_size,
                        uint8_t* dst, size_t dst_cap);

} // namespace orot::bzip2
