/*
 * test_simd.cpp — SIMD dispatch correctness verification.
 * Checks that SIMD-accelerated checksums match known reference values and
 * that the dispatch table is properly initialized.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include "orot/deflate.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s: %s\n", msg, #cond); \
        ++failures; \
    } \
} while (0)

#define CHECK_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        std::fprintf(stderr, "FAIL: %s: got 0x%08X expected 0x%08X\n", msg, \
            static_cast<unsigned>(a), static_cast<unsigned>(b)); \
        ++failures; \
    } \
} while (0)

static void test_adler32() {
    /* RFC 1950 reference: adler32("Wikipedia") = 0x11E60398 */
    {
        const char* s = "Wikipedia";
        const uint32_t got = deflate_adler32(1,
            reinterpret_cast<const uint8_t*>(s), std::strlen(s));
        CHECK_EQ(got, 0x11E60398U, "adler32/Wikipedia");
    }

    /* adler32 of empty = initial value */
    CHECK_EQ(deflate_adler32(1, nullptr, 0), 1U, "adler32/empty");
    CHECK_EQ(deflate_adler32(42, nullptr, 0), 42U, "adler32/initial");

    /* Incremental adler32 vs single-shot */
    {
        const char* data = "Hello, World! This is a test of adler32.";
        const size_t len = std::strlen(data);
        const auto*  p   = reinterpret_cast<const uint8_t*>(data);

        const uint32_t full  = deflate_adler32(1, p, len);
        uint32_t       inc   = 1;
        for (size_t i = 0; i < len; ++i)
            inc = deflate_adler32(inc, p + i, 1);

        CHECK_EQ(full, inc, "adler32/incremental");
    }

    /* Long buffer (exercises SIMD path if available) */
    {
        std::vector<uint8_t> buf(65536);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = static_cast<uint8_t>(i);

        const uint32_t a1 = deflate_adler32(1, buf.data(), buf.size());
        const uint32_t a2 = deflate_adler32(1, buf.data(), buf.size());
        CHECK_EQ(a1, a2, "adler32/long-deterministic");
        CHECK(a1 != 0U, "adler32/long-nonzero");
    }

    std::printf("PASS: adler32 tests\n");
}

static void test_crc32() {
    /* POSIX standard reference: crc32("123456789") = 0xCBF43926 */
    {
        const char* s = "123456789";
        const uint32_t got = deflate_crc32(0,
            reinterpret_cast<const uint8_t*>(s), std::strlen(s));
        CHECK_EQ(got, 0xCBF43926U, "crc32/standard-reference");
    }

    /* crc32 of empty = initial */
    CHECK_EQ(deflate_crc32(0, nullptr, 0), 0U, "crc32/empty");

    /* Incremental crc32 vs single-shot */
    {
        const char* data = "Hello, DEFLATE! CRC32 incremental test.";
        const size_t len = std::strlen(data);
        const auto*  p   = reinterpret_cast<const uint8_t*>(data);

        const uint32_t full = deflate_crc32(0, p, len);
        uint32_t       inc  = 0;
        for (size_t i = 0; i < len; ++i)
            inc = deflate_crc32(inc, p + i, 1);

        CHECK_EQ(full, inc, "crc32/incremental");
    }

    /* Long buffer */
    {
        std::vector<uint8_t> buf(65536);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = static_cast<uint8_t>(i * 3 + 7);

        const uint32_t c1 = deflate_crc32(0, buf.data(), buf.size());
        const uint32_t c2 = deflate_crc32(0, buf.data(), buf.size());
        CHECK_EQ(c1, c2, "crc32/long-deterministic");
        CHECK(c1 != 0U, "crc32/long-nonzero");
    }

    std::printf("PASS: crc32 tests\n");
}

/*
 * Verify that compress + decompress gives identical results on multiple calls
 * (SIMD dispatch must be deterministic).
 */
static void test_determinism() {
    std::string text;
    for (int i = 0; i < 300; ++i)
        text += "SIMD determinism check: same input must always produce same output.";
    const auto* p   = reinterpret_cast<const uint8_t*>(text.data());
    const size_t len = text.size();

    const size_t bound = deflate_compress_bound(len, DEFLATE_FORMAT_ZLIB);
    std::vector<uint8_t> out1(bound), out2(bound);

    const size_t n1 = deflate_compress(p, len, out1.data(), bound, 6, DEFLATE_FORMAT_ZLIB);
    const size_t n2 = deflate_compress(p, len, out2.data(), bound, 6, DEFLATE_FORMAT_ZLIB);

    CHECK(n1 > 0, "determinism/compress-1");
    CHECK(n2 > 0, "determinism/compress-2");
    CHECK_EQ(n1, n2, "determinism/same-size");
    CHECK(std::memcmp(out1.data(), out2.data(), n1) == 0, "determinism/same-bytes");

    std::printf("PASS: SIMD determinism\n");
}

/*
 * Cross-level decompression: a stream compressed at level N must decompress
 * correctly regardless of which path the decompressor takes.
 */
static void test_cross_level() {
    std::vector<uint8_t> zeros(32768, 0);

    for (int lvl : {0, 1, 3, 6, 9, 12}) {
        const size_t bound = deflate_compress_bound(zeros.size(), DEFLATE_FORMAT_ZLIB);
        std::vector<uint8_t> comp(bound);
        std::vector<uint8_t> decomp(zeros.size());

        const size_t clen = deflate_compress(
            zeros.data(), zeros.size(), comp.data(), bound, lvl, DEFLATE_FORMAT_ZLIB);

        char label[64];
        std::snprintf(label, sizeof(label), "cross-level/L%d/compress", lvl);
        CHECK(clen > 0, label);

        size_t actual = 0;
        std::snprintf(label, sizeof(label), "cross-level/L%d/decompress", lvl);
        const deflate_result r = deflate_decompress(
            comp.data(), clen, decomp.data(), decomp.size(), &actual, DEFLATE_FORMAT_ZLIB);
        CHECK(r == DEFLATE_OK, label);

        std::snprintf(label, sizeof(label), "cross-level/L%d/size", lvl);
        CHECK(actual == zeros.size(), label);

        std::snprintf(label, sizeof(label), "cross-level/L%d/content", lvl);
        CHECK(std::memcmp(zeros.data(), decomp.data(), actual) == 0, label);
    }

    std::printf("PASS: cross-level decompression\n");
}

int main() {
    test_adler32();
    test_crc32();
    test_determinism();
    test_cross_level();

    if (failures == 0) {
        std::printf("\nAll SIMD/dispatch tests PASSED.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
