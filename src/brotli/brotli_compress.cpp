#include "brotli_compress.hpp"

namespace orot { namespace brotli {

size_t brotli_compress_bound(size_t src_len) noexcept {
    /* Brotli has no tiny closed-form bound exposed by the format itself.
     * Keep a conservative bound for the future encoder scaffold. */
    return src_len + (src_len / 16) + 1024;
}

size_t brotli_compress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    int quality, int lgwin) noexcept
{
    (void)src;
    (void)src_len;
    (void)dst;
    (void)dst_cap;
    (void)quality;
    (void)lgwin;
    return 0;
}

} } /* namespace orot::brotli */
