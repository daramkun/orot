/*
 * bench_bzip2.cpp — OROT Bzip2 benchmark.
 *
 * Usage: ./bench_bzip2 [iterations]
 */
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <numeric>

#include "orot/bzip2.h"

using Clock = std::chrono::steady_clock;

struct BenchResult {
    double comp_mbs   = 0;
    double decomp_mbs = 0;
    double ratio_pct  = 0;
    bool   ok         = false;
};

static BenchResult run_bzip2(const uint8_t* src, size_t slen, int level, int iters) {
    BenchResult r;

    const size_t bound = orot_bzip2_compress_bound(slen);
    std::vector<uint8_t> comp(bound);
    std::vector<uint8_t> decomp(slen + 64);

    int clen = orot_bzip2_compress(src, slen, comp.data(), bound, level);
    if (clen <= 0) return r;

    int dlen = orot_bzip2_decompress(comp.data(), (size_t)clen,
                                     decomp.data(), slen + 64, nullptr);
    if (dlen != (int)slen || std::memcmp(src, decomp.data(), slen) != 0) {
        std::fprintf(stderr, "  [WARN] Bzip2 round-trip mismatch!\n");
        return r;
    }

    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        orot_bzip2_compress(src, slen, comp.data(), bound, level);
    auto t1 = Clock::now();

    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i)
        orot_bzip2_decompress(comp.data(), (size_t)clen,
                              decomp.data(), slen + 64, nullptr);
    auto t3 = Clock::now();

    double secs_c  = std::chrono::duration<double>(t1 - t0).count();
    double secs_d  = std::chrono::duration<double>(t3 - t2).count();
    double mb      = (double)(slen * (size_t)iters) / 1e6;

    r.comp_mbs   = mb / secs_c;
    r.decomp_mbs = mb / secs_d;
    r.ratio_pct  = (double)clen * 100.0 / (double)slen;
    r.ok         = true;
    return r;
}

static void print_result(int level, const BenchResult& r) {
    if (!r.ok) { std::printf("  bzip2  L%d  FAILED\n", level); return; }
    std::printf("  bzip2  L%d  comp=%7.1f MB/s  decomp=%7.1f MB/s  ratio=%5.1f%%\n",
        level, r.comp_mbs, r.decomp_mbs, r.ratio_pct);
}

static std::vector<uint8_t> make_zeros(size_t n)     { return std::vector<uint8_t>(n, 0); }
static std::vector<uint8_t> make_pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(i % 7);
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
    for (auto& b : v) { rng = rng * 1664525u + 1013904223u; b = (uint8_t)(rng >> 24); }
    return v;
}

int main(int argc, char** argv) {
    const int iters = (argc > 1) ? std::atoi(argv[1]) : 3;
    const size_t N  = 1u << 20; /* 1 MiB */

    std::printf("Bzip2 benchmark  iters=%d  buf=%zu KiB\n\n", iters, N / 1024);

    struct { const char* label; std::vector<uint8_t> data; } datasets[] = {
        { "zeros",      make_zeros(N)      },
        { "pattern-7",  make_pattern(N)    },
        { "sequential", make_sequential(N) },
        { "random",     make_random(N)     },
    };

    for (auto& ds : datasets) {
        std::printf("[%s]\n", ds.label);
        for (int lvl : {1, 5, 9})
            print_result(lvl, run_bzip2(ds.data.data(), ds.data.size(), lvl, iters));
        std::putchar('\n');
    }

    return 0;
}
