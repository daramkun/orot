#include "brotli_compress.hpp"
#include "brotli_bit.hpp"

#include <algorithm>
#include <cstring>

namespace orot { namespace brotli {

static constexpr size_t kMaxMetaBlockSize = 1u << 24;

size_t brotli_compress_bound(size_t src_len) noexcept {
    size_t blocks = (src_len + kMaxMetaBlockSize - 1) / kMaxMetaBlockSize;
    return src_len + 16 + blocks * 8;
}

static bool write_wbits(BitWriter& bw, int lgwin) noexcept {
    switch (lgwin) {
    case 10: return bw.write_bits(0b0100001u, 7);
    case 11: return bw.write_bits(0b0110001u, 7);
    case 12: return bw.write_bits(0b1000001u, 7);
    case 13: return bw.write_bits(0b1010001u, 7);
    case 14: return bw.write_bits(0b1100001u, 7);
    case 15: return bw.write_bits(0b1110001u, 7);
    case 16: return bw.write_bits(0u, 1);
    case 17: return bw.write_bits(0b0000001u, 7);
    case 18: return bw.write_bits(0b0011u, 4);
    case 19: return bw.write_bits(0b0101u, 4);
    case 20: return bw.write_bits(0b0111u, 4);
    case 21: return bw.write_bits(0b1001u, 4);
    case 22: return bw.write_bits(0b1011u, 4);
    case 23: return bw.write_bits(0b1101u, 4);
    case 24: return bw.write_bits(0b1111u, 4);
    default: return false;
    }
}

static bool write_meta_len(BitWriter& bw, size_t len) noexcept {
    if (len == 0 || len > kMaxMetaBlockSize) return false;
    size_t encoded = len - 1;
    int mnibbles = (encoded <= 0xFFFFu) ? 4 :
                   (encoded <= 0xFFFFFu) ? 5 : 6;
    uint32_t mnibbles_code = (mnibbles == 4) ? 0u :
                             (mnibbles == 5) ? 1u : 2u;
    return bw.write_bits(mnibbles_code, 2)
        && bw.write_bits((uint32_t)encoded, mnibbles * 4);
}

static int alphabet_bits(int alphabet_size) noexcept {
    int bits = 0;
    int limit = 1;
    while (limit < alphabet_size) {
        limit <<= 1;
        ++bits;
    }
    return bits;
}

static bool write_simple_prefix_code(
    BitWriter& bw,
    int alphabet_size,
    const uint16_t* symbols,
    int nsym) noexcept
{
    if (nsym < 1 || nsym > 4)
        return false;
    int abits = alphabet_bits(alphabet_size);
    if (!bw.write_bits(1, 2) ||
        !bw.write_bits(static_cast<uint32_t>(nsym - 1), 2))
        return false;
    for (int i = 0; i < nsym; ++i)
        if (!bw.write_bits(symbols[i], abits))
            return false;
    if (nsym == 4)
        return bw.write_bits(0, 1);
    return true;
}

static bool command_symbol_for_insert(size_t len, uint16_t& symbol) noexcept {
    static const uint16_t kBase[24] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 10, 14, 18, 26, 34, 50, 66,
        98, 130, 194, 322, 578, 1090, 2114, 6210
    };
    static const uint8_t kExtra[24] = {
        0, 0, 0, 0, 0, 0, 1, 2,
        0, 1, 2, 3, 3, 4, 4, 5,
        5, 6, 7, 8, 9, 10, 12, 14
    };
    static const uint8_t kInsertBase[11] = {
        0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16
    };
    for (int code = 0; code < 24; ++code) {
        size_t count = static_cast<size_t>(1u) << kExtra[code];
        if (len < kBase[code] || len >= kBase[code] + count)
            continue;
        for (int group = 0; group < 11; ++group) {
            if (code < kInsertBase[group] || code >= kInsertBase[group] + 8)
                continue;
            symbol = static_cast<uint16_t>(
                group * 64 + ((code - kInsertBase[group]) << 3));
            return true;
        }
    }
    return false;
}

static bool write_insert_extra(BitWriter& bw, uint16_t symbol, size_t len) noexcept {
    static const uint16_t kBase[24] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 10, 14, 18, 26, 34, 50, 66,
        98, 130, 194, 322, 578, 1090, 2114, 6210
    };
    static const uint8_t kExtra[24] = {
        0, 0, 0, 0, 0, 0, 1, 2,
        0, 1, 2, 3, 3, 4, 4, 5,
        5, 6, 7, 8, 9, 10, 12, 14
    };
    static const uint8_t kInsertBase[11] = {
        0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16
    };
    int group = symbol >> 6;
    int low = symbol & 63;
    int code = kInsertBase[group] + ((low >> 3) & 7);
    return bw.write_bits(static_cast<uint32_t>(len - kBase[code]), kExtra[code]);
}

static int collect_literals(
    const uint8_t* src,
    size_t src_len,
    uint16_t* symbols) noexcept
{
    int count = 0;
    bool seen[256];
    std::memset(seen, 0, sizeof(seen));
    for (size_t i = 0; i < src_len; ++i) {
        uint8_t b = src[i];
        if (seen[b])
            continue;
        if (count == 4)
            return 0;
        seen[b] = true;
        symbols[count++] = b;
    }
    return count;
}

static int literal_symbol_index(
    const uint16_t* symbols,
    int count,
    uint8_t value) noexcept
{
    for (int i = 0; i < count; ++i)
        if (symbols[i] == value)
            return i;
    return -1;
}

static bool write_simple_literal_code(BitWriter& bw, int index, int count) noexcept {
    switch (count) {
    case 1:
        return true;
    case 2:
        return bw.write_bits(static_cast<uint32_t>(index), 1);
    case 3:
        if (index == 0)
            return bw.write_bits(0, 1);
        return bw.write_bits(index == 1 ? 1u : 3u, 2);
    case 4:
        return bw.write_bits(static_cast<uint32_t>(index), 2);
    default:
        return false;
    }
}

static bool write_compressed_literal_block(
    BitWriter& bw,
    const uint8_t* src,
    size_t src_len) noexcept
{
    if (src_len == 0)
        return false;

    uint16_t literal_symbols[4] = {};
    int literal_count = collect_literals(src, src_len, literal_symbols);
    uint16_t command_symbol = 0;
    if (literal_count == 0 || !command_symbol_for_insert(src_len, command_symbol))
        return false;

    uint16_t command_symbols[1] = { command_symbol };
    uint16_t distance_symbols[1] = { 0 };

    if (!bw.write_bits(0, 1) || !write_meta_len(bw, src_len) ||
        !bw.write_bits(0, 1))
        return false;

    if (!bw.write_bits(0, 1) || !bw.write_bits(0, 1) ||
        !bw.write_bits(0, 1))
        return false;
    if (!bw.write_bits(0, 2) || !bw.write_bits(0, 4) ||
        !bw.write_bits(0, 2))
        return false;
    if (!bw.write_bits(0, 1) || !bw.write_bits(0, 1))
        return false;

    if (!write_simple_prefix_code(bw, 256, literal_symbols, literal_count) ||
        !write_simple_prefix_code(bw, 704, command_symbols, 1) ||
        !write_simple_prefix_code(bw, 64, distance_symbols, 1))
        return false;

    if (!write_insert_extra(bw, command_symbol, src_len))
        return false;
    for (size_t i = 0; i < src_len; ++i) {
        int index = literal_symbol_index(literal_symbols, literal_count, src[i]);
        if (!write_simple_literal_code(bw, index, literal_count))
            return false;
    }
    return true;
}

size_t brotli_compress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    int quality, int lgwin) noexcept
{
    (void)quality;

    BitWriter bw;
    bw.init(dst, dst_cap);
    const uint8_t* input = src ? src : reinterpret_cast<const uint8_t*>("");

    if (!write_wbits(bw, lgwin))
        return 0;

    size_t pos = 0;
    if (quality > 0 && src_len > 0 && src_len <= kMaxMetaBlockSize) {
        uint16_t literal_symbols[4] = {};
        uint16_t command_symbol = 0;
        bool eligible = collect_literals(input, src_len, literal_symbols) != 0
            && command_symbol_for_insert(src_len, command_symbol);
        if (eligible) {
            if (!write_compressed_literal_block(bw, input, src_len))
                return 0;
            if (!bw.write_bits(1, 1)) return 0;
            if (!bw.write_bits(1, 1)) return 0;
            if (!bw.finish_zero()) return 0;
            return bw.overflow ? 0 : bw.bytes_written(dst);
        }
    }

    while (pos < src_len) {
        size_t chunk = std::min(src_len - pos, kMaxMetaBlockSize);

        /* ISLAST=0, non-empty uncompressed meta-block. */
        if (!bw.write_bits(0, 1)) return 0;
        if (!write_meta_len(bw, chunk)) return 0;
        if (!bw.write_bits(1, 1)) return 0; /* ISUNCOMPRESSED */
        if (!bw.align_zero()) return 0;
        if (!bw.write_bytes(input + pos, chunk)) return 0;

        pos += chunk;
    }

    /* Final empty meta-block: ISLAST=1, ISLASTEMPTY=1. */
    if (!bw.write_bits(1, 1)) return 0;
    if (!bw.write_bits(1, 1)) return 0;
    if (!bw.finish_zero()) return 0;

    return bw.overflow ? 0 : bw.bytes_written(dst);
}

} } /* namespace orot::brotli */
