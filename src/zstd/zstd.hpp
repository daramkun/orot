#pragma once

#include <cstdint>

namespace orot { namespace zstd {

static constexpr uint32_t ZSTD_MAGIC = 0xFD2FB528u;
static constexpr int ZSTD_MAX_LEVEL = 9;
static constexpr int ZSTD_MIN_LEVEL = 1;
static constexpr int ZSTD_BLOCK_MAX_SIZE = 128 * 1024;

int zstd_compress_bound(int src_len) noexcept;

int zstd_compress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap,
    int level) noexcept;

int zstd_compress_dict(
    const uint8_t* src, int src_len,
    const uint8_t* dict, int dict_len,
    uint32_t dict_id,
    uint8_t* dst, int dst_cap,
    int level) noexcept;

int zstd_decompress(
    const uint8_t* src, int src_len,
    uint8_t* dst, int dst_cap) noexcept;

int zstd_decompress_dict(
    const uint8_t* src, int src_len,
    const uint8_t* dict, int dict_len,
    uint32_t expected_dict_id,
    uint8_t* dst, int dst_cap) noexcept;

} } /* namespace orot::zstd */
