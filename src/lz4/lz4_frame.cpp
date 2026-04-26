#include "lz4_frame.hpp"
#include "lz4_block.hpp"
#include "xxhash32.hpp"

#include <cstring>
#include <memory>
#include <new>

namespace orot { namespace lz4 {

static constexpr int LZ4F_BLOCK_MAX = 4 * 1024 * 1024; /* 4 MB block max */

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void write_le32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >>  8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

static uint32_t read_le32(const uint8_t* p) noexcept {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    /* Correct for big-endian hosts: LZ4 frame is always little-endian */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

/* XXH32 of the header byte sequence (FLG + BD + optional content size) */
static uint8_t header_checksum(const uint8_t* header, int len) noexcept {
    return static_cast<uint8_t>((xxh32(header, static_cast<size_t>(len)) >> 8) & 0xFF);
}

/* ── Compress bound ──────────────────────────────────────────────────────── */

int lz4f_compress_bound(int src_len) noexcept {
    if (src_len <= 0) src_len = 0;
    /* header(7) + blocks(4+data each) + endmark(4) + checksum(4) */
    int nblocks = (src_len + LZ4F_BLOCK_MAX - 1) / LZ4F_BLOCK_MAX + 1;
    return 7 + nblocks * (4 + lz4_block_compress_bound(LZ4F_BLOCK_MAX)) + 8;
}

/* ── Compress ────────────────────────────────────────────────────────────── */

int lz4f_compress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap,
    int level) noexcept
{
    if (dst_cap < 7 + 8) return -1;

    uint8_t* out     = dst;
    uint8_t* out_end = dst + dst_cap;

    /* Magic */
    write_le32(out, LZ4F_MAGIC);
    out += 4;

    /* FLG: version=01, B.Indep=1, B.Checksum=0, C.Size=0, C.Checksum=1 */
    uint8_t flg = LZ4F_VERSION | 0x20 | LZ4F_FLAG_CONTENT_CS; /* B.Indep=1 */
    *out++ = flg;

    /* BD: block max size = 4MB (bits [6:4]=111) */
    uint8_t bd = LZ4F_BD_BLOCK_4MB;
    *out++ = bd;

    /* Header checksum (XXH32(FLG+BD) >> 8, low byte) */
    uint8_t hdr_buf[2] = { flg, bd };
    *out++ = header_checksum(hdr_buf, 2);

    /* Allocate block compress buffer + LZ4State on heap.
     * Use actual data size (capped at block max) to avoid large mmap allocations
     * for small inputs. */
    int actual_block_max = (src_len < LZ4F_BLOCK_MAX) ? src_len : LZ4F_BLOCK_MAX;
    int block_bound = lz4_block_compress_bound(actual_block_max);
    std::unique_ptr<uint8_t[]> block_buf(new (std::nothrow) uint8_t[block_bound]);
    if (!block_buf) return -1;

    std::unique_ptr<LZ4State> state(new (std::nothrow) LZ4State);
    if (!state) return -1;

    LZ4Config cfg = lz4_config_for_level(level);

    /* Content checksum accumulator */
    XXH32State content_xxh;
    content_xxh.reset(0);

    /* Blocks */
    const uint8_t* ip = src;
    int remaining = src_len;

    while (remaining > 0) {
        int chunk = (remaining > LZ4F_BLOCK_MAX) ? LZ4F_BLOCK_MAX : remaining;

        content_xxh.update(ip, static_cast<size_t>(chunk));

        int compressed = lz4_block_compress(
            ip, chunk,
            block_buf.get(), block_bound,
            *state, cfg);

        if (compressed < 0) return -1;

        /* If compressed is not smaller, store uncompressed */
        bool use_uncomp = (compressed >= chunk);
        uint32_t block_size_field;
        const uint8_t* block_data;
        int block_data_len;

        if (use_uncomp) {
            block_size_field = static_cast<uint32_t>(chunk) | LZ4F_BLOCK_UNCOMP;
            block_data       = ip;
            block_data_len   = chunk;
        } else {
            block_size_field = static_cast<uint32_t>(compressed);
            block_data       = block_buf.get();
            block_data_len   = compressed;
        }

        if (out + 4 + block_data_len > out_end) return -1;
        write_le32(out, block_size_field);
        out += 4;
        __builtin_memcpy(out, block_data, static_cast<size_t>(block_data_len));
        out += block_data_len;

        ip        += chunk;
        remaining -= chunk;
    }

    /* End mark */
    if (out + 4 > out_end) return -1;
    write_le32(out, LZ4F_ENDMARK);
    out += 4;

    /* Content checksum */
    if (out + 4 > out_end) return -1;
    write_le32(out, content_xxh.digest());
    out += 4;

    return static_cast<int>(out - dst);
}

/* ── Decompress ──────────────────────────────────────────────────────────── */

int lz4f_decompress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap) noexcept
{
    const uint8_t* ip     = src;
    const uint8_t* ip_end = src + src_len;

    /* Magic */
    if (ip + 4 > ip_end) return -1;
    if (read_le32(ip) != LZ4F_MAGIC) return -1;
    ip += 4;

    /* FLG + BD + optional fields + HC */
    if (ip + 2 > ip_end) return -1;
    const uint8_t* hdr_start = ip;
    uint8_t flg = *ip++;
    uint8_t bd  = *ip++;
    (void)bd;

    /* Validate version bits [7:6] == 01 */
    if ((flg & 0xC0) != LZ4F_VERSION) return -1;

    /* Optional content size (bit 3 of FLG) */
    bool has_content_size = (flg & 0x08) != 0;
    if (has_content_size) {
        if (ip + 8 > ip_end) return -1;
        ip += 8; /* skip content size */
    }

    bool has_dict_id = (flg & 0x01) != 0;
    if (has_dict_id) {
        if (ip + 4 > ip_end) return -1;
        ip += 4; /* skip dictionary id */
    }

    if (ip + 1 > ip_end) return -1;
    uint8_t hc = *ip++;

    /* Validate header checksum */
    {
        int hdr_len = static_cast<int>((ip - 1) - hdr_start); /* exclude HC byte */
        if (header_checksum(hdr_start, hdr_len) != hc) return -1;
    }

    bool has_content_cs = (flg & LZ4F_FLAG_CONTENT_CS) != 0;
    bool block_independent = (flg & 0x20) != 0;

    XXH32State content_xxh;
    content_xxh.reset(0);

    uint8_t* op     = dst;
    uint8_t* op_end = dst + dst_cap;

    /* Blocks */
    for (;;) {
        if (ip + 4 > ip_end) return -1;
        uint32_t block_sz_field = read_le32(ip);
        ip += 4;

        if (block_sz_field == LZ4F_ENDMARK) break;

        bool uncompressed = (block_sz_field & LZ4F_BLOCK_UNCOMP) != 0;
        int  block_data_len = static_cast<int>(block_sz_field & ~LZ4F_BLOCK_UNCOMP);

        if (block_data_len > LZ4F_BLOCK_MAX) return -1;
        if (ip + block_data_len > ip_end)    return -1;

        if (uncompressed) {
            if (op + block_data_len > op_end) return -2;
            __builtin_memcpy(op, ip, static_cast<size_t>(block_data_len));
            content_xxh.update(op, static_cast<size_t>(block_data_len));
            op += block_data_len;
        } else {
            int avail = static_cast<int>(op_end - op);
            uint8_t* prefix_base = block_independent ? op : dst;
            int decompressed = lz4_block_decompress_with_prefix(
                ip, block_data_len,
                prefix_base, op, avail);
            if (decompressed < 0) {
                return (decompressed == -2) ? -2 : -1;
            }
            content_xxh.update(op, static_cast<size_t>(decompressed));
            op += decompressed;
        }
        ip += block_data_len;
    }

    /* Content checksum */
    if (has_content_cs) {
        if (ip + 4 > ip_end) return -1;
        uint32_t stored_cs   = read_le32(ip);
        uint32_t computed_cs = content_xxh.digest();
        if (stored_cs != computed_cs) return -3;
    }

    return static_cast<int>(op - dst);
}

} } /* namespace orot::lz4 */
