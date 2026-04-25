#include "bwt.hpp"
#include <algorithm>
#include <vector>

namespace orot::bzip2 {

/* ── Cyclic suffix array via prefix doubling (Manber-Myers, O(n log² n)) ─────
   Works on the cyclic string: suffix i = block[i..n-1] ++ block[0..i-1]. */

static void build_suffix_array(const uint8_t* block, uint32_t n,
                                std::vector<uint32_t>& sa,
                                std::vector<uint32_t>& work)
{
    sa.resize(n);
    std::vector<int32_t> rank(n), tmp(n);

    /* Initial rank = byte value */
    for (uint32_t i = 0; i < n; ++i) { sa[i] = i; rank[i] = block[i]; }

    for (uint32_t gap = 1; gap < n; gap <<= 1) {
        /* Sort by (rank[i], rank[(i+gap)%n]) */
        auto cmp = [&](uint32_t a, uint32_t b) {
            if (rank[a] != rank[b]) return rank[a] < rank[b];
            int32_t ra = rank[(a + gap) % n];
            int32_t rb = rank[(b + gap) % n];
            return ra < rb;
        };
        std::sort(sa.begin(), sa.end(), cmp);

        /* Reassign ranks */
        tmp[sa[0]] = 0;
        for (uint32_t i = 1; i < n; ++i)
            tmp[sa[i]] = tmp[sa[i-1]] + (cmp(sa[i-1], sa[i]) ? 1 : 0);
        for (uint32_t i = 0; i < n; ++i) rank[i] = tmp[i];

        if (rank[sa[n-1]] == (int32_t)(n-1)) break; /* all unique, done */
    }
    (void)work;
}

uint32_t bwt_transform(const uint8_t* in, uint8_t* out, uint32_t len,
                       std::vector<uint32_t>& sa_buf)
{
    if (len == 0) return 0;
    if (len == 1) { out[0] = in[0]; return 0; }

    std::vector<uint32_t> work;
    build_suffix_array(in, len, sa_buf, work);

    uint32_t primary = 0;
    for (uint32_t i = 0; i < len; ++i) {
        uint32_t rot = sa_buf[i];
        out[i] = in[(rot + len - 1) % len];
        if (rot == 0) primary = i;
    }
    return primary;
}

void bwt_inverse(const uint8_t* in, uint8_t* out, uint32_t len,
                 uint32_t primary_index, std::vector<uint32_t>& tmp_buf)
{
    if (len == 0) return;
    if (len == 1) { out[0] = in[0]; return; }

    tmp_buf.resize(len);
    uint32_t* T = tmp_buf.data();

    /* Count sort to build T[] (next pointer array) */
    uint32_t cnt[256] = {};
    for (uint32_t i = 0; i < len; ++i) cnt[in[i]]++;

    uint32_t pos[256];
    uint32_t acc = 0;
    for (int c = 0; c < 256; ++c) { pos[c] = acc; acc += cnt[c]; }

    for (uint32_t i = 0; i < len; ++i)
        T[pos[in[i]]++] = i;

    /* Walk T[] from T[primary_index] to reconstruct */
    uint32_t idx = T[primary_index];
    for (uint32_t i = 0; i < len; ++i) {
        out[i] = in[idx];
        idx = T[idx];
    }
}

} // namespace orot::bzip2
