/*
 * bench_brotli_compare.cpp - Brotli benchmark against Google's libbrotli.
 */
#include <brotli/decode.h>
#include <brotli/encode.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "orot/brotli.h"

static std::vector<uint8_t> make_text() {
    std::vector<uint8_t> out;
    const char* text =
        "Brotli comparison payload with repeated text, paths, headers, "
        "literal contexts, and copy distances. ";
    for (int i = 0; i < 8192; ++i)
        out.insert(out.end(), text, text + std::strlen(text));
    return out;
}

static double mbps(size_t bytes, double seconds) {
    return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds;
}

int main() {
    std::vector<uint8_t> input = make_text();
    constexpr int kIters = 50;

    std::vector<uint8_t> orot_out(orot_brotli_compress_bound(input.size()));
    int orot_size = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        orot_size = orot_brotli_compress(
            input.data(), input.size(), orot_out.data(), orot_out.size(),
            OROT_BROTLI_QUALITY_DEFAULT, OROT_BROTLI_LGWIN_DEFAULT);
        if (orot_size <= 0)
            return 1;
    }
    auto orot_end = std::chrono::steady_clock::now();

    size_t ref_cap = BrotliEncoderMaxCompressedSize(input.size());
    std::vector<uint8_t> ref_out(ref_cap);
    size_t ref_size = ref_cap;
    auto ref_start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) {
        ref_size = ref_cap;
        BROTLI_BOOL ok = BrotliEncoderCompress(
            OROT_BROTLI_QUALITY_DEFAULT, OROT_BROTLI_LGWIN_DEFAULT,
            BROTLI_MODE_GENERIC, input.size(), input.data(),
            &ref_size, ref_out.data());
        if (!ok)
            return 1;
    }
    auto ref_end = std::chrono::steady_clock::now();

    std::printf("input=%zu\n", input.size());
    std::printf("orot: size=%d ratio=%.3f speed=%.1f MiB/s\n",
                orot_size,
                static_cast<double>(orot_size) / static_cast<double>(input.size()),
                mbps(input.size() * kIters,
                     std::chrono::duration<double>(orot_end - start).count()));
    std::printf("libbrotli: size=%zu ratio=%.3f speed=%.1f MiB/s\n",
                ref_size,
                static_cast<double>(ref_size) / static_cast<double>(input.size()),
                mbps(input.size() * kIters,
                     std::chrono::duration<double>(ref_end - ref_start).count()));
    return 0;
}
