#include "zstd.hpp"
#include "fse.hpp"
#include "huf.hpp"

#include <cstddef>
#include <cstdint>
#include <climits>
#include <cstring>

namespace orot { namespace zstd {

namespace {

/* ── Frame header ────────────────────────────────────────────────────────── */

struct FrameHeader {
    bool     single_segment    = false;
    bool     checksum          = false;
    uint64_t content_size      = 0;
    bool     has_content_size  = false;
    uint32_t dict_id           = 0;
    uint64_t window_size       = 0;
};

static uint32_t read_le24(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16);
}

static uint32_t read_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t read_le64(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

static bool read_uint(const uint8_t*& p, const uint8_t* end,
                      int bytes, uint64_t& out) noexcept {
    if (bytes == 0) { out = 0; return true; }
    if (p + bytes > end) return false;
    switch (bytes) {
    case 1: out = p[0]; break;
    case 2: out = static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8); break;
    case 4: out = read_le32(p); break;
    case 8: out = read_le64(p); break;
    default: return false;
    }
    p += bytes;
    return true;
}

static bool parse_frame_header(const uint8_t*& p, const uint8_t* end,
                                FrameHeader& header) noexcept {
    if (p >= end) return false;
    const uint8_t descriptor = *p++;
    const int fcs_flag = descriptor >> 6;
    header.single_segment = (descriptor & 0x20) != 0;
    header.checksum = (descriptor & 0x04) != 0;
    const int dict_id_flag = descriptor & 0x03;
    if ((descriptor & 0x18) != 0) return false;

    if (!header.single_segment) {
        if (p >= end) return false;
        const uint8_t window_descriptor = *p++;
        const uint64_t exponent = window_descriptor >> 3;
        const uint64_t mantissa = window_descriptor & 0x07;
        const uint64_t window_base = 1ull << (10 + exponent);
        header.window_size = window_base + (window_base >> 3) * mantissa;
    }

    int dict_bytes = 0;
    if (dict_id_flag == 1) dict_bytes = 1;
    else if (dict_id_flag == 2) dict_bytes = 2;
    else if (dict_id_flag == 3) dict_bytes = 4;

    uint64_t dict_id = 0;
    if (!read_uint(p, end, dict_bytes, dict_id)) return false;
    header.dict_id = static_cast<uint32_t>(dict_id);

    int fcs_bytes = 0;
    if (fcs_flag == 0) fcs_bytes = header.single_segment ? 1 : 0;
    else if (fcs_flag == 1) fcs_bytes = 2;
    else if (fcs_flag == 2) fcs_bytes = 4;
    else fcs_bytes = 8;

    uint64_t content_size = 0;
    if (!read_uint(p, end, fcs_bytes, content_size)) return false;
    if (fcs_flag == 1) content_size += 256;
    header.has_content_size = fcs_bytes != 0;
    header.content_size = content_size;
    if (header.single_segment) header.window_size = content_size;
    return true;
}

/* ── XXH64 ───────────────────────────────────────────────────────────────── */

static uint64_t xxh64_round(uint64_t acc, uint64_t input) noexcept {
    static constexpr uint64_t prime2 = 14029467366897019727ull;
    static constexpr uint64_t prime1 = 11400714785074694791ull;
    acc += input * prime2;
    acc = (acc << 31) | (acc >> 33);
    acc *= prime1;
    return acc;
}

static uint64_t xxh64_merge_round(uint64_t acc, uint64_t val) noexcept {
    static constexpr uint64_t prime1 = 11400714785074694791ull;
    static constexpr uint64_t prime4 = 9650029242287828579ull;
    val = xxh64_round(0, val);
    acc ^= val;
    acc = acc * prime1 + prime4;
    return acc;
}

static uint64_t xxh64_avalanche(uint64_t h) noexcept {
    static constexpr uint64_t prime2 = 14029467366897019727ull;
    static constexpr uint64_t prime3 = 1609587929392839161ull;
    h ^= h >> 33; h *= prime2;
    h ^= h >> 29; h *= prime3;
    h ^= h >> 32;
    return h;
}

static uint64_t xxh64(const uint8_t* input, size_t len) noexcept {
    static constexpr uint64_t prime1 = 11400714785074694791ull;
    static constexpr uint64_t prime2 = 14029467366897019727ull;
    static constexpr uint64_t prime3 = 1609587929392839161ull;
    static constexpr uint64_t prime4 = 9650029242287828579ull;
    static constexpr uint64_t prime5 = 2870177450012600261ull;
    const uint8_t* p = input;
    const uint8_t* const end = input + len;
    uint64_t h = 0;
    if (len >= 32) {
        const uint8_t* const limit = end - 32;
        uint64_t v1 = prime1 + prime2, v2 = prime2, v3 = 0, v4 = 0 - prime1;
        do {
            v1 = xxh64_round(v1, read_le64(p)); p += 8;
            v2 = xxh64_round(v2, read_le64(p)); p += 8;
            v3 = xxh64_round(v3, read_le64(p)); p += 8;
            v4 = xxh64_round(v4, read_le64(p)); p += 8;
        } while (p <= limit);
        h = ((v1 << 1) | (v1 >> 63)) + ((v2 << 7) | (v2 >> 57)) +
            ((v3 << 12) | (v3 >> 52)) + ((v4 << 18) | (v4 >> 46));
        h = xxh64_merge_round(h, v1); h = xxh64_merge_round(h, v2);
        h = xxh64_merge_round(h, v3); h = xxh64_merge_round(h, v4);
    } else {
        h = prime5;
    }
    h += len;
    while (p + 8 <= end) {
        uint64_t k1 = xxh64_round(0, read_le64(p));
        h ^= k1; h = ((h << 27) | (h >> 37)) * prime1 + prime4; p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<uint64_t>(read_le32(p)) * prime1;
        h = ((h << 23) | (h >> 41)) * prime2 + prime3; p += 4;
    }
    while (p < end) {
        h ^= static_cast<uint64_t>(*p++) * prime5;
        h = ((h << 11) | (h >> 53)) * prime1;
    }
    return xxh64_avalanche(h);
}

/* ── Extra bits tables (RFC 8878 Appendix A) ─────────────────────────────── */

static const uint8_t LL_EXTRA[36] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     1,  1,  1,  1,  2,  2,  3,  3,
     4,  6,  7,  8,  9, 10, 11, 12,
    13, 14, 15, 16
};
static const uint32_t LL_BASE[36] = {
         0,      1,      2,      3,      4,      5,      6,      7,
         8,      9,     10,     11,     12,     13,     14,     15,
        16,     18,     20,     22,     24,     28,     32,     40,
        48,     64,    128,    256,    512,   1024,   2048,   4096,
      8192,  16384,  32768,  65536
};

static const uint8_t ML_EXTRA[53] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,
     1,  1,  1,  1,  2,  2,  3,  3,
     4,  4,  5,  7,  8,  9, 10, 11,
    12, 13, 14, 15, 16
};
static const uint32_t ML_BASE[53] = {
         3,      4,      5,      6,      7,      8,      9,     10,
        11,     12,     13,     14,     15,     16,     17,     18,
        19,     20,     21,     22,     23,     24,     25,     26,
        27,     28,     29,     30,     31,     32,     33,     34,
        35,     37,     39,     41,     43,     47,     51,     59,
        67,     83,     99,    131,    259,    515,   1027,   2051,
      4099,   8195,  16387,  32771,  65539
};

/* ── Block state (repeat mode tracking across blocks) ───────────────────── */

struct BlockState {
    FseDTable ll_table, ml_table, of_table;
    HufDTable huf_table;
    bool have_ll  = false;
    bool have_ml  = false;
    bool have_of  = false;
    bool have_huf = false;
    uint32_t rep[3] = {1u, 4u, 8u};
};

/* Build single-symbol (RLE) FSE table. */
static void fse_build_rle_table(FseDTable& dt, uint8_t symbol) noexcept {
    dt.accuracy_log = 0;
    dt.entries[0].symbol   = symbol;
    dt.entries[0].nb_bits  = 0;
    dt.entries[0].baseline = 0;
}

/* Decode repeated offset (RFC 8878 §3.1.1.5.3). */
static uint32_t decode_rep_offset(uint32_t raw_ov, uint32_t ll,
                                   uint32_t rep[3]) noexcept {
    uint32_t offset;
    if (raw_ov >= 4u) {
        offset = raw_ov - 3u;
        rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = offset;
    } else if (ll == 0u) {
        if (raw_ov == 1u) {
            /* special: offset = rep[0]-1, no update */
            return rep[0] - 1u;
        } else if (raw_ov == 2u) {
            offset = rep[0];
            rep[1] = rep[0]; rep[0] = offset;
        } else { /* 3 */
            offset = rep[1];
            rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = offset;
        }
    } else {
        if (raw_ov == 1u) {
            offset = rep[0]; /* no update */
        } else if (raw_ov == 2u) {
            offset = rep[1];
            rep[1] = rep[0]; rep[0] = offset;
        } else { /* 3 */
            offset = rep[2];
            rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = offset;
        }
    }
    return offset;
}

/* ── Literals section decoder ────────────────────────────────────────────── */

/* Returns bytes consumed from src, or -1 on error.
   Decoded literals written to lits_buf[0..regen_size). */
static int decode_literals_section(
    const uint8_t* src, int src_len,
    uint8_t* lits_buf, int lits_cap,
    int* lits_len_out,
    BlockState& bs) noexcept
{
    if (src_len <= 0) return -1;

    /* Read up to 4 bytes for lhc */
    uint32_t lhc = 0;
    for (int i = 0, avail = src_len < 4 ? src_len : 4; i < avail; ++i)
        lhc |= static_cast<uint32_t>(src[i]) << (i * 8);

    const int block_type  = static_cast<int>(lhc & 3u);
    const int size_format = static_cast<int>((lhc >> 2) & 3u);

    if (block_type <= 1) {
        /* Raw or RLE */
        int lh_size, regen_size;
        switch (size_format) {
        case 0: case 2:
            lh_size    = 1;
            regen_size = static_cast<int>(lhc >> 3) & 0x1F;
            break;
        case 1:
            lh_size    = 2;
            regen_size = static_cast<int>((lhc >> 4) & 0xFFFu);
            break;
        default: /* 3 */
            lh_size    = 3;
            regen_size = static_cast<int>((lhc >> 4) & 0x3FFFFu);
            break;
        }
        if (lh_size > src_len) return -1;
        if (regen_size > lits_cap) return -1;

        if (block_type == 0) {
            /* Raw */
            if (lh_size + regen_size > src_len) return -1;
            std::memcpy(lits_buf, src + lh_size, static_cast<size_t>(regen_size));
            *lits_len_out = regen_size;
            return lh_size + regen_size;
        } else {
            /* RLE: one byte of literal value */
            if (lh_size >= src_len) return -1;
            std::memset(lits_buf, src[lh_size], static_cast<size_t>(regen_size));
            *lits_len_out = regen_size;
            return lh_size + 1;
        }
    }

    /* Compressed (2) or Treeless (3) */
    bool single_stream;
    int  lh_size, regen_size, comp_size;

    switch (size_format) {
    case 0:
        single_stream = true;
        lh_size    = 3;
        regen_size = static_cast<int>((lhc >> 4) & 0x3FFu);
        comp_size  = static_cast<int>((lhc >> 14) & 0x3FFu);
        break;
    case 1:
        single_stream = false;
        lh_size    = 3;
        regen_size = static_cast<int>((lhc >> 4) & 0x3FFu);
        comp_size  = static_cast<int>((lhc >> 14) & 0x3FFu);
        break;
    case 2:
        single_stream = false;
        lh_size    = 4;
        regen_size = static_cast<int>((lhc >> 4) & 0x3FFFu);
        comp_size  = static_cast<int>((lhc >> 18) & 0x3FFFu);
        break;
    default: /* 3 */
        single_stream = false;
        lh_size    = 5;
        regen_size = static_cast<int>((lhc >> 4) & 0x3FFFFu);
        if (src_len < 5) return -1;
        comp_size  = static_cast<int>(((lhc >> 22) & 0x3FFu) |
                                      (static_cast<uint32_t>(src[4]) << 10));
        break;
    }

    if (lh_size + comp_size > src_len) return -1;
    if (regen_size > lits_cap) return -1;
    if (regen_size == 0) { *lits_len_out = 0; return lh_size + comp_size; }

    const uint8_t* huf_src   = src + lh_size;
    int            huf_avail = comp_size;

    if (block_type == 2) {
        /* Compressed: decode Huffman table */
        int hdr_bytes = 0;
        if (huf_build_dtable(bs.huf_table, huf_src, huf_avail, &hdr_bytes) < 0)
            return -1;
        bs.have_huf = true;
        huf_src   += hdr_bytes;
        huf_avail -= hdr_bytes;
    } else {
        /* Treeless: reuse previous Huffman table */
        if (!bs.have_huf) return -1;
    }

    int rc;
    if (single_stream)
        rc = huf_decode_1stream(bs.huf_table, huf_src, huf_avail, lits_buf, regen_size);
    else
        rc = huf_decode_4stream(bs.huf_table, huf_src, huf_avail, lits_buf, regen_size);

    if (rc < 0) return -1;
    *lits_len_out = regen_size;
    return lh_size + comp_size;
}

/* ── Compressed block decoder ────────────────────────────────────────────── */

static int decode_compressed_block(
    const uint8_t* src, int src_len,
    uint8_t* dst_base, int dst_written, int dst_cap,
    BlockState& bs) noexcept
{
    /* Literals buffer on stack (max block = 128 KiB) */
    uint8_t lits_buf[ZSTD_BLOCK_MAX_SIZE];
    int     lits_len = 0;

    int lits_consumed = decode_literals_section(
        src, src_len, lits_buf, ZSTD_BLOCK_MAX_SIZE, &lits_len, bs);
    if (lits_consumed < 0) return -1;

    const uint8_t* sp     = src + lits_consumed;
    int            sp_len = src_len - lits_consumed;

    /* Sequences section */
    if (sp_len <= 0) {
        /* No sequences: copy literals */
        if (lits_len > dst_cap - dst_written) return -2;
        std::memcpy(dst_base + dst_written, lits_buf, static_cast<size_t>(lits_len));
        return lits_len;
    }

    /* Sequence count */
    int seq_count = 0, seq_hdr = 0;
    uint8_t b0 = sp[0];
    if (b0 == 0u) {
        seq_count = 0; seq_hdr = 1;
    } else if (b0 <= 127u) {
        seq_count = b0; seq_hdr = 1;
    } else if (b0 <= 254u) {
        if (sp_len < 2) return -1;
        seq_count = ((b0 - 128) << 8) | sp[1]; seq_hdr = 2;
    } else {
        if (sp_len < 3) return -1;
        seq_count = sp[1] | (sp[2] << 8); seq_count += 0x7F00; seq_hdr = 3;
    }

    if (seq_count == 0) {
        if (lits_len > dst_cap - dst_written) return -2;
        std::memcpy(dst_base + dst_written, lits_buf, static_cast<size_t>(lits_len));
        return lits_len;
    }

    if (seq_hdr >= sp_len) return -1;

    /* Modes byte */
    uint8_t modes = sp[seq_hdr];
    const int ll_mode = (modes >> 6) & 3;
    const int of_mode = (modes >> 4) & 3;
    const int ml_mode = (modes >> 2) & 3;

    int p = seq_hdr + 1; /* cursor in sp[] */

    /* Load LL, OF, ML tables */
    auto load_table = [&](FseDTable& dt, bool& have, int mode,
                          void (*init_default)(FseDTable&),
                          int max_sym, int max_log) -> bool {
        switch (mode) {
        case 0:
            init_default(dt);
            have = true;
            return true;
        case 1: /* RLE */
            if (p >= sp_len) return false;
            fse_build_rle_table(dt, sp[p++]);
            have = true;
            return true;
        case 2: { /* FSE_Compressed */
            int16_t norm[FSE_MAX_SYMS] = {};
            int alog = 0;
            int nc = fse_read_ncount(sp + p, sp_len - p, max_sym, &alog, norm);
            if (nc < 0 || alog > max_log) return false;
            p += nc;
            if (fse_build_dtable(dt, norm, max_sym, alog) < 0) return false;
            have = true;
            return true;
        }
        default: /* 3 = Repeat */
            return have;
        }
    };

    if (!load_table(bs.ll_table, bs.have_ll, ll_mode, fse_init_default_ll, 35, 9)) return -1;
    if (!load_table(bs.of_table, bs.have_of, of_mode, fse_init_default_of, 31, 8)) return -1;
    if (!load_table(bs.ml_table, bs.have_ml, ml_mode, fse_init_default_ml, 52, 9)) return -1;

    /* Sequence FSE bitstream (remainder of block) */
    if (p >= sp_len) return -1;
    FseBitStream fbs;
    if (!fbs.init(sp + p, sp_len - p)) return -1;

    /* Init FSE states: LL → OF → ML (reading from end of bitstream) */
    uint32_t ll_state = fse_init_state(bs.ll_table, fbs);
    uint32_t of_state = fse_init_state(bs.of_table, fbs);
    uint32_t ml_state = fse_init_state(bs.ml_table, fbs);

    /* Decode sequences and execute them inline */
    const uint8_t* lp            = lits_buf;
    int            lits_remaining = lits_len;
    int            produced       = 0;

    for (int i = 0; i < seq_count; ++i) {
        const bool is_last = (i == seq_count - 1);

        /* 1. Decode symbols */
        const uint8_t ll_sym = fse_decode_symbol(bs.ll_table, ll_state);
        const uint8_t of_sym = fse_decode_symbol(bs.of_table, of_state);
        const uint8_t ml_sym = fse_decode_symbol(bs.ml_table, ml_state);

        /* 2. Validate symbol ranges */
        if (ll_sym >= 36u || ml_sym >= 53u) return -1;

        /* 3. Read extra bits in zstd sequence order: OF, ML, LL */
        const uint32_t raw_ov  = (of_sym >= 1u)
            ? ((1u << of_sym) + fbs.read_bits(of_sym))
            : 1u; /* of_sym==0 → raw_ov=1 (repeated offset) */
        const uint32_t ml_val  = ML_BASE[ml_sym] + fbs.read_bits(ML_EXTRA[ml_sym]);
        const uint32_t ll_val  = LL_BASE[ll_sym] + fbs.read_bits(LL_EXTRA[ll_sym]);

        if (!fbs.is_valid()) return -1;

        if (!is_last) {
            ll_state = fse_next_state(bs.ll_table, ll_state, fbs);
            ml_state = fse_next_state(bs.ml_table, ml_state, fbs);
            of_state = fse_next_state(bs.of_table, of_state, fbs);
            if (!fbs.is_valid()) return -1;
        }

        /* 4. Resolve offset through rep table */
        const uint32_t offset  = decode_rep_offset(raw_ov, ll_val, bs.rep);

        /* 5. Execute: copy ll_val literals */
        if (ll_val > static_cast<uint32_t>(lits_remaining)) return -1;
        if (produced + static_cast<int>(ll_val) > dst_cap - dst_written) return -2;
        std::memcpy(dst_base + dst_written + produced, lp,
                    static_cast<size_t>(ll_val));
        lp              += ll_val;
        lits_remaining  -= static_cast<int>(ll_val);
        produced        += static_cast<int>(ll_val);

        /* 6. Execute: copy ml_val match bytes */
        if (offset == 0u) return -1;
        if (offset > static_cast<uint32_t>(dst_written + produced)) return -1;
        if (produced + static_cast<int>(ml_val) > dst_cap - dst_written) return -2;

        const uint8_t* msrc = dst_base + dst_written + produced - offset;
        uint8_t*       mdst = dst_base + dst_written + produced;
        /* overlap-safe byte-by-byte */
        for (uint32_t k = 0u; k < ml_val; ++k) mdst[k] = msrc[k];
        produced += static_cast<int>(ml_val);
    }

    /* Copy remaining literals */
    if (lits_remaining > 0) {
        if (produced + lits_remaining > dst_cap - dst_written) return -2;
        std::memcpy(dst_base + dst_written + produced, lp,
                    static_cast<size_t>(lits_remaining));
        produced += lits_remaining;
    }

    return produced;
}

} // anonymous namespace

/* ── Public API ──────────────────────────────────────────────────────────── */

int zstd_compress_bound(int src_len) noexcept {
    if (src_len < 0) return -1;
    const int block_overhead = (src_len / 128) + 64;
    if (src_len > INT_MAX - block_overhead) return -1;
    return src_len + block_overhead;
}

int zstd_compress(const uint8_t* src, int src_len,
                  uint8_t* dst, int dst_cap, int level) noexcept {
    (void)src; (void)src_len; (void)dst; (void)dst_cap; (void)level;
    return -1;
}

int zstd_decompress(const uint8_t* src, int src_len,
                    uint8_t* dst, int dst_cap) noexcept {
    if (src_len < 0 || dst_cap < 0) return -1;
    if ((src_len > 0 && src == nullptr) || (dst_cap > 0 && dst == nullptr)) return -1;

    const uint8_t* p            = src;
    const uint8_t* const end    = src + src_len;
    uint8_t empty_output        = 0;
    uint8_t* const dst_base     = (dst != nullptr) ? dst : &empty_output;

    /* Skip skippable frames (magic 0x184D2A50..0x184D2A5F) */
    while (p + 8 <= end) {
        if (p + 4 > end) return -1;
        const uint32_t magic = read_le32(p);
        if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {
            p += 4;
            const uint32_t skip_size = read_le32(p); p += 4;
            if (p + skip_size > end) return -1;
            p += skip_size;
            continue;
        }
        break;
    }

    if (p + 4 > end) return -1;
    const uint32_t magic = read_le32(p); p += 4;
    if (magic != ZSTD_MAGIC) return -1;

    FrameHeader header;
    if (!parse_frame_header(p, end, header)) return -1;

    /* Dictionary frames are not supported */
    if (header.dict_id != 0u) return -1;

    BlockState bs;
    uint8_t* out           = dst_base;
    int      dst_written   = 0;
    uint64_t produced_total = 0;
    bool     last_block    = false;

    while (!last_block) {
        if (p + 3 > end) return -1;
        const uint32_t block_header = read_le24(p); p += 3;

        last_block       = (block_header & 0x01u) != 0;
        const int block_type   = static_cast<int>((block_header >> 1) & 0x03u);
        const uint32_t block_size = block_header >> 3;

        if (block_size > static_cast<uint32_t>(ZSTD_BLOCK_MAX_SIZE)) return -1;
        if (block_type == 1) {
            if (p >= end) return -1;
        } else {
            if (p + block_size > end) return -1;
        }

        int block_produced = 0;

        if (block_type == 0) {
            /* Raw block */
            if (dst_cap - dst_written < static_cast<int>(block_size)) return -2;
            if (block_size > 0) {
                std::memcpy(out + dst_written, p, block_size);
            }
            block_produced = static_cast<int>(block_size);
            p += block_size;
        } else if (block_type == 1) {
            /* RLE block */
            if (p >= end) return -1;
            if (dst_cap - dst_written < static_cast<int>(block_size)) return -2;
            if (block_size > 0) {
                std::memset(out + dst_written, *p, block_size);
            }
            block_produced = static_cast<int>(block_size);
            ++p;
        } else if (block_type == 2) {
            /* Compressed block */
            block_produced = decode_compressed_block(
                p, static_cast<int>(block_size),
                out, dst_written, dst_cap, bs);
            if (block_produced < 0) return block_produced;
            p += block_size;
        } else {
            return -1; /* reserved */
        }

        dst_written     += block_produced;
        produced_total  += static_cast<uint64_t>(block_produced);

        if (header.has_content_size && produced_total > header.content_size) return -1;
        if (produced_total > static_cast<uint64_t>(INT_MAX)) return -1;
    }

    if (header.has_content_size && produced_total != header.content_size) return -1;

    if (header.checksum) {
        if (p + 4 > end) return -1;
        const uint32_t expected = read_le32(p); p += 4;
        const uint32_t actual = static_cast<uint32_t>(
            xxh64(dst_base, static_cast<size_t>(produced_total)) & 0xFFFFFFFFu);
        if (actual != expected) return -3;
    }

    if (p != end) return -1;
    return static_cast<int>(produced_total);
}

} } /* namespace orot::zstd */
