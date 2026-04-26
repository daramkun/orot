/*
 * bench_zstd.cpp - OROT-only Zstandard benchmark.
 */
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#include "orot/zstd.h"

using Clock = std::chrono::steady_clock;

struct BenchResult {
    double comp_mbs = 0;
    double decomp_mbs = 0;
    double ratio_pct = 0;
    bool ok = false;
};

static BenchResult run(const uint8_t* src, size_t slen, int level, int iters) {
    BenchResult r;
    const int bound = orot_zstd_compress_bound(static_cast<int>(slen));
    if (bound <= 0) return r;

    std::vector<uint8_t> comp(static_cast<size_t>(bound));
    std::vector<uint8_t> decomp(slen + 64);

    const int clen = orot_zstd_compress(
        src, static_cast<int>(slen), comp.data(), bound, level);
    if (clen <= 0) return r;

    const int dlen = orot_zstd_decompress(
        comp.data(), clen, decomp.data(), static_cast<int>(decomp.size()));
    if (dlen != static_cast<int>(slen) || std::memcmp(src, decomp.data(), slen) != 0) {
        std::fprintf(stderr, "  [WARN] round-trip mismatch\n");
        return r;
    }

    r.ratio_pct = 100.0 * static_cast<double>(clen) / static_cast<double>(slen);

    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        orot_zstd_compress(src, static_cast<int>(slen), comp.data(), bound, level);
    auto t1 = Clock::now();

    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i)
        orot_zstd_decompress(comp.data(), clen, decomp.data(), static_cast<int>(decomp.size()));
    auto t3 = Clock::now();

    const double mb = static_cast<double>(slen * static_cast<size_t>(iters)) / 1e6;
    r.comp_mbs = mb / std::chrono::duration<double>(t1 - t0).count();
    r.decomp_mbs = mb / std::chrono::duration<double>(t3 - t2).count();
    r.ok = true;
    return r;
}

static void print_row(const char* label, int level, const BenchResult& r) {
    if (!r.ok) {
        std::printf("  %-16s lvl=%2d FAILED\n", label, level);
        return;
    }
    std::printf("  %-16s lvl=%2d comp=%7.1f MB/s decomp=%7.1f MB/s ratio=%5.1f%%\n",
        label, level, r.comp_mbs, r.decomp_mbs, r.ratio_pct);
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
    uint32_t s = 0xDEADBEEFu;
    for (auto& b : v) {
        s = s * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(s >> 24);
    }
    return v;
}

static std::vector<uint8_t> make_code(size_t n) {
    std::vector<uint8_t> v(n);
    const char* pat = "int foo(int x) { return x * 2 + 1; }\n";
    const size_t pl = std::strlen(pat);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(pat[i % pl]);
    return v;
}

int main(int argc, char** argv) {
    const int iters = (argc > 1) ? std::atoi(argv[1]) : 100;
    std::printf("Zstandard benchmark iters=%d\n\n", iters);

    struct Dataset { const char* label; std::vector<uint8_t> data; };
    Dataset datasets[] = {
        { "text (~90KB)", make_text() },
        { "zeros (1MB)", make_zeros(1 << 20) },
        { "random (1MB)", make_random(1 << 20) },
        { "code (~512KB)", make_code(512 * 1024) },
    };

    const int levels[] = { 1, 6, 9 };
    for (const auto& ds : datasets) {
        std::printf("[%s]\n", ds.label);
        for (int level : levels)
            print_row("orot-zstd", level, run(ds.data.data(), ds.data.size(), level, iters));
        std::putchar('\n');
    }

    return 0;
}
