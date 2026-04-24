/*
 * bench_lzw.cpp — OROT-only LZW benchmark.
 *
 *   - Compression throughput (MB/s)
 *   - Decompression throughput (MB/s)
 *   - Compression ratio (%)
 *
 * Usage: ./bench_lzw [iterations]
 */
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <numeric>

#include "orot/lzw.h"

using Clock = std::chrono::steady_clock;

struct BenchResult {
    double comp_mbs   = 0;
    double decomp_mbs = 0;
    double ratio_pct  = 0;
    bool   ok         = false;
};

static BenchResult run_bench(
    const uint8_t* src, size_t slen,
    int max_bits, int iters)
{
    BenchResult r;

    const int bound = orot_lzw_compress_bound(static_cast<int>(slen)) + 64;
    std::vector<uint8_t> comp(static_cast<size_t>(bound));
    std::vector<uint8_t> decomp(slen + 64);

    const int clen = orot_lzw_compress(src, static_cast<int>(slen),
                                       comp.data(), bound, max_bits);
    if (clen <= 0) return r;

    const int dlen = orot_lzw_decompress(comp.data(), clen,
                                         decomp.data(), static_cast<int>(decomp.size()));
    if (dlen != static_cast<int>(slen) || std::memcmp(src, decomp.data(), slen) != 0) {
        std::fprintf(stderr, "  [WARN] round-trip mismatch!\n");
        return r;
    }

    /* Compress timing */
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        orot_lzw_compress(src, static_cast<int>(slen), comp.data(), bound, max_bits);
    auto t1 = Clock::now();

    /* Decompress timing */
    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i)
        orot_lzw_decompress(comp.data(), clen, decomp.data(), static_cast<int>(decomp.size()));
    auto t3 = Clock::now();

    const double secs_comp   = std::chrono::duration<double>(t1 - t0).count();
    const double secs_decomp = std::chrono::duration<double>(t3 - t2).count();
    const double mb          = static_cast<double>(slen * static_cast<size_t>(iters)) / 1e6;

    r.comp_mbs   = mb / secs_comp;
    r.decomp_mbs = mb / secs_decomp;
    r.ratio_pct  = static_cast<double>(clen) * 100.0 / static_cast<double>(slen);
    r.ok         = true;
    return r;
}

static void print_result(const char* label, int max_bits, const BenchResult& r) {
    if (!r.ok) {
        std::printf("  %-38s bits=%2d  FAILED\n", label, max_bits);
        return;
    }
    std::printf("  %-38s bits=%2d  comp=%7.1f MB/s  decomp=%7.1f MB/s  ratio=%5.1f%%\n",
        label, max_bits, r.comp_mbs, r.decomp_mbs, r.ratio_pct);
}

static std::vector<uint8_t> make_zeros(size_t n) {
    return std::vector<uint8_t>(n, 0);
}
static std::vector<uint8_t> make_pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i % 7);
    return v;
}
static std::vector<uint8_t> make_sequential(size_t n) {
    std::vector<uint8_t> v(n);
    std::iota(v.begin(), v.end(), 0);
    return v;
}
static std::vector<uint8_t> make_random(size_t n) {
    std::vector<uint8_t> v(n);
    uint32_t rng = 0xDEADBEEFu;
    for (auto& b : v) {
        rng = rng * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(rng >> 24);
    }
    return v;
}

int main(int argc, char** argv) {
    const int iters = (argc > 1) ? std::atoi(argv[1]) : 20;
    const size_t N  = 1 << 20; /* 1 MiB */

    std::printf("LZW benchmark  iters=%d  buf=%zu KiB\n\n", iters, N / 1024);

    struct { const char* label; std::vector<uint8_t> data; } datasets[] = {
        { "zeros",      make_zeros(N)      },
        { "pattern-7",  make_pattern(N)    },
        { "sequential", make_sequential(N) },
        { "random",     make_random(N)     },
    };

    for (auto& ds : datasets) {
        std::printf("[%s]\n", ds.label);
        for (int mb : {9, 12, 16}) {
            auto r = run_bench(ds.data.data(), ds.data.size(), mb, iters);
            print_result("orot-lzw", mb, r);
        }
        std::putchar('\n');
    }

    return 0;
}
