/*
 * bench_lzma_compare.cpp — Cross-library LZMA / LZMA2 benchmark.
 *
 * Compares orot and liblzma across:
 *   - LZMA alone format
 *   - LZMA2 raw stream
 *
 * Build: cmake -B build -DOROT_BENCHMARK_COMPARE=ON
 * Run:   ./build/tests/bench_lzma_compare [iterations]
 */
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <time.h>

#include "orot/lzma.h"
#include <lzma.h>

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

static bool init_liblzma_options(lzma_options_lzma& opt, int level) {
    std::memset(&opt, 0, sizeof(opt));
    return lzma_lzma_preset(&opt, static_cast<uint32_t>(level)) == false;
}

static int liblzma_alone_compress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    int level)
{
    lzma_options_lzma opt;
    if (!init_liblzma_options(opt, level)) return -1;

    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_alone_encoder(&strm, &opt) != LZMA_OK) return -1;

    strm.next_in = src;
    strm.avail_in = src_len;
    strm.next_out = dst;
    strm.avail_out = dst_cap;

    lzma_ret ret = LZMA_OK;
    while (ret == LZMA_OK)
        ret = lzma_code(&strm, LZMA_FINISH);

    const int out = (ret == LZMA_STREAM_END) ? static_cast<int>(strm.total_out) : -1;
    lzma_end(&strm);
    return out;
}

static int liblzma_alone_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&strm, UINT64_MAX) != LZMA_OK) return -1;

    strm.next_in = src;
    strm.avail_in = src_len;
    strm.next_out = dst;
    strm.avail_out = dst_cap;

    lzma_ret ret = LZMA_OK;
    while (ret == LZMA_OK)
        ret = lzma_code(&strm, LZMA_RUN);

    const int out = (ret == LZMA_STREAM_END) ? static_cast<int>(strm.total_out) : -1;
    lzma_end(&strm);
    return out;
}

static int liblzma_alone_decompress_level(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    int)
{
    return liblzma_alone_decompress(src, src_len, dst, dst_cap);
}

static int liblzma_lzma2_compress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    int level)
{
    lzma_options_lzma opt;
    if (!init_liblzma_options(opt, level)) return -1;

    lzma_filter filters[2];
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[0].options = &opt;
    filters[1].id = LZMA_VLI_UNKNOWN;
    filters[1].options = nullptr;

    size_t out_pos = 0;
    const lzma_ret ret = lzma_raw_buffer_encode(filters, nullptr,
                                                src, src_len,
                                                dst, &out_pos, dst_cap);
    return (ret == LZMA_OK) ? static_cast<int>(out_pos) : -1;
}

static int liblzma_lzma2_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    int level)
{
    lzma_options_lzma opt;
    if (!init_liblzma_options(opt, level)) return -1;

    lzma_filter filters[2];
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[0].options = &opt;
    filters[1].id = LZMA_VLI_UNKNOWN;
    filters[1].options = nullptr;

    size_t in_pos = 0;
    size_t out_pos = 0;
    const lzma_ret ret = lzma_raw_buffer_decode(filters, nullptr,
                                                src, &in_pos, src_len,
                                                dst, &out_pos, dst_cap);
    return (ret == LZMA_OK && in_pos == src_len) ? static_cast<int>(out_pos) : -1;
}

static size_t liblzma_alone_bound(size_t src_len) {
    const size_t stream_bound = lzma_stream_buffer_bound(src_len) + 64;
    const size_t conservative = src_len * 2 + 64;
    return stream_bound > conservative ? stream_bound : conservative;
}

static size_t liblzma_raw_lzma2_bound(size_t src_len) {
    return src_len * 2 + 4096;
}

static int orot_lzma_comp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int lv) {
    return orot_lzma_compress(s, sl, d, dc, lv);
}
static int orot_lzma_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    return orot_lzma_decompress(s, sl, d, dc, nullptr);
}
static size_t orot_lzma_bound(size_t sl) { return orot_lzma_compress_bound(sl); }

static int orot_lzma2_comp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int lv) {
    return orot_lzma2_compress(s, sl, d, dc, lv);
}
static int orot_lzma2_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    return orot_lzma2_decompress(s, sl, d, dc);
}
static size_t orot_lzma2_bound(size_t sl) { return orot_lzma2_compress_bound(sl); }

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

int main(int argc, char** argv) {
    const int iters = (argc > 1) ? std::atoi(argv[1]) : 10;

    std::printf("LZMA comparison benchmark  iters=%d\n", iters);
    std::printf("  orot:     orot LZMA / LZMA2\n");
    std::printf("  liblzma:  XZ Utils liblzma (alone + raw LZMA2)\n\n");

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

            std::printf("[%s  fmt=lzma-alone  lvl=%s]\n", ds.label, lnames[li]);
            auto r_orot = run(ds.data.data(), ds.data.size(), lv,
                              orot_lzma_bound, orot_lzma_comp, orot_lzma_decomp, iters);
            auto r_lib  = run(ds.data.data(), ds.data.size(), lv,
                              liblzma_alone_bound, liblzma_alone_compress, liblzma_alone_decompress_level, iters);
            print_row("orot", r_orot);
            print_row("liblzma", r_lib);
            if (r_orot.ok && r_lib.ok) {
                std::printf("  speedup  comp=%.2fx  decomp=%.2fx\n",
                            r_orot.comp_mbs / r_lib.comp_mbs,
                            r_orot.decomp_mbs / r_lib.decomp_mbs);
            }
            std::putchar('\n');

            std::printf("[%s  fmt=lzma2-raw  lvl=%s]\n", ds.label, lnames[li]);
            auto r2_orot = run(ds.data.data(), ds.data.size(), lv,
                               orot_lzma2_bound, orot_lzma2_comp, orot_lzma2_decomp, iters);
            auto r2_lib  = run(ds.data.data(), ds.data.size(), lv,
                               liblzma_raw_lzma2_bound, liblzma_lzma2_compress, liblzma_lzma2_decompress, iters);
            print_row("orot", r2_orot);
            print_row("liblzma", r2_lib);
            if (r2_orot.ok && r2_lib.ok) {
                std::printf("  speedup  comp=%.2fx  decomp=%.2fx\n",
                            r2_orot.comp_mbs / r2_lib.comp_mbs,
                            r2_orot.decomp_mbs / r2_lib.decomp_mbs);
            }
            std::putchar('\n');
        }
    }

    return 0;
}
