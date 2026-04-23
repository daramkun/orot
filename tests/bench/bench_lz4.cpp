/*
 * bench_lz4.cpp — OROT-only LZ4 benchmark.
 *
 * Mirrors bench_compress.cpp for the LZ4 block/frame APIs:
 *   - Compression throughput (MB/s)
 *   - Decompression throughput (MB/s)
 *   - Compression ratio (%)
 *
 * Usage: ./bench_lz4 [iterations]
 */
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "orot/lz4.h"

using Clock = std::chrono::steady_clock;

struct BenchResult {
    double comp_mbs   = 0;
    double decomp_mbs = 0;
    double ratio_pct  = 0;
    bool ok           = false;
};

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

    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i)
        comp_fn(src, static_cast<int>(slen), comp.data(), bound, level);
    auto t1 = Clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    r.comp_mbs = static_cast<double>(slen) * iters / secs / (1024.0 * 1024.0);

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
    int iters = 100;
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

    std::printf("%-10s %-16s %-7s  %10s  %11s  %7s\n",
                "Format", "Dataset", "Level",
                "Comp MB/s", "Decomp MB/s", "Ratio%");
    std::printf("%s\n", std::string(71, '-').c_str());

    auto print_row = [](const char* format, const char* dataset, int level, const BenchResult& r) {
        if (!r.ok) {
            std::printf("%-10s %-16s L%-6d  %10s  %11s  %7s\n",
                        format, dataset, level, "FAIL", "-", "-");
            return;
        }
        std::printf("%-10s %-16s L%-6d  %10.1f  %11.1f  %6.1f%%\n",
                    format, dataset, level, r.comp_mbs, r.decomp_mbs, r.ratio_pct);
    };

    for (const auto& ds : datasets) {
        for (int level : levels) {
            print_row("block", ds.name, level, run_bench(
                ds.p, ds.len, level,
                [](int len) { return orot_lz4_compress_bound(len); },
                [](const uint8_t* src, int slen, uint8_t* dst, int dcap, int lvl) {
                    return orot_lz4_compress(src, slen, dst, dcap, lvl);
                },
                [](const uint8_t* src, int slen, uint8_t* dst, int dcap) {
                    return orot_lz4_decompress(src, slen, dst, dcap);
                },
                iters));

            print_row("frame", ds.name, level, run_bench(
                ds.p, ds.len, level,
                [](int len) { return orot_lz4f_compress_bound(len); },
                [](const uint8_t* src, int slen, uint8_t* dst, int dcap, int lvl) {
                    return orot_lz4f_compress(src, slen, dst, dcap, lvl);
                },
                [](const uint8_t* src, int slen, uint8_t* dst, int dcap) {
                    return orot_lz4f_decompress(src, slen, dst, dcap);
                },
                iters));
        }
    }

    return 0;
}
