/*
 * bench_brotli_compare.cpp - Brotli benchmark against Google's libbrotli.
 */
#include <brotli/decode.h>
#include <brotli/encode.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "orot/brotli.h"

struct Dataset {
    const char* name;
    std::vector<uint8_t> data;
};

static std::vector<uint8_t> make_text() {
    std::vector<uint8_t> out;
    const char* text =
        "Brotli comparison payload with repeated text, paths, headers, "
        "literal contexts, and copy distances. ";
    for (int i = 0; i < 8192; ++i)
        out.insert(out.end(), text, text + std::strlen(text));
    return out;
}

static std::vector<uint8_t> make_mixed() {
    std::vector<uint8_t> out;
    for (int section = 0; section < 128; ++section) {
        const char* header = "HTTP/2 200 OK\ncontent-type: application/json\n\n";
        out.insert(out.end(), header, header + std::strlen(header));
        const char* json = "{\"codec\":\"brotli\",\"path\":\"/assets/app.css\",\"repeat\":";
        out.insert(out.end(), json, json + std::strlen(json));
        out.push_back(static_cast<uint8_t>('0' + (section % 10)));
        out.insert(out.end(), {'}', '\n'});
        for (int i = 0; i < 256; ++i)
            out.push_back(static_cast<uint8_t>((i * 31 + section * 17) & 0xff));
    }
    return out;
}

static std::vector<uint8_t> make_random(size_t n) {
    std::vector<uint8_t> out(n);
    uint32_t x = 0x12345678u;
    for (auto& b : out) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(x >> 24);
    }
    return out;
}

static double mbps(size_t bytes, double seconds) {
    if (seconds <= 0.0)
        return 0.0;
    return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds;
}

static bool verify_bytes(
    const std::vector<uint8_t>& expected,
    const std::vector<uint8_t>& actual,
    size_t actual_size)
{
    return actual_size == expected.size() &&
        (expected.empty() ||
         std::memcmp(expected.data(), actual.data(), expected.size()) == 0);
}

static int run_dataset(const Dataset& ds) {
    int iters = ds.data.size() < 128 * 1024 ? 100 : 30;

    std::vector<uint8_t> orot_out(orot_brotli_compress_bound(ds.data.size()));
    std::vector<uint8_t> ref_out(BrotliEncoderMaxCompressedSize(ds.data.size()));
    int orot_size = 0;
    size_t ref_size = ref_out.size();

    auto orot_c0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        orot_size = orot_brotli_compress(
            ds.data.data(), ds.data.size(), orot_out.data(), orot_out.size(),
            OROT_BROTLI_QUALITY_DEFAULT, OROT_BROTLI_LGWIN_DEFAULT);
        if (orot_size <= 0)
            return 1;
    }
    auto orot_c1 = std::chrono::steady_clock::now();

    auto ref_c0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        ref_size = ref_out.size();
        BROTLI_BOOL ok = BrotliEncoderCompress(
            OROT_BROTLI_QUALITY_DEFAULT, OROT_BROTLI_LGWIN_DEFAULT,
            BROTLI_MODE_GENERIC, ds.data.size(), ds.data.data(),
            &ref_size, ref_out.data());
        if (!ok)
            return 1;
    }
    auto ref_c1 = std::chrono::steady_clock::now();

    std::vector<uint8_t> decoded(ds.data.size() + 16);
    size_t actual = 0;
    auto orot_d0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        int dlen = orot_brotli_decompress(
            orot_out.data(), static_cast<size_t>(orot_size),
            decoded.data(), decoded.size(), &actual);
        if (dlen != static_cast<int>(ds.data.size()) ||
            !verify_bytes(ds.data, decoded, actual))
            return 1;
    }
    auto orot_d1 = std::chrono::steady_clock::now();

    auto ref_d0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        size_t decoded_size = decoded.size();
        BrotliDecoderResult ret = BrotliDecoderDecompress(
            ref_size, ref_out.data(), &decoded_size, decoded.data());
        if (ret != BROTLI_DECODER_RESULT_SUCCESS ||
            !verify_bytes(ds.data, decoded, decoded_size))
            return 1;
    }
    auto ref_d1 = std::chrono::steady_clock::now();

    size_t cross_size = decoded.size();
    BrotliDecoderResult cross = BrotliDecoderDecompress(
        static_cast<size_t>(orot_size), orot_out.data(),
        &cross_size, decoded.data());
    bool orot_to_ref = cross == BROTLI_DECODER_RESULT_SUCCESS &&
        verify_bytes(ds.data, decoded, cross_size);

    actual = 0;
    int ref_to_orot_len = orot_brotli_decompress(
        ref_out.data(), ref_size, decoded.data(), decoded.size(), &actual);
    bool ref_to_orot = ref_to_orot_len == static_cast<int>(ds.data.size()) &&
        verify_bytes(ds.data, decoded, actual);

    double orot_c_sec = std::chrono::duration<double>(orot_c1 - orot_c0).count();
    double ref_c_sec = std::chrono::duration<double>(ref_c1 - ref_c0).count();
    double orot_d_sec = std::chrono::duration<double>(orot_d1 - orot_d0).count();
    double ref_d_sec = std::chrono::duration<double>(ref_d1 - ref_d0).count();

    std::printf("[%s] input=%zu iters=%d\n", ds.name, ds.data.size(), iters);
    std::printf("  orot      size=%d ratio=%.4f enc=%7.1f MiB/s dec=%7.1f MiB/s\n",
                orot_size,
                static_cast<double>(orot_size) / static_cast<double>(ds.data.size()),
                mbps(ds.data.size() * static_cast<size_t>(iters), orot_c_sec),
                mbps(ds.data.size() * static_cast<size_t>(iters), orot_d_sec));
    std::printf("  libbrotli size=%zu ratio=%.4f enc=%7.1f MiB/s dec=%7.1f MiB/s\n",
                ref_size,
                static_cast<double>(ref_size) / static_cast<double>(ds.data.size()),
                mbps(ds.data.size() * static_cast<size_t>(iters), ref_c_sec),
                mbps(ds.data.size() * static_cast<size_t>(iters), ref_d_sec));
    std::printf("  compat    orot->lib=%s lib->orot=%s\n",
                orot_to_ref ? "yes" : "no",
                ref_to_orot ? "yes" : "no");
    return orot_to_ref ? 0 : 1;
}

int main() {
    Dataset datasets[] = {
        {"text", make_text()},
        {"mixed", make_mixed()},
        {"random", make_random(64 * 1024)},
    };

    for (const auto& ds : datasets) {
        if (run_dataset(ds) != 0)
            return 1;
    }
    return 0;
}
