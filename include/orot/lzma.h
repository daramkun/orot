#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * orot LZMA / LZMA2 API
 *
 * Two variants:
 *   orot_lzma_*  — LZMA "alone" format (props + dict_size + uncompressed_size header)
 *   orot_lzma2_* — LZMA2 chunk stream format
 *
 * Return convention: non-negative = byte count written; negative = error.
 *   -1: general error (bad level, null pointer, internal failure)
 *   -2: output buffer too small
 *   -3: data error (malformed input on decompress)
 *
 * Compression levels: 1 (fastest) … 9 (best compression)
 */

/* ── Level constants ─────────────────────────────────────────────────────── */

#define OROT_LZMA_LEVEL_FAST    1
#define OROT_LZMA_LEVEL_DEFAULT 5
#define OROT_LZMA_LEVEL_MAX     9

/* ── LZMA (alone format) ─────────────────────────────────────────────────── */

/**
 * Conservative upper bound on orot_lzma_compress output size.
 */
size_t orot_lzma_compress_bound(size_t src_size);

/**
 * Compress src_size bytes into LZMA alone format.
 *
 * Output is a self-contained LZMA stream compatible with tools that accept
 * the .lzma format (xz --format=lzma, lzma CLI, etc.).
 *
 * level: 1 (fastest) … 9 (best compression)
 */
int orot_lzma_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         level);

/**
 * Decompress an LZMA alone stream.
 *
 * uncompressed_size_out: if non-NULL, receives the uncompressed byte count
 *                        (read from the embedded header; may be used to
 *                        pre-allocate dst).
 */
int orot_lzma_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    size_t*     uncompressed_size_out);

/* ── LZMA2 (chunk stream format) ─────────────────────────────────────────── */

/**
 * Conservative upper bound on orot_lzma2_compress output size.
 */
size_t orot_lzma2_compress_bound(size_t src_size);

/**
 * Compress src_size bytes into an LZMA2 chunk stream.
 *
 * LZMA2 is used as the inner stream in .xz containers.
 * Each 64 KB block is compressed independently, falling back to
 * uncompressed if the compressed form is larger.
 *
 * level: 1 (fastest) … 9 (best compression)
 */
int orot_lzma2_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         level);

/**
 * Decompress an LZMA2 chunk stream.
 */
int orot_lzma2_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ── C++ convenience API ─────────────────────────────────────────────────── */

#ifdef __cplusplus
#include <vector>
#include <span>
#include <cstdint>

namespace orot { namespace lzma_api {

template <typename Container = std::vector<uint8_t>>
Container compress(std::span<const uint8_t> src, int level = OROT_LZMA_LEVEL_DEFAULT) {
    Container out(orot_lzma_compress_bound(src.size()));
    int n = orot_lzma_compress(src.data(), src.size(),
                               out.data(), out.size(), level);
    if (n <= 0) return {};
    out.resize((size_t)n);
    return out;
}

template <typename Container = std::vector<uint8_t>>
Container decompress(std::span<const uint8_t> src, size_t max_size = 0) {
    size_t usz = 0;
    if (orot_lzma_decompress(src.data(), src.size(), nullptr, 0, &usz) < 0
        && usz == 0)
        return {};
    if (max_size > 0 && usz > max_size) return {};
    Container out(usz);
    int n = orot_lzma_decompress(src.data(), src.size(),
                                 out.data(), out.size(), nullptr);
    if (n <= 0) return {};
    out.resize((size_t)n);
    return out;
}

template <typename Container = std::vector<uint8_t>>
Container compress2(std::span<const uint8_t> src, int level = OROT_LZMA_LEVEL_DEFAULT) {
    Container out(orot_lzma2_compress_bound(src.size()));
    int n = orot_lzma2_compress(src.data(), src.size(),
                                out.data(), out.size(), level);
    if (n <= 0) return {};
    out.resize((size_t)n);
    return out;
}

template <typename Container = std::vector<uint8_t>>
Container decompress2(std::span<const uint8_t> src, size_t dst_cap) {
    Container out(dst_cap);
    int n = orot_lzma2_decompress(src.data(), src.size(),
                                  out.data(), out.size());
    if (n <= 0) return {};
    out.resize((size_t)n);
    return out;
}

} } /* namespace orot::lzma_api */
#endif /* __cplusplus */
