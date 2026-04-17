#include "deflate_block.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace deflate {

/* =========================================================================
 * Helper: emit a Huffman symbol
 * ========================================================================= */

static inline void emit_sym(BitWriter& bw, uint16_t code, uint8_t len) {
    bw.write_bits(code, len);
}

/* =========================================================================
 * Block statistics
 * ========================================================================= */

void compute_block_stats(
    const Token* tokens, size_t n_tokens,
    BlockStats& stats)
{
    std::memset(stats.lit_freq,  0, sizeof(stats.lit_freq));
    std::memset(stats.dist_freq, 0, sizeof(stats.dist_freq));
    stats.n_tokens = n_tokens;
    stats.raw_bytes = 0;

    for (size_t i = 0; i < n_tokens; ++i) {
        const Token& t = tokens[i];
        if (t.is_literal()) {
            ++stats.lit_freq[t.literal()];
            ++stats.raw_bytes;
        } else {
            const int lcode = length_to_code(t.length()) + 257;
            const int dcode = dist_to_code  (t.distance());
            ++stats.lit_freq [lcode];
            ++stats.dist_freq[dcode];
            stats.raw_bytes += t.length();
        }
    }
    ++stats.lit_freq[256];  /* EOB */
}

/* =========================================================================
 * Bit cost estimation
 * ========================================================================= */

size_t estimate_fixed_bits(const BlockStats& stats) {
    /* Build fixed Huffman tables */
    HuffEncTable litlen;
    HuffDistTable dist;
    build_fixed_litlen_enc(litlen);
    build_fixed_dist_enc(dist);

    size_t bits = 3;  /* BFINAL + BTYPE */
    for (int i = 0; i < LITLEN_SYMS; ++i)
        bits += static_cast<size_t>(litlen.lens[i]) * stats.lit_freq[i];
    for (int i = 0; i < DIST_SYMS; ++i)
        bits += static_cast<size_t>(dist.lens[i]) * stats.dist_freq[i];
    /* Add extra bits for length/distance codes */
    for (int i = 0; i < 29; ++i) {
        const int lcode = i + 257;
        bits += static_cast<size_t>(LENGTH_EXTRA_BITS[i]) * stats.lit_freq[lcode];
    }
    for (int i = 0; i < 30; ++i)
        bits += static_cast<size_t>(DIST_EXTRA_BITS[i]) * stats.dist_freq[i];
    return bits;
}

/* Internal: estimate dynamic block size from pre-built Huffman lengths.
 * Avoids an extra build_huffman_lengths call when lengths are already known. */
static size_t estimate_dynamic_bits_from_lens(
    const BlockStats& stats,
    const uint8_t* litlen_lens,
    const uint8_t* dist_lens)
{
    size_t bits = 3 + 5 + 5 + 4;  /* BFINAL/BTYPE + HLIT + HDIST + HCLEN */
    bits += 19 * 3;  /* code-length lengths (rough estimate) */
    bits += 3 * (LITLEN_SYMS + DIST_SYMS);  /* conservative RLE estimate */

    for (int i = 0; i < LITLEN_SYMS; ++i)
        bits += static_cast<size_t>(litlen_lens[i]) * stats.lit_freq[i];
    for (int i = 0; i < DIST_SYMS; ++i)
        bits += static_cast<size_t>(dist_lens[i]) * stats.dist_freq[i];
    for (int i = 0; i < 29; ++i)
        bits += static_cast<size_t>(LENGTH_EXTRA_BITS[i]) * stats.lit_freq[i + 257];
    for (int i = 0; i < 30; ++i)
        bits += static_cast<size_t>(DIST_EXTRA_BITS[i]) * stats.dist_freq[i];
    return bits;
}

size_t estimate_dynamic_bits(const BlockStats& stats) {
    /* Build optimal Huffman lengths, then delegate to prebuilt variant */
    uint8_t litlen_lens[LITLEN_SYMS] = {};
    uint8_t dist_lens  [DIST_SYMS]   = {};
    build_huffman_lengths(stats.lit_freq,  LITLEN_SYMS, litlen_lens, MAX_CODE_BITS);
    build_huffman_lengths(stats.dist_freq, DIST_SYMS,   dist_lens,   MAX_CODE_BITS);
    return estimate_dynamic_bits_from_lens(stats, litlen_lens, dist_lens);
}

/* =========================================================================
 * Stored block
 * ========================================================================= */

void emit_stored_block(
    const uint8_t* raw, size_t len,
    BitWriter& bw, bool is_last)
{
    /* Stored blocks are limited to 65535 bytes each */
    size_t offset = 0;
    while (offset < len || offset == 0) {
        const size_t chunk = std::min<size_t>(len - offset, 65535U);
        const bool   last  = is_last && (offset + chunk == len);

        bw.write_bit(last ? 1 : 0);  /* BFINAL */
        bw.write_bits(0, 2);          /* BTYPE = 00 */
        bw.align_to_byte();

        const uint16_t nlen   = static_cast<uint16_t>(chunk);
        const uint16_t nlen_c = static_cast<uint16_t>(~nlen);
        bw.write_byte(static_cast<uint8_t>(nlen));
        bw.write_byte(static_cast<uint8_t>(nlen >> 8));
        bw.write_byte(static_cast<uint8_t>(nlen_c));
        bw.write_byte(static_cast<uint8_t>(nlen_c >> 8));
        bw.write_bytes(raw + offset, chunk);

        offset += chunk;
        if (chunk == 0) break;  /* len == 0 case */
    }
}

/* =========================================================================
 * Token emission helpers
 * ========================================================================= */

static void emit_litlen_token(
    const Token& t,
    const HuffEncTable& litlen,
    const HuffDistTable& dist,
    BitWriter& bw)
{
    if (t.is_literal()) {
        emit_sym(bw, litlen.codes[t.literal()], litlen.lens[t.literal()]);
    } else {
        const int li    = length_to_code(t.length());
        const int lcode = li + 257;
        emit_sym(bw, litlen.codes[lcode], litlen.lens[lcode]);
        /* Extra bits for length */
        if (LENGTH_EXTRA_BITS[li] > 0)
            bw.write_bits(t.length() - LENGTH_BASE[li], LENGTH_EXTRA_BITS[li]);

        const int di = dist_to_code(t.distance());
        emit_sym(bw, dist.codes[di], dist.lens[di]);
        /* Extra bits for distance */
        if (DIST_EXTRA_BITS[di] > 0)
            bw.write_bits(t.distance() - DIST_BASE[di], DIST_EXTRA_BITS[di]);
    }
}

/* =========================================================================
 * Fixed block
 * ========================================================================= */

void emit_fixed_block(
    const Token* tokens, size_t n_tokens,
    BitWriter& bw, bool is_last)
{
    bw.write_bit(is_last ? 1 : 0);
    bw.write_bits(1, 2);  /* BTYPE = 01 = fixed Huffman */

    HuffEncTable  litlen;
    HuffDistTable dist;
    build_fixed_litlen_enc(litlen);
    build_fixed_dist_enc(dist);

    for (size_t i = 0; i < n_tokens; ++i)
        emit_litlen_token(tokens[i], litlen, dist, bw);

    /* EOB symbol (256) */
    emit_sym(bw, litlen.codes[256], litlen.lens[256]);
}

/* =========================================================================
 * Code-length alphabet encoding (RFC 1951 §3.2.7)
 * ========================================================================= */

/*
 * Run-length encode a sequence of code lengths using the code-length
 * alphabet: symbols 0-15 = literal length, 16 = repeat prev (2-bit extra),
 * 17 = repeat 0 (3-bit extra), 18 = repeat 0 (7-bit extra).
 *
 * Output: writes HLIT, HDIST, HCLEN and all code-length codes.
 */
void encode_code_lengths(
    const uint8_t* litlen_lens, int litlen_count,
    const uint8_t* dist_lens,   int dist_count,
    BitWriter& bw)
{
    /* Merge litlen and dist lens into one sequence */
    static uint8_t combined[LITLEN_SYMS + DIST_SYMS];
    std::memcpy(combined,              litlen_lens, static_cast<size_t>(litlen_count));
    std::memcpy(combined + litlen_count, dist_lens, static_cast<size_t>(dist_count));
    const int total = litlen_count + dist_count;

    /* Run-length encode */
    static uint8_t  rle_sym [LITLEN_SYMS + DIST_SYMS];
    static uint8_t  rle_xtra[LITLEN_SYMS + DIST_SYMS];
    int n_rle = 0;

    for (int i = 0; i < total; ) {
        const uint8_t sym = combined[i];
        /* Count run */
        int run = 1;
        while (i + run < total && combined[i + run] == sym && run < 138) ++run;

        if (sym == 0 && run >= 3) {
            if (run <= 10) {
                rle_sym [n_rle]   = 17;
                rle_xtra[n_rle++] = static_cast<uint8_t>(run - 3);
                i += run;
            } else {
                rle_sym [n_rle]   = 18;
                rle_xtra[n_rle++] = static_cast<uint8_t>(run - 11);
                i += run;
            }
        } else if (sym != 0 && run >= 4) {
            /* Emit first symbol literally, then use repeat (16) for rest */
            rle_sym [n_rle]   = sym;
            rle_xtra[n_rle++] = 0xFF;  /* 0xFF = no extra bits */
            ++i;
            const int repeat = std::min(run - 1, 6);
            rle_sym [n_rle]   = 16;
            rle_xtra[n_rle++] = static_cast<uint8_t>(repeat - 3);
            i += repeat;
        } else {
            rle_sym [n_rle]   = sym;
            rle_xtra[n_rle++] = 0xFF;
            ++i;
        }
    }

    /* Build code-length Huffman codes */
    uint32_t cl_freq[CODELEN_SYMS] = {};
    for (int i = 0; i < n_rle; ++i)
        if (rle_sym[i] < CODELEN_SYMS)
            ++cl_freq[rle_sym[i]];

    uint8_t  cl_lens [CODELEN_SYMS] = {};
    uint16_t cl_codes[CODELEN_SYMS] = {};
    build_huffman_lengths(cl_freq, CODELEN_SYMS, cl_lens, MAX_CODELEN_BITS);
    build_enc_table_from_lens(cl_lens, CODELEN_SYMS, cl_codes);

    /* RFC 1951 code-length order */
    static const int CL_ORDER[CODELEN_SYMS] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };

    /* Find HCLEN: how many code-length codes to emit */
    int hclen = CODELEN_SYMS;
    while (hclen > 4 && cl_lens[CL_ORDER[hclen - 1]] == 0) --hclen;

    /* Write HLIT, HDIST, HCLEN */
    bw.write_bits(static_cast<uint32_t>(litlen_count - 257), 5);  /* HLIT  */
    bw.write_bits(static_cast<uint32_t>(dist_count   -   1), 5);  /* HDIST */
    bw.write_bits(static_cast<uint32_t>(hclen         -   4), 4); /* HCLEN */

    /* Write code-length lengths in order */
    for (int i = 0; i < hclen; ++i)
        bw.write_bits(cl_lens[CL_ORDER[i]], 3);

    /* Write the RLE-encoded combined code lengths */
    for (int i = 0; i < n_rle; ++i) {
        const uint8_t s = rle_sym[i];
        if (rle_xtra[i] == 0xFF) {
            /* Literal length */
            bw.write_bits(cl_codes[s], cl_lens[s]);
        } else if (s == 16) {
            bw.write_bits(cl_codes[16], cl_lens[16]);
            bw.write_bits(rle_xtra[i], 2);
        } else if (s == 17) {
            bw.write_bits(cl_codes[17], cl_lens[17]);
            bw.write_bits(rle_xtra[i], 3);
        } else {  /* s == 18 */
            bw.write_bits(cl_codes[18], cl_lens[18]);
            bw.write_bits(rle_xtra[i], 7);
        }
    }
}

/* =========================================================================
 * Dynamic block
 * ========================================================================= */

void emit_dynamic_block(
    const Token* tokens, size_t n_tokens,
    const BlockStats& stats,
    BitWriter& bw, bool is_last)
{
    bw.write_bit(is_last ? 1 : 0);
    bw.write_bits(2, 2);  /* BTYPE = 10 = dynamic Huffman */

    /* Build optimal Huffman tables */
    uint8_t litlen_lens[LITLEN_SYMS] = {};
    uint8_t dist_lens  [DIST_SYMS]   = {};
    build_huffman_lengths(stats.lit_freq, LITLEN_SYMS, litlen_lens, MAX_CODE_BITS);
    build_huffman_lengths(stats.dist_freq, DIST_SYMS,  dist_lens,   MAX_CODE_BITS);

    /* Ensure EOB (256) has a valid code */
    if (litlen_lens[256] == 0) litlen_lens[256] = 1;
    /* Ensure at least one distance code if there were any matches */
    bool has_dist = false;
    for (int i = 0; i < DIST_SYMS; ++i) if (dist_lens[i] > 0) { has_dist = true; break; }
    if (!has_dist) dist_lens[0] = 1;

    /* Determine actual number of litlen/dist codes to emit */
    int hlit = LITLEN_SYMS;
    while (hlit > 257 && litlen_lens[hlit - 1] == 0) --hlit;

    int hdist = DIST_SYMS;
    while (hdist > 1 && dist_lens[hdist - 1] == 0) --hdist;

    /* Write code-length encoding */
    encode_code_lengths(litlen_lens, hlit, dist_lens, hdist, bw);

    /* Build canonical codes */
    HuffEncTable  litlen_enc;
    HuffDistTable dist_enc;
    std::memcpy(litlen_enc.lens, litlen_lens, LITLEN_SYMS);
    std::memcpy(dist_enc.lens,   dist_lens,   DIST_SYMS);
    build_enc_table_from_lens(litlen_lens, LITLEN_SYMS, litlen_enc.codes);
    build_enc_table_from_lens(dist_lens,   DIST_SYMS,   dist_enc.codes);

    /* Emit tokens */
    for (size_t i = 0; i < n_tokens; ++i)
        emit_litlen_token(tokens[i], litlen_enc, dist_enc, bw);

    /* EOB */
    emit_sym(bw, litlen_enc.codes[256], litlen_enc.lens[256]);
}

/* =========================================================================
 * Internal: emit dynamic block from pre-built Huffman lengths
 * ========================================================================= */

static void emit_dynamic_block_prebuilt(
    const Token*   tokens, size_t n_tokens,
    const uint8_t* litlen_lens, const uint8_t* dist_lens,
    BitWriter& bw, bool is_last)
{
    bw.write_bit(is_last ? 1 : 0);
    bw.write_bits(2, 2);  /* BTYPE = 10 = dynamic Huffman */

    /* Ensure EOB (256) has a valid code */
    uint8_t ll[LITLEN_SYMS];
    uint8_t dl[DIST_SYMS];
    std::memcpy(ll, litlen_lens, LITLEN_SYMS);
    std::memcpy(dl, dist_lens,   DIST_SYMS);
    if (ll[256] == 0) ll[256] = 1;
    bool has_dist = false;
    for (int i = 0; i < DIST_SYMS; ++i) if (dl[i] > 0) { has_dist = true; break; }
    if (!has_dist) dl[0] = 1;

    int hlit  = LITLEN_SYMS;
    while (hlit  > 257 && ll[hlit  - 1] == 0) --hlit;
    int hdist = DIST_SYMS;
    while (hdist > 1   && dl[hdist - 1] == 0) --hdist;

    encode_code_lengths(ll, hlit, dl, hdist, bw);

    HuffEncTable  litlen_enc;
    HuffDistTable dist_enc;
    std::memcpy(litlen_enc.lens, ll, LITLEN_SYMS);
    std::memcpy(dist_enc.lens,   dl, DIST_SYMS);
    build_enc_table_from_lens(ll, LITLEN_SYMS, litlen_enc.codes);
    build_enc_table_from_lens(dl, DIST_SYMS,   dist_enc.codes);

    for (size_t i = 0; i < n_tokens; ++i)
        emit_litlen_token(tokens[i], litlen_enc, dist_enc, bw);

    emit_sym(bw, litlen_enc.codes[256], litlen_enc.lens[256]);
}

/* =========================================================================
 * Top-level encode_block: automatic block type selection
 * ========================================================================= */

size_t encode_block(
    const Token*    tokens,
    size_t          n_tokens,
    const uint8_t*  raw_input,
    size_t          raw_len,
    BitWriter&      bw,
    bool            is_last,
    BlockTypeHint   hint)
{
    const size_t start = bw.bytes_written();

    BlockType type;

    /* Pre-built dynamic Huffman lengths (populated lazily for Auto/Dynamic) */
    uint8_t dyn_litlen_lens[LITLEN_SYMS] = {};
    uint8_t dyn_dist_lens  [DIST_SYMS]   = {};
    bool    dyn_lens_built = false;

    if (hint == BlockTypeHint::Stored) {
        type = BlockType::Stored;
    } else {
        /* For all non-stored hints: always compare against stored cost.
         * Fixed/Dynamic Huffman can exceed stored size on incompressible data,
         * which would overflow deflate_compress_bound().  Fall back to stored
         * when it would be cheaper (or equal). */
        const size_t stored_bits = (raw_len + 5) * 8;  /* 5 bytes header */

        BlockStats stats{};
        compute_block_stats(tokens, n_tokens, stats);

        if (hint == BlockTypeHint::Fixed) {
            const size_t fixed_bits = estimate_fixed_bits(stats);
            type = (stored_bits <= fixed_bits) ? BlockType::Stored : BlockType::Fixed;
        } else if (hint == BlockTypeHint::Dynamic) {
            /* Build lengths once for estimate; reuse in emit */
            build_huffman_lengths(stats.lit_freq,  LITLEN_SYMS, dyn_litlen_lens, MAX_CODE_BITS);
            build_huffman_lengths(stats.dist_freq, DIST_SYMS,   dyn_dist_lens,   MAX_CODE_BITS);
            dyn_lens_built = true;
            const size_t dynamic_bits = estimate_dynamic_bits_from_lens(
                stats, dyn_litlen_lens, dyn_dist_lens);
            type = (stored_bits <= dynamic_bits) ? BlockType::Stored : BlockType::Dynamic;
        } else {
            /* Auto: compare all three — build dynamic lengths once */
            const size_t fixed_bits = estimate_fixed_bits(stats);
            build_huffman_lengths(stats.lit_freq,  LITLEN_SYMS, dyn_litlen_lens, MAX_CODE_BITS);
            build_huffman_lengths(stats.dist_freq, DIST_SYMS,   dyn_dist_lens,   MAX_CODE_BITS);
            dyn_lens_built = true;
            const size_t dynamic_bits = estimate_dynamic_bits_from_lens(
                stats, dyn_litlen_lens, dyn_dist_lens);

            if (stored_bits <= fixed_bits && stored_bits <= dynamic_bits)
                type = BlockType::Stored;
            else if (fixed_bits <= dynamic_bits)
                type = BlockType::Fixed;
            else
                type = BlockType::Dynamic;
        }
    }

    switch (type) {
    case BlockType::Stored:
        emit_stored_block(raw_input, raw_len, bw, is_last);
        break;
    case BlockType::Fixed:
        emit_fixed_block(tokens, n_tokens, bw, is_last);
        break;
    case BlockType::Dynamic:
        if (dyn_lens_built) {
            /* Reuse pre-built lengths — avoids redundant build_huffman_lengths calls */
            emit_dynamic_block_prebuilt(
                tokens, n_tokens, dyn_litlen_lens, dyn_dist_lens, bw, is_last);
        } else {
            /* hint == Dynamic but lens not built (shouldn't happen, but safe fallback) */
            BlockStats stats{};
            compute_block_stats(tokens, n_tokens, stats);
            emit_dynamic_block(tokens, n_tokens, stats, bw, is_last);
        }
        break;
    }

    /* Only byte-align at the end of the final block.  For non-last blocks in
     * a multi-block stream (e.g., parallel compressor), the next block's bits
     * follow immediately without padding. */
    if (is_last) bw.flush();
    return bw.bytes_written() - start;
}

} /* namespace deflate */
