/*
 * bench_brotli.cpp - Brotli whole-buffer benchmark.
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "orot/brotli.h"

static std::vector<uint8_t> make_text() {
    std::vector<uint8_t> out;
    const char* parts[] = {
        "Brotli benchmark text with repeated phrases and copy commands. ",
        "The quick brown fox jumps over the lazy dog. ",
        "content-type=text/plain; cache-control=no-cache; path=/orot/brotli\n",
    };
    for (int i = 0; i < 8192; ++i) {
        const char* p = parts[i % 3];
        out.insert(out.end(), p, p + std::strlen(p));
    }
    return out;
}

static double mbps(size_t bytes, double seconds) {
    return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds;
}

int main() {
    std::vector<uint8_t> input = make_text();
    std::vector<uint8_t> compressed(orot_brotli_compress_bound(input.size()));
    std::vector<uint8_t> decoded(input.size() + 16);

    constexpr int kIters = 100;
    int clen = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        clen = orot_brotli_compress(
            input.data(), input.size(),
            compressed.data(), compressed.size(),
            OROT_BROTLI_QUALITY_DEFAULT,
            OROT_BROTLI_LGWIN_DEFAULT);
        if (clen <= 0) {
            std::printf("compress failed: %d\n", clen);
            return 1;
        }
    }
    auto mid = std::chrono::steady_clock::now();

    size_t actual = 0;
    for (int i = 0; i < kIters; ++i) {
        int dlen = orot_brotli_decompress(
            compressed.data(), static_cast<size_t>(clen),
            decoded.data(), decoded.size(), &actual);
        if (dlen != static_cast<int>(input.size()) ||
            actual != input.size() ||
            std::memcmp(input.data(), decoded.data(), input.size()) != 0) {
            std::printf("decompress failed: %d actual=%zu\n", dlen, actual);
            return 1;
        }
    }
    auto end = std::chrono::steady_clock::now();

    double csec = std::chrono::duration<double>(mid - start).count();
    double dsec = std::chrono::duration<double>(end - mid).count();
    std::printf("Brotli input=%zu compressed=%d ratio=%.3f\n",
                input.size(), clen,
                static_cast<double>(clen) / static_cast<double>(input.size()));
    std::printf("compress %.1f MiB/s, decompress %.1f MiB/s\n",
                mbps(input.size() * kIters, csec),
                mbps(input.size() * kIters, dsec));
    return 0;
}
