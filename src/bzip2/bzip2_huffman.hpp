#pragma once
#include <cstdint>
#include <cstddef>

namespace orot::bzip2 {

static constexpr int BZ_MAX_ALPHA_SIZE = 258;  /* 256 literals + RUNA + RUNB */
static constexpr int BZ_MAX_CODE_LEN   = 20;
static constexpr int BZ_MIN_TABLES     = 2;
static constexpr int BZ_MAX_TABLES     = 6;
static constexpr int BZ_G_SIZE         = 50;
static constexpr int BZ_MAX_SELECTORS  = (900000 / BZ_G_SIZE + 2);

static constexpr int BZ_RUNA = 0;
static constexpr int BZ_RUNB = 1;

/* ── Encode side ─────────────────────────────────────────────────────────── */

struct HuffEncTable {
    uint8_t  len[BZ_MAX_ALPHA_SIZE];
    uint32_t code[BZ_MAX_ALPHA_SIZE];
    int      alpha_size;

    /* n_syms: actual number of symbols to use (sets alpha_size internally) */
    void build_from_freqs(const uint32_t* freq, int n_syms,
                          int max_len = BZ_MAX_CODE_LEN);
    void build_codes();
};

void build_huffman_tables(
    const uint16_t* syms, uint32_t n_syms,
    int alpha_size,
    int n_tables,
    HuffEncTable tables[BZ_MAX_TABLES],
    uint8_t* selectors, uint32_t* n_selectors_out);

/* ── Decode side ─────────────────────────────────────────────────────────── */

static constexpr int HUFF_FAST_BITS = 10;
static constexpr int HUFF_FAST_SIZE = (1 << HUFF_FAST_BITS);

struct HuffDecTable {
    /* Canonical decode tables (MSB-first) */
    int      alpha_size;
    int      min_len, max_len;
    /* perm[i] = symbol, sorted by (length asc, code asc) */
    int      perm[BZ_MAX_ALPHA_SIZE];
    /* base[l]   = first canonical code value for length l */
    /* limit[l]  = last  canonical code value for length l (-1 if none) */
    /* offset[l] = index into perm[] for first symbol of length l */
    uint32_t base[BZ_MAX_CODE_LEN + 2];
    uint32_t limit[BZ_MAX_CODE_LEN + 2];
    int      offset[BZ_MAX_CODE_LEN + 2];
    uint8_t  len[BZ_MAX_ALPHA_SIZE];

    struct FastEntry {
        int16_t sym;  /* symbol, or -1 = slow path */
        uint8_t len;  /* code length in bits */
    };
    FastEntry fast_table[HUFF_FAST_SIZE];

    void build_from_lengths(const uint8_t* lengths, int size);

    /* Decode one symbol. Returns symbol (>=0) or -1 on error.
       Uses a 64-bit {buf, buf_bits} accumulator (MSB-first).
       buf/buf_bits must be maintained across calls to the same stream. */
    int decode_sym(uint64_t& buf, int& buf_bits,
                   const uint8_t* src, size_t src_size, size_t& src_pos) const;
};

} // namespace orot::bzip2
