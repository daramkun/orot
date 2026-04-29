#pragma once

#include <cstddef>
#include <cstdint>

namespace orot { namespace brotli {

static constexpr int kQualityMin = 0;
static constexpr int kQualityMax = 11;
static constexpr int kLgWinMin = 10;
static constexpr int kLgWinMax = 24;

size_t brotli_compress_bound(size_t src_len) noexcept;

size_t brotli_compress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_cap,
    int quality, int lgwin) noexcept;

} } /* namespace orot::brotli */
