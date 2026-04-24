/*
 * Huffman coding unit tests.
 */
#include <cassert>
#include <cstdio>
#include <cstring>

#include "core/huffman.hpp"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        ++failures; \
    } \
} while (0)

using namespace orot::deflate;

static void test_fixed_litlen() {
    HuffEncTable t;
    build_fixed_litlen_enc(t);

    /* RFC 1951: literal 'A' (0x41=65) uses 8-bit code starting at 0x30+65=0x71 */
    CHECK(t.lens[65]  == 8, "fixed/litlen: literal A len==8");
    CHECK(t.lens[256] == 7, "fixed/litlen: EOB len==7");
    CHECK(t.lens[144] == 9, "fixed/litlen: code 144 len==9");
    CHECK(t.lens[280] == 8, "fixed/litlen: code 280 len==8");
    std::printf("PASS: fixed litlen table\n");
}

static void test_fixed_dist() {
    HuffDistTable t;
    build_fixed_dist_enc(t);
    for (int i = 0; i < 30; ++i)
        CHECK(t.lens[i] == 5, "fixed/dist: all 5-bit");
    std::printf("PASS: fixed dist table\n");
}

static void test_length_tables() {
    /* Verify RFC 1951 length/distance table values */
    CHECK(LENGTH_BASE[0]  == 3,   "len_base[0]==3");
    CHECK(LENGTH_BASE[28] == 258, "len_base[28]==258");
    CHECK(DIST_BASE[0]    == 1,   "dist_base[0]==1");
    CHECK(DIST_BASE[29]   == 24577, "dist_base[29]==24577");
    CHECK(length_to_code(3)   == 0, "length_to_code(3)==0");
    CHECK(length_to_code(258) == 28,"length_to_code(258)==28");
    CHECK(dist_to_code(1)     == 0, "dist_to_code(1)==0");
    std::printf("PASS: length/distance tables\n");
}

static void test_huffman_lengths() {
    /* Uniform distribution: all codes should have equal length */
    uint32_t freqs[8] = {1,1,1,1,1,1,1,1};
    uint8_t  lens[8]  = {};
    build_huffman_lengths(freqs, 8, lens, 15);
    for (int i = 0; i < 8; ++i)
        CHECK(lens[i] == 3, "uniform 8 sym: lens==3");
    std::printf("PASS: huffman lengths (uniform)\n");
}

static void test_canonical_codes() {
    /* Build codes from known lengths, verify no two codes are equal */
    uint8_t  lens[8]  = {2,2,2,3,3,3,3,3};
    uint16_t codes[8] = {};
    build_enc_table_from_lens(lens, 8, codes);

    /* Verify uniqueness */
    for (int i = 0; i < 8; ++i)
        for (int j = i + 1; j < 8; ++j)
            if (lens[i] == lens[j])
                CHECK(codes[i] != codes[j], "canonical codes unique");

    std::printf("PASS: canonical code uniqueness\n");
}

int main() {
    test_fixed_litlen();
    test_fixed_dist();
    test_length_tables();
    test_huffman_lengths();
    test_canonical_codes();

    if (failures == 0) {
        std::printf("\nAll huffman tests PASSED.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}
