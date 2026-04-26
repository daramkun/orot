/*
 * Zstd stage-1 API skeleton tests.
 */
#include <cstdio>
#include <cstdint>
#include <vector>

#include "orot/zstd.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

int main() {
    CHECK(orot_zstd_compress_bound(0) >= 0, "empty bound is valid");
    CHECK(orot_zstd_compress_bound(1024) >= 1024, "bound covers input size");
    CHECK(orot_zstd_compress_bound(-1) == -1, "negative bound rejected");

    const uint8_t src[] = { 'z', 's', 't', 'd' };
    std::vector<uint8_t> compressed(128);
    std::vector<uint8_t> decompressed(128);

    CHECK(orot_zstd_compress(src, 4, compressed.data(),
                             static_cast<int>(compressed.size()), 1) == -1,
          "compressor is explicit unsupported skeleton");
    CHECK(orot_zstd_decompress(compressed.data(), 4, decompressed.data(),
                               static_cast<int>(decompressed.size())) == -1,
          "decompressor is explicit unsupported skeleton");

    if (failures == 0)
        std::printf("All Zstd stage-1 API tests PASSED.\n");
    else
        std::fprintf(stderr, "%d Zstd test(s) FAILED.\n", failures);

    return failures == 0 ? 0 : 1;
}
