#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "../core/lz77.hpp"
#include "level_config.hpp"
#include "../memory/arena.hpp"
#include "../memory/aligned_alloc.hpp"

namespace deflate {

/*
 * Whole-buffer block compressor (libdeflate style).
 * No streaming state — compresses src entirely in one call.
 * Uses an arena for all internal allocations (zero malloc in hot path).
 */
class BlockCompressor {
public:
    explicit BlockCompressor(int level);

    /*
     * Compress src[0..src_len) → dst[0..dst_capacity).
     * is_last: set BFINAL=1 in the DEFLATE block header (default: true).
     * Returns compressed byte count, or 0 if dst too small.
     */
    size_t compress(
        const uint8_t* src, size_t src_len,
        uint8_t*       dst, size_t dst_capacity,
        bool           is_last = true);

private:
    CompressConfig cfg_;

    /* Internal scratch (heap-allocated once in ctor) */
    static constexpr size_t ARENA_SIZE =
          sizeof(LZ77State)         /* hash tables: ~192 KB */
        + (1 << 17) * sizeof(Token) /* token buffer: 128K tokens × 4 B */
        + 4096;                     /* alignment headroom */

    AlignedBuffer<uint8_t>  arena_buf_;
    Arena                   arena_;
    std::vector<Token>      heap_tokens_; /* overflow fallback for large blocks */
};

} /* namespace deflate */
