/*
 * bench_lzw_compare.cpp — LZW cross-implementation benchmark.
 *
 * Compares orot LZW against a minimal reference implementation bundled here.
 * No external library dependency — the reference is a naive but correct LZW
 * with fixed 12-bit codes, used purely for baseline comparison.
 *
 *   - Compression throughput (MB/s)
 *   - Decompression throughput (MB/s)
 *   - Compression ratio (%)
 *   - CPU time (ms per iteration)
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <numeric>
#include <time.h>
#include <sys/resource.h>

#include "orot/lzw.h"

using Clock = std::chrono::steady_clock;

/* ══════════════════════════════════════════════════════════════════════════
 * Reference LZW implementation (naive, 12-bit fixed, for baseline only)
 * ══════════════════════════════════════════════════════════════════════════ */

namespace ref_lzw {

static constexpr int MAX_BITS  = 12;
static constexpr int MAX_CODES = 1 << MAX_BITS;   /* 4096 */
static constexpr int CLR       = 256;
static constexpr int EOI       = 257;
static constexpr int FIRST     = 258;
static constexpr int HASH_SIZE = MAX_CODES * 2;   /* 8192, power-of-2 */

/* Compress: produce raw bit stream (no header). */
static int compress(
    const uint8_t* src, int slen,
    uint8_t* dst, int dcap) noexcept
{
    uint32_t ht_key[HASH_SIZE];
    uint16_t ht_val[HASH_SIZE];
    std::memset(ht_key, 0xFF, sizeof(ht_key));

    /* Bit accumulator */
    uint64_t buf  = 0;
    int      bits = 0;
    int      pos  = 0;

    auto emit = [&](uint32_t code) -> bool {
        buf  |= (uint64_t)code << bits;
        bits += MAX_BITS;
        while (bits >= 8) {
            if (pos >= dcap) return false;
            dst[pos++] = (uint8_t)(buf & 0xFF);
            buf >>= 8; bits -= 8;
        }
        return true;
    };
    auto flush_bits = [&]() -> int {
        if (bits > 0) {
            if (pos >= dcap) return -1;
            dst[pos++] = (uint8_t)(buf & 0xFF);
        }
        return pos;
    };

    auto clear_ht = [&]() { std::memset(ht_key, 0xFF, sizeof(ht_key)); };

    if (!emit(CLR)) return -1;
    if (slen == 0)  { if (!emit(EOI)) return -1; return flush_bits(); }

    clear_ht();
    int next = FIRST;
    int pfx  = src[0];

    for (int i = 1; i < slen; ++i) {
        const uint8_t  ch  = src[i];
        const uint32_t key = ((uint32_t)pfx << 8) | ch;
        uint32_t h = (key * 2654435761u) >> (32 - (MAX_BITS + 1));
        h &= (HASH_SIZE - 1);
        while (ht_key[h] != 0xFFFFFFFFu && ht_key[h] != key)
            h = (h + 1) & (HASH_SIZE - 1);

        if (ht_key[h] == key) {
            pfx = ht_val[h];
        } else {
            if (!emit((uint32_t)pfx)) return -1;
            if (next < MAX_CODES) {
                ht_key[h] = key;
                ht_val[h] = (uint16_t)next++;
            } else {
                if (!emit(CLR)) return -1;
                clear_ht();
                next = FIRST;
            }
            pfx = ch;
        }
    }
    if (!emit((uint32_t)pfx)) return -1;
    if (!emit(EOI))            return -1;
    return flush_bits();
}

struct DecEntry { uint16_t pfx; uint8_t sfx; };

static int decompress(
    const uint8_t* src, int slen,
    uint8_t* dst, int dcap) noexcept
{
    DecEntry table[MAX_CODES];
    for (int i = 0; i < 256; ++i) { table[i].pfx = 0xFFFFu; table[i].sfx = (uint8_t)i; }

    uint64_t buf  = 0;
    int      bits = 0;
    int      rpos = 0;
    int      out  = 0;
    int      next = FIRST;
    int      prev = -1;

    auto read_code = [&]() -> int {
        while (bits < MAX_BITS && rpos < slen) {
            buf |= (uint64_t)src[rpos++] << bits;
            bits += 8;
        }
        if (bits < MAX_BITS) return -1;
        int c = (int)(buf & ((1u << MAX_BITS) - 1));
        buf >>= MAX_BITS; bits -= MAX_BITS;
        return c;
    };

    /* Stack-based string emit */
    uint8_t stk[MAX_CODES];
    auto emit_code = [&](int code) -> int {
        int top = 0;
        int c   = code;
        while (c >= 256) { stk[top++] = table[c].sfx; c = table[c].pfx; }
        stk[top++] = (uint8_t)c;
        if (out + top > dcap) return -2;
        for (int k = top - 1; k >= 0; --k)
            dst[out++] = stk[k];
        return 0;
    };
    auto first_char = [&](int code) -> uint8_t {
        while (code >= 256) code = table[code].pfx;
        return (uint8_t)code;
    };

    for (;;) {
        int code = read_code();
        if (code < 0) return -1;
        if (code == EOI) break;
        if (code == CLR) { next = FIRST; prev = -1; continue; }

        bool kwkwk = (code >= FIRST && code == next);
        if (kwkwk) {
            if (prev < 0) return -1;
            uint8_t fc = first_char(prev);
            int r = emit_code(prev); if (r < 0) return r;
            if (out >= dcap) return -2;
            dst[out++] = fc;
        } else {
            if (code >= FIRST && code > next) return -1;
            int r = emit_code(code); if (r < 0) return r;
        }

        if (prev >= 0 && next < MAX_CODES) {
            uint8_t fc = kwkwk ? first_char(prev) : first_char(code);
            table[next].pfx = (uint16_t)prev;
            table[next].sfx = fc;
            ++next;
        }
        prev = code;
    }
    return out;
}

static int compress_bound(int slen) { return slen * 2 + 64; }

} /* namespace ref_lzw */

/* ══════════════════════════════════════════════════════════════════════════
 * Benchmark helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static double cpu_now() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct BenchResult {
    double comp_mbs   = 0;
    double decomp_mbs = 0;
    double ratio_pct  = 0;
    double cpu_ms     = 0;
    bool   ok         = false;
};

template<typename CompFn, typename DecompFn, typename BoundFn>
static BenchResult run(
    const uint8_t* src, size_t slen,
    CompFn comp, DecompFn decomp, BoundFn bound,
    int comp_extra, int iters)
{
    BenchResult r;
    const int cap = bound(static_cast<int>(slen)) + 64;
    std::vector<uint8_t> cbuf(static_cast<size_t>(cap));
    std::vector<uint8_t> dbuf(slen + 64);

    const int clen = comp(src, static_cast<int>(slen), cbuf.data(), cap, comp_extra);
    if (clen <= 0) return r;

    const int dlen = decomp(cbuf.data(), clen, dbuf.data(), static_cast<int>(dbuf.size()));
    if (dlen != static_cast<int>(slen) || std::memcmp(src, dbuf.data(), slen) != 0) {
        std::fprintf(stderr, "  [WARN] round-trip mismatch!\n");
        return r;
    }

    /* Compress */
    double cpu0 = cpu_now();
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        comp(src, static_cast<int>(slen), cbuf.data(), cap, comp_extra);
    auto t1 = Clock::now();
    double cpu1 = cpu_now();

    /* Decompress */
    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i)
        decomp(cbuf.data(), clen, dbuf.data(), static_cast<int>(dbuf.size()));
    auto t3 = Clock::now();

    const double mb          = static_cast<double>(slen * static_cast<size_t>(iters)) / 1e6;
    r.comp_mbs   = mb / std::chrono::duration<double>(t1 - t0).count();
    r.decomp_mbs = mb / std::chrono::duration<double>(t3 - t2).count();
    r.ratio_pct  = static_cast<double>(clen) * 100.0 / static_cast<double>(slen);
    r.cpu_ms     = (cpu1 - cpu0) * 1000.0 / iters;
    r.ok         = true;
    return r;
}

static void print_row(const char* impl, const BenchResult& r) {
    if (!r.ok) { std::printf("  %-16s  FAILED\n", impl); return; }
    std::printf("  %-16s  comp=%7.1f MB/s  decomp=%7.1f MB/s  ratio=%5.1f%%  cpu=%.2f ms\n",
        impl, r.comp_mbs, r.decomp_mbs, r.ratio_pct, r.cpu_ms);
}

/* Adaptor: ref_lzw::compress ignores the last int argument. */
static int ref_compress_adapter(
    const void* s, int sl, void* d, int dc, int /*unused*/)
{
    return ref_lzw::compress(
        static_cast<const uint8_t*>(s), sl,
        static_cast<uint8_t*>(d), dc);
}

static int ref_decompress_adapter(const void* s, int sl, void* d, int dc) {
    return ref_lzw::decompress(
        static_cast<const uint8_t*>(s), sl,
        static_cast<uint8_t*>(d), dc);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Dataset generators
 * ══════════════════════════════════════════════════════════════════════════ */

static std::vector<uint8_t> make_zeros(size_t n) { return std::vector<uint8_t>(n, 0); }
static std::vector<uint8_t> make_pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i % 7);
    return v;
}
static std::vector<uint8_t> make_sequential(size_t n) {
    std::vector<uint8_t> v(n); std::iota(v.begin(), v.end(), 0); return v;
}
static std::vector<uint8_t> make_random(size_t n) {
    std::vector<uint8_t> v(n);
    uint32_t rng = 0xDEADBEEFu;
    for (auto& b : v) { rng = rng * 1664525u + 1013904223u; b = (uint8_t)(rng >> 24); }
    return v;
}

int main(int argc, char** argv) {
    const int    iters = (argc > 1) ? std::atoi(argv[1]) : 20;
    const size_t N     = 1 << 20;

    std::printf("LZW comparison benchmark  iters=%d  buf=%zu KiB\n", iters, N / 1024);
    std::printf("  orot-lzw-12: variable-width codes, max_bits=12\n");
    std::printf("  ref-lzw-12:  naive fixed 12-bit reference (no header overhead)\n\n");

    struct { const char* label; std::vector<uint8_t> data; } datasets[] = {
        { "zeros",      make_zeros(N)      },
        { "pattern-7",  make_pattern(N)    },
        { "sequential", make_sequential(N) },
        { "random",     make_random(N)     },
    };

    for (auto& ds : datasets) {
        std::printf("[%s]\n", ds.label);

        auto orot_r = run(ds.data.data(), ds.data.size(),
            [](const void* s, int sl, void* d, int dc, int mb) {
                return orot_lzw_compress(s, sl, d, dc, mb);
            },
            [](const void* s, int sl, void* d, int dc) {
                return orot_lzw_decompress(s, sl, d, dc);
            },
            [](int sl) { return orot_lzw_compress_bound(sl); },
            12, iters);
        print_row("orot-lzw-12", orot_r);

        auto ref_r = run(ds.data.data(), ds.data.size(),
            ref_compress_adapter,
            ref_decompress_adapter,
            ref_lzw::compress_bound,
            0, iters);
        print_row("ref-lzw-12", ref_r);

        /* Speedup summary */
        if (orot_r.ok && ref_r.ok) {
            std::printf("  speedup  comp=%.2fx  decomp=%.2fx\n",
                orot_r.comp_mbs / ref_r.comp_mbs,
                orot_r.decomp_mbs / ref_r.decomp_mbs);
        }
        std::putchar('\n');
    }

    return 0;
}
