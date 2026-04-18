#pragma once

#include <cstddef>
#include <cstdint>
#include "thread_pool.hpp"
#include "../../include/orot/deflate_types.h"

namespace orot { namespace deflate {

/*
 * pigz-style parallel compressor.
 *
 * Splits input into independent blocks and compresses each in parallel.
 * Output blocks are concatenated in order.
 * Each block is a self-contained DEFLATE stream (no cross-block references
 * by default; improves parallelism at ~1-3% ratio cost).
 */
class ParallelCompressor {
public:
    ParallelCompressor(
        int    level,
        deflate_format format,
        int    num_threads = 0,
        size_t block_size  = 0);

    ~ParallelCompressor();

    /*
     * Compress src[0..src_len) using multiple threads.
     * Returns compressed size, or 0 on failure.
     */
    size_t compress(
        const uint8_t* src, size_t src_len,
        uint8_t*       dst, size_t dst_capacity);

    int    thread_count() const noexcept;
    size_t block_size()   const noexcept { return block_size_; }

private:
    size_t deflate_compress_bound_internal(size_t n) const;
    int            level_;
    deflate_format format_;
    size_t         block_size_;
    ThreadPool     pool_;
};

} } /* namespace orot::deflate */
