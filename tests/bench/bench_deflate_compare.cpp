/*
 * bench_deflate_compare.cpp — Cross-library Deflate benchmark.
 *
 * Compares orot / zlib / libdeflate:
 *   - Compression throughput (MB/s)
 *   - Decompression throughput (MB/s)
 *   - Compression ratio (%)
 *   - CPU time (ms per iteration)
 *
 * Build: cmake -B build -DOROT_BENCHMARK_COMPARE=ON
 * Run:   ./build/tests/bench_deflate_compare [iterations]
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/resource.h>
#include <time.h>

#include "orot/deflate.h"
#include <zlib.h>
#include <libdeflate.h>

using Clock = std::chrono::steady_clock;

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

/* ── Library wrappers ────────────────────────────────────────────────────── */

static size_t orot_comp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int lv) {
    return deflate_compress(s, sl, d, dc, lv, DEFLATE_FORMAT_ZLIB);
}
static size_t orot_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc) {
    size_t actual = 0;
    deflate_result r = deflate_decompress(s, sl, d, dc, &actual, DEFLATE_FORMAT_ZLIB);
    return (r == DEFLATE_OK) ? actual : 0;
}
static size_t orot_bound(size_t sl) {
    return deflate_compress_bound(sl, DEFLATE_FORMAT_ZLIB);
}

static size_t zlib_comp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int lv) {
    uLongf dl = static_cast<uLongf>(dc);
    return (compress2(d, &dl, s, static_cast<uLong>(sl), lv) == Z_OK)
           ? static_cast<size_t>(dl) : 0;
}
static size_t zlib_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc) {
    uLongf dl = static_cast<uLongf>(dc);
    return (uncompress(d, &dl, s, static_cast<uLong>(sl)) == Z_OK)
           ? static_cast<size_t>(dl) : 0;
}
static size_t zlib_bound(size_t sl) { return compressBound(static_cast<uLong>(sl)); }

static size_t ldf_comp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int lv) {
    auto* c = libdeflate_alloc_compressor(lv);
    if (!c) return 0;
    size_t out = libdeflate_zlib_compress(c, s, sl, d, dc);
    libdeflate_free_compressor(c);
    return out;
}
static size_t ldf_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc) {
    auto* c = libdeflate_alloc_decompressor();
    if (!c) return 0;
    size_t actual = 0;
    auto r = libdeflate_zlib_decompress(c, s, sl, d, dc, &actual);
    libdeflate_free_decompressor(c);
    return (r == LIBDEFLATE_SUCCESS) ? actual : 0;
}
static size_t ldf_bound(size_t sl) { return sl + sl / 2 + 65536; }

/* ── Runner ──────────────────────────────────────────────────────────────── */

template<typename CompFn, typename DecompFn, typename BoundFn>
static BenchResult run(
    const uint8_t* src, size_t slen,
    int level, CompFn comp, DecompFn decomp, BoundFn bound,
    int iters)
{
    BenchResult r;
    const size_t cap = bound(slen);
    std::vector<uint8_t> cbuf(cap);
    std::vector<uint8_t> dbuf(slen + 64);

    const size_t clen = comp(src, slen, cbuf.data(), cap, level);
    if (clen == 0) return r;
    const size_t dlen = decomp(cbuf.data(), clen, dbuf.data(), dbuf.size());
    if (dlen != slen || std::memcmp(src, dbuf.data(), slen) != 0) {
        std::fprintf(stderr, "  [WARN] round-trip mismatch!\n");
        return r;
    }

    r.ratio_pct = 100.0 * static_cast<double>(clen) / static_cast<double>(slen);

    double cpu0 = cpu_now();
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        comp(src, slen, cbuf.data(), cap, level);
    auto t1 = Clock::now();
    double cpu1 = cpu_now();

    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i)
        decomp(cbuf.data(), clen, dbuf.data(), dbuf.size());
    auto t3 = Clock::now();

    const double mb  = static_cast<double>(slen * static_cast<size_t>(iters)) / 1e6;
    r.comp_mbs   = mb / std::chrono::duration<double>(t1 - t0).count();
    r.decomp_mbs = mb / std::chrono::duration<double>(t3 - t2).count();
    r.cpu_ms     = (cpu1 - cpu0) * 1000.0 / iters;
    r.ok = true;
    return r;
}

static void print_row(const char* impl, const BenchResult& r) {
    if (!r.ok) { std::printf("  %-12s  FAILED\n", impl); return; }
    std::printf("  %-12s  comp=%7.1f MB/s  decomp=%7.1f MB/s  ratio=%5.1f%%  cpu=%.2f ms\n",
        impl, r.comp_mbs, r.decomp_mbs, r.ratio_pct, r.cpu_ms);
}

/* ── Datasets ────────────────────────────────────────────────────────────── */

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
    uint32_t s = 0xABCD1234u;
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
    const int iters = (argc > 1) ? std::atoi(argv[1]) : 50;

    std::printf("Deflate comparison benchmark  iters=%d  fmt=zlib\n", iters);
    std::printf("  orot:       orot Deflate\n");
    std::printf("  zlib:       zlib compress2/uncompress\n");
    std::printf("  libdeflate: libdeflate zlib\n\n");

    struct { const char* label; std::vector<uint8_t> data; } datasets[] = {
        { "text (~90KB)",  make_text()           },
        { "zeros (1MB)",   make_zeros(1 << 20)   },
        { "random (1MB)",  make_random(1 << 20)  },
        { "code (~512KB)", make_code(512 * 1024) },
    };

    static const int   levels[] = { 1, 6, 9 };
    static const char* lnames[] = { "fast", "default", "best" };

    for (auto& ds : datasets) {
        for (int li = 0; li < 3; ++li) {
            const int lv = levels[li];
            std::printf("[%s  lvl=%s]\n", ds.label, lnames[li]);

            auto r_orot = run(ds.data.data(), ds.data.size(), lv, orot_comp, orot_decomp, orot_bound, iters);
            auto r_zlib = run(ds.data.data(), ds.data.size(), lv, zlib_comp, zlib_decomp, zlib_bound, iters);
            auto r_ldf  = run(ds.data.data(), ds.data.size(), lv, ldf_comp,  ldf_decomp,  ldf_bound,  iters);

            print_row("orot",       r_orot);
            print_row("zlib",       r_zlib);
            print_row("libdeflate", r_ldf);

            if (r_orot.ok && r_zlib.ok)
                std::printf("  speedup (vs zlib)  comp=%.2fx  decomp=%.2fx\n",
                    r_orot.comp_mbs / r_zlib.comp_mbs,
                    r_orot.decomp_mbs / r_zlib.decomp_mbs);
            if (r_orot.ok && r_zlib.ok && r_ldf.ok) {
                double best_comp = (r_zlib.comp_mbs > r_ldf.comp_mbs)
                    ? r_zlib.comp_mbs : r_ldf.comp_mbs;
                double best_decomp = (r_zlib.decomp_mbs > r_ldf.decomp_mbs)
                    ? r_zlib.decomp_mbs : r_ldf.decomp_mbs;
                std::printf("  speedup (vs best)  comp=%.2fx  decomp=%.2fx\n",
                    r_orot.comp_mbs / best_comp,
                    r_orot.decomp_mbs / best_decomp);
            }

            std::putchar('\n');
        }
    }

    return 0;
}
