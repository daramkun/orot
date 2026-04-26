/*
 * fuzz_zstd_decompress.cpp - libFuzzer harness for Zstandard frame decode.
 */
#include <cstddef>
#include <cstdint>
#include <vector>

#include "orot/zstd.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;

    std::vector<uint8_t> out(1024 * 1024);
    (void)orot_zstd_decompress(
        data, static_cast<int>(size),
        out.data(), static_cast<int>(out.size()));

    return 0;
}
