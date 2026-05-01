#include "lzma_decompress.hpp"
#include "lzma_prob_model.hpp"

#include <cstring>
#include <memory>
#include <new>

namespace orot { namespace lzma {

/* ── Header parsing ──────────────────────────────────────────────────────── */

uint64_t lzma_header_uncompressed_size(const uint8_t* src) noexcept {
    if (!src) return (uint64_t)-1;
    uint8_t props = src[0];
    /* Validate props byte: pb*45+lp*9+lc, max value = 4*45+4*9+8 = 224 */
    if (props > 224) return (uint64_t)-1;
    uint64_t usz;
    memcpy(&usz, src + 5, 8);
    return usz;
}

/* ── Literal decode ──────────────────────────────────────────────────────── */

static uint8_t decode_literal(RangeDecoder& rd, LzmaProbTables& pt,
                              uint8_t match_byte, bool is_char,
                              int lit_ctx) noexcept {
    Prob* probs = pt.literal[lit_ctx];
    uint32_t sym = 1;

    if (is_char) {
        for (int i = 7; i >= 0; --i)
            sym = (sym << 1) | rd.decode_bit(&probs[sym]);
    } else {
        uint32_t mb = (uint32_t)match_byte;
        for (int i = 7; i >= 0; --i) {
            int mbit = (mb >> i) & 1;
            int bit  = rd.decode_bit(&probs[0x100 + (mbit << 8) + sym]);
            sym = (sym << 1) | bit;
            if (mbit != bit) {
                /* diverged: continue with standard tree */
                for (int j = i - 1; j >= 0; --j)
                    sym = (sym << 1) | rd.decode_bit(&probs[sym]);
                break;
            }
        }
    }
    return (uint8_t)(sym - 256u);
}

/* ── Main decompressor ───────────────────────────────────────────────────── */

size_t lzma_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_cap) noexcept
{
    /* Need at least 13-byte header + 5-byte RC init */
    if (src_len < 18) return 0;

    /* Parse header */
    uint8_t  props_byte = src[0];
    if (props_byte > 224) return 0;

    uint8_t pb_lp = props_byte / 9;
    int lc = (int)(props_byte % 9);
    int lp = (int)(pb_lp % 5);
    int pb = (int)(pb_lp / 5);
    if (pb > 4 || lp > 4 || lc > 8) return 0;
    if (lc + lp > 4) return 0;  /* SDK constraint */

    uint32_t dict_size;
    memcpy(&dict_size, src + 1, 4);
    if (dict_size < 4096) dict_size = 4096;

    uint64_t usz;
    memcpy(&usz, src + 5, 8);
    bool has_usz = (usz != 0xFFFFFFFFFFFFFFFFull);

    if (has_usz && usz > dst_cap) return 0;

    int pos_states = 1 << pb;
    int pos_mask   = pos_states - 1;
    /* Allocate probability tables */
    std::unique_ptr<LzmaProbTables> pt(new (std::nothrow) LzmaProbTables);
    if (!pt) return 0;
    pt->reset(lc, lp);

    /* Allocate dictionary ring buffer */
    uint32_t dmask = dict_size - 1;
    /* Dict size must be power of 2 for mask trick; if not, use full size */
    bool     pow2  = (dict_size & dmask) == 0;
    if (!pow2) dmask = 0xFFFFFFFFu;  /* fallback: modulo (slower but correct) */

    std::unique_ptr<uint8_t[]> dict_buf(new (std::nothrow) uint8_t[dict_size]());
    if (!dict_buf) return 0;

    uint32_t dict_pos = 0;  /* next write pos in ring buffer */

    auto dict_get = [&](uint32_t dist1) -> uint8_t {
        /* dist1 = 1-based distance (dist+1), so actual offset = dist1 from write head */
        uint32_t off = (dict_pos + dict_size - (dist1 % dict_size)) % dict_size;
        return dict_buf[off];
    };

    auto dict_put = [&](uint8_t b) {
        dict_buf[dict_pos] = b;
        dict_pos = (dict_pos + 1 == dict_size) ? 0 : dict_pos + 1;
    };

    /* Init range decoder (5 bytes after the 13-byte header) */
    RangeDecoder rd;
    if (!rd.init(src + 13, src_len - 13)) return 0;

    LzmaState state;
    uint32_t rep[4] = { 0, 0, 0, 0 };  /* 0-based distances */
    uint64_t out_pos = 0;

    auto lit_ctx = [&](uint64_t p2, uint8_t prev) -> int {
        return (int)(((p2 & ((uint64_t)((1u << lp) - 1))) << lc)
                   | (prev >> (8 - lc)));
    };

    for (;;) {
        if (has_usz && out_pos >= usz) break;

        int ps = (int)(out_pos & (uint32_t)pos_mask);
        uint8_t prev_byte = (out_pos > 0) ? dict_get(1) : 0;
        int lctx = lit_ctx(out_pos, prev_byte);

        if (rd.decode_bit(&pt->is_match[state.state][ps]) == 0) {
            /* Literal */
            uint8_t match_byte = (out_pos > 0 && rep[0] + 1 <= out_pos)
                                 ? dict_get(rep[0] + 1) : 0;

            uint8_t byte = decode_literal(rd, *pt, match_byte,
                                          state.is_char_state(), lctx);
            if (out_pos < dst_cap) dst[out_pos] = byte;
            dict_put(byte);
            state.update_literal();
            out_pos++;

        } else {
            /* Match or rep */
            uint32_t len;
            uint32_t dist0;  /* 0-based */

            if (rd.decode_bit(&pt->is_rep[state.state]) == 0) {
                /* New match */
                len  = (uint32_t)decode_len(rd, pt->match_len, ps);
                dist0 = decode_dist(rd, *pt, (int)len);

                /* End-of-stream marker: dist0 == 0xFFFFFFFF */
                if (dist0 == 0xFFFFFFFFu) break;

                rep[3] = rep[2]; rep[2] = rep[1]; rep[1] = rep[0];
                rep[0] = dist0;
                state.update_match();

            } else {
                /* Rep match */
                if (rd.decode_bit(&pt->is_rep_g0[state.state]) == 0) {
                    /* Rep0 or short rep */
                    if (rd.decode_bit(&pt->is_rep0_long[state.state][ps]) == 0) {
                        /* Short rep: 1-byte match with rep0 */
                        state.update_short_rep();
                        uint8_t b = (rep[0] + 1 <= out_pos) ? dict_get(rep[0] + 1) : 0;
                        if (out_pos < dst_cap) dst[out_pos] = b;
                        dict_put(b);
                        out_pos++;
                        continue;
                    }
                    dist0 = rep[0];
                } else {
                    if (rd.decode_bit(&pt->is_rep_g1[state.state]) == 0) {
                        dist0 = rep[1];
                    } else {
                        if (rd.decode_bit(&pt->is_rep_g2[state.state]) == 0) {
                            dist0 = rep[2];
                        } else {
                            dist0 = rep[3];
                            rep[3] = rep[2];
                        }
                        rep[2] = rep[1];
                    }
                    rep[1] = rep[0];
                    rep[0] = dist0;
                }
                state.update_rep();
                len = (uint32_t)decode_len(rd, pt->rep_len, ps);
            }

            /* Copy len bytes from distance dist0+1 */
            if (dist0 + 1 > out_pos && out_pos != 0) {
                /* Dictionary underflow — corrupt input */
                return 0;
            }

            for (uint32_t k = 0; k < len; ++k) {
                uint8_t b = dict_get(dist0 + 1);
                if (out_pos < dst_cap) dst[out_pos] = b;
                dict_put(b);
                out_pos++;
                if (has_usz && out_pos >= usz) break;
            }
        }

        if (rd.corrupted) return 0;
    }

    if (has_usz && out_pos != usz) return 0;
    return (size_t)out_pos;
}

} } /* namespace orot::lzma */
