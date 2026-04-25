#include "bwt.hpp"
#include <vector>
#include <cstring>

namespace orot::bzip2 {

/* ── Cyclic suffix array via prefix doubling with counting sort ─────────────
   Replaces std::sort (O(n log² n)) with counting sort per iteration (O(n log n)).
   Works on the cyclic string: suffix i = block[i..n-1] ++ block[0..i-1]. */

static void build_suffix_array(const uint8_t* block, uint32_t n,
                                std::vector<uint32_t>& sa,
                                std::vector<uint32_t>& work,
                                std::vector<int32_t>&  rank,
                                std::vector<int32_t>&  rank_tmp,
                                std::vector<uint32_t>& cnt)
{
    sa.resize(n);
    work.resize(n);
    rank.resize(n);
    rank_tmp.resize(n);

    /* ── Initial sort by byte value ── */
    {
        uint32_t freq[256] = {};
        for (uint32_t i = 0; i < n; ++i) freq[block[i]]++;

        uint32_t pos[256];
        uint32_t acc = 0;
        for (int c = 0; c < 256; ++c) { pos[c] = acc; acc += freq[c]; }
        for (uint32_t i = 0; i < n; ++i) sa[pos[block[i]]++] = i;

        /* Assign initial ranks (equal bytes get equal rank) */
        rank_tmp[sa[0]] = 0;
        for (uint32_t i = 1; i < n; ++i)
            rank_tmp[sa[i]] = rank_tmp[sa[i-1]] + (block[sa[i]] != block[sa[i-1]] ? 1 : 0);
        for (uint32_t i = 0; i < n; ++i) rank[i] = rank_tmp[i];
    }

    /* ── Prefix doubling with two-pass counting sort per iteration ── */
    for (uint32_t gap = 1; gap < n; gap <<= 1) {
        /* Check for early termination: all ranks unique */
        if (rank[sa[n-1]] == (int32_t)(n-1)) break;

        /* Maximum rank value (used to size counting array) */
        int32_t max_rank = rank[sa[n-1]];
        uint32_t cnt_size = (uint32_t)(max_rank + 2);
        cnt.assign(cnt_size, 0);

        /* Pass 1: stable sort by second key rank[(i+gap)%n] */
        for (uint32_t i = 0; i < n; ++i)
            cnt[(uint32_t)rank[(sa[i] + gap) % n] + 1]++;
        for (uint32_t i = 1; i < cnt_size; ++i) cnt[i] += cnt[i-1];
        for (uint32_t i = 0; i < n; ++i)
            work[cnt[(uint32_t)rank[(sa[i] + gap) % n]]++] = sa[i];

        /* Pass 2: stable sort by first key rank[i] */
        cnt.assign(cnt_size, 0);
        for (uint32_t i = 0; i < n; ++i)
            cnt[(uint32_t)rank[work[i]] + 1]++;
        for (uint32_t i = 1; i < cnt_size; ++i) cnt[i] += cnt[i-1];
        for (uint32_t i = 0; i < n; ++i)
            sa[cnt[(uint32_t)rank[work[i]]]++] = work[i];

        /* Reassign ranks based on (rank[sa[i]], rank[(sa[i]+gap)%n]) pairs */
        rank_tmp[sa[0]] = 0;
        for (uint32_t i = 1; i < n; ++i) {
            bool same = (rank[sa[i]]            == rank[sa[i-1]]) &&
                        (rank[(sa[i]+gap)%n]    == rank[(sa[i-1]+gap)%n]);
            rank_tmp[sa[i]] = rank_tmp[sa[i-1]] + (same ? 0 : 1);
        }
        for (uint32_t i = 0; i < n; ++i) rank[i] = rank_tmp[i];
    }
}

uint32_t bwt_transform(const uint8_t* in, uint8_t* out, uint32_t len,
                       std::vector<uint32_t>& sa_buf,
                       std::vector<uint32_t>& work_buf)
{
    if (len == 0) return 0;
    if (len == 1) { out[0] = in[0]; return 0; }

    /* Reused rank buffers (allocated once per stream, grown as needed) */
    static thread_local std::vector<int32_t>  rank_buf;
    static thread_local std::vector<int32_t>  rank_tmp_buf;
    static thread_local std::vector<uint32_t> cnt_buf;

    build_suffix_array(in, len, sa_buf, work_buf,
                       rank_buf, rank_tmp_buf, cnt_buf);

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
