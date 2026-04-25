/*
 * Bzip2 roundtrip tests.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <numeric>

#include "orot/bzip2.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static void test_bzip2_roundtrip(
    const uint8_t* data, size_t len,
    int level, const char* label)
{
    const size_t bound = orot_bzip2_compress_bound(len);
    std::vector<uint8_t> compressed(bound);
    std::vector<uint8_t> decompressed(len + 256, 0);

    int clen = orot_bzip2_compress(data, len, compressed.data(), bound, level);
    if (len == 0) {
        CHECK(clen > 0, label);
        return;
    }
    CHECK(clen > 0, label);
    if (clen <= 0) return;

    size_t usz_out = 0;
    int dlen = orot_bzip2_decompress(compressed.data(), (size_t)clen,
                                     decompressed.data(), len + 256,
                                     &usz_out);
    CHECK(dlen == (int)len, label);
    CHECK(usz_out == len, label);
    if (len > 0 && dlen == (int)len)
        CHECK(std::memcmp(data, decompressed.data(), len) == 0, label);

    if (dlen == (int)len) {
        std::printf("PASS %-50s  in=%7zu  out=%6d  ratio=%.2f  L%d\n",
            label, len, clen,
            len > 0 ? (double)len / (double)clen : 0.0,
            level);
    }
}

int main() {
    std::printf("=== Bzip2 roundtrip tests ===\n");

    /* Single byte */
    {
        uint8_t b = 0x42;
        test_bzip2_roundtrip(&b, 1, 1, "bzip2 single-byte L1");
        test_bzip2_roundtrip(&b, 1, 9, "bzip2 single-byte L9");
    }

    /* All zeros */
    {
        std::vector<uint8_t> buf(1 << 16, 0);
        test_bzip2_roundtrip(buf.data(), buf.size(), 1, "bzip2 zeros-64K L1");
        test_bzip2_roundtrip(buf.data(), buf.size(), 9, "bzip2 zeros-64K L9");
    }

    /* Repeating pattern */
    {
        std::vector<uint8_t> buf(1 << 16);
        for (size_t i = 0; i < buf.size(); ++i) buf[i] = (uint8_t)(i % 3);
        test_bzip2_roundtrip(buf.data(), buf.size(), 1, "bzip2 pattern-mod3-64K L1");
        test_bzip2_roundtrip(buf.data(), buf.size(), 9, "bzip2 pattern-mod3-64K L9");
    }

    /* Sequential bytes */
    {
        std::vector<uint8_t> buf(1 << 16);
        std::iota(buf.begin(), buf.end(), 0);
        test_bzip2_roundtrip(buf.data(), buf.size(), 1, "bzip2 sequential-64K L1");
        test_bzip2_roundtrip(buf.data(), buf.size(), 9, "bzip2 sequential-64K L9");
    }

    /* Pseudo-random */
    {
        std::vector<uint8_t> buf(1 << 16);
        uint32_t rng = 0xDEADBEEFu;
        for (auto& b : buf) {
            rng = rng * 1664525u + 1013904223u;
            b = (uint8_t)(rng >> 24);
        }
        test_bzip2_roundtrip(buf.data(), buf.size(), 1, "bzip2 random-64K L1");
        test_bzip2_roundtrip(buf.data(), buf.size(), 9, "bzip2 random-64K L9");
    }

    /* Multi-block (> 100KB at L1) */
    {
        std::vector<uint8_t> buf(200000, 0xAB);
        test_bzip2_roundtrip(buf.data(), buf.size(), 1, "bzip2 repeat-200KB L1");
        test_bzip2_roundtrip(buf.data(), buf.size(), 9, "bzip2 repeat-200KB L9");
    }

    /* All 256 byte values */
    {
        std::vector<uint8_t> buf(256);
        std::iota(buf.begin(), buf.end(), 0);
        test_bzip2_roundtrip(buf.data(), buf.size(), 5, "bzip2 all-256-values L5");
    }

    /* RLE-heavy data */
    {
        std::vector<uint8_t> buf;
        for (int c = 0; c < 10; ++c)
            for (int k = 0; k < 300; ++k) buf.push_back((uint8_t)c);
        test_bzip2_roundtrip(buf.data(), buf.size(), 5, "bzip2 rle-heavy L5");
    }

    if (failures == 0)
        std::printf("\nAll Bzip2 tests PASSED.\n");
    else
        std::fprintf(stderr, "\n%d Bzip2 test(s) FAILED.\n", failures);

    return failures == 0 ? 0 : 1;
}
