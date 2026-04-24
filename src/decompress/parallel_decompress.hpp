#pragma once

#include <cstddef>
#include <cstdint>
#include "../deflate_fwd.hpp"

namespace orot { namespace deflate {

/*
 * Parallel decompression for multi-member gzip files.
 *
 * Scans the input for gzip member boundaries (each member begins with
 * the gzip magic \x1f\x8b\x08), decompresses each member independently
 * on the thread pool, and concatenates output in order.
 *
 * Returns DEFLATE_DATA_ERROR if the input does not contain at least two
 * valid gzip members (caller should fall back to serial gzip_decompress).
 *
 * out_capacity must be sufficient for all members' output combined.
 * On success, *actual_out_size is set to total decompressed bytes.
 */
deflate_result parallel_gzip_decompress(
    const uint8_t* in,         size_t in_len,
    uint8_t*       out,        size_t out_capacity,
    size_t*        actual_out_size,
    int            num_threads = 0);   /* 0 = hardware_concurrency */

} } /* namespace orot::deflate */
