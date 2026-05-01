#include "brotli_compress.hpp"
#include "brotli_bit.hpp"

#include <algorithm>
#include <cstring>

namespace orot { namespace brotli {

static constexpr size_t kMaxMetaBlockSize = 1u << 24;

size_t brotli_compress_bound(size_t src_len) noexcept {
    size_t blocks = (src_len + kMaxMetaBlockSize - 1) / kMaxMetaBlockSize;
    return src_len + 512 + blocks * 16;
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
        0, 1, 2, 3, 4, 5, 6, 8,
        10, 14, 18, 26, 34, 50, 66, 98,
        130, 194, 322, 578, 1090, 2114, 6210, 22594
    };
    static const uint8_t kExtra[24] = {
        0, 0, 0, 0, 0, 0, 1, 1,
        2, 2, 3, 3, 4, 4, 5, 5,
        6, 7, 8, 9, 10, 12, 14, 24
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
        0, 1, 2, 3, 4, 5, 6, 8,
        10, 14, 18, 26, 34, 50, 66, 98,
        130, 194, 322, 578, 1090, 2114, 6210, 22594
    };
    static const uint8_t kExtra[24] = {
        0, 0, 0, 0, 0, 0, 1, 1,
        2, 2, 3, 3, 4, 4, 5, 5,
        6, 7, 8, 9, 10, 12, 14, 24
    };
    static const uint8_t kInsertBase[11] = {
        0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16
    };
    int group = symbol >> 6;
    int low = symbol & 63;
    int code = kInsertBase[group] + ((low >> 3) & 7);
    return bw.write_bits(static_cast<uint32_t>(len - kBase[code]), kExtra[code]);
}

static bool length_code(int value, const uint16_t* base, const uint8_t* extra, int& code) noexcept {
    if (value < 0)
        return false;
    for (int i = 0; i < 24; ++i) {
        int count = 1 << extra[i];
        if (value >= base[i] && value < base[i] + count) {
            code = i;
            return true;
        }
    }
    return false;
}

static bool command_symbol_for_insert_copy(
    size_t insert_len,
    size_t copy_len,
    uint16_t& symbol) noexcept
{
    static const uint16_t kInsertBaseValue[24] = {
        0, 1, 2, 3, 4, 5, 6, 8,
        10, 14, 18, 26, 34, 50, 66, 98,
        130, 194, 322, 578, 1090, 2114, 6210, 22594
    };
    static const uint8_t kInsertExtra[24] = {
        0, 0, 0, 0, 0, 0, 1, 1,
        2, 2, 3, 3, 4, 4, 5, 5,
        6, 7, 8, 9, 10, 12, 14, 24
    };
    static const uint16_t kCopyBaseValue[24] = {
        2, 3, 4, 5, 6, 7, 8, 9,
        10, 12, 14, 18, 22, 30, 38, 54,
        70, 102, 134, 198, 326, 582, 1094, 2118
    };
    static const uint8_t kCopyExtra[24] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 2, 2, 3, 3, 4, 4,
        5, 5, 6, 7, 8, 9, 10, 24
    };
    static const uint8_t kInsertBase[11] = {
        0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16
    };
    static const uint8_t kCopyBase[11] = {
        0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16
    };

    int insert_code = 0;
    int copy_code = 0;
    if (!length_code(static_cast<int>(insert_len), kInsertBaseValue,
                     kInsertExtra, insert_code) ||
        !length_code(static_cast<int>(copy_len), kCopyBaseValue,
                     kCopyExtra, copy_code))
        return false;

    for (int group = 2; group < 11; ++group) {
        if (insert_code < kInsertBase[group] ||
            insert_code >= kInsertBase[group] + 8 ||
            copy_code < kCopyBase[group] ||
            copy_code >= kCopyBase[group] + 8)
            continue;
        symbol = static_cast<uint16_t>(
            group * 64 + ((insert_code - kInsertBase[group]) << 3)
            + (copy_code - kCopyBase[group]));
        return true;
    }
    return false;
}

static bool write_command_extra(
    BitWriter& bw,
    uint16_t symbol,
    size_t insert_len,
    size_t copy_len) noexcept
{
    static const uint16_t kInsertBaseValue[24] = {
        0, 1, 2, 3, 4, 5, 6, 8,
        10, 14, 18, 26, 34, 50, 66, 98,
        130, 194, 322, 578, 1090, 2114, 6210, 22594
    };
    static const uint8_t kInsertExtra[24] = {
        0, 0, 0, 0, 0, 0, 1, 1,
        2, 2, 3, 3, 4, 4, 5, 5,
        6, 7, 8, 9, 10, 12, 14, 24
    };
    static const uint16_t kCopyBaseValue[24] = {
        2, 3, 4, 5, 6, 7, 8, 9,
        10, 12, 14, 18, 22, 30, 38, 54,
        70, 102, 134, 198, 326, 582, 1094, 2118
    };
    static const uint8_t kCopyExtra[24] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 2, 2, 3, 3, 4, 4,
        5, 5, 6, 7, 8, 9, 10, 24
    };
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
    return bw.write_bits(static_cast<uint32_t>(insert_len - kInsertBaseValue[insert_code]),
                         kInsertExtra[insert_code]) &&
           bw.write_bits(static_cast<uint32_t>(copy_len - kCopyBaseValue[copy_code]),
                         kCopyExtra[copy_code]);
}

static bool distance_code_for_distance(
    size_t distance,
    uint16_t& code,
    uint32_t& extra,
    int& extra_bits) noexcept
{
    if (distance == 0)
        return false;
    for (int bucket = 0; bucket < 48; ++bucket) {
        int nbits = (bucket >> 1) + 1;
        int offset = ((2 + (bucket & 1)) << nbits) - 4;
        int base = offset + 1;
        int limit = base + (1 << nbits);
        if (distance < static_cast<size_t>(base) ||
            distance >= static_cast<size_t>(limit))
            continue;
        code = static_cast<uint16_t>(16 + bucket);
        extra = static_cast<uint32_t>(distance - static_cast<size_t>(base));
        extra_bits = nbits;
        return true;
    }
    return false;
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
    std::sort(symbols, symbols + count);
    return count;
}

static bool has_simple_literal_alphabet(const uint8_t* src, size_t src_len) noexcept {
    uint16_t symbols[4] = {};
    return collect_literals(src, src_len, symbols) > 0;
}

static bool has_high_byte_diversity(const uint8_t* src, size_t src_len) noexcept {
    if (src_len < 32768)
        return false;

    bool seen[256];
    std::memset(seen, 0, sizeof(seen));
    int count = 0;
    size_t limit = std::min(src_len, static_cast<size_t>(8192));
    for (size_t i = 0; i < limit; ++i) {
        uint8_t b = src[i];
        if (seen[b])
            continue;
        seen[b] = true;
        if (++count > 200)
            return true;
    }
    return false;
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

static bool write_prefix_bits(BitWriter& bw, uint32_t code, int len) noexcept {
    uint32_t reversed = 0;
    for (int i = 0; i < len; ++i)
        reversed = (reversed << 1) | ((code >> i) & 1u);
    return bw.write_bits(reversed, len);
}

static bool write_fixed_8_literal_prefix_code(BitWriter& bw) noexcept {
    static const uint8_t kOrder[18] = {
        1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };

    if (!bw.write_bits(0, 2))
        return false;

    for (int i = 0; i < 18; ++i) {
        if (kOrder[i] == 8) {
            if (!bw.write_bits(0b0111, 4))
                return false;
        } else {
            if (!bw.write_bits(0, 2))
                return false;
        }
    }
    return true;
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
    if (!command_symbol_for_insert(src_len, command_symbol))
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

    if (!((literal_count > 0 &&
           write_simple_prefix_code(bw, 256, literal_symbols, literal_count)) ||
          write_fixed_8_literal_prefix_code(bw)) ||
        !write_simple_prefix_code(bw, 704, command_symbols, 1) ||
        !write_simple_prefix_code(bw, 64, distance_symbols, 1))
        return false;

    if (!write_insert_extra(bw, command_symbol, src_len))
        return false;
    for (size_t i = 0; i < src_len; ++i) {
        if (literal_count > 0) {
            int index = literal_symbol_index(literal_symbols, literal_count, src[i]);
            if (!write_simple_literal_code(bw, index, literal_count))
                return false;
        } else {
            if (!write_prefix_bits(bw, src[i], 8))
                return false;
        }
    }
    return true;
}

struct Match {
    size_t pos = 0;
    size_t len = 0;
    size_t distance = 0;
};

struct CommandToken {
    size_t insert_pos = 0;
    size_t insert_len = 0;
    size_t copy_len = 0;
    size_t distance = 0;
    uint16_t command_symbol = 0;
    uint16_t distance_symbol = 0;
    uint32_t distance_extra = 0;
    int distance_extra_bits = 0;
};

struct TokenList {
    static constexpr int kMaxTokens = 128;
    CommandToken tokens[kMaxTokens];
    int count = 0;
};

static void push_distance_encoder(size_t distance, size_t last_distances[4]) noexcept {
    if (distance == last_distances[0])
        return;
    for (int i = 3; i > 0; --i)
        last_distances[i] = last_distances[i - 1];
    last_distances[0] = distance;
}

static uint32_t hash4(const uint8_t* src) noexcept {
    uint32_t v = static_cast<uint32_t>(src[0])
        | (static_cast<uint32_t>(src[1]) << 8)
        | (static_cast<uint32_t>(src[2]) << 16)
        | (static_cast<uint32_t>(src[3]) << 24);
    return (v * 0x1e35a7bdu) >> 16;
}

static void insert_hash_position(
    const uint8_t* src,
    size_t src_len,
    size_t pos,
    int* table) noexcept
{
    if (pos + 4 <= src_len)
        table[hash4(src + pos)] = static_cast<int>(pos);
}

static void insert_match_coverage(
    const uint8_t* src,
    size_t src_len,
    size_t pos,
    size_t len,
    int quality,
    int* table) noexcept
{
    if (len == 0)
        return;

    insert_hash_position(src, src_len, pos, table);
    if (len <= 16) {
        for (size_t p = pos + 1; p < pos + len; ++p)
            insert_hash_position(src, src_len, p, table);
        return;
    }

    size_t step = 32;
    if (quality >= 9)
        step = 8;
    else if (quality >= 5)
        step = 64;
    if (len >= 4096 && quality <= 5)
        step = 256;

    size_t end = pos + len;
    for (size_t p = pos + step; p < end; p += step)
        insert_hash_position(src, src_len, p, table);

    size_t tail = end > 16 ? end - 16 : pos + 1;
    for (size_t p = tail; p < end; ++p)
        insert_hash_position(src, src_len, p, table);
}

static bool has_sampled_match(
    const uint8_t* src,
    size_t src_len,
    int quality) noexcept
{
    if (src_len < 4096)
        return true;

    int table[4096];
    std::fill(table, table + 4096, -1);

    size_t step = quality >= 9 ? 1u : quality >= 5 ? 2u : 4u;
    size_t limit = std::min(src_len - 8, static_cast<size_t>(256 * 1024));
    for (size_t pos = 0; pos <= limit; pos += step) {
        uint32_t h = hash4(src + pos) & 4095u;
        int prev = table[h];
        table[h] = static_cast<int>(pos);
        if (prev < 0)
            continue;
        if (src[static_cast<size_t>(prev)] == src[pos] &&
            std::memcmp(src + static_cast<size_t>(prev), src + pos, 8) == 0)
            return true;
    }
    return false;
}

static Match find_match_from_table(
    const uint8_t* src,
    size_t src_len,
    size_t pos,
    const int* table) noexcept
{
    if (pos < 4 || pos + 8 > src_len)
        return Match{};

    int prev = table[hash4(src + pos)];
    if (prev < 0 || static_cast<size_t>(prev) >= pos)
        return Match{};

    size_t distance = pos - static_cast<size_t>(prev);
    if (distance > 65535)
        return Match{};

    size_t len = 0;
    size_t max_len = std::min(src_len - pos, static_cast<size_t>(65536));
    const uint8_t* a = src + pos;
    const uint8_t* b = src + static_cast<size_t>(prev);
    while (len + sizeof(size_t) <= max_len) {
        size_t av = 0;
        size_t bv = 0;
        std::memcpy(&av, a + len, sizeof(av));
        std::memcpy(&bv, b + len, sizeof(bv));
        if (av == bv) {
            len += sizeof(size_t);
            continue;
        }
        size_t diff = av ^ bv;
#if defined(__GNUC__) || defined(__clang__)
        if constexpr (sizeof(size_t) == 8)
            len += static_cast<size_t>(__builtin_ctzll(static_cast<unsigned long long>(diff)) / 8);
        else
            len += static_cast<size_t>(__builtin_ctz(static_cast<unsigned int>(diff)) / 8);
#else
        while (len < max_len && a[len] == b[len])
            ++len;
#endif
        break;
    }
    while (len < max_len && a[len] == b[len])
        ++len;
    if (len < 8)
        return Match{};
    return Match{pos, len, distance};
}

static bool add_unique_symbol(
    uint16_t* symbols,
    int capacity,
    int& count,
    uint16_t symbol) noexcept
{
    for (int i = 0; i < count; ++i)
        if (symbols[i] == symbol)
            return true;
    if (count == capacity)
        return false;
    symbols[count++] = symbol;
    std::sort(symbols, symbols + count);
    return true;
}

static bool build_command_tokens(
    const uint8_t* src,
    size_t src_len,
    int quality,
    TokenList& out,
    uint16_t* command_symbols,
    int& command_count,
    uint16_t* distance_symbols,
    int& distance_count) noexcept
{
    out = TokenList{};
    command_count = 0;
    distance_count = 0;

    size_t pos = 0;
    size_t anchor = 0;
    size_t last_distances[4] = {4, 11, 15, 16};
    int hash_table[65536];
    std::fill(hash_table, hash_table + 65536, -1);
    while (pos < src_len) {
        Match match = find_match_from_table(src, src_len, pos, hash_table);
        if (match.len == 0) {
            insert_hash_position(src, src_len, pos, hash_table);
            ++pos;
            continue;
        }

        if (out.count == TokenList::kMaxTokens)
            return false;
        CommandToken& token = out.tokens[out.count];
        token.insert_pos = anchor;
        token.insert_len = pos - anchor;
        token.copy_len = match.len;
        token.distance = match.distance;
        if (!command_symbol_for_insert_copy(token.insert_len, token.copy_len,
                                            token.command_symbol))
            return false;
        if (token.distance == last_distances[0]) {
            token.distance_symbol = 0;
            token.distance_extra = 0;
            token.distance_extra_bits = 0;
        } else if (!distance_code_for_distance(token.distance, token.distance_symbol,
                                              token.distance_extra,
                                              token.distance_extra_bits)) {
            return false;
        }
        if (!add_unique_symbol(command_symbols, 4, command_count,
                               token.command_symbol) ||
            !add_unique_symbol(distance_symbols, 4, distance_count,
                               token.distance_symbol))
            return false;
        ++out.count;
        if (token.distance_symbol != 0)
            push_distance_encoder(token.distance, last_distances);

        insert_match_coverage(src, src_len, pos, match.len, quality, hash_table);
        pos += match.len;
        anchor = pos;
    }

    size_t tail_len = src_len - anchor;
    if (tail_len > 0) {
        if (out.count == TokenList::kMaxTokens)
            return false;
        CommandToken& token = out.tokens[out.count];
        token.insert_pos = anchor;
        token.insert_len = tail_len;
        token.copy_len = 0;
        if (!command_symbol_for_insert(tail_len, token.command_symbol) ||
            !add_unique_symbol(command_symbols, 4, command_count,
                               token.command_symbol))
            return false;
        ++out.count;
    }

    if (out.count == 0 || distance_count == 0)
        return false;
    return true;
}

static bool write_symbol_from_simple_code(
    BitWriter& bw,
    const uint16_t* symbols,
    int count,
    uint16_t symbol) noexcept
{
    for (int i = 0; i < count; ++i) {
        if (symbols[i] != symbol)
            continue;
        return write_simple_literal_code(bw, i, count);
    }
    return false;
}

static bool write_compressed_token_block(
    BitWriter& bw,
    const uint8_t* src,
    size_t src_len,
    const TokenList& tokens,
    const uint16_t* command_symbols,
    int command_count,
    const uint16_t* distance_symbols,
    int distance_count) noexcept
{
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

    if (!write_fixed_8_literal_prefix_code(bw))
        return false;
    if (!write_simple_prefix_code(bw, 704, command_symbols, command_count))
        return false;
    if (!write_simple_prefix_code(bw, 64, distance_symbols, distance_count))
        return false;

    for (int i = 0; i < tokens.count; ++i) {
        const CommandToken& token = tokens.tokens[i];
        if (!write_symbol_from_simple_code(bw, command_symbols, command_count,
                                           token.command_symbol))
            return false;
        if (token.copy_len > 0) {
            if (!write_command_extra(bw, token.command_symbol,
                                     token.insert_len, token.copy_len))
                return false;
        } else if (!write_insert_extra(bw, token.command_symbol, token.insert_len)) {
            return false;
        }

        for (size_t j = 0; j < token.insert_len; ++j)
            if (!write_prefix_bits(bw, src[token.insert_pos + j], 8))
                return false;

        if (token.copy_len > 0) {
            if (!write_symbol_from_simple_code(bw, distance_symbols, distance_count,
                                               token.distance_symbol))
                return false;
            if (!bw.write_bits(token.distance_extra, token.distance_extra_bits))
                return false;
        }
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
        bool skip_compressed_attempt = quality <= 5 &&
            has_high_byte_diversity(input, src_len);
        TokenList tokens;
        uint16_t command_symbols[704] = {};
        uint16_t distance_symbols[64] = {};
        int command_count = 0;
        int distance_count = 0;
        if (!skip_compressed_attempt &&
            has_sampled_match(input, src_len, quality) &&
            build_command_tokens(input, src_len, quality, tokens, command_symbols,
                                 command_count, distance_symbols, distance_count)) {
            if (!write_compressed_token_block(bw, input, src_len, tokens,
                                             command_symbols, command_count,
                                             distance_symbols, distance_count))
                return 0;
            if (!bw.write_bits(1, 1)) return 0;
            if (!bw.write_bits(1, 1)) return 0;
            if (!bw.finish_zero()) return 0;
            return bw.overflow ? 0 : bw.bytes_written(dst);
        }

        uint16_t command_symbol = 0;
        bool eligible = !skip_compressed_attempt &&
            command_symbol_for_insert(src_len, command_symbol) &&
            has_simple_literal_alphabet(input, src_len);
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
