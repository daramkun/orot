/*
 * bench_lz4_compare.cpp — Cross-library LZ4 benchmark.
 *
 * Compares orot and liblz4 across block/frame formats:
 *   - Compression throughput (MB/s)
 *   - Decompression throughput (MB/s)
 *   - Compression ratio (%)
 *   - CPU time (ms per iteration)
 *
 * Build: cmake -B build -DOROT_BENCHMARK_COMPARE=ON
 * Run:   ./build/tests/bench_lz4_compare [iterations]
 */
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/resource.h>
#include <time.h>

#include "orot/lz4.h"
#include <lz4.h>
#include <lz4frame.h>
#include <lz4hc.h>

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

static int orot_block_comp(const uint8_t* s, int sl, uint8_t* d, int dc, int lv) {
    return orot_lz4_compress(s, sl, d, dc, lv);
}
static int orot_block_decomp(const uint8_t* s, int sl, uint8_t* d, int dc) {
    return orot_lz4_decompress(s, sl, d, dc);
}
static int orot_block_bound(int sl) { return orot_lz4_compress_bound(sl); }

static int orot_frame_comp(const uint8_t* s, int sl, uint8_t* d, int dc, int lv) {
    return orot_lz4f_compress(s, sl, d, dc, lv);
}
static int orot_frame_decomp(const uint8_t* s, int sl, uint8_t* d, int dc) {
    return orot_lz4f_decompress(s, sl, d, dc);
}
static int orot_frame_bound(int sl) { return orot_lz4f_compress_bound(sl); }

static int liblz4_block_comp(const uint8_t* s, int sl, uint8_t* d, int dc, int lv) {
    return LZ4_compress_HC(reinterpret_cast<const char*>(s),
                           reinterpret_cast<char*>(d), sl, dc, lv);
}
static int liblz4_block_decomp(const uint8_t* s, int sl, uint8_t* d, int dc) {
    return LZ4_decompress_safe(reinterpret_cast<const char*>(s),
                               reinterpret_cast<char*>(d), sl, dc);
}
static int liblz4_block_bound(int sl) { return LZ4_compressBound(sl); }

static int liblz4_frame_comp(const uint8_t* s, int sl, uint8_t* d, int dc, int lv) {
    LZ4F_preferences_t prefs{};
    prefs.compressionLevel = lv;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    size_t written = LZ4F_compressFrame(d, static_cast<size_t>(dc),
                                        s, static_cast<size_t>(sl), &prefs);
    return LZ4F_isError(written) ? -1 : static_cast<int>(written);
}
static int liblz4_frame_decomp(const uint8_t* s, int sl, uint8_t* d, int dc) {
    LZ4F_dctx* ctx = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION))) return -1;
    const uint8_t* sp = s; size_t sr = static_cast<size_t>(sl);
    uint8_t*       dp = d; size_t dr = static_cast<size_t>(dc);
    while (sr > 0) {
        size_t sc = sr, dc2 = dr;
        size_t ret = LZ4F_decompress(ctx, dp, &dc2, sp, &sc, nullptr);
        if (LZ4F_isError(ret)) { LZ4F_freeDecompressionContext(ctx); return -1; }
        sp += sc; sr -= sc; dp += dc2; dr -= dc2;
        if (ret == 0) break;
        if (sc == 0 && dc2 == 0) { LZ4F_freeDecompressionContext(ctx); return -1; }
    }
    LZ4F_freeDecompressionContext(ctx);
    return static_cast<int>(static_cast<size_t>(dc) - dr);
}
static int liblz4_frame_bound(int sl) {
    LZ4F_preferences_t prefs{};
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    return static_cast<int>(LZ4F_compressFrameBound(static_cast<size_t>(sl), &prefs));
}

/* ── Runner ──────────────────────────────────────────────────────────────── */

template<typename BoundFn, typename CompFn, typename DecompFn>
static BenchResult run(
    const uint8_t* src, size_t slen, int level,
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
        std::fprintf(stderr, "  [WARN] round-trip mismatch!\n");
        return r;
    }

    r.ratio_pct = 100.0 * static_cast<double>(clen) / static_cast<double>(slen);

    double cpu0 = cpu_now();
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        comp_fn(src, static_cast<int>(slen), comp.data(), bound, level);
    auto t1 = Clock::now();
    double cpu1 = cpu_now();

    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i)
        decomp_fn(comp.data(), clen, decomp.data(), static_cast<int>(decomp.size()));
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
    uint32_t s = 0xDEADBEEFu;
    for (auto& b : v) { s = s*1664525u+1013904223u; b = (uint8_t)(s>>24); }
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

    std::printf("LZ4 comparison benchmark  iters=%d\n", iters);
    std::printf("  orot:   orot LZ4 block/frame\n");
    std::printf("  liblz4: liblz4 LZ4_compress_HC / LZ4F_compressFrame\n\n");

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

            /* block */
            std::printf("[%s  fmt=block  lvl=%s]\n", ds.label, lnames[li]);
            auto rb_orot = run(ds.data.data(), ds.data.size(), lv,
                orot_block_bound, orot_block_comp, orot_block_decomp, iters);
            auto rb_lib  = run(ds.data.data(), ds.data.size(), lv,
                liblz4_block_bound, liblz4_block_comp, liblz4_block_decomp, iters);
            print_row("orot",   rb_orot);
            print_row("liblz4", rb_lib);
            if (rb_orot.ok && rb_lib.ok)
                std::printf("  speedup  comp=%.2fx  decomp=%.2fx\n",
                    rb_orot.comp_mbs / rb_lib.comp_mbs,
                    rb_orot.decomp_mbs / rb_lib.decomp_mbs);
            std::putchar('\n');

            /* frame */
            std::printf("[%s  fmt=frame  lvl=%s]\n", ds.label, lnames[li]);
            auto rf_orot = run(ds.data.data(), ds.data.size(), lv,
                orot_frame_bound, orot_frame_comp, orot_frame_decomp, iters);
            auto rf_lib  = run(ds.data.data(), ds.data.size(), lv,
                liblz4_frame_bound, liblz4_frame_comp, liblz4_frame_decomp, iters);
            print_row("orot",   rf_orot);
            print_row("liblz4", rf_lib);
            if (rf_orot.ok && rf_lib.ok)
                std::printf("  speedup  comp=%.2fx  decomp=%.2fx\n",
                    rf_orot.comp_mbs / rf_lib.comp_mbs,
                    rf_orot.decomp_mbs / rf_lib.decomp_mbs);
            std::putchar('\n');
        }
    }

    return 0;
}
