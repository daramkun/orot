#pragma once

#include <cstddef>
#include <cstdint>

namespace deflate {

/* ── Constants ───────────────────────────────────────────────────────────── */

static constexpr int LZ77_MIN_MATCH  = 3;
static constexpr int LZ77_MAX_MATCH  = 258;
static constexpr int LZ77_WIN_SIZE   = 32768;   /* 2^15 sliding window    */
static constexpr int LZ77_WIN_MASK   = LZ77_WIN_SIZE - 1;
static constexpr int LZ77_HASH_BITS  = 16;
static constexpr int LZ77_HASH_SIZE  = (1 << LZ77_HASH_BITS);
static constexpr int LZ77_HASH_MASK  = LZ77_HASH_SIZE - 1;

/* ── Token ───────────────────────────────────────────────────────────────── */

/**
 * A LZ77 token is either:
 *   Literal:     bits[31]   = 0, bits[7:0]  = byte value
 *   Match:       bits[31]   = 1, bits[24:16] = length (3-258),
 *                                bits[15:0]  = distance (1-32768)
 */
struct Token {
    uint32_t data;

    static Token literal(uint8_t byte) noexcept {
        return { static_cast<uint32_t>(byte) };
    }
    static Token match(uint16_t len, uint16_t dist) noexcept {
        return { 0x80000000U
               | (static_cast<uint32_t>(len)  << 16)
               | static_cast<uint32_t>(dist) };
    }

    bool     is_match()   const noexcept { return (data >> 31) != 0; }
    bool     is_literal() const noexcept { return !is_match(); }
    uint8_t  literal()    const noexcept { return static_cast<uint8_t>(data); }
    uint16_t length()     const noexcept { return static_cast<uint16_t>((data >> 16) & 0x1FF); }
    uint16_t distance()   const noexcept { return static_cast<uint16_t>(data & 0xFFFF); }
};

/* ── Level configuration ─────────────────────────────────────────────────── */

struct LZ77Config {
    int  max_chain;      /* max hash chain traversal steps */
    int  nice_len;       /* stop searching if match >= this length */
    int  lazy_depth;     /* lazy match lookahead depth (0 = greedy) */
    bool bt4;            /* use binary-tree match finder (level 10+) */
    bool fast_path;      /* L1-L3: head-only lookup, skip prev[], skip covered-pos hashing */
    int  hash_bits;      /* runtime hash table bits: 12 (L1), 14 (L2-3), 16 (L4+) */
};

LZ77Config lz77_config_for_level(int level);

/* ── Hash chain state ────────────────────────────────────────────────────── */

struct LZ77State {
    /* head[hash] = most recent position for this hash value */
    uint16_t head[LZ77_HASH_SIZE];
    /* prev[pos & WIN_MASK] = previous position in chain */
    uint16_t prev[LZ77_WIN_SIZE];

    void reset(int hash_bits = LZ77_HASH_BITS) noexcept {
        /* Zero only the portion of head[] used by the configured hash table.
         * At L1-L3 (hash_bits=12/14) this is 8-32 KB instead of 128 KB. */
        __builtin_memset(head, 0, sizeof(uint16_t) * (1u << hash_bits));
        __builtin_memset(prev, 0, sizeof(prev));
    }
};

/* ── Hash functions ──────────────────────────────────────────────────────── */

inline uint32_t lz77_hash4(const uint8_t* p) noexcept {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    /* Multiplicative hash: good distribution, single multiply */
    return (v * 0x1E35A7BDU) >> (32 - LZ77_HASH_BITS);
}

/* Runtime-configurable variant for fast-path with reduced hash_bits */
inline uint32_t lz77_hash4_n(const uint8_t* p, int bits) noexcept {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return (v * 0x1E35A7BDU) >> (32 - bits);
}

/* ── Scalar match length ─────────────────────────────────────────────────── */

/**
 * Returns the length of the common prefix of a[] and b[], up to max_len.
 * This is the scalar fallback; SIMD paths override via simd_dispatch.hpp.
 */
int match_length_scalar(
    const uint8_t* a,
    const uint8_t* b,
    int max_len) noexcept;

/* ── LZ77 compression ────────────────────────────────────────────────────── */

/**
 * Compress src[0..src_len) using LZ77, appending tokens to out[].
 *
 * state:       persistent hash chain (caller manages lifetime)
 * out:         output token array (caller ensures capacity >= src_len)
 * config:      compression parameters
 * dict:        optional preset dictionary (last 32KB of previous block)
 * dict_len:    length of dict (0 = none)
 *
 * Returns number of tokens emitted.
 */
size_t lz77_compress(
    const uint8_t* src, size_t src_len,
    Token* out,
    LZ77State& state,
    const LZ77Config& config,
    const uint8_t* dict    = nullptr,
    size_t         dict_len = 0);

/**
 * Insert entries into hash table without emitting tokens (dictionary load).
 */
void lz77_insert_dict(
    const uint8_t* dict, size_t dict_len,
    LZ77State& state);

} /* namespace deflate */
