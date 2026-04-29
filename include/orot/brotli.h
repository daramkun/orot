#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * orot Brotli API
 *
 * Brotli stream support scaffold. The public API is available so callers can
 * compile against it while the encoder/decoder implementation is filled in.
 *
 * Return convention: non-negative = byte count written; negative = error.
 *   -1: general error (bad parameter, null pointer)
 *   -2: output buffer too small
 *   -3: data error (malformed input on decompress)
 *   -4: feature not implemented yet
 *
 * Compression quality: 0 (fastest) ... 11 (best compression)
 * Window size lgwin: 10 ... 24
 */

#define OROT_BROTLI_QUALITY_FAST    1
#define OROT_BROTLI_QUALITY_DEFAULT 5
#define OROT_BROTLI_QUALITY_MAX     11

#define OROT_BROTLI_LGWIN_MIN       10
#define OROT_BROTLI_LGWIN_DEFAULT   22
#define OROT_BROTLI_LGWIN_MAX       24

/**
 * Conservative upper bound on compressed output size.
 */
size_t orot_brotli_compress_bound(size_t src_size);

/**
 * Compress src into Brotli stream format.
 */
int orot_brotli_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         quality,
    int         lgwin);

/**
 * Decompress a Brotli stream.
 * uncompressed_size_out: if non-NULL, receives the decompressed byte count.
 */
int orot_brotli_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    size_t*     uncompressed_size_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* C++ convenience API */

#ifdef __cplusplus
#include <vector>
#include <span>
#include <cstdint>

namespace orot { namespace brotli_api {

template <typename Container = std::vector<uint8_t>>
Container compress(std::span<const uint8_t> src,
                   int quality = OROT_BROTLI_QUALITY_DEFAULT,
                   int lgwin = OROT_BROTLI_LGWIN_DEFAULT) {
    Container out(orot_brotli_compress_bound(src.size()));
    int n = orot_brotli_compress(src.data(), src.size(),
                                 out.data(), out.size(),
                                 quality, lgwin);
    if (n <= 0) return {};
    out.resize((size_t)n);
    return out;
}

template <typename Container = std::vector<uint8_t>>
Container decompress(std::span<const uint8_t> src, size_t dst_cap) {
    Container out(dst_cap);
    int n = orot_brotli_decompress(src.data(), src.size(),
                                   out.data(), out.size(), nullptr);
    if (n <= 0) return {};
    out.resize((size_t)n);
    return out;
}

} } /* namespace orot::brotli_api */
#endif /* __cplusplus */
