#include "fse.hpp"

#include <cstring>

namespace orot { namespace zstd {

namespace {

/* Bit reader for ncount parsing (forward, bit-by-bit within stream) */
struct NcountBitReader {
    const uint8_t* src;
    const uint8_t* end;
    uint32_t       bits;
    int            bits_avail;

    NcountBitReader(const uint8_t* s, int len) noexcept
        : src(s), end(s + len), bits(0), bits_avail(0) {}

    void refill() noexcept {
        while (bits_avail <= 24 && src < end) {
            bits |= static_cast<uint32_t>(*src++) << bits_avail;
            bits_avail += 8;
        }
    }

    /* Read n bits LSB-first. Caller must call refill() before reading. */
    uint32_t read(int n) noexcept {
        uint32_t v = bits & ((1u << n) - 1u);
        bits >>= n;
        bits_avail -= n;
        return v;
    }

    bool ok() const noexcept { return bits_avail > 0 || src < end; }

    int bytes_consumed(const uint8_t* orig) const noexcept {
        /* bits_avail are bits already pulled from src but not yet consumed */
        return static_cast<int>(src - orig) - (bits_avail / 8);
    }
};

} // anonymous namespace

int fse_read_ncount(
    const uint8_t* src, int src_len,
    int max_symbol_value,
    int* accuracy_log_out,
    int16_t* norm_count) noexcept
{
    if (!src || src_len <= 0 || !accuracy_log_out || !norm_count) return -1;

    NcountBitReader br(src, src_len);
    br.refill();

    /* 4-bit accuracy_log adjustment: actual_log = 5 + bits[3:0] */
    if (br.bits_avail < 4) return -1;
    const int accuracy_log = 5 + static_cast<int>(br.read(4));
    *accuracy_log_out = accuracy_log;

    if (accuracy_log > FSE_MAX_LOG) return -1;

    const int table_size = 1 << accuracy_log;
    int remaining = table_size;  /* remaining probability mass */

    std::memset(norm_count, 0, sizeof(int16_t) * (max_symbol_value + 1));

    int sym = 0;
    while (remaining > 1 && sym <= max_symbol_value) {
        br.refill();

        /* Compute how many bits to read for this symbol */
        int bit_count = fse_highbit32(static_cast<uint32_t>(remaining + 1)) + 1;
        /* lower threshold: values below this can use one fewer bit */
        uint32_t const threshold = static_cast<uint32_t>((1 << bit_count) - 1 - remaining);

        if (br.bits_avail < bit_count) return -1;

        uint32_t val = br.read(bit_count);

        /* Check if the top bit is a "signal" bit that should be put back */
        if ((val & (1u << (bit_count - 1))) == 0) {
            /* top bit is 0: value fits in bit_count-1 bits if val < threshold */
            uint32_t const low_val = val & ((1u << (bit_count - 1)) - 1u);
            if (low_val < threshold) {
                /* put back the top bit we over-read */
                br.bits = (val >> (bit_count - 1)) | (br.bits << 1);
                br.bits_avail += 1;
                val = low_val;
            }
        }

        if (val == 0) {
            /* Probability 0; 2-bit run-length of additional zero symbols */
            norm_count[sym++] = 0;
            if (br.bits_avail < 2) { br.refill(); }
            uint32_t const repeat = br.read(2);
            for (uint32_t k = 0; k < repeat && sym <= max_symbol_value; ++k)
                norm_count[sym++] = 0;
        } else if (val == 1) {
            /* Low-probability: prob = -1 */
            norm_count[sym++] = -1;
            remaining -= 1;
        } else {
            int prob = static_cast<int>(val) - 1;
            norm_count[sym++] = static_cast<int16_t>(prob);
            remaining -= prob;
        }
    }

    if (remaining != 1) return -1;  /* probabilities don't sum to table_size */

    /* Bytes consumed = ceil(bits_consumed / 8).
       bits_consumed = (bytes loaded) * 8 - (bits still in buffer). */
    int bits_consumed = static_cast<int>(br.src - src) * 8 - br.bits_avail;
    return (bits_consumed + 7) / 8;
}

int fse_build_dtable(
    FseDTable& dt,
    const int16_t* norm_count,
    int max_symbol_value,
    int accuracy_log) noexcept
{
    if (accuracy_log <= 0 || accuracy_log > FSE_MAX_LOG) return -1;
    if (!norm_count) return -1;

    const int table_size = 1 << accuracy_log;
    const uint32_t table_mask = static_cast<uint32_t>(table_size - 1);
    const uint32_t step = (static_cast<uint32_t>(table_size) >> 1) +
                          (static_cast<uint32_t>(table_size) >> 3) + 3u;

    dt.accuracy_log = accuracy_log;

    /* Temporary spread buffer */
    uint8_t spread[1 << FSE_MAX_LOG];

    /* Place low-probability symbols (-1) at high positions */
    uint32_t high_threshold = static_cast<uint32_t>(table_size - 1);
    for (int s = 0; s <= max_symbol_value; ++s) {
        if (norm_count[s] == -1) {
            spread[high_threshold--] = static_cast<uint8_t>(s);
        }
    }

    /* Spread remaining symbols */
    uint32_t position = 0;
    for (int s = 0; s <= max_symbol_value; ++s) {
        int prob = norm_count[s];
        if (prob <= 0) continue;
        for (int i = 0; i < prob; ++i) {
            spread[position] = static_cast<uint8_t>(s);
            do {
                position = (position + step) & table_mask;
            } while (position > high_threshold);
        }
    }

    /* Build decode table entries.
       symbolNext[s] tracks the running "nexts" counter per symbol.
       Initialise to effective probability (1 for -1 prob). */
    uint16_t symbol_next[FSE_MAX_SYMS];
    for (int s = 0; s <= max_symbol_value; ++s) {
        int prob = norm_count[s];
        symbol_next[s] = static_cast<uint16_t>(prob == -1 ? 1 : prob);
    }

    for (int u = 0; u < table_size; ++u) {
        uint8_t s = spread[u];
        uint16_t nexts = symbol_next[s]++;
        int nb = accuracy_log - fse_highbit32(static_cast<uint32_t>(nexts));
        dt.entries[u].symbol   = s;
        dt.entries[u].nb_bits  = static_cast<uint8_t>(nb);
        dt.entries[u].baseline = static_cast<uint16_t>((static_cast<uint32_t>(nexts) << nb) -
                                                        static_cast<uint32_t>(table_size));
    }

    return 0;
}

/* ── Predefined tables (RFC 8878 Appendix B) ─────────────────────────────── */

void fse_init_default_ll(FseDTable& dt) noexcept {
    static const int16_t norm[36] = {
         4,  3,  2,  2,  2,  2,  2,  2,
         2,  2,  2,  2,  2,  1,  1,  1,
         2,  2,  2,  2,  2,  2,  2,  2,
         2,  3,  2,  1,  1,  1,  1,  1,
        -1, -1, -1, -1
    };
    fse_build_dtable(dt, norm, 35, 6);
}

void fse_init_default_ml(FseDTable& dt) noexcept {
    static const int16_t norm[53] = {
         1,  4,  3,  2,  2,  2,  2,  2,
         2,  1,  1,  1,  1,  1,  1,  1,
         1,  1,  1,  1,  1,  1,  1,  1,
         1,  1,  1,  1,  1,  1,  1,  1,
         1,  1,  1,  1,  1,  1,  1,  1,
         1,  1,  1,  1,  1,  1, -1, -1,
        -1, -1, -1, -1, -1
    };
    fse_build_dtable(dt, norm, 52, 6);
}

void fse_init_default_of(FseDTable& dt) noexcept {
    static const int16_t norm[29] = {
         1,  1,  1,  1,  1,  1,  2,  2,
         2,  1,  1,  1,  1,  1,  1,  1,
         1,  1,  1,  1,  1,  1,  1,  1,
        -1, -1, -1, -1, -1
    };
    fse_build_dtable(dt, norm, 28, 5);
}

} } /* namespace orot::zstd */
