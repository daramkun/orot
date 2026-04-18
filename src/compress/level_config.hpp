#pragma once

#include <cstddef>
#include "../core/lz77.hpp"
#include "../core/deflate_block.hpp"

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

    if (level <= 0)
        c.block_hint = BlockTypeHint::Stored;
    else if (level <= 3)
        c.block_hint = BlockTypeHint::Fixed;
    else
        c.block_hint = BlockTypeHint::Auto;

    return c;
}

} } /* namespace orot::deflate */
