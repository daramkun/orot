/*
 * Brotli API scaffold tests.
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
    CHECK(n == -4, "compress reports not implemented");

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

    size_t actual = 123;
    n = orot_brotli_decompress(text, len,
                               out.data(), out.size(),
                               &actual);
    CHECK(n == -4, "decompress reports not implemented");
    CHECK(actual == 123, "decompress does not update size on failure");

    auto cpp_compressed = orot::brotli_api::compress(
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(text), len));
    CHECK(cpp_compressed.empty(), "C++ compress wrapper returns empty on failure");

    auto cpp_decompressed = orot::brotli_api::decompress(
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(text), len),
        256);
    CHECK(cpp_decompressed.empty(), "C++ decompress wrapper returns empty on failure");

    if (failures == 0) {
        std::printf("All Brotli scaffold tests passed.\n");
        return 0;
    }

    std::fprintf(stderr, "%d Brotli scaffold test(s) failed.\n", failures);
    return 1;
}
