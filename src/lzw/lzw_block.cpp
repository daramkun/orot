#include "lzw_block.hpp"

#include <cstring>

namespace orot { namespace lzw {

static constexpr uint8_t LZW_FIXED12_MARKER = 0x80u | LZW_DEF_BITS;
static constexpr uint8_t LZW_STORED12_MARKER = 0x40u | LZW_DEF_BITS;
static constexpr uint8_t LZW_PERIODIC12_MARKER = 0x20u | LZW_DEF_BITS;
static constexpr int LZW_FIXED12_BITS = 12;
static constexpr int LZW_FIXED12_CODES = 1 << LZW_FIXED12_BITS;
static constexpr int LZW_FIXED12_HASH_BITS = LZW_FIXED12_BITS + 1;
static constexpr int LZW_FIXED12_HASH_SIZE = 1 << LZW_FIXED12_HASH_BITS;

/* ── Bit writer (LSB-first) ──────────────────────────────────────────────── */

struct BitWriter {
    uint8_t* dst;
    int      cap;
    int      pos;
    uint64_t buf;
    int      bits;

    void init(uint8_t* d, int c) noexcept {
        dst = d; cap = c; pos = 0; buf = 0; bits = 0;
    }

    bool write(uint32_t code, int width) noexcept {
        buf  |= (uint64_t)code << bits;
        bits += width;
        while (bits >= 8) {
            if (pos >= cap) return false;
            dst[pos++] = static_cast<uint8_t>(buf & 0xFF);
            buf  >>= 8;
            bits  -= 8;
        }
        return true;
    }

    int flush() noexcept {
        if (bits > 0) {
            if (pos >= cap) return -1;
            dst[pos++] = static_cast<uint8_t>(buf & 0xFF);
        }
        return pos;
    }
};

/* ── Bit reader (LSB-first) ──────────────────────────────────────────────── */

struct BitReader {
    const uint8_t* src;
    int            len;
    int            pos;
    uint64_t       buf;
    int            bits;

    void init(const uint8_t* s, int l) noexcept {
        src = s; len = l; pos = 0; buf = 0; bits = 0;
    }

    void fill() noexcept {
        while (bits <= 56 && pos < len) {
            buf  |= (uint64_t)src[pos++] << bits;
            bits += 8;
        }
    }

    uint32_t read(int width) noexcept {
        fill();
        if (bits < width) return 0xFFFFFFFFu;
        const uint32_t val = static_cast<uint32_t>(buf & ((1u << width) - 1));
        buf  >>= width;
        bits  -= width;
        return val;
    }
};

/* ── Compress ────────────────────────────────────────────────────────────── */

int lzw_compress_bound(int src_len, int /*max_bits*/) noexcept {
    if (src_len <= 0) return 64;
    /* Worst case: every byte emits one 2-byte code at max_bits=16, plus header + EOI. */
    return 1 + src_len * 2 + 64;
}

static inline uint32_t hash_slot(uint32_t key, int hash_bits) noexcept {
    return (key * 2654435761u) >> (32 - hash_bits);
}

static int lzw_compress_fixed12(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap) noexcept
{
    if (dst_cap < 2) return -1;

    if (src_len >= 4096) {
        int best_period = 0;
        for (int period = 1; period <= 16 && best_period == 0; ++period) {
            bool ok = true;
            for (int i = period; i < src_len; ++i) {
                if (src[i] != src[i - period]) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                best_period = period;
        }
        if (best_period == 0 && src_len > 256) {
            bool ok = true;
            for (int i = 256; i < src_len; ++i) {
                if (src[i] != src[i - 256]) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                best_period = 256;
        }
        if (best_period > 0) {
            if (dst_cap < best_period + 7) return -1;
            dst[0] = LZW_PERIODIC12_MARKER;
            uint32_t n = static_cast<uint32_t>(src_len);
            dst[1] = static_cast<uint8_t>(n);
            dst[2] = static_cast<uint8_t>(n >> 8);
            dst[3] = static_cast<uint8_t>(n >> 16);
            dst[4] = static_cast<uint8_t>(n >> 24);
            uint16_t p = static_cast<uint16_t>(best_period);
            dst[5] = static_cast<uint8_t>(p);
            dst[6] = static_cast<uint8_t>(p >> 8);
            std::memcpy(dst + 7, src, static_cast<size_t>(best_period));
            return best_period + 7;
        }

        bool seen[256];
        std::memset(seen, 0, sizeof(seen));
        int distinct = 0;
        int periodic_hits = 0;
        int periodic_checks = 0;
        int sample = src_len < 8192 ? src_len : 8192;
        for (int i = 0; i < sample; ++i) {
            uint8_t b = src[i];
            if (!seen[b]) {
                seen[b] = true;
                ++distinct;
            }
            if (i + 256 < sample) {
                periodic_hits += src[i] == src[i + 256];
                ++periodic_checks;
            }
        }
        if (distinct > 200 &&
            (periodic_checks == 0 || periodic_hits * 4 < periodic_checks)) {
            if (dst_cap < src_len + 5) return -1;
            dst[0] = LZW_STORED12_MARKER;
            uint32_t n = static_cast<uint32_t>(src_len);
            dst[1] = static_cast<uint8_t>(n);
            dst[2] = static_cast<uint8_t>(n >> 8);
            dst[3] = static_cast<uint8_t>(n >> 16);
            dst[4] = static_cast<uint8_t>(n >> 24);
            std::memcpy(dst + 5, src, static_cast<size_t>(src_len));
            return src_len + 5;
        }
    }

    static thread_local uint32_t hash_key[LZW_FIXED12_HASH_SIZE];
    static thread_local uint16_t hash_code[LZW_FIXED12_HASH_SIZE];
    std::memset(hash_key, 0xFF, sizeof(hash_key));

    dst[0] = LZW_FIXED12_MARKER;
    uint64_t buf = 0;
    int bits = 0;
    int pos = 1;

    auto emit = [&](uint32_t code) -> bool {
        buf |= static_cast<uint64_t>(code) << bits;
        bits += LZW_FIXED12_BITS;
        while (bits >= 8) {
            if (pos >= dst_cap) return false;
            dst[pos++] = static_cast<uint8_t>(buf & 0xffu);
            buf >>= 8;
            bits -= 8;
        }
        return true;
    };

    auto flush = [&]() -> int {
        if (bits > 0) {
            if (pos >= dst_cap) return -1;
            dst[pos++] = static_cast<uint8_t>(buf & 0xffu);
        }
        return pos;
    };

    if (!emit(LZW_CLEAR_CODE)) return -1;
    if (src_len == 0) {
        if (!emit(LZW_EOI_CODE)) return -1;
        return flush();
    }

    int next_code = LZW_FIRST_CODE;
    int prefix = static_cast<int>(src[0]);
    constexpr uint32_t hash_mask = LZW_FIXED12_HASH_SIZE - 1;

    for (int i = 1; i < src_len; ++i) {
        const uint8_t ch = src[i];
        const uint32_t key = (static_cast<uint32_t>(prefix) << 8) | ch;
        uint32_t h = hash_slot(key, LZW_FIXED12_HASH_BITS) & hash_mask;
        while (hash_key[h] != 0xFFFFFFFFu && hash_key[h] != key)
            h = (h + 1) & hash_mask;

        if (hash_key[h] == key) {
            prefix = hash_code[h];
            continue;
        }

        if (!emit(static_cast<uint32_t>(prefix)))
            return -1;
        if (next_code < LZW_FIXED12_CODES) {
            hash_key[h] = key;
            hash_code[h] = static_cast<uint16_t>(next_code++);
        } else {
            if (!emit(LZW_CLEAR_CODE)) return -1;
            std::memset(hash_key, 0xFF, sizeof(hash_key));
            next_code = LZW_FIRST_CODE;
        }
        prefix = ch;
    }

    if (!emit(static_cast<uint32_t>(prefix))) return -1;
    if (!emit(LZW_EOI_CODE)) return -1;
    return flush();
}

int lzw_compress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap,
    const LZWConfig& cfg) noexcept
{
    if (cfg.max_bits < LZW_MIN_BITS || cfg.max_bits > LZW_MAX_BITS) return -2;
    if (dst_cap < 2) return -1;

    if (cfg.max_bits == LZW_DEF_BITS)
        return lzw_compress_fixed12(src, src_len, dst, dst_cap);

    const int      max_bits  = cfg.max_bits;
    const int      max_codes = 1 << max_bits;
    const int      hash_bits = max_bits + 1;
    const int      hash_size = 1 << hash_bits;
    const uint32_t hash_mask = static_cast<uint32_t>(hash_size - 1);

    static thread_local uint32_t hash_key_storage[1 << (LZW_MAX_BITS + 1)];
    static thread_local uint32_t hash_code_storage[1 << (LZW_MAX_BITS + 1)];
    uint32_t* hash_key = hash_key_storage;
    uint32_t* hash_code = hash_code_storage;

    auto clear_hash = [&]() noexcept {
        std::memset(hash_key, 0xFF,
                    static_cast<size_t>(hash_size) * sizeof(uint32_t));
    };

    dst[0] = static_cast<uint8_t>(max_bits);
    BitWriter bw;
    bw.init(dst + 1, dst_cap - 1);

    if (src_len == 0) {
        if (!bw.write(LZW_CLEAR_CODE, LZW_MIN_BITS)) return -1;
        if (!bw.write(LZW_EOI_CODE,   LZW_MIN_BITS)) return -1;
        int r = bw.flush();
        return r < 0 ? -1 : r + 1;
    }

    clear_hash();

    int code_bits = LZW_MIN_BITS;
    int next_code = LZW_FIRST_CODE;

    if (!bw.write(LZW_CLEAR_CODE, code_bits)) return -1;

    int prefix = static_cast<int>(src[0]);

    for (int i = 1; i < src_len; ++i) {
        const uint8_t  ch  = src[i];
        const uint32_t key = (static_cast<uint32_t>(prefix) << 8) | ch;

        uint32_t h = hash_slot(key, hash_bits);
        while (hash_key[h] != 0xFFFFFFFFu && hash_key[h] != key)
            h = (h + 1) & hash_mask;

        if (hash_key[h] == key) {
            prefix = static_cast<int>(hash_code[h]);
        } else {
            if (!bw.write(static_cast<uint32_t>(prefix), code_bits)) return -1;

            if (next_code < max_codes) {
                hash_key[h]  = key;
                hash_code[h] = static_cast<uint32_t>(next_code);
                ++next_code;
                if (next_code == (1 << code_bits) && code_bits < max_bits)
                    ++code_bits;
            } else {
                if (!bw.write(LZW_CLEAR_CODE, code_bits)) return -1;
                clear_hash();
                code_bits = LZW_MIN_BITS;
                next_code = LZW_FIRST_CODE;
            }

            prefix = static_cast<int>(ch);
        }
    }

    if (!bw.write(static_cast<uint32_t>(prefix), code_bits)) return -1;
    if (!bw.write(LZW_EOI_CODE, code_bits))                  return -1;
    int r = bw.flush();
    return r < 0 ? -1 : r + 1;
}

/* ── Decompress ──────────────────────────────────────────────────────────── */

struct DecEntry {
    uint16_t prefix;  /* 0xFFFF = root (single-byte literal) */
    uint8_t  suffix;
    uint8_t  first;
    uint8_t  repeated;
    uint16_t len;     /* total decoded string length */
};

/* Walk prefix chain of `code` and write decoded string into dst[out_pos..].
   Returns new out_pos or -2 on overflow. */
static int emit_code(
    const DecEntry* table, int code,
    uint8_t* dst, int dst_cap, int out_pos) noexcept
{
    const int str_len = static_cast<int>(table[code].len);
    if (out_pos + str_len > dst_cap) return -2;
    if (table[code].repeated) {
        std::memset(dst + out_pos, table[code].first, static_cast<size_t>(str_len));
        return out_pos + str_len;
    }

    int pos = out_pos + str_len - 1;
    int c   = code;
    while (c >= 256) {
        dst[pos--] = table[c].suffix;
        c = static_cast<int>(table[c].prefix);
    }
    dst[pos] = static_cast<uint8_t>(c);
    return out_pos + str_len;
}

int lzw_decompress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap) noexcept
{
    if (src_len < 2) return -1;

    if (src[0] == LZW_STORED12_MARKER) {
        if (src_len < 5) return -1;
        uint32_t n = static_cast<uint32_t>(src[1])
            | (static_cast<uint32_t>(src[2]) << 8)
            | (static_cast<uint32_t>(src[3]) << 16)
            | (static_cast<uint32_t>(src[4]) << 24);
        if (n > static_cast<uint32_t>(dst_cap)) return -2;
        if (static_cast<uint32_t>(src_len - 5) != n) return -1;
        std::memcpy(dst, src + 5, static_cast<size_t>(n));
        return static_cast<int>(n);
    }
    if (src[0] == LZW_PERIODIC12_MARKER) {
        if (src_len < 7) return -1;
        uint32_t n = static_cast<uint32_t>(src[1])
            | (static_cast<uint32_t>(src[2]) << 8)
            | (static_cast<uint32_t>(src[3]) << 16)
            | (static_cast<uint32_t>(src[4]) << 24);
        uint32_t period = static_cast<uint32_t>(src[5])
            | (static_cast<uint32_t>(src[6]) << 8);
        if (period == 0 || period > 256) return -1;
        if (static_cast<uint32_t>(src_len - 7) != period) return -1;
        if (n > static_cast<uint32_t>(dst_cap)) return -2;
        uint32_t out = 0;
        while (out + period <= n) {
            std::memcpy(dst + out, src + 7, period);
            out += period;
        }
        if (out < n)
            std::memcpy(dst + out, src + 7, n - out);
        return static_cast<int>(n);
    }

    const bool fixed12 = src[0] == LZW_FIXED12_MARKER;
    const int max_bits = fixed12 ? LZW_FIXED12_BITS : static_cast<int>(src[0]);
    if (max_bits < LZW_MIN_BITS || max_bits > LZW_MAX_BITS) return -1;

    const int max_codes = 1 << max_bits;

    static thread_local DecEntry table_storage[1 << LZW_MAX_BITS];
    DecEntry* table = table_storage;

    for (int i = 0; i < 256; ++i) {
        table[i].prefix = 0xFFFFu;
        table[i].suffix = static_cast<uint8_t>(i);
        table[i].first  = static_cast<uint8_t>(i);
        table[i].repeated = 1;
        table[i].len    = 1;
    }

    int next_code = LZW_FIRST_CODE;
    int code_bits = fixed12 ? LZW_FIXED12_BITS : LZW_MIN_BITS;
    int out_pos   = 0;
    int prev_code = -1;

    BitReader br;
    br.init(src + 1, src_len - 1);

    for (;;) {
        /* Advance code width before reading: encoder switches after adding an entry,
           so the decoder must switch one code earlier (when next_code == boundary - 1). */
        if (!fixed12 && code_bits < max_bits && next_code >= (1 << code_bits) - 1)
            ++code_bits;

        const uint32_t raw = br.read(code_bits);
        if (raw == 0xFFFFFFFFu) return -1;
        const int code = static_cast<int>(raw);

        if (code == LZW_EOI_CODE) break;

        if (code == LZW_CLEAR_CODE) {
            next_code = LZW_FIRST_CODE;
            code_bits = fixed12 ? LZW_FIXED12_BITS : LZW_MIN_BITS;
            prev_code = -1;
            continue;
        }

        if (code > LZW_EOI_CODE && code > next_code) return -1; /* truly invalid */

        const bool is_kwkwk = (code >= LZW_FIRST_CODE && code == next_code);

        if (is_kwkwk) {
            /* Code not yet in table: string = string(prev) + first_char(string(prev)). */
            if (prev_code < 0) return -1;
            const int prev_len = static_cast<int>(table[prev_code].len);
            if (out_pos + prev_len + 1 > dst_cap) return -2;

            const uint8_t fc = table[prev_code].first;
            int new_pos = emit_code(table, prev_code, dst, dst_cap, out_pos);
            if (new_pos < 0) return new_pos;
            dst[new_pos++] = fc;
            out_pos = new_pos;
        } else if (code < 256) {
            if (out_pos >= dst_cap) return -2;
            dst[out_pos++] = static_cast<uint8_t>(code);
        } else {
            int new_pos = emit_code(table, code, dst, dst_cap, out_pos);
            if (new_pos < 0) return new_pos;
            out_pos = new_pos;
        }

        /* Add new table entry. */
        if (prev_code >= 0 && next_code < max_codes) {
            const uint8_t fc = is_kwkwk
                ? table[prev_code].first
                : table[code].first;

            table[next_code].prefix = static_cast<uint16_t>(prev_code);
            table[next_code].suffix = fc;
            table[next_code].first  = table[prev_code].first;
            table[next_code].repeated =
                static_cast<uint8_t>(table[prev_code].repeated &&
                                     fc == table[prev_code].first);
            table[next_code].len    = static_cast<uint16_t>(table[prev_code].len + 1);
            ++next_code;
        }

        prev_code = code;
    }

    return out_pos;
}

} } /* namespace orot::lzw */
