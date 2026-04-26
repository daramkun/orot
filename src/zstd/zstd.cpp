#include "zstd.hpp"

#include <climits>

namespace orot { namespace zstd {

int zstd_compress_bound(int src_len) noexcept {
    if (src_len < 0) return -1;

    /*
     * Zstandard's final encoder can choose raw/RLE/compressed blocks. Until the
     * encoder exists, expose a conservative whole-frame bound that is large
     * enough for block headers and incompressible block overhead.
     */
    const int block_overhead = (src_len / 128) + 64;
    if (src_len > INT_MAX - block_overhead) return -1;
    return src_len + block_overhead;
}

int zstd_compress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap,
    int level) noexcept
{
    (void)src;
    (void)src_len;
    (void)dst;
    (void)dst_cap;
    (void)level;
    return -1;
}

int zstd_decompress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap) noexcept
{
    (void)src;
    (void)src_len;
    (void)dst;
    (void)dst_cap;
    return -1;
}

} } /* namespace orot::zstd */
