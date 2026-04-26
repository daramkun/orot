#include "orot/zstd.h"
#include "zstd/zstd.hpp"

#include <cstdint>

extern "C" {

int orot_zstd_compress_bound(int src_size) {
    return orot::zstd::zstd_compress_bound(src_size);
}

int orot_zstd_compress(
    const void* src, int src_size,
    void*       dst, int dst_cap,
    int         level)
{
    return orot::zstd::zstd_compress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap,
        level);
}

int orot_zstd_decompress(
    const void* src, int src_size,
    void*       dst, int dst_cap)
{
    return orot::zstd::zstd_decompress(
        static_cast<const uint8_t*>(src), src_size,
        static_cast<uint8_t*>(dst), dst_cap);
}

} /* extern "C" */
