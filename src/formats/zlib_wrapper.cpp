#include "zlib_wrapper.hpp"
#include "raw_deflate.hpp"
#include "../simd/simd_dispatch.hpp"

#include <cstring>

namespace orot { namespace deflate {

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

static bool try_stored_zlib_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_capacity,
    size_t* actual_out_size) noexcept
{
    const uint8_t* ip = src + 2;
    const uint8_t* const end = src + src_len - 4;
    uint8_t* op = dst;
    uint8_t* const op_end = dst + dst_capacity;
    uint32_t adler = 1;

    for (;;) {
        if (ip + 5 > end) return false;
        const uint8_t hdr = *ip++;
        if ((hdr & 0x06) != 0) return false;
        if ((hdr & 0xF8) != 0) return false;

        const uint16_t len = static_cast<uint16_t>(
            static_cast<uint16_t>(ip[0]) | (static_cast<uint16_t>(ip[1]) << 8));
        const uint16_t nlen = static_cast<uint16_t>(
            static_cast<uint16_t>(ip[2]) | (static_cast<uint16_t>(ip[3]) << 8));
        ip += 4;
        if (static_cast<uint16_t>(len ^ 0xFFFFU) != nlen) return false;
        if (ip + len > end) return false;
        if (op + len > op_end) return false;

        adler = simd_adler32_fn()(adler, ip, len);
        std::memcpy(op, ip, len);
        ip += len;
        op += len;

        if ((hdr & 1) != 0)
            break;
    }

    if (ip != end) return false;

    const uint8_t* trailer = src + src_len - 4;
    const uint32_t expected =
          (static_cast<uint32_t>(trailer[0]) << 24)
        | (static_cast<uint32_t>(trailer[1]) << 16)
        | (static_cast<uint32_t>(trailer[2]) <<  8)
        |  static_cast<uint32_t>(trailer[3]);
    const size_t decoded_size = static_cast<size_t>(op - dst);
    if (adler != expected) return false;

    *actual_out_size = decoded_size;
    return true;
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

    if (try_stored_zlib_decompress(src, src_len, dst, dst_capacity, actual_out_size))
        return DEFLATE_OK;

    /* Decompress — retrieve incremental adler when available (STORED-only streams) */
    uint32_t inc_adler  = 1;
    bool     adler_exact = false;
    const deflate_result r = raw_decompress_ex(
        src + 2, src_len - 6,
        dst, dst_capacity,
        actual_out_size,
        &inc_adler, &adler_exact);
    if (r != DEFLATE_OK) return r;

    /* Verify Adler-32: use incremental value for STORED-only streams,
     * recompute over output for Huffman streams. */
    const uint8_t* trailer = src + src_len - 4;
    const uint32_t expected =
          (static_cast<uint32_t>(trailer[0]) << 24)
        | (static_cast<uint32_t>(trailer[1]) << 16)
        | (static_cast<uint32_t>(trailer[2]) <<  8)
        |  static_cast<uint32_t>(trailer[3]);
    const uint32_t actual = adler_exact
        ? inc_adler
        : simd_adler32_fn()(1, dst, *actual_out_size);
    if (actual != expected) return DEFLATE_DATA_ERROR;

    return DEFLATE_OK;
}

} } /* namespace orot::deflate */
