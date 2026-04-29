#pragma once

#include "brotli_bit.hpp"
#include "brotli_huffman.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

namespace orot { namespace brotli {

struct BlockCategoryHeader {
    int num_types = 1;
    int block_count = 16777216;
    PrefixCode type_code;
    PrefixCode count_code;
};

struct CompressedMetaBlockHeader {
    std::array<BlockCategoryHeader, 3> block_categories;
    int npostfix = 0;
    int ndirect = 0;
    std::vector<uint8_t> literal_context_modes;
    std::vector<uint8_t> literal_context_map;
    std::vector<uint8_t> distance_context_map;
    std::vector<PrefixCode> literal_trees;
    std::vector<PrefixCode> command_trees;
    std::vector<PrefixCode> distance_trees;
};

int read_var_len_uint8_plus_one(BitReader& br) noexcept;
int read_block_count(BitReader& br, int code) noexcept;

bool read_context_map(
    BitReader& br,
    int context_map_size,
    int num_trees,
    std::vector<uint8_t>& out) noexcept;

bool read_compressed_meta_block_header(
    BitReader& br,
    CompressedMetaBlockHeader& out) noexcept;

} } /* namespace orot::brotli */
