#include "bzip2_decompress.hpp"
#include "bzip2_crc.hpp"
#include "bwt.hpp"
#include "bzip2_huffman.hpp"

#include <vector>
#include <cstring>

namespace orot::bzip2 {

/* ── Bit reader ──────────────────────────────────────────────────────────── */

struct BitReader {
    const uint8_t* src;
    size_t src_size;
    size_t pos;
    uint32_t buf;
    int buf_bits;

    uint32_t read_bits(int n) {
        while (buf_bits < n) {
            if (pos >= src_size) return (uint32_t)-1;
            buf = (buf << 8) | src[pos++];
            buf_bits += 8;
        }
        buf_bits -= n;
        return (buf >> buf_bits) & ((1u << n) - 1);
    }
};

/* ── RLE1 decode ─────────────────────────────────────────────────────────── */

static bool rle1_decode(const uint8_t* in, uint32_t in_len,
                         std::vector<uint8_t>& out)
{
    out.clear();
    int    run_cnt = 0;
    uint8_t run_ch = 0;

    for (uint32_t i = 0; i < in_len; ) {
        uint8_t c = in[i++];
        out.push_back(c);

        if (c == run_ch) {
            ++run_cnt;
        } else {
            run_ch  = c;
            run_cnt = 1;
        }

        if (run_cnt == 4) {
            if (i >= in_len) return false;
            uint8_t cnt = in[i++];
            for (int k = 0; k < cnt; ++k) out.push_back(c);
            run_cnt = 0; /* reset: count byte consumed, run is over */
        }
    }
    return true;
}

/* ── RLE2 / Huffman decode ───────────────────────────────────────────────── */

static bool decode_block(BitReader& br,
                          std::vector<uint8_t>& out,
                          uint32_t* block_crc_out,
                          std::vector<uint32_t>& ibwt_buf)
{
    /* Block CRC (32 bits) */
    uint32_t hi = br.read_bits(16);
    uint32_t lo = br.read_bits(16);
    if (hi == (uint32_t)-1 || lo == (uint32_t)-1) return false;
    *block_crc_out = (hi << 16) | lo;

    /* Randomized flag */
    uint32_t rnd = br.read_bits(1);
    if (rnd == (uint32_t)-1) return false;
    if (rnd) return false; /* randomized blocks not supported */

    /* BWT primary index (24 bits) */
    uint32_t primary = br.read_bits(24);
    if (primary == (uint32_t)-1) return false;

    /* In-use map */
    uint8_t in_use[256] = {};
    uint32_t coarse = br.read_bits(16);
    if (coarse == (uint32_t)-1) return false;
    for (int i = 0; i < 16; ++i) {
        if (!(coarse & (1u << (15 - i)))) continue;
        uint32_t fine = br.read_bits(16);
        if (fine == (uint32_t)-1) return false;
        for (int j = 0; j < 16; ++j)
            if (fine & (1u << (15 - j))) in_use[i * 16 + j] = 1;
    }

    /* Build symbol-to-rank mapping */
    int sym_map[256];
    int n_in_use = 0;
    for (int i = 0; i < 256; ++i)
        if (in_use[i]) sym_map[n_in_use++] = i;
    int alpha_size = n_in_use + 2;

    /* Number of tables and selectors */
    uint32_t n_tables = br.read_bits(3);
    uint32_t n_selectors = br.read_bits(15);
    if (n_tables < 2 || n_tables > 6) return false;
    if (n_selectors == (uint32_t)-1) return false;

    /* Read selectors (MTF-coded) */
    std::vector<uint8_t> selectors(n_selectors);
    {
        uint8_t order[BZ_MAX_TABLES];
        for (uint32_t t = 0; t < n_tables; ++t) order[t] = (uint8_t)t;
        for (uint32_t g = 0; g < n_selectors; ++g) {
            int pos = 0;
            while (true) {
                uint32_t b = br.read_bits(1);
                if (b == (uint32_t)-1) return false;
                if (b == 0) break;
                ++pos;
                if (pos >= (int)n_tables) return false;
            }
            uint8_t tbl = order[pos];
            memmove(order + 1, order, pos);
            order[0] = tbl;
            selectors[g] = tbl;
        }
    }

    /* Read Huffman code lengths for each table */
    HuffDecTable dec_tables[BZ_MAX_TABLES];
    for (uint32_t t = 0; t < n_tables; ++t) {
        uint8_t lens[BZ_MAX_ALPHA_SIZE] = {};
        uint32_t cur = br.read_bits(5);
        if (cur == (uint32_t)-1) return false;
        for (int s = 0; s < alpha_size; ++s) {
            while (true) {
                uint32_t b = br.read_bits(1);
                if (b == (uint32_t)-1) return false;
                if (b == 0) break;
                uint32_t dir = br.read_bits(1);
                if (dir == (uint32_t)-1) return false;
                if (dir == 0) { if (cur < 20) ++cur; }
                else          { if (cur > 1)  --cur; }
            }
            lens[s] = (uint8_t)cur;
        }
        dec_tables[t].build_from_lengths(lens, alpha_size);
    }

    /* Decode symbol stream */
    std::vector<uint8_t> mtf_out;
    mtf_out.reserve(900000);
    int EOB = alpha_size - 1;
    /* Use br.buf/br.buf_bits so bits left after header parsing are not lost */
    uint32_t g = 0, g_cnt = 0;
    uint8_t cur_tbl = selectors[0];
    uint32_t run = 0;
    uint32_t run_mul = 1;

    auto flush_run = [&]() {
        for (uint32_t k = 0; k < run; ++k) mtf_out.push_back(0);
        run = 0; run_mul = 1;
    };

    while (true) {
        if (g_cnt == BZ_G_SIZE) {
            g_cnt = 0;
            ++g;
            if (g >= n_selectors) return false;
            cur_tbl = selectors[g];
        }

        int sym = dec_tables[cur_tbl].decode_sym(
            br.buf, br.buf_bits, br.src, br.src_size, br.pos);
        ++g_cnt;

        if (sym < 0) return false;
        if (sym == EOB) { flush_run(); break; }
        if (sym == BZ_RUNA) { run += run_mul;     run_mul <<= 1; continue; }
        if (sym == BZ_RUNB) { run += 2 * run_mul; run_mul <<= 1; continue; }
        flush_run();
        run_mul = 1;
        mtf_out.push_back((uint8_t)(sym - 1));
    }

    /* MTF decode */
    uint32_t mtf_len = (uint32_t)mtf_out.size();
    {
        uint8_t mtf_sym[256];
        for (int i = 0; i < n_in_use; ++i) mtf_sym[i] = (uint8_t)sym_map[i];
        for (uint32_t i = 0; i < mtf_len; ++i) {
            uint8_t rank = mtf_out[i];
            if (rank >= n_in_use) return false;
            uint8_t c = mtf_sym[rank];
            memmove(mtf_sym + 1, mtf_sym, rank);
            mtf_sym[0] = c;
            mtf_out[i] = c;
        }
    }

    /* BWT inverse */
    std::vector<uint8_t> bwt_inv(mtf_len);
    bwt_inverse(mtf_out.data(), bwt_inv.data(), mtf_len, primary, ibwt_buf);

    /* RLE1 decode */
    std::vector<uint8_t> rle1_dec;
    if (!rle1_decode(bwt_inv.data(), mtf_len, rle1_dec)) return false;

    /* Verify CRC */
    uint32_t computed = crc32_block(rle1_dec.data(), rle1_dec.size());
    if (computed != *block_crc_out) return false;

    for (uint8_t b : rle1_dec) out.push_back(b);
    return true;
}

/* ── Top-level decompress ───────────────────────────────────────────────── */

size_t bzip2_decompress(const uint8_t* src, size_t src_size,
                         uint8_t* dst, size_t dst_cap)
{
    if (src_size < 10) return 0;

    /* Stream header */
    if (src[0] != 'B' || src[1] != 'Z' || src[2] != 'h') return 0;
    int level = src[3] - '0';
    if (level < 1 || level > 9) return 0;

    BitReader br{src + 4, src_size - 4, 0, 0, 0};

    std::vector<uint8_t> output;
    output.reserve(level * 100000);
    uint32_t combined_crc = 0;
    std::vector<uint32_t> ibwt_buf;

    while (true) {
        uint32_t m0 = br.read_bits(16);
        uint32_t m1 = br.read_bits(16);
        uint32_t m2 = br.read_bits(16);
        if (m0 == (uint32_t)-1) return 0;

        if (m0 == 0x1772 && m1 == 0x4538 && m2 == 0x5090) {
            /* Stream end */
            uint32_t shi = br.read_bits(16);
            uint32_t slo = br.read_bits(16);
            if (shi == (uint32_t)-1 || slo == (uint32_t)-1) return 0;
            uint32_t stored_crc = (shi << 16) | slo;
            if (stored_crc != combined_crc) return 0;
            break;
        }
        if (m0 != 0x3141 || m1 != 0x5926 || m2 != 0x5359) return 0;

        uint32_t block_crc = 0;
        std::vector<uint8_t> block_out;
        if (!decode_block(br, block_out, &block_crc, ibwt_buf)) return 0;
        combined_crc = crc32_combine(combined_crc, block_crc);

        for (uint8_t b : block_out) output.push_back(b);
    }

    if (output.size() > dst_cap) return 0;
    memcpy(dst, output.data(), output.size());
    return output.size();
}

} // namespace orot::bzip2
