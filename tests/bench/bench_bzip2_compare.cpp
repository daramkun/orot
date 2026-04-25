/*
 * bench_bzip2_compare.cpp — Cross-library Bzip2 benchmark.
 *
 * Compares orot and system libbz2 across 4 datasets and 3 compression levels.
 *
 * Build: cmake -B build -DOROT_BENCHMARK_COMPARE=ON
 * Run:   ./build/tests/bench_bzip2_compare [iterations]
 */
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <time.h>

#include "orot/bzip2.h"
#include <bzlib.h>

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

// ── libbz2 wrappers ──────────────────────────────────────────────────────────

static size_t libbz2_bound(size_t src_len) {
    return src_len * 2 + 1024;
}

static int libbz2_compress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    int level)
{
    unsigned int dest_len = static_cast<unsigned int>(dst_cap);
    const int ret = BZ2_bzBuffToBuffCompress(
        reinterpret_cast<char*>(dst), &dest_len,
        const_cast<char*>(reinterpret_cast<const char*>(src)),
        static_cast<unsigned int>(src_len),
        level, 0, 30);
    return (ret == BZ_OK) ? static_cast<int>(dest_len) : -1;
}

static int libbz2_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    int)
{
    unsigned int dest_len = static_cast<unsigned int>(dst_cap);
    const int ret = BZ2_bzBuffToBuffDecompress(
        reinterpret_cast<char*>(dst), &dest_len,
        const_cast<char*>(reinterpret_cast<const char*>(src)),
        static_cast<unsigned int>(src_len),
        0, 0);
    return (ret == BZ_OK) ? static_cast<int>(dest_len) : -1;
}

// ── orot wrappers ─────────────────────────────────────────────────────────────

static size_t orot_bz2_bound(size_t sl) { return orot_bzip2_compress_bound(sl); }

static int orot_bz2_comp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int lv) {
    return orot_bzip2_compress(s, sl, d, dc, lv);
}

static int orot_bz2_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    return orot_bzip2_decompress(s, sl, d, dc, nullptr);
}

// ── Benchmark runner ──────────────────────────────────────────────────────────

template<typename BoundFn, typename CompFn, typename DecompFn>
static BenchResult run(
    const uint8_t* src, size_t slen, int level,
    BoundFn bound_fn, CompFn comp_fn, DecompFn decomp_fn,
    int iters)
{
    BenchResult r;
    const size_t cap = bound_fn(slen);
    if (cap == 0) return r;

    std::vector<uint8_t> comp(cap);
    std::vector<uint8_t> decomp(slen + 64);

    const int clen = comp_fn(src, slen, comp.data(), comp.size(), level);
    if (clen <= 0) return r;

    const int dlen = decomp_fn(comp.data(), static_cast<size_t>(clen),
                               decomp.data(), decomp.size(), level);
    if (dlen != static_cast<int>(slen) || std::memcmp(src, decomp.data(), slen) != 0) {
        std::fprintf(stderr, "  [WARN] round-trip mismatch!\n");
        return r;
    }

    r.ratio_pct = 100.0 * static_cast<double>(clen) / static_cast<double>(slen);

    double cpu0 = cpu_now();
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        comp_fn(src, slen, comp.data(), comp.size(), level);
    auto t1 = Clock::now();
    double cpu1 = cpu_now();

    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i)
        decomp_fn(comp.data(), static_cast<size_t>(clen), decomp.data(), decomp.size(), level);
    auto t3 = Clock::now();

    const double mb = static_cast<double>(slen * static_cast<size_t>(iters)) / 1e6;
    r.comp_mbs   = mb / std::chrono::duration<double>(t1 - t0).count();
    r.decomp_mbs = mb / std::chrono::duration<double>(t3 - t2).count();
    r.cpu_ms     = (cpu1 - cpu0) * 1000.0 / iters;
    r.ok = true;
    return r;
}

static void print_row(const char* impl, const BenchResult& r) {
    if (!r.ok) {
        std::printf("  %-12s  FAILED\n", impl);
        return;
    }
    std::printf("  %-12s  comp=%7.1f MB/s  decomp=%7.1f MB/s  ratio=%5.1f%%  cpu=%.2f ms\n",
                impl, r.comp_mbs, r.decomp_mbs, r.ratio_pct, r.cpu_ms);
}

// ── Datasets ──────────────────────────────────────────────────────────────────

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
    for (auto& b : v) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        b = static_cast<uint8_t>(s);
    }
    return v;
}

static std::vector<uint8_t> make_code(size_t n) {
    std::vector<uint8_t> v(n);
    const char* pat = "int foo(int x) { return x * 2 + 1; }\n";
    const size_t pl = std::strlen(pat);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<uint8_t>(pat[i % pl]);
    return v;
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const int iters = (argc > 1) ? std::atoi(argv[1]) : 10;

    std::printf("Bzip2 comparison benchmark  iters=%d\n", iters);
    std::printf("  orot:    orot bzip2\n");
    std::printf("  libbz2:  system libbz2 (BZ2_bzBuffToBuffCompress)\n\n");

    struct { const char* label; std::vector<uint8_t> data; } datasets[] = {
        { "text (~90KB)",  make_text()           },
        { "zeros (1MB)",   make_zeros(1 << 20)   },
        { "random (1MB)",  make_random(1 << 20)  },
        { "code (~512KB)", make_code(512 * 1024) },
    };

    static const int   levels[] = { 1, 5, 9 };
    static const char* lnames[] = { "fast", "default", "best" };

    for (auto& ds : datasets) {
        for (int li = 0; li < 3; ++li) {
            const int lv = levels[li];

            std::printf("[%s  lvl=%s]\n", ds.label, lnames[li]);
            auto r_orot = run(ds.data.data(), ds.data.size(), lv,
                              orot_bz2_bound, orot_bz2_comp, orot_bz2_decomp, iters);
            auto r_lib  = run(ds.data.data(), ds.data.size(), lv,
                              libbz2_bound, libbz2_compress, libbz2_decompress, iters);
            print_row("orot", r_orot);
            print_row("libbz2", r_lib);
            if (r_orot.ok && r_lib.ok) {
                std::printf("  speedup  comp=%.2fx  decomp=%.2fx\n",
                            r_orot.comp_mbs / r_lib.comp_mbs,
                            r_orot.decomp_mbs / r_lib.decomp_mbs);
            }
            std::putchar('\n');
        }
    }

    return 0;
}
