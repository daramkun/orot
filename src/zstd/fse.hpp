#pragma once

#include <cstdint>
#include <cstring>

namespace orot { namespace zstd {

static constexpr int FSE_MAX_LOG  = 13;
static constexpr int FSE_MAX_SYMS = 256;

struct FseEntry {
    uint16_t baseline;
    uint8_t  symbol;
    uint8_t  nb_bits;
};

struct FseDTable {
    int      accuracy_log = 0;
    FseEntry entries[1 << FSE_MAX_LOG];
};

/* Reverse bitstream (end→start, LSB-first, sentinel-bit init) */
struct FseBitStream {
    const uint8_t* src;
    int            src_len;
    uint64_t       bits;
    int            bits_avail;
    bool           valid;

    bool init(const uint8_t* s, int len) noexcept {
        if (len <= 0) { valid = false; return false; }
        src = s;
        uint8_t last = s[len - 1];
        if (last == 0) { valid = false; return false; }
        int msb = 0;
        while ((last >> (msb + 1)) != 0) ++msb;
        bits = last;
        bits_avail = msb;
        src_len = len - 1;
        valid = true;
        return true;
    }

    bool fetch(int n) noexcept {
        while (bits_avail < n && src_len > 0) {
            --src_len;
            bits = (bits << 8) | static_cast<uint64_t>(src[src_len]);
            bits_avail += 8;
        }
        if (bits_avail < n) {
            valid = false;
            return false;
        }
        return true;
    }

    uint32_t read_bits(int n) noexcept {
        if (n == 0) return 0;
        if (!fetch(n)) return 0;
        bits_avail -= n;
        uint32_t v = static_cast<uint32_t>((bits >> bits_avail) & ((1ull << n) - 1u));
        return v;
    }

    bool is_valid() const noexcept { return valid; }
};

/* Return position of highest set bit (0-indexed). x must be > 0. */
static inline int fse_highbit32(uint32_t x) noexcept {
    return 31 - __builtin_clz(x);
}

/* Parse normalized count from src. Returns bytes consumed or -1 on error.
   norm_count[i] = probability of symbol i (-1 = low-prob = 1/table_size).
   max_symbol_value: maximum symbol index to accept. */
int fse_read_ncount(
    const uint8_t* src, int src_len,
    int max_symbol_value,
    int* accuracy_log_out,
    int16_t* norm_count) noexcept;

/* Build decode table from normalized_count. Returns 0 on success, -1 on error. */
int fse_build_dtable(
    FseDTable& dt,
    const int16_t* norm_count,
    int max_symbol_value,
    int accuracy_log) noexcept;

/* Initialize predefined decode tables (RFC 8878 Appendix B). */
void fse_init_default_ll(FseDTable& dt) noexcept;
void fse_init_default_ml(FseDTable& dt) noexcept;
void fse_init_default_of(FseDTable& dt) noexcept;

/* Initialize FSE state from bitstream (reads accuracy_log bits). */
static inline uint32_t fse_init_state(const FseDTable& dt, FseBitStream& bs) noexcept {
    return bs.read_bits(dt.accuracy_log);
}

/* Decode symbol at current state (does NOT advance state). */
static inline uint8_t fse_decode_symbol(const FseDTable& dt, uint32_t state) noexcept {
    return dt.entries[state].symbol;
}

/* Update state: read nb_bits from stream, return new state. */
static inline uint32_t fse_next_state(const FseDTable& dt, uint32_t state,
                                       FseBitStream& bs) noexcept {
    const FseEntry& e = dt.entries[state];
    return e.baseline + bs.read_bits(e.nb_bits);
}

} } /* namespace orot::zstd */
