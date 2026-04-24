/*
 * bench_compare.cpp — Cross-library compression benchmark.
 *
 * Compares ours / zlib / libdeflate across:
 *   - Compression throughput (MB/s)
 *   - Decompression throughput (MB/s)
 *   - Compression ratio (%)
 *   - CPU time (ms per iteration)
 *   - RSS memory delta (KB)
 *
 * Build: cmake -B build -DDEFLATE_COMPARE_BENCH=ON && cmake --build build
 * Run:   ./build/tests/bench_compare [iterations]
 */

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <time.h>

/* ── Library headers ─────────────────────────────────────────────────── */
#include "orot/deflate.hpp"   /* ours */
#include <zlib.h>              /* zlib */
#include <libdeflate.h>        /* libdeflate */

using Clock = std::chrono::steady_clock;

/* ── Helper: current process RSS (KB) ───────────────────────────────── */
static long rss_kb() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;   /* already KB on Linux */
#endif
}

/* ── Helper: CPU time (seconds) ─────────────────────────────────────── */
static double cpu_now() {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ══════════════════════════════════════════════════════════════════════
 * Library wrappers — common signature:
 *   compress:   (src, src_len, dst, dst_cap, level) → compressed_len (0=error)
 *   decompress: (src, src_len, dst, dst_cap, original_len) → decompressed_len
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Ours ─────────────────────────────────────────────────────────────── */
static size_t ours_compress(const uint8_t* src, size_t slen,
                             uint8_t* dst, size_t dcap,
                             int level, deflate_format fmt)
{
    return deflate_compress(src, slen, dst, dcap, level, fmt);
}

static size_t ours_decompress(const uint8_t* src, size_t slen,
                               uint8_t* dst, size_t dcap,
                               deflate_format /*fmt*/)
{
    size_t actual = 0;
    deflate_result r = deflate_decompress(src, slen, dst, dcap, &actual,
                                          DEFLATE_FORMAT_ZLIB);
    return (r == DEFLATE_OK) ? actual : 0;
}

/* ── zlib ─────────────────────────────────────────────────────────────── */
static size_t zlib_comp(const uint8_t* src, size_t slen,
                         uint8_t* dst, size_t dcap,
                         int level, deflate_format /*fmt*/)
{
    uLongf dl = static_cast<uLongf>(dcap);
    return (compress2(dst, &dl, src, static_cast<uLong>(slen), level) == Z_OK)
           ? static_cast<size_t>(dl) : 0;
}

static size_t zlib_decomp(const uint8_t* src, size_t slen,
                            uint8_t* dst, size_t dcap,
                            deflate_format /*fmt*/)
{
    uLongf dl = static_cast<uLongf>(dcap);
    return (uncompress(dst, &dl, src, static_cast<uLong>(slen)) == Z_OK)
           ? static_cast<size_t>(dl) : 0;
}

/* ── libdeflate ───────────────────────────────────────────────────────── */
static size_t ldf_comp(const uint8_t* src, size_t slen,
                        uint8_t* dst, size_t dcap,
                        int level, deflate_format /*fmt*/)
{
    struct libdeflate_compressor* c = libdeflate_alloc_compressor(level);
    if (!c) return 0;
    /* Use zlib format for cross-library interoperability */
    size_t out = libdeflate_zlib_compress(c, src, slen, dst, dcap);
    libdeflate_free_compressor(c);
    return out;
}

static size_t ldf_decomp(const uint8_t* src, size_t slen,
                          uint8_t* dst, size_t dcap,
                          deflate_format /*fmt*/)
{
    struct libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    if (!d) return 0;
    size_t actual = 0;
    enum libdeflate_result r =
        libdeflate_zlib_decompress(d, src, slen, dst, dcap, &actual);
    libdeflate_free_decompressor(d);
    return (r == LIBDEFLATE_SUCCESS) ? actual : 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * Benchmark runner
 * ══════════════════════════════════════════════════════════════════════ */

struct BenchResult {
    double comp_mbs   = 0;
    double decomp_mbs = 0;
    double ratio_pct  = 0;
    double cpu_ms     = 0;
    long   rss_delta  = 0;  /* KB */
    bool   ok         = false;
};

template<typename CompFn, typename DecompFn>
static BenchResult run_bench(
    const uint8_t* src, size_t slen,
    int level, deflate_format fmt,
    CompFn comp_fn, DecompFn decomp_fn,
    int iters)
{
    BenchResult r;

    /* Estimate bound: 1.5× original + 64 KB should cover any library */
    const size_t bound = slen + slen / 2 + 65536;
    std::vector<uint8_t> comp_buf(bound);
    std::vector<uint8_t> decomp_buf(slen + 64);

    /* Warm-up */
    size_t clen = comp_fn(src, slen, comp_buf.data(), bound, level, fmt);
    if (clen == 0) return r;
    size_t dlen = decomp_fn(comp_buf.data(), clen, decomp_buf.data(), decomp_buf.size(), fmt);
    if (dlen != slen || std::memcmp(src, decomp_buf.data(), slen) != 0) {
        std::fprintf(stderr, "  [WARN] round-trip mismatch! clen=%zu dlen=%zu expected=%zu\n",
                     clen, dlen, slen);
        return r;
    }

    r.ratio_pct = 100.0 * clen / slen;

    /* ── Compression throughput ───────────────── */
    long rss_before = rss_kb();
    double cpu_before = cpu_now();
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        comp_fn(src, slen, comp_buf.data(), bound, level, fmt);
    auto t1 = Clock::now();
    double cpu_after = cpu_now();
    long rss_after = rss_kb();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    r.comp_mbs   = static_cast<double>(slen) * iters / secs / (1024.0 * 1024.0);
    r.cpu_ms     = (cpu_after - cpu_before) * 1000.0 / iters;
    r.rss_delta  = rss_after - rss_before;

    /* ── Decompression throughput ─────────────── */
    t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        decomp_fn(comp_buf.data(), clen, decomp_buf.data(), decomp_buf.size(), fmt);
    t1 = Clock::now();

    secs = std::chrono::duration<double>(t1 - t0).count();
    r.decomp_mbs = static_cast<double>(slen) * iters / secs / (1024.0 * 1024.0);
    r.ok = true;
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════════ */

int main(int argc, char* argv[]) {
    int iters = 50;
    if (argc > 1) iters = std::atoi(argv[1]);
    if (iters < 1) iters = 1;

    /* ── Build datasets ─────────────────────────────────────────────── */
    std::string text;
    for (int i = 0; i < 2000; ++i)
        text += "The quick brown fox jumps over the lazy dog. ";
    const auto* tp = reinterpret_cast<const uint8_t*>(text.data());
    const size_t tl = text.size();

    std::vector<uint8_t> zeros(1 << 20, 0);

    std::vector<uint8_t> rnd(1 << 20);
    { uint32_t s = 0xABCD1234;
      for (auto& b : rnd) { s ^= s<<13; s ^= s>>17; s ^= s<<5; b = (uint8_t)s; } }

    std::vector<uint8_t> src_code(512 * 1024);
    { /* simulate source code: mostly printable ASCII, structured */
      const char* pat = "int foo(int x) { return x * 2 + 1; }\n";
      size_t pl = std::strlen(pat);
      for (size_t i = 0; i < src_code.size(); ++i)
          src_code[i] = (uint8_t)pat[i % pl]; }

    struct DS { const uint8_t* p; size_t len; const char* name; };
    DS datasets[] = {
        { tp,              tl,              "text (~90KB)"  },
        { zeros.data(),    zeros.size(),    "zeros (1MB)"   },
        { rnd.data(),      rnd.size(),      "random (1MB)"  },
        { src_code.data(), src_code.size(), "code (~512KB)" },
    };

    static const int levels[] = { 1, 6, 9 };
    static const char* lnames[] = { "fast", "default", "best" };

    /* ── Print header ───────────────────────────────────────────────── */
    std::printf("%-16s %-8s %-9s  %10s  %11s  %7s  %8s  %7s\n",
        "Library", "Dataset", "Level",
        "Comp MB/s", "Decomp MB/s", "Ratio%", "CPU ms", "RSS dKB");
    std::printf("%s\n", std::string(87, '-').c_str());

    auto print_row = [](const char* lib, const char* ds, const char* lvl,
                        const BenchResult& r) {
        if (!r.ok) {
            std::printf("%-16s %-8s %-9s  %10s  %11s  %7s  %8s  %7s\n",
                lib, ds, lvl, "FAIL", "-", "-", "-", "-");
            return;
        }
        std::printf("%-16s %-8s %-9s  %10.1f  %11.1f  %6.1f%%  %8.3f  %7ld\n",
            lib, ds, lvl,
            r.comp_mbs, r.decomp_mbs, r.ratio_pct, r.cpu_ms, r.rss_delta);
    };

    for (const auto& ds : datasets) {
        for (int li = 0; li < 3; ++li) {
            int lv = levels[li];
            const char* ln = lnames[li];

            /* ours (ZLIB format, using zlib wrapper for fair comparison) */
            auto r_ours = run_bench(ds.p, ds.len, lv, DEFLATE_FORMAT_ZLIB,
                ours_compress, ours_decompress, iters);
            print_row("ours", ds.name, ln, r_ours);

            /* zlib */
            auto r_zlib = run_bench(ds.p, ds.len, lv, DEFLATE_FORMAT_ZLIB,
                zlib_comp, zlib_decomp, iters);
            print_row("zlib", ds.name, ln, r_zlib);

            /* libdeflate */
            auto r_ldf = run_bench(ds.p, ds.len, lv, DEFLATE_FORMAT_ZLIB,
                ldf_comp, ldf_decomp, iters);
            print_row("libdeflate", ds.name, ln, r_ldf);

            std::printf("\n");
        }
    }

    return 0;
}
