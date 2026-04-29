#pragma once

#include "brotli_bit.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orot { namespace brotli {

int read_var_len_uint8_plus_one(BitReader& br) noexcept;
int read_block_count(BitReader& br, int code) noexcept;

bool read_context_map(
    BitReader& br,
    int context_map_size,
    int num_trees,
    std::vector<uint8_t>& out) noexcept;

} } /* namespace orot::brotli */
