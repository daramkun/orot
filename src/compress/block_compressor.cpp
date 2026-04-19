#include "block_compressor.hpp"
#include "core/deflate_block.hpp"
#include "core/bit_writer.hpp"

#include <cassert>
#include <cstring>

namespace orot { namespace deflate {

BlockCompressor::BlockCompressor(int level)
    : cfg_(compress_config_for_level(level))
    , arena_buf_(ARENA_SIZE)
    , arena_(arena_buf_.data(), ARENA_SIZE)
{}

size_t BlockCompressor::compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    bool           is_last)
{
    arena_.reset();

    /* Allocate LZ77 hash state from arena; zero only the active hash table
     * portion (1<<hash_bits entries) instead of the full 128 KB head[]. */
    LZ77State* state = arena_.alloc<LZ77State>();
    if (!state) return 0;
    state->reset(cfg_.lz77.hash_bits, cfg_.lz77.bt4);

    /* Allocate token buffer (worst case: all literals).
     * Fall back to heap when src_len exceeds arena capacity. */
    const size_t max_tokens = src_len;
    Token* tokens;
    {
        const size_t arena_avail = arena_.remaining() / sizeof(Token);
        if (max_tokens <= arena_avail) {
            tokens = arena_.alloc<Token>(max_tokens);
            if (!tokens) return 0;
        } else {
            heap_tokens_.resize(max_tokens);
            tokens = heap_tokens_.data();
        }
    }

    /* LZ77 compress */
    size_t n_tokens = lz77_compress(src, src_len, tokens, *state, cfg_.lz77);

    /* DEFLATE encode */
    BitWriter bw(dst, dst_capacity);

    /* All-literal: stored block is always cheaper than fixed/dynamic.
     * Skip block stats and Huffman estimation entirely. */
    if (n_tokens == src_len && cfg_.block_hint != BlockTypeHint::Stored) {
        emit_stored_block(src, src_len, bw, is_last);
    } else {
        encode_block(tokens, n_tokens, src, src_len, bw, is_last, cfg_.block_hint);
    }

    const size_t out_bytes = bw.bytes_written();
    if (bw.pending_bits() > 0) {
        /* Flush remaining bits */
        bw.flush();
        return bw.bytes_written();
    }
    return out_bytes;
}

} } /* namespace orot::deflate */
