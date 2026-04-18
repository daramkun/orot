#pragma once

#include <cstddef>
#include <cstdint>
#include "inflate_fast.hpp"
#include "../../include/deflate/deflate_types.h"
#include "../memory/arena.hpp"
#include "../memory/aligned_alloc.hpp"

namespace deflate {

/*
 * DEFLATE decompressor state machine.
 * Handles raw DEFLATE bitstreams (RFC 1951).
 * Format wrappers (zlib/gzip) are applied above this layer.
 */
class Decompressor {
public:
    Decompressor();

    /*
     * Decompress incrementally.
     * Updates *next_in, *avail_in, *next_out, *avail_out.
     * Returns:
     *   DEFLATE_OK         - more I/O needed
     *   DEFLATE_STREAM_END - final block decoded
     *   DEFLATE_DATA_ERROR - corrupt or invalid input
     */
    deflate_result decompress(
        const uint8_t** next_in,  size_t* avail_in,
        uint8_t**       next_out, size_t* avail_out);

    void reset();

private:
    /* ── State machine states ────────────────────────────────────────────── */
    enum class State {
        BLOCK_HEADER,
        STORED_LEN,
        STORED_COPY,
        DECODE_LITLEN,     /* decode symbols using current tables */
        DECODE_DIST,       /* waiting for distance code after length */
        COPY_MATCH,        /* copying a back-reference */
        CODE_LENGTHS,      /* building dynamic Huffman tables */
        DONE,
        ERROR,
    };

    /* ── Per-block decode tables ─────────────────────────────────────────── */
    static constexpr size_t ARENA_SIZE = sizeof(InflateTables) + 4096;
    AlignedBuffer<uint8_t>  arena_buf_;
    Arena                   arena_;
    InflateTables*          tables_     = nullptr;

    /* ── Output window (32 KB for back-references) ───────────────────────── */
    static constexpr size_t WIN_SIZE = 32768;
    AlignedBuffer<uint8_t>  window_;
    size_t                  win_pos_ = 0;  /* write position in window */

    /* ── Bit reader state (persistent across calls) ──────────────────────── */
    uint64_t bits_      = 0;
    int      bit_count_ = 0;

    /* ── Block state ─────────────────────────────────────────────────────── */
    State state_    = State::BLOCK_HEADER;
    bool  is_final_ = false;

    /* Stored block copy counters */
    uint16_t stored_len_ = 0;
    uint16_t stored_pos_ = 0;

    /* Code-length decoding state */
    int      hlit_  = 0;
    int      hdist_ = 0;
    int      hclen_ = 0;
    int      cl_idx_      = 0;  /* progress through code-length sequence */
    int      codelens_i_  = 0;  /* progress through combined litlen+dist lengths */
    uint8_t  cl_lens_[CODELEN_SYMS] = {};  /* code-length code lengths */
    uint8_t  combined_lens_[LITLEN_SYMS + DIST_SYMS] = {};

    /* Match copy state */
    int      match_len_  = 0;
    int      match_dist_ = 0;

    /* inflate_fast integration: track contiguous output buffer across calls */
    const uint8_t* out_origin_       = nullptr; // start of first output buffer
    uint8_t*       out_expected_end_ = nullptr; // expected next_out at next call start

    /* Helpers */
    bool build_tables_from_combined();
    void sync_window_from_buf(const uint8_t* buf_start, size_t len) noexcept;
};

} /* namespace deflate */
