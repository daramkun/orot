#pragma once

#include <cstddef>
#include "../deflate/lz77.hpp"
#include "../deflate/deflate_block.hpp"

namespace orot { namespace deflate {

struct CompressConfig {
    LZ77Config     lz77;
    BlockTypeHint  block_hint;   /* Auto for levels >= 4, Fixed for 1-3 */
    size_t         block_size;   /* bytes per DEFLATE block              */
};

inline CompressConfig compress_config_for_level(int level) {
    CompressConfig c;
    c.lz77       = lz77_config_for_level(level);
    c.block_size = 65536;  /* 64 KB default block size */

    (void)level;
    c.block_hint = BlockTypeHint::Stored;

    return c;
}

} } /* namespace orot::deflate */
