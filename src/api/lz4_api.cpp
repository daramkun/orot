#include "../../include/orot/lz4.h"
#include "../lz4/lz4_block.hpp"
#include "../lz4/lz4_frame.hpp"

#if defined(OROT_HAS_LZ4_BACKEND)
#include <lz4.h>
#include <lz4frame.h>
#endif

#include <memory>
#include <new>
#include <cstdint>

using namespace orot::lz4;

/* ── Raw block ───────────────────────────────────────────────────────────── */

int orot_lz4_compress_bound(int src_size) {
#if defined(OROT_HAS_LZ4_BACKEND)
    return LZ4_compressBound(src_size);
#else
    return lz4_block_compress_bound(src_size);
#endif
}

int orot_lz4_compress(
    const void* src, int src_size,
    void*       dst, int dst_cap,
    int level)
{
#if defined(OROT_HAS_LZ4_BACKEND)
    if (src_size < 0 || dst_cap < 0 || (src_size > 0 && !src) || !dst)
        return -1;
    const int acceleration = level <= 1 ? 2 : 1;
    int n = LZ4_compress_fast(
        static_cast<const char*>(src), static_cast<char*>(dst),
        src_size, dst_cap, acceleration);
    if (n > 0) return n;
#endif

    std::unique_ptr<LZ4State> state(new (std::nothrow) LZ4State);
    if (!state) return -1;

    LZ4Config cfg = lz4_config_for_level(level);
    return lz4_block_compress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        *state, cfg);
}

int orot_lz4_decompress(
    const void* src, int src_size,
    void*       dst, int dst_cap)
{
#if defined(OROT_HAS_LZ4_BACKEND)
    if (src_size < 0 || dst_cap < 0 || (src_size > 0 && !src) || !dst)
        return -1;
    int n = LZ4_decompress_safe(
        static_cast<const char*>(src), static_cast<char*>(dst),
        src_size, dst_cap);
    if (n >= 0) return n;
#endif

    return lz4_block_decompress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap);
}

/* ── Frame ───────────────────────────────────────────────────────────────── */

int orot_lz4f_compress_bound(int src_size) {
    return lz4f_compress_bound(src_size);
}

int orot_lz4f_compress(
    const void* src, int src_size,
    void*       dst, int dst_cap,
    int level)
{
    return lz4f_compress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        level);
}

int orot_lz4f_decompress(
    const void* src, int src_size,
    void*       dst, int dst_cap)
{
    return lz4f_decompress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap);
}
