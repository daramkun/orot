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
    /* fast_path=true: head-only lookup (no chain), smaller hash table */
    /* hash_bits: 12 (L1=4KB), 14 (L2-3=32KB), 16 (L4+=128KB head[]) */
    static const LZ77Config configs[13] = {
        /* 0  store  */ {0,    0,   0, false, false, 16},
        /* 1  fast   */ {1,   32,  0, false, true,  12},
        /* 2         */ {16,  64,  0, false, true,  14},
        /* 3         */ {32,  128, 0, false, true,  14},
        /* 4  def    */ {64,  128, 1, false, false, 16},
        /* 5         */ {128, 128, 1, false, false, 16},
        /* 6         */ {128, 258, 1, false, false, 16},
        /* 7  better */ {256, 258, 2, false, false, 16},
        /* 8         */ {512, 258, 2, false, false, 16},
        /* 9         */ {768, 258, 3, false, false, 16},
        /* 10 best   */ {1024,258, 4, true,  false, 16},
        /* 11        */ {2048,258, 4, true,  false, 16},
        /* 12        */ {4096,258, 4, true,  false, 16},
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
 * Used for L4+ (chain traversal with software prefetch).
 */
static int match_find(
    const uint8_t* src, int pos, int src_len,
    const LZ77State& state,
    const LZ77Config& cfg,
    MatchLengthFn match_len_fn,
    int& best_dist)
{
    const int max_match = std::min(LZ77_MAX_MATCH, src_len - pos);
    if (max_match < LZ77_MIN_MATCH) return 0;
    /* lz77_hash4 reads 4 bytes; need at least 4 bytes remaining */
    if (src_len - pos < 4) return 0;

    int best_len  = LZ77_MIN_MATCH - 1;
    best_dist     = 0;

    const uint32_t h   = lz77_hash4(src + pos) & LZ77_HASH_MASK;
    uint16_t cur       = state.head[h];
    int      steps     = cfg.max_chain;
    /* Early-exit threshold: if no min-match found in first quarter of chain,
     * the input is likely high-entropy and further traversal is wasteful. */
    const int early_exit_steps = steps - std::max(4, steps >> 2);

    while (steps-- > 0 && cur != 0) {
        /* High-entropy early exit: no match in first quarter of chain → give up. */
        if (__builtin_expect(steps == early_exit_steps && best_len < LZ77_MIN_MATCH, 0))
            break;

        const int dist = (pos - static_cast<int>(cur)) & LZ77_WIN_MASK;
        if (dist == 0 || dist > LZ77_WIN_SIZE) break;

        /* Quick first-byte check before full compare */
        if (src[cur] != src[pos]) {
            cur = state.prev[cur & LZ77_WIN_MASK];
            continue;
        }

        /* Adaptive-width early reject: widen the comparison as best_len grows.
         * Checks bytes 0..N at the candidate start before calling the SIMD
         * match function.  Unaligned loads are safe on x86/ARM. */
        {
            const uint8_t* cand = src + pos - dist;
            const uint8_t* ref  = src + pos;
            if (best_len >= 8 && pos + 7 < src_len) {
                uint64_t cv, rv;
                __builtin_memcpy(&cv, cand, 8);
                __builtin_memcpy(&rv, ref,  8);
                if (cv != rv) {
                    cur = state.prev[cur & LZ77_WIN_MASK];
                    continue;
                }
            } else if (best_len >= 4 && pos + 3 < src_len) {
                uint32_t cv, rv;
                __builtin_memcpy(&cv, cand, 4);
                __builtin_memcpy(&rv, ref,  4);
                if (cv != rv) {
                    cur = state.prev[cur & LZ77_WIN_MASK];
                    continue;
                }
            }
        }

        /* Quick-reject at best_len offset: if the byte at that position doesn't
         * match, this candidate can't improve the current best — skip the SIMD
         * call.  Saves ~80-90% of match_len_fn() invocations during traversal.
         * Use pos - dist (true candidate start) rather than cur (uint16_t, may
         * have wrapped for large inputs). Guard: pos + best_len must be in range. */
        if (best_len >= LZ77_MIN_MATCH && pos + best_len < src_len &&
            src[pos - dist + best_len] != src[pos + best_len])
        {
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
 * match_find_fast: single head[] lookup for L1-L3 fast path.
 * No chain traversal, no prev[] access. Also updates head[] for current pos.
 * Returns match length (< MIN_MATCH = no match).
 */
static inline int match_find_fast(
    const uint8_t* src, int pos, int src_len,
    LZ77State& state,
    const LZ77Config& cfg,
    MatchLengthFn match_len_fn,
    int& best_dist)
{
    if (src_len - pos < 4) return 0;
    const int max_match = std::min(LZ77_MAX_MATCH, src_len - pos);

    const uint32_t h    = lz77_hash4_n(src + pos, cfg.hash_bits);
    const uint16_t cur  = state.head[h];
    /* Update head immediately; fast path skips prev[] entirely */
    state.head[h] = static_cast<uint16_t>(pos);

    if (cur == 0) return 0;

    const int dist = (pos - static_cast<int>(cur)) & LZ77_WIN_MASK;
    if (dist == 0 || dist > LZ77_WIN_SIZE) return 0;

    /* Quick first-byte check */
    if (src[cur] != src[pos]) return 0;

    const int len = match_len_fn(src + pos, src + pos - dist, max_match);
    if (len < LZ77_MIN_MATCH) return 0;

    best_dist = dist;
    return len;
}

/*
 * Insert position `pos` into hash chain (L4+ full path with prev[]).
 * Also clears bt_right so BT4 traversal sees a clean node.
 */
static inline void hash_insert(
    const uint8_t* src, int pos, int src_len,
    LZ77State& state)
{
    if (pos + 4 > src_len) return;  /* lz77_hash4 needs 4 bytes */
    const uint32_t h = lz77_hash4(src + pos) & LZ77_HASH_MASK;
    state.prev[pos & LZ77_WIN_MASK] = state.head[h];
    state.bt_right[pos & LZ77_WIN_MASK] = 0;
    state.head[h] = static_cast<uint16_t>(pos);
}

/*
 * match_find_bt4: binary-tree match finder for L10-12.
 *
 * Each call BOTH finds the best match AND inserts `pos` into the tree.
 * Uses prev[] as left children and bt_right[] as right children.
 * Skips already-known common prefix (len_left / len_right) at each step,
 * reducing redundant byte comparisons vs. chain search.
 *
 * Positions covered by a match use hash_insert (degrades to chain for
 * those slots, which is acceptable since they won't be searched from).
 */
static int match_find_bt4(
    const uint8_t* src, int pos, int src_len,
    LZ77State& state,
    const LZ77Config& cfg,
    MatchLengthFn match_len_fn,
    int& best_dist)
{
    const int max_match = std::min(LZ77_MAX_MATCH, src_len - pos);
    if (max_match < LZ77_MIN_MATCH || src_len - pos < 4) {
        /* Too close to end: just update head with no tree */
        if (pos + 4 <= src_len) {
            const uint32_t h = lz77_hash4(src + pos) & LZ77_HASH_MASK;
            state.prev[pos & LZ77_WIN_MASK]     = state.head[h];
            state.bt_right[pos & LZ77_WIN_MASK] = 0;
            state.head[h] = static_cast<uint16_t>(pos);
        }
        return 0;
    }

    int best_len  = LZ77_MIN_MATCH - 1;
    best_dist     = 0;

    const uint32_t h = lz77_hash4(src + pos) & LZ77_HASH_MASK;
    uint16_t cur     = state.head[h];

    /* Insert pos as new root immediately */
    state.head[h] = static_cast<uint16_t>(pos);

    /* Pointers to the left/right child slots of the new node (pos) */
    uint16_t* pleft  = &state.prev[pos & LZ77_WIN_MASK];
    uint16_t* pright = &state.bt_right[pos & LZ77_WIN_MASK];

    /* Known match lengths from the tree walk:
     * len_left  = how many bytes already match when descending into left subtree
     * len_right = how many bytes already match when descending into right subtree */
    int len_left  = 0;
    int len_right = 0;

    int steps = cfg.max_chain;
    /* Early exit: give up if no MIN_MATCH found after first half of chain */
    const int early_exit_threshold = steps - std::max(4, steps >> 1);

    while (steps-- > 0 && cur != 0) {
        /* High-entropy early exit for BT4: first 50% of budget, no match → give up */
        if (__builtin_expect(steps == early_exit_threshold && best_len < LZ77_MIN_MATCH, 0))
            break;

        const int dist = (pos - static_cast<int>(cur)) & LZ77_WIN_MASK;
        if (dist == 0 || dist > LZ77_WIN_SIZE) break;

        const uint8_t* cand = src + pos - dist;

        /* Start from already-known prefix minimum */
        const int skip = std::min(len_left, len_right);
        const int avail = std::min(max_match, src_len - (pos - dist));

        int len = skip;
        if (skip < avail)
            len = skip + match_len_fn(src + pos + skip, cand + skip, avail - skip);

        if (len > best_len) {
            best_len  = len;
            best_dist = dist;
            if (len >= cfg.nice_len) {
                /* Accept: link cur's children as new node's children */
                *pleft  = state.prev[cur & LZ77_WIN_MASK];
                *pright = state.bt_right[cur & LZ77_WIN_MASK];
                return best_len;
            }
        }

        /* Navigate: compare byte at position `len` to decide left vs right */
        if (pos + len < src_len && (pos - dist + len) < src_len
            && cand[len] < src[pos + len])
        {
            /* cand sorts before pos → cur goes into left subtree of pos */
            *pleft   = cur;
            pleft    = &state.bt_right[cur & LZ77_WIN_MASK];
            cur      = state.bt_right[cur & LZ77_WIN_MASK];
            len_left = len;
        } else {
            /* cur sorts after pos → cur goes into right subtree of pos */
            *pright   = cur;
            pright    = &state.prev[cur & LZ77_WIN_MASK];
            cur       = state.prev[cur & LZ77_WIN_MASK];
            len_right = len;
        }
    }

    /* Terminate both subtrees */
    *pleft  = 0;
    *pright = 0;
    return best_len;
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

    /* Hoist SIMD function pointer — avoids repeated indirect dispatch calls. */
    const MatchLengthFn match_len_fn = simd_match_length_fn();

    if (cfg.fast_path) {
        /* ── Fast path (L1-L3): head-only lookup, no prev[], no lazy matching ── */
        while (pos < isrc_len) {
            if (pos + LZ77_MIN_MATCH > isrc_len) {
                /* Too few bytes for hash — emit as literal (no head update needed) */
                out[n_tokens++] = Token::literal(src[pos++]);
                continue;
            }

            int best_dist = 0;
            /* match_find_fast also updates head[h] for pos */
            int best_len = match_find_fast(src, pos, isrc_len, state, cfg, match_len_fn, best_dist);

            if (best_len < LZ77_MIN_MATCH) {
                out[n_tokens++] = Token::literal(src[pos++]);
            } else {
                out[n_tokens++] = Token::match(
                    static_cast<uint16_t>(best_len),
                    static_cast<uint16_t>(best_dist));
                /* Insert pos+1 into hash before skipping (improves ratio at near-zero cost). */
                if (pos + 1 + 4 <= isrc_len)
                    state.head[lz77_hash4_n(src + pos + 1, cfg.hash_bits)] =
                        static_cast<uint16_t>(pos + 1);
                pos += best_len;
            }
        }
    } else if (cfg.bt4) {
        /* ── BT4 path (L10-12): binary-tree match finder, no lazy matching ──
         * BT4 finds near-optimal matches without lazy; lazy matching would
         * require careful double-insertion tracking, so it is omitted here. */
        while (pos < isrc_len) {
            if (pos + LZ77_MIN_MATCH > isrc_len) {
                out[n_tokens++] = Token::literal(src[pos++]);
                continue;
            }

            int best_dist = 0;
            /* match_find_bt4 also inserts pos into the tree */
            int best_len = match_find_bt4(src, pos, isrc_len, state, cfg, match_len_fn, best_dist);

            if (best_len < LZ77_MIN_MATCH) {
                out[n_tokens++] = Token::literal(src[pos]);
                ++pos;
            } else {
                out[n_tokens++] = Token::match(
                    static_cast<uint16_t>(best_len),
                    static_cast<uint16_t>(best_dist));
                /* Insert covered positions (pos already inserted by match_find_bt4) */
                const int end = pos + best_len;
                for (int i = pos + 1; i < end && i + LZ77_MIN_MATCH <= isrc_len; ++i)
                    hash_insert(src, i, isrc_len, state);
                pos = end;
            }
        }
    } else {
        /* ── Standard path (L4-9): full chain traversal with lazy matching ── */
        /* Lazy checks use half the chain depth to halve their traversal cost. */
        const int lazy_chain = std::max(1, cfg.max_chain >> 1);
        LZ77Config lazy_cfg  = cfg;
        lazy_cfg.max_chain   = lazy_chain;

        while (pos < isrc_len) {
            /* Need at least MIN_MATCH bytes for a hash key */
            if (pos + LZ77_MIN_MATCH > isrc_len) {
                out[n_tokens++] = Token::literal(src[pos++]);
                continue;
            }

            int best_dist = 0;
            int best_len  = match_find(src, pos, isrc_len, state, cfg, match_len_fn, best_dist);

            if (best_len < LZ77_MIN_MATCH) {
                /* No match: emit literal */
                hash_insert(src, pos, isrc_len, state);
                out[n_tokens++] = Token::literal(src[pos]);
                ++pos;
            } else {
                /* Lazy matching: peek ahead to see if next pos has a better match.
                 * Use half max_chain for the lazy check — halves traversal cost. */
                int lazy_steps = cfg.lazy_depth;
                while (lazy_steps > 0
                       && pos + best_len + 1 < isrc_len
                       && pos + 1 + LZ77_MIN_MATCH <= isrc_len)
                {
                    int next_dist = 0;
                    const int next_len = match_find(
                        src, pos + 1, isrc_len, state, lazy_cfg, match_len_fn, next_dist);

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
    }

    return n_tokens;
}

} /* namespace deflate */
