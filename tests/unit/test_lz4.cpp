/*
 * LZ4 block/frame roundtrip + error-path tests.
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <numeric>
#include <algorithm>

#include "orot/lz4.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static std::vector<uint8_t> make_random(size_t len, uint32_t seed = 0xDEADBEEFu) {
    std::vector<uint8_t> out(len);
    for (auto& b : out) {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(seed >> 24);
    }
    return out;
}

static std::vector<uint8_t> make_pattern(size_t len, uint8_t mod) {
    std::vector<uint8_t> out(len);
    for (size_t i = 0; i < len; ++i)
        out[i] = static_cast<uint8_t>(i % mod);
    return out;
}

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int count_frame_blocks(const uint8_t* src, int len) {
    if (len < 11) return -1;
    const uint8_t* p = src + 7; /* magic + FLG + BD + HC */
    const uint8_t* end = src + len;
    int blocks = 0;

    while (p + 4 <= end) {
        uint32_t field = read_le32(p);
        p += 4;
        if (field == 0) return blocks;
        int block_len = static_cast<int>(field & 0x7FFFFFFFu);
        if (block_len < 0 || p + block_len > end) return -1;
        p += block_len;
        ++blocks;
    }
    return -1;
}

static void test_block_roundtrip(
    const uint8_t* data, int len,
    int level, const char* label)
{
    const int bound = orot_lz4_compress_bound(len);
    std::vector<uint8_t> compressed(static_cast<size_t>(bound));
    std::vector<uint8_t> decompressed(static_cast<size_t>(len + 256), 0);

    const int clen = orot_lz4_compress(data, len, compressed.data(), bound, level);
    CHECK(clen > 0, label);
    if (clen <= 0) return;

    const int dlen = orot_lz4_decompress(
        compressed.data(), clen,
        decompressed.data(), len + 256);
    CHECK(dlen == len, label);
    if (dlen == len && len > 0)
        CHECK(std::memcmp(data, decompressed.data(), static_cast<size_t>(len)) == 0, label);

    if (failures == 0 || dlen == len)
        std::printf("PASS block %-42s  in=%7d  out=%7d  ratio=%.2f\n",
                    label, len, clen, len > 0 ? (double)len / (double)clen : 0.0);
}

static void test_frame_roundtrip(
    const uint8_t* data, int len,
    int level, const char* label)
{
    const int bound = orot_lz4f_compress_bound(len);
    std::vector<uint8_t> compressed(static_cast<size_t>(bound));
    std::vector<uint8_t> decompressed(static_cast<size_t>(len + 256), 0);

    const int clen = orot_lz4f_compress(data, len, compressed.data(), bound, level);
    CHECK(clen > 0, label);
    if (clen <= 0) return;

    const int dlen = orot_lz4f_decompress(
        compressed.data(), clen,
        decompressed.data(), len + 256);
    CHECK(dlen == len, label);
    if (dlen == len && len > 0)
        CHECK(std::memcmp(data, decompressed.data(), static_cast<size_t>(len)) == 0, label);

    if (failures == 0 || dlen == len)
        std::printf("PASS frame %-42s  in=%7d  out=%7d  ratio=%.2f\n",
                    label, len, clen, len > 0 ? (double)len / (double)clen : 0.0);
}

static void expect_block_decompress_error(
    const uint8_t* src, int src_len, int dst_cap, int expected, const char* label)
{
    std::vector<uint8_t> dst(static_cast<size_t>(std::max(dst_cap, 1)), 0);
    const int rc = orot_lz4_decompress(src, src_len, dst.data(), dst_cap);
    CHECK(rc == expected, label);
}

static void expect_frame_decompress_error(
    const uint8_t* src, int src_len, int dst_cap, int expected, const char* label)
{
    std::vector<uint8_t> dst(static_cast<size_t>(std::max(dst_cap, 1)), 0);
    const int rc = orot_lz4f_decompress(src, src_len, dst.data(), dst_cap);
    CHECK(rc == expected, label);
}

int main() {
    /* Raw block roundtrip */
    {
        std::vector<int> sizes = { 0, 1, 4, 5, 15, 16, 19, 20, 255, 256, 257, 65535, 65536 };
        for (int lvl : {1, 6, 9}) {
            for (int n : sizes) {
                auto seq = make_pattern(static_cast<size_t>(n), 251);
                char label[64];
                std::snprintf(label, sizeof(label), "pattern-%d L%d", n, lvl);
                test_block_roundtrip(seq.data(), n, lvl, label);
            }
        }
    }
    {
        auto zeros = std::vector<uint8_t>(1 * 1024 * 1024, 0);
        auto random = make_random(128 * 1024);
        auto text = make_pattern(100000, 17);

        for (int lvl : {1, 3, 6, 9}) {
            char label[64];
            std::snprintf(label, sizeof(label), "zeros-1MB L%d", lvl);
            test_block_roundtrip(zeros.data(), static_cast<int>(zeros.size()), lvl, label);

            std::snprintf(label, sizeof(label), "random-128KB L%d", lvl);
            test_block_roundtrip(random.data(), static_cast<int>(random.size()), lvl, label);

            std::snprintf(label, sizeof(label), "pattern-100KB L%d", lvl);
            test_block_roundtrip(text.data(), static_cast<int>(text.size()), lvl, label);
        }
    }

    /* Block error handling */
    {
        const uint8_t empty_block[] = { 0x00 };
        std::vector<uint8_t> out(16, 0);
        const int dlen = orot_lz4_decompress(empty_block, 1, out.data(), static_cast<int>(out.size()));
        CHECK(dlen == 0, "block empty stream decompresses to zero");
    }
    {
        const uint8_t bad_offset[] = { 0x00, 0x00, 0x00 };
        expect_block_decompress_error(bad_offset, 3, 32, -1, "block rejects zero offset");
    }
    {
        const uint8_t truncated_literals[] = { 0xF0, 0x10 };
        expect_block_decompress_error(truncated_literals, 2, 64, -1, "block rejects truncated literal run");
    }
    {
        auto data = make_pattern(4096, 7);
        const int bound = orot_lz4_compress_bound(static_cast<int>(data.size()));
        std::vector<uint8_t> compressed(static_cast<size_t>(bound));
        const int clen = orot_lz4_compress(data.data(), static_cast<int>(data.size()),
                                           compressed.data(), bound, 6);
        CHECK(clen > 0, "setup valid LZ4 block");
        if (clen > 0) {
            expect_block_decompress_error(compressed.data(), clen, 32, -2,
                                          "block reports dst too small");
        }
    }

    /* Frame roundtrip */
    {
        auto zeros = std::vector<uint8_t>(1 * 1024 * 1024, 0);
        auto random = make_random(512 * 1024);
        auto multi_block = make_pattern((4 * 1024 * 1024) + 123, 251);

        for (int lvl : {1, 6, 9}) {
            char label[64];
            std::snprintf(label, sizeof(label), "zeros-1MB L%d", lvl);
            test_frame_roundtrip(zeros.data(), static_cast<int>(zeros.size()), lvl, label);

            std::snprintf(label, sizeof(label), "random-512KB L%d", lvl);
            test_frame_roundtrip(random.data(), static_cast<int>(random.size()), lvl, label);

            std::snprintf(label, sizeof(label), "4MB+123 pattern L%d", lvl);
            test_frame_roundtrip(multi_block.data(), static_cast<int>(multi_block.size()), lvl, label);
        }

        const int bound = orot_lz4f_compress_bound(static_cast<int>(multi_block.size()));
        std::vector<uint8_t> frame(static_cast<size_t>(bound));
        const int clen = orot_lz4f_compress(multi_block.data(), static_cast<int>(multi_block.size()),
                                            frame.data(), bound, 1);
        CHECK(clen > 0, "setup multi-block frame");
        if (clen > 0) {
            CHECK(count_frame_blocks(frame.data(), clen) == 2,
                  "frame splits data larger than 4MB into multiple blocks");
        }
    }
    {
        const int bound = orot_lz4f_compress_bound(0);
        std::vector<uint8_t> compressed(static_cast<size_t>(bound));
        std::vector<uint8_t> decompressed(32, 0);
        const int clen = orot_lz4f_compress(nullptr, 0, compressed.data(), bound, 1);
        CHECK(clen > 0, "frame empty input compresses");
        if (clen > 0) {
            const int dlen = orot_lz4f_decompress(compressed.data(), clen,
                                                  decompressed.data(), static_cast<int>(decompressed.size()));
            CHECK(dlen == 0, "frame empty input decompresses to zero");
        }
    }

    /* Frame structure and error handling */
    {
        auto random = make_random(1 * 1024 * 1024, 0xCAFEBABEu);
        const int bound = orot_lz4f_compress_bound(static_cast<int>(random.size()));
        std::vector<uint8_t> frame(static_cast<size_t>(bound));
        const int clen = orot_lz4f_compress(random.data(), static_cast<int>(random.size()),
                                            frame.data(), bound, 1);
        CHECK(clen > 0, "setup random frame");
        if (clen > 0) {
            CHECK((read_le32(frame.data() + 7) & 0x80000000u) != 0,
                  "frame stores incompressible first block uncompressed");

            expect_frame_decompress_error(frame.data(), clen, 32, -2,
                                          "frame reports dst too small");

            std::vector<uint8_t> broken = frame;
            broken[6] ^= 0xFF;
            expect_frame_decompress_error(broken.data(), clen,
                                          static_cast<int>(random.size()) + 16, -1,
                                          "frame rejects header checksum corruption");

            broken = frame;
            broken[clen - 1] ^= 0x01;
            expect_frame_decompress_error(broken.data(), clen,
                                          static_cast<int>(random.size()) + 16, -3,
                                          "frame rejects content checksum corruption");

            expect_frame_decompress_error(frame.data(), clen - 1,
                                          static_cast<int>(random.size()) + 16, -1,
                                          "frame rejects truncated valid stream");
        }
    }

    if (failures == 0) {
        std::printf("\nAll LZ4 tests passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
