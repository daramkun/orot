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

static uint8_t decode_literal_plain(RangeDecoder& rd, LzmaProbTables& pt,
                                    int lit_ctx) noexcept {
    Prob* probs = pt.literal[lit_ctx];
    uint32_t sym = 1;
    sym = (sym << 1) | rd.decode_bit(&probs[sym]);
    sym = (sym << 1) | rd.decode_bit(&probs[sym]);
    sym = (sym << 1) | rd.decode_bit(&probs[sym]);
    sym = (sym << 1) | rd.decode_bit(&probs[sym]);
    sym = (sym << 1) | rd.decode_bit(&probs[sym]);
    sym = (sym << 1) | rd.decode_bit(&probs[sym]);
    sym = (sym << 1) | rd.decode_bit(&probs[sym]);
    sym = (sym << 1) | rd.decode_bit(&probs[sym]);
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

    /* Init range decoder (5 bytes after the 13-byte header) */
    RangeDecoder rd;
    if (!rd.init(src + 13, src_len - 13)) return 0;

    LzmaState state;
    uint32_t rep[4] = { 0, 0, 0, 0 };  /* 0-based distances */
    uint64_t out_pos = 0;

    const uint32_t lp_mask = (1u << lp) - 1u;
    const int lit_shift = 8 - lc;

    auto hist_get = [&](uint32_t dist1) -> uint8_t {
        return (dist1 <= out_pos) ? dst[out_pos - dist1] : 0;
    };

    for (;;) {
        if (has_usz && out_pos >= usz) break;

        int ps = (int)(out_pos & (uint32_t)pos_mask);
        uint8_t prev_byte = (out_pos > 0) ? dst[out_pos - 1] : 0;
        int lctx = (int)(((out_pos & lp_mask) << lc) | (prev_byte >> lit_shift));

        if (rd.decode_bit(&pt->is_match[state.state][ps]) == 0) {
            /* Literal */
            const bool is_char = state.is_char_state();
            uint8_t byte;
            if (is_char) {
                byte = decode_literal_plain(rd, *pt, lctx);
            } else {
                uint8_t match_byte = (out_pos > 0 && rep[0] + 1 <= out_pos)
                                     ? hist_get(rep[0] + 1) : 0;
                byte = decode_literal(rd, *pt, match_byte, false, lctx);
            }
            if (out_pos >= dst_cap) return 0;
            dst[out_pos] = byte;
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
                        uint8_t b = (rep[0] + 1 <= out_pos) ? hist_get(rep[0] + 1) : 0;
                        if (out_pos >= dst_cap) return 0;
                        dst[out_pos] = b;
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
            const uint64_t dist1_u64 = static_cast<uint64_t>(dist0) + 1u;
            if (dist1_u64 > dict_size) return 0;
            if (dist1_u64 > out_pos && out_pos != 0) {
                /* Dictionary underflow — corrupt input */
                return 0;
            }

            size_t copy_len = len;
            if (has_usz && out_pos + copy_len > usz)
                copy_len = static_cast<size_t>(usz - out_pos);
            if (out_pos + copy_len > dst_cap) return 0;

            const size_t dist1 = static_cast<size_t>(dist1_u64);
            uint8_t* out = dst + out_pos;
            if (dist1 == 1) {
                std::memset(out, out[-1], copy_len);
            } else if (dist1 >= copy_len) {
                std::memcpy(out, out - dist1, copy_len);
            } else {
                std::memcpy(out, out - dist1, dist1);
                size_t filled = dist1;
                while (filled < copy_len) {
                    const size_t chunk = (filled < copy_len - filled)
                        ? filled : copy_len - filled;
                    std::memcpy(out + filled, out, chunk);
                    filled += chunk;
                }
            }
            out_pos += copy_len;
        }

        if (rd.corrupted) return 0;
    }

    if (has_usz && out_pos != usz) return 0;
    return (size_t)out_pos;
}

} } /* namespace orot::lzma */
