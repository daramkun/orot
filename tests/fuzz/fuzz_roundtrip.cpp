/*
 * fuzz_roundtrip.cpp — libFuzzer harness: compress then decompress.
 *
 * Build with:
 *   clang++ -fsanitize=fuzzer,address -O1 -std=c++20 fuzz_roundtrip.cpp \
 *           -I../../include -L../../build -ldeflate -o fuzz_roundtrip
 *
 * The fuzzer explores compression at various levels and formats,
 * verifying that compress → decompress always round-trips correctly.
 */
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

#include "orot/deflate.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;

    /* Use first byte to select level + format */
    const int            level  = (data[0] & 0x0F) % 13;
    const deflate_format fmt    = static_cast<deflate_format>((data[0] >> 4) % 3);
    const uint8_t*       src    = data + 1;
    const size_t         src_len = size - 1;

    const size_t bound = deflate_compress_bound(src_len, fmt);
    std::vector<uint8_t> comp(bound);
    std::vector<uint8_t> decomp(src_len + 1);

    const size_t clen = deflate_compress(src, src_len, comp.data(), bound, level, fmt);
    if (clen == 0 && src_len == 0) return 0;
    if (clen == 0) return 0;

    size_t actual = 0;
    const deflate_result r = deflate_decompress(
        comp.data(), clen, decomp.data(), decomp.size(), &actual, fmt);

    /* For a valid compress, decompress MUST succeed */
    if (r != DEFLATE_OK) __builtin_trap();
    if (actual != src_len) __builtin_trap();
    if (src_len > 0 && std::memcmp(src, decomp.data(), src_len) != 0) __builtin_trap();

    return 0;
}
