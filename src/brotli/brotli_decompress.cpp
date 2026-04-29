#include "brotli_decompress.hpp"
#include "brotli_bit.hpp"
#include "brotli_compress.hpp"
#include "brotli_meta.hpp"

#include <cstring>

namespace orot { namespace brotli {

static int read_wbits(BitReader& br) noexcept {
    if (br.read_bits(1) == 0) return 16;
    uint32_t suffix = br.read_bits(3);
    if (br.error) return 0;
    switch (suffix) {
    case 0b001: {
        uint32_t top = br.read_bits(3);
        if (br.error) return 0;
        if (top == 0) return 17;
        if (top <= 6) return 9 + (int)top;
        return 0;
    }
    case 0b011: return 18;
    case 0b101: return 19;
    case 0b111: return 20;
    case 0b100: return 21;
    case 0b110: return 22;
    case 0b010: return 23;
    case 0b000: return 24;
    default: return 0;
    }
}

static int decode_mnibbles(uint32_t code) noexcept {
    switch (code) {
    case 0: return 4;
    case 1: return 5;
    case 2: return 6;
    default: return 0;
    }
}

struct CommandLengths {
    int insert_len = 0;
    int copy_len = 0;
    bool implicit_distance = false;
};

static bool decode_insert_length(int code, BitReader& br, int& out) noexcept {
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
    if (code < 0 || code >= 24)
        return false;
    uint32_t extra = br.read_bits(kExtra[code]);
    if (br.error)
        return false;
    out = static_cast<int>(kBase[code] + extra);
    return true;
}

static bool decode_copy_length(int code, BitReader& br, int& out) noexcept {
    static const uint16_t kBase[24] = {
        2, 3, 4, 5, 6, 7, 8, 9,
        10, 12, 14, 18, 22, 30, 38, 54,
        70, 102, 134, 198, 326, 582, 1094, 2118
    };
    static const uint8_t kExtra[24] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 2, 2, 3, 3, 4, 4,
        5, 5, 6, 7, 8, 9, 10, 24
    };
    if (code < 0 || code >= 24)
        return false;
    uint32_t extra = br.read_bits(kExtra[code]);
    if (br.error)
        return false;
    out = static_cast<int>(kBase[code] + extra);
    return true;
}

static bool decode_command_symbol(
    int symbol,
    BitReader& br,
    CommandLengths& out) noexcept
{
    if (symbol < 0 || symbol >= 704)
        return false;

    static const uint8_t kInsertBase[11] = {
        0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16
    };
    static const uint8_t kCopyBase[11] = {
        0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16
    };

    int group = symbol >> 6;
    int low = symbol & 63;
    int insert_code = kInsertBase[group] + ((low >> 3) & 7);
    int copy_code = kCopyBase[group] + (low & 7);

    out.implicit_distance = symbol < 128;
    return decode_insert_length(insert_code, br, out.insert_len)
        && decode_copy_length(copy_code, br, out.copy_len);
}

static bool compute_distance(
    int distance_code,
    BitReader& br,
    int ndirect,
    int npostfix,
    const int last_distances[4],
    int& out) noexcept
{
    if (distance_code < 0)
        return false;
    if (distance_code < 16) {
        static const uint8_t kIndex[16] = {
            0, 1, 2, 3, 0, 0, 0, 0,
            0, 0, 1, 1, 1, 1, 1, 1
        };
        static const int8_t kDelta[16] = {
            0, 0, 0, 0, -1, 1, -2, 2,
            -3, 3, -1, 1, -2, 2, -3, 3
        };
        int distance = last_distances[kIndex[distance_code]] + kDelta[distance_code];
        if (distance <= 0)
            return false;
        out = distance;
        return true;
    }

    int adjusted = distance_code - 16;
    if (adjusted < ndirect) {
        out = adjusted + 1;
        return true;
    }

    int postfix = adjusted & ((1 << npostfix) - 1);
    int bucket = (adjusted - ndirect) >> npostfix;
    int nbits = (bucket >> 1) + 1;
    int offset = ((2 + (bucket & 1)) << nbits) - 4;
    uint32_t extra = br.read_bits(nbits);
    if (br.error)
        return false;
    out = ((offset + static_cast<int>(extra)) << npostfix) + postfix + ndirect + 1;
    return true;
}

static void push_distance(int distance, int last_distances[4]) noexcept {
    if (distance == last_distances[0])
        return;
    for (int i = 3; i > 0; --i)
        last_distances[i] = last_distances[i - 1];
    last_distances[0] = distance;
}

static BrotliDecodeStatus decode_compressed_meta_block(
    BitReader& br,
    const CompressedMetaBlockHeader& header,
    size_t meta_len,
    int wbits,
    uint8_t* dst,
    size_t dst_cap,
    size_t& out_pos) noexcept
{
    (void)wbits;

    if (header.block_categories[0].num_types != 1 ||
        header.block_categories[1].num_types != 1 ||
        header.block_categories[2].num_types != 1 ||
        header.literal_trees.size() != 1 ||
        header.command_trees.size() != 1 ||
        header.distance_trees.size() != 1)
        return BrotliDecodeStatus::Unsupported;

    size_t produced = 0;
    int last_distances[4] = {4, 11, 15, 16};

    while (produced < meta_len) {
        int command_symbol = header.command_trees[0].decode(br);
        if (command_symbol < 0 || br.error)
            return BrotliDecodeStatus::DataError;

        CommandLengths lengths;
        if (!decode_command_symbol(command_symbol, br, lengths))
            return BrotliDecodeStatus::DataError;

        if (lengths.insert_len < 0 || (size_t)lengths.insert_len > meta_len - produced)
            return BrotliDecodeStatus::DataError;
        if (out_pos + static_cast<size_t>(lengths.insert_len) > dst_cap)
            return BrotliDecodeStatus::NeedOutput;

        for (int i = 0; i < lengths.insert_len; ++i) {
            int literal = header.literal_trees[0].decode(br);
            if (literal < 0 || literal > 255 || br.error)
                return BrotliDecodeStatus::DataError;
            dst[out_pos++] = static_cast<uint8_t>(literal);
            ++produced;
        }

        if (produced == meta_len)
            break;

        if ((size_t)lengths.copy_len > meta_len - produced)
            return BrotliDecodeStatus::DataError;

        int distance = last_distances[0];
        bool should_push_distance = false;
        if (!lengths.implicit_distance) {
            int distance_code = header.distance_trees[0].decode(br);
            if (distance_code < 0 || br.error)
                return BrotliDecodeStatus::DataError;
            if (!compute_distance(distance_code, br, header.ndirect,
                                  header.npostfix, last_distances, distance))
                return BrotliDecodeStatus::DataError;
            should_push_distance = distance_code != 0;
        }

        if (distance <= 0 || static_cast<size_t>(distance) > out_pos)
            return BrotliDecodeStatus::Unsupported;
        if (out_pos + static_cast<size_t>(lengths.copy_len) > dst_cap)
            return BrotliDecodeStatus::NeedOutput;

        for (int i = 0; i < lengths.copy_len; ++i)
            dst[out_pos + static_cast<size_t>(i)] =
                dst[out_pos - static_cast<size_t>(distance) + static_cast<size_t>(i)];
        out_pos += static_cast<size_t>(lengths.copy_len);
        produced += static_cast<size_t>(lengths.copy_len);
        if (should_push_distance)
            push_distance(distance, last_distances);
    }

    return BrotliDecodeStatus::Ok;
}

BrotliDecodeStatus brotli_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    size_t* actual_out) noexcept
{
    if (actual_out) *actual_out = 0;

    BitReader br;
    br.init(src, src_len);

    int wbits = read_wbits(br);
    if (wbits < kLgWinMin || wbits > kLgWinMax || br.error)
        return BrotliDecodeStatus::DataError;

    size_t out_pos = 0;

    for (;;) {
        bool is_last = br.read_bits(1) != 0;
        if (br.error) return BrotliDecodeStatus::DataError;

        if (is_last) {
            bool is_last_empty = br.read_bits(1) != 0;
            if (br.error) return BrotliDecodeStatus::DataError;
            if (is_last_empty) {
                if (!br.remaining_zero()) return BrotliDecodeStatus::DataError;
                if (actual_out) *actual_out = out_pos;
                return BrotliDecodeStatus::Ok;
            }
        }

        int mnibbles = decode_mnibbles(br.read_bits(2));
        if (br.error) return BrotliDecodeStatus::DataError;

        if (mnibbles == 0) {
            if (br.read_bits(1) != 0) return BrotliDecodeStatus::DataError;
            uint32_t mskipbytes = br.read_bits(2);
            if (br.error) return BrotliDecodeStatus::DataError;
            size_t skip_len = 0;
            if (mskipbytes > 0) {
                uint32_t raw = br.read_bits((int)mskipbytes * 8);
                if (br.error) return BrotliDecodeStatus::DataError;
                if (mskipbytes > 1 && ((raw >> ((mskipbytes - 1) * 8)) & 0xFFu) == 0)
                    return BrotliDecodeStatus::DataError;
                skip_len = (size_t)raw + 1;
            }
            if (!br.align_zero()) return BrotliDecodeStatus::DataError;
            if (!br.consume_bytes(skip_len)) return BrotliDecodeStatus::DataError;
            continue;
        }

        uint32_t raw_len = br.read_bits(mnibbles * 4);
        if (br.error) return BrotliDecodeStatus::DataError;
        if (mnibbles > 4 && ((raw_len >> ((mnibbles - 1) * 4)) & 0xFu) == 0)
            return BrotliDecodeStatus::DataError;
        size_t meta_len = (size_t)raw_len + 1;

        if (!is_last) {
            bool is_uncompressed = br.read_bits(1) != 0;
            if (br.error) return BrotliDecodeStatus::DataError;
            if (is_uncompressed) {
                if (!br.align_zero()) return BrotliDecodeStatus::DataError;
                if (out_pos + meta_len > dst_cap) return BrotliDecodeStatus::NeedOutput;
                if ((size_t)(br.end - br.byte_ptr()) < meta_len)
                    return BrotliDecodeStatus::DataError;
                std::memcpy(dst + out_pos, br.byte_ptr(), meta_len);
                if (!br.consume_bytes(meta_len)) return BrotliDecodeStatus::DataError;
                out_pos += meta_len;
                continue;
            }
        }

        CompressedMetaBlockHeader header;
        if (!read_compressed_meta_block_header(br, header) || br.error)
            return BrotliDecodeStatus::DataError;
        BrotliDecodeStatus status = decode_compressed_meta_block(
            br, header, meta_len, wbits, dst, dst_cap, out_pos);
        if (status != BrotliDecodeStatus::Ok)
            return status;
        if (is_last) {
            if (!br.remaining_zero())
                return BrotliDecodeStatus::DataError;
            if (actual_out) *actual_out = out_pos;
            return BrotliDecodeStatus::Ok;
        }
    }
}

} } /* namespace orot::brotli */
