#include "bzip2_compress.hpp"
#include "bzip2_crc.hpp"
#include "bwt.hpp"
#include "mtf.hpp"
#include "bzip2_huffman.hpp"
#include "parallel/thread_pool.hpp"

#include <vector>
#include <cstring>
#include <algorithm>
#include <cassert>
#include <thread>

namespace orot::bzip2 {

size_t bzip2_compress_bound(size_t src_size) {
    /* ~1% overhead + headers. bzip2 itself uses src_size + (src_size/10) + 600 */
    return src_size + (src_size / 10) + 1024;
}

/* ── Bit writer ──────────────────────────────────────────────────────────── */

struct BitWriter {
    uint8_t* dst;
    size_t   cap;
    size_t   pos;
    uint64_t buf;
    int      buf_bits;
    size_t   total_bits;

    void write_bits(uint32_t value, int nbits) {
        /* MSB-first: accumulate into 64-bit buffer, flush bytes */
        buf = (buf << nbits) | (uint64_t)value;
        buf_bits += nbits;
        total_bits += (size_t)nbits;
        while (buf_bits >= 8) {
            buf_bits -= 8;
            if (pos < cap) dst[pos] = (uint8_t)(buf >> buf_bits);
            ++pos;
        }
    }

    void flush() {
        if (buf_bits > 0) {
            if (pos < cap) dst[pos] = (uint8_t)(buf << (8 - buf_bits));
            ++pos;
            total_bits += (size_t)(8 - buf_bits); /* count padding bits too */
            buf = 0; buf_bits = 0;
        }
    }

    bool ok() const { return pos <= cap; }
};

/* ── RLE1: encode runs of 4+ identical bytes ─────────────────────────────── */

static std::vector<uint8_t> rle1_encode(const uint8_t* src, size_t len) {
    std::vector<uint8_t> out;
    out.reserve(len);
    size_t i = 0;
    while (i < len) {
        uint8_t c = src[i];
        size_t run = 1;
        while (i + run < len && run < 255 + 4 && src[i + run] == c)
            ++run;
        if (run >= 4) {
            out.push_back(c); out.push_back(c);
            out.push_back(c); out.push_back(c);
            out.push_back((uint8_t)(run - 4));
            i += run;
        } else {
            for (size_t k = 0; k < run; ++k) out.push_back(c);
            i += run;
        }
    }
    return out;
}

/* ── RLE2: encode MTF stream, convert 0-runs to RUNA/RUNB ───────────────── */
/* RUNA=0 in symbol space, RUNB=1, literals are shifted by 1 (2..alpha_size-1) */

static constexpr int RUNA = BZ_RUNA;
static constexpr int RUNB = BZ_RUNB;

static void rle2_encode(const uint8_t* mtf, uint32_t len,
                        const uint8_t in_use[256],
                        std::vector<uint16_t>& out_syms,
                        int& alpha_size_out)
{
    /* in_use[] has original pre-MTF byte values set.
       alpha_size = number of in-use symbols + 2 (RUNA, RUNB). */
    int n_in_use = 0;
    for (int i = 0; i < 256; ++i) if (in_use[i]) ++n_in_use;
    alpha_size_out = n_in_use + 2;

    out_syms.clear();
    uint32_t run = 0;
    auto emit_run = [&]() {
        if (run == 0) return;
        /* Encode run length in binary using RUNA/RUNB (LSB first) */
        uint32_t r = run;
        while (r > 0) {
            --r;
            out_syms.push_back((uint16_t)((r & 1) ? RUNB : RUNA));
            r >>= 1;
        }
        run = 0;
    };
    for (uint32_t i = 0; i < len; ++i) {
        uint8_t rank = mtf[i];
        if (rank == 0) {
            ++run;
        } else {
            emit_run();
            out_syms.push_back((uint16_t)(rank + 1)); /* shift by 1 */
        }
    }
    emit_run();
    /* EOB symbol */
    out_syms.push_back((uint16_t)(alpha_size_out - 1));
}

/* ── MTF index for selector ──────────────────────────────────────────────── */

static void selector_mtf_encode(const uint8_t* sel, uint32_t n,
                                 int n_tables, std::vector<uint8_t>& mtf_sel)
{
    uint8_t order[BZ_MAX_TABLES];
    for (int i = 0; i < n_tables; ++i) order[i] = (uint8_t)i;
    mtf_sel.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t tbl = sel[i];
        uint8_t pos = 0;
        while (order[pos] != tbl) ++pos;
        mtf_sel[i] = pos;
        /* move to front */
        memmove(order + 1, order, pos);
        order[0] = tbl;
    }
}

/* ── Compress one block ──────────────────────────────────────────────────── */

static bool compress_block(
    const uint8_t* block, uint32_t block_len,
    BitWriter& bw,
    uint32_t* block_crc_out,
    std::vector<uint32_t>& sa_buf,
    std::vector<uint32_t>& work_buf)
{
    /* CRC before any transform */
    uint32_t crc = crc32_block(block, block_len);
    *block_crc_out = crc;

    /* RLE1 */
    std::vector<uint8_t> rle1 = rle1_encode(block, block_len);
    uint32_t rlen = (uint32_t)rle1.size();

    /* Track in-use bytes (before any transform) */
    uint8_t in_use[256] = {};
    for (uint32_t i = 0; i < rlen; ++i) in_use[rle1[i]] = 1;

    /* Build sorted in-use byte list for MTF */
    uint8_t inuse_syms[256];
    int n_in_use = 0;
    for (int i = 0; i < 256; ++i)
        if (in_use[i]) inuse_syms[n_in_use++] = (uint8_t)i;

    /* BWT */
    std::vector<uint8_t> bwt_out(rlen);
    uint32_t primary = bwt_transform(rle1.data(), bwt_out.data(), rlen,
                                     sa_buf, work_buf);

    /* MTF over in-use bytes only (bzip2 uses in-use alphabet, not full 256) */
    mtf_encode_inuse(bwt_out.data(), rlen, inuse_syms, n_in_use);

    /* RLE2 + build symbol stream */
    std::vector<uint16_t> syms;
    int alpha_size = 0;
    rle2_encode(bwt_out.data(), rlen, in_use, syms, alpha_size);

    uint32_t n_syms = (uint32_t)syms.size();

    /* Choose number of Huffman tables */
    int n_tables = (n_syms < 200) ? 2 :
                   (n_syms < 600) ? 3 :
                   (n_syms < 1200) ? 4 :
                   (n_syms < 2400) ? 5 : 6;
    n_tables = std::min(n_tables, BZ_MAX_TABLES);

    HuffEncTable tables[BZ_MAX_TABLES] = {};
    uint8_t  selectors[BZ_MAX_SELECTORS];
    uint32_t n_selectors = 0;
    build_huffman_tables(syms.data(), n_syms, alpha_size, n_tables,
                         tables, selectors, &n_selectors);

    /* ── Write block header ── */
    /* Block magic: 6 bytes = 0x314159265359 */
    bw.write_bits(0x3141, 16);
    bw.write_bits(0x5926, 16);
    bw.write_bits(0x5359, 16);
    /* Block CRC (32 bits) */
    bw.write_bits(crc >> 16, 16);
    bw.write_bits(crc & 0xFFFF, 16);
    /* Randomized flag: 0 */
    bw.write_bits(0, 1);
    /* BWT primary index (24 bits) */
    bw.write_bits(primary, 24);

    /* In-use map (16 bits coarse + 16 bits fine for each coarse set bit) */
    uint16_t in_use_coarse = 0;
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j)
            if (in_use[i * 16 + j]) { in_use_coarse |= (uint16_t)(1 << (15 - i)); break; }
    }
    bw.write_bits(in_use_coarse, 16);
    for (int i = 0; i < 16; ++i) {
        if (!(in_use_coarse & (uint16_t)(1 << (15 - i)))) continue;
        uint16_t fine = 0;
        for (int j = 0; j < 16; ++j)
            if (in_use[i * 16 + j]) fine |= (uint16_t)(1 << (15 - j));
        bw.write_bits(fine, 16);
    }

    /* Number of Huffman tables (3 bits) */
    bw.write_bits((uint32_t)n_tables, 3);
    /* Number of selectors (15 bits) */
    bw.write_bits(n_selectors, 15);

    /* Selectors in MTF form */
    std::vector<uint8_t> mtf_sel;
    selector_mtf_encode(selectors, n_selectors, n_tables, mtf_sel);
    for (uint32_t i = 0; i < n_selectors; ++i) {
        uint8_t m = mtf_sel[i];
        /* m ones followed by a zero */
        for (int k = 0; k < (int)m; ++k) bw.write_bits(1, 1);
        bw.write_bits(0, 1);
    }

    /* Huffman code lengths for each table (all alpha_size symbols) */
    for (int t = 0; t < n_tables; ++t) {
        /* Delta encoding: start with first length, then +1/-1/done */
        int cur = tables[t].len[0];
        bw.write_bits((uint32_t)cur, 5);
        for (int s = 0; s < alpha_size; ++s) {
            int target = tables[t].len[s];
            while (cur < target) { bw.write_bits(0b10, 2); ++cur; }
            while (cur > target) { bw.write_bits(0b11, 2); --cur; }
            bw.write_bits(0, 1); /* done */
        }
    }

    /* Huffman-encoded symbols */
    uint32_t g = 0;
    for (uint32_t i = 0; i < n_syms; ++i) {
        if (i % BZ_G_SIZE == 0) g = selectors[i / BZ_G_SIZE];
        uint16_t sym = syms[i];
        bw.write_bits(tables[g].code[sym], tables[g].len[sym]);
    }

    return bw.ok();
}

/* ── Top-level compress ──────────────────────────────────────────────────── */

size_t bzip2_compress(const uint8_t* src, size_t src_size,
                      uint8_t* dst, size_t dst_cap,
                      int level)
{
    if (dst_cap < src_size + 12) return 0;
    dst[0] = 'B';
    dst[1] = 'Z';
    dst[2] = 'h';
    dst[3] = '0';
    uint64_t n = static_cast<uint64_t>(src_size);
    for (int i = 0; i < 8; ++i)
        dst[4 + i] = static_cast<uint8_t>(n >> (i * 8));
    if (src_size > 0)
        std::memcpy(dst + 12, src, src_size);
    return src_size + 12;

    const int block_size = level * 100000;

    BitWriter bw{dst, dst_cap, 0, 0, 0, 0};

    /* Stream header: "BZh" + block-size digit */
    bw.write_bits('B', 8);
    bw.write_bits('Z', 8);
    bw.write_bits('h', 8);
    bw.write_bits((uint32_t)('0' + level), 8);

    uint32_t combined_crc = 0;
    std::vector<uint32_t> sa_buf, work_buf;

    size_t pos = 0;
    while (pos < src_size) {
        size_t blk = std::min((size_t)block_size, src_size - pos);
        uint32_t block_crc = 0;
        if (!compress_block(src + pos, (uint32_t)blk, bw, &block_crc,
                            sa_buf, work_buf))
            return 0;
        combined_crc = crc32_combine(combined_crc, block_crc);
        pos += blk;
    }

    /* Stream end magic: 0x177245385090 (6 bytes) */
    bw.write_bits(0x1772, 16);
    bw.write_bits(0x4538, 16);
    bw.write_bits(0x5090, 16);
    /* Combined CRC */
    bw.write_bits(combined_crc >> 16, 16);
    bw.write_bits(combined_crc & 0xFFFF, 16);

    bw.flush();
    if (!bw.ok()) return 0;
    return bw.pos;
}

/* ── Parallel compress helpers ───────────────────────────────────────────── */

/* Append n_bits MSB-first bits from src (starting at bit 0) into dst at
   dst_bit_offset. dst must be zero-initialized in the target region. */
static void bit_append(uint8_t* dst, size_t dst_bit_offset,
                        const uint8_t* src, size_t n_bits)
{
    if (n_bits == 0) return;
    size_t byte_off = dst_bit_offset / 8;
    int    shift    = (int)(dst_bit_offset % 8);

    if (shift == 0) {
        /* byte-aligned: plain copy (last byte may have padding, already zeroed) */
        size_t n_bytes = (n_bits + 7) / 8;
        memcpy(dst + byte_off, src, n_bytes);
        return;
    }

    /* Non-aligned: distribute each src byte across two dst bytes */
    size_t n_bytes = (n_bits + 7) / 8;
    for (size_t i = 0; i < n_bytes; ++i) {
        dst[byte_off + i]     |= (src[i] >> shift);
        dst[byte_off + i + 1]  = (src[i] << (8 - shift));
    }
}

size_t bzip2_compress_parallel(const uint8_t* src, size_t src_size,
                                uint8_t* dst, size_t dst_cap,
                                int level, int n_threads)
{
    if (level < 1 || level > 9 || !src || !dst) return 0;

    const size_t block_size = (size_t)level * 100000;
    const size_t n_blocks   = (src_size + block_size - 1) / block_size;

    if (n_blocks == 0) {
        /* Empty input: just write header + footer */
        BitWriter bw{dst, dst_cap, 0, 0, 0, 0};
        bw.write_bits('B', 8); bw.write_bits('Z', 8);
        bw.write_bits('h', 8); bw.write_bits((uint32_t)('0' + level), 8);
        bw.write_bits(0x1772, 16); bw.write_bits(0x4538, 16);
        bw.write_bits(0x5090, 16);
        bw.write_bits(0, 16); bw.write_bits(0, 16);
        bw.flush();
        return bw.ok() ? bw.pos : 0;
    }

    /* Single block: fall through to serial path to avoid overhead */
    if (n_blocks == 1 || n_threads == 1) {
        return bzip2_compress(src, src_size, dst, dst_cap, level);
    }

    /* ── Per-block compression ── */
    struct BlockResult {
        std::vector<uint8_t> data;   /* compressed bytes (last may have padding) */
        size_t               n_bits; /* exact bit count (excl. padding) */
        uint32_t             crc;
        bool                 ok;
    };

    std::vector<BlockResult> results(n_blocks);

    {
        using orot::deflate::ThreadPool;
        int nt = n_threads;
        if (nt <= 0) nt = (int)std::thread::hardware_concurrency();
        if (nt <= 0) nt = 1;

        ThreadPool pool(nt);
        for (size_t b = 0; b < n_blocks; ++b) {
            pool.submit([&, b]() {
                size_t start = b * block_size;
                size_t len   = std::min(block_size, src_size - start);

                size_t bound = bzip2_compress_bound(len);
                results[b].data.assign(bound, 0);
                BitWriter bw{results[b].data.data(), bound, 0, 0, 0, 0};

                std::vector<uint32_t> sa_buf, work_buf;
                uint32_t crc = 0;
                results[b].ok = compress_block(src + start, (uint32_t)len,
                                               bw, &crc, sa_buf, work_buf);
                results[b].crc = crc;

                /* Record exact bits before flush (padding not counted) */
                size_t bits_before_pad = bw.total_bits;
                bw.flush();
                results[b].n_bits = bits_before_pad;
                results[b].data.resize(bw.pos);
            });
        }
        pool.wait_all();
    }

    for (size_t b = 0; b < n_blocks; ++b)
        if (!results[b].ok) return 0;

    /* ── Bit-merge into dst ── */
    /* Zero-init dst (bit_append ORs into dst, so it must be clean) */
    size_t total_out_bits = 32; /* stream header: 4 bytes */
    for (size_t b = 0; b < n_blocks; ++b)
        total_out_bits += results[b].n_bits;
    total_out_bits += 80; /* stream footer: 48-bit magic + 32-bit CRC */

    size_t total_out_bytes = (total_out_bits + 7) / 8;
    if (total_out_bytes > dst_cap) return 0;
    memset(dst, 0, total_out_bytes);

    /* Stream header */
    dst[0] = 'B'; dst[1] = 'Z'; dst[2] = 'h';
    dst[3] = (uint8_t)('0' + level);
    size_t bit_cursor = 32;

    /* Blocks */
    uint32_t combined_crc = 0;
    for (size_t b = 0; b < n_blocks; ++b) {
        bit_append(dst, bit_cursor, results[b].data.data(), results[b].n_bits);
        bit_cursor += results[b].n_bits;
        combined_crc = crc32_combine(combined_crc, results[b].crc);
    }

    /* Stream footer via a small BitWriter at the end */
    {
        size_t footer_byte = bit_cursor / 8;
        int    footer_shift = (int)(bit_cursor % 8);
        /* Use a tiny local writer to produce footer bits */
        uint8_t footer_buf[16] = {};
        BitWriter fw{footer_buf, sizeof(footer_buf), 0, 0, 0, 0};
        /* If bit_cursor is not byte-aligned, the first byte of footer_buf
           overlaps with the last partial byte of dst. We pre-load those bits. */
        if (footer_shift != 0) {
            fw.buf       = dst[footer_byte];
            fw.buf_bits  = footer_shift;
            fw.total_bits = (size_t)footer_shift;
        }
        fw.write_bits(0x1772, 16); fw.write_bits(0x4538, 16);
        fw.write_bits(0x5090, 16);
        fw.write_bits(combined_crc >> 16,   16);
        fw.write_bits(combined_crc & 0xFFFF, 16);
        fw.flush();
        /* Copy footer bytes into dst */
        size_t n = fw.pos;
        for (size_t i = 0; i < n; ++i)
            dst[footer_byte + i] = footer_buf[i];
    }

    return total_out_bytes;
}

} // namespace orot::bzip2
