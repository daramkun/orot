#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

/* XXH32 — LZ4 frame content checksum (seed=0).
 * Public domain implementation following the XXHash specification. */

namespace orot {

static constexpr uint32_t XXH_PRIME1 = 0x9E3779B1U;
static constexpr uint32_t XXH_PRIME2 = 0x85EBCA77U;
static constexpr uint32_t XXH_PRIME3 = 0xC2B2AE3DU;
static constexpr uint32_t XXH_PRIME4 = 0x27D4EB2FU;
static constexpr uint32_t XXH_PRIME5 = 0x165667B1U;

inline uint32_t xxh32_rotl(uint32_t x, int r) noexcept {
    return (x << r) | (x >> (32 - r));
}

inline uint32_t xxh32_round(uint32_t acc, uint32_t lane) noexcept {
    return xxh32_rotl(acc + lane * XXH_PRIME2, 13) * XXH_PRIME1;
}

inline uint32_t xxh32(const void* data, size_t len, uint32_t seed = 0) noexcept {
    const uint8_t* p   = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;
    uint32_t h32;

    if (len >= 16) {
        uint32_t v1 = seed + XXH_PRIME1 + XXH_PRIME2;
        uint32_t v2 = seed + XXH_PRIME2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - XXH_PRIME1;

        do {
            uint32_t lane;
            __builtin_memcpy(&lane, p,      4); v1 = xxh32_round(v1, lane); p += 4;
            __builtin_memcpy(&lane, p,      4); v2 = xxh32_round(v2, lane); p += 4;
            __builtin_memcpy(&lane, p,      4); v3 = xxh32_round(v3, lane); p += 4;
            __builtin_memcpy(&lane, p,      4); v4 = xxh32_round(v4, lane); p += 4;
        } while (p <= end - 16);

        h32 = xxh32_rotl(v1, 1) + xxh32_rotl(v2, 7)
            + xxh32_rotl(v3, 12) + xxh32_rotl(v4, 18);
    } else {
        h32 = seed + XXH_PRIME5;
    }

    h32 += static_cast<uint32_t>(len);

    while (p <= end - 4) {
        uint32_t lane;
        __builtin_memcpy(&lane, p, 4);
        h32 += lane * XXH_PRIME3;
        h32  = xxh32_rotl(h32, 17) * XXH_PRIME4;
        p   += 4;
    }
    while (p < end) {
        h32 += (*p) * XXH_PRIME5;
        h32  = xxh32_rotl(h32, 11) * XXH_PRIME1;
        ++p;
    }

    h32 ^= h32 >> 15;
    h32 *= XXH_PRIME2;
    h32 ^= h32 >> 13;
    h32 *= XXH_PRIME3;
    h32 ^= h32 >> 16;
    return h32;
}

/* Streaming XXH32 for incremental hashing (LZ4 frame content checksum) */
struct XXH32State {
    uint32_t v1, v2, v3, v4;
    uint32_t total_len;
    uint8_t  buf[16];
    uint32_t buf_used;

    void reset(uint32_t seed = 0) noexcept {
        v1 = seed + XXH_PRIME1 + XXH_PRIME2;
        v2 = seed + XXH_PRIME2;
        v3 = seed;
        v4 = seed - XXH_PRIME1;
        total_len = 0;
        buf_used  = 0;
    }

    void update(const void* data, size_t len) noexcept {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total_len += static_cast<uint32_t>(len);

        if (buf_used + len < 16) {
            __builtin_memcpy(buf + buf_used, p, len);
            buf_used += static_cast<uint32_t>(len);
            return;
        }

        const uint8_t* end = p + len;

        if (buf_used > 0) {
            uint32_t fill = 16 - buf_used;
            __builtin_memcpy(buf + buf_used, p, fill);
            p += fill;
            uint32_t lane;
            __builtin_memcpy(&lane, buf,      4); v1 = xxh32_round(v1, lane);
            __builtin_memcpy(&lane, buf +  4, 4); v2 = xxh32_round(v2, lane);
            __builtin_memcpy(&lane, buf +  8, 4); v3 = xxh32_round(v3, lane);
            __builtin_memcpy(&lane, buf + 12, 4); v4 = xxh32_round(v4, lane);
            buf_used = 0;
        }

        while (p <= end - 16) {
            uint32_t lane;
            __builtin_memcpy(&lane, p,      4); v1 = xxh32_round(v1, lane); p += 4;
            __builtin_memcpy(&lane, p,      4); v2 = xxh32_round(v2, lane); p += 4;
            __builtin_memcpy(&lane, p,      4); v3 = xxh32_round(v3, lane); p += 4;
            __builtin_memcpy(&lane, p,      4); v4 = xxh32_round(v4, lane); p += 4;
        }

        if (p < end) {
            buf_used = static_cast<uint32_t>(end - p);
            __builtin_memcpy(buf, p, buf_used);
        }
    }

    uint32_t digest() const noexcept {
        uint32_t h32;
        if (total_len >= 16) {
            h32 = xxh32_rotl(v1, 1) + xxh32_rotl(v2, 7)
                + xxh32_rotl(v3, 12) + xxh32_rotl(v4, 18);
        } else {
            h32 = v3 + XXH_PRIME5; /* v3 == seed when < 16 bytes processed */
        }
        h32 += total_len;

        const uint8_t* p   = buf;
        const uint8_t* end = buf + buf_used;
        while (p <= end - 4) {
            uint32_t lane;
            __builtin_memcpy(&lane, p, 4);
            h32 += lane * XXH_PRIME3;
            h32  = xxh32_rotl(h32, 17) * XXH_PRIME4;
            p   += 4;
        }
        while (p < end) {
            h32 += (*p) * XXH_PRIME5;
            h32  = xxh32_rotl(h32, 11) * XXH_PRIME1;
            ++p;
        }

        h32 ^= h32 >> 15;
        h32 *= XXH_PRIME2;
        h32 ^= h32 >> 13;
        h32 *= XXH_PRIME3;
        h32 ^= h32 >> 16;
        return h32;
    }
};

} /* namespace orot */
