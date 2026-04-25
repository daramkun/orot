#include "lzma2_compress.hpp"
#include "lzma_compress.hpp"

#include <cstring>
#include <memory>
#include <new>

namespace orot { namespace lzma {

/* ── LZMA2 format ────────────────────────────────────────────────────────────
 * Chunk header byte:
 *   0x00        → end-of-stream
 *   0x01        → uncompressed chunk, reset dict
 *   0x02        → uncompressed chunk, keep dict
 *   0x80..0xFF  → LZMA chunk
 *     bits[7:6] = 0b10
 *     bit[5]    = props reset (1 = write new props byte, 0 = reuse)
 *     bit[4]    = state reset (1 = reset state, 0 = keep)
 *     bit[3]    = dict reset (1 = reset dict, 0 = keep)
 *     bits[1:0] = reserved 0
 *
 * For simplicity we emit:
 *   LZMA chunks: header = 0x80 | 0x20 | 0x10 | 0x08 (props+state+dict reset on first)
 *                then    0x80 | 0x20               (props reset only, on subsequent)
 * Uncompressed: header = 0x01 or 0x02
 *
 * Chunk data (LZMA):
 *   [1B header]
 *   [2B compressed size - 1 (LE)]
 *   [2B uncompressed size - 1 (LE)]
 *   [if props reset: 1B props]
 *   [LZMA data without LZMA-alone 13-byte header]
 *
 * Chunk data (uncompressed):
 *   [1B header]
 *   [2B uncompressed size - 1 (LE)]
 *   [raw bytes]
 * ─────────────────────────────────────────────────────────────────────────── */

static constexpr size_t kChunkSize = 1u << 16;  /* 64 KB uncompressed per chunk */

size_t lzma2_compress_bound(size_t src_len) noexcept {
    /* Each 64KB chunk: 5-byte overhead + up to ~65536*1.001 bytes compressed */
    size_t chunks = (src_len + kChunkSize - 1) / kChunkSize + 1;
    return chunks * (5 + kChunkSize + (kChunkSize >> 10) + 64) + 1 /* EOS */;
}

size_t lzma2_compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_cap,
    int            level) noexcept
{
    if (dst_cap < lzma2_compress_bound(src_len)) return 0;

    /* Empty input: just emit EOS marker */
    if (src_len == 0) {
        dst[0] = 0x00;
        return 1;
    }

    LzmaConfig cfg = lzma_config_for_level(level);

    /* Props byte for LZMA (same encoding as LZMA alone header byte 0) */
    uint8_t props_byte = (uint8_t)((cfg.pb * 5 + cfg.lp) * 9 + cfg.lc);

    /* Allocate temporary buffer for per-chunk compression */
    size_t chunk_buf_cap = lzma_compress_bound(kChunkSize);
    std::unique_ptr<uint8_t[]> chunk_buf(new (std::nothrow) uint8_t[chunk_buf_cap]);
    if (!chunk_buf) return 0;

    uint8_t* out     = dst;
    uint8_t* out_end = dst + dst_cap;
    bool first_chunk = true;

    size_t pos = 0;
    while (pos < src_len) {
        size_t chunk_in = std::min(src_len - pos, kChunkSize);
        const uint8_t* chunk_src = src + pos;

        /* Try LZMA compression of this chunk.
         * lzma_compress outputs LZMA alone format:
         *   [13B header][5B RC init][RC data]
         * For LZMA2 we strip the 13-byte header. */
        size_t lzma_out = lzma_compress(chunk_src, chunk_in,
                                        chunk_buf.get(), chunk_buf_cap,
                                        level);

        /* If LZMA expands, emit as uncompressed chunk */
        if (lzma_out == 0 || lzma_out >= chunk_in + 5) {
            /* Uncompressed chunk */
            if (out + 3 + chunk_in > out_end) return 0;
            *out++ = first_chunk ? 0x01 : 0x02;
            uint16_t usz16 = (uint16_t)(chunk_in - 1);
            memcpy(out, &usz16, 2); out += 2;
            memcpy(out, chunk_src, chunk_in); out += chunk_in;
        } else {
            /* LZMA chunk: chunk_buf has [13B header][5B RC-init][data...] */
            /* Strip the 13-byte LZMA-alone header, keep RC data */
            const uint8_t* rc_data = chunk_buf.get() + 13;
            size_t rc_size = lzma_out - 13;

            if (out + 6 + (first_chunk ? 1 : 0) + rc_size > out_end) return 0;

            /* Header byte */
            uint8_t hdr = 0x80u;
            if (first_chunk) {
                hdr |= 0x20 | 0x10 | 0x08;  /* props + state + dict reset */
            } else {
                hdr |= 0x20;  /* props reset (re-write props on each chunk for simplicity) */
            }
            *out++ = hdr;

            uint16_t csz16 = (uint16_t)(rc_size - 1);
            uint16_t usz16 = (uint16_t)(chunk_in - 1);
            memcpy(out, &csz16, 2); out += 2;
            memcpy(out, &usz16, 2); out += 2;

            if (hdr & 0x20) {
                *out++ = props_byte;
            }

            memcpy(out, rc_data, rc_size);
            out += rc_size;
        }

        first_chunk = false;
        pos += chunk_in;
    }

    /* End-of-stream */
    if (out >= out_end) return 0;
    *out++ = 0x00;

    return (size_t)(out - dst);
}

} } /* namespace orot::lzma */
