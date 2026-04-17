/*
 * Format framing (zlib / gzip) unit tests.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "deflate/deflate.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        ++failures; \
    } \
} while (0)

static const char TEXT[] =
    "The quick brown fox jumps over the lazy dog. "
    "Pack my box with five dozen liquor jugs. "
    "How vexingly quick daft zebras jump!";

static void test_zlib_header() {
    const size_t len   = std::strlen(TEXT);
    const size_t bound = deflate_compress_bound(len, DEFLATE_FORMAT_ZLIB);
    std::vector<uint8_t> out(bound);

    const size_t n = deflate_compress(
        TEXT, len, out.data(), out.size(), 6, DEFLATE_FORMAT_ZLIB);
    CHECK(n >= 6, "zlib: compressed size >= 6");
    /* CMF byte: low 4 bits = 8 (deflate), high 4 bits = window size */
    CHECK((out[0] & 0x0F) == 8, "zlib: CMF method=8");
    /* CMF*256+FLG divisible by 31 */
    CHECK(((static_cast<unsigned>(out[0]) << 8) | out[1]) % 31 == 0,
          "zlib: header checksum");
    std::printf("PASS: zlib header validity\n");
}

static void test_gzip_header() {
    const size_t len   = std::strlen(TEXT);
    const size_t bound = deflate_compress_bound(len, DEFLATE_FORMAT_GZIP);
    std::vector<uint8_t> out(bound);

    const size_t n = deflate_compress(
        TEXT, len, out.data(), out.size(), 6, DEFLATE_FORMAT_GZIP);
    CHECK(n >= 18, "gzip: compressed size >= 18");
    CHECK(out[0] == 0x1F && out[1] == 0x8B, "gzip: magic bytes");
    CHECK(out[2] == 8, "gzip: method=deflate");
    std::printf("PASS: gzip header validity\n");
}

static void test_checksum_mismatch() {
    const size_t len   = std::strlen(TEXT);
    const size_t bound = deflate_compress_bound(len, DEFLATE_FORMAT_ZLIB);
    std::vector<uint8_t> out(bound);

    const size_t n = deflate_compress(
        TEXT, len, out.data(), out.size(), 6, DEFLATE_FORMAT_ZLIB);
    CHECK(n > 0, "checksum: compress ok");

    /* Corrupt the Adler-32 trailer */
    out[n - 1] ^= 0xFF;

    std::vector<uint8_t> dec(len + 16, 0);
    size_t actual = 0;
    const deflate_result r = deflate_decompress(
        out.data(), n, dec.data(), dec.size(), &actual, DEFLATE_FORMAT_ZLIB);
    CHECK(r == DEFLATE_DATA_ERROR, "zlib: corrupted checksum detected");
    std::printf("PASS: corrupted checksum detected\n");
}

static void test_crc32() {
    /* CRC-32 of "123456789" = 0xCBF43926 */
    static const uint8_t data[] = "123456789";
    const uint32_t crc = deflate_crc32(0, data, 9);
    CHECK(crc == 0xCBF43926U, "crc32('123456789')==0xCBF43926");
    std::printf("PASS: crc32 reference vector\n");
}

static void test_adler32() {
    /* Adler-32 of "Wikipedia" = 0x11E60398 */
    static const uint8_t data[] = "Wikipedia";
    const uint32_t a = deflate_adler32(1, data, 9);
    CHECK(a == 0x11E60398U, "adler32('Wikipedia')==0x11E60398");
    std::printf("PASS: adler32 reference vector\n");
}

int main() {
    test_zlib_header();
    test_gzip_header();
    test_checksum_mismatch();
    test_crc32();
    test_adler32();

    if (failures == 0) {
        std::printf("\nAll format tests PASSED.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}
