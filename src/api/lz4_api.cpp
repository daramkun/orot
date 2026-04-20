#include "../../include/orot/lz4.h"
#include "../lz4/lz4_block.hpp"
#include "../lz4/lz4_frame.hpp"

#include <memory>
#include <new>

using namespace orot::lz4;

/* ── Raw block ───────────────────────────────────────────────────────────── */

int orot_lz4_compress_bound(int src_size) {
    return lz4_block_compress_bound(src_size);
}

int orot_lz4_compress(
    const void* src, int src_size,
    void*       dst, int dst_cap,
    int level)
{
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
