/*
 * Brotli uncompressed stream tests.
 */
#include <cstdio>
#include <cstring>
#include <vector>

#include "orot/brotli.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

int main() {
    const char* text = "brotli api scaffold";
    const size_t len = std::strlen(text);
    const size_t bound = orot_brotli_compress_bound(len);

    CHECK(bound >= len, "compress bound is conservative");

    std::vector<uint8_t> out(bound);
    int n = orot_brotli_compress(text, len,
                                 out.data(), out.size(),
                                 OROT_BROTLI_QUALITY_DEFAULT,
                                 OROT_BROTLI_LGWIN_DEFAULT);
    CHECK(n > 0, "compress uncompressed meta-block stream");
    const int clen = n;

    n = orot_brotli_compress(text, len,
                             out.data(), out.size(),
                             -1,
                             OROT_BROTLI_LGWIN_DEFAULT);
    CHECK(n == -1, "invalid quality rejected");

    n = orot_brotli_compress(text, len,
                             out.data(), out.size(),
                             OROT_BROTLI_QUALITY_DEFAULT,
                             OROT_BROTLI_LGWIN_MAX + 1);
    CHECK(n == -1, "invalid lgwin rejected");

    std::vector<uint8_t> tiny(1);
    n = orot_brotli_compress(text, len,
                             tiny.data(), tiny.size(),
                             OROT_BROTLI_QUALITY_DEFAULT,
                             OROT_BROTLI_LGWIN_DEFAULT);
    CHECK(n == -2, "small output buffer rejected");

    std::vector<uint8_t> decompressed(len + 16, 0);
    size_t actual = 0;
    n = orot_brotli_decompress(out.data(), (size_t)clen,
                               decompressed.data(), decompressed.size(),
                               &actual);
    CHECK(n == (int)len, "decompress uncompressed meta-block stream");
    CHECK(actual == len, "decompress reports actual size");
    CHECK(std::memcmp(text, decompressed.data(), len) == 0, "roundtrip bytes match");

    n = orot_brotli_decompress(out.data(), (size_t)clen,
                               tiny.data(), tiny.size(),
                               nullptr);
    CHECK(n == -2, "decompress reports small output buffer");

    uint8_t bad[] = {0xFF, 0xFF};
    n = orot_brotli_decompress(bad, sizeof(bad),
                               decompressed.data(), decompressed.size(),
                               nullptr);
    CHECK(n == -3, "decompress rejects malformed stream");

    std::vector<uint8_t> empty_compressed(orot_brotli_compress_bound(0));
    n = orot_brotli_compress(nullptr, 0,
                             empty_compressed.data(), empty_compressed.size(),
                             OROT_BROTLI_QUALITY_DEFAULT,
                             OROT_BROTLI_LGWIN_DEFAULT);
    CHECK(n > 0, "compress empty stream");
    actual = 999;
    int dlen = orot_brotli_decompress(empty_compressed.data(), (size_t)n,
                                      decompressed.data(), decompressed.size(),
                                      &actual);
    CHECK(dlen == 0, "decompress empty stream");
    CHECK(actual == 0, "empty stream actual size");

    auto cpp_compressed = orot::brotli_api::compress(
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(text), len));
    CHECK(!cpp_compressed.empty(), "C++ compress wrapper returns data");

    auto cpp_decompressed = orot::brotli_api::decompress(
        std::span<const uint8_t>(cpp_compressed.data(), cpp_compressed.size()),
        256);
    CHECK(cpp_decompressed.size() == len, "C++ decompress wrapper returns data");
    CHECK(std::memcmp(text, cpp_decompressed.data(), len) == 0,
          "C++ wrapper roundtrip bytes match");

    if (failures == 0) {
        std::printf("All Brotli uncompressed stream tests passed.\n");
        return 0;
    }

    std::fprintf(stderr, "%d Brotli uncompressed stream test(s) failed.\n", failures);
    return 1;
}
