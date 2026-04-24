#pragma once

#include <cstddef>
#include <cstdint>

namespace orot { namespace deflate {

/* ── Constants ───────────────────────────────────────────────────────────── */

/* Literal/length alphabet: 0-255 literals, 256 EOB, 257-285 length codes */
static constexpr int LITLEN_SYMS   = 288;  /* padded to 288 for alignment  */
static constexpr int DIST_SYMS     = 32;   /* padded to 32                 */
static constexpr int CODELEN_SYMS  = 19;   /* code-length alphabet         */
static constexpr int MAX_CODE_BITS = 15;   /* max Huffman code length      */
static constexpr int MAX_CODELEN_BITS = 7; /* max code-length code length  */

/* ── Huffman table for encoding ──────────────────────────────────────────── */
struct HuffEncTable {
    uint16_t codes[LITLEN_SYMS];   /* canonical code (LSB-first)            */
    uint8_t  lens [LITLEN_SYMS];   /* code length in bits (0 = unused)      */
};

struct HuffDistTable {
    uint16_t codes[DIST_SYMS];
    uint8_t  lens [DIST_SYMS];
};

/* ── Huffman decode table (two-level) ────────────────────────────────────── */
/*
 * Primary table: 2^DECODE_TABLE_BITS entries.
 * Each entry (32-bit):
 *   bits [15: 0]  = symbol OR secondary table offset
 *   bits [23:16]  = number of bits consumed
 *   bit  [24]     = 1 if this is a secondary table pointer
 *
 * Secondary table: entries for codes longer than DECODE_TABLE_BITS bits.
 */
static constexpr int LITLEN_DECODE_BITS = 11;  /* 8KB primary table: fewer secondary lookups */
static constexpr int DIST_DECODE_BITS   = 11;  /* 8KB primary table: eliminates dist secondary lookups */

/* Decode table entry flag bits (above bits[23:16] = code length):
 *   bit[24] = HUFF_LITERAL_FLAG  — set iff sym < 256 (literal byte in bits[7:0])
 *   bit[25] = HUFF_SUBTABLE_FLAG — set iff entry is a secondary-table pointer
 * Mutually exclusive: a secondary pointer never has LITERAL set. */
static constexpr uint32_t HUFF_LITERAL_FLAG  = (1U << 24);
static constexpr uint32_t HUFF_SUBTABLE_FLAG = (1U << 25);

struct HuffDecTable {
    uint32_t* table;     /* primary + secondary entries (contiguous)       */
    int       table_bits;/* primary table index bits                       */
    int       num_syms;  /* number of symbols                              */

    /* Decode one symbol.  Returns symbol value (< num_syms) on success,
     * or -1 on error.  Updates br in place. */
};

/* ── Fixed (RFC 1951) Huffman tables ─────────────────────────────────────── */
void build_fixed_litlen_enc(HuffEncTable& out);
void build_fixed_dist_enc  (HuffDistTable& out);
void build_fixed_litlen_dec(uint32_t* table);  /* table must be >= 2^LITLEN_DECODE_BITS entries */
void build_fixed_dist_dec  (uint32_t* table);  /* table must be 2^8 entries  */

/* ── Dynamic Huffman tree construction ───────────────────────────────────── */

/**
 * Build canonical Huffman lengths from frequency table using Package-Merge.
 * symbols: number of symbols.
 * freqs[]:  symbol frequencies (0 = unused).
 * lens[]:   output, code lengths in bits (0 = unused symbol).
 * max_bits: maximum allowed code length.
 */
void build_huffman_lengths(
    const uint32_t* freqs, int symbols,
    uint8_t* lens, int max_bits);

/**
 * Build encoding table (codes + lengths) from a length array.
 * Uses canonical code assignment.
 */
void build_enc_table_from_lens(
    const uint8_t* lens, int symbols,
    uint16_t* codes);

/**
 * Build decode table from a length array.
 * table must be allocated to hold at least (2^table_bits + num_extra) entries.
 * Returns total number of entries used.
 */
int build_dec_table_from_lens(
    const uint8_t* lens, int symbols,
    int table_bits,
    uint32_t* table);

/* ── Length/distance coding tables (RFC 1951) ────────────────────────────── */

struct LengthCode {
    uint16_t code;      /* base length code (257-285) */
    uint8_t  extra_bits;
    uint16_t base_len;
};

struct DistCode {
    uint16_t code;      /* distance code (0-29)       */
    uint8_t  extra_bits;
    uint16_t base_dist;
};

/* For a match length (3..258), return the code and extra bits. */
int length_to_code(int len);         /* returns index into LENGTHS table */
int dist_to_code  (int dist);        /* returns index into DISTANCES table */

/* Decode: code → base value + extra bits */
extern const LengthCode LENGTHS[29];    /* length codes 257..285          */
extern const DistCode   DISTANCES[30];  /* distance codes 0..29           */

/* Extra bits for length codes 257..285 */
extern const uint8_t LENGTH_EXTRA_BITS[29];
extern const uint16_t LENGTH_BASE[29];

/* Extra bits for distance codes 0..29 */
extern const uint8_t DIST_EXTRA_BITS[30];
extern const uint16_t DIST_BASE[30];

} } /* namespace orot::deflate */
