#pragma once

#include "range_coder.hpp"
#include <cstring>

namespace orot { namespace lzma {

/* ── LZMA constants ──────────────────────────────────────────────────────── */

static constexpr int kNumStates        = 12;
static constexpr int kNumPosBitsMax    = 4;   /* max pb */
static constexpr int kNumPosStatesMax  = (1 << kNumPosBitsMax);  /* 16 */
static constexpr int kNumLenToPosStates = 4;  /* floor((len-2)/...) capped at 3 */

static constexpr int kStartPosModelIndex = 4;
static constexpr int kEndPosModelIndex   = 14;
static constexpr int kNumPosSlotBits     = 6;
static constexpr int kNumAlignBits       = 4;
static constexpr int kAlignTableSize     = (1 << kNumAlignBits);  /* 16 */

/* dist_special: covers slots [4, 13], total 124 probs (pad to 128) */
static constexpr int kDistSpecialSize = 128;

static constexpr int kLenLowBits  = 3;
static constexpr int kLenMidBits  = 3;
static constexpr int kLenHighBits = 8;
static constexpr int kLenLowSymbols  = (1 << kLenLowBits);   /* 8  */
static constexpr int kLenMidSymbols  = (1 << kLenMidBits);   /* 8  */
static constexpr int kLenHighSymbols = (1 << kLenHighBits);  /* 256 */

static constexpr int kMatchMinLen = 2;
static constexpr int kMatchMaxLen = kMatchMinLen + kLenLowSymbols + kLenMidSymbols + kLenHighSymbols - 1;  /* 273 */

/* ── LZMA State machine ──────────────────────────────────────────────────── */

struct LzmaState {
    uint32_t state = 0;  /* 0..11 */

    bool is_char_state() const noexcept { return state < 7; }

    void update_literal() noexcept {
        if (state < 4)       state = 0;
        else if (state < 10) state -= 3;
        else                 state -= 6;
    }
    void update_match()     noexcept { state = state < 7 ? 7 : 10; }
    void update_rep()       noexcept { state = state < 7 ? 8 : 11; }
    void update_short_rep() noexcept { state = state < 7 ? 9 : 11; }
};

/* ── Length coder probability table ─────────────────────────────────────── */

struct LenProbs {
    Prob choice;                             /* low vs (mid|high) */
    Prob choice2;                            /* mid vs high */
    Prob low[kNumPosStatesMax][kLenLowSymbols * 2];   /* bit tree (2^3 * 2 - 1, 1-indexed) */
    Prob mid[kNumPosStatesMax][kLenMidSymbols * 2];
    Prob high[kLenHighSymbols * 2];          /* 8-bit bit tree, 1-indexed */

    void reset() noexcept {
        prob_init(&choice,  1);
        prob_init(&choice2, 1);
        for (int s = 0; s < kNumPosStatesMax; ++s) {
            prob_init(low[s],  kLenLowSymbols * 2);
            prob_init(mid[s],  kLenMidSymbols * 2);
        }
        prob_init(high, kLenHighSymbols * 2);
    }
};

/* ── Full probability table ──────────────────────────────────────────────── */

struct LzmaProbTables {
    /* [state][pos_state]: is next symbol a match (1) or literal (0)? */
    Prob is_match[kNumStates][kNumPosStatesMax];

    /* [state]: is this a rep match (1) or new match (0)? */
    Prob is_rep[kNumStates];

    /* [state]: rep0 (0) or rep1/2/3 (1)? */
    Prob is_rep_g0[kNumStates];

    /* [state]: rep1 (0) or rep2/3 (1)? */
    Prob is_rep_g1[kNumStates];

    /* [state]: rep2 (0) or rep3 (1)? */
    Prob is_rep_g2[kNumStates];

    /* [state][pos_state]: short rep (0) or long rep0 (1)? */
    Prob is_rep0_long[kNumStates][kNumPosStatesMax];

    /* Literal probs: [lit_context][0..767]
     * lit_context = (pos_lp << lc) | (prev_byte >> (8-lc))
     * With default lc=3, lp=0: lit_context = prev_byte >> 5 → 8 tables of 768 */
    static constexpr int kLitContextMax = 16 * 9;  /* (1<<lp_max) * (lc_max+1) = 16*9=144 */
    Prob literal[144][768];

    /* Position slots: [len_to_pos_state][6-bit tree (indices 1..64)] */
    Prob dist_slot[kNumLenToPosStates][64 * 2];

    /* Distance special (slots 4-13, reversed bit tree) */
    Prob dist_special[kDistSpecialSize];

    /* Distance align: 4-bit reversed bit tree */
    Prob dist_align[kAlignTableSize * 2];

    /* Match and rep length coders */
    LenProbs match_len;
    LenProbs rep_len;

    void reset(int lc, int lp) noexcept {
        int lit_tables = (1 << (lc + lp));
        for (int i = 0; i < lit_tables; ++i)
            prob_init(literal[i], 768);

        for (int s = 0; s < kNumStates; ++s) {
            for (int ps = 0; ps < kNumPosStatesMax; ++ps) {
                is_match[s][ps]    = kProbInit;
                is_rep0_long[s][ps] = kProbInit;
            }
            is_rep[s]    = kProbInit;
            is_rep_g0[s] = kProbInit;
            is_rep_g1[s] = kProbInit;
            is_rep_g2[s] = kProbInit;
        }

        for (int ls = 0; ls < kNumLenToPosStates; ++ls)
            prob_init(dist_slot[ls], 64 * 2);

        prob_init(dist_special, kDistSpecialSize);
        prob_init(dist_align,   kAlignTableSize * 2);

        match_len.reset();
        rep_len.reset();
    }
};

/* ── Helper: len → pos state (0..3) ─────────────────────────────────────── */

inline int len_to_pos_state(int len) noexcept {
    int v = len - kMatchMinLen;
    return v < kNumLenToPosStates ? v : kNumLenToPosStates - 1;
}

/* ── Distance slot ↔ distance helpers ───────────────────────────────────── */

/* Given 0-based dist d, return its 6-bit slot (0..63) */
inline uint32_t dist_to_slot(uint32_t d) noexcept {
    if (d < 4) return d;
    /* MSB position = floor(log2(d)) */
    int msb = 31 - __builtin_clz(d);
    return (uint32_t)(msb * 2) + ((d >> (msb - 1)) & 1u);
}

/* Offset into dist_special[] for slot s in [4, 13] */
inline int dist_special_offset(int slot) noexcept {
    /* footerBits[slot] = (slot>>1) - 1 → 1,1,2,2,3,3,4,4,5,5 for slot 4..13 */
    static const int kOff[14] = { 0, 0, 0, 0, 0, 2, 4, 8, 12, 20, 28, 44, 60, 92 };
    return kOff[slot];
}

/* ── Length encode / decode helpers (shared by compress and decompress) ──── */

inline void encode_len(RangeEncoder& rc, LenProbs& lp_table,
                       int len, int pos_state) noexcept {
    int v = len - kMatchMinLen;
    if (v < kLenLowSymbols) {
        rc.encode_bit(&lp_table.choice, 0);
        rc.encode_bit_tree(lp_table.low[pos_state], kLenLowBits, (uint32_t)v);
    } else {
        rc.encode_bit(&lp_table.choice, 1);
        v -= kLenLowSymbols;
        if (v < kLenMidSymbols) {
            rc.encode_bit(&lp_table.choice2, 0);
            rc.encode_bit_tree(lp_table.mid[pos_state], kLenMidBits, (uint32_t)v);
        } else {
            rc.encode_bit(&lp_table.choice2, 1);
            v -= kLenMidSymbols;
            rc.encode_bit_tree(lp_table.high, kLenHighBits, (uint32_t)v);
        }
    }
}

inline int decode_len(RangeDecoder& rd, LenProbs& lp_table, int pos_state) noexcept {
    if (rd.decode_bit(&lp_table.choice) == 0) {
        return kMatchMinLen + (int)rd.decode_bit_tree(lp_table.low[pos_state], kLenLowBits);
    }
    if (rd.decode_bit(&lp_table.choice2) == 0) {
        return kMatchMinLen + kLenLowSymbols
             + (int)rd.decode_bit_tree(lp_table.mid[pos_state], kLenMidBits);
    }
    return kMatchMinLen + kLenLowSymbols + kLenMidSymbols
         + (int)rd.decode_bit_tree(lp_table.high, kLenHighBits);
}

/* Encode 0-based distance (actual distance = d + 1) */
inline void encode_dist(RangeEncoder& rc, LzmaProbTables& pt,
                        uint32_t d, int len) noexcept {
    int lps = len_to_pos_state(len);
    uint32_t slot = dist_to_slot(d);
    rc.encode_bit_tree(pt.dist_slot[lps], kNumPosSlotBits, slot);

    if (slot >= (uint32_t)kStartPosModelIndex) {
        int footerBits = (int)(slot >> 1) - 1;
        uint32_t base  = (2u | (slot & 1u)) << footerBits;
        uint32_t extra = d - base;

        if (slot < (uint32_t)kEndPosModelIndex) {
            Prob* sp = pt.dist_special + dist_special_offset((int)slot);
            rc.encode_bit_tree_reverse(sp, footerBits, extra);
        } else {
            /* Direct bits for upper part, align bits for lower 4 */
            rc.encode_direct_bits(extra >> kNumAlignBits, footerBits - kNumAlignBits);
            rc.encode_bit_tree_reverse(pt.dist_align, kNumAlignBits, extra & (kAlignTableSize - 1));
        }
    }
}

/* Decode 0-based distance */
inline uint32_t decode_dist(RangeDecoder& rd, LzmaProbTables& pt, int len) noexcept {
    int lps = len_to_pos_state(len);
    uint32_t slot = rd.decode_bit_tree(pt.dist_slot[lps], kNumPosSlotBits);

    if (slot < (uint32_t)kStartPosModelIndex)
        return slot;

    int footerBits = (int)(slot >> 1) - 1;
    uint32_t base  = (2u | (slot & 1u)) << footerBits;
    uint32_t extra;

    if (slot < (uint32_t)kEndPosModelIndex) {
        Prob* sp = pt.dist_special + dist_special_offset((int)slot);
        extra = rd.decode_bit_tree_reverse(sp, footerBits);
    } else {
        uint32_t direct = rd.decode_direct_bits(footerBits - kNumAlignBits);
        extra = (direct << kNumAlignBits)
              | rd.decode_bit_tree_reverse(pt.dist_align, kNumAlignBits);
    }
    return base + extra;
}

} } /* namespace orot::lzma */
