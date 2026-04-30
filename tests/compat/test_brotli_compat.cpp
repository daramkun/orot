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

static std::vector<uint8_t> make_long_repeated_text() {
    std::vector<uint8_t> out;
    const char* paragraphs[] = {
        "Brotli compressed meta-block compatibility depends on repeated "
        "phrases, literal contexts, and copy commands staying in sync. ",
        "The decoder should accept reference encoder output across several "
        "commands without losing the last-distance ring buffer. ",
        "Long text payloads also exercise context-map selection for ordinary "
        "ASCII prose with punctuation, numbers 1234567890, and spacing. ",
    };
    for (int round = 0; round < 700; ++round) {
        const char* text = paragraphs[round % 3];
        size_t n = std::strlen(text);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(text),
                   reinterpret_cast<const uint8_t*>(text) + n);
    }
    return out;
}

static std::vector<uint8_t> make_context_text() {
    std::vector<uint8_t> out;
    const char* fragments[] = {
        "alpha beta gamma delta epsilon zeta eta theta iota kappa\n",
        "HTTP/2 headers: content-type=text/plain; cache-control=no-cache\n",
        "Signed bytes and UTF-8-ish text: Cafe naive resume jalapeno.\n",
        "path=/var/tmp/orot/brotli; query=literal_context&distance=copy\n",
    };
    for (int round = 0; round < 512; ++round) {
        const char* text = fragments[round % 4];
        size_t n = std::strlen(text);
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(text),
                   reinterpret_cast<const uint8_t*>(text) + n);
    }
    return out;
}

static std::vector<uint8_t> make_block_switch_text() {
    std::vector<uint8_t> out;
    for (int section = 0; section < 48; ++section) {
        if ((section % 4) == 0) {
            const char* text =
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
            for (int i = 0; i < 80; ++i)
                out.insert(out.end(), text, text + std::strlen(text));
        } else if ((section % 4) == 1) {
            const char* text =
                "JSON:{\"name\":\"orot\",\"codec\":\"brotli\",\"block\":";
            for (int i = 0; i < 80; ++i) {
                out.insert(out.end(), text, text + std::strlen(text));
                out.push_back(static_cast<uint8_t>('0' + (i % 10)));
                out.insert(out.end(), {'}', '\n'});
            }
        } else if ((section % 4) == 2) {
            for (int i = 0; i < 4096; ++i)
                out.push_back(static_cast<uint8_t>((i * 37 + section * 11) & 0xff));
        } else {
            const char* text =
                "literal block contexts should change when nearby bytes change; ";
            for (int i = 0; i < 96; ++i)
                out.insert(out.end(), text, text + std::strlen(text));
        }
    }
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

static void run_libbrotli_to_orot_distance_params(
    const Dataset& ds,
    int quality,
    int lgwin,
    uint32_t npostfix,
    uint32_t ndirect)
{
    char label[220];
    std::snprintf(label, sizeof(label),
                  "Brotli libbrotlienc->orot ds=%-8s q=%d lgwin=%d npostfix=%u ndirect=%u",
                  ds.name, quality, lgwin, npostfix, ndirect);

    size_t compressed_size = BrotliEncoderMaxCompressedSize(ds.data.size());
    std::vector<uint8_t> compressed(compressed_size);
    BrotliEncoderState* state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        std::fprintf(stderr, "FAIL: %s (encoder allocation failed)\n", label);
        ++failures;
        return;
    }

    size_t available_in = ds.data.size();
    const uint8_t* next_in = ds.data.data();
    size_t available_out = compressed_size;
    uint8_t* next_out = compressed.data();

    BROTLI_BOOL ok =
        BrotliEncoderSetParameter(state, BROTLI_PARAM_QUALITY, quality) &&
        BrotliEncoderSetParameter(state, BROTLI_PARAM_LGWIN, lgwin) &&
        BrotliEncoderSetParameter(state, BROTLI_PARAM_MODE, BROTLI_MODE_GENERIC) &&
        BrotliEncoderSetParameter(state, BROTLI_PARAM_NPOSTFIX, npostfix) &&
        BrotliEncoderSetParameter(state, BROTLI_PARAM_NDIRECT, ndirect) &&
        BrotliEncoderCompressStream(
            state, BROTLI_OPERATION_FINISH,
            &available_in, &next_in,
            &available_out, &next_out,
            nullptr);
    if (ok) {
        ok = BrotliEncoderIsFinished(state);
        compressed_size -= available_out;
    }
    BrotliEncoderDestroyInstance(state);

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
        {"quick", {'T', 'h', 'e', ' ', 'q', 'u', 'i', 'c',
                   'k', ' ', 'b', 'r', 'o', 'w', 'n', ' ',
                   'f', 'o', 'x', ' ', 'j', 'u', 'm', 'p',
                   's', ' ', 'o', 'v', 'e', 'r', ' ', 't',
                   'h', 'e', ' ', 'l', 'a', 'z', 'y', ' ',
                   'd', 'o', 'g', '.'}},
        {"repeata", std::vector<uint8_t>(32, 'a')},
    };
    for (const auto& ds : small_datasets) {
        run_libbrotli_to_orot(ds, 0, OROT_BROTLI_LGWIN_DEFAULT);
        run_libbrotli_to_orot(ds, 1, OROT_BROTLI_LGWIN_DEFAULT);
    }
    run_libbrotli_to_orot(small_datasets[2], 5, OROT_BROTLI_LGWIN_DEFAULT);
    run_libbrotli_to_orot(small_datasets[3], 5, OROT_BROTLI_LGWIN_DEFAULT);

    Dataset text_compat_datasets[] = {
        {"longtext", make_long_repeated_text()},
        {"contexts", make_context_text()},
        {"blocks", make_block_switch_text()},
    };
    for (const auto& ds : text_compat_datasets) {
        run_libbrotli_to_orot(ds, 5, OROT_BROTLI_LGWIN_DEFAULT);
        run_libbrotli_to_orot(ds, 9, OROT_BROTLI_LGWIN_DEFAULT);
    }

    run_libbrotli_to_orot_distance_params(
        text_compat_datasets[0], 5, OROT_BROTLI_LGWIN_DEFAULT, 1, 4);
    run_libbrotli_to_orot_distance_params(
        text_compat_datasets[0], 9, OROT_BROTLI_LGWIN_DEFAULT, 2, 12);
    run_libbrotli_to_orot_distance_params(
        text_compat_datasets[2], 9, OROT_BROTLI_LGWIN_DEFAULT, 3, 24);

    std::printf("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
