#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace orot { namespace lz4 {

/* ── Constants ───────────────────────────────────────────────────────────── */

static constexpr int LZ4_MIN_MATCH  = 4;
static constexpr int LZ4_WIN_SIZE   = 65536;
static constexpr int LZ4_WIN_MASK   = LZ4_WIN_SIZE - 1;
static constexpr int LZ4_HASH_BITS  = 16;
static constexpr int LZ4_HASH_SIZE  = (1 << LZ4_HASH_BITS);
static constexpr int LZ4_LAST_LIT   = 5;   /* last 5 bytes must be literals */
static constexpr int LZ4_LAST_MATCH = 12;  /* last match must end >= 12 from end */

/* ── Level configuration ─────────────────────────────────────────────────── */

struct LZ4Config {
    bool fast_path;  /* L1-3: head-only lookup */
    int  max_chain;  /* L4+: chain traversal limit */
    int  miss_limit; /* consecutive 4-byte misses before abandon */
    int  nice_len;   /* stop searching if match >= this */
};

LZ4Config lz4_config_for_level(int level) noexcept;

/* ── Hash chain state ────────────────────────────────────────────────────── */

struct LZ4State {
    uint32_t head[LZ4_HASH_SIZE]; /* most recent position for each 4-gram hash */
    uint32_t prev[LZ4_WIN_SIZE];  /* previous chain pos (HC mode only) */

    void reset() noexcept {
        __builtin_memset(head, 0, sizeof(head));
        __builtin_memset(prev, 0, sizeof(prev));
    }

    void reset_fast() noexcept {
        __builtin_memset(head, 0, sizeof(head));
    }
};

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Maximum compressed size for a raw LZ4 block.
 * Worst case: src_len + (src_len / 255) + 16.
 */
int lz4_block_compress_bound(int src_len) noexcept;

/**
 * Compress src into a raw LZ4 block (no frame header).
 *
 * Returns number of bytes written to dst, or negative on error:
 *   -1: dst too small
 */
int lz4_block_compress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap,
    LZ4State& state, const LZ4Config& cfg) noexcept;

/**
 * Decompress a raw LZ4 block.
 *
 * Returns number of bytes written to dst, or negative on error:
 *   -1: malformed input
 *   -2: dst too small
 */
int lz4_block_decompress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap) noexcept;

/**
 * Decompress a raw LZ4 block while allowing matches to reference bytes in the
 * already-produced output prefix [prefix_base, dst). Used by linked LZ4 frames.
 */
int lz4_block_decompress_with_prefix(
    const uint8_t* src, int src_len,
    uint8_t* prefix_base,
    uint8_t* dst, int dst_cap) noexcept;

} } /* namespace orot::lz4 */
