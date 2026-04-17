#include "raw_deflate.hpp"
#include "../compress/block_compressor.hpp"
#include "../decompress/decompressor.hpp"

#include <cstring>

namespace deflate {

size_t raw_compress_bound(size_t src_len) {
    /* Worst case: stored blocks, 5 bytes overhead per 65535 bytes */
    return src_len + (src_len >> 12) + (src_len >> 14) + (src_len >> 25) + 13;
}

size_t raw_compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    int level,
    bool is_last)
{
    if (dst_capacity < raw_compress_bound(src_len)) return 0;
    BlockCompressor c(level);
    return c.compress(src, src_len, dst, dst_capacity, is_last);
}

deflate_result raw_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    size_t*        actual_out_size)
{
    Decompressor d;
    size_t avail_in  = src_len;
    size_t avail_out = dst_capacity;
    deflate_result r = d.decompress(&src, &avail_in, &dst, &avail_out);
    if (r == DEFLATE_STREAM_END) {
        *actual_out_size = dst_capacity - avail_out;
        return DEFLATE_OK;
    }
    /* Output buffer exactly full: retry with a scratch byte to drain EOB from
     * the bit buffer.  The decompressor returns DEFLATE_NEED_OUTPUT when
     * avail_out reaches 0 before it processes the end-of-block symbol.  A
     * single extra call with 1 byte of space lets it finish cleanly. */
    if (r == DEFLATE_NEED_OUTPUT && avail_out == 0 && avail_in == 0) {
        uint8_t scratch = 0;
        uint8_t* sp = &scratch;
        size_t   so = 1;
        r = d.decompress(&src, &avail_in, &sp, &so);
        if (r == DEFLATE_STREAM_END && so == 1) {
            /* scratch was not written: EOB decoded with zero output */
            *actual_out_size = dst_capacity;
            return DEFLATE_OK;
        }
    }
    return (r < 0) ? r : DEFLATE_DATA_ERROR;
}

} /* namespace deflate */
