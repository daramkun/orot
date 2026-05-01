#include "orot/lzma.h"
#include "lzma/lzma_compress.hpp"
#include "lzma/lzma_decompress.hpp"
#include "lzma/lzma2_compress.hpp"
#include "lzma/lzma2_decompress.hpp"

#if defined(OROT_HAS_LIBLZMA_BACKEND)
#include <lzma.h>
#endif

#include <cstring>
#include <climits>
#include <cstdint>

using namespace orot::lzma;

namespace {

#if defined(OROT_HAS_LIBLZMA_BACKEND)
static int liblzma_alone_decompress_backend(
    const void* src, size_t src_size,
    void* dst, size_t dst_cap,
    size_t* uncompressed_size_out)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&strm, UINT64_MAX) != LZMA_OK)
        return -1;

    strm.next_in = static_cast<const uint8_t*>(src);
    strm.avail_in = src_size;
    strm.next_out = static_cast<uint8_t*>(dst);
    strm.avail_out = dst_cap;

    lzma_ret ret = LZMA_OK;
    while (ret == LZMA_OK)
        ret = lzma_code(&strm, LZMA_RUN);

    const size_t out = strm.total_out;
    lzma_end(&strm);

    if (ret != LZMA_STREAM_END) {
        if (ret == LZMA_BUF_ERROR && out == dst_cap) return -2;
        return -3;
    }
    if (out > static_cast<size_t>(INT_MAX)) return -1;
    if (uncompressed_size_out) *uncompressed_size_out = out;
    return static_cast<int>(out);
}

static bool likely_incompressible_lzma(size_t compressed_size, uint64_t uncompressed_size) noexcept {
    if (uncompressed_size == 0 || uncompressed_size == UINT64_MAX) return false;
    if (uncompressed_size > static_cast<uint64_t>(SIZE_MAX)) return false;
    return compressed_size * 100 > static_cast<size_t>(uncompressed_size) * 95;
}
#endif

} // namespace

/* ── LZMA ─────────────────────────────────────────────────────────────────── */

size_t orot_lzma_compress_bound(size_t src_size) {
    return lzma_compress_bound(src_size);
}

int orot_lzma_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         level)
{
    if (src_size > 0 && !src) return -1;
    if (!dst) return -1;
    if (level < 1 || level > 9) return -1;

    size_t need = lzma_compress_bound(src_size);
    if (dst_cap < need) return -2;

    size_t n = lzma_compress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        level);

    if (n == 0) return -1;
    if (n > (size_t)INT_MAX) return -1;
    return (int)n;
}

int orot_lzma_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    size_t*     uncompressed_size_out)
{
    if (!src) return -1;
    if (src_size < 13) return -3;

    /* Extract uncompressed size from header */
    uint64_t usz = lzma_header_uncompressed_size(
        static_cast<const uint8_t*>(src));
    if (uncompressed_size_out)
        *uncompressed_size_out = (usz == (uint64_t)-1) ? 0 : (size_t)usz;

    /* Size query only */
    if (!dst || dst_cap == 0) {
        if (usz == (uint64_t)-1 || usz == 0xFFFFFFFFFFFFFFFFull) return -1;
        return 0;
    }

#if defined(OROT_HAS_LIBLZMA_BACKEND)
    if (likely_incompressible_lzma(src_size, usz)) {
        return liblzma_alone_decompress_backend(src, src_size, dst, dst_cap, uncompressed_size_out);
    }
#endif

    size_t n = lzma_decompress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap);

    if (n == 0) return -3;
    if (n > (size_t)INT_MAX) return -1;
    return (int)n;
}

/* ── LZMA2 ────────────────────────────────────────────────────────────────── */

size_t orot_lzma2_compress_bound(size_t src_size) {
    return lzma2_compress_bound(src_size);
}

int orot_lzma2_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         level)
{
    if (src_size > 0 && !src) return -1;
    if (!dst) return -1;
    if (level < 1 || level > 9) return -1;

    size_t need = lzma2_compress_bound(src_size);
    if (dst_cap < need) return -2;

    size_t n = lzma2_compress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        level);

    if (n == 0) return -1;
    if (n > (size_t)INT_MAX) return -1;
    return (int)n;
}

int orot_lzma2_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap)
{
    if (!src || !dst) return -1;

    size_t n = lzma2_decompress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap);

    if (n == 0) return -3;
    if (n > (size_t)INT_MAX) return -1;
    return (int)n;
}
