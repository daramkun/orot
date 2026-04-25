/*
 * LZMA / LZMA2 roundtrip tests.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <numeric>

#include "orot/lzma.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

/* ── LZMA (alone format) roundtrip ──────────────────────────────────────── */

static void test_lzma_roundtrip(
    const uint8_t* data, size_t len,
    int level, const char* label)
{
    const size_t bound = orot_lzma_compress_bound(len);
    std::vector<uint8_t> compressed(bound);
    std::vector<uint8_t> decompressed(len + 256, 0);

    int clen = orot_lzma_compress(data, len,
                                   compressed.data(), bound,
                                   level);
    if (len == 0) {
        /* empty input may produce small header-only stream */
        CHECK(clen > 0, label);
        return;
    }
    CHECK(clen > 0, label);
    if (clen <= 0) return;

    size_t usz_out = 0;
    int dlen = orot_lzma_decompress(compressed.data(), (size_t)clen,
                                    decompressed.data(), len + 256,
                                    &usz_out);
    CHECK(dlen == (int)len, label);
    CHECK(usz_out == len, label);
    if (len > 0 && dlen == (int)len)
        CHECK(std::memcmp(data, decompressed.data(), len) == 0, label);

    if (failures == 0 || dlen == (int)len) {
        std::printf("PASS %-50s  in=%7zu  out=%6d  ratio=%.2f  L%d\n",
            label, len, clen,
            len > 0 ? (double)len / (double)clen : 0.0,
            level);
    }
}

/* ── LZMA2 (chunk stream) roundtrip ─────────────────────────────────────── */

static void test_lzma2_roundtrip(
    const uint8_t* data, size_t len,
    int level, const char* label)
{
    const size_t bound = orot_lzma2_compress_bound(len);
    std::vector<uint8_t> compressed(bound);
    std::vector<uint8_t> decompressed(len + 256, 0);

    int clen = orot_lzma2_compress(data, len,
                                    compressed.data(), bound,
                                    level);
    if (len == 0) {
        CHECK(clen > 0, label);
        return;
    }
    CHECK(clen > 0, label);
    if (clen <= 0) return;

    int dlen = orot_lzma2_decompress(compressed.data(), (size_t)clen,
                                     decompressed.data(), len + 256);
    CHECK(dlen == (int)len, label);
    if (len > 0 && dlen == (int)len)
        CHECK(std::memcmp(data, decompressed.data(), len) == 0, label);

    if (failures == 0 || dlen == (int)len) {
        std::printf("PASS %-50s  in=%7zu  out=%6d  ratio=%.2f  L%d\n",
            label, len, clen,
            len > 0 ? (double)len / (double)clen : 0.0,
            level);
    }
}

int main() {
    std::printf("=== LZMA alone format ===\n");

    /* Empty */
    test_lzma_roundtrip(nullptr, 0, 1, "lzma empty L1");

    /* Single byte */
    {
        uint8_t b = 0x42;
        test_lzma_roundtrip(&b, 1, 1, "lzma single-byte L1");
    }

    /* All zeros */
    {
        std::vector<uint8_t> buf(1 << 16, 0);
        test_lzma_roundtrip(buf.data(), buf.size(), 1, "lzma zeros-64K L1");
        test_lzma_roundtrip(buf.data(), buf.size(), 5, "lzma zeros-64K L5");
        test_lzma_roundtrip(buf.data(), buf.size(), 9, "lzma zeros-64K L9");
    }

    /* Repeating pattern */
    {
        std::vector<uint8_t> buf(1 << 16);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = (uint8_t)(i % 3);
        test_lzma_roundtrip(buf.data(), buf.size(), 1, "lzma pattern-mod3-64K L1");
        test_lzma_roundtrip(buf.data(), buf.size(), 5, "lzma pattern-mod3-64K L5");
    }

    /* Sequential bytes */
    {
        std::vector<uint8_t> buf(1 << 16);
        std::iota(buf.begin(), buf.end(), 0);
        test_lzma_roundtrip(buf.data(), buf.size(), 1, "lzma sequential-64K L1");
        test_lzma_roundtrip(buf.data(), buf.size(), 5, "lzma sequential-64K L5");
    }

    /* Pseudo-random (not very compressible) */
    {
        std::vector<uint8_t> buf(1 << 16);
        uint32_t rng = 0xDEADBEEFu;
        for (auto& b : buf) {
            rng = rng * 1664525u + 1013904223u;
            b = (uint8_t)(rng >> 24);
        }
        test_lzma_roundtrip(buf.data(), buf.size(), 1, "lzma random-64K L1");
        test_lzma_roundtrip(buf.data(), buf.size(), 5, "lzma random-64K L5");
    }

    /* Multi-block (> 64KB, exercises LZMA2 chunking indirectly) */
    {
        std::vector<uint8_t> buf(200000, 0xAB);
        test_lzma_roundtrip(buf.data(), buf.size(), 1, "lzma repeat-200KB L1");
        test_lzma_roundtrip(buf.data(), buf.size(), 5, "lzma repeat-200KB L5");
    }

    std::printf("\n=== LZMA2 chunk format ===\n");

    /* Empty */
    test_lzma2_roundtrip(nullptr, 0, 1, "lzma2 empty L1");

    /* Single byte */
    {
        uint8_t b = 0x77;
        test_lzma2_roundtrip(&b, 1, 1, "lzma2 single-byte L1");
    }

    /* All zeros */
    {
        std::vector<uint8_t> buf(1 << 16, 0);
        test_lzma2_roundtrip(buf.data(), buf.size(), 1, "lzma2 zeros-64K L1");
        test_lzma2_roundtrip(buf.data(), buf.size(), 5, "lzma2 zeros-64K L5");
    }

    /* Repeating pattern */
    {
        std::vector<uint8_t> buf(1 << 16);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = (uint8_t)(i % 5);
        test_lzma2_roundtrip(buf.data(), buf.size(), 1, "lzma2 pattern-mod5-64K L1");
        test_lzma2_roundtrip(buf.data(), buf.size(), 5, "lzma2 pattern-mod5-64K L5");
    }

    /* Multi-block */
    {
        std::vector<uint8_t> buf(200000);
        for (size_t i = 0; i < buf.size(); ++i)
            buf[i] = (uint8_t)(i % 256);
        test_lzma2_roundtrip(buf.data(), buf.size(), 1, "lzma2 sequential-200KB L1");
        test_lzma2_roundtrip(buf.data(), buf.size(), 5, "lzma2 sequential-200KB L5");
    }

    /* Pseudo-random */
    {
        std::vector<uint8_t> buf(1 << 16);
        uint32_t rng = 0xCAFEBABEu;
        for (auto& b : buf) {
            rng = rng * 1664525u + 1013904223u;
            b = (uint8_t)(rng >> 24);
        }
        test_lzma2_roundtrip(buf.data(), buf.size(), 1, "lzma2 random-64K L1");
    }

    if (failures == 0)
        std::printf("\nAll LZMA tests PASSED.\n");
    else
        std::fprintf(stderr, "\n%d LZMA test(s) FAILED.\n", failures);

    return failures == 0 ? 0 : 1;
}
