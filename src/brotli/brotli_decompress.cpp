#include "brotli_decompress.hpp"
#include "brotli_bit.hpp"
#include "brotli_compress.hpp"
#include "brotli_dictionary.hpp"
#include "brotli_meta.hpp"

#include <algorithm>
#include <cstring>

namespace orot { namespace brotli {

static int read_wbits(BitReader& br) noexcept {
    if (br.read_bits(1) == 0) return 16;
    uint32_t n = br.read_bits(3);
    if (br.error) return 0;
    if (n != 0)
        return 17 + static_cast<int>(n);
    n = br.read_bits(3);
    if (br.error) return 0;
    if (n == 1)
        return 0;
    if (n != 0)
        return 8 + static_cast<int>(n);
    return 17;
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
        0, 1, 2, 3, 4, 5, 6, 8,
        10, 14, 18, 26, 34, 50, 66, 98,
        130, 194, 322, 578, 1090, 2114, 6210, 22594
    };
    static const uint8_t kExtra[24] = {
        0, 0, 0, 0, 0, 0, 1, 1,
        2, 2, 3, 3, 4, 4, 5, 5,
        6, 7, 8, 9, 10, 12, 14, 24
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

static bool literal_context_id(
    int mode,
    uint8_t p1,
    uint8_t p2,
    int& out) noexcept
{
    static const uint8_t kUtf8Lut0[256] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 0, 4, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        8, 12, 16, 12, 12, 20, 12, 16, 24, 28, 12, 12, 32, 12, 36, 12,
        44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 32, 32, 24, 40, 28, 12,
        12, 48, 52, 52, 52, 48, 52, 52, 52, 48, 52, 52, 52, 52, 52, 48,
        52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 24, 12, 28, 12, 12,
        12, 56, 60, 60, 60, 56, 60, 60, 60, 56, 60, 60, 60, 60, 60, 56,
        60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 24, 12, 28, 12, 0,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
        2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
        2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
        2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3
    };
    static const uint8_t kUtf8Lut1[256] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
        1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
        1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2
    };
    static const uint8_t kSignedLut[256] = {
        0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7
    };
    switch (mode) {
    case 0:
        out = p1 & 0x3f;
        return true;
    case 1:
        out = p1 >> 2;
        return true;
    case 2:
        out = kUtf8Lut0[p1] | kUtf8Lut1[p2];
        return true;
    case 3:
        out = (kSignedLut[p1] << 3) | kSignedLut[p2];
        return true;
    default:
        return false;
    }
}

static int distance_context_id(int copy_len) noexcept {
    if (copy_len <= 2)
        return 0;
    if (copy_len == 3)
        return 1;
    if (copy_len == 4)
        return 2;
    return 3;
}

static bool all_zero_map(const std::vector<uint8_t>& map) noexcept {
    for (uint8_t v : map)
        if (v != 0)
            return false;
    return true;
}

static void copy_match_bytes(
    uint8_t* dst,
    size_t out_pos,
    size_t distance,
    size_t len) noexcept
{
    uint8_t* out = dst + out_pos;
    const uint8_t* from = out - distance;

    if (distance == 1) {
        std::memset(out, from[0], len);
        return;
    }
    if (distance >= len) {
        std::memcpy(out, from, len);
        return;
    }

    size_t copied = 0;
    size_t seed = std::min(distance, len);
    std::memcpy(out, from, seed);
    copied = seed;
    while (copied < len) {
        size_t chunk = std::min(copied, len - copied);
        std::memcpy(out + copied, out, chunk);
        copied += chunk;
    }
}

struct BlockState {
    const BlockCategoryHeader* header = nullptr;
    int type = 0;
    int remaining = 16777216;

    void init(const BlockCategoryHeader& h) noexcept {
        header = &h;
        type = 0;
        remaining = h.block_count;
    }

    bool is_single_type() const noexcept {
        return header && header->num_types == 1;
    }

    bool advance(BitReader& br) noexcept {
        if (!header)
            return false;
        if (remaining > 0) {
            --remaining;
            return true;
        }
        if (header->num_types < 2)
            return false;

        int symbol = header->type_code.decode(br);
        if (symbol < 0 || br.error)
            return false;
        if (symbol == 0)
            type = (type + 1) % header->num_types;
        else if (symbol == 1)
            type = (type + 2) % header->num_types;
        else if (symbol - 2 < header->num_types)
            type = symbol - 2;
        else
            return false;

        int count_code = header->count_code.decode(br);
        if (count_code < 0 || br.error)
            return false;
        int count = read_block_count(br, count_code);
        if (count <= 0)
            return false;
        remaining = count - 1;
        return true;
    }
};

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

    if (header.command_trees.empty() ||
        header.literal_trees.empty() ||
        header.distance_trees.empty())
        return BrotliDecodeStatus::Unsupported;

    size_t produced = 0;
    int last_distances[4] = {4, 11, 15, 16};
    BlockState literal_block;
    BlockState command_block;
    BlockState distance_block;
    literal_block.init(header.block_categories[0]);
    command_block.init(header.block_categories[1]);
    distance_block.init(header.block_categories[2]);
    const bool single_literal_block = literal_block.is_single_type();
    const bool single_command_block = command_block.is_single_type();
    const bool single_distance_block = distance_block.is_single_type();
    const bool literal_map_all_zero = all_zero_map(header.literal_context_map);
    const bool distance_map_all_zero = all_zero_map(header.distance_context_map);

    while (produced < meta_len) {
        if (!single_command_block && !command_block.advance(br))
            return BrotliDecodeStatus::DataError;
        if (command_block.type < 0 ||
            static_cast<size_t>(command_block.type) >= header.command_trees.size())
            return BrotliDecodeStatus::DataError;

        int command_symbol =
            header.command_trees[static_cast<size_t>(command_block.type)].decode(br);
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
            if (!single_literal_block && !literal_block.advance(br))
                return BrotliDecodeStatus::DataError;
            if (literal_block.type < 0 ||
                static_cast<size_t>(literal_block.type) >=
                    header.literal_context_modes.size())
                return BrotliDecodeStatus::DataError;

            uint8_t p1 = out_pos > 0 ? dst[out_pos - 1] : 0;
            uint8_t p2 = out_pos > 1 ? dst[out_pos - 2] : 0;
            int context_id = 0;
            if (!literal_context_id(
                    header.literal_context_modes[static_cast<size_t>(literal_block.type)],
                    p1, p2, context_id))
                return BrotliDecodeStatus::Unsupported;

            int literal_tree_index = literal_block.type;
            if (!literal_map_all_zero) {
                size_t map_index = static_cast<size_t>(literal_block.type) * 64u
                    + static_cast<size_t>(context_id);
                if (map_index >= header.literal_context_map.size())
                    return BrotliDecodeStatus::DataError;
                literal_tree_index = header.literal_context_map[map_index];
            }
            if (literal_tree_index < 0 ||
                static_cast<size_t>(literal_tree_index) >= header.literal_trees.size())
                return BrotliDecodeStatus::DataError;

            int literal =
                header.literal_trees[static_cast<size_t>(literal_tree_index)].decode(br);
            if (literal < 0 || literal > 255 || br.error)
                return BrotliDecodeStatus::DataError;
            dst[out_pos++] = static_cast<uint8_t>(literal);
            ++produced;
        }

        if (produced == meta_len)
            break;

        int distance = last_distances[0];
        bool should_push_distance = false;
        if (!lengths.implicit_distance) {
            if (!single_distance_block && !distance_block.advance(br))
                return BrotliDecodeStatus::DataError;
            int distance_tree_index = distance_block.type;
            if (!distance_map_all_zero) {
                size_t map_index = static_cast<size_t>(distance_block.type) * 4u
                    + static_cast<size_t>(distance_context_id(lengths.copy_len));
                if (map_index >= header.distance_context_map.size())
                    return BrotliDecodeStatus::DataError;
                distance_tree_index = header.distance_context_map[map_index];
            }
            if (distance_tree_index < 0 ||
                static_cast<size_t>(distance_tree_index) >= header.distance_trees.size())
                return BrotliDecodeStatus::DataError;

            int distance_code =
                header.distance_trees[static_cast<size_t>(distance_tree_index)].decode(br);
            if (distance_code < 0 || br.error)
                return BrotliDecodeStatus::DataError;
            if (!compute_distance(distance_code, br, header.ndirect,
                                  header.npostfix, last_distances, distance))
                return BrotliDecodeStatus::DataError;
            should_push_distance = distance_code != 0;
        }

        if (distance <= 0)
            return BrotliDecodeStatus::DataError;

        bool used_dictionary = false;
        if (static_cast<size_t>(distance) > out_pos) {
            if (out_pos > dst_cap)
                return BrotliDecodeStatus::NeedOutput;
            size_t word_len = 0;
            if (!decode_static_dictionary_word(
                    distance, out_pos, lengths.copy_len, dst + out_pos,
                    dst_cap - out_pos, word_len))
                return BrotliDecodeStatus::Unsupported;
            if (word_len > meta_len - produced)
                return BrotliDecodeStatus::DataError;
            out_pos += word_len;
            produced += word_len;
            used_dictionary = true;
        } else {
            if ((size_t)lengths.copy_len > meta_len - produced)
                return BrotliDecodeStatus::DataError;
            if (out_pos + static_cast<size_t>(lengths.copy_len) > dst_cap)
                return BrotliDecodeStatus::NeedOutput;
            copy_match_bytes(
                dst, out_pos, static_cast<size_t>(distance),
                static_cast<size_t>(lengths.copy_len));
            out_pos += static_cast<size_t>(lengths.copy_len);
            produced += static_cast<size_t>(lengths.copy_len);
        }
        if (should_push_distance && !used_dictionary)
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
