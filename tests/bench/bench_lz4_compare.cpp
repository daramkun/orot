/*
 * bench_lz4_compare.cpp — Cross-library LZ4 benchmark.
 *
 * Compares orot and liblz4 across block/frame formats:
 *   - Compression throughput (MB/s)
 *   - Decompression throughput (MB/s)
 *   - Compression ratio (%)
 *   - CPU time (ms per iteration)
 *   - RSS memory delta (KB)
 *
 * Build: cmake -B build -DOROT_DEFLATE_TESTS=ON -DOROT_LZ4=ON -DOROT_BENCHMARK_COMPARE=ON
 * Run:   ./build/tests/bench_lz4_compare [iterations]
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <time.h>

#include "orot/lz4.h"
#include <lz4.h>
#include <lz4frame.h>
#include <lz4hc.h>

using Clock = std::chrono::steady_clock;

static long rss_kb() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / 1024;
#else
    return ru.ru_maxrss;
#endif
}

static double cpu_now() {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct BenchResult {
    double comp_mbs   = 0;
    double decomp_mbs = 0;
    double ratio_pct  = 0;
    double cpu_ms     = 0;
    long rss_delta    = 0;
    bool ok           = false;
};

static int orot_block_bound(int src_size) {
    return orot_lz4_compress_bound(src_size);
}

static int orot_block_compress(
    const uint8_t* src, int src_size,
    uint8_t* dst, int dst_cap,
    int level)
{
    return orot_lz4_compress(src, src_size, dst, dst_cap, level);
}

static int orot_block_decompress(
    const uint8_t* src, int src_size,
    uint8_t* dst, int dst_cap)
{
    return orot_lz4_decompress(src, src_size, dst, dst_cap);
}

static int orot_frame_bound(int src_size) {
    return orot_lz4f_compress_bound(src_size);
}

static int orot_frame_compress(
    const uint8_t* src, int src_size,
    uint8_t* dst, int dst_cap,
    int level)
{
    return orot_lz4f_compress(src, src_size, dst, dst_cap, level);
}

static int orot_frame_decompress(
    const uint8_t* src, int src_size,
    uint8_t* dst, int dst_cap)
{
    return orot_lz4f_decompress(src, src_size, dst, dst_cap);
}

static int liblz4_block_bound(int src_size) {
    return LZ4_compressBound(src_size);
}

static int liblz4_block_compress(
    const uint8_t* src, int src_size,
    uint8_t* dst, int dst_cap,
    int level)
{
    return LZ4_compress_HC(reinterpret_cast<const char*>(src),
                           reinterpret_cast<char*>(dst),
                           src_size, dst_cap, level);
}

static int liblz4_block_decompress(
    const uint8_t* src, int src_size,
    uint8_t* dst, int dst_cap)
{
    return LZ4_decompress_safe(reinterpret_cast<const char*>(src),
                               reinterpret_cast<char*>(dst),
                               src_size, dst_cap);
}

static int liblz4_frame_bound(int src_size) {
    return static_cast<int>(LZ4F_compressFrameBound(
        static_cast<size_t>(src_size), nullptr));
}

static int liblz4_frame_compress(
    const uint8_t* src, int src_size,
    uint8_t* dst, int dst_cap,
    int level)
{
    LZ4F_preferences_t prefs{};
    prefs.compressionLevel = level;
    const size_t written = LZ4F_compressFrame(dst, static_cast<size_t>(dst_cap),
                                              src, static_cast<size_t>(src_size),
                                              &prefs);
    return LZ4F_isError(written) ? -1 : static_cast<int>(written);
}

static int liblz4_frame_decompress(
    const uint8_t* src, int src_size,
    uint8_t* dst, int dst_cap)
{
    LZ4F_dctx* dctx = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION)))
        return -1;

    const uint8_t* src_ptr = src;
    size_t src_remaining = static_cast<size_t>(src_size);
    uint8_t* dst_ptr = dst;
    size_t dst_remaining = static_cast<size_t>(dst_cap);

    while (src_remaining > 0) {
        size_t src_chunk = src_remaining;
        size_t dst_chunk = dst_remaining;
        const size_t ret = LZ4F_decompress(dctx, dst_ptr, &dst_chunk, src_ptr, &src_chunk, nullptr);
        if (LZ4F_isError(ret)) {
            LZ4F_freeDecompressionContext(dctx);
            return -1;
        }
        src_ptr += src_chunk;
        src_remaining -= src_chunk;
        dst_ptr += dst_chunk;
        dst_remaining -= dst_chunk;
        if (ret == 0) break;
        if (src_chunk == 0 && dst_chunk == 0) {
            LZ4F_freeDecompressionContext(dctx);
            return -1;
        }
    }

    LZ4F_freeDecompressionContext(dctx);
    return static_cast<int>(static_cast<size_t>(dst_cap) - dst_remaining);
}

template<typename BoundFn, typename CompFn, typename DecompFn>
static BenchResult run_bench(
    const uint8_t* src, size_t slen,
    int level,
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
        std::fprintf(stderr, "  [WARN] round-trip mismatch! clen=%d dlen=%d expected=%zu\n",
                     clen, dlen, slen);
        return r;
    }

    r.ratio_pct = 100.0 * static_cast<double>(clen) / static_cast<double>(slen);

    long rss_before = rss_kb();
    double cpu_before = cpu_now();
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        comp_fn(src, static_cast<int>(slen), comp.data(), bound, level);
    auto t1 = Clock::now();
    double cpu_after = cpu_now();
    long rss_after = rss_kb();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    r.comp_mbs = static_cast<double>(slen) * iters / secs / (1024.0 * 1024.0);
    r.cpu_ms = (cpu_after - cpu_before) * 1000.0 / iters;
    r.rss_delta = rss_after - rss_before;

    t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        decomp_fn(comp.data(), clen, decomp.data(), static_cast<int>(decomp.size()));
    t1 = Clock::now();

    secs = std::chrono::duration<double>(t1 - t0).count();
    r.decomp_mbs = static_cast<double>(slen) * iters / secs / (1024.0 * 1024.0);
    r.ok = true;
    return r;
}

int main(int argc, char* argv[]) {
    int iters = 50;
    if (argc > 1) iters = std::atoi(argv[1]);
    if (iters < 1) iters = 1;

    std::string text;
    for (int i = 0; i < 2000; ++i)
        text += "The quick brown fox jumps over the lazy dog. ";
    const auto* tp = reinterpret_cast<const uint8_t*>(text.data());
    const size_t tl = text.size();

    std::vector<uint8_t> zeros(1 << 20, 0);

    std::vector<uint8_t> rnd(1 << 20);
    {
        uint32_t state = 0xDEADBEEFU;
        for (auto& b : rnd) {
            state = state * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(state >> 24);
        }
    }

    std::vector<uint8_t> src_code(512 * 1024);
    {
        const char* pat = "int foo(int x) { return x * 2 + 1; }\n";
        const size_t pl = std::strlen(pat);
        for (size_t i = 0; i < src_code.size(); ++i)
            src_code[i] = static_cast<uint8_t>(pat[i % pl]);
    }

    struct Dataset { const uint8_t* p; size_t len; const char* name; };
    Dataset datasets[] = {
        { tp,              tl,              "text (~90KB)"  },
        { zeros.data(),    zeros.size(),    "zeros (1MB)"   },
        { rnd.data(),      rnd.size(),      "random (1MB)"  },
        { src_code.data(), src_code.size(), "code (~512KB)" },
    };

    static const int levels[] = { 1, 6, 9 };

    std::printf("%-10s %-8s %-16s %-7s  %10s  %11s  %7s  %8s  %7s\n",
                "Library", "Format", "Dataset", "Level",
                "Comp MB/s", "Decomp MB/s", "Ratio%", "CPU ms", "RSS dKB");
    std::printf("%s\n", std::string(94, '-').c_str());

    auto print_row = [](const char* lib, const char* format, const char* dataset,
                        int level, const BenchResult& r) {
        if (!r.ok) {
            std::printf("%-10s %-8s %-16s L%-6d  %10s  %11s  %7s  %8s  %7s\n",
                        lib, format, dataset, level, "FAIL", "-", "-", "-", "-");
            return;
        }
        std::printf("%-10s %-8s %-16s L%-6d  %10.1f  %11.1f  %6.1f%%  %8.3f  %7ld\n",
                    lib, format, dataset, level,
                    r.comp_mbs, r.decomp_mbs, r.ratio_pct, r.cpu_ms, r.rss_delta);
    };

    for (const auto& ds : datasets) {
        for (int level : levels) {
            print_row("orot", "block", ds.name, level, run_bench(
                ds.p, ds.len, level,
                orot_block_bound, orot_block_compress, orot_block_decompress, iters));
            print_row("liblz4", "block", ds.name, level, run_bench(
                ds.p, ds.len, level,
                liblz4_block_bound, liblz4_block_compress, liblz4_block_decompress, iters));
            print_row("orot", "frame", ds.name, level, run_bench(
                ds.p, ds.len, level,
                orot_frame_bound, orot_frame_compress, orot_frame_decompress, iters));
            print_row("liblz4", "frame", ds.name, level, run_bench(
                ds.p, ds.len, level,
                liblz4_frame_bound, liblz4_frame_compress, liblz4_frame_decompress, iters));
        }
    }

    return 0;
}
