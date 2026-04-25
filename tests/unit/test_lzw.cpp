/*
 * LZW roundtrip + API/error-path tests.
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <numeric>
#include <algorithm>

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

static void test_default_bits_roundtrip(
    const uint8_t* data, int len, const char* label)
{
    const int bound = orot_lzw_compress_bound(len) + 64;
    std::vector<uint8_t> compressed(static_cast<size_t>(bound));
    std::vector<uint8_t> decompressed(static_cast<size_t>(len + 64), 0);

    const int clen = orot_lzw_compress(data, len, compressed.data(), bound, 0);
    CHECK(clen > 0 || len == 0, label);
    if (clen <= 0) return;

    const int dlen = orot_lzw_decompress(
        compressed.data(), clen,
        decompressed.data(), len + 64);
    CHECK(dlen == len, label);
    if (len > 0)
        CHECK(std::memcmp(data, decompressed.data(), static_cast<size_t>(len)) == 0, label);
}

static void expect_compress_error(
    const uint8_t* data, int len, int dst_cap, int max_bits,
    int expected, const char* label)
{
    std::vector<uint8_t> dst(std::max(dst_cap, 1), 0);
    const int rc = orot_lzw_compress(data, len, dst.data(), dst_cap, max_bits);
    CHECK(rc == expected, label);
}

static void expect_decompress_error(
    const uint8_t* data, int len, int dst_cap,
    int expected, const char* label)
{
    std::vector<uint8_t> dst(std::max(dst_cap, 1), 0);
    const int rc = orot_lzw_decompress(data, len, dst.data(), dst_cap);
    CHECK(rc == expected, label);
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
    {
        /* Default max_bits=12 path in the C API */
        std::vector<uint8_t> buf(8192);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = static_cast<uint8_t>((i * 13) & 0xFF);
        test_default_bits_roundtrip(buf.data(), static_cast<int>(buf.size()),
                                    "default-max_bits=0 uses 12");
    }
    {
        /* Force code-width growth and dictionary clear/reset at max_bits=9 */
        std::vector<uint8_t> buf(128 * 1024);
        uint32_t rng = 0x12345678u;
        for (auto& b : buf) {
            rng = rng * 1103515245u + 12345u;
            b = static_cast<uint8_t>(rng >> 24);
        }
        test_roundtrip(buf.data(), static_cast<int>(buf.size()), 9, "random-128K clear-reset bits=9");
    }
    {
        /* Small boundary sizes around initial dictionary growth */
        for (int n : {2, 3, 4, 255, 256, 257, 511, 512, 513}) {
            std::vector<uint8_t> buf(static_cast<size_t>(n));
            std::iota(buf.begin(), buf.end(), 0);
            char label[64];
            std::snprintf(label, sizeof(label), "sequential-%d bits=9", n);
            test_roundtrip(buf.data(), n, 9, label);
        }
    }

    /* Negative / contract tests */
    {
        uint8_t sample[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
        expect_compress_error(sample, 8, 64, 8,  -1, "compress rejects max_bits<9");
        expect_compress_error(sample, 8, 64, 17, -1, "compress rejects max_bits>16");
        expect_compress_error(sample, 8, 1,  12, -1, "compress rejects too-small dst");
    }
    {
        const uint8_t invalid_header[] = { 8, 0x00 };
        expect_decompress_error(invalid_header, 2, 64, -1, "decompress rejects invalid header bits");
    }
    {
        const uint8_t truncated_header[] = { 12 };
        expect_decompress_error(truncated_header, 1, 64, -1, "decompress rejects truncated stream");
    }
    {
        /* CLEAR without a full follow-up code */
        const uint8_t truncated_codes[] = { 12, 0x00 };
        expect_decompress_error(truncated_codes, 2, 64, -1, "decompress rejects truncated code stream");
    }
    {
        std::vector<uint8_t> buf(4096);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = static_cast<uint8_t>(i % 11);
        const int bound = orot_lzw_compress_bound(static_cast<int>(buf.size())) + 64;
        std::vector<uint8_t> compressed(static_cast<size_t>(bound));
        const int clen = orot_lzw_compress(buf.data(), static_cast<int>(buf.size()),
                                           compressed.data(), bound, 12);
        CHECK(clen > 0, "setup valid compressed LZW stream");
        if (clen > 0) {
            expect_decompress_error(compressed.data(), clen, 32, -2,
                                    "decompress reports dst too small");
            expect_decompress_error(compressed.data(), clen - 1,
                                    static_cast<int>(buf.size()) + 16, -1,
                                    "decompress rejects truncated valid stream");
        }
    }

    if (failures == 0)
        std::printf("\nAll LZW tests PASSED.\n");
    else
        std::fprintf(stderr, "\n%d LZW test(s) FAILED.\n", failures);

    return failures == 0 ? 0 : 1;
}
