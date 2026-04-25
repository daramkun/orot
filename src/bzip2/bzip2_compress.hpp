#pragma once
#include <cstdint>
#include <cstddef>

namespace orot::bzip2 {

size_t bzip2_compress_bound(size_t src_size);

/* Returns bytes written, or 0 on failure. */
size_t bzip2_compress(const uint8_t* src, size_t src_size,
                      uint8_t* dst, size_t dst_cap,
                      int level);

/* Parallel version: n_threads=0 uses hardware_concurrency. */
size_t bzip2_compress_parallel(const uint8_t* src, size_t src_size,
                                uint8_t* dst, size_t dst_cap,
                                int level, int n_threads);

} // namespace orot::bzip2
