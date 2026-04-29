#include "brotli_huffman.hpp"

#include <algorithm>
#include <cstring>

namespace orot { namespace brotli {

static int alphabet_bits(int alphabet_size) noexcept {
    int bits = 0;
    int limit = 1;
    while (limit < alphabet_size) {
        limit <<= 1;
        ++bits;
    }
    return bits;
}

bool PrefixCode::build(const uint8_t* lengths, int alphabet_size) noexcept {
    num_entries = 0;
    max_length = 0;
    single_symbol = 0;
    is_single_symbol = false;

    int nonzero = 0;
    for (int i = 0; i < alphabet_size; ++i) {
        if (lengths[i] != 0) {
            ++nonzero;
            single_symbol = static_cast<uint16_t>(i);
            if (lengths[i] > max_length)
                max_length = lengths[i];
        }
    }

    if (nonzero == 0 || nonzero > kMaxEntries || max_length > 15)
        return false;

    if (nonzero == 1) {
        is_single_symbol = true;
        return true;
    }

    int bl_count[16] = {};
    for (int i = 0; i < alphabet_size; ++i)
        if (lengths[i] != 0)
            ++bl_count[lengths[i]];

    int code = 0;
    int next_code[16] = {};
    for (int bits = 1; bits <= max_length; ++bits) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    for (int len = 1; len <= max_length; ++len) {
        for (int symbol = 0; symbol < alphabet_size; ++symbol) {
            if (lengths[symbol] != len)
                continue;
            if (num_entries >= kMaxEntries)
                return false;
            entries[num_entries++] = PrefixCodeEntry{
                static_cast<uint16_t>(symbol),
                static_cast<uint16_t>(next_code[len]++),
                static_cast<uint8_t>(len)
            };
        }
    }

    return true;
}

int PrefixCode::decode(BitReader& br) const noexcept {
    if (is_single_symbol)
        return single_symbol;

    uint32_t code = 0;
    for (int len = 1; len <= max_length; ++len) {
        code = (code << 1) | br.read_bits(1);
        if (br.error)
            return -1;
        for (int i = 0; i < num_entries; ++i) {
            if (entries[i].length == len && entries[i].code == code)
                return entries[i].symbol;
        }
    }
    br.error = true;
    return -1;
}

bool read_simple_prefix_code(BitReader& br, int alphabet_size, PrefixCode& out) noexcept {
    if (alphabet_size <= 0 || alphabet_size > PrefixCode::kMaxEntries)
        return false;

    if (br.read_bits(2) != 1 || br.error)
        return false;

    int nsym = static_cast<int>(br.read_bits(2)) + 1;
    int abits = alphabet_bits(alphabet_size);
    if (br.error)
        return false;

    uint16_t symbols[4] = {};
    for (int i = 0; i < nsym; ++i) {
        uint32_t sym = br.read_bits(abits);
        if (br.error || sym >= static_cast<uint32_t>(alphabet_size))
            return false;
        for (int j = 0; j < i; ++j)
            if (symbols[j] == sym)
                return false;
        symbols[i] = static_cast<uint16_t>(sym);
    }

    bool tree_select = false;
    if (nsym == 4) {
        tree_select = br.read_bits(1) != 0;
        if (br.error)
            return false;
    }

    uint8_t lengths[PrefixCode::kMaxEntries];
    std::memset(lengths, 0, static_cast<size_t>(alphabet_size));

    switch (nsym) {
    case 1:
        lengths[symbols[0]] = 1;
        break;
    case 2:
        lengths[symbols[0]] = 1;
        lengths[symbols[1]] = 1;
        break;
    case 3:
        lengths[symbols[0]] = 1;
        lengths[symbols[1]] = 2;
        lengths[symbols[2]] = 2;
        break;
    case 4:
        if (tree_select) {
            lengths[symbols[0]] = 1;
            lengths[symbols[1]] = 2;
            lengths[symbols[2]] = 3;
            lengths[symbols[3]] = 3;
        } else {
            lengths[symbols[0]] = 2;
            lengths[symbols[1]] = 2;
            lengths[symbols[2]] = 2;
            lengths[symbols[3]] = 2;
        }
        break;
    default:
        return false;
    }

    return out.build(lengths, alphabet_size);
}

} } /* namespace orot::brotli */
