#include "zstd.hpp"

#include <cstddef>
#include <cstdint>
#include <climits>
#include <cstring>

namespace orot { namespace zstd {

namespace {

struct FrameHeader {
    bool single_segment = false;
    bool checksum = false;
    uint64_t content_size = 0;
    bool has_content_size = false;
    uint32_t dict_id = 0;
    uint64_t window_size = 0;
};

static uint32_t read_le24(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16);
}

static uint32_t read_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t read_le64(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

static bool read_uint(const uint8_t*& p, const uint8_t* end,
                      int bytes, uint64_t& out) noexcept {
    if (bytes == 0) {
        out = 0;
        return true;
    }
    if (p + bytes > end) return false;

    switch (bytes) {
    case 1: out = p[0]; break;
    case 2: out = static_cast<uint64_t>(p[0]) |
                  (static_cast<uint64_t>(p[1]) << 8); break;
    case 4: out = read_le32(p); break;
    case 8: out = read_le64(p); break;
    default: return false;
    }

    p += bytes;
    return true;
}

static bool parse_frame_header(const uint8_t*& p, const uint8_t* end,
                               FrameHeader& header) noexcept {
    if (p >= end) return false;

    const uint8_t descriptor = *p++;
    const int fcs_flag = descriptor >> 6;
    header.single_segment = (descriptor & 0x20) != 0;
    header.checksum = (descriptor & 0x04) != 0;
    const int dict_id_flag = descriptor & 0x03;

    if ((descriptor & 0x18) != 0) return false;

    if (!header.single_segment) {
        if (p >= end) return false;
        const uint8_t window_descriptor = *p++;
        const uint64_t exponent = window_descriptor >> 3;
        const uint64_t mantissa = window_descriptor & 0x07;
        const uint64_t window_base = 1ull << (10 + exponent);
        header.window_size = window_base + (window_base >> 3) * mantissa;
    }

    int dict_bytes = 0;
    if (dict_id_flag == 1) dict_bytes = 1;
    else if (dict_id_flag == 2) dict_bytes = 2;
    else if (dict_id_flag == 3) dict_bytes = 4;

    uint64_t dict_id = 0;
    if (!read_uint(p, end, dict_bytes, dict_id)) return false;
    header.dict_id = static_cast<uint32_t>(dict_id);

    int fcs_bytes = 0;
    if (fcs_flag == 0) fcs_bytes = header.single_segment ? 1 : 0;
    else if (fcs_flag == 1) fcs_bytes = 2;
    else if (fcs_flag == 2) fcs_bytes = 4;
    else fcs_bytes = 8;

    uint64_t content_size = 0;
    if (!read_uint(p, end, fcs_bytes, content_size)) return false;
    if (fcs_flag == 1) content_size += 256;

    header.has_content_size = fcs_bytes != 0;
    header.content_size = content_size;
    if (header.single_segment) header.window_size = content_size;

    return true;
}

static uint64_t xxh64_round(uint64_t acc, uint64_t input) noexcept {
    static constexpr uint64_t prime2 = 14029467366897019727ull;
    static constexpr uint64_t prime1 = 11400714785074694791ull;
    acc += input * prime2;
    acc = (acc << 31) | (acc >> 33);
    acc *= prime1;
    return acc;
}

static uint64_t xxh64_merge_round(uint64_t acc, uint64_t val) noexcept {
    static constexpr uint64_t prime1 = 11400714785074694791ull;
    static constexpr uint64_t prime4 = 9650029242287828579ull;
    val = xxh64_round(0, val);
    acc ^= val;
    acc = acc * prime1 + prime4;
    return acc;
}

static uint64_t xxh64_avalanche(uint64_t h) noexcept {
    static constexpr uint64_t prime2 = 14029467366897019727ull;
    static constexpr uint64_t prime3 = 1609587929392839161ull;
    h ^= h >> 33;
    h *= prime2;
    h ^= h >> 29;
    h *= prime3;
    h ^= h >> 32;
    return h;
}

static uint64_t xxh64(const uint8_t* input, size_t len) noexcept {
    static constexpr uint64_t prime1 = 11400714785074694791ull;
    static constexpr uint64_t prime2 = 14029467366897019727ull;
    static constexpr uint64_t prime3 = 1609587929392839161ull;
    static constexpr uint64_t prime4 = 9650029242287828579ull;
    static constexpr uint64_t prime5 = 2870177450012600261ull;

    const uint8_t* p = input;
    const uint8_t* const end = input + len;
    uint64_t h = 0;

    if (len >= 32) {
        const uint8_t* const limit = end - 32;
        uint64_t v1 = prime1 + prime2;
        uint64_t v2 = prime2;
        uint64_t v3 = 0;
        uint64_t v4 = 0 - prime1;

        do {
            v1 = xxh64_round(v1, read_le64(p)); p += 8;
            v2 = xxh64_round(v2, read_le64(p)); p += 8;
            v3 = xxh64_round(v3, read_le64(p)); p += 8;
            v4 = xxh64_round(v4, read_le64(p)); p += 8;
        } while (p <= limit);

        h = ((v1 << 1) | (v1 >> 63)) +
            ((v2 << 7) | (v2 >> 57)) +
            ((v3 << 12) | (v3 >> 52)) +
            ((v4 << 18) | (v4 >> 46));
        h = xxh64_merge_round(h, v1);
        h = xxh64_merge_round(h, v2);
        h = xxh64_merge_round(h, v3);
        h = xxh64_merge_round(h, v4);
    } else {
        h = prime5;
    }

    h += len;

    while (p + 8 <= end) {
        uint64_t k1 = xxh64_round(0, read_le64(p));
        h ^= k1;
        h = ((h << 27) | (h >> 37)) * prime1 + prime4;
        p += 8;
    }

    if (p + 4 <= end) {
        h ^= static_cast<uint64_t>(read_le32(p)) * prime1;
        h = ((h << 23) | (h >> 41)) * prime2 + prime3;
        p += 4;
    }

    while (p < end) {
        h ^= static_cast<uint64_t>(*p++) * prime5;
        h = ((h << 11) | (h >> 53)) * prime1;
    }

    return xxh64_avalanche(h);
}

} // namespace

int zstd_compress_bound(int src_len) noexcept {
    if (src_len < 0) return -1;

    /*
     * Zstandard's final encoder can choose raw/RLE/compressed blocks. Until the
     * encoder exists, expose a conservative whole-frame bound that is large
     * enough for block headers and incompressible block overhead.
     */
    const int block_overhead = (src_len / 128) + 64;
    if (src_len > INT_MAX - block_overhead) return -1;
    return src_len + block_overhead;
}

int zstd_compress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap,
    int level) noexcept
{
    (void)src;
    (void)src_len;
    (void)dst;
    (void)dst_cap;
    (void)level;
    return -1;
}

int zstd_decompress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap) noexcept
{
    if (src_len < 0 || dst_cap < 0) return -1;
    if ((src_len > 0 && src == nullptr) || (dst_cap > 0 && dst == nullptr)) return -1;

    const uint8_t* p = src;
    const uint8_t* const end = src + src_len;
    uint8_t empty_output = 0;
    uint8_t* const dst_base = dst != nullptr ? dst : &empty_output;

    if (p + 4 > end) return -1;
    const uint32_t magic = read_le32(p);
    p += 4;
    if (magic != ZSTD_MAGIC) return -1;

    FrameHeader header;
    if (!parse_frame_header(p, end, header)) return -1;

    uint8_t* out = dst_base;
    uint8_t* const out_end = dst_base + dst_cap;
    uint64_t produced = 0;
    bool last_block = false;

    while (!last_block) {
        if (p + 3 > end) return -1;
        const uint32_t block_header = read_le24(p);
        p += 3;

        last_block = (block_header & 0x01u) != 0;
        const int block_type = static_cast<int>((block_header >> 1) & 0x03u);
        const uint32_t block_size = block_header >> 3;

        if (block_size > ZSTD_BLOCK_MAX_SIZE) return -1;

        if (block_type == 0) {
            if (p + block_size > end) return -1;
            if (out_end - out < static_cast<ptrdiff_t>(block_size)) return -2;
            if (block_size > 0) {
                std::memcpy(out, p, block_size);
                out += block_size;
                p += block_size;
            }
            produced += block_size;
        } else if (block_type == 1) {
            if (p >= end) return -1;
            if (out_end - out < static_cast<ptrdiff_t>(block_size)) return -2;
            if (block_size > 0) {
                std::memset(out, *p, block_size);
                out += block_size;
            }
            ++p;
            produced += block_size;
        } else {
            return -1;
        }

        if (header.has_content_size && produced > header.content_size) return -1;
        if (produced > static_cast<uint64_t>(INT_MAX)) return -1;
    }

    if (header.has_content_size && produced != header.content_size) return -1;

    if (header.checksum) {
        if (p + 4 > end) return -1;
        const uint32_t expected = read_le32(p);
        p += 4;
        const uint32_t actual = static_cast<uint32_t>(
            xxh64(dst_base, static_cast<size_t>(produced)) & 0xFFFFFFFFu);
        if (actual != expected) return -3;
    }

    if (p != end) return -1;
    return static_cast<int>(produced);
}

} } /* namespace orot::zstd */
