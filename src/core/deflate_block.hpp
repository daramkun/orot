#pragma once

#include "huffman.hpp"
#include "lz77.hpp"
#include "bit_writer.hpp"

#include <cstddef>
#include <cstdint>

namespace orot { namespace deflate {

/* ── Block types (RFC 1951 §3.2.3) ──────────────────────────────────────── */
enum class BlockType : int {
    Stored  = 0,  /* BTYPE = 00: no compression                           */
    Fixed   = 1,  /* BTYPE = 01: fixed Huffman codes                      */
    Dynamic = 2,  /* BTYPE = 10: dynamic Huffman codes                    */
};

/* ── Block statistics (for block type selection) ─────────────────────────── */
struct BlockStats {
    uint32_t lit_freq[LITLEN_SYMS];  /* literal/length symbol frequencies */
    uint32_t dist_freq[DIST_SYMS];   /* distance symbol frequencies       */
    size_t   n_tokens;               /* number of LZ77 tokens             */
    size_t   raw_bytes;              /* uncompressed byte count           */
};

/* ── Block encoder ───────────────────────────────────────────────────────── */

/**
 * Encode a sequence of LZ77 tokens as a DEFLATE block.
 *
 * tokens[0..n_tokens): input token array.
 * bw:                  output bit stream (must be pre-allocated).
 * is_last:             set BFINAL bit.
 * preferred_type:      override block type (Auto = choose best).
 *
 * Returns bytes written to bw on success, 0 on failure.
 */
enum class BlockTypeHint { Auto, Stored, Fixed, Dynamic };

size_t encode_block(
    const Token*    tokens,
    size_t          n_tokens,
    const uint8_t*  raw_input,   /* original bytes (for stored block)    */
    size_t          raw_len,
    BitWriter&      bw,
    bool            is_last,
    BlockTypeHint   hint = BlockTypeHint::Auto);

/**
 * Emit a stored (no-compression) block.
 * raw[0..len) must be byte-aligned in bw before call.
 */
void emit_stored_block(
    const uint8_t* raw, size_t len,
    BitWriter& bw, bool is_last);

/**
 * Emit a block using fixed Huffman codes.
 */
void emit_fixed_block(
    const Token* tokens, size_t n_tokens,
    BitWriter& bw, bool is_last);

/**
 * Emit a block using dynamic Huffman codes.
 */
void emit_dynamic_block(
    const Token* tokens, size_t n_tokens,
    const BlockStats& stats,
    BitWriter& bw, bool is_last);

/* ── Code-length encoding helpers ────────────────────────────────────────── */

/**
 * Encode literal/length and distance code lengths using the code-length
 * alphabet (RFC 1951 §3.2.7).  Writes HLIT, HDIST, HCLEN, and the
 * code-length sequences.
 */
void encode_code_lengths(
    const uint8_t* litlen_lens, int litlen_count,
    const uint8_t* dist_lens,   int dist_count,
    BitWriter& bw);

/* ── Token stream statistics ─────────────────────────────────────────────── */

/**
 * Scan a token stream and compute frequency tables.
 */
void compute_block_stats(
    const Token* tokens, size_t n_tokens,
    BlockStats& stats);

/**
 * Estimate compressed size (in bits) for a given block type.
 */
size_t estimate_fixed_bits  (const BlockStats& stats);
size_t estimate_dynamic_bits(const BlockStats& stats);

} } /* namespace orot::deflate */
