#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace orot::bzip2 {

/* Forward BWT: transform block, return primary index.
   out must be at least len bytes. */
uint32_t bwt_transform(const uint8_t* in, uint8_t* out, uint32_t len,
                       std::vector<uint32_t>& sa_buf);

/* Inverse BWT: reconstruct original from BWT output + primary index. */
void bwt_inverse(const uint8_t* in, uint8_t* out, uint32_t len,
                 uint32_t primary_index, std::vector<uint32_t>& tmp_buf);

} // namespace orot::bzip2
