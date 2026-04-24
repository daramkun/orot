#include "lz77.hpp"
#include "simd/simd_dispatch.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace orot { namespace deflate {

/* =========================================================================
 * Level configuration
 * ========================================================================= */

LZ77Config lz77_config_for_level(int level) {
    /* level: 0=store, 1-3=fast, 4-6=default, 7-9=better, 10-12=best */
    /* fast_path=true: head-only lookup (no chain), smaller hash table */
    /* hash_bits: 12 (L1=4KB), 14 (L2-3=32KB), 16 (L4+=128KB head[]) */
    static const LZ77Config configs[13] = {
        /* 0  store  */ {0,    0,   0, false, false, 16, 8},
        /* 1  fast   */ {1,   32,   0, false, true,  12, 8},
        /* 2         */ {16,  64,   0, false, true,  14, 8},
        /* 3         */ {32,  128,  0, false, true,  14, 8},
        /* 4  def    */ {64,  128,  1, false, false, 16, 5},
        /* 5         */ {128, 128,  1, false, false, 16, 5},
        /* 6         */ {128, 258,  1, false, false, 16, 5},
        /* 7  better */ {256, 258,  2, false, false, 16, 6},
        /* 8         */ {512, 258,  2, false, false, 16, 6},
        /* 9         */ {768, 258,  3, false, false, 16, 6},
        /* 10 best   */ {1024,258,  4, true,  false, 16, 8},
        /* 11        */ {2048,258,  4, true,  false, 16, 8},
        /* 12        */ {4096,258,  4, true,  false, 16, 8},
    };
    if (level < 0)  level = 0;
    if (level > 12) level = 12;
    return configs[level];
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
        const uint32_t pos = static_cast<uint32_t>(i & LZ77_WIN_MASK);
        state.prev[pos] = state.head[h];
        state.head[h]   = static_cast<uint32_t>(i);
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
    uint32_t cur       = state.head[h];
    int      steps     = cfg.max_chain;
    /* Consecutive 4-byte miss counter: if candidates in a row fail the
     * 4-byte check, the chain is high-entropy; abort early.
     * miss_limit is tuned per level: lower for L4-6 (faster for random),
     * higher for L10-12 (BT4 handles separately). */
    int consec_misses  = 0;

    while (steps-- > 0 && cur != 0) {
        const int dist = (pos - static_cast<int>(cur)) & LZ77_WIN_MASK;
        if (dist == 0 || dist > LZ77_WIN_SIZE) break;

        /* 4-byte quick reject: catches high-entropy chains faster than first-byte.
         * Falls back to single-byte check near end of input. */
        {
            const uint8_t* cand = src + pos - dist;
            const uint8_t* ref  = src + pos;
            bool mismatch;
            if (__builtin_expect(pos + 3 < src_len, 1)) {
                uint32_t cv, rv;
                __builtin_memcpy(&cv, cand, 4);
                __builtin_memcpy(&rv, ref,  4);
                mismatch = (cv != rv);
            } else {
                mismatch = (cand[0] != ref[0]);
            }
            if (mismatch) {
                if (__builtin_expect(++consec_misses >= cfg.miss_limit, 0)) break;
                cur = state.prev[cur & LZ77_WIN_MASK];
                continue;
            }
        }
        consec_misses = 0;

        /* Adaptive-width early reject: widen the comparison as best_len grows.
         * 4-byte case already handled above; only 8-byte check is needed here. */
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
    const uint32_t cur  = state.head[h];
    /* Update head immediately; fast path skips prev[] entirely */
    state.head[h] = static_cast<uint32_t>(pos);

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
 * Insert position `pos` into hash chain (L4-9 standard path).
 * Does NOT touch bt_right — that array is only needed for BT4 (L10-12).
 * Skipping the bt_right write saves one store per covered position in the
 * match coverage loop, which matters for long matches at high levels.
 */
static inline void hash_insert_chain(
    const uint8_t* src, int pos, int src_len,
    LZ77State& state)
{
    if (pos + 4 > src_len) return;
    const uint32_t h = lz77_hash4(src + pos) & LZ77_HASH_MASK;
    state.prev[pos & LZ77_WIN_MASK] = state.head[h];
    state.head[h] = static_cast<uint32_t>(pos);
}

/*
 * Insert position `pos` into hash chain (L10-12 BT4 path with prev[]).
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
    state.head[h] = static_cast<uint32_t>(pos);
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
            state.head[h] = static_cast<uint32_t>(pos);
        }
        return 0;
    }

    int best_len  = LZ77_MIN_MATCH - 1;
    best_dist     = 0;

    const uint32_t h = lz77_hash4(src + pos) & LZ77_HASH_MASK;
    uint32_t cur     = state.head[h];

    /* Insert pos as new root immediately */
    state.head[h] = static_cast<uint32_t>(pos);

    /* Pointers to the left/right child slots of the new node (pos) */
    uint32_t* pleft  = &state.prev[pos & LZ77_WIN_MASK];
    uint32_t* pright = &state.bt_right[pos & LZ77_WIN_MASK];

    /* Known match lengths from the tree walk:
     * len_left  = how many bytes already match when descending into left subtree
     * len_right = how many bytes already match when descending into right subtree */
    int len_left  = 0;
    int len_right = 0;

    int steps = cfg.max_chain;
    /* Consecutive first-byte miss counter: BT4 must navigate the tree even on
     * misses (structural correctness), but aborts after miss_limit consecutive
     * misses since the chain is high-entropy. */
    int consec_misses = 0;

    while (steps-- > 0 && cur != 0) {
        const int dist = (pos - static_cast<int>(cur)) & LZ77_WIN_MASK;
        if (dist == 0 || dist > LZ77_WIN_SIZE) break;

        const uint8_t* cand = src + pos - dist;

        /* Fast first-byte check: if mismatch, navigate tree without calling
         * the full match function, and count consecutive misses. */
        if (cand[0] != src[pos]) {
            if (__builtin_expect(++consec_misses >= cfg.miss_limit, 0)) {
                *pleft = 0; *pright = 0;
                return best_len;
            }
            /* Still must navigate tree to maintain BT4 structure */
            if (cand[0] < src[pos])
            {
                *pleft = cur;
                pleft  = &state.bt_right[cur & LZ77_WIN_MASK];
                cur    = state.bt_right[cur & LZ77_WIN_MASK];
                len_left = 0;
            } else {
                *pright = cur;
                pright  = &state.prev[cur & LZ77_WIN_MASK];
                cur     = state.prev[cur & LZ77_WIN_MASK];
                len_right = 0;
            }
            continue;
        }
        consec_misses = 0;

        /* Start from already-known prefix minimum */
        const int skip = std::min(len_left, len_right);
        /* avail == max_match: src_len-(pos-dist) = src_len-pos+dist >= max_match always */
        const int avail = max_match;

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
        if (pos + len < src_len && cand[len] < src[pos + len])
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
                /* Insert all covered positions into head[] so that future searches
                 * can find nearby matches for repeating patterns.  No prev[] update
                 * needed: fast_path is head-only (max_chain=1). */
                const int match_end = pos + best_len;
                for (int i = pos + 1; i < match_end && i + 4 <= isrc_len; ++i)
                    state.head[lz77_hash4_n(src + i, cfg.hash_bits)] =
                        static_cast<uint32_t>(i);
                pos = match_end;
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

                /* Insert all positions covered by the match.
                 * Use hash_insert_chain (no bt_right clear) — bt_right is
                 * only needed for the BT4 path (L10-12). */
                const int end = pos + best_len;
                for (int i = pos; i < end && i + LZ77_MIN_MATCH <= isrc_len; ++i)
                    hash_insert_chain(src, i, isrc_len, state);
                pos = end;
            }
        }
    }

    return n_tokens;
}

} } /* namespace orot::deflate */
