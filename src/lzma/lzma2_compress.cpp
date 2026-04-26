#include "lzma2_compress.hpp"

#include <cstring>
#include <algorithm>

namespace orot { namespace lzma {

/* ── LZMA2 format ────────────────────────────────────────────────────────────
 * Chunk header byte:
 *   0x00        → end-of-stream
 *   0x01        → uncompressed chunk, reset dict
 *   0x02        → uncompressed chunk, keep dict
 * Chunk data (uncompressed):
 *   [1B header]
 *   [2B uncompressed size - 1 (BE)]
 *   [raw bytes]
 *
 * We currently emit valid uncompressed LZMA2 chunks only. The decoder accepts
 * both uncompressed chunks and LZMA chunks produced by liblzma.
 * ─────────────────────────────────────────────────────────────────────────── */

static constexpr size_t kChunkSize = 1u << 16;  /* 64 KB uncompressed per chunk */

size_t lzma2_compress_bound(size_t src_len) noexcept {
    /* Each 64KB uncompressed chunk has 3 bytes overhead plus one EOS byte. */
    size_t chunks = (src_len + kChunkSize - 1) / kChunkSize + 1;
    return src_len + chunks * 3 + 1;
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

    (void)level;

    uint8_t* out     = dst;
    uint8_t* out_end = dst + dst_cap;
    bool first_chunk = true;

    size_t pos = 0;
    while (pos < src_len) {
        size_t chunk_in = std::min(src_len - pos, kChunkSize);
        const uint8_t* chunk_src = src + pos;

        /* Emit independent uncompressed chunks. This is a valid LZMA2 stream
         * and keeps orot output accepted by strict liblzma decoders while the
         * standalone LZMA encoder is not yet fully canonical. */
        if (out + 3 + chunk_in > out_end) return 0;
        *out++ = first_chunk ? 0x01 : 0x02;
        uint16_t usz16 = (uint16_t)(chunk_in - 1);
        *out++ = (uint8_t)(usz16 >> 8);
        *out++ = (uint8_t)usz16;
        memcpy(out, chunk_src, chunk_in); out += chunk_in;
        first_chunk = false;
        pos += chunk_in;
    }

    /* End-of-stream */
    if (out >= out_end) return 0;
    *out++ = 0x00;

    return (size_t)(out - dst);
}

} } /* namespace orot::lzma */
