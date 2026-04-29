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

int main() {
    test_simple_prefix_code();
    test_complex_prefix_code();
    test_meta_helpers();
    test_compressed_meta_block_header();
    test_compressed_insert_only_stream();
    test_compressed_copy_stream();

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
                               nullptr);
    CHECK(n == -4, "valid compressed meta-block reaches unsupported body decoder");

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
