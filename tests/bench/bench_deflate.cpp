/*
 * bench_deflate.cpp — OROT-only Deflate benchmark.
 *
 *   - Compression throughput (MB/s)
 *   - Decompression throughput (MB/s)
 *   - Compression ratio (%)
 *
 * Usage: ./bench_deflate [iterations]
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "orot/deflate.h"

using Clock = std::chrono::steady_clock;

struct BenchResult {
    double comp_mbs   = 0;
    double decomp_mbs = 0;
    double ratio_pct  = 0;
    bool   ok         = false;
};

static BenchResult run(
    const uint8_t* src, size_t slen,
    int level, deflate_format fmt, int iters)
{
    BenchResult r;

    const size_t bound = deflate_compress_bound(slen, fmt);
    std::vector<uint8_t> comp(bound);
    std::vector<uint8_t> decomp(slen + 64);

    const size_t clen = deflate_compress(src, slen, comp.data(), bound, level, fmt);
    if (clen == 0) return r;

    size_t actual = 0;
    deflate_result dr = deflate_decompress(comp.data(), clen, decomp.data(), decomp.size(), &actual, fmt);
    if (dr != DEFLATE_OK || actual != slen || std::memcmp(src, decomp.data(), slen) != 0) {
        std::fprintf(stderr, "  [WARN] round-trip mismatch!\n");
        return r;
    }

    r.ratio_pct = 100.0 * static_cast<double>(clen) / static_cast<double>(slen);

    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        deflate_compress(src, slen, comp.data(), bound, level, fmt);
    auto t1 = Clock::now();

    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        actual = 0;
        deflate_decompress(comp.data(), clen, decomp.data(), decomp.size(), &actual, fmt);
    }
    auto t3 = Clock::now();

    const double mb      = static_cast<double>(slen * static_cast<size_t>(iters)) / 1e6;
    r.comp_mbs   = mb / std::chrono::duration<double>(t1 - t0).count();
    r.decomp_mbs = mb / std::chrono::duration<double>(t3 - t2).count();
    r.ok = true;
    return r;
}

static void print_result(const char* label, int level, const char* fmt_name, const BenchResult& r) {
    if (!r.ok) {
        std::printf("  %-16s  lvl=%2d  fmt=%-4s  FAILED\n", label, level, fmt_name);
        return;
    }
    std::printf("  %-16s  lvl=%2d  fmt=%-4s  comp=%7.1f MB/s  decomp=%7.1f MB/s  ratio=%5.1f%%\n",
        label, level, fmt_name, r.comp_mbs, r.decomp_mbs, r.ratio_pct);
}

static std::vector<uint8_t> make_text() {
    std::vector<uint8_t> v;
    const char* pat = "The quick brown fox jumps over the lazy dog. ";
    const size_t pl = std::strlen(pat);
    for (int i = 0; i < 2000; ++i)
        v.insert(v.end(), reinterpret_cast<const uint8_t*>(pat),
                           reinterpret_cast<const uint8_t*>(pat) + pl);
    return v;
}
static std::vector<uint8_t> make_zeros(size_t n) { return std::vector<uint8_t>(n, 0); }
static std::vector<uint8_t> make_random(size_t n) {
    std::vector<uint8_t> v(n);
    uint32_t s = 0xABCDEF01u;
    for (auto& b : v) { s ^= s<<13; s ^= s>>17; s ^= s<<5; b = (uint8_t)s; }
    return v;
}
static std::vector<uint8_t> make_code(size_t n) {
    std::vector<uint8_t> v(n);
    const char* pat = "int foo(int x) { return x * 2 + 1; }\n";
    const size_t pl = std::strlen(pat);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)pat[i % pl];
    return v;
}

int main(int argc, char** argv) {
    const int iters = (argc > 1) ? std::atoi(argv[1]) : 100;

    std::printf("Deflate benchmark  iters=%d\n\n", iters);

    struct { const char* label; std::vector<uint8_t> data; } datasets[] = {
        { "text (~90KB)",  make_text()           },
        { "zeros (1MB)",   make_zeros(1 << 20)   },
        { "random (1MB)",  make_random(1 << 20)  },
        { "code (~512KB)", make_code(512 * 1024) },
    };

    static const int    levels[]    = { 1, 3, 6, 9 };
    static const char*  fmt_names[] = { "raw", "zlib", "gzip" };
    static const deflate_format fmts[] = {
        DEFLATE_FORMAT_RAW, DEFLATE_FORMAT_ZLIB, DEFLATE_FORMAT_GZIP
    };

    for (auto& ds : datasets) {
        std::printf("[%s]\n", ds.label);
        for (int level : levels) {
            for (int fi = 0; fi < 3; ++fi) {
                auto r = run(ds.data.data(), ds.data.size(), level, fmts[fi], iters);
                print_result("orot-deflate", level, fmt_names[fi], r);
            }
        }
        std::putchar('\n');
    }

    return 0;
}
