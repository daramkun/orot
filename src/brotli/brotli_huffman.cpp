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

static bool read_simple_prefix_code_body(
    BitReader& br, int alphabet_size, PrefixCode& out) noexcept;

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

    return read_simple_prefix_code_body(br, alphabet_size, out);
}

static bool read_simple_prefix_code_body(
    BitReader& br, int alphabet_size, PrefixCode& out) noexcept {
    if (alphabet_size <= 0 || alphabet_size > PrefixCode::kMaxEntries)
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

static bool build_code_length_code(
    BitReader& br, int hskip, PrefixCode& out) noexcept {
    static const uint8_t kOrder[18] = {
        1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };
    static const uint8_t kFixedCodeLengths[6] = {2, 4, 3, 2, 2, 4};

    PrefixCode fixed;
    if (!fixed.build(kFixedCodeLengths, 6))
        return false;

    uint8_t lengths[18];
    std::memset(lengths, 0, sizeof(lengths));

    int nonzero = 0;
    int space = 32;

    for (int i = hskip; i < 18; ++i) {
        int len = fixed.decode(br);
        if (len < 0 || len > 5 || br.error)
            return false;
        lengths[kOrder[i]] = static_cast<uint8_t>(len);
        if (len != 0) {
            ++nonzero;
            space -= 32 >> len;
            if (space < 0)
                return false;
            if (nonzero >= 2 && space == 0)
                break;
        }
    }

    if (nonzero == 0)
        return false;
    if (nonzero >= 2 && space != 0)
        return false;

    return out.build(lengths, 18);
}

static bool flush_repeat(
    uint8_t* lengths,
    int alphabet_size,
    int& pos,
    int repeat_symbol,
    int repeat_count,
    int& prev_nonzero,
    int& space,
    int& nonzero) noexcept {
    if (repeat_symbol == 0)
        return true;
    if (repeat_count <= 0 || pos + repeat_count > alphabet_size)
        return false;

    uint8_t value = 0;
    if (repeat_symbol == 16) {
        value = static_cast<uint8_t>(prev_nonzero != 0 ? prev_nonzero : 8);
        prev_nonzero = value;
    }

    for (int i = 0; i < repeat_count; ++i) {
        lengths[pos++] = value;
        if (value != 0) {
            ++nonzero;
            space -= 32768 >> value;
            if (space < 0)
                return false;
        }
    }
    return true;
}

static bool read_complex_prefix_code(
    BitReader& br, int hskip, int alphabet_size, PrefixCode& out) noexcept {
    PrefixCode code_length_code;
    if (!build_code_length_code(br, hskip, code_length_code))
        return false;

    uint8_t lengths[PrefixCode::kMaxEntries];
    std::memset(lengths, 0, static_cast<size_t>(alphabet_size));

    int pos = 0;
    int prev_nonzero = 0;
    int repeat_symbol = 0;
    int repeat_count = 0;
    int last_symbol = -1;
    int space = 32768;
    int nonzero = 0;

    auto add_length = [&](int len) -> bool {
        if (pos >= alphabet_size)
            return false;
        lengths[pos++] = static_cast<uint8_t>(len);
        if (len != 0) {
            prev_nonzero = len;
            ++nonzero;
            space -= 32768 >> len;
            if (space < 0)
                return false;
        }
        return true;
    };

    while (pos < alphabet_size && space > 0) {
        int sym = code_length_code.decode(br);
        if (sym < 0 || br.error)
            return false;

        if (sym == 16 || sym == 17) {
            int extra_bits = (sym == 16) ? 2 : 3;
            int base = (sym == 16) ? 3 : 3;
            int mult = (sym == 16) ? 4 : 8;
            int count = base + static_cast<int>(br.read_bits(extra_bits));
            if (br.error)
                return false;

            if (repeat_symbol == sym) {
                repeat_count = mult * (repeat_count - 2) + count;
            } else {
                if (!flush_repeat(lengths, alphabet_size, pos,
                                  repeat_symbol, repeat_count, prev_nonzero,
                                  space, nonzero))
                    return false;
                repeat_symbol = sym;
                repeat_count = count;
            }
            last_symbol = sym;
            continue;
        }

        if (!flush_repeat(lengths, alphabet_size, pos,
                          repeat_symbol, repeat_count, prev_nonzero,
                          space, nonzero))
            return false;
        repeat_symbol = 0;
        repeat_count = 0;

        if (!add_length(sym))
            return false;
        last_symbol = sym;
    }

    if (!flush_repeat(lengths, alphabet_size, pos,
                      repeat_symbol, repeat_count, prev_nonzero,
                      space, nonzero))
        return false;

    if (last_symbol == 0 || last_symbol == 17)
        return false;
    if (nonzero < 2 || space != 0)
        return false;

    return out.build(lengths, alphabet_size);
}

bool read_prefix_code(BitReader& br, int alphabet_size, PrefixCode& out) noexcept {
    if (alphabet_size <= 0 || alphabet_size > PrefixCode::kMaxEntries)
        return false;

    uint32_t tag = br.read_bits(2);
    if (br.error)
        return false;
    if (tag == 1)
        return read_simple_prefix_code_body(br, alphabet_size, out);
    if (tag == 0 || tag == 2 || tag == 3)
        return read_complex_prefix_code(br, static_cast<int>(tag), alphabet_size, out);
    return false;
}

} } /* namespace orot::brotli */
