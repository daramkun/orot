#include "compressor.hpp"
#include "../deflate/deflate_block.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace orot { namespace deflate {

Compressor::Compressor(int level)
    : cfg_(compress_config_for_level(level))
    , input_buf_(INPUT_BUF)
    , output_buf_(OUTPUT_BUF)
    , arena_buf_(ARENA_SIZE)
    , arena_(arena_buf_.data(), ARENA_SIZE)
{
    lz77_state_ = arena_.alloc_zeroed<LZ77State>();
}

Compressor::~Compressor() = default;

void Compressor::flush_block(bool is_last) {
    /* Compress accumulated input → output buffer */
    Token* tokens = arena_.alloc<Token>(input_pos_);
    if (!tokens || !lz77_state_) return;

    const size_t n_tokens = lz77_compress(
        input_buf_.data(), input_pos_,
        tokens, *lz77_state_, cfg_.lz77);

    /* Write directly to output buffer */
    const size_t out_space = OUTPUT_BUF - output_len_;
    BitWriter bw(output_buf_.data() + output_len_, out_space);
    encode_block(tokens, n_tokens,
                 input_buf_.data(), input_pos_,
                 bw, is_last, cfg_.block_hint);
    bw.flush();
    output_len_ += bw.bytes_written();

    /* Reset input and arena (preserves LZ77 state for next block) */
    input_pos_ = 0;
    /* Keep LZ77 state — allows cross-block back-references */
    /* Reset only token portion of arena (arena base stays, offset rewinds past LZ77State) */
    /* Simplified: re-alloc tokens from the same offset next call */
}

deflate_result Compressor::compress(
    const uint8_t** next_in,  size_t* avail_in,
    uint8_t**       next_out, size_t* avail_out,
    deflate_flush   flush)
{
    for (;;) {
        /* 1. Drain pending output first */
        if (output_pos_ < output_len_) {
            const size_t copy = std::min(*avail_out, output_len_ - output_pos_);
            std::memcpy(*next_out, output_buf_.data() + output_pos_, copy);
            *next_out  += copy;
            *avail_out -= copy;
            output_pos_ += copy;
            if (output_pos_ == output_len_) {
                output_pos_ = 0;
                output_len_ = 0;
            }
            if (*avail_out == 0) return DEFLATE_NEED_OUTPUT;
        }

        /* 2. Consume input into accumulation buffer */
        if (*avail_in > 0) {
            const size_t space = INPUT_BUF - input_pos_;
            const size_t copy  = std::min(*avail_in, space);
            std::memcpy(input_buf_.data() + input_pos_, *next_in, copy);
            *next_in   += copy;
            *avail_in  -= copy;
            input_pos_ += copy;
        }

        /* 3. Decide whether to flush a block */
        const bool input_full   = (input_pos_ >= INPUT_BUF);
        const bool need_finish  = (flush == DEFLATE_FINISH && *avail_in == 0);
        const bool need_sync    = (flush == DEFLATE_SYNC_FLUSH || flush == DEFLATE_FULL_FLUSH);

        if (input_full || need_finish || (need_sync && input_pos_ > 0)) {
            /* Only flush a block when there is data to compress.
             * Flushing an empty input_buf_ would spin: BlockCompressor now
             * produces a non-zero output for empty input, causing the
             * DEFLATE_STREAM_END guard below to never trigger. */
            if (input_pos_ > 0) {
                flush_block(need_finish);
            }
            if (need_finish && input_pos_ == 0 && output_len_ == 0)
                return DEFLATE_STREAM_END;
        } else if (*avail_in == 0) {
            /* No more input for now */
            return DEFLATE_OK;
        }
    }
}

} } /* namespace orot::deflate */
