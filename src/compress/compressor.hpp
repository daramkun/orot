#pragma once

#include <cstddef>
#include <cstdint>
#include "level_config.hpp"
#include "../core/lz77.hpp"
#include "../memory/arena.hpp"
#include "../memory/aligned_alloc.hpp"
#include "../deflate_fwd.hpp"

namespace orot { namespace deflate {

/*
 * Streaming compressor state machine.
 * Accumulates input in a sliding window, emitting DEFLATE blocks
 * as they fill.  Compatible with the z_stream API pattern.
 */
class Compressor {
public:
    explicit Compressor(int level);
    ~Compressor();

    /*
     * Feed input and/or drain output.
     * Updates *next_in / *avail_in / *next_out / *avail_out.
     * Returns:
     *   DEFLATE_OK           — more I/O needed
     *   DEFLATE_STREAM_END   — stream finalised (flush == DEFLATE_FINISH)
     *   DEFLATE_NEED_OUTPUT  — output buffer full, call again with more space
     *   negative             — error
     */
    deflate_result compress(
        const uint8_t** next_in,  size_t* avail_in,
        uint8_t**       next_out, size_t* avail_out,
        deflate_flush   flush);

private:
    void flush_block(bool is_last);

    CompressConfig cfg_;

    /* Input accumulation buffer (two-buffer ping-pong) */
    static constexpr size_t INPUT_BUF  = 131072;  /* 128 KB */
    static constexpr size_t OUTPUT_BUF = 262144;  /* 256 KB */

    AlignedBuffer<uint8_t> input_buf_;
    AlignedBuffer<uint8_t> output_buf_;
    size_t input_pos_  = 0;   /* bytes pending in input buffer */
    size_t output_pos_ = 0;   /* write position in output buffer */
    size_t output_len_ = 0;   /* valid bytes in output buffer */

    /* LZ77 state (persistent across blocks for back-references) */
    static constexpr size_t ARENA_SIZE =
          sizeof(LZ77State)
        + INPUT_BUF * sizeof(Token)  /* worst-case tokens */
        + 4096;

    AlignedBuffer<uint8_t> arena_buf_;
    Arena                  arena_;
    LZ77State*             lz77_state_ = nullptr;
};

} } /* namespace orot::deflate */
