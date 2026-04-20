/*
 * LZ4 block and frame roundtrip tests.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <numeric>

#include "orot/lz4.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

/* ── Block roundtrip ─────────────────────────────────────────────────────── */

static void test_block_roundtrip(
    const uint8_t* data, int len,
    int level, const char* label)
{
    int bound = orot_lz4_compress_bound(len);
    std::vector<uint8_t> compressed(bound);
    std::vector<uint8_t> decompressed(len + 256, 0);

    int clen = orot_lz4_compress(data, len, compressed.data(), bound, level);
    CHECK(clen > 0 || len == 0, label);

    int dlen = orot_lz4_decompress(
        compressed.data(), clen,
        decompressed.data(), len + 256);
    CHECK(dlen == len, label);
    CHECK(std::memcmp(data, decompressed.data(), static_cast<size_t>(len)) == 0, label);

    if (failures == 0 || dlen == len)
        std::printf("PASS block %-40s  in=%6d  out=%6d  ratio=%.2f\n",
            label, len, clen, len > 0 ? (double)len / (double)clen : 0.0);
}

/* ── Frame roundtrip ─────────────────────────────────────────────────────── */

static void test_frame_roundtrip(
    const uint8_t* data, int len,
    int level, const char* label)
{
    int bound = orot_lz4f_compress_bound(len);
    std::vector<uint8_t> compressed(bound);
    std::vector<uint8_t> decompressed(len + 256, 0);

    int clen = orot_lz4f_compress(data, len, compressed.data(), bound, level);
    CHECK(clen > 0, label);

    int dlen = orot_lz4f_decompress(
        compressed.data(), clen,
        decompressed.data(), len + 256);
    CHECK(dlen == len, label);
    CHECK(std::memcmp(data, decompressed.data(), static_cast<size_t>(len)) == 0, label);

    if (failures == 0 || dlen == len)
        std::printf("PASS frame %-40s  in=%6d  out=%6d  ratio=%.2f\n",
            label, len, clen, len > 0 ? (double)len / (double)clen : 0.0);
}

int main() {
    /* ── Test data ──────────────────────────────────────────────────────── */

    /* All zeros */
    std::vector<uint8_t> zeros(1 * 1024 * 1024, 0);

    /* Repeating pattern */
    std::vector<uint8_t> repeat(65536);
    for (size_t i = 0; i < repeat.size(); ++i)
        repeat[i] = static_cast<uint8_t>(i % 251);

    /* Sequential bytes (low entropy) */
    std::vector<uint8_t> seq(100000);
    std::iota(seq.begin(), seq.end(), uint8_t(0));

    /* Pseudo-random (high entropy) */
    std::vector<uint8_t> rnd(128 * 1024);
    uint32_t lcg = 0xDEADBEEF;
    for (auto& b : rnd) {
        lcg = lcg * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(lcg >> 24);
    }

    /* Short text */
    const char* text_str = "Hello, LZ4! This is a test of the LZ4 compression "
                           "algorithm. LZ4 LZ4 LZ4 repetition repetition repetition.";
    std::vector<uint8_t> text(
        reinterpret_cast<const uint8_t*>(text_str),
        reinterpret_cast<const uint8_t*>(text_str) + std::strlen(text_str));

    /* Single byte */
    std::vector<uint8_t> single = { 0x42 };

    /* Small: 4 bytes */
    std::vector<uint8_t> four = { 'a', 'b', 'c', 'd' };

    /* ── Block tests ────────────────────────────────────────────────────── */

    for (int lvl : {1, 3, 6, 9}) {
        char lbl[64];

        std::snprintf(lbl, sizeof(lbl), "zeros-1MB L%d", lvl);
        test_block_roundtrip(zeros.data(), static_cast<int>(zeros.size()), lvl, lbl);

        std::snprintf(lbl, sizeof(lbl), "repeat-64KB L%d", lvl);
        test_block_roundtrip(repeat.data(), static_cast<int>(repeat.size()), lvl, lbl);

        std::snprintf(lbl, sizeof(lbl), "seq-100KB L%d", lvl);
        test_block_roundtrip(seq.data(), static_cast<int>(seq.size()), lvl, lbl);

        std::snprintf(lbl, sizeof(lbl), "random-128KB L%d", lvl);
        test_block_roundtrip(rnd.data(), static_cast<int>(rnd.size()), lvl, lbl);

        std::snprintf(lbl, sizeof(lbl), "text L%d", lvl);
        test_block_roundtrip(text.data(), static_cast<int>(text.size()), lvl, lbl);

        std::snprintf(lbl, sizeof(lbl), "single-byte L%d", lvl);
        test_block_roundtrip(single.data(), static_cast<int>(single.size()), lvl, lbl);

        std::snprintf(lbl, sizeof(lbl), "four-bytes L%d", lvl);
        test_block_roundtrip(four.data(), static_cast<int>(four.size()), lvl, lbl);
    }

    /* ── Frame tests ────────────────────────────────────────────────────── */

    for (int lvl : {1, 6, 9}) {
        char lbl[64];

        std::snprintf(lbl, sizeof(lbl), "zeros-1MB L%d", lvl);
        test_frame_roundtrip(zeros.data(), static_cast<int>(zeros.size()), lvl, lbl);

        std::snprintf(lbl, sizeof(lbl), "repeat-64KB L%d", lvl);
        test_frame_roundtrip(repeat.data(), static_cast<int>(repeat.size()), lvl, lbl);

        std::snprintf(lbl, sizeof(lbl), "random-128KB L%d", lvl);
        test_frame_roundtrip(rnd.data(), static_cast<int>(rnd.size()), lvl, lbl);

        std::snprintf(lbl, sizeof(lbl), "text L%d", lvl);
        test_frame_roundtrip(text.data(), static_cast<int>(text.size()), lvl, lbl);
    }

    /* ── Edge: corrupt decompress ───────────────────────────────────────── */
    {
        std::vector<uint8_t> buf(64);
        int r = orot_lz4_decompress(
            reinterpret_cast<const uint8_t*>("garbage!!"), 9,
            buf.data(), static_cast<int>(buf.size()));
        /* Should return error (≤0) or valid decode — not crash */
        (void)r;
        std::printf("PASS block corrupt-input (no crash)\n");
    }

    /* ── Edge: empty frame ───────────────────────────────────────────────── */
    {
        std::vector<uint8_t> empty_src;
        int bound = orot_lz4f_compress_bound(0);
        std::vector<uint8_t> cmp(bound);
        std::vector<uint8_t> out(256);

        int clen = orot_lz4f_compress(nullptr, 0, cmp.data(), bound, 1);
        if (clen > 0) {
            int dlen = orot_lz4f_decompress(cmp.data(), clen, out.data(), 256);
            CHECK(dlen == 0, "empty-frame");
            std::printf("PASS frame empty-input\n");
        }
    }

    if (failures == 0) {
        std::printf("\nAll LZ4 tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
