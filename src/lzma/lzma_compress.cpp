#include "lzma_compress.hpp"
#include "lzma_prob_model.hpp"

#include <cstring>
#include <memory>
#include <algorithm>
#include <new>

namespace orot { namespace lzma {

/* ── Level configuration ─────────────────────────────────────────────────── */

LzmaConfig lzma_config_for_level(int level) noexcept {
    if (level < 1) level = 1;
    if (level > 9) level = 9;

    /* dict_size: 256KB (L1) → 32MB (L9), powers of 2 */
    static const uint32_t kDictSize[10] = {
        0,
        (1u << 18),  /* L1: 256KB  */
        (1u << 19),  /* L2: 512KB  */
        (1u << 20),  /* L3: 1MB    */
        (1u << 21),  /* L4: 2MB    */
        (1u << 22),  /* L5: 4MB    */
        (1u << 23),  /* L6: 8MB    */
        (1u << 24),  /* L7: 16MB   */
        (1u << 24),  /* L8: 16MB   */
        (1u << 25),  /* L9: 32MB   */
    };
    static const int kNiceLen[10] = { 0, 16, 20, 24, 32, 32, 48, 48, 64, 64 };
    static const int kDepth[10]   = { 0,  4,  8, 16, 32, 32, 64, 64,128,128 };
    static const int kHashBits[10]= { 0, 16, 16, 16, 17, 18, 18, 19, 20, 20 };

    LzmaConfig cfg;
    cfg.dict_size = kDictSize[level];
    cfg.lc        = 3;
    cfg.lp        = 0;
    cfg.pb        = 2;
    cfg.nice_len  = kNiceLen[level];
    cfg.depth     = kDepth[level];
    cfg.hash_bits = kHashBits[level];
    return cfg;
}

size_t lzma_compress_bound(size_t src_len) noexcept {
    /* Conservative bound for this encoder.
     * Incompressible inputs can expand noticeably because each byte is encoded
     * through adaptive range-coded literals plus the end marker. */
    return 13 + 5 + (src_len * 2) + 64;
}

/* ── Match finder ────────────────────────────────────────────────────────── */

struct MatchFinder {
    const uint8_t* src;
    uint32_t       src_size;
    uint32_t       pos;
    uint32_t       dict_size;
    uint32_t       hash_mask;
    uint32_t*      head;   /* hash table: hash_size entries */
    uint32_t*      chain;  /* hash chain: dict_size entries (circular) */

    struct Match {
        uint32_t len;
        uint32_t dist;  /* 0-based: actual distance = dist+1 */
    };

    static uint32_t hash4(const uint8_t* p) noexcept {
        uint32_t v;
        memcpy(&v, p, 4);
        return (v * 2654435761u);
    }

    void insert(uint32_t p) noexcept {
        if (p + 4 > src_size) return;
        uint32_t h = (hash4(src + p) >> (32 - 20)) & hash_mask;
        chain[p & (dict_size - 1)] = head[h];
        head[h] = p + 1;  /* +1 so 0 means "empty" */
    }

    int find(uint32_t p, int nice_len, int depth, Match* out, int max_out) noexcept {
        if (p + 4 > src_size) return 0;

        uint32_t h = (hash4(src + p) >> (32 - 20)) & hash_mask;
        uint32_t cur = head[h];
        chain[p & (dict_size - 1)] = cur;
        head[h] = p + 1;

        int count = 0;
        uint32_t best_len = 1;
        const uint8_t* sp = src + p;
        uint32_t avail = src_size - p;

        for (int d = 0; d < depth && cur != 0; ++d) {
            uint32_t match_pos = cur - 1;
            if (p < match_pos || p - match_pos > dict_size) break;

            uint32_t dist = p - match_pos - 1;  /* 0-based distance */
            const uint8_t* mp = src + match_pos;

            /* Quick check first byte */
            if (mp[0] != sp[0]) {
                cur = chain[match_pos & (dict_size - 1)];
                continue;
            }

            /* Extend match */
            uint32_t max_len = (uint32_t)std::min(avail, (uint32_t)kMatchMaxLen);
            uint32_t len = 1;
            while (len < max_len && mp[len] == sp[len])
                ++len;

            if (len > best_len) {
                best_len = len;
                if (count < max_out)
                    out[count++] = {len, dist};
                else
                    out[count - 1] = {len, dist};  /* replace last */

                if (len >= (uint32_t)nice_len || len >= (uint32_t)kMatchMaxLen)
                    break;
            }

            cur = chain[match_pos & (dict_size - 1)];
        }
        return count;
    }
};

/* ── Literal encoding ────────────────────────────────────────────────────── */

static void encode_literal(RangeEncoder& rc, LzmaProbTables& pt,
                           uint8_t byte, uint8_t match_byte,
                           bool is_char, int lit_ctx) noexcept {
    Prob* probs = pt.literal[lit_ctx];
    uint32_t m = 1;
    if (is_char) {
        /* Standard 8-bit tree */
        for (int i = 7; i >= 0; --i) {
            int bit = (byte >> i) & 1;
            rc.encode_bit(&probs[m], bit);
            m = (m << 1) | bit;
        }
    } else {
        /* Matched literal: use match byte prediction */
        uint32_t mb = (uint32_t)match_byte;
        for (int i = 7; i >= 0; --i) {
            int bit  = (byte >> i) & 1;
            int mbit = (mb >> i) & 1;
            rc.encode_bit(&probs[0x100 + (mbit << 8) + m], bit);
            if (mbit != bit) {
                /* prediction diverged: switch to standard tree for rest */
                m = (m << 1) | bit;
                /* encode remaining bits with standard tree */
                for (int j = i - 1; j >= 0; --j) {
                    bit = (byte >> j) & 1;
                    rc.encode_bit(&probs[m], bit);
                    m = (m << 1) | bit;
                }
                return;
            }
            m = (m << 1) | bit;
        }
    }
}

/* ── Main compressor ─────────────────────────────────────────────────────── */

size_t lzma_compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_cap,
    int            level) noexcept
{
    LzmaConfig cfg = lzma_config_for_level(level);
    if (dst_cap < lzma_compress_bound(src_len)) return 0;

    /* Cap dict_size and hash table to actual input size (no point in larger). */
    if (src_len > 0) {
        /* Smallest power-of-2 >= src_len, min 4096 */
        uint32_t effective = 4096u;
        while (effective < (uint32_t)src_len && effective < cfg.dict_size)
            effective <<= 1;
        if (effective < cfg.dict_size) {
            cfg.dict_size = effective;
            /* Adjust hash_bits proportionally */
            int hb = cfg.hash_bits;
            while (hb > 12 && (1u << hb) > effective * 4)
                --hb;
            cfg.hash_bits = hb;
        }
    } else {
        cfg.dict_size = 4096;
        cfg.hash_bits = 12;
    }

    /* ── Allocate match finder state ── */
    uint32_t hash_size = 1u << cfg.hash_bits;
    uint32_t hash_mask = hash_size - 1;

    std::unique_ptr<uint32_t[]> head_buf(new (std::nothrow) uint32_t[hash_size]());
    std::unique_ptr<uint32_t[]> chain_buf(new (std::nothrow) uint32_t[cfg.dict_size]());
    if (!head_buf || !chain_buf) return 0;

    MatchFinder mf;
    mf.src       = src ? src : (const uint8_t*)"";  /* safe empty ptr */
    mf.src_size  = (uint32_t)src_len;
    mf.pos       = 0;
    mf.dict_size = cfg.dict_size;
    mf.hash_mask = hash_mask;
    mf.head      = head_buf.get();
    mf.chain     = chain_buf.get();

    /* ── Allocate prob tables ── */
    std::unique_ptr<LzmaProbTables> pt(new (std::nothrow) LzmaProbTables);
    if (!pt) return 0;
    pt->reset(cfg.lc, cfg.lp);

    /* ── Write LZMA alone header ── */
    uint8_t* p = dst;
    /* Properties byte */
    *p++ = (uint8_t)((cfg.pb * 5 + cfg.lp) * 9 + cfg.lc);
    /* Dict size (LE uint32) */
    uint32_t ds = cfg.dict_size;
    memcpy(p, &ds, 4); p += 4;
    /* Uncompressed size (LE int64) */
    uint64_t usz = (uint64_t)src_len;
    memcpy(p, &usz, 8); p += 8;

    /* ── Init range encoder ── */
    /* RangeEncoder::flush() emits the 5-byte decoder init prefix itself. */
    RangeEncoder rc;
    rc.reset(p, (size_t)(dst_cap - (p - dst)));

    /* ── State ── */
    LzmaState state;
    uint32_t rep[4] = { 0, 0, 0, 0 };  /* 0-based distances */

    uint32_t pos = 0;
    int pos_states = 1 << cfg.pb;
    int pos_mask   = pos_states - 1;

    auto lit_ctx = [&](uint32_t p2, uint8_t prev) -> int {
        return (int)(((p2 & ((1u << cfg.lp) - 1)) << cfg.lc)
                   | (prev >> (8 - cfg.lc)));
    };

    /* For empty input: just emit end marker */
    if (src_len == 0) {
        /* Encode end-of-stream marker: match with distance 0xFFFFFFFF */
        int ps = 0;
        rc.encode_bit(&pt->is_match[state.state][ps], 1);
        rc.encode_bit(&pt->is_rep[state.state], 0);
        encode_len(rc, pt->match_len, 2, ps);
        /* Distance slot for end marker (0xFFFFFFFF → slot 63) */
        rc.encode_bit_tree(pt->dist_slot[0], kNumPosSlotBits, 63);
        int footerBits = (63 >> 1) - 1;  /* 30 */
        rc.encode_direct_bits(0xFFFFFF, footerBits - kNumAlignBits);
        rc.encode_bit_tree_reverse(pt->dist_align, kNumAlignBits, 0xF);
    }

    MatchFinder::Match matches[16];

    while (pos < (uint32_t)src_len) {
        int ps = (int)(pos & (uint32_t)pos_mask);
        uint8_t cur_byte  = src[pos];
        uint8_t prev_byte = (pos > 0) ? src[pos - 1] : 0;
        int lctx = lit_ctx(pos, prev_byte);

        /* Check rep distances */
        uint32_t rep_len[4] = { 0, 0, 0, 0 };
        for (int r = 0; r < 4; ++r) {
            if (rep[r] + 1 > pos) continue;
            uint32_t match_pos = pos - rep[r] - 1;
            uint32_t max_len = (uint32_t)std::min((uint32_t)(src_len - pos), (uint32_t)kMatchMaxLen);
            uint32_t len = 0;
            while (len < max_len && src[match_pos + len] == src[pos + len])
                ++len;
            rep_len[r] = len;
        }

        /* Find hash chain matches */
        int nm = 0;
        if (pos + 4 <= (uint32_t)src_len) {
            nm = mf.find(pos, cfg.nice_len, cfg.depth, matches, 16);
        } else {
            mf.insert(pos);
        }

        /* Choose best action */
        /* Priority: rep match ≥ 2, then new match ≥ 2, then literal */
        int    best_rep   = -1;
        uint32_t best_rep_len = 1;
        for (int r = 0; r < 4; ++r) {
            if (rep_len[r] > best_rep_len) {
                best_rep_len = rep_len[r];
                best_rep     = r;
            }
        }

        uint32_t best_match_len  = 0;
        uint32_t best_match_dist = 0;
        for (int i = 0; i < nm; ++i) {
            if (matches[i].len > best_match_len) {
                best_match_len  = matches[i].len;
                best_match_dist = matches[i].dist;
            }
        }

        /* Encode symbol */
        if (best_rep >= 0 && best_rep_len >= (uint32_t)kMatchMinLen
            && (best_match_len < 2 || best_rep_len >= best_match_len))
        {
            /* Rep match */
            rc.encode_bit(&pt->is_match[state.state][ps], 1);
            rc.encode_bit(&pt->is_rep[state.state], 1);

            if (best_rep == 0) {
                rc.encode_bit(&pt->is_rep_g0[state.state], 0);
                if (best_rep_len == 1) {
                    rc.encode_bit(&pt->is_rep0_long[state.state][ps], 0);
                    state.update_short_rep();
                    pos++;
                    continue;
                }
                rc.encode_bit(&pt->is_rep0_long[state.state][ps], 1);
            } else {
                rc.encode_bit(&pt->is_rep_g0[state.state], 1);
                if (best_rep == 1) {
                    rc.encode_bit(&pt->is_rep_g1[state.state], 0);
                } else {
                    rc.encode_bit(&pt->is_rep_g1[state.state], 1);
                    rc.encode_bit(&pt->is_rep_g2[state.state], best_rep == 2 ? 0 : 1);
                }
                /* Rotate rep history */
                uint32_t dist = rep[best_rep];
                for (int r = best_rep; r > 0; --r)
                    rep[r] = rep[r - 1];
                rep[0] = dist;
            }

            encode_len(rc, pt->rep_len, (int)best_rep_len, ps);
            state.update_rep();

            /* Advance past matched bytes, inserting into hash chain */
            for (uint32_t k = 1; k < best_rep_len; ++k)
                mf.insert(pos + k);
            pos += best_rep_len;

        } else if (best_match_len >= (uint32_t)kMatchMinLen) {
            /* New match */
            rc.encode_bit(&pt->is_match[state.state][ps], 1);
            rc.encode_bit(&pt->is_rep[state.state], 0);

            encode_len(rc, pt->match_len, (int)best_match_len, ps);
            encode_dist(rc, *pt, best_match_dist, (int)best_match_len);

            /* Update rep history */
            rep[3] = rep[2]; rep[2] = rep[1]; rep[1] = rep[0];
            rep[0] = best_match_dist;
            state.update_match();

            for (uint32_t k = 1; k < best_match_len; ++k)
                mf.insert(pos + k);
            pos += best_match_len;

        } else {
            /* Literal */
            rc.encode_bit(&pt->is_match[state.state][ps], 0);

            uint8_t match_byte = (pos > 0 && rep[0] + 1 <= pos)
                                 ? src[pos - rep[0] - 1] : 0;

            encode_literal(rc, *pt, cur_byte, match_byte,
                           state.is_char_state(), lctx);
            state.update_literal();
            pos++;
        }
    }

    /* End-of-stream marker: match with max distance 0xFFFFFFFF */
    {
        int ps = (int)(pos & (uint32_t)pos_mask);
        rc.encode_bit(&pt->is_match[state.state][ps], 1);
        rc.encode_bit(&pt->is_rep[state.state], 0);
        encode_len(rc, pt->match_len, kMatchMinLen, ps);
        rc.encode_bit_tree(pt->dist_slot[0], kNumPosSlotBits, 63);
        int footerBits = (63 >> 1) - 1;  /* 30 */
        rc.encode_direct_bits(0xFFFFFF, footerBits - kNumAlignBits);
        rc.encode_bit_tree_reverse(pt->dist_align, kNumAlignBits, 0xF);
    }

    if (!rc.flush()) return 0;

    /* Total bytes: header (13) + range-coded payload (including 5-byte init) */
    size_t data_len = rc.bytes_written(p);
    return (size_t)(p - dst) + data_len;
}

} } /* namespace orot::lzma */
