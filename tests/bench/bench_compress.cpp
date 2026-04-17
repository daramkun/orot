/*
 * bench_compress.cpp — Compression throughput benchmark.
 *
 * Measures compression MB/s at various levels for different data types.
 * No external dependency; uses std::chrono.
 *
 * Usage: ./bench_compress [iterations]
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "deflate/deflate.h"

using Clock = std::chrono::steady_clock;

static double bench_compress_once(
    const uint8_t* data, size_t len,
    int level, deflate_format fmt,
    int iterations)
{
    const size_t bound = deflate_compress_bound(len, fmt);
    std::vector<uint8_t> out(bound);

    /* Warm up */
    deflate_compress(data, len, out.data(), bound, level, fmt);

    auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i)
        deflate_compress(data, len, out.data(), bound, level, fmt);
    auto t1 = Clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double bytes = static_cast<double>(len) * iterations;
    return bytes / secs / (1024.0 * 1024.0);  /* MB/s */
}

static double bench_decompress_once(
    const uint8_t* data, size_t len,
    int level, deflate_format fmt,
    int iterations)
{
    const size_t bound = deflate_compress_bound(len, fmt);
    std::vector<uint8_t> comp(bound);
    const size_t clen = deflate_compress(data, len, comp.data(), bound, level, fmt);
    if (clen == 0) return 0.0;

    std::vector<uint8_t> out(len + 64);

    /* Warm up */
    size_t actual = 0;
    deflate_decompress(comp.data(), clen, out.data(), out.size(), &actual, fmt);

    auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        actual = 0;
        deflate_decompress(comp.data(), clen, out.data(), out.size(), &actual, fmt);
    }
    auto t1 = Clock::now();

    const double secs  = std::chrono::duration<double>(t1 - t0).count();
    const double bytes = static_cast<double>(len) * iterations;
    return bytes / secs / (1024.0 * 1024.0);
}

int main(int argc, char* argv[]) {
    int iters = 100;
    if (argc > 1) iters = std::atoi(argv[1]);
    if (iters < 1) iters = 1;

    /* Build test datasets */
    std::string text;
    for (int i = 0; i < 2000; ++i)
        text += "The quick brown fox jumps over the lazy dog. ";
    const auto* tp  = reinterpret_cast<const uint8_t*>(text.data());
    const size_t tl = text.size();

    std::vector<uint8_t> zeros(1 << 20, 0);  /* 1 MB zeros */

    std::vector<uint8_t> rnd(1 << 20);
    {
        uint32_t st = 0xABCDEF01U;
        for (auto& b : rnd) {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;
            b = static_cast<uint8_t>(st);
        }
    }

    struct Dataset { const uint8_t* p; size_t len; const char* name; };
    Dataset datasets[] = {
        { tp,            tl,           "text (~90KB)"  },
        { zeros.data(),  zeros.size(), "zeros (1MB)"   },
        { rnd.data(),    rnd.size(),   "random (1MB)"  },
    };

    static const int levels[] = { 1, 3, 6, 9 };
    static const char* fmt_names[] = { "raw", "zlib", "gzip" };
    static const deflate_format fmts[] = {
        DEFLATE_FORMAT_RAW, DEFLATE_FORMAT_ZLIB, DEFLATE_FORMAT_GZIP
    };

    std::printf("%-30s  %4s  %4s  %12s  %12s\n",
        "dataset", "lvl", "fmt", "comp MB/s", "decomp MB/s");
    std::printf("%s\n", std::string(72, '-').c_str());

    for (const auto& ds : datasets) {
        for (int level : levels) {
            for (int fi = 0; fi < 3; ++fi) {
                const double comp_mbs = bench_compress_once(
                    ds.p, ds.len, level, fmts[fi], iters);
                const double decomp_mbs = bench_decompress_once(
                    ds.p, ds.len, level, fmts[fi], iters);

                std::printf("%-30s  %4d  %4s  %12.1f  %12.1f\n",
                    ds.name, level, fmt_names[fi], comp_mbs, decomp_mbs);
            }
        }
    }

    return 0;
}
