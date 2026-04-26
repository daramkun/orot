#include "huf.hpp"
#include "fse.hpp"

#include <cstring>

namespace orot { namespace zstd {

namespace {

/* Build Huffman decode table from weight array.
   weight[s]: 0 = absent, 1..max_bits = weight.
   code_len = max_bits + 1 - weight  (for weight > 0).
   Returns max_bits used, or -1 on error. */
int build_from_weights(HufDTable& ht,
                       const uint8_t* weights, int n_weights) noexcept {
    if (n_weights <= 0 || n_weights > HUF_MAX_SYMS) return -1;

    /* Find max weight → max_bits */
    int max_w = 0;
    for (int i = 0; i < n_weights; ++i) {
        if (weights[i] > max_w) max_w = weights[i];
    }
    if (max_w == 0) return -1;

    /* Infer the last symbol's weight so that sum(2^(w-1)) = 2^max_w.
       zstd does NOT store the last symbol (it's derived). The caller
       should pass n_weights = actual symbols stored; we infer symbol n_weights. */
    uint32_t weight_sum = 0;
    for (int i = 0; i < n_weights; ++i) {
        if (weights[i] > 0)
            weight_sum += 1u << (weights[i] - 1);
    }
    /* target = 2^max_w; remaining for inferred symbol: */
    uint32_t target = 1u << max_w;
    if (weight_sum >= target) return -1;
    uint32_t remaining = target - weight_sum;
    /* remaining must be a power of two */
    if ((remaining & (remaining - 1)) != 0) return -1;

    /* The inferred symbol weight */
    uint8_t all_weights[HUF_MAX_SYMS + 1];
    std::memcpy(all_weights, weights, static_cast<size_t>(n_weights));
    /* inferred symbol has weight log2(remaining)+1 */
    int inferred_w = fse_highbit32(remaining) + 1;
    all_weights[n_weights] = static_cast<uint8_t>(inferred_w);
    int total_syms = n_weights + 1;

    /* max_bits from weights: max_w means max code_len = max_w+1? No:
       code_len = max_bits + 1 - weight  → for weight=max_w: code_len = max_bits+1-max_w.
       The minimum code_len is 1, achieved at weight = max_w: 1 = max_bits+1-max_w → max_bits = max_w.
    */
    int max_bits = max_w;
    if (max_bits > HUF_MAX_BITS) return -1;
    ht.max_bits = max_bits;

    /* Compute code lengths and count per length */
    int code_len[HUF_MAX_SYMS + 1];
    int count[HUF_MAX_BITS + 1] = {};
    for (int s = 0; s < total_syms; ++s) {
        if (all_weights[s] == 0) {
            code_len[s] = 0;
        } else {
            code_len[s] = max_bits + 1 - all_weights[s];
            ++count[code_len[s]];
        }
    }

    /* Assign canonical codes (length-sorted, symbol-sorted within length) */
    int next_code[HUF_MAX_BITS + 2] = {};
    {
        int c = 0;
        for (int len = 1; len <= max_bits; ++len) {
            next_code[len] = c;
            c = (c + count[len]) << 1;
        }
    }

    /* Build flat decode table of size 2^max_bits */
    const int table_size = 1 << max_bits;
    std::memset(ht.entries, 0, sizeof(HufDEntry) * static_cast<size_t>(table_size));

    for (int s = 0; s < total_syms; ++s) {
        int len = code_len[s];
        if (len == 0) continue;
        int code = next_code[len]++;
        /* Fill all entries that share this code prefix */
        int fill_count = table_size >> len;
        int start = code << (max_bits - len);
        HufDEntry e;
        e.symbol  = static_cast<uint8_t>(s);
        e.nb_bits = static_cast<uint8_t>(len);
        for (int k = 0; k < fill_count; ++k) {
            ht.entries[start + k] = e;
        }
    }

    return max_bits;
}

} // anonymous namespace

int huf_build_dtable(HufDTable& ht, const uint8_t* src, int src_len,
                     int* bytes_read_out) noexcept {
    if (!src || src_len <= 0 || !bytes_read_out) return -1;

    uint8_t header = src[0];

    if (header >= 128) {
        /* Direct encoding: n_syms = header & 0x7F, nibble pairs follow */
        int n_syms = header & 0x7F;
        int data_bytes = (n_syms + 1) / 2;
        if (1 + data_bytes > src_len) return -1;

        uint8_t weights[HUF_MAX_SYMS] = {};
        for (int i = 0; i < n_syms; ++i) {
            uint8_t byte = src[1 + i / 2];
            weights[i] = (i & 1) ? (byte & 0x0Fu) : (byte >> 4);
        }

        if (build_from_weights(ht, weights, n_syms) < 0) return -1;
        *bytes_read_out = 1 + data_bytes;
    } else {
        /* FSE-compressed weights */
        int compressed_size = header;
        if (compressed_size == 0 || 1 + compressed_size > src_len) return -1;

        /* Parse FSE ncount for weight table */
        int16_t norm[HUF_MAX_SYMS] = {};
        int     wlog = 0;
        int     nc_bytes = fse_read_ncount(src + 1, compressed_size,
                                           HUF_MAX_SYMS - 1, &wlog, norm);
        if (nc_bytes < 0) return -1;

        FseDTable wdt;
        if (fse_build_dtable(wdt, norm, HUF_MAX_SYMS - 1, wlog) < 0) return -1;

        /* Decode weights from FSE bitstream */
        const uint8_t* bs_start = src + 1 + nc_bytes;
        int            bs_len   = compressed_size - nc_bytes;
        if (bs_len <= 0) return -1;

        FseBitStream wbs;
        if (!wbs.init(bs_start, bs_len)) return -1;

        uint8_t weights[HUF_MAX_SYMS] = {};
        int n_syms = 0;
        uint32_t wstate = fse_init_state(wdt, wbs);
        /* Decode until bitstream exhausted */
        while (wbs.bits_avail > 0 || wbs.src_len > 0) {
            weights[n_syms++] = fse_decode_symbol(wdt, wstate);
            if (n_syms >= HUF_MAX_SYMS) break;
            wstate = fse_next_state(wdt, wstate, wbs);
        }
        /* Decode one last symbol from final state */
        if (n_syms < HUF_MAX_SYMS)
            weights[n_syms++] = fse_decode_symbol(wdt, wstate);

        if (build_from_weights(ht, weights, n_syms) < 0) return -1;
        *bytes_read_out = 1 + compressed_size;
    }

    return 0;
}

int huf_decode_1stream(const HufDTable& ht, const uint8_t* src, int src_len,
                       uint8_t* dst, int dst_len) noexcept {
    if (!src || src_len <= 0 || !dst || dst_len <= 0) return -1;
    if (ht.max_bits <= 0) return -1;

    HufBitStream bs;
    bs.init(src, src_len);

    for (int i = 0; i < dst_len; ++i) {
        if (!bs.has_bits(ht.max_bits)) {
            bs.refill();
            if (!bs.has_bits(1)) return -1;
        }
        int nb = ht.max_bits;
        if (bs.bits_avail < nb) nb = bs.bits_avail;
        uint32_t idx = bs.peek(nb);
        /* Pad to max_bits if fewer bits available */
        if (nb < ht.max_bits) idx <<= (ht.max_bits - nb);
        const HufDEntry& e = ht.entries[idx];
        if (e.nb_bits == 0) return -1; /* no symbol */
        dst[i] = e.symbol;
        bs.consume(e.nb_bits);
    }

    return 0;
}

int huf_decode_4stream(const HufDTable& ht, const uint8_t* src, int src_len,
                       uint8_t* dst, int dst_len) noexcept {
    if (!src || src_len < 6 || !dst || dst_len <= 0) return -1;
    if (ht.max_bits <= 0) return -1;

    /* 6-byte jump table: sizes of streams 1, 2, 3 (stream 4 is remainder) */
    int s1 = static_cast<int>(src[0]) | (static_cast<int>(src[1]) << 8);
    int s2 = static_cast<int>(src[2]) | (static_cast<int>(src[3]) << 8);
    int s3 = static_cast<int>(src[4]) | (static_cast<int>(src[5]) << 8);
    int s4 = src_len - 6 - s1 - s2 - s3;
    if (s1 <= 0 || s2 <= 0 || s3 <= 0 || s4 <= 0) return -1;
    if (6 + s1 + s2 + s3 + s4 != src_len) return -1;

    const uint8_t* p1 = src + 6;
    const uint8_t* p2 = p1 + s1;
    const uint8_t* p3 = p2 + s2;
    const uint8_t* p4 = p3 + s3;

    /* Output split: each sub-stream produces ceil(dst_len/4) */
    int out_size = (dst_len + 3) / 4;  /* per stream 1-3 */
    int out4 = dst_len - out_size * 3;
    if (out4 <= 0) {
        /* Adjust if dst_len % 4 != 0 */
        out4 = dst_len - out_size * 3;
        if (out4 <= 0) {
            out_size = dst_len / 4;
            out4 = dst_len - out_size * 3;
        }
    }

    if (huf_decode_1stream(ht, p1, s1, dst,                 out_size) < 0) return -1;
    if (huf_decode_1stream(ht, p2, s2, dst + out_size,     out_size) < 0) return -1;
    if (huf_decode_1stream(ht, p3, s3, dst + out_size * 2, out_size) < 0) return -1;
    if (huf_decode_1stream(ht, p4, s4, dst + out_size * 3, out4)     < 0) return -1;

    return 0;
}

} } /* namespace orot::zstd */
