#include "lzma2_decompress.hpp"
#include "lzma_prob_model.hpp"
#include "lzma_decompress.hpp"

#include <cstring>
#include <memory>
#include <new>

namespace orot { namespace lzma {

/* ── LZMA2 chunk stream decompressor ─────────────────────────────────────── */

size_t lzma2_decompress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_cap) noexcept
{
    /* Allocate prob tables (reused across chunks, reset when required) */
    std::unique_ptr<LzmaProbTables> pt(new (std::nothrow) LzmaProbTables);
    if (!pt) return 0;

    /* Dict ring buffer: size driven by props (we discover on first LZMA chunk).
     * For now allocate max (32MB). On typical inputs this is fine. */
    static constexpr uint32_t kMaxDict = 1u << 25;  /* 32MB */
    std::unique_ptr<uint8_t[]> dict_buf(new (std::nothrow) uint8_t[kMaxDict]());
    if (!dict_buf) return 0;

    uint32_t dict_size = kMaxDict;
    uint32_t dict_pos  = 0;  /* next write position */
    bool     pt_valid  = false;
    uint8_t  props_byte = 0;

    /* Temp buffer for reconstructing LZMA-alone format per chunk */
    static constexpr size_t kChunkMax = 1u << 16;
    std::unique_ptr<uint8_t[]> tmp(new (std::nothrow) uint8_t[13 + kChunkMax + 64]);
    if (!tmp) return 0;

    const uint8_t* p   = src;
    const uint8_t* end = src + src_len;
    size_t out_pos = 0;

    for (;;) {
        if (p >= end) return 0;  /* expected EOS byte */
        uint8_t hdr = *p++;

        if (hdr == 0x00) break;  /* end-of-stream */

        if (hdr == 0x01 || hdr == 0x02) {
            /* Uncompressed chunk */
            if (p + 2 > end) return 0;
            uint16_t usz16;
            memcpy(&usz16, p, 2); p += 2;
            size_t usz = (size_t)usz16 + 1;

            if (p + usz > end) return 0;
            if (out_pos + usz > dst_cap) return 0;

            if (hdr == 0x01) {
                /* reset dict */
                dict_pos = 0;
                memset(dict_buf.get(), 0, dict_size);
            }

            memcpy(dst + out_pos, p, usz);
            for (size_t k = 0; k < usz; ++k) {
                dict_buf[dict_pos] = p[k];
                dict_pos = (dict_pos + 1 == dict_size) ? 0 : dict_pos + 1;
            }
            p += usz;
            out_pos += usz;
            continue;
        }

        if ((hdr & 0x80) == 0) return 0;  /* unknown chunk type */

        /* LZMA chunk */
        bool reset_props = (hdr & 0x20) != 0;
        bool reset_state = (hdr & 0x10) != 0;
        bool reset_dict  = (hdr & 0x08) != 0;

        if (p + 4 > end) return 0;
        uint16_t csz16, usz16;
        memcpy(&csz16, p, 2); p += 2;
        memcpy(&usz16, p, 2); p += 2;
        size_t csz = (size_t)csz16 + 1;  /* RC data size */
        size_t usz = (size_t)usz16 + 1;  /* uncompressed size */

        if (reset_props) {
            if (p >= end) return 0;
            props_byte = *p++;
            pt_valid   = false;
        }

        if (!pt_valid || reset_state) {
            uint8_t pb_lp = props_byte / 9;
            int lc = (int)(props_byte % 9);
            int lp = (int)(pb_lp % 5);
            int pb = (int)(pb_lp / 5);
            if (pb > 4 || lp > 4 || lc > 8 || lc + lp > 4) return 0;
            pt->reset(lc, lp);
            pt_valid = true;
        }

        if (reset_dict) {
            dict_pos = 0;
            memset(dict_buf.get(), 0, dict_size);
        }

        if (p + csz > end) return 0;
        if (out_pos + usz > dst_cap) return 0;

        /* Reconstruct LZMA-alone format:
         * [1B props][4B dict_size][8B usz][5B RC init][csz bytes RC data] */
        uint8_t* tb = tmp.get();
        tb[0] = props_byte;
        uint32_t ds = dict_size;
        memcpy(tb + 1, &ds, 4);
        uint64_t u64 = (uint64_t)usz;
        memcpy(tb + 5, &u64, 8);
        /* RC data starts with 5 init bytes already in the stream */
        memcpy(tb + 13, p, csz);
        p += csz;

        size_t got = lzma_decompress(tb, 13 + csz,
                                     dst + out_pos, dst_cap - out_pos);
        if (got == 0 || got != usz) return 0;

        /* Update dict buffer from decoded output */
        for (size_t k = 0; k < got; ++k) {
            dict_buf[dict_pos] = dst[out_pos + k];
            dict_pos = (dict_pos + 1 == dict_size) ? 0 : dict_pos + 1;
        }
        out_pos += got;
    }

    return out_pos;
}

} } /* namespace orot::lzma */
