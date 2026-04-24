#include "lz4_block.hpp"
#include "../simd/simd_dispatch.hpp"

#include <cstring>

namespace orot { namespace lz4 {

/* ── Level config ────────────────────────────────────────────────────────── */

LZ4Config lz4_config_for_level(int level) noexcept {
    if (level <= 0) level = 1;
    if (level > 9)  level = 9;

    switch (level) {
    case 1: return { true,   0,  3,  16 };
    case 2: return { true,   0,  4,  32 };
    case 3: return { true,   0,  5,  64 };
    case 4: return { false,  4,  5,  64 };
    case 5: return { false,  8,  6,  128 };
    case 6: return { false, 16,  6,  128 };
    case 7: return { false, 32,  7,  256 };
    case 8: return { false, 64,  7,  256 };
    default: return { false, 128, 8,  258 };
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline uint32_t lz4_hash(const uint8_t* p) noexcept {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return (v * 0x9E3779B1U) >> (32 - LZ4_HASH_BITS);
}

static inline uint8_t* write_varint_extra(uint8_t* p, int extra) noexcept {
    /* extra is the amount above 15 (for literals) or above 19 (for matches) */
    while (extra >= 255) {
        *p++ = 255;
        extra -= 255;
    }
    *p++ = static_cast<uint8_t>(extra);
    return p;
}

static inline int read_varint_extra(const uint8_t*& p, const uint8_t* end) noexcept {
    int sum = 0;
    while (p < end) {
        uint8_t b = *p++;
        sum += b;
        if (b != 255)
            return sum;
    }
    return -1; /* truncated */
}

/* ── Compress bound ──────────────────────────────────────────────────────── */

int lz4_block_compress_bound(int src_len) noexcept {
    if (src_len <= 0) return 16;
    return src_len + (src_len / 255) + 16;
}

/* ── Compress ────────────────────────────────────────────────────────────── */

/*
 * Emit a sequence: [token][extra-lit][literals][offset-LE16][extra-match]
 * lit_start..lit_end are the literals, match_len is the back-reference length,
 * match_offset is the distance (1-65535). match_len==0 means last sequence.
 */
static uint8_t* emit_sequence(
    uint8_t*       dst,
    const uint8_t* dst_end,
    const uint8_t* lit_start,
    const uint8_t* lit_end,
    int            match_len,
    int            match_offset) noexcept
{
    int lit_len = static_cast<int>(lit_end - lit_start);

    /* Token byte */
    int lit_nibble   = (lit_len   >= 15) ? 15 : lit_len;
    int match_nibble = (match_len == 0)  ? 0
                     : ((match_len - LZ4_MIN_MATCH) >= 15) ? 15
                     : (match_len - LZ4_MIN_MATCH);
    if (dst >= dst_end) return nullptr;
    *dst++ = static_cast<uint8_t>((lit_nibble << 4) | match_nibble);

    /* Extra literal length */
    if (lit_nibble == 15) {
        dst = write_varint_extra(dst, lit_len - 15);
        if (dst >= dst_end) return nullptr;
    }

    /* Literal bytes */
    if (lit_len > 0) {
        if (dst + lit_len > dst_end) return nullptr;
        __builtin_memcpy(dst, lit_start, static_cast<size_t>(lit_len));
        dst += lit_len;
    }

    /* No offset/match for last sequence */
    if (match_len == 0) return dst;

    /* Offset (little-endian 16-bit) */
    if (dst + 2 > dst_end) return nullptr;
    dst[0] = static_cast<uint8_t>(match_offset & 0xFF);
    dst[1] = static_cast<uint8_t>(match_offset >> 8);
    dst += 2;

    /* Extra match length */
    if (match_nibble == 15) {
        dst = write_varint_extra(dst, match_len - LZ4_MIN_MATCH - 15);
        if (dst >= dst_end) return nullptr;
    }

    return dst;
}

int lz4_block_compress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap,
    LZ4State& state, const LZ4Config& cfg) noexcept
{
    if (src_len <= 0) {
        /* Empty input: single token byte with no literals, no match */
        if (dst_cap < 1) return -1;
        dst[0] = 0;
        return 1;
    }

    using namespace orot::deflate;
    auto match_fn = simd_match_length_fn();

    if (cfg.fast_path)
        state.reset_fast();
    else
        state.reset();

    const uint8_t* dst_end = dst + dst_cap;
    uint8_t*       out     = dst;

    const uint8_t* anchor  = src;          /* start of current literal run */
    const uint8_t* ip      = src;          /* current input position */
    const uint8_t* ip_end  = src + src_len;
    /* Last LZ4_LAST_LIT bytes must be literals */
    const uint8_t* match_limit = ip_end - LZ4_LAST_LIT;

    /* Must have at least LZ4_MIN_MATCH bytes to start matching */
    if (src_len < LZ4_MIN_MATCH + LZ4_LAST_LIT) {
        /* Too small: emit as all literals */
        out = emit_sequence(out, dst_end, src, ip_end, 0, 0);
        return (out == nullptr) ? -1 : static_cast<int>(out - dst);
    }

    /* Seed hash table with first position */
    {
        uint32_t h = lz4_hash(ip);
        state.head[h] = static_cast<uint32_t>(ip - src);
    }

    ip++; /* advance past first position */

    /* Adaptive-step counter for fast path: starts at 64 so step=1 initially,
     * increases by 1 every 64 consecutive misses to skip incompressible runs. */
    uint32_t step_ctr = 64;

    while (ip < match_limit) {
        uint32_t h = lz4_hash(ip);
        uint32_t candidate_pos = state.head[h];

        /* Update head */
        uint32_t ip_abs = static_cast<uint32_t>(ip - src);
        if (!cfg.fast_path)
            state.prev[ip_abs & LZ4_WIN_MASK] = candidate_pos;
        state.head[h] = ip_abs;

        /* Try to find a match */
        int best_len    = 0;
        int best_offset = 0;

        auto try_match = [&](uint32_t cpos) -> bool {
            if (cpos == 0 && ip != src) return false; /* sentinel */
            int offset = static_cast<int>(ip_abs - cpos);
            if (offset <= 0 || offset > 65535) return false;
            const uint8_t* ref = src + cpos;

            /* Verify first 4 bytes */
            uint32_t a, b;
            __builtin_memcpy(&a, ip,  4);
            __builtin_memcpy(&b, ref, 4);
            if (a != b) return false;

            /* Measure full match length */
            int max_len = static_cast<int>(ip_end - ip);
            int ml = 4 + match_fn(ip + 4, ref + 4, max_len - 4);

            if (ml > best_len) {
                best_len    = ml;
                best_offset = offset;
                if (ml >= cfg.nice_len) return true; /* good enough */
            }
            return false;
        };

        bool done = false;

        if (cfg.fast_path) {
            try_match(candidate_pos);
        } else {
            /* Chain traversal */
            int steps       = 0;
            int consec_miss = 0;
            uint32_t cpos   = candidate_pos;

            while (cpos != 0 && steps < cfg.max_chain) {
                done = try_match(cpos);
                if (done) break;

                /* Count 4-byte misses for early exit */
                uint32_t a, b;
                __builtin_memcpy(&a, ip,          4);
                __builtin_memcpy(&b, src + cpos,  4);
                if (a != b) {
                    if (++consec_miss >= cfg.miss_limit) break;
                }

                int offset_to_prev = static_cast<int>(ip_abs - cpos);
                if (offset_to_prev > 65535) break;

                cpos = state.prev[cpos & LZ4_WIN_MASK];
                ++steps;
            }
        }

        if (best_len < LZ4_MIN_MATCH) {
            if (cfg.fast_path) {
                /* Adaptive skip: step grows ~1 per 64 consecutive misses.
                 * Avoids O(n) hashing on incompressible runs. */
                ip += step_ctr++ >> 6;
            } else {
                ++ip;
            }
            continue;
        }

        /* Emit sequence */
        out = emit_sequence(out, dst_end, anchor, ip, best_len, best_offset);
        if (out == nullptr) return -1;

        /* Advance past matched region, update hash for covered positions */
        ip += best_len;
        anchor = ip;

        if (cfg.fast_path) {
            step_ctr = 64; /* reset skip counter after each match */
        } else {
            /* Insert intermediate positions into hash, capped to avoid O(match_len)
             * cost on highly compressible data with very long matches. */
            const uint8_t* fill     = ip - best_len + 1;
            const uint8_t* fill_cap = fill + 8;
            while (fill < ip && fill < fill_cap && fill < match_limit) {
                uint32_t fh   = lz4_hash(fill);
                uint32_t fabs = static_cast<uint32_t>(fill - src);
                state.prev[fabs & LZ4_WIN_MASK] = state.head[fh];
                state.head[fh] = fabs;
                ++fill;
            }
        }

        if (ip < match_limit) {
            h = lz4_hash(ip);
            uint32_t new_abs = static_cast<uint32_t>(ip - src);
            if (!cfg.fast_path)
                state.prev[new_abs & LZ4_WIN_MASK] = state.head[h];
            state.head[h] = new_abs;
        }
    }

    /* Emit final sequence (remaining literals, no match) */
    out = emit_sequence(out, dst_end, anchor, ip_end, 0, 0);
    return (out == nullptr) ? -1 : static_cast<int>(out - dst);
}

/* ── Decompress ──────────────────────────────────────────────────────────── */

int lz4_block_decompress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap) noexcept
{
    const uint8_t* ip     = src;
    const uint8_t* ip_end = src + src_len;
    uint8_t*       op     = dst;
    uint8_t*       op_end = dst + dst_cap;

    while (ip < ip_end) {
        /* Token */
        uint8_t token = *ip++;
        int lit_len   = (token >> 4) & 0x0F;
        int match_len = (token     ) & 0x0F;

        /* Extra literal length */
        if (lit_len == 15) {
            int extra = read_varint_extra(ip, ip_end);
            if (extra < 0) return -1;
            lit_len += extra;
        }

        /* Copy literals */
        if (op + lit_len > op_end) return -2;
        if (ip + lit_len > ip_end) return -1;
        __builtin_memcpy(op, ip, static_cast<size_t>(lit_len));
        op += lit_len;
        ip += lit_len;

        /* Last sequence has no match */
        if (ip >= ip_end) break;

        /* Offset */
        if (ip + 2 > ip_end) return -1;
        uint16_t offset;
        __builtin_memcpy(&offset, ip, 2);
        ip += 2;
        if (offset == 0) return -1;

        /* Extra match length */
        if (match_len == 15) {
            int extra = read_varint_extra(ip, ip_end);
            if (extra < 0) return -1;
            match_len += extra;
        }
        match_len += LZ4_MIN_MATCH;

        /* Copy match */
        const uint8_t* ref = op - offset;
        if (ref < dst) return -1;
        if (op + match_len > op_end) return -2;

        if (offset >= match_len) {
            /* Non-overlapping: bulk copy */
            __builtin_memcpy(op, ref, static_cast<size_t>(match_len));
        } else {
            /* Overlapping: byte-by-byte or doubling */
            int done = 0;
            /* Copy 'offset' bytes at a time using doubling */
            int chunk = offset;
            while (done + chunk <= match_len) {
                __builtin_memcpy(op + done, ref, static_cast<size_t>(chunk));
                done += chunk;
                chunk += chunk;
            }
            if (done < match_len)
                __builtin_memmove(op + done, ref, static_cast<size_t>(match_len - done));
        }
        op += match_len;
    }

    return static_cast<int>(op - dst);
}

} } /* namespace orot::lz4 */
