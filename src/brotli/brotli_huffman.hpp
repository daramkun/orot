#pragma once

#include "brotli_bit.hpp"

#include <cstddef>
#include <cstdint>

namespace orot { namespace brotli {

struct PrefixCodeEntry {
    uint16_t symbol = 0;
    uint16_t code = 0;
    uint8_t length = 0;
};

struct PrefixCode {
    static constexpr int kMaxEntries = 704;
    static constexpr int kLookupBits = 8;
    static constexpr int kLookupSize = 1 << kLookupBits;
    PrefixCodeEntry entries[kMaxEntries];
    uint16_t symbols[kMaxEntries];
    uint16_t first_code[16];
    uint16_t first_index[16];
    uint16_t code_count[16];
    uint16_t lookup_symbol[kLookupSize];
    uint8_t lookup_length[kLookupSize];
    int num_entries = 0;
    int max_length = 0;
    uint16_t single_symbol = 0;
    bool is_single_symbol = false;

    bool build(const uint8_t* lengths, int alphabet_size) noexcept;
    int decode(BitReader& br) const noexcept;
};

bool read_simple_prefix_code(BitReader& br, int alphabet_size, PrefixCode& out) noexcept;
bool read_prefix_code(BitReader& br, int alphabet_size, PrefixCode& out) noexcept;

} } /* namespace orot::brotli */
