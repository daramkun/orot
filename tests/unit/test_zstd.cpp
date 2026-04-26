/*
 * Zstd frame/block parser tests.
 */
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "orot/zstd.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

#define CHECK_EQ(actual, expected, msg) do { \
    auto actual_value = (actual); \
    auto expected_value = (expected); \
    if (actual_value != expected_value) { \
        std::fprintf(stderr, "FAIL: %s (line %d): actual=%lld expected=%lld\n", \
                     msg, __LINE__, static_cast<long long>(actual_value), \
                     static_cast<long long>(expected_value)); \
        ++failures; \
    } \
} while (0)

static void write_le24(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
}

static void write_le32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

static std::vector<uint8_t> make_frame(
    const std::vector<uint8_t>& payload,
    int block_type,
    bool checksum = false,
    uint32_t checksum_value = 0)
{
    std::vector<uint8_t> frame;
    write_le32(frame, 0xFD2FB528u);
    frame.push_back(static_cast<uint8_t>(0x20u | (checksum ? 0x04u : 0u)));
    frame.push_back(static_cast<uint8_t>(payload.size()));

    const uint32_t block_header =
        1u | (static_cast<uint32_t>(block_type) << 1) |
        (static_cast<uint32_t>(payload.size()) << 3);
    write_le24(frame, block_header);

    if (block_type == 0) {
        frame.insert(frame.end(), payload.begin(), payload.end());
    } else if (block_type == 1) {
        frame.push_back(payload.empty() ? 0 : payload[0]);
    }

    if (checksum)
        write_le32(frame, checksum_value);

    return frame;
}

static std::vector<uint8_t> make_windowed_raw_frame(
    const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame;
    write_le32(frame, 0xFD2FB528u);
    frame.push_back(0x00);
    frame.push_back(0x00); /* 1 KB window descriptor */
    write_le24(frame, 1u | (static_cast<uint32_t>(payload.size()) << 3));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

static std::vector<uint8_t> make_dict_two_byte_fcs_raw_frame(
    const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> frame;
    write_le32(frame, 0xFD2FB528u);
    frame.push_back(0x61); /* single segment + 2-byte FCS + 1-byte dict id */
    frame.push_back(0x7B);
    const uint32_t stored_size = static_cast<uint32_t>(payload.size() - 256);
    frame.push_back(static_cast<uint8_t>(stored_size & 0xFFu));
    frame.push_back(static_cast<uint8_t>((stored_size >> 8) & 0xFFu));
    write_le24(frame, 1u | (static_cast<uint32_t>(payload.size()) << 3));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

static void expect_decompress(
    const std::vector<uint8_t>& frame,
    const std::vector<uint8_t>& expected,
    const char* label)
{
    std::vector<uint8_t> out(expected.size() + 16, 0);
    const int rc = orot_zstd_decompress(
        frame.data(), static_cast<int>(frame.size()),
        out.data(), static_cast<int>(out.size()));
    CHECK_EQ(rc, static_cast<int>(expected.size()), label);
    if (rc == static_cast<int>(expected.size()) && !expected.empty()) {
        CHECK(std::memcmp(out.data(), expected.data(), expected.size()) == 0, label);
    }
}

static void expect_decompress_error(
    const std::vector<uint8_t>& frame,
    int dst_cap,
    int expected,
    const char* label)
{
    std::vector<uint8_t> out(static_cast<size_t>(dst_cap > 0 ? dst_cap : 1), 0);
    const int rc = orot_zstd_decompress(
        frame.data(), static_cast<int>(frame.size()),
        out.data(), dst_cap);
    CHECK(rc == expected, label);
}

int main() {
    CHECK(orot_zstd_compress_bound(0) >= 0, "empty bound is valid");
    CHECK(orot_zstd_compress_bound(1024) >= 1024, "bound covers input size");
    CHECK(orot_zstd_compress_bound(-1) == -1, "negative bound rejected");

    const uint8_t src[] = { 'z', 's', 't', 'd' };
    std::vector<uint8_t> compressed(128);

    CHECK(orot_zstd_compress(src, 4, compressed.data(),
                             static_cast<int>(compressed.size()), 1) == -1,
          "compressor remains unsupported until encoder stage");

    {
        std::vector<uint8_t> payload = { 'h', 'e', 'l', 'l', 'o' };
        expect_decompress(make_frame(payload, 0), payload, "raw block frame");
    }
    {
        std::vector<uint8_t> payload(17, 'A');
        expect_decompress(make_frame(payload, 1), payload, "RLE block frame");
    }
    {
        std::vector<uint8_t> payload = { 'a', 'b', 'c' };
        expect_decompress(make_windowed_raw_frame(payload), payload,
                          "window descriptor raw frame");
    }
    {
        std::vector<uint8_t> payload(300);
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<uint8_t>(i);
        std::vector<uint8_t> frame = make_dict_two_byte_fcs_raw_frame(payload);
        expect_decompress_error(frame, 512, -1,
                                "dictionary id frame rejected");
    }
    {
        std::vector<uint8_t> frame = make_frame({}, 0, true, 0x51D8E999u);
        expect_decompress(frame, {}, "empty raw frame with checksum");
    }
    {
        std::vector<uint8_t> frame = make_frame({}, 0, true, 0);
        expect_decompress_error(frame, 0, -3, "checksum mismatch");
    }
    {
        std::vector<uint8_t> payload = { 1, 2, 3, 4 };
        std::vector<uint8_t> frame = make_frame(payload, 0);
        expect_decompress_error(frame, 3, -2, "raw block dst too small");
    }
    {
        std::vector<uint8_t> frame = make_frame({ 'x' }, 2);
        expect_decompress_error(frame, 16, -1, "compressed block unsupported");
    }
    {
        std::vector<uint8_t> frame = make_frame({ 'x' }, 3);
        expect_decompress_error(frame, 16, -1, "reserved block rejected");
    }
    {
        std::vector<uint8_t> frame = make_frame({ 'x' }, 0);
        frame[4] = 0x08;
        expect_decompress_error(frame, 16, -1, "reserved descriptor bit rejected");
    }
    {
        std::vector<uint8_t> frame = make_frame({ 'x', 'y' }, 0);
        frame.pop_back();
        expect_decompress_error(frame, 16, -1, "truncated raw block rejected");
    }
    {
        std::vector<uint8_t> frame = make_frame({ 'x' }, 0);
        frame.push_back(0);
        expect_decompress_error(frame, 16, -1, "trailing bytes rejected");
    }

    /* ── Cross-compatibility samples (libzstd -1 output) ─────────────────── */

    /* "aaa..." 309 bytes → compressed block (1 sequence, predefined FSE) */
    {
        static const uint8_t aaa_zst[] = {
            0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x48, 0x4d, 0x00, 0x00, 0x10, 0x61, 0x61,
            0x01, 0x00, 0x30, 0x2a, 0xc0, 0x02, 0x32, 0x2a, 0xdb, 0xbe
        };
        std::vector<uint8_t> expected(309, 'a');
        std::vector<uint8_t> out(400, 0);
        int rc = orot_zstd_decompress(aaa_zst, static_cast<int>(sizeof(aaa_zst)),
                                      out.data(), static_cast<int>(out.size()));
        CHECK_EQ(rc, 309, "aaa 309 compressed: output size");
        if (rc == 309) {
            CHECK(std::memcmp(out.data(), expected.data(), 309) == 0,
                  "aaa 309 compressed: content");
        }
    }

    /* "abcdef"*50 = 300 bytes → compressed block */
    {
        static const uint8_t rpt_zst[] = {
            0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x48, 0x6d, 0x00, 0x00, 0x30, 0x61, 0x62,
            0x63, 0x64, 0x65, 0x66, 0x01, 0x00, 0x23, 0x51, 0x4b, 0x11, 0x73, 0x97,
            0x5d, 0xff
        };
        std::vector<uint8_t> expected;
        for (int i = 0; i < 50; ++i) {
            const uint8_t pat[] = {'a','b','c','d','e','f'};
            expected.insert(expected.end(), pat, pat + 6);
        }
        std::vector<uint8_t> out(400, 0);
        int rc = orot_zstd_decompress(rpt_zst, static_cast<int>(sizeof(rpt_zst)),
                                      out.data(), static_cast<int>(out.size()));
        CHECK_EQ(rc, 300, "abcdef*50 compressed: output size");
        if (rc == 300) {
            CHECK(std::memcmp(out.data(), expected.data(), 300) == 0,
                  "abcdef*50 compressed: content");
        }
    }

    /* "The quick brown fox..." * 20 = 900 bytes → compressed + checksum */
    {
        static const uint8_t text_zst[] = {
            0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x48, 0xb5, 0x01, 0x00, 0xd4, 0x02, 0x54,
            0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6b, 0x20, 0x62, 0x72, 0x6f,
            0x77, 0x6e, 0x20, 0x66, 0x6f, 0x78, 0x20, 0x6a, 0x75, 0x6d, 0x70, 0x73,
            0x20, 0x6f, 0x76, 0x65, 0x72, 0x20, 0x74, 0x68, 0x65, 0x20, 0x6c, 0x61,
            0x7a, 0x79, 0x20, 0x64, 0x6f, 0x67, 0x2e, 0x20, 0x01, 0x00, 0xa5, 0x0a,
            0x2b, 0x55, 0x06, 0x59, 0x13, 0xfa, 0x85
        };
        std::vector<uint8_t> expected;
        const char* sentence = "The quick brown fox jumps over the lazy dog. ";
        for (int i = 0; i < 20; ++i) {
            for (const char* c = sentence; *c; ++c)
                expected.push_back(static_cast<uint8_t>(*c));
        }
        std::vector<uint8_t> out(1024, 0);
        int rc = orot_zstd_decompress(text_zst, static_cast<int>(sizeof(text_zst)),
                                      out.data(), static_cast<int>(out.size()));
        CHECK_EQ(rc, 900, "text*20 compressed: output size");
        if (rc == 900) {
            CHECK(std::memcmp(out.data(), expected.data(), 900) == 0,
                  "text*20 compressed: content");
        }
    }

    /* bytes(range(256))*4 = 1024 bytes → compressed with checksum */
    {
        static const uint8_t seq_zst[] = {
            0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x48, 0x55, 0x08, 0x00, 0x04, 0x10, 0x00,
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
            0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
            0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24,
            0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
            0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c,
            0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
            0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54,
            0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60,
            0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c,
            0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
            0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84,
            0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90,
            0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c,
            0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
            0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4,
            0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0,
            0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc,
            0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8,
            0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4,
            0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0,
            0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc,
            0xfd, 0xfe, 0xff, 0x01, 0x00, 0x00, 0xfd, 0x06, 0xaa, 0x35, 0x05, 0x57,
            0xdf, 0xe4, 0x8f
        };
        std::vector<uint8_t> expected;
        for (int rep = 0; rep < 4; ++rep)
            for (int b = 0; b < 256; ++b)
                expected.push_back(static_cast<uint8_t>(b));
        std::vector<uint8_t> out(1200, 0);
        int rc = orot_zstd_decompress(seq_zst, static_cast<int>(sizeof(seq_zst)),
                                      out.data(), static_cast<int>(out.size()));
        CHECK_EQ(rc, 1024, "bytes(range(256))*4 compressed: output size");
        if (rc == 1024) {
            CHECK(std::memcmp(out.data(), expected.data(), 1024) == 0,
                  "bytes(range(256))*4 compressed: content");
        }
    }

    /* ── Skippable frame handling ─────────────────────────────────────────── */
    {
        /* Skippable frame (8 bytes: magic + 4-byte size=0) followed by raw data */
        std::vector<uint8_t> skip_frame;
        write_le32(skip_frame, 0x184D2A50u); /* skippable magic */
        write_le32(skip_frame, 0u);           /* skip 0 bytes */
        /* Append a valid raw block frame */
        std::vector<uint8_t> payload = { 'x', 'y', 'z' };
        std::vector<uint8_t> data_frame = make_frame(payload, 0);
        skip_frame.insert(skip_frame.end(), data_frame.begin(), data_frame.end());
        expect_decompress(skip_frame, payload, "skippable frame then data frame");
    }
    {
        /* Truncated skippable frame (size field says 5 bytes but none follow) */
        std::vector<uint8_t> bad;
        write_le32(bad, 0x184D2A55u);
        write_le32(bad, 5u); /* claims 5 bytes but none follow */
        expect_decompress_error(bad, 16, -1, "truncated skippable frame");
    }

    /* ── Dictionary frame rejected ───────────────────────────────────────── */
    {
        /* Frame with dict_id != 0 should return -1 */
        std::vector<uint8_t> frame;
        write_le32(frame, 0xFD2FB528u);
        frame.push_back(0x23u); /* single_segment=1, dict_id_flag=3 (4-byte dict id) */
        frame.push_back(0x05u); /* FCS = 5 bytes (1-byte field for single_seg, value=5) */
        /* dict_id: 4 bytes non-zero */
        write_le32(frame, 1u);
        /* FCS */
        frame.push_back(5u);
        /* block: raw, 5 bytes */
        write_le24(frame, 1u | (5u << 3));
        for (int i = 0; i < 5; ++i) frame.push_back(static_cast<uint8_t>(i));
        expect_decompress_error(frame, 64, -1, "dictionary frame rejected");
    }

    /* ── Compressed block dst too small ─────────────────────────────────── */
    {
        static const uint8_t aaa_zst[] = {
            0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x48, 0x4d, 0x00, 0x00, 0x10, 0x61, 0x61,
            0x01, 0x00, 0x30, 0x2a, 0xc0, 0x02, 0x32, 0x2a, 0xdb, 0xbe
        };
        std::vector<uint8_t> out(100, 0);
        int rc = orot_zstd_decompress(aaa_zst, static_cast<int>(sizeof(aaa_zst)),
                                      out.data(), 100);
        CHECK_EQ(rc, -2, "compressed block dst too small returns -2");
    }

    if (failures == 0)
        std::printf("All Zstd frame/block parser tests PASSED.\n");
    else
        std::fprintf(stderr, "%d Zstd test(s) FAILED.\n", failures);

    return failures == 0 ? 0 : 1;
}
