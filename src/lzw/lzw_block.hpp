#pragma once

#include <cstdint>
#include <cstddef>

namespace orot { namespace lzw {

/* ── Constants ───────────────────────────────────────────────────────────── */

static constexpr int LZW_CLEAR_CODE = 256;
static constexpr int LZW_EOI_CODE   = 257;
static constexpr int LZW_FIRST_CODE = 258;
static constexpr int LZW_MIN_BITS   = 9;
static constexpr int LZW_MAX_BITS   = 16;
static constexpr int LZW_DEF_BITS   = 12;

/* ── Configuration ───────────────────────────────────────────────────────── */

struct LZWConfig {
    int max_bits;  /* 9..16, default 12 */
};

inline LZWConfig lzw_config_default() noexcept {
    return LZWConfig{ LZW_DEF_BITS };
}

/* ── API ─────────────────────────────────────────────────────────────────── */

/**
 * Conservative upper bound on lzw_compress output size.
 * Accounts for 1-byte header storing max_bits.
 */
int lzw_compress_bound(int src_len, int max_bits) noexcept;

/**
 * Compress src into dst using variable-width LZW codes.
 *
 * Output format: [1 byte: max_bits] [LSB-first variable-width codes]
 * Returns bytes written to dst, or:
 *   -1: dst too small
 *   -2: invalid config
 */
int lzw_compress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap,
    const LZWConfig& cfg) noexcept;

/**
 * Decompress LZW-compressed data from src into dst.
 * Reads max_bits from the 1-byte header embedded in src.
 *
 * Returns bytes written to dst, or:
 *   -1: malformed input
 *   -2: dst too small
 */
int lzw_decompress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap) noexcept;

} } /* namespace orot::lzw */
