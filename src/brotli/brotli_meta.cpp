#include "brotli_meta.hpp"

namespace orot { namespace brotli {

int read_var_len_uint8_plus_one(BitReader& br) noexcept {
    uint32_t marker = br.read_bits(1);
    if (br.error)
        return -1;
    if (marker == 0)
        return 1;
    int n = static_cast<int>(br.read_bits(3));
    if (br.error)
        return -1;
    if (n == 0)
        return 2;
    uint32_t extra = br.read_bits(n);
    if (br.error)
        return -1;
    return 1 + (1 << n) + static_cast<int>(extra);
}

int read_block_count(BitReader& br, int code) noexcept {
    static const uint8_t kExtraBits[26] = {
        2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
        5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24
    };
    static const int kBase[26] = {
        1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81, 97, 113,
        145, 177, 209, 241, 305, 369, 497, 753, 1265, 2289,
        4337, 8433, 16625
    };

    if (code < 0 || code >= 26)
        return -1;
    int extra_bits = kExtraBits[code];
    uint32_t extra = br.read_bits(extra_bits);
    if (br.error)
        return -1;
    return kBase[code] + static_cast<int>(extra);
}

static int read_rlemax(BitReader& br) noexcept {
    uint32_t marker = br.read_bits(1);
    if (br.error)
        return -1;
    if (marker == 0)
        return 0;
    uint32_t tail = br.read_bits(4);
    if (br.error)
        return -1;
    return 1 + static_cast<int>(tail);
}

static void inverse_mtf(std::vector<uint8_t>& values) {
    uint8_t mtf[256];
    for (int i = 0; i < 256; ++i)
        mtf[i] = static_cast<uint8_t>(i);

    for (uint8_t& v : values) {
        uint8_t index = v;
        uint8_t value = mtf[index];
        v = value;
        while (index != 0) {
            mtf[index] = mtf[index - 1];
            --index;
        }
        mtf[0] = value;
    }
}

bool read_context_map(
    BitReader& br,
    int context_map_size,
    int num_trees,
    std::vector<uint8_t>& out) noexcept
{
    if (context_map_size < 0 || num_trees <= 0 || num_trees > 256)
        return false;

    out.clear();
    out.reserve(static_cast<size_t>(context_map_size));

    int rlemax = read_rlemax(br);
    if (rlemax < 0 || rlemax > 16)
        return false;

    PrefixCode code;
    if (!read_prefix_code(br, num_trees + rlemax, code))
        return false;

    while ((int)out.size() < context_map_size) {
        int sym = code.decode(br);
        if (sym < 0 || br.error)
            return false;

        if (sym == 0) {
            out.push_back(0);
        } else if (sym <= rlemax) {
            int repeat = (1 << sym) + static_cast<int>(br.read_bits(sym));
            if (br.error || (int)out.size() + repeat > context_map_size)
                return false;
            out.insert(out.end(), static_cast<size_t>(repeat), 0);
        } else {
            int value = sym - rlemax;
            if (value < 1 || value >= num_trees)
                return false;
            out.push_back(static_cast<uint8_t>(value));
        }
    }

    bool imtf = br.read_bits(1) != 0;
    if (br.error)
        return false;
    if (imtf)
        inverse_mtf(out);

    return true;
}

static bool read_block_category_header(
    BitReader& br,
    BlockCategoryHeader& out) noexcept
{
    int num_types = read_var_len_uint8_plus_one(br);
    if (num_types <= 0 || num_types > 256)
        return false;

    out = BlockCategoryHeader{};
    out.num_types = num_types;
    if (num_types < 2)
        return true;

    if (!read_prefix_code(br, num_types + 2, out.type_code))
        return false;
    if (!read_prefix_code(br, 26, out.count_code))
        return false;

    int block_count_code = out.count_code.decode(br);
    if (block_count_code < 0 || br.error)
        return false;
    int block_count = read_block_count(br, block_count_code);
    if (block_count <= 0)
        return false;

    out.block_count = block_count;
    return true;
}

static bool read_prefix_code_array(
    BitReader& br,
    int count,
    int alphabet_size,
    std::vector<PrefixCode>& out) noexcept
{
    if (count <= 0 || alphabet_size <= 0 || alphabet_size > PrefixCode::kMaxEntries)
        return false;

    out.clear();
    out.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        if (!read_prefix_code(br, alphabet_size, out[static_cast<size_t>(i)]))
            return false;
    }
    return true;
}

bool read_compressed_meta_block_header(
    BitReader& br,
    CompressedMetaBlockHeader& out) noexcept
{
    out = CompressedMetaBlockHeader{};

    for (BlockCategoryHeader& category : out.block_categories) {
        if (!read_block_category_header(br, category))
            return false;
    }

    out.npostfix = static_cast<int>(br.read_bits(2));
    uint32_t ndirect_msb = br.read_bits(4);
    if (br.error)
        return false;
    out.ndirect = static_cast<int>(ndirect_msb << out.npostfix);

    const int num_literal_block_types = out.block_categories[0].num_types;
    out.literal_context_modes.resize(static_cast<size_t>(num_literal_block_types));
    for (int i = 0; i < num_literal_block_types; ++i) {
        out.literal_context_modes[static_cast<size_t>(i)] =
            static_cast<uint8_t>(br.read_bits(2));
        if (br.error)
            return false;
    }

    int num_literal_trees = read_var_len_uint8_plus_one(br);
    if (num_literal_trees <= 0 || num_literal_trees > 256)
        return false;
    int literal_context_map_size = 64 * num_literal_block_types;
    if (num_literal_trees >= 2) {
        if (!read_context_map(br, literal_context_map_size, num_literal_trees,
                              out.literal_context_map))
            return false;
    } else {
        out.literal_context_map.assign(
            static_cast<size_t>(literal_context_map_size), 0);
    }

    int num_distance_trees = read_var_len_uint8_plus_one(br);
    if (num_distance_trees <= 0 || num_distance_trees > 256)
        return false;
    int distance_context_map_size = 4 * out.block_categories[2].num_types;
    if (num_distance_trees >= 2) {
        if (!read_context_map(br, distance_context_map_size, num_distance_trees,
                              out.distance_context_map))
            return false;
    } else {
        out.distance_context_map.assign(
            static_cast<size_t>(distance_context_map_size), 0);
    }

    const int distance_alphabet_size = 16 + out.ndirect + (48 << out.npostfix);
    return read_prefix_code_array(br, num_literal_trees, 256, out.literal_trees)
        && read_prefix_code_array(br, out.block_categories[1].num_types, 704,
                                  out.command_trees)
        && read_prefix_code_array(br, num_distance_trees, distance_alphabet_size,
                                  out.distance_trees);
}

} } /* namespace orot::brotli */
