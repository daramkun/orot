#include "brotli_meta.hpp"
#include "brotli_huffman.hpp"

namespace orot { namespace brotli {

int read_var_len_uint8_plus_one(BitReader& br) noexcept {
    uint32_t marker = br.read_bits(1);
    if (br.error)
        return -1;
    if (marker == 0)
        return 1;
    int n = static_cast<int>(br.read_bits(3));
    if (br.error)
        return -1;
    if (n == 0)
        return 2;
    uint32_t extra = br.read_bits(n);
    if (br.error)
        return -1;
    return 1 + (1 << n) + static_cast<int>(extra);
}

int read_block_count(BitReader& br, int code) noexcept {
    static const uint8_t kExtraBits[26] = {
        2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5,
        5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24
    };
    static const int kBase[26] = {
        1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81, 97, 113,
        145, 177, 209, 241, 305, 369, 497, 753, 1265, 2289,
        4337, 8433, 16625
    };

    if (code < 0 || code >= 26)
        return -1;
    int extra_bits = kExtraBits[code];
    uint32_t extra = br.read_bits(extra_bits);
    if (br.error)
        return -1;
    return kBase[code] + static_cast<int>(extra);
}

static int read_rlemax(BitReader& br) noexcept {
    uint32_t marker = br.read_bits(1);
    if (br.error)
        return -1;
    if (marker == 0)
        return 0;
    uint32_t tail = br.read_bits(4);
    if (br.error)
        return -1;
    return 1 + static_cast<int>(tail);
}

static void inverse_mtf(std::vector<uint8_t>& values) {
    uint8_t mtf[256];
    for (int i = 0; i < 256; ++i)
        mtf[i] = static_cast<uint8_t>(i);

    for (uint8_t& v : values) {
        uint8_t index = v;
        uint8_t value = mtf[index];
        v = value;
        while (index != 0) {
            mtf[index] = mtf[index - 1];
            --index;
        }
        mtf[0] = value;
    }
}

bool read_context_map(
    BitReader& br,
    int context_map_size,
    int num_trees,
    std::vector<uint8_t>& out) noexcept
{
    if (context_map_size < 0 || num_trees <= 0 || num_trees > 256)
        return false;

    out.clear();
    out.reserve(static_cast<size_t>(context_map_size));

    int rlemax = read_rlemax(br);
    if (rlemax < 0 || rlemax > 16)
        return false;

    PrefixCode code;
    if (!read_prefix_code(br, num_trees + rlemax, code))
        return false;

    while ((int)out.size() < context_map_size) {
        int sym = code.decode(br);
        if (sym < 0 || br.error)
            return false;

        if (sym == 0) {
            out.push_back(0);
        } else if (sym <= rlemax) {
            int repeat = (1 << sym) + static_cast<int>(br.read_bits(sym));
            if (br.error || (int)out.size() + repeat > context_map_size)
                return false;
            out.insert(out.end(), static_cast<size_t>(repeat), 0);
        } else {
            int value = sym - rlemax;
            if (value < 1 || value >= num_trees)
                return false;
            out.push_back(static_cast<uint8_t>(value));
        }
    }

    bool imtf = br.read_bits(1) != 0;
    if (br.error)
        return false;
    if (imtf)
        inverse_mtf(out);

    return true;
}

} } /* namespace orot::brotli */
