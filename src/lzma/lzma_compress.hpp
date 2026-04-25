#pragma once

#include <cstdint>
#include <cstddef>

namespace orot { namespace lzma {

/* ── LZMA compression level config ──────────────────────────────────────── */

struct LzmaConfig {
    uint32_t dict_size;  /* sliding window size (bytes, power of 2) */
    int      lc;         /* literal context bits (0-8, default 3) */
    int      lp;         /* literal position bits (0-4, default 0) */
    int      pb;         /* position bits (0-4, default 2) */
    int      nice_len;   /* nice match length (early exit) */
    int      depth;      /* hash chain search depth */
    int      hash_bits;  /* log2 of hash table size */
};

LzmaConfig lzma_config_for_level(int level) noexcept;

/* ── Compress / decompress bounds ────────────────────────────────────────── */

size_t lzma_compress_bound(size_t src_len) noexcept;

/* Compress src → dst in LZMA alone format.
 * Returns bytes written to dst, or 0 on error (dst too small, bad level). */
size_t lzma_compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_cap,
    int            level) noexcept;

} } /* namespace orot::lzma */
