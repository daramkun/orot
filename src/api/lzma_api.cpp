#include "orot/lzma.h"
#include "lzma/lzma_compress.hpp"
#include "lzma/lzma_decompress.hpp"
#include "lzma/lzma2_compress.hpp"
#include "lzma/lzma2_decompress.hpp"

#include <cstring>
#include <climits>

using namespace orot::lzma;

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
