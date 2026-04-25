#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * orot Bzip2 API
 *
 * BWT + MTF + Huffman (multi-table) based compression.
 * Compatible with the .bz2 file format.
 *
 * Return convention: non-negative = byte count written; negative = error.
 *   -1: general error (bad level, null pointer, internal failure)
 *   -2: output buffer too small
 *   -3: data error (malformed input on decompress)
 *
 * Compression levels: 1 (fastest, 100KB blocks) … 9 (best, 900KB blocks)
 */

#define OROT_BZIP2_LEVEL_FAST    1
#define OROT_BZIP2_LEVEL_DEFAULT 9
#define OROT_BZIP2_LEVEL_MAX     9

/**
 * Conservative upper bound on compressed output size.
 */
size_t orot_bzip2_compress_bound(size_t src_size);

/**
 * Compress src into bzip2 (.bz2) format.
 * level: 1 (fastest) … 9 (best compression)
 */
int orot_bzip2_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         level);

/**
 * Decompress a bzip2 stream.
 * uncompressed_size_out: if non-NULL, receives the decompressed byte count.
 */
int orot_bzip2_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    size_t*     uncompressed_size_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ── C++ convenience API ─────────────────────────────────────────────────── */

#ifdef __cplusplus
#include <vector>
#include <span>
#include <cstdint>

namespace orot { namespace bzip2_api {

template <typename Container = std::vector<uint8_t>>
Container compress(std::span<const uint8_t> src,
                   int level = OROT_BZIP2_LEVEL_DEFAULT) {
    Container out(orot_bzip2_compress_bound(src.size()));
    int n = orot_bzip2_compress(src.data(), src.size(),
                                out.data(), out.size(), level);
    if (n <= 0) return {};
    out.resize((size_t)n);
    return out;
}

template <typename Container = std::vector<uint8_t>>
Container decompress(std::span<const uint8_t> src, size_t dst_cap) {
    Container out(dst_cap);
    int n = orot_bzip2_decompress(src.data(), src.size(),
                                  out.data(), out.size(), nullptr);
    if (n <= 0) return {};
    out.resize((size_t)n);
    return out;
}

} } /* namespace orot::bzip2_api */
#endif /* __cplusplus */
