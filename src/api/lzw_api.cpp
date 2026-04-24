#include "orot/lzw.h"
#include "lzw/lzw_block.hpp"

#include <cstdint>

extern "C" {

int orot_lzw_compress_bound(int src_size) {
    return orot::lzw::lzw_compress_bound(src_size, orot::lzw::LZW_DEF_BITS);
}

int orot_lzw_compress(
    const void* src, int src_size,
    void*       dst, int dst_cap,
    int         max_bits)
{
    if (max_bits == 0) max_bits = orot::lzw::LZW_DEF_BITS;
    if (max_bits < orot::lzw::LZW_MIN_BITS ||
        max_bits > orot::lzw::LZW_MAX_BITS) return -1;

    orot::lzw::LZWConfig cfg{ max_bits };
    return orot::lzw::lzw_compress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        cfg);
}

int orot_lzw_decompress(
    const void* src, int src_size,
    void*       dst, int dst_cap)
{
    return orot::lzw::lzw_decompress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap);
}

} /* extern "C" */
