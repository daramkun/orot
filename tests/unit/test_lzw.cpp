/*
 * LZW roundtrip tests.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <numeric>

#include "orot/lzw.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static void test_roundtrip(
    const uint8_t* data, int len,
    int max_bits, const char* label)
{
    const int bound = orot_lzw_compress_bound(len) + 64;
    std::vector<uint8_t> compressed(static_cast<size_t>(bound));
    std::vector<uint8_t> decompressed(static_cast<size_t>(len + 256), 0);

    const int clen = orot_lzw_compress(data, len, compressed.data(), bound, max_bits);
    CHECK(clen > 0 || len == 0, label);

    if (clen > 0) {
        const int dlen = orot_lzw_decompress(
            compressed.data(), clen,
            decompressed.data(), len + 256);
        CHECK(dlen == len, label);
        if (len > 0)
            CHECK(std::memcmp(data, decompressed.data(), static_cast<size_t>(len)) == 0, label);

        if (failures == 0 || dlen == len)
            std::printf("PASS %-45s  in=%6d  out=%6d  ratio=%.2f  bits=%d\n",
                label, len, clen,
                len > 0 ? static_cast<double>(len) / static_cast<double>(clen) : 0.0,
                max_bits);
    }
}

int main() {
    /* Zeros */
    {
        std::vector<uint8_t> buf(65536, 0);
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 12, "zeros-64K bits=12");
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 16, "zeros-64K bits=16");
    }

    /* Repeating pattern */
    {
        std::vector<uint8_t> buf(65536);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = static_cast<uint8_t>(i % 3);
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 12, "pattern-mod3-64K bits=12");
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 16, "pattern-mod3-64K bits=16");
    }

    /* Sequential */
    {
        std::vector<uint8_t> buf(65536);
        std::iota(buf.begin(), buf.end(), 0);
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 12, "sequential-64K bits=12");
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 16, "sequential-64K bits=16");
    }

    /* Pseudo-random (LCG) */
    {
        std::vector<uint8_t> buf(65536);
        uint32_t rng = 0xDEADBEEFu;
        for (auto& b : buf) {
            rng = rng * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(rng >> 24);
        }
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 12, "random-64K bits=12");
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 16, "random-64K bits=16");
    }

    /* Edge cases */
    test_roundtrip(nullptr, 0, 12, "empty bits=12");
    {
        uint8_t single = 0x42;
        test_roundtrip(&single, 1, 12, "single-byte bits=12");
    }
    {
        /* Same byte repeated — triggers KwKwK */
        std::vector<uint8_t> buf(1024, 0xAA);
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 9,  "kwkwk-1K bits=9");
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 12, "kwkwk-1K bits=12");
    }
    {
        /* min bits */
        std::vector<uint8_t> buf(256);
        std::iota(buf.begin(), buf.end(), 0);
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 9,  "all-bytes bits=9");
    }

    if (failures == 0)
        std::printf("\nAll LZW tests PASSED.\n");
    else
        std::fprintf(stderr, "\n%d LZW test(s) FAILED.\n", failures);

    return failures == 0 ? 0 : 1;
}
