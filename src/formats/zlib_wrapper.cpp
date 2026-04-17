#include "zlib_wrapper.hpp"
#include "raw_deflate.hpp"
#include "../simd/simd_dispatch.hpp"

#include <cstring>

namespace deflate {

/* zlib header:
 *   CMF: 0x78 = deflate (CM=8), window=32KB (CINFO=7)
 *   FLG: set so that CMF*256+FLG is divisible by 31
 *   FCHECK: (0x78 * 256 + FLG) % 31 == 0
 *   0x78 * 256 = 30720; 30720 % 31 = 2; so FLG = 31 - 2 = 29 → 0x9C
 *   Default: CMF=0x78, FLG=0x9C (no dict, level 6)
 *
 *   For level >=6: FLG bits [7:6] = 10 (best) → 0xDA
 *   For level 1-5: FLG bits [7:6] = 01 (fast)  → 0x5E after adjustment
 *
 *   Simplified: always use 0x78 0x9C (interoperable).
 */
static constexpr uint8_t ZLIB_CMF = 0x78;
static constexpr uint8_t ZLIB_FLG = 0x9C;

size_t zlib_compress_bound(size_t src_len) {
    return raw_compress_bound(src_len) + 6;  /* 2 header + 4 adler32 */
}

size_t zlib_compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    int level)
{
    if (dst_capacity < zlib_compress_bound(src_len)) return 0;

    /* Write header */
    dst[0] = ZLIB_CMF;
    dst[1] = ZLIB_FLG;

    /* Compress raw deflate into dst+2 */
    const size_t raw_n = raw_compress(src, src_len, dst + 2, dst_capacity - 6, level);
    if (raw_n == 0) return 0;

    /* Adler-32 trailer (big-endian) */
    const uint32_t adler = simd_adler32_fn()(1, src, src_len);
    const size_t   end   = 2 + raw_n;
    dst[end + 0] = static_cast<uint8_t>(adler >> 24);
    dst[end + 1] = static_cast<uint8_t>(adler >> 16);
    dst[end + 2] = static_cast<uint8_t>(adler >>  8);
    dst[end + 3] = static_cast<uint8_t>(adler);

    return end + 4;
}

deflate_result zlib_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    size_t*        actual_out_size)
{
    if (src_len < 6) return DEFLATE_DATA_ERROR;

    /* Validate header */
    if ((src[0] & 0x0F) != 8) return DEFLATE_DATA_ERROR;  /* CM must be 8 */
    if (((static_cast<uint32_t>(src[0]) << 8) | src[1]) % 31 != 0)
        return DEFLATE_DATA_ERROR;
    if (src[1] & 0x20) return DEFLATE_DATA_ERROR;  /* no preset dict support */

    /* Decompress */
    const deflate_result r = raw_decompress(
        src + 2, src_len - 6,
        dst, dst_capacity,
        actual_out_size);
    if (r != DEFLATE_OK) return r;

    /* Verify Adler-32 */
    const uint8_t* trailer = src + src_len - 4;
    const uint32_t expected =
          (static_cast<uint32_t>(trailer[0]) << 24)
        | (static_cast<uint32_t>(trailer[1]) << 16)
        | (static_cast<uint32_t>(trailer[2]) <<  8)
        |  static_cast<uint32_t>(trailer[3]);
    const uint32_t actual = simd_adler32_fn()(1, dst, *actual_out_size);
    if (actual != expected) return DEFLATE_DATA_ERROR;

    return DEFLATE_OK;
}

} /* namespace deflate */
