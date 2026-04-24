/*
 * fuzz_decompress.cpp — libFuzzer harness: raw decompressor with arbitrary bytes.
 *
 * This harness feeds arbitrary byte sequences into the decompressor,
 * checking that it never crashes (it may return error codes).
 *
 * Build with:
 *   clang++ -fsanitize=fuzzer,address -O1 -std=c++20 fuzz_decompress.cpp \
 *           -I../../include -L../../build -ldeflate -o fuzz_decompress
 */
#include <cstdint>
#include <cstddef>
#include <vector>

#include "orot/deflate.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;

    const deflate_format fmt = static_cast<deflate_format>((data[0]) % 3);
    const uint8_t* src    = data + 1;
    const size_t   srclen = size > 1 ? size - 1 : 0;

    /* Use a generous output buffer */
    const size_t out_capacity = 1024 * 1024;  /* 1 MB max output */
    std::vector<uint8_t> out(out_capacity);
    size_t actual = 0;

    /* Result can be anything — we just must not crash */
    (void)deflate_decompress(src, srclen, out.data(), out_capacity, &actual, fmt);

    return 0;
}
