#include "gzip_wrapper.hpp"
#include "raw_deflate.hpp"
#include "../simd/simd_dispatch.hpp"

#include <cstring>

namespace orot { namespace deflate {

/* gzip magic + method + flags + mtime + xfl + os */
static constexpr uint8_t GZIP_MAGIC[2] = { 0x1F, 0x8B };
static constexpr uint8_t GZIP_METHOD   = 8;  /* deflate */
static constexpr uint8_t GZIP_OS       = 255;  /* unknown */

/* gzip header flags */
static constexpr uint8_t FNAME_FLAG = 0x08;

size_t gzip_compress_bound(size_t src_len) {
    return raw_compress_bound(src_len) + 18;  /* 10-byte header + 8-byte trailer */
}

size_t gzip_compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    int level,
    const char*    filename,
    uint32_t       mtime)
{
    if (dst_capacity < gzip_compress_bound(src_len)) return 0;

    uint8_t* p = dst;
    const uint8_t xfl = (level >= 9) ? 2 : (level <= 2) ? 4 : 0;
    const uint8_t flags = filename ? FNAME_FLAG : 0;

    /* Header */
    *p++ = GZIP_MAGIC[0];
    *p++ = GZIP_MAGIC[1];
    *p++ = GZIP_METHOD;
    *p++ = flags;
    *p++ = static_cast<uint8_t>(mtime);
    *p++ = static_cast<uint8_t>(mtime >> 8);
    *p++ = static_cast<uint8_t>(mtime >> 16);
    *p++ = static_cast<uint8_t>(mtime >> 24);
    *p++ = xfl;
    *p++ = GZIP_OS;

    /* Optional filename (null-terminated) */
    if (filename) {
        const size_t fname_len = std::strlen(filename) + 1;
        std::memcpy(p, filename, fname_len);
        p += fname_len;
    }

    /* Compressed data */
    const size_t used    = static_cast<size_t>(p - dst);
    const size_t raw_n   = raw_compress(src, src_len, p, dst_capacity - used - 8, level);
    if (raw_n == 0) return 0;
    p += raw_n;

    /* CRC-32 trailer (little-endian) */
    const uint32_t crc = simd_crc32_fn()(0, src, src_len);
    *p++ = static_cast<uint8_t>(crc);
    *p++ = static_cast<uint8_t>(crc >>  8);
    *p++ = static_cast<uint8_t>(crc >> 16);
    *p++ = static_cast<uint8_t>(crc >> 24);

    /* ISIZE (uncompressed size mod 2^32, little-endian) */
    const uint32_t isize = static_cast<uint32_t>(src_len);
    *p++ = static_cast<uint8_t>(isize);
    *p++ = static_cast<uint8_t>(isize >>  8);
    *p++ = static_cast<uint8_t>(isize >> 16);
    *p++ = static_cast<uint8_t>(isize >> 24);

    return static_cast<size_t>(p - dst);
}

deflate_result gzip_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity,
    size_t*        actual_out_size)
{
    if (src_len < 18) return DEFLATE_DATA_ERROR;

    /* Validate magic */
    if (src[0] != GZIP_MAGIC[0] || src[1] != GZIP_MAGIC[1]) return DEFLATE_DATA_ERROR;
    if (src[2] != GZIP_METHOD) return DEFLATE_DATA_ERROR;

    const uint8_t flags = src[3];
    size_t offset = 10;  /* past fixed header */

    /* Skip FEXTRA */
    if (flags & 0x04) {
        if (offset + 2 > src_len) return DEFLATE_DATA_ERROR;
        const uint16_t xlen = static_cast<uint16_t>(src[offset]) | (static_cast<uint16_t>(src[offset+1]) << 8);
        offset += 2 + xlen;
    }
    /* Skip FNAME */
    if (flags & 0x08) {
        while (offset < src_len && src[offset] != 0) ++offset;
        ++offset;  /* skip null terminator */
    }
    /* Skip FCOMMENT */
    if (flags & 0x10) {
        while (offset < src_len && src[offset] != 0) ++offset;
        ++offset;
    }
    /* Skip FHCRC */
    if (flags & 0x02) offset += 2;

    if (offset + 8 > src_len) return DEFLATE_DATA_ERROR;

    /* Decompress */
    const deflate_result r = raw_decompress(
        src + offset, src_len - offset - 8,
        dst, dst_capacity,
        actual_out_size);
    if (r != DEFLATE_OK) return r;

    /* Verify CRC-32 */
    const uint8_t* trailer = src + src_len - 8;
    const uint32_t exp_crc =
          static_cast<uint32_t>(trailer[0])
        | (static_cast<uint32_t>(trailer[1]) <<  8)
        | (static_cast<uint32_t>(trailer[2]) << 16)
        | (static_cast<uint32_t>(trailer[3]) << 24);
    const uint32_t act_crc = simd_crc32_fn()(0, dst, *actual_out_size);
    if (act_crc != exp_crc) return DEFLATE_DATA_ERROR;

    /* Verify ISIZE */
    const uint32_t exp_isize =
          static_cast<uint32_t>(trailer[4])
        | (static_cast<uint32_t>(trailer[5]) <<  8)
        | (static_cast<uint32_t>(trailer[6]) << 16)
        | (static_cast<uint32_t>(trailer[7]) << 24);
    if (exp_isize != static_cast<uint32_t>(*actual_out_size & 0xFFFFFFFF))
        return DEFLATE_DATA_ERROR;

    return DEFLATE_OK;
}

} } /* namespace orot::deflate */
