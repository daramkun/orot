/*
 * Roundtrip compression / decompression test.
 * Tests raw, zlib, gzip formats at levels 1, 6, 9.
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
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static void test_roundtrip(
    const uint8_t* data, size_t len,
    int level, deflate_format fmt,
    const char* label)
{
    const size_t bound = deflate_compress_bound(len, fmt);
    std::vector<uint8_t> compressed(bound);
    std::vector<uint8_t> decompressed(len + 256, 0);

    const size_t clen = deflate_compress(
        data, len, compressed.data(), compressed.size(), level, fmt);
    CHECK(clen > 0, label);

    size_t actual = 0;
    const deflate_result r = deflate_decompress(
        compressed.data(), clen,
        decompressed.data(), decompressed.size(),
        &actual, fmt);
    CHECK(r == DEFLATE_OK, label);
    CHECK(actual == len, label);
    CHECK(std::memcmp(data, decompressed.data(), len) == 0, label);

    std::printf("PASS: %-40s  in=%6zu  out=%6zu  ratio=%.2f\n",
        label, len, clen, len > 0 ? (double)len / (double)clen : 0.0);
}

int main() {
    /* Test 1: all-zero buffer */
    {
        std::vector<uint8_t> zeros(65536, 0);
        test_roundtrip(zeros.data(), zeros.size(), 1, DEFLATE_FORMAT_RAW,  "zeros/raw/1");
        test_roundtrip(zeros.data(), zeros.size(), 6, DEFLATE_FORMAT_ZLIB, "zeros/zlib/6");
        test_roundtrip(zeros.data(), zeros.size(), 9, DEFLATE_FORMAT_GZIP, "zeros/gzip/9");
    }

    /* Test 2: text-like repetitive data */
    {
        std::string text;
        text.reserve(65536);
        for (int i = 0; i < 2048; ++i)
            text += "Hello, world! This is a deflate test. ";
        test_roundtrip(
            reinterpret_cast<const uint8_t*>(text.data()), text.size(),
            6, DEFLATE_FORMAT_ZLIB, "text/zlib/6");
        test_roundtrip(
            reinterpret_cast<const uint8_t*>(text.data()), text.size(),
            1, DEFLATE_FORMAT_GZIP, "text/gzip/1");
    }

    /* Test 3: pseudo-random (incompressible) */
    {
        std::vector<uint8_t> rnd(32768);
        uint32_t state = 0xDEADBEEFU;
        for (auto& b : rnd) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            b = static_cast<uint8_t>(state);
        }
        test_roundtrip(rnd.data(), rnd.size(), 6, DEFLATE_FORMAT_RAW,  "random/raw/6");
        test_roundtrip(rnd.data(), rnd.size(), 6, DEFLATE_FORMAT_GZIP, "random/gzip/6");
    }

    /* Test 4: empty input */
    {
        uint8_t dummy = 0;
        const size_t bound = deflate_compress_bound(0, DEFLATE_FORMAT_ZLIB);
        std::vector<uint8_t> out(bound);
        (void)dummy; (void)bound; (void)out;
        /* Just verify bound is non-zero */
        CHECK(bound > 0, "empty/bound>0");
        std::printf("PASS: empty/bound>0\n");
    }

    /* Test 5: single byte */
    {
        const uint8_t byte = 0x42;
        test_roundtrip(&byte, 1, 6, DEFLATE_FORMAT_ZLIB, "single-byte/zlib/6");
        test_roundtrip(&byte, 1, 6, DEFLATE_FORMAT_GZIP, "single-byte/gzip/6");
    }

    if (failures == 0) {
        std::printf("\nAll roundtrip tests PASSED.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}
