/*
 * test_parallel.cpp — Parallel compression correctness.
 * Verifies that ParallelCompressor produces output that decompresses to
 * the original input for various thread counts and block sizes.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <thread>

#include "deflate/deflate.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s: %s\n", msg, #cond); \
        ++failures; \
    } \
} while (0)

static bool parallel_roundtrip(
    const uint8_t* data, size_t len,
    int level, deflate_format fmt,
    int threads, size_t block_size)
{
    deflate_parallel_ctx* ctx = deflate_parallel_new(level, fmt, threads, block_size);
    if (!ctx) return false;

    const size_t bound = deflate_compress_bound(len, fmt) + 4096;
    std::vector<uint8_t> comp(bound);

    const size_t clen = deflate_parallel_compress(
        ctx, data, len, comp.data(), comp.size());
    deflate_parallel_free(ctx);

    if (clen == 0 && len > 0) return false;

    std::vector<uint8_t> decomp(len + 64, 0);
    size_t actual = 0;
    const deflate_result r = deflate_decompress(
        comp.data(), clen, decomp.data(), decomp.size(), &actual, fmt);

    if (r != DEFLATE_OK) return false;
    if (actual != len)   return false;
    if (len > 0 && std::memcmp(data, decomp.data(), len) != 0) return false;
    return true;
}

int main() {
    const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    const int max_threads = (hw_threads > 0) ? std::min(hw_threads, 8) : 2;

    /* Test data */
    std::string text;
    for (int i = 0; i < 1000; ++i)
        text += "Parallel deflate test data. The quick brown fox jumps over the lazy dog. ";
    const auto* td   = reinterpret_cast<const uint8_t*>(text.data());
    const size_t tlen = text.size();

    std::vector<uint8_t> zeros(256 * 1024, 0);

    std::vector<uint8_t> rnd(128 * 1024);
    {
        uint32_t st = 0x12345678U;
        for (auto& b : rnd) {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;
            b = static_cast<uint8_t>(st);
        }
    }

    static const struct { int threads; size_t block; } configs[] = {
        {1,  64*1024 },
        {2,  64*1024 },
        {4, 128*1024 },
        {0,  256*1024},  /* 0 = auto-detect */
    };

    static const deflate_format formats[] = {
        DEFLATE_FORMAT_RAW, DEFLATE_FORMAT_ZLIB, DEFLATE_FORMAT_GZIP
    };
    static const char* fnames[] = { "raw", "zlib", "gzip" };

    for (const auto& cfg : configs) {
        int t = cfg.threads > 0 ? std::min(cfg.threads, max_threads) : 0;

        for (int fi = 0; fi < 3; ++fi) {
            char label[128];

            std::snprintf(label, sizeof(label),
                "text/L6/%s/T%d/B%zu", fnames[fi], t, cfg.block);
            CHECK(parallel_roundtrip(td, tlen, 6, formats[fi], t, cfg.block), label);
            if (failures == 0) std::printf("PASS: %s\n", label);

            std::snprintf(label, sizeof(label),
                "zeros/L1/%s/T%d/B%zu", fnames[fi], t, cfg.block);
            CHECK(parallel_roundtrip(zeros.data(), zeros.size(), 1, formats[fi], t, cfg.block), label);
            if (failures == 0) std::printf("PASS: %s\n", label);

            std::snprintf(label, sizeof(label),
                "random/L6/%s/T%d/B%zu", fnames[fi], t, cfg.block);
            CHECK(parallel_roundtrip(rnd.data(), rnd.size(), 6, formats[fi], t, cfg.block), label);
            if (failures == 0) std::printf("PASS: %s\n", label);
        }
    }

    if (failures == 0) {
        std::printf("\nAll parallel tests PASSED.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
