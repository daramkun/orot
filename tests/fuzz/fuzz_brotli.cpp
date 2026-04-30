/*
 * fuzz_brotli.cpp - libFuzzer harness for Brotli whole-buffer APIs.
 */

#include "orot/brotli.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<uint8_t> decoded(size * 64 + 1024);
    size_t actual = 0;
    int dlen = orot_brotli_decompress(
        data, size, decoded.data(), decoded.size(), &actual);
    if (dlen >= 0 && actual <= decoded.size()) {
        std::vector<uint8_t> recompressed(orot_brotli_compress_bound(actual));
        int clen = orot_brotli_compress(
            decoded.data(), actual,
            recompressed.data(), recompressed.size(),
            OROT_BROTLI_QUALITY_DEFAULT,
            OROT_BROTLI_LGWIN_DEFAULT);
        if (clen > 0) {
            std::vector<uint8_t> roundtrip(actual + 16);
            size_t roundtrip_actual = 0;
            (void)orot_brotli_decompress(
                recompressed.data(), static_cast<size_t>(clen),
                roundtrip.data(), roundtrip.size(), &roundtrip_actual);
        }
    }

    if (size <= 4096) {
        std::vector<uint8_t> compressed(orot_brotli_compress_bound(size));
        int clen = orot_brotli_compress(
            data, size, compressed.data(), compressed.size(),
            OROT_BROTLI_QUALITY_DEFAULT,
            OROT_BROTLI_LGWIN_DEFAULT);
        if (clen > 0) {
            std::vector<uint8_t> out(size + 16);
            size_t out_size = 0;
            (void)orot_brotli_decompress(
                compressed.data(), static_cast<size_t>(clen),
                out.data(), out.size(), &out_size);
        }
    }
    return 0;
}
