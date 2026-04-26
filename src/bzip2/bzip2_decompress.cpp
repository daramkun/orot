#include "bzip2_decompress.hpp"
#include "bzip2_crc.hpp"
#include "bzip2_huffman.hpp"

#include <algorithm>
#include <vector>
#include <cstring>

namespace orot::bzip2 {

/* ── Bit reader ──────────────────────────────────────────────────────────── */

struct BitReader {
    const uint8_t* src;
    size_t src_size;
    size_t pos;
    uint64_t buf;
    int buf_bits;

    /* Fill buf to at least 56 bits from src */
    void refill() {
        if (buf_bits > 56) return;
        /* Bulk path: load up to 8 source bytes at once (MSB-first via bswap) */
        if (pos + 8 <= src_size) {
            int slots = (64 - buf_bits) >> 3;  /* 1..8 bytes to add */
            int shift = slots * 8;              /* 8..64 */
            uint64_t chunk;
            __builtin_memcpy(&chunk, src + pos, 8);
            chunk = __builtin_bswap64(chunk);
            /* shift < 64: make room in buf; shift == 64: buf was empty → overwrite */
            buf = (shift < 64 ? buf << shift : 0) | (chunk >> (64 - shift));
            pos += slots;
            buf_bits += shift;
            return;
        }
        /* Byte-by-byte fallback near end of stream */
        while (buf_bits <= 56 && pos < src_size) {
            buf = (buf << 8) | src[pos++];
            buf_bits += 8;
        }
    }

    uint32_t read_bits(int n) {
        while (buf_bits < n) {
            if (pos >= src_size) return (uint32_t)-1;
            buf = (buf << 8) | src[pos++];
            buf_bits += 8;
        }
        buf_bits -= n;
        return (uint32_t)((buf >> buf_bits) & ((1u << n) - 1));
    }
};

/* ── Block decode ────────────────────────────────────────────────────────── */

static constexpr uint32_t BZ2_BLOCK_MAX = 900001;

static inline int decode_sym_hot(const HuffDecTable& ht, BitReader& br) {
    if (br.buf_bits >= HUFF_FAST_BITS) {
        uint32_t peek = (uint32_t)((br.buf >> (br.buf_bits - HUFF_FAST_BITS)) &
                                   (HUFF_FAST_SIZE - 1));
        const HuffDecTable::FastEntry& e = ht.fast_table[peek];
        if (e.sym >= 0) {
            br.buf_bits -= e.len;
            return e.sym;
        }

        br.buf_bits -= HUFF_FAST_BITS;
        uint32_t v = peek;
        for (int l = HUFF_FAST_BITS + 1; l <= ht.max_len; ++l) {
            if (br.buf_bits == 0) {
                if (br.pos >= br.src_size) return -1;
                br.buf = (br.buf << 8) | br.src[br.pos++];
                br.buf_bits = 8;
            }
            --br.buf_bits;
            v = (v << 1) | (uint32_t)((br.buf >> br.buf_bits) & 1);
            if (ht.limit[l] == (uint32_t)-1) continue;
            if (v <= ht.limit[l])
                return ht.perm[ht.offset[l] + (int)(v - ht.base[l])];
        }
        return -1;
    }

    uint32_t v = 0;
    for (int l = 1; l <= ht.max_len; ++l) {
        if (br.buf_bits == 0) {
            if (br.pos >= br.src_size) return -1;
            br.buf = (br.buf << 8) | br.src[br.pos++];
            br.buf_bits = 8;
        }
        --br.buf_bits;
        v = (v << 1) | (uint32_t)((br.buf >> br.buf_bits) & 1);
        if (ht.limit[l] == (uint32_t)-1) continue;
        if (v <= ht.limit[l])
            return ht.perm[ht.offset[l] + (int)(v - ht.base[l])];
    }
    return -1;
}

static inline void mtf_move_to_front(uint8_t* mtf_sym, uint8_t rank, uint8_t sym) {
    switch (rank) {
    case 0:
        return;
    case 1:
        mtf_sym[1] = mtf_sym[0];
        mtf_sym[0] = sym;
        return;
    case 2:
        mtf_sym[2] = mtf_sym[1];
        mtf_sym[1] = mtf_sym[0];
        mtf_sym[0] = sym;
        return;
    case 3:
        mtf_sym[3] = mtf_sym[2];
        mtf_sym[2] = mtf_sym[1];
        mtf_sym[1] = mtf_sym[0];
        mtf_sym[0] = sym;
        return;
    case 4:
        mtf_sym[4] = mtf_sym[3];
        mtf_sym[3] = mtf_sym[2];
        mtf_sym[2] = mtf_sym[1];
        mtf_sym[1] = mtf_sym[0];
        mtf_sym[0] = sym;
        return;
    case 5:
        mtf_sym[5] = mtf_sym[4];
        mtf_sym[4] = mtf_sym[3];
        mtf_sym[3] = mtf_sym[2];
        mtf_sym[2] = mtf_sym[1];
        mtf_sym[1] = mtf_sym[0];
        mtf_sym[0] = sym;
        return;
    case 6:
        mtf_sym[6] = mtf_sym[5];
        mtf_sym[5] = mtf_sym[4];
        mtf_sym[4] = mtf_sym[3];
        mtf_sym[3] = mtf_sym[2];
        mtf_sym[2] = mtf_sym[1];
        mtf_sym[1] = mtf_sym[0];
        mtf_sym[0] = sym;
        return;
    case 7:
        mtf_sym[7] = mtf_sym[6];
        mtf_sym[6] = mtf_sym[5];
        mtf_sym[5] = mtf_sym[4];
        mtf_sym[4] = mtf_sym[3];
        mtf_sym[3] = mtf_sym[2];
        mtf_sym[2] = mtf_sym[1];
        mtf_sym[1] = mtf_sym[0];
        mtf_sym[0] = sym;
        return;
    case 8:
        mtf_sym[8] = mtf_sym[7];
        mtf_sym[7] = mtf_sym[6];
        mtf_sym[6] = mtf_sym[5];
        mtf_sym[5] = mtf_sym[4];
        mtf_sym[4] = mtf_sym[3];
        mtf_sym[3] = mtf_sym[2];
        mtf_sym[2] = mtf_sym[1];
        mtf_sym[1] = mtf_sym[0];
        mtf_sym[0] = sym;
        return;
    default:
        memmove(mtf_sym + 1, mtf_sym, rank);
        mtf_sym[0] = sym;
        return;
    }
}

static inline void ibwt_prefetch_next(const uint32_t* tt, uint32_t next) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(tt + next, 0, 1);
#else
    (void)tt; (void)next;
#endif
}

static inline void repeat_store_hot(uint8_t* out, uint8_t byte, size_t cnt) {
    if (cnt >= 64) {
        std::memset(out, byte, cnt);
        return;
    }

#if defined(DEFLATE_HAS_NEON)
    const uint8x16_t v = vdupq_n_u8(byte);
    while (cnt >= 16) {
        vst1q_u8(out, v);
        out += 16;
        cnt -= 16;
    }
#elif defined(DEFLATE_HAS_SSE2)
    const __m128i v = _mm_set1_epi8((char)byte);
    while (cnt >= 16) {
        _mm_storeu_si128((__m128i*)out, v);
        out += 16;
        cnt -= 16;
    }
#endif

    while (cnt-- > 0) *out++ = byte;
}

/*
 * decode_block: fused Huffman+MTF decode → BWT pack → BWT walk+RLE1.
 *
 * Architecture (libbz2-inspired):
 *   Phase 1 — Huffman + MTF decode: decoded chars written to ll[],
 *              unzftab[] counts accumulated. No separate mtf_out buffer.
 *   Phase 2 — BWT pack: tt[f_pos] = source index, ll[] remains the L column.
 *   Phase 3 — Output walk: tPos walks tt[], chars come from ll[].
 */
static bool decode_block(BitReader& br,
                          uint8_t* dst, size_t dst_cap, size_t& written,
                          uint32_t* block_crc_out,
                          std::vector<uint32_t>& tt,
                          std::vector<uint8_t>& ll)
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

    /* Build sequential-index → actual-byte mapping */
    int sym_map[256];
    int n_in_use = 0;
    for (int i = 0; i < 256; ++i)
        if (in_use[i]) sym_map[n_in_use++] = i;
    if (n_in_use == 0) return false;
    int alpha_size = n_in_use + 2;

    /* Number of Huffman tables and selectors */
    uint32_t n_tables = br.read_bits(3);
    uint32_t n_selectors = br.read_bits(15);
    if (n_tables < 2 || n_tables > 6) return false;
    if (n_selectors == (uint32_t)-1) return false;

    /* Read selectors (MTF-coded, max 6 tables → unrolled shift is safe) */
    uint8_t selectors[BZ_MAX_SELECTORS];
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
            for (int k = pos; k > 0; --k) order[k] = order[k - 1];
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

    /* ── Phase 1: Huffman + MTF → ll[] ─────────────────────────────────────── */

    uint32_t* tt_data = tt.data();  /* already sized BZ2_BLOCK_MAX by caller */
    uint8_t* ll_data = ll.data();   /* already sized BZ2_BLOCK_MAX by caller */

    /* MTF list — actual byte values, index == rank */
    uint8_t mtf_sym[256];
    for (int i = 0; i < n_in_use; ++i) mtf_sym[i] = (uint8_t)sym_map[i];

    uint32_t unzftab[256] = {};
    uint32_t rle2_run_syms = 0;
    uint32_t nblock = 0;
    const int EOB = alpha_size - 1;
    uint32_t g = 0;
    uint32_t g_rem = BZ_G_SIZE;
    const HuffDecTable* cur_ht = &dec_tables[selectors[0]];
    uint32_t run = 0, run_mul = 1;

    while (true) {
        if (g_rem == 0) {
            ++g;
            if (g >= n_selectors) return false;
            cur_ht = &dec_tables[selectors[g]];
            g_rem = BZ_G_SIZE;
        }
        br.refill();
        int sym = decode_sym_hot(*cur_ht, br);
        --g_rem;
        if (sym < 0) return false;

        if ((uint32_t)sym <= BZ_RUNB) {
            run += ((uint32_t)sym + 1) * run_mul;
            run_mul <<= 1;
            ++rle2_run_syms;
            continue;
        }

        /* Flush any pending RUNA/RUNB run: copies of MTF[0] */
        if (run > 0) {
            if (nblock + run > BZ2_BLOCK_MAX) return false;
            uint8_t uc = mtf_sym[0];
            std::fill_n(ll_data + nblock, run, uc);
            unzftab[uc] += run;
            nblock += run;
            run = 0; run_mul = 1;
        }

        if (sym == EOB) break;

        /* MTF decode: sym-1 = rank */
        uint8_t rank = (uint8_t)(sym - 1);
        if ((int)rank >= n_in_use) return false;
        uint8_t uc = mtf_sym[rank];
        mtf_move_to_front(mtf_sym, rank, uc);

        if (nblock >= BZ2_BLOCK_MAX) return false;
        ll_data[nblock++] = uc;
        unzftab[uc]++;
    }

    if (primary >= nblock) return false;

    /* ── Phase 2: BWT pack — tt[f_pos] = source index, then pack char into tt[index] */

    uint32_t cftab[257] = {};
    for (int i = 1; i <= 256; i++) cftab[i] = unzftab[i-1];
    for (int i = 1; i <= 256; i++) cftab[i] += cftab[i-1];
    for (int i = 0; i <= 256; i++) if (cftab[i] > nblock) return false;

    for (uint32_t i = 0; i < nblock; i++) {
        uint8_t uc = ll_data[i];
        tt_data[cftab[uc]] = i;
        cftab[uc]++;
    }
    static constexpr uint32_t TT_INDEX_MASK = 0xFFFFFu;
    for (uint32_t i = 0; i < nblock; i++)
        tt_data[i] |= ((uint32_t)ll_data[i] << 20);

    /* ── Phase 3: BWT walk + RLE1 ───────────────────────────────────────────── */

    /* Restore all hot state to locals (encourages register allocation) */
    uint8_t* out = dst + written;
    uint8_t* const out_end = dst + dst_cap;
    uint8_t* const block_begin = out;
    uint8_t  c_run_ch = 0;
    int      c_run_cnt = 0;
    uint32_t c_tPos   = tt_data[primary] & TT_INDEX_MASK;
    uint32_t c_nb     = 0;
    uint32_t c_crc    = 0xFFFFFFFFu;
    size_t   c_run_extra = 0;
    static constexpr size_t CRC_DEFER_RUN_THRESHOLD = 512;
    const size_t dst_remaining = (size_t)(out_end - out);
    const bool defer_crc_from_start =
        dst_remaining >= 4096 &&
        ((uint64_t)nblock * 8u < dst_remaining ||
         (rle2_run_syms > 0 && rle2_run_syms * 2u >= nblock));
    if (defer_crc_from_start)
        goto phase3_deferred;

    while (c_nb < nblock) {
        uint32_t idx = c_tPos;
        uint32_t packed = tt_data[idx];
        uint8_t c = (uint8_t)(packed >> 20);
        c_tPos = packed & TT_INDEX_MASK;
        c_nb++;
        ibwt_prefetch_next(tt_data, c_tPos);

        if (out >= out_end) return false;
        *out++ = c;
        c_crc = crc32_update(c_crc, c);

        if (c == c_run_ch) {
            c_run_cnt++;
            if (c_run_cnt == 4) {
                /* Consume run-length count byte from BWT walk */
                if (c_nb >= nblock) return false;
                uint32_t idx2 = c_tPos;
                uint32_t packed2 = tt_data[idx2];
                uint32_t cnt = packed2 >> 20;
                c_tPos = packed2 & TT_INDEX_MASK;
                c_nb++;

                if ((size_t)(out_end - out) < cnt) return false;
                repeat_store_hot(out, c_run_ch, cnt);
                c_run_extra += cnt;
                size_t block_out = (size_t)(out - block_begin) + cnt;
                if (c_run_extra >= CRC_DEFER_RUN_THRESHOLD &&
                    c_run_extra * 2 >= block_out) {
                    out += cnt;
                    c_run_cnt = 0;
                    goto phase3_deferred;
                }
                c_crc = crc32_update_repeat(c_crc, c_run_ch, cnt);
                out += cnt;
                c_run_cnt = 0;
            }
        } else {
            c_run_ch  = c;
            c_run_cnt = 1;
        }
    }

    c_crc ^= 0xFFFFFFFFu;
    goto phase3_done;

phase3_deferred:
    while (c_nb < nblock) {
        uint32_t idx = c_tPos;
        uint32_t packed = tt_data[idx];
        uint8_t c = (uint8_t)(packed >> 20);
        c_tPos = packed & TT_INDEX_MASK;
        c_nb++;
        ibwt_prefetch_next(tt_data, c_tPos);

        if (out >= out_end) return false;
        *out++ = c;

        if (c == c_run_ch) {
            c_run_cnt++;
            if (c_run_cnt == 4) {
                if (c_nb >= nblock) return false;
                uint32_t idx2 = c_tPos;
                uint32_t packed2 = tt_data[idx2];
                uint32_t cnt = packed2 >> 20;
                c_tPos = packed2 & TT_INDEX_MASK;
                c_nb++;

                if ((size_t)(out_end - out) < cnt) return false;
                repeat_store_hot(out, c_run_ch, cnt);
                out += cnt;
                c_run_cnt = 0;
            }
        } else {
            c_run_ch  = c;
            c_run_cnt = 1;
        }
    }

    c_crc = crc32_block(block_begin, (size_t)(out - block_begin));

phase3_done:
    if (c_crc != *block_crc_out) return false;
    written = (size_t)(out - dst);
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

    size_t written = 0;
    uint32_t combined_crc = 0;
    static thread_local std::vector<uint32_t> tt;  /* reused across calls and blocks */
    static thread_local std::vector<uint8_t> ll;
    if (tt.size() < BZ2_BLOCK_MAX) tt.resize(BZ2_BLOCK_MAX);
    if (ll.size() < BZ2_BLOCK_MAX) ll.resize(BZ2_BLOCK_MAX);

    while (true) {
        uint32_t m0 = br.read_bits(16);
        uint32_t m1 = br.read_bits(16);
        uint32_t m2 = br.read_bits(16);
        if (m0 == (uint32_t)-1) return 0;

        if (m0 == 0x1772 && m1 == 0x4538 && m2 == 0x5090) {
            /* Stream end marker */
            uint32_t shi = br.read_bits(16);
            uint32_t slo = br.read_bits(16);
            if (shi == (uint32_t)-1 || slo == (uint32_t)-1) return 0;
            uint32_t stored_crc = (shi << 16) | slo;
            if (stored_crc != combined_crc) return 0;
            break;
        }
        if (m0 != 0x3141 || m1 != 0x5926 || m2 != 0x5359) return 0;

        uint32_t block_crc = 0;
        if (!decode_block(br, dst, dst_cap, written, &block_crc, tt, ll))
            return 0;
        combined_crc = crc32_combine(combined_crc, block_crc);
    }

    return written;
}

} // namespace orot::bzip2
