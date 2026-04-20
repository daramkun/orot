#pragma once

#include <cstdint>
#include <cstddef>

namespace orot { namespace lz4 {

/*
 * LZ4 Frame Format (LZ4 Frame Specification v1.6.2)
 *
 * [Magic:4][FLG:1][BD:1][HC:1]
 * ([Content Size:8])?
 * ([Block:4+data])* [EndMark:4]
 * ([Content Checksum:4])?
 */

static constexpr uint32_t LZ4F_MAGIC           = 0x184D2204U;
static constexpr uint32_t LZ4F_ENDMARK         = 0x00000000U;
static constexpr uint8_t  LZ4F_VERSION         = 0x40; /* version=01 in bits [7:6] */
static constexpr uint8_t  LZ4F_FLAG_CONTENT_CS = 0x04; /* bit 2: content checksum */
static constexpr uint8_t  LZ4F_BD_BLOCK_4MB    = 0x70; /* bits [6:4]=111 → 4MB */

/* Block uncompressed flag in the block size field */
static constexpr uint32_t LZ4F_BLOCK_UNCOMP    = 0x80000000U;

/* ── Frame compress ──────────────────────────────────────────────────────── */

/**
 * Maximum output size for lz4f_compress.
 * Returns a conservative upper bound.
 */
int lz4f_compress_bound(int src_len) noexcept;

/**
 * Compress src into a complete LZ4 frame (magic + header + blocks + endmark).
 * Includes content checksum.
 *
 * level: 1-9
 * Returns number of bytes written to dst, or -1 on error.
 */
int lz4f_compress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap,
    int level) noexcept;

/* ── Frame decompress ────────────────────────────────────────────────────── */

/**
 * Decompress a complete LZ4 frame.
 * Validates magic, header checksum, and optional content checksum.
 *
 * Returns number of bytes written to dst, or negative on error:
 *   -1: invalid frame (bad magic, header checksum, or block data)
 *   -2: dst too small
 *   -3: content checksum mismatch
 */
int lz4f_decompress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap) noexcept;

} } /* namespace orot::lz4 */
