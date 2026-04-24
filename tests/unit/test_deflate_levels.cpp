/*
 * test_levels.cpp — All compression levels 0-12 roundtrip validation.
 * Verifies decompressed output equals input for every level × format × data type.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include "orot/deflate.hpp"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s: %s\n", msg, #cond); \
        ++failures; \
    } \
} while (0)

static bool roundtrip(
    const uint8_t* data, size_t len,
    int level, deflate_format fmt)
{
    const size_t bound = deflate_compress_bound(len, fmt);
    std::vector<uint8_t> comp(bound);
    std::vector<uint8_t> decomp(len + 64, 0);

    const size_t clen = deflate_compress(data, len, comp.data(), comp.size(), level, fmt);
    if (clen == 0 && len > 0) return false;

    size_t actual = 0;
    const deflate_result r = deflate_decompress(
        comp.data(), clen, decomp.data(), decomp.size(), &actual, fmt);

    if (r != DEFLATE_OK) return false;
    if (actual != len)   return false;
    if (len > 0 && std::memcmp(data, decomp.data(), len) != 0) return false;
    return true;
}

static void test_data(const uint8_t* data, size_t len, const char* name) {
    static const deflate_format formats[] = {
        DEFLATE_FORMAT_RAW, DEFLATE_FORMAT_ZLIB, DEFLATE_FORMAT_GZIP
    };
    static const char* fmt_names[] = { "raw", "zlib", "gzip" };

    for (int level = 0; level <= 12; ++level) {
        for (int fi = 0; fi < 3; ++fi) {
            char label[128];
            std::snprintf(label, sizeof(label), "%s/L%d/%s", name, level, fmt_names[fi]);
            CHECK(roundtrip(data, len, level, formats[fi]), label);
            if (failures == 0)
                std::printf("PASS: %s\n", label);
        }
    }
}

int main() {
    /* Empty input */
    {
        test_data(nullptr, 0, "empty");
    }

    /* Single byte */
    {
        const uint8_t b = 0x42;
        test_data(&b, 1, "single-byte");
    }

    /* All-zero buffer (highly compressible) */
    {
        std::vector<uint8_t> zeros(4096, 0);
        test_data(zeros.data(), zeros.size(), "zeros-4k");
    }

    /* Repetitive text (good LZ77 target) */
    {
        std::string s;
        for (int i = 0; i < 200; ++i)
            s += "The quick brown fox jumps over the lazy dog. ";
        test_data(reinterpret_cast<const uint8_t*>(s.data()), s.size(), "text");
    }

    /* Pseudo-random (incompressible) */
    {
        std::vector<uint8_t> rnd(8192);
        uint32_t st = 0xCAFEBABEU;
        for (auto& b : rnd) {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;
            b = static_cast<uint8_t>(st);
        }
        test_data(rnd.data(), rnd.size(), "random");
    }

    /* All-same byte (extreme RLE) */
    {
        std::vector<uint8_t> rep(16384, 0xAA);
        test_data(rep.data(), rep.size(), "repeat-0xAA");
    }

    /* Binary-like (mix of patterns) */
    {
        std::vector<uint8_t> bin(8192);
        for (size_t i = 0; i < bin.size(); ++i)
            bin[i] = static_cast<uint8_t>(i * 73 + (i >> 3));
        test_data(bin.data(), bin.size(), "binary");
    }

    if (failures == 0) {
        std::printf("\nAll level tests PASSED.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
