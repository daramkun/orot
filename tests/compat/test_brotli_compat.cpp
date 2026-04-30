/*
 * test_brotli_compat.cpp - Brotli compatibility tests for the current
 * uncompressed meta-block implementation.
 */

#include <brotli/decode.h>
#include <brotli/encode.h>

#include "orot/brotli.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static int passes = 0;

struct Dataset {
    const char* name;
    std::vector<uint8_t> data;
};

static std::vector<uint8_t> make_text() {
    std::vector<uint8_t> out;
    const char* pat = "Brotli uncompressed compatibility payload. ";
    size_t n = std::strlen(pat);
    for (int i = 0; i < 1000; ++i)
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(pat),
                   reinterpret_cast<const uint8_t*>(pat) + n);
    return out;
}

static std::vector<uint8_t> make_random(size_t n) {
    std::vector<uint8_t> out(n);
    uint32_t x = 0xBADC0DEu;
    for (auto& b : out) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(x >> 24);
    }
    return out;
}

static void run_orot_to_libbrotli(const Dataset& ds, int lgwin) {
    char label[160];
    std::snprintf(label, sizeof(label),
                  "Brotli orot->libbrotlidec ds=%-8s lgwin=%d",
                  ds.name, lgwin);

    std::vector<uint8_t> compressed(orot_brotli_compress_bound(ds.data.size()));
    int clen = orot_brotli_compress(
        ds.data.data(), ds.data.size(),
        compressed.data(), compressed.size(),
        OROT_BROTLI_QUALITY_DEFAULT, lgwin);
    if (clen <= 0) {
        std::fprintf(stderr, "FAIL: %s (orot compress returned %d)\n", label, clen);
        ++failures;
        return;
    }

    std::vector<uint8_t> decoded(ds.data.size() + 16);
    size_t decoded_size = decoded.size();
    BrotliDecoderResult ret = BrotliDecoderDecompress(
        static_cast<size_t>(clen), compressed.data(),
        &decoded_size, decoded.data());

    if (ret != BROTLI_DECODER_RESULT_SUCCESS ||
        decoded_size != ds.data.size() ||
        (!ds.data.empty() && std::memcmp(ds.data.data(), decoded.data(), ds.data.size()) != 0)) {
        std::fprintf(stderr, "FAIL: %s (decoder result=%d got=%zu expected=%zu)\n",
                     label, static_cast<int>(ret), decoded_size, ds.data.size());
        ++failures;
        return;
    }

    std::printf("PASS: %s\n", label);
    ++passes;
}

static void run_libbrotli_to_orot(const Dataset& ds, int quality, int lgwin) {
    char label[180];
    std::snprintf(label, sizeof(label),
                  "Brotli libbrotlienc->orot ds=%-8s q=%d lgwin=%d",
                  ds.name, quality, lgwin);

    size_t compressed_size = BrotliEncoderMaxCompressedSize(ds.data.size());
    std::vector<uint8_t> compressed(compressed_size);
    BROTLI_BOOL ok = BrotliEncoderCompress(
        quality, lgwin, BROTLI_MODE_GENERIC,
        ds.data.size(), ds.data.data(),
        &compressed_size, compressed.data());
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (libbrotlienc failed)\n", label);
        ++failures;
        return;
    }

    std::vector<uint8_t> decoded(ds.data.size() + 16);
    size_t actual = 0;
    int dlen = orot_brotli_decompress(
        compressed.data(), compressed_size,
        decoded.data(), decoded.size(),
        &actual);

    if (dlen != static_cast<int>(ds.data.size()) ||
        actual != ds.data.size() ||
        (!ds.data.empty() && std::memcmp(ds.data.data(), decoded.data(), ds.data.size()) != 0)) {
        std::fprintf(stderr, "FAIL: %s (orot returned=%d got=%zu expected=%zu)\n",
                     label, dlen, actual, ds.data.size());
        ++failures;
        return;
    }

    std::printf("PASS: %s\n", label);
    ++passes;
}

int main() {
    Dataset datasets[] = {
        {"empty", {}},
        {"text", make_text()},
        {"zeros", std::vector<uint8_t>(64 * 1024, 0)},
        {"random", make_random(64 * 1024)},
    };

    int windows[] = {
        OROT_BROTLI_LGWIN_MIN,
        OROT_BROTLI_LGWIN_DEFAULT,
        OROT_BROTLI_LGWIN_MAX,
    };

    for (const auto& ds : datasets)
        for (int lgwin : windows)
            run_orot_to_libbrotli(ds, lgwin);

    Dataset small_datasets[] = {
        {"abc", {'a', 'b', 'c'}},
        {"phrase", {'h', 'e', 'l', 'l', 'o', ' ', 'h', 'e',
                    'l', 'l', 'o', ' ', 'h', 'e', 'l', 'l', 'o'}},
    };
    for (const auto& ds : small_datasets) {
        run_libbrotli_to_orot(ds, 0, OROT_BROTLI_LGWIN_DEFAULT);
        run_libbrotli_to_orot(ds, 1, OROT_BROTLI_LGWIN_DEFAULT);
    }

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
