/*
 * Brotli stream and parser tests.
 */
#include <cstdio>
#include <cstring>
#include <vector>

#include "orot/brotli.h"
#include "brotli/brotli_bit.hpp"
#include "brotli/brotli_huffman.hpp"
#include "brotli/brotli_meta.hpp"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d): %s\n", msg, __LINE__, #cond); \
        ++failures; \
    } \
} while (0)

static int brotli_alphabet_bits(int alphabet_size) {
    int bits = 0;
    int limit = 1;
    while (limit < alphabet_size) {
        limit <<= 1;
        ++bits;
    }
    return bits;
}

static bool write_single_symbol_prefix(
    orot::brotli::BitWriter& bw,
    int alphabet_size,
    uint32_t symbol)
{
    return bw.write_bits(1, 2)
        && bw.write_bits(0, 2)
        && bw.write_bits(symbol, brotli_alphabet_bits(alphabet_size));
}

static bool write_prefix_bits(orot::brotli::BitWriter& bw, uint32_t code, int len) {
    uint32_t reversed = 0;
    for (int i = 0; i < len; ++i)
        reversed = (reversed << 1) | ((code >> i) & 1u);
    return bw.write_bits(reversed, len);
}

static bool write_varlen_uint8_plus_one(
    orot::brotli::BitWriter& bw,
    uint32_t value)
{
    if (value == 1)
        return bw.write_bits(0, 1);
    if (value == 2)
        return bw.write_bits(1, 1) && bw.write_bits(0, 3);
    return false;
}

static bool write_two_type_block_category(orot::brotli::BitWriter& bw) {
    return write_varlen_uint8_plus_one(bw, 2)
        && write_single_symbol_prefix(bw, 4, 0)
        && write_single_symbol_prefix(bw, 26, 0)
        && bw.write_bits(0, 2);
}

static bool write_two_tree_context_map(
    orot::brotli::BitWriter& bw,
    int first_count,
    int second_count)
{
    if (!bw.write_bits(0, 1) ||
        !bw.write_bits(1, 2) ||
        !bw.write_bits(1, 2) ||
        !bw.write_bits(0, 1) ||
        !bw.write_bits(1, 1))
        return false;
    for (int i = 0; i < first_count; ++i)
        if (!bw.write_bits(0, 1))
            return false;
    for (int i = 0; i < second_count; ++i)
        if (!bw.write_bits(1, 1))
            return false;
    return bw.write_bits(0, 1);
}

static bool write_binary_context_map(
    orot::brotli::BitWriter& bw,
    const std::vector<uint8_t>& values)
{
    if (!bw.write_bits(0, 1) ||
        !bw.write_bits(1, 2) ||
        !bw.write_bits(1, 2) ||
        !bw.write_bits(0, 1) ||
        !bw.write_bits(1, 1))
        return false;
    for (uint8_t value : values)
        if (!bw.write_bits(value != 0 ? 1u : 0u, 1))
            return false;
    return bw.write_bits(0, 1);
}

static void test_simple_prefix_code() {
    uint8_t storage[16] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(1, 2), "simple prefix marker");
    CHECK(bw.write_bits(1, 2), "two-symbol prefix count");
    CHECK(bw.write_bits('A', 8), "first simple symbol");
    CHECK(bw.write_bits('B', 8), "second simple symbol");
    CHECK(bw.write_bits(0, 1), "decode A bit");
    CHECK(bw.write_bits(1, 1), "decode B bit");
    CHECK(bw.finish_zero(), "finish simple prefix bits");

    orot::brotli::BitReader br;
    br.init(storage, bw.bytes_written(storage));

    orot::brotli::PrefixCode code;
    CHECK(orot::brotli::read_simple_prefix_code(br, 256, code),
          "read two-symbol simple prefix code");
    CHECK(code.decode(br) == 'A', "decode first simple symbol");
    CHECK(code.decode(br) == 'B', "decode second simple symbol");
    CHECK(!br.error, "simple prefix decode has no bit error");

    bw.init(storage, sizeof(storage));
    CHECK(bw.write_bits(1, 2), "single simple prefix marker");
    CHECK(bw.write_bits(0, 2), "single-symbol prefix count");
    CHECK(bw.write_bits(17, 8), "single symbol value");
    CHECK(bw.finish_zero(), "finish single prefix bits");

    br.init(storage, bw.bytes_written(storage));
    CHECK(orot::brotli::read_simple_prefix_code(br, 256, code),
          "read one-symbol simple prefix code");
    CHECK(code.decode(br) == 17, "single-symbol prefix decodes without bits");
    CHECK(code.decode(br) == 17, "single-symbol prefix is reusable");
}

static void test_complex_prefix_code() {
    uint8_t storage[32] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    /* HSKIP=0, then the code-length-code lengths in Brotli order.
     * Only code-length symbol 1 is present, so the represented prefix code
     * expands to two symbols with length 1. */
    CHECK(bw.write_bits(0, 2), "complex prefix marker");
    CHECK(bw.write_bits(0b0111, 4), "code-length symbol 1 has length 1");
    for (int i = 1; i < 18; ++i)
        CHECK(bw.write_bits(0, 2), "remaining code-length symbol is zero");
    CHECK(bw.write_bits(0, 1), "decode first complex symbol");
    CHECK(bw.write_bits(1, 1), "decode second complex symbol");
    CHECK(bw.finish_zero(), "finish complex prefix bits");

    orot::brotli::BitReader br;
    br.init(storage, bw.bytes_written(storage));

    orot::brotli::PrefixCode code;
    CHECK(orot::brotli::read_prefix_code(br, 2, code),
          "read complex prefix code");
    CHECK(code.decode(br) == 0, "decode first complex prefix symbol");
    CHECK(code.decode(br) == 1, "decode second complex prefix symbol");
    CHECK(!br.error, "complex prefix decode has no bit error");
}

static void test_meta_helpers() {
    uint8_t storage[32] = {};
    orot::brotli::BitWriter bw;
    orot::brotli::BitReader br;

    bw.init(storage, sizeof(storage));
    CHECK(bw.write_bits(0, 1), "varlen value 1 marker");
    CHECK(bw.finish_zero(), "finish varlen value 1");
    br.init(storage, bw.bytes_written(storage));
    CHECK(orot::brotli::read_var_len_uint8_plus_one(br) == 1,
          "decode varlen value 1");

    bw.init(storage, sizeof(storage));
    CHECK(bw.write_bits(1, 1), "varlen extended marker");
    CHECK(bw.write_bits(0, 3), "varlen value 2 selector");
    CHECK(bw.finish_zero(), "finish varlen value 2");
    br.init(storage, bw.bytes_written(storage));
    CHECK(orot::brotli::read_var_len_uint8_plus_one(br) == 2,
          "decode varlen value 2");

    bw.init(storage, sizeof(storage));
    CHECK(bw.write_bits(1, 1), "varlen extended marker for extra");
    CHECK(bw.write_bits(2, 3), "varlen two extra bits selector");
    CHECK(bw.write_bits(1, 2), "varlen extra payload");
    CHECK(bw.finish_zero(), "finish varlen extra value");
    br.init(storage, bw.bytes_written(storage));
    CHECK(orot::brotli::read_var_len_uint8_plus_one(br) == 6,
          "decode varlen value with extra bits");

    br.init(storage, 0);
    CHECK(orot::brotli::read_var_len_uint8_plus_one(br) == -1,
          "varlen rejects truncated marker");

    bw.init(storage, sizeof(storage));
    CHECK(bw.write_bits(0, 2), "block count code zero extra");
    CHECK(bw.finish_zero(), "finish block count code zero");
    br.init(storage, bw.bytes_written(storage));
    CHECK(orot::brotli::read_block_count(br, 0) == 1,
          "decode block count base code");

    bw.init(storage, sizeof(storage));
    CHECK(bw.write_bits(3, 2), "block count code one extra");
    CHECK(bw.finish_zero(), "finish block count code one");
    br.init(storage, bw.bytes_written(storage));
    CHECK(orot::brotli::read_block_count(br, 1) == 8,
          "decode block count with extra bits");

    bw.init(storage, sizeof(storage));
    CHECK(bw.write_bits(0, 1), "context map rlemax zero");
    CHECK(bw.write_bits(1, 2), "context map simple prefix marker");
    CHECK(bw.write_bits(1, 2), "context map two-symbol prefix count");
    CHECK(bw.write_bits(0, 1), "context map symbol zero");
    CHECK(bw.write_bits(1, 1), "context map symbol one");
    CHECK(bw.write_bits(0, 1), "context map entry zero");
    CHECK(bw.write_bits(1, 1), "context map entry one");
    CHECK(bw.write_bits(0, 1), "context map entry zero again");
    CHECK(bw.write_bits(0, 1), "context map no inverse mtf");
    CHECK(bw.finish_zero(), "finish context map");

    br.init(storage, bw.bytes_written(storage));
    std::vector<uint8_t> context_map;
    CHECK(orot::brotli::read_context_map(br, 3, 2, context_map),
          "decode context map");
    CHECK(context_map.size() == 3, "context map size");
    CHECK(context_map[0] == 0 && context_map[1] == 1 && context_map[2] == 0,
          "context map entries");
}

static void test_compressed_meta_block_header() {
    uint8_t storage[64] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(0, 1), "one literal block type");
    CHECK(bw.write_bits(0, 1), "one command block type");
    CHECK(bw.write_bits(0, 1), "one distance block type");
    CHECK(bw.write_bits(0, 2), "npostfix zero");
    CHECK(bw.write_bits(0, 4), "ndirect zero");
    CHECK(bw.write_bits(0, 2), "literal context mode zero");
    CHECK(bw.write_bits(0, 1), "one literal tree");
    CHECK(bw.write_bits(0, 1), "one distance tree");
    CHECK(write_single_symbol_prefix(bw, 256, 0), "literal tree");
    CHECK(write_single_symbol_prefix(bw, 704, 0), "command tree");
    CHECK(write_single_symbol_prefix(bw, 64, 0), "distance tree");
    CHECK(bw.finish_zero(), "finish compressed meta-block header");

    orot::brotli::BitReader br;
    br.init(storage, bw.bytes_written(storage));

    orot::brotli::CompressedMetaBlockHeader header;
    CHECK(orot::brotli::read_compressed_meta_block_header(br, header),
          "read compressed meta-block header");
    CHECK(header.block_categories[0].num_types == 1, "literal block type count");
    CHECK(header.block_categories[1].num_types == 1, "command block type count");
    CHECK(header.block_categories[2].num_types == 1, "distance block type count");
    CHECK(header.npostfix == 0 && header.ndirect == 0, "distance parameters");
    CHECK(header.literal_context_modes.size() == 1 &&
          header.literal_context_modes[0] == 0,
          "literal context mode");
    CHECK(header.literal_context_map.size() == 64, "literal context map size");
    CHECK(header.distance_context_map.size() == 4, "distance context map size");
    CHECK(header.literal_trees.size() == 1, "literal tree count");
    CHECK(header.command_trees.size() == 1, "command tree count");
    CHECK(header.distance_trees.size() == 1, "distance tree count");
    CHECK(header.literal_trees[0].decode(br) == 0,
          "single literal tree decodes without bits");
}

static void test_compressed_insert_only_stream() {
    uint8_t storage[64] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(0b1011, 4), "insert-only wbits 22");
    CHECK(bw.write_bits(0, 1), "insert-only non-final meta-block");
    CHECK(bw.write_bits(0, 2), "insert-only mnibbles");
    CHECK(bw.write_bits(2, 16), "insert-only meta length three");
    CHECK(bw.write_bits(0, 1), "insert-only compressed flag");

    CHECK(bw.write_bits(0, 1), "insert-only one literal block type");
    CHECK(bw.write_bits(0, 1), "insert-only one command block type");
    CHECK(bw.write_bits(0, 1), "insert-only one distance block type");
    CHECK(bw.write_bits(0, 2), "insert-only npostfix zero");
    CHECK(bw.write_bits(0, 4), "insert-only ndirect zero");
    CHECK(bw.write_bits(0, 2), "insert-only literal context mode zero");
    CHECK(bw.write_bits(0, 1), "insert-only one literal tree");
    CHECK(bw.write_bits(0, 1), "insert-only one distance tree");

    CHECK(bw.write_bits(1, 2), "insert-only literal simple prefix marker");
    CHECK(bw.write_bits(2, 2), "insert-only literal three-symbol prefix");
    CHECK(bw.write_bits('a', 8), "insert-only literal symbol a");
    CHECK(bw.write_bits('b', 8), "insert-only literal symbol b");
    CHECK(bw.write_bits('c', 8), "insert-only literal symbol c");
    CHECK(write_single_symbol_prefix(bw, 704, 24), "insert-only command tree");
    CHECK(write_single_symbol_prefix(bw, 64, 0), "insert-only distance tree");

    CHECK(write_prefix_bits(bw, 0, 1), "insert-only literal a");
    CHECK(write_prefix_bits(bw, 2, 2), "insert-only literal b");
    CHECK(write_prefix_bits(bw, 3, 2), "insert-only literal c");

    CHECK(bw.write_bits(1, 1), "insert-only final meta-block");
    CHECK(bw.write_bits(1, 1), "insert-only final empty");
    CHECK(bw.finish_zero(), "finish insert-only stream");

    uint8_t output[8] = {};
    size_t actual = 0;
    int n = orot_brotli_decompress(storage, bw.bytes_written(storage),
                                   output, sizeof(output), &actual);
    CHECK(n == 3, "decode insert-only compressed stream");
    CHECK(actual == 3, "insert-only actual size");
    CHECK(std::memcmp(output, "abc", 3) == 0, "insert-only output bytes");
}

static void test_compressed_copy_stream() {
    uint8_t storage[64] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(0b1011, 4), "copy stream wbits 22");
    CHECK(bw.write_bits(0, 1), "copy stream non-final meta-block");
    CHECK(bw.write_bits(0, 2), "copy stream mnibbles");
    CHECK(bw.write_bits(5, 16), "copy stream meta length six");
    CHECK(bw.write_bits(0, 1), "copy stream compressed flag");

    CHECK(bw.write_bits(0, 1), "copy stream one literal block type");
    CHECK(bw.write_bits(0, 1), "copy stream one command block type");
    CHECK(bw.write_bits(0, 1), "copy stream one distance block type");
    CHECK(bw.write_bits(0, 2), "copy stream npostfix zero");
    CHECK(bw.write_bits(0, 4), "copy stream ndirect zero");
    CHECK(bw.write_bits(0, 2), "copy stream literal context mode zero");
    CHECK(bw.write_bits(0, 1), "copy stream one literal tree");
    CHECK(bw.write_bits(0, 1), "copy stream one distance tree");

    CHECK(bw.write_bits(1, 2), "copy stream literal simple prefix marker");
    CHECK(bw.write_bits(1, 2), "copy stream literal two-symbol prefix");
    CHECK(bw.write_bits('a', 8), "copy stream literal symbol a");
    CHECK(bw.write_bits('b', 8), "copy stream literal symbol b");
    CHECK(write_single_symbol_prefix(bw, 704, 146), "copy stream command tree");
    CHECK(write_single_symbol_prefix(bw, 64, 16), "copy stream distance tree");

    CHECK(write_prefix_bits(bw, 0, 1), "copy stream literal a");
    CHECK(write_prefix_bits(bw, 1, 1), "copy stream literal b");
    CHECK(bw.write_bits(1, 1), "copy stream distance extra for distance two");

    CHECK(bw.write_bits(1, 1), "copy stream final meta-block");
    CHECK(bw.write_bits(1, 1), "copy stream final empty");
    CHECK(bw.finish_zero(), "finish copy stream");

    uint8_t output[8] = {};
    size_t actual = 0;
    int n = orot_brotli_decompress(storage, bw.bytes_written(storage),
                                   output, sizeof(output), &actual);
    CHECK(n == 6, "decode compressed copy stream");
    CHECK(actual == 6, "copy stream actual size");
    CHECK(std::memcmp(output, "ababab", 6) == 0, "copy stream output bytes");
}

static void test_compressed_command_block_switch_stream() {
    uint8_t storage[96] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(0b1011, 4), "command switch wbits 22");
    CHECK(bw.write_bits(0, 1), "command switch non-final meta-block");
    CHECK(bw.write_bits(0, 2), "command switch mnibbles");
    CHECK(bw.write_bits(3, 16), "command switch meta length four");
    CHECK(bw.write_bits(0, 1), "command switch compressed flag");

    CHECK(bw.write_bits(0, 1), "command switch one literal block type");
    CHECK(write_two_type_block_category(bw), "command switch two command types");
    CHECK(bw.write_bits(0, 1), "command switch one distance block type");
    CHECK(bw.write_bits(0, 2), "command switch npostfix zero");
    CHECK(bw.write_bits(0, 4), "command switch ndirect zero");
    CHECK(bw.write_bits(0, 2), "command switch literal context mode zero");
    CHECK(bw.write_bits(0, 1), "command switch one literal tree");
    CHECK(bw.write_bits(0, 1), "command switch one distance tree");

    CHECK(write_single_symbol_prefix(bw, 256, 'a'), "command switch literal tree");
    CHECK(write_single_symbol_prefix(bw, 704, 136), "command switch command tree 0");
    CHECK(write_single_symbol_prefix(bw, 704, 8), "command switch command tree 1");
    CHECK(write_single_symbol_prefix(bw, 64, 16), "command switch distance tree");

    CHECK(bw.write_bits(0, 1), "command switch direct distance one");
    CHECK(bw.write_bits(0, 2), "command switch next block count one");

    CHECK(bw.write_bits(1, 1), "command switch final meta-block");
    CHECK(bw.write_bits(1, 1), "command switch final empty");
    CHECK(bw.finish_zero(), "finish command switch stream");

    uint8_t output[8] = {};
    size_t actual = 0;
    int n = orot_brotli_decompress(storage, bw.bytes_written(storage),
                                   output, sizeof(output), &actual);
    CHECK(n == 4, "decode command block switch stream");
    CHECK(actual == 4, "command switch actual size");
    CHECK(std::memcmp(output, "aaaa", 4) == 0, "command switch output bytes");
}

static void test_compressed_literal_block_switch_stream() {
    uint8_t storage[128] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(0b1011, 4), "literal switch wbits 22");
    CHECK(bw.write_bits(0, 1), "literal switch non-final meta-block");
    CHECK(bw.write_bits(0, 2), "literal switch mnibbles");
    CHECK(bw.write_bits(1, 16), "literal switch meta length two");
    CHECK(bw.write_bits(0, 1), "literal switch compressed flag");

    CHECK(write_two_type_block_category(bw), "literal switch two literal types");
    CHECK(bw.write_bits(0, 1), "literal switch one command block type");
    CHECK(bw.write_bits(0, 1), "literal switch one distance block type");
    CHECK(bw.write_bits(0, 2), "literal switch npostfix zero");
    CHECK(bw.write_bits(0, 4), "literal switch ndirect zero");
    CHECK(bw.write_bits(0, 2), "literal switch context mode 0");
    CHECK(bw.write_bits(0, 2), "literal switch context mode 1");
    CHECK(write_varlen_uint8_plus_one(bw, 2), "literal switch two literal trees");
    CHECK(write_two_tree_context_map(bw, 64, 64), "literal switch context map");
    CHECK(bw.write_bits(0, 1), "literal switch one distance tree");

    CHECK(write_single_symbol_prefix(bw, 256, 'a'), "literal switch literal tree 0");
    CHECK(write_single_symbol_prefix(bw, 256, 'b'), "literal switch literal tree 1");
    CHECK(write_single_symbol_prefix(bw, 704, 16), "literal switch command tree");
    CHECK(write_single_symbol_prefix(bw, 64, 0), "literal switch distance tree");

    CHECK(bw.write_bits(0, 2), "literal switch next block count one");

    CHECK(bw.write_bits(1, 1), "literal switch final meta-block");
    CHECK(bw.write_bits(1, 1), "literal switch final empty");
    CHECK(bw.finish_zero(), "finish literal switch stream");

    uint8_t output[8] = {};
    size_t actual = 0;
    int n = orot_brotli_decompress(storage, bw.bytes_written(storage),
                                   output, sizeof(output), &actual);
    CHECK(n == 2, "decode literal block switch stream");
    CHECK(actual == 2, "literal switch actual size");
    CHECK(std::memcmp(output, "ab", 2) == 0, "literal switch output bytes");
}

static void test_compressed_distance_block_switch_stream() {
    uint8_t storage[128] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(0b1011, 4), "distance switch wbits 22");
    CHECK(bw.write_bits(0, 1), "distance switch non-final meta-block");
    CHECK(bw.write_bits(0, 2), "distance switch mnibbles");
    CHECK(bw.write_bits(5, 16), "distance switch meta length six");
    CHECK(bw.write_bits(0, 1), "distance switch compressed flag");

    CHECK(bw.write_bits(0, 1), "distance switch one literal block type");
    CHECK(bw.write_bits(0, 1), "distance switch one command block type");
    CHECK(write_two_type_block_category(bw), "distance switch two distance types");
    CHECK(bw.write_bits(0, 2), "distance switch npostfix zero");
    CHECK(bw.write_bits(0, 4), "distance switch ndirect zero");
    CHECK(bw.write_bits(0, 2), "distance switch literal context mode zero");
    CHECK(bw.write_bits(0, 1), "distance switch one literal tree");
    CHECK(write_varlen_uint8_plus_one(bw, 2), "distance switch two distance trees");
    CHECK(write_two_tree_context_map(bw, 4, 4), "distance switch context map");

    CHECK(bw.write_bits(1, 2), "distance switch literal simple prefix marker");
    CHECK(bw.write_bits(1, 2), "distance switch literal two-symbol prefix");
    CHECK(bw.write_bits('a', 8), "distance switch literal symbol a");
    CHECK(bw.write_bits('b', 8), "distance switch literal symbol b");
    CHECK(bw.write_bits(1, 2), "distance switch command simple prefix marker");
    CHECK(bw.write_bits(1, 2), "distance switch command two-symbol prefix");
    CHECK(bw.write_bits(144, 10), "distance switch command symbol insert/copy");
    CHECK(bw.write_bits(128, 10), "distance switch command symbol copy");
    CHECK(write_single_symbol_prefix(bw, 64, 16), "distance switch distance tree 0");
    CHECK(write_single_symbol_prefix(bw, 64, 17), "distance switch distance tree 1");

    CHECK(write_prefix_bits(bw, 1, 1), "distance switch first command");
    CHECK(write_prefix_bits(bw, 0, 1), "distance switch literal a");
    CHECK(write_prefix_bits(bw, 1, 1), "distance switch literal b");
    CHECK(bw.write_bits(1, 1), "distance switch first distance extra");
    CHECK(write_prefix_bits(bw, 0, 1), "distance switch second command");
    CHECK(bw.write_bits(0, 2), "distance switch next block count one");
    CHECK(bw.write_bits(1, 1), "distance switch second distance extra");

    CHECK(bw.write_bits(1, 1), "distance switch final meta-block");
    CHECK(bw.write_bits(1, 1), "distance switch final empty");
    CHECK(bw.finish_zero(), "finish distance switch stream");

    uint8_t output[8] = {};
    size_t actual = 0;
    int n = orot_brotli_decompress(storage, bw.bytes_written(storage),
                                   output, sizeof(output), &actual);
    CHECK(n == 6, "decode distance block switch stream");
    CHECK(actual == 6, "distance switch actual size");
    CHECK(std::memcmp(output, "ababab", 6) == 0, "distance switch output bytes");
}

static void test_compressed_literal_context_mode_stream() {
    uint8_t storage[128] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(0b1011, 4), "literal context wbits 22");
    CHECK(bw.write_bits(0, 1), "literal context non-final meta-block");
    CHECK(bw.write_bits(0, 2), "literal context mnibbles");
    CHECK(bw.write_bits(1, 16), "literal context meta length two");
    CHECK(bw.write_bits(0, 1), "literal context compressed flag");

    CHECK(bw.write_bits(0, 1), "literal context one literal block type");
    CHECK(bw.write_bits(0, 1), "literal context one command block type");
    CHECK(bw.write_bits(0, 1), "literal context one distance block type");
    CHECK(bw.write_bits(0, 2), "literal context npostfix zero");
    CHECK(bw.write_bits(0, 4), "literal context ndirect zero");
    CHECK(bw.write_bits(1, 2), "literal context MSB6 mode");
    CHECK(write_varlen_uint8_plus_one(bw, 2), "literal context two literal trees");
    std::vector<uint8_t> literal_map(64, 0);
    literal_map[static_cast<size_t>('A' >> 2)] = 1;
    CHECK(write_binary_context_map(bw, literal_map), "literal context map");
    CHECK(bw.write_bits(0, 1), "literal context one distance tree");

    CHECK(write_single_symbol_prefix(bw, 256, 'A'), "literal context tree 0");
    CHECK(write_single_symbol_prefix(bw, 256, 'B'), "literal context tree 1");
    CHECK(write_single_symbol_prefix(bw, 704, 16), "literal context command tree");
    CHECK(write_single_symbol_prefix(bw, 64, 0), "literal context distance tree");

    CHECK(bw.write_bits(1, 1), "literal context final meta-block");
    CHECK(bw.write_bits(1, 1), "literal context final empty");
    CHECK(bw.finish_zero(), "finish literal context stream");

    uint8_t output[8] = {};
    size_t actual = 0;
    int n = orot_brotli_decompress(storage, bw.bytes_written(storage),
                                   output, sizeof(output), &actual);
    CHECK(n == 2, "decode literal context stream");
    CHECK(actual == 2, "literal context actual size");
    CHECK(std::memcmp(output, "AB", 2) == 0, "literal context output bytes");
}

static void test_compressed_utf8_context_mode_stream() {
    uint8_t storage[128] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(0b1011, 4), "utf8 context wbits 22");
    CHECK(bw.write_bits(0, 1), "utf8 context non-final meta-block");
    CHECK(bw.write_bits(0, 2), "utf8 context mnibbles");
    CHECK(bw.write_bits(1, 16), "utf8 context meta length two");
    CHECK(bw.write_bits(0, 1), "utf8 context compressed flag");

    CHECK(bw.write_bits(0, 1), "utf8 context one literal block type");
    CHECK(bw.write_bits(0, 1), "utf8 context one command block type");
    CHECK(bw.write_bits(0, 1), "utf8 context one distance block type");
    CHECK(bw.write_bits(0, 2), "utf8 context npostfix zero");
    CHECK(bw.write_bits(0, 4), "utf8 context ndirect zero");
    CHECK(bw.write_bits(2, 2), "utf8 context mode");
    CHECK(write_varlen_uint8_plus_one(bw, 2), "utf8 context two literal trees");
    std::vector<uint8_t> literal_map(64, 0);
    literal_map[48] = 1;
    CHECK(write_binary_context_map(bw, literal_map), "utf8 context map");
    CHECK(bw.write_bits(0, 1), "utf8 context one distance tree");

    CHECK(write_single_symbol_prefix(bw, 256, 'A'), "utf8 context tree 0");
    CHECK(write_single_symbol_prefix(bw, 256, 'B'), "utf8 context tree 1");
    CHECK(write_single_symbol_prefix(bw, 704, 16), "utf8 context command tree");
    CHECK(write_single_symbol_prefix(bw, 64, 0), "utf8 context distance tree");

    CHECK(bw.write_bits(1, 1), "utf8 context final meta-block");
    CHECK(bw.write_bits(1, 1), "utf8 context final empty");
    CHECK(bw.finish_zero(), "finish utf8 context stream");

    uint8_t output[8] = {};
    size_t actual = 0;
    int n = orot_brotli_decompress(storage, bw.bytes_written(storage),
                                   output, sizeof(output), &actual);
    CHECK(n == 2, "decode utf8 context stream");
    CHECK(actual == 2, "utf8 context actual size");
    CHECK(std::memcmp(output, "AB", 2) == 0, "utf8 context output bytes");
}

static void test_compressed_signed_context_mode_stream() {
    uint8_t storage[128] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(0b1011, 4), "signed context wbits 22");
    CHECK(bw.write_bits(0, 1), "signed context non-final meta-block");
    CHECK(bw.write_bits(0, 2), "signed context mnibbles");
    CHECK(bw.write_bits(1, 16), "signed context meta length two");
    CHECK(bw.write_bits(0, 1), "signed context compressed flag");

    CHECK(bw.write_bits(0, 1), "signed context one literal block type");
    CHECK(bw.write_bits(0, 1), "signed context one command block type");
    CHECK(bw.write_bits(0, 1), "signed context one distance block type");
    CHECK(bw.write_bits(0, 2), "signed context npostfix zero");
    CHECK(bw.write_bits(0, 4), "signed context ndirect zero");
    CHECK(bw.write_bits(3, 2), "signed context mode");
    CHECK(write_varlen_uint8_plus_one(bw, 2), "signed context two literal trees");
    std::vector<uint8_t> literal_map(64, 0);
    literal_map[24] = 1;
    CHECK(write_binary_context_map(bw, literal_map), "signed context map");
    CHECK(bw.write_bits(0, 1), "signed context one distance tree");

    CHECK(write_single_symbol_prefix(bw, 256, '@'), "signed context tree 0");
    CHECK(write_single_symbol_prefix(bw, 256, 'A'), "signed context tree 1");
    CHECK(write_single_symbol_prefix(bw, 704, 16), "signed context command tree");
    CHECK(write_single_symbol_prefix(bw, 64, 0), "signed context distance tree");

    CHECK(bw.write_bits(1, 1), "signed context final meta-block");
    CHECK(bw.write_bits(1, 1), "signed context final empty");
    CHECK(bw.finish_zero(), "finish signed context stream");

    uint8_t output[8] = {};
    size_t actual = 0;
    int n = orot_brotli_decompress(storage, bw.bytes_written(storage),
                                   output, sizeof(output), &actual);
    CHECK(n == 2, "decode signed context stream");
    CHECK(actual == 2, "signed context actual size");
    CHECK(std::memcmp(output, "@A", 2) == 0, "signed context output bytes");
}

static void test_compressed_distance_context_stream() {
    uint8_t storage[128] = {};
    orot::brotli::BitWriter bw;
    bw.init(storage, sizeof(storage));

    CHECK(bw.write_bits(0b1011, 4), "distance context wbits 22");
    CHECK(bw.write_bits(0, 1), "distance context non-final meta-block");
    CHECK(bw.write_bits(0, 2), "distance context mnibbles");
    CHECK(bw.write_bits(5, 16), "distance context meta length six");
    CHECK(bw.write_bits(0, 1), "distance context compressed flag");

    CHECK(bw.write_bits(0, 1), "distance context one literal block type");
    CHECK(bw.write_bits(0, 1), "distance context one command block type");
    CHECK(bw.write_bits(0, 1), "distance context one distance block type");
    CHECK(bw.write_bits(0, 2), "distance context npostfix zero");
    CHECK(bw.write_bits(0, 4), "distance context ndirect zero");
    CHECK(bw.write_bits(0, 2), "distance context literal mode zero");
    CHECK(bw.write_bits(0, 1), "distance context one literal tree");
    CHECK(write_varlen_uint8_plus_one(bw, 2), "distance context two distance trees");
    std::vector<uint8_t> distance_map = {0, 0, 1, 0};
    CHECK(write_binary_context_map(bw, distance_map), "distance context map");

    CHECK(bw.write_bits(1, 2), "distance context literal simple prefix marker");
    CHECK(bw.write_bits(1, 2), "distance context literal two-symbol prefix");
    CHECK(bw.write_bits('a', 8), "distance context literal symbol a");
    CHECK(bw.write_bits('b', 8), "distance context literal symbol b");
    CHECK(write_single_symbol_prefix(bw, 704, 146), "distance context command tree");
    CHECK(write_single_symbol_prefix(bw, 64, 17), "distance context tree 0");
    CHECK(write_single_symbol_prefix(bw, 64, 16), "distance context tree 1");

    CHECK(write_prefix_bits(bw, 0, 1), "distance context literal a");
    CHECK(write_prefix_bits(bw, 1, 1), "distance context literal b");
    CHECK(bw.write_bits(1, 1), "distance context distance extra");

    CHECK(bw.write_bits(1, 1), "distance context final meta-block");
    CHECK(bw.write_bits(1, 1), "distance context final empty");
    CHECK(bw.finish_zero(), "finish distance context stream");

    uint8_t output[8] = {};
    size_t actual = 0;
    int n = orot_brotli_decompress(storage, bw.bytes_written(storage),
                                   output, sizeof(output), &actual);
    CHECK(n == 6, "decode distance context stream");
    CHECK(actual == 6, "distance context actual size");
    CHECK(std::memcmp(output, "ababab", 6) == 0, "distance context output bytes");
}

int main() {
    test_simple_prefix_code();
    test_complex_prefix_code();
    test_meta_helpers();
    test_compressed_meta_block_header();
    test_compressed_insert_only_stream();
    test_compressed_copy_stream();
    test_compressed_command_block_switch_stream();
    test_compressed_literal_block_switch_stream();
    test_compressed_distance_block_switch_stream();
    test_compressed_literal_context_mode_stream();
    test_compressed_utf8_context_mode_stream();
    test_compressed_signed_context_mode_stream();
    test_compressed_distance_context_stream();

    const char* text = "brotli api scaffold";
    const size_t len = std::strlen(text);
    const size_t bound = orot_brotli_compress_bound(len);

    CHECK(bound >= len, "compress bound is conservative");

    std::vector<uint8_t> out(bound);
    int n = orot_brotli_compress(text, len,
                                 out.data(), out.size(),
                                 OROT_BROTLI_QUALITY_DEFAULT,
                                 OROT_BROTLI_LGWIN_DEFAULT);
    CHECK(n > 0, "compress uncompressed meta-block stream");
    const int clen = n;

    n = orot_brotli_compress(text, len,
                             out.data(), out.size(),
                             -1,
                             OROT_BROTLI_LGWIN_DEFAULT);
    CHECK(n == -1, "invalid quality rejected");

    n = orot_brotli_compress(text, len,
                             out.data(), out.size(),
                             OROT_BROTLI_QUALITY_DEFAULT,
                             OROT_BROTLI_LGWIN_MAX + 1);
    CHECK(n == -1, "invalid lgwin rejected");

    std::vector<uint8_t> tiny(1);
    n = orot_brotli_compress(text, len,
                             tiny.data(), tiny.size(),
                             OROT_BROTLI_QUALITY_DEFAULT,
                             OROT_BROTLI_LGWIN_DEFAULT);
    CHECK(n == -2, "small output buffer rejected");

    std::vector<uint8_t> decompressed(len + 16, 0);
    size_t actual = 0;
    n = orot_brotli_decompress(out.data(), (size_t)clen,
                               decompressed.data(), decompressed.size(),
                               &actual);
    CHECK(n == (int)len, "decompress uncompressed meta-block stream");
    CHECK(actual == len, "decompress reports actual size");
    CHECK(std::memcmp(text, decompressed.data(), len) == 0, "roundtrip bytes match");

    const char* multi_copy_text =
        "abcdefghABCDEFGHzzzzzzzz"
        "abcdefghABCDEFGHyyyyyyyy"
        "zzzzzzzz12345678"
        "abcdefghABCDEFGH";
    const size_t multi_copy_len = std::strlen(multi_copy_text);
    std::vector<uint8_t> multi_copy_out(orot_brotli_compress_bound(multi_copy_len));
    n = orot_brotli_compress(multi_copy_text, multi_copy_len,
                             multi_copy_out.data(), multi_copy_out.size(),
                             OROT_BROTLI_QUALITY_DEFAULT,
                             OROT_BROTLI_LGWIN_DEFAULT);
    CHECK(n > 0, "compress multi-copy stream");
    decompressed.assign(multi_copy_len + 16, 0);
    actual = 0;
    n = orot_brotli_decompress(multi_copy_out.data(), (size_t)n,
                               decompressed.data(), decompressed.size(),
                               &actual);
    CHECK(n == (int)multi_copy_len, "decompress multi-copy stream");
    CHECK(actual == multi_copy_len, "multi-copy actual size");
    CHECK(std::memcmp(multi_copy_text, decompressed.data(), multi_copy_len) == 0,
          "multi-copy roundtrip bytes match");

    n = orot_brotli_decompress(out.data(), (size_t)clen,
                               tiny.data(), tiny.size(),
                               nullptr);
    CHECK(n == -2, "decompress reports small output buffer");

    uint8_t bad[] = {0xFF, 0xFF};
    n = orot_brotli_decompress(bad, sizeof(bad),
                               decompressed.data(), decompressed.size(),
                               nullptr);
    CHECK(n == -3, "decompress rejects malformed stream");

    uint8_t truncated_compressed[8] = {};
    orot::brotli::BitWriter bw;
    bw.init(truncated_compressed, sizeof(truncated_compressed));
    CHECK(bw.write_bits(0b1011, 4), "compressed test wbits 22");
    CHECK(bw.write_bits(0, 1), "compressed test non-final meta-block");
    CHECK(bw.write_bits(0, 2), "compressed test mnibbles");
    CHECK(bw.write_bits(0, 16), "compressed test meta length one");
    CHECK(bw.write_bits(0, 1), "compressed test is compressed");
    CHECK(bw.finish_zero(), "finish truncated compressed stream");
    n = orot_brotli_decompress(truncated_compressed,
                               bw.bytes_written(truncated_compressed),
                               decompressed.data(), decompressed.size(),
                               nullptr);
    CHECK(n == -3, "truncated compressed meta-block header is malformed");

    const uint8_t brotli_cli_compressed[] = {
        0x1f, 0x16, 0x00, 0x00, 0x24, 0x40, 0x6a,
        0x10, 0x65, 0xea, 0xf0, 0x9c, 0x3e
    };
    n = orot_brotli_decompress(brotli_cli_compressed,
                               sizeof(brotli_cli_compressed),
                               decompressed.data(), decompressed.size(),
                               &actual);
    CHECK(n == 23, "decompress brotli CLI q5 hello stream");
    CHECK(actual == 23, "brotli CLI q5 hello actual size");
    CHECK(std::memcmp(decompressed.data(), "hello hello hello hello", 23) == 0,
          "brotli CLI q5 hello output bytes");

    const uint8_t brotli_cli_q0_abc[] = {
        0x0f, 0x01, 0x80, 0x61, 0x62, 0x63, 0x03
    };
    n = orot_brotli_decompress(brotli_cli_q0_abc,
                               sizeof(brotli_cli_q0_abc),
                               decompressed.data(), decompressed.size(),
                               &actual);
    CHECK(n == 3, "decompress brotli CLI q0 abc stream");
    CHECK(actual == 3, "brotli CLI q0 abc actual size");
    CHECK(std::memcmp(decompressed.data(), "abc", 3) == 0,
          "brotli CLI q0 abc output bytes");

    const uint8_t brotli_cli_q5_repeated_a[] = {
        0x1f, 0x1f, 0x00, 0x00, 0x24, 0xc2, 0xa2, 0x99, 0x40, 0x02
    };
    n = orot_brotli_decompress(brotli_cli_q5_repeated_a,
                               sizeof(brotli_cli_q5_repeated_a),
                               decompressed.data(), decompressed.size(),
                               &actual);
    CHECK(n == 32, "decompress brotli CLI q5 repeated-a stream");
    CHECK(actual == 32, "brotli CLI q5 repeated-a actual size");
    CHECK(std::memcmp(decompressed.data(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32) == 0,
          "brotli CLI q5 repeated-a output bytes");

    std::vector<uint8_t> empty_compressed(orot_brotli_compress_bound(0));
    n = orot_brotli_compress(nullptr, 0,
                             empty_compressed.data(), empty_compressed.size(),
                             OROT_BROTLI_QUALITY_DEFAULT,
                             OROT_BROTLI_LGWIN_DEFAULT);
    CHECK(n > 0, "compress empty stream");
    actual = 999;
    int dlen = orot_brotli_decompress(empty_compressed.data(), (size_t)n,
                                      decompressed.data(), decompressed.size(),
                                      &actual);
    CHECK(dlen == 0, "decompress empty stream");
    CHECK(actual == 0, "empty stream actual size");

    auto cpp_compressed = orot::brotli_api::compress(
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(text), len));
    CHECK(!cpp_compressed.empty(), "C++ compress wrapper returns data");

    auto cpp_decompressed = orot::brotli_api::decompress(
        std::span<const uint8_t>(cpp_compressed.data(), cpp_compressed.size()),
        256);
    CHECK(cpp_decompressed.size() == len, "C++ decompress wrapper returns data");
    CHECK(std::memcmp(text, cpp_decompressed.data(), len) == 0,
          "C++ wrapper roundtrip bytes match");

    if (failures == 0) {
        std::printf("All Brotli stream and parser tests passed.\n");
        return 0;
    }

    std::fprintf(stderr, "%d Brotli stream/parser test(s) failed.\n", failures);
    return 1;
}
