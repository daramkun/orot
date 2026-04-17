#include "lz77.hpp"
#include "simd/simd_dispatch.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace deflate {

/* =========================================================================
 * Level configuration
 * ========================================================================= */

LZ77Config lz77_config_for_level(int level) {
    /* level: 0=store, 1-3=fast, 4-6=default, 7-9=better, 10-12=best */
    static const LZ77Config configs[13] = {
        /* 0  store  */ {0,   0,   0, false},
        /* 1  fast   */ {8,   32,  0, false},
        /* 2         */ {16,  64,  0, false},
        /* 3         */ {32,  128, 0, false},
        /* 4  def    */ {64,  128, 1, false},
        /* 5         */ {128, 128, 1, false},
        /* 6         */ {128, 258, 1, false},
        /* 7  better */ {256, 258, 2, false},
        /* 8         */ {512, 258, 2, false},
        /* 9         */ {768, 258, 3, false},
        /* 10 best   */ {1024,258, 4, true },
        /* 11        */ {2048,258, 4, true },
        /* 12        */ {4096,258, 4, true },
    };
    if (level < 0)  level = 0;
    if (level > 12) level = 12;
    return configs[level];
}

/* =========================================================================
 * Scalar match length
 * ========================================================================= */

int match_length_scalar(
    const uint8_t* a,
    const uint8_t* b,
    int max_len) noexcept
{
    int len = 0;
    /* Process 8 bytes at a time */
    while (len + 8 <= max_len) {
        uint64_t wa, wb;
        std::memcpy(&wa, a + len, 8);
        std::memcpy(&wb, b + len, 8);
        uint64_t diff = wa ^ wb;
        if (diff) {
            /* Find first differing byte via ctz */
#if defined(__GNUC__) || defined(__clang__)
            len += static_cast<int>(__builtin_ctzll(diff) >> 3);
#else
            while (len < max_len && a[len] == b[len]) ++len;
#endif
            return len;
        }
        len += 8;
    }
    while (len < max_len && a[len] == b[len]) ++len;
    return len;
}

/* =========================================================================
 * Dictionary insertion
 * ========================================================================= */

void lz77_insert_dict(
    const uint8_t* dict, size_t dict_len,
    LZ77State& state)
{
    if (dict_len < 4) return;  /* lz77_hash4 needs 4 bytes */
    const size_t limit = dict_len - 3;  /* i + 4 <= dict_len → i < dict_len - 3 */
    for (size_t i = 0; i < limit; ++i) {
        const uint32_t h = lz77_hash4(dict + i) & LZ77_HASH_MASK;
        const uint16_t pos = static_cast<uint16_t>(i & LZ77_WIN_MASK);
        state.prev[pos] = state.head[h];
        state.head[h]   = static_cast<uint16_t>(i);
    }
}

/* =========================================================================
 * Core LZ77 compressor
 * ========================================================================= */

/*
 * match_find: find best match at position `pos` within the window.
 * Returns best match length (< MIN_MATCH means no match found).
 */
static int match_find(
    const uint8_t* src, int pos, int src_len,
    const LZ77State& state,
    const LZ77Config& cfg,
    int& best_dist)
{
    const int max_match = std::min(LZ77_MAX_MATCH, src_len - pos);
    if (max_match < LZ77_MIN_MATCH) return 0;
    /* lz77_hash4 reads 4 bytes; need at least 4 bytes remaining */
    if (src_len - pos < 4) return 0;

    /* Retrieve SIMD-accelerated match length function */
    const auto match_len_fn = simd_match_length_fn();

    int best_len  = LZ77_MIN_MATCH - 1;
    best_dist     = 0;

    const uint32_t h   = lz77_hash4(src + pos) & LZ77_HASH_MASK;
    uint16_t cur       = state.head[h];
    int      steps     = cfg.max_chain;

    while (steps-- > 0 && cur != 0) {
        const int dist = (pos - static_cast<int>(cur)) & LZ77_WIN_MASK;
        if (dist == 0 || dist > LZ77_WIN_SIZE) break;

        /* Quick first-byte check before full compare */
        if (src[cur] != src[pos]) {
            cur = state.prev[cur & LZ77_WIN_MASK];
            continue;
        }

        const int len = match_len_fn(
            src + pos, src + pos - dist, max_match);

        if (len > best_len) {
            best_len  = len;
            best_dist = dist;
            if (len >= cfg.nice_len) break;
        }
        cur = state.prev[cur & LZ77_WIN_MASK];
    }

    return best_len;
}

/*
 * Insert position `pos` into hash chain.
 */
static inline void hash_insert(
    const uint8_t* src, int pos, int src_len,
    LZ77State& state)
{
    if (pos + 4 > src_len) return;  /* lz77_hash4 needs 4 bytes */
    const uint32_t h = lz77_hash4(src + pos) & LZ77_HASH_MASK;
    state.prev[pos & LZ77_WIN_MASK] = state.head[h];
    state.head[h] = static_cast<uint16_t>(pos);
}

size_t lz77_compress(
    const uint8_t* src, size_t src_len,
    Token*         out,
    LZ77State&     state,
    const LZ77Config& cfg,
    const uint8_t* dict,
    size_t         dict_len)
{
    if (dict && dict_len > 0)
        lz77_insert_dict(dict, dict_len, state);

    if (src_len == 0) return 0;
    if (cfg.max_chain == 0) {
        /* Level 0: store all as literals */
        for (size_t i = 0; i < src_len; ++i)
            out[i] = Token::literal(src[i]);
        return src_len;
    }

    const int isrc_len = static_cast<int>(src_len);
    size_t n_tokens = 0;
    int pos = 0;

    while (pos < isrc_len) {
        /* Need at least MIN_MATCH bytes for a hash key */
        if (pos + LZ77_MIN_MATCH > isrc_len) {
            out[n_tokens++] = Token::literal(src[pos++]);
            continue;
        }

        int best_dist = 0;
        int best_len  = match_find(src, pos, isrc_len, state, cfg, best_dist);

        if (best_len < LZ77_MIN_MATCH) {
            /* No match: emit literal */
            hash_insert(src, pos, isrc_len, state);
            out[n_tokens++] = Token::literal(src[pos]);
            ++pos;
        } else {
            /* Lazy matching: peek ahead to see if next pos has a better match */
            int lazy_steps = cfg.lazy_depth;
            while (lazy_steps > 0
                   && pos + best_len + 1 < isrc_len
                   && pos + 1 + LZ77_MIN_MATCH <= isrc_len)
            {
                int next_dist = 0;
                const int next_len = match_find(
                    src, pos + 1, isrc_len, state, cfg, next_dist);

                if (next_len > best_len + 1) {
                    /* Better match one position ahead: emit literal, advance */
                    hash_insert(src, pos, isrc_len, state);
                    out[n_tokens++] = Token::literal(src[pos]);
                    ++pos;
                    best_len  = next_len;
                    best_dist = next_dist;
                    --lazy_steps;
                } else {
                    break;
                }
            }

            /* Emit match token */
            out[n_tokens++] = Token::match(
                static_cast<uint16_t>(best_len),
                static_cast<uint16_t>(best_dist));

            /* Insert all positions covered by the match */
            const int end = pos + best_len;
            for (int i = pos; i < end && i + LZ77_MIN_MATCH <= isrc_len; ++i)
                hash_insert(src, i, isrc_len, state);
            pos = end;
        }
    }

    return n_tokens;
}

} /* namespace deflate */
