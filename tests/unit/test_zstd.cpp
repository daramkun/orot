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
    CHECK(rc == static_cast<int>(expected.size()), label);
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
        expect_decompress(make_dict_two_byte_fcs_raw_frame(payload), payload,
                          "dictionary id and two-byte content size frame");
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

    if (failures == 0)
        std::printf("All Zstd frame/block parser tests PASSED.\n");
    else
        std::fprintf(stderr, "%d Zstd test(s) FAILED.\n", failures);

    return failures == 0 ? 0 : 1;
}
