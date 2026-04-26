/*
 * bench_zstd_compare.cpp - Cross-library Zstandard benchmark.
 */
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <time.h>

#include "orot/zstd.h"
#include <zstd.h>

using Clock = std::chrono::steady_clock;

static double cpu_now() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct BenchResult {
    double comp_mbs = 0;
    double decomp_mbs = 0;
    double ratio_pct = 0;
    double cpu_ms = 0;
    bool ok = false;
};

static int orot_bound(int sl) { return orot_zstd_compress_bound(sl); }
static int orot_comp(const uint8_t* s, int sl, uint8_t* d, int dc, int lv) {
    return orot_zstd_compress(s, sl, d, dc, lv);
}
static int orot_decomp(const uint8_t* s, int sl, uint8_t* d, int dc) {
    return orot_zstd_decompress(s, sl, d, dc);
}

static int libzstd_bound(int sl) {
    return static_cast<int>(ZSTD_compressBound(static_cast<size_t>(sl)));
}
static int libzstd_comp(const uint8_t* s, int sl, uint8_t* d, int dc, int lv) {
    const size_t n = ZSTD_compress(d, static_cast<size_t>(dc),
                                   s, static_cast<size_t>(sl), lv);
    return ZSTD_isError(n) ? -1 : static_cast<int>(n);
}
static int libzstd_decomp(const uint8_t* s, int sl, uint8_t* d, int dc) {
    const size_t n = ZSTD_decompress(d, static_cast<size_t>(dc),
                                     s, static_cast<size_t>(sl));
    return ZSTD_isError(n) ? -1 : static_cast<int>(n);
}

template<typename BoundFn, typename CompFn, typename DecompFn>
static BenchResult run(const uint8_t* src, size_t slen, int level,
                       BoundFn bound_fn, CompFn comp_fn, DecompFn decomp_fn,
                       int iters)
{
    BenchResult r;
    const int bound = bound_fn(static_cast<int>(slen));
    if (bound <= 0) return r;

    std::vector<uint8_t> comp(static_cast<size_t>(bound));
    std::vector<uint8_t> decomp(slen + 64);

    const int clen = comp_fn(src, static_cast<int>(slen), comp.data(), bound, level);
    if (clen <= 0) return r;

    const int dlen = decomp_fn(comp.data(), clen, decomp.data(), static_cast<int>(decomp.size()));
    if (dlen != static_cast<int>(slen) || std::memcmp(src, decomp.data(), slen) != 0) {
        std::fprintf(stderr, "  [WARN] round-trip mismatch\n");
        return r;
    }

    r.ratio_pct = 100.0 * static_cast<double>(clen) / static_cast<double>(slen);

    const double cpu0 = cpu_now();
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        comp_fn(src, static_cast<int>(slen), comp.data(), bound, level);
    auto t1 = Clock::now();
    const double cpu1 = cpu_now();

    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i)
        decomp_fn(comp.data(), clen, decomp.data(), static_cast<int>(decomp.size()));
    auto t3 = Clock::now();

    const double mb = static_cast<double>(slen * static_cast<size_t>(iters)) / 1e6;
    r.comp_mbs = mb / std::chrono::duration<double>(t1 - t0).count();
    r.decomp_mbs = mb / std::chrono::duration<double>(t3 - t2).count();
    r.cpu_ms = (cpu1 - cpu0) * 1000.0 / iters;
    r.ok = true;
    return r;
}

static void print_row(const char* impl, const BenchResult& r) {
    if (!r.ok) {
        std::printf("  %-8s FAILED\n", impl);
        return;
    }
    std::printf("  %-8s comp=%7.1f MB/s decomp=%7.1f MB/s ratio=%5.1f%% cpu=%.2f ms\n",
        impl, r.comp_mbs, r.decomp_mbs, r.ratio_pct, r.cpu_ms);
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
    const int iters = (argc > 1) ? std::atoi(argv[1]) : 50;
    std::printf("Zstandard comparison benchmark iters=%d\n\n", iters);

    struct Dataset { const char* label; std::vector<uint8_t> data; };
    Dataset datasets[] = {
        { "text (~90KB)", make_text() },
        { "zeros (1MB)", make_zeros(1 << 20) },
        { "random (1MB)", make_random(1 << 20) },
        { "code (~512KB)", make_code(512 * 1024) },
    };

    const int levels[] = { 1, 6, 9 };
    for (const auto& ds : datasets) {
        for (int level : levels) {
            std::printf("[%s lvl=%d]\n", ds.label, level);
            const auto ro = run(ds.data.data(), ds.data.size(), level,
                                orot_bound, orot_comp, orot_decomp, iters);
            const auto rz = run(ds.data.data(), ds.data.size(), level,
                                libzstd_bound, libzstd_comp, libzstd_decomp, iters);
            print_row("orot", ro);
            print_row("libzstd", rz);
            if (ro.ok && rz.ok) {
                std::printf("  speedup comp=%.2fx decomp=%.2fx\n",
                    ro.comp_mbs / rz.comp_mbs, ro.decomp_mbs / rz.decomp_mbs);
            }
            std::putchar('\n');
        }
    }

    return 0;
}
