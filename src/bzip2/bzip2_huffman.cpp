#include "bzip2_huffman.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

namespace orot::bzip2 {

/* ── Huffman length assignment from frequency table ─────────────────────── */

void HuffEncTable::build_from_freqs(const uint32_t* freq, int n_syms, int max_len) {
    alpha_size = n_syms;

    struct Node {
        uint64_t weight;
        int left, right; /* -1 = leaf */
    };
    const int N = alpha_size;
    std::vector<Node> nodes(N * 2 + 4);
    for (int i = 0; i < N; ++i) {
        nodes[i].weight = freq[i] ? freq[i] : 1;
        nodes[i].left = nodes[i].right = -1;
    }

    std::vector<int> heap(N);
    for (int i = 0; i < N; ++i) heap[i] = i;
    int hsz = N, ncnt = N;

    auto cmp = [&](int a, int b){ return nodes[a].weight > nodes[b].weight; };
    std::make_heap(heap.begin(), heap.begin() + hsz, cmp);

    auto pop_min = [&](){
        std::pop_heap(heap.begin(), heap.begin() + hsz, cmp);
        return heap[--hsz];
    };
    auto push_node = [&](int idx){
        if (hsz >= (int)heap.size()) heap.push_back(0);
        heap[hsz++] = idx;
        std::push_heap(heap.begin(), heap.begin() + hsz, cmp);
    };

    while (hsz > 1) {
        int a = pop_min(), b = pop_min();
        nodes[ncnt] = { nodes[a].weight + nodes[b].weight, a, b };
        push_node(ncnt++);
    }

    /* Assign depths */
    memset(len, 0, sizeof(len));
    if (ncnt == 1) { len[0] = 1; build_codes(); return; }

    std::vector<std::pair<int,int>> stk;
    stk.push_back({ncnt - 1, 0});
    while (!stk.empty()) {
        auto [n, d] = stk.back(); stk.pop_back();
        if (nodes[n].left == -1) {
            len[n] = (uint8_t)std::min(d, max_len);
        } else {
            stk.push_back({nodes[n].left,  d + 1});
            stk.push_back({nodes[n].right, d + 1});
        }
    }
    build_codes();
}

void HuffEncTable::build_codes() {
    int max_l = 0;
    for (int i = 0; i < alpha_size; ++i)
        if (len[i] > max_l) max_l = len[i];

    int bl_count[BZ_MAX_CODE_LEN + 1] = {};
    for (int i = 0; i < alpha_size; ++i)
        if (len[i]) bl_count[len[i]]++;

    /* bzip2 canonical: MSB-first */
    uint32_t c = 0;
    uint32_t next_code[BZ_MAX_CODE_LEN + 1] = {};
    for (int bits = 1; bits <= max_l; ++bits) {
        c = (c + bl_count[bits - 1]) << 1;
        next_code[bits] = c;
    }
    for (int i = 0; i < alpha_size; ++i) {
        if (len[i]) code[i] = next_code[len[i]]++;
        else        code[i] = 0;
    }
}

/* ── Multi-table build (k-means style) ──────────────────────────────────── */

void build_huffman_tables(
    const uint16_t* syms, uint32_t n_syms,
    int alpha_size,
    int n_tables,
    HuffEncTable tables[BZ_MAX_TABLES],
    uint8_t* selectors, uint32_t* n_selectors_out)
{
    uint32_t n_groups = (n_syms + BZ_G_SIZE - 1) / BZ_G_SIZE;
    *n_selectors_out  = n_groups;

    for (uint32_t g = 0; g < n_groups; ++g)
        selectors[g] = (uint8_t)(g % n_tables);

    (void)alpha_size;

    for (int iter = 0; iter < 4; ++iter) {
        uint32_t freq[BZ_MAX_TABLES][BZ_MAX_ALPHA_SIZE] = {};
        for (uint32_t g = 0; g < n_groups; ++g) {
            uint32_t start = g * BZ_G_SIZE;
            uint32_t end   = std::min(start + (uint32_t)BZ_G_SIZE, n_syms);
            int t = selectors[g];
            for (uint32_t i = start; i < end; ++i) freq[t][syms[i]]++;
        }
        for (int t = 0; t < n_tables; ++t)
            tables[t].build_from_freqs(freq[t], alpha_size);

        /* Re-assign each group */
        for (uint32_t g = 0; g < n_groups; ++g) {
            uint32_t start = g * BZ_G_SIZE;
            uint32_t end   = std::min(start + (uint32_t)BZ_G_SIZE, n_syms);
            uint32_t best = ~0u; uint8_t best_t = 0;
            for (int t = 0; t < n_tables; ++t) {
                uint32_t cost = 0;
                for (uint32_t i = start; i < end; ++i)
                    cost += tables[t].len[syms[i]];
                if (cost < best) { best = cost; best_t = (uint8_t)t; }
            }
            selectors[g] = best_t;
        }
    }
}

/* ── Decode table build ──────────────────────────────────────────────────── */

void HuffDecTable::build_from_lengths(const uint8_t* lengths, int size) {
    alpha_size = size;
    memcpy(len, lengths, size);
    min_len = BZ_MAX_CODE_LEN; max_len = 0;
    for (int i = 0; i < size; ++i) {
        if (lengths[i] && lengths[i] < min_len) min_len = lengths[i];
        if (lengths[i] > max_len) max_len = lengths[i];
    }
    if (max_len == 0) { min_len = 0; return; }

    /* Count symbols per length */
    int cnt[BZ_MAX_CODE_LEN + 1] = {};
    for (int i = 0; i < size; ++i)
        if (lengths[i]) cnt[lengths[i]]++;

    /* base[l] = first canonical code for length l */
    uint32_t c = 0;
    int bl_prev = 0;
    for (int l = 1; l <= max_len; ++l) {
        c = (c + bl_prev) << 1;
        base[l]   = c;
        limit[l]  = cnt[l] ? (c + cnt[l] - 1) : (uint32_t)-1;
        offset[l] = 0;
        bl_prev   = cnt[l];
    }

    /* Build perm[]: symbols sorted by (length, natural order) */
    int pidx = 0;
    for (int l = 1; l <= max_len; ++l) {
        offset[l] = pidx;
        for (int s = 0; s < size; ++s)
            if (lengths[s] == l) perm[pidx++] = s;
    }

    /* Build fast lookup table for codes with length <= HUFF_FAST_BITS */
    std::memset(fast_table, 0xFF, sizeof(fast_table));

    int fast_max = std::min(max_len, HUFF_FAST_BITS);
    for (int l = 1; l <= fast_max; ++l) {
        if (limit[l] == (uint32_t)-1) continue;
        int num = (int)(limit[l] - base[l] + 1);
        int fill = 1 << (HUFF_FAST_BITS - l);
        for (int j = 0; j < num; ++j) {
            int sym = perm[offset[l] + j];
            uint32_t idx = (base[l] + (uint32_t)j) << (HUFF_FAST_BITS - l);
            FastEntry e = { (int16_t)sym, (uint8_t)l };
            for (int k = 0; k < fill; ++k)
                fast_table[idx + k] = e;
        }
    }
}

int HuffDecTable::decode_sym(uint64_t& buf, int& buf_bits,
                              const uint8_t* src, size_t src_size, size_t& src_pos) const
{
    /* Fast path: peek HUFF_FAST_BITS bits and do flat lookup */
    if (buf_bits >= HUFF_FAST_BITS) {
        uint32_t peek = (uint32_t)((buf >> (buf_bits - HUFF_FAST_BITS)) & (HUFF_FAST_SIZE - 1));
        const FastEntry& e = fast_table[peek];
        if (e.sym >= 0) {
            buf_bits -= e.len;
            return e.sym;
        }
        /* Code is longer than HUFF_FAST_BITS. Consume the 10 bits already peeked
         * and continue from l = HUFF_FAST_BITS+1, avoiding re-scanning bits 1..10. */
        buf_bits -= HUFF_FAST_BITS;
        uint32_t v = peek;
        for (int l = HUFF_FAST_BITS + 1; l <= max_len; ++l) {
            if (buf_bits == 0) {
                if (src_pos >= src_size) return -1;
                buf = (buf << 8) | src[src_pos++];
                buf_bits = 8;
            }
            --buf_bits;
            v = (v << 1) | (uint32_t)((buf >> buf_bits) & 1);
            if (limit[l] == (uint32_t)-1) continue;
            if (v <= limit[l])
                return perm[offset[l] + (int)(v - base[l])];
        }
        return -1;
    }

    /* Slow path: buf too low for fast lookup — read bit by bit from the start */
    uint32_t v = 0;
    for (int l = 1; l <= max_len; ++l) {
        if (buf_bits == 0) {
            if (src_pos >= src_size) return -1;
            buf = (buf << 8) | src[src_pos++];
            buf_bits = 8;
        }
        --buf_bits;
        v = (v << 1) | (uint32_t)((buf >> buf_bits) & 1);
        if (limit[l] == (uint32_t)-1) continue;
        if (v <= limit[l])
            return perm[offset[l] + (int)(v - base[l])];
    }
    return -1;
}

} // namespace orot::bzip2
