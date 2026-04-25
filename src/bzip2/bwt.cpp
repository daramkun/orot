#include "bwt.hpp"
#include <vector>
#include <cstring>
#include <algorithm>

namespace orot::bzip2 {

/* ── SA-IS: O(n) suffix array construction (Nong-Zhang-Chan 2009) ───────────
   Computes the suffix array of s[0..n-1] where s[n-1] = 0 is a unique minimum
   sentinel smaller than all other values (1..sigma-1). */

static void sais_get_bkt(const int32_t* s, int32_t n, int32_t sigma,
                          std::vector<int32_t>& bkt, bool tail)
{
    bkt.assign(sigma, 0);
    for (int32_t i = 0; i < n; ++i) bkt[s[i]]++;
    if (tail) {
        int32_t sum = 0;
        for (int32_t c = 0; c < sigma; ++c) { sum += bkt[c]; bkt[c] = sum - 1; }
    } else {
        int32_t sum = 0;
        for (int32_t c = 0; c < sigma; ++c) { int32_t t = bkt[c]; bkt[c] = sum; sum += t; }
    }
}

static void sais_induce(const int32_t* s, int32_t* sa, int32_t n, int32_t sigma,
                         const std::vector<int8_t>& tp)
{
    std::vector<int32_t> bkt;
    /* Induce L-type from left */
    sais_get_bkt(s, n, sigma, bkt, false);
    for (int32_t i = 0; i < n; ++i) {
        if (sa[i] <= 0) continue;
        int32_t j = sa[i] - 1;
        if (tp[j] == 0) sa[bkt[s[j]]++] = j; /* L-type */
    }
    /* Induce S-type from right */
    sais_get_bkt(s, n, sigma, bkt, true);
    for (int32_t i = n - 1; i >= 0; --i) {
        if (sa[i] <= 0) continue;
        int32_t j = sa[i] - 1;
        if (tp[j] == 1) sa[bkt[s[j]]--] = j; /* S-type */
    }
}

/* Compare two LMS substrings starting at pos a and b in s (with type array tp).
   Returns true if they are identical LMS substrings. */
static bool sais_lms_equal(const int32_t* s, int32_t n,
                             const std::vector<int8_t>& tp,
                             int32_t a, int32_t b)
{
    for (int32_t k = 0; ; ++k) {
        if (a + k >= n || b + k >= n) return (a + k >= n) == (b + k >= n);
        bool a_lms = (k > 0 && tp[a+k] == 1 && tp[a+k-1] == 0);
        bool b_lms = (k > 0 && tp[b+k] == 1 && tp[b+k-1] == 0);
        if (s[a+k] != s[b+k] || tp[a+k] != tp[b+k]) return false;
        if (a_lms && b_lms) return true;
        if (a_lms != b_lms) return false;
    }
}

static void sais_impl(const int32_t* s, int32_t* sa, int32_t n, int32_t sigma)
{
    /* ── Type classification ── */
    std::vector<int8_t> tp(n);
    tp[n-1] = 1; /* S-type sentinel */
    for (int32_t i = n-2; i >= 0; --i)
        tp[i] = (s[i] < s[i+1] || (s[i] == s[i+1] && tp[i+1])) ? 1 : 0;

    /* ── Place LMS at bucket tails ── */
    std::vector<int32_t> bkt;
    sais_get_bkt(s, n, sigma, bkt, true);
    std::fill(sa, sa + n, -1);
    for (int32_t i = n-2; i >= 1; --i)
        if (tp[i] == 1 && tp[i-1] == 0) /* LMS */
            sa[bkt[s[i]]--] = i;
    sa[0] = n-1; /* sentinel always goes first */

    /* ── Induced sort to order LMS substrings ── */
    sais_induce(s, sa, n, sigma, tp);

    /* ── Compact sorted LMS positions into sa[0..m-1] ── */
    int32_t m = 0;
    for (int32_t i = 0; i < n; ++i) {
        int32_t p = sa[i];
        if (p > 0 && tp[p] == 1 && tp[p-1] == 0)
            sa[m++] = p;
    }
    /* sa[0..m-1] = sorted LMS positions (m includes the sentinel at n-1) */

    /* ── Assign names to LMS substrings ── */
    std::vector<int32_t> tmp(n, -1);
    int32_t name = 0, prev = -1;
    for (int32_t i = 0; i < m; ++i) {
        int32_t pos = sa[i];
        bool diff = (prev < 0) || !sais_lms_equal(s, n, tp, prev, pos);
        if (diff) { ++name; prev = pos; }
        tmp[pos] = name - 1;
    }

    /* ── Build reduced string s1 and LMS position map ── */
    /* Collect names in left-to-right position order.
       The original sentinel (position n-1) always gets name 0 and ends up at
       s1[m-1] — no extra sentinel byte needed. */
    std::vector<int32_t> s1, lms_pos;
    s1.reserve((size_t)m);
    lms_pos.reserve((size_t)m);
    for (int32_t i = 0; i < n; ++i)
        if (tmp[i] >= 0) { s1.push_back(tmp[i]); lms_pos.push_back(i); }
    /* s1 has length m; s1[m-1] = 0 (sentinel, from original sentinel at n-1) */

    /* ── Solve reduced problem ── */
    /* name == m means all m LMS substrings are unique: direct SA1 construction.
       name < m means at least one pair is equal: must recurse. */
    std::vector<int32_t> sa1((size_t)m, -1);
    if (name < m) {
        sais_impl(s1.data(), sa1.data(), m, name + 1);
    } else {
        /* All names unique: SA1 is trivially derived from names (=ranks) */
        sa1[0] = m - 1; /* sentinel at s1[m-1]=0 */
        for (int32_t i = 0; i < m - 1; ++i)
            sa1[s1[i]] = i; /* s1[i] ∈ 1..m-1 for non-sentinel positions */
    }

    /* ── Final induced sort using sorted LMS order ── */
    sais_get_bkt(s, n, sigma, bkt, true);
    std::fill(sa, sa + n, -1);
    /* Place LMS in reverse order of SA1[1..m-1] (SA1[0]=sentinel, handled below) */
    for (int32_t i = m - 1; i >= 1; --i)
        sa[bkt[s[lms_pos[sa1[i]]]]--] = lms_pos[sa1[i]];
    sa[0] = n - 1; /* sentinel always first */
    sais_induce(s, sa, n, sigma, tp);
}

/* ── Build cyclic suffix array via SA-IS on doubled string ──────────────────
   Creates t = s[0]+1, ..., s[n-1]+1, s[0]+1, ..., s[n-1]+1, 0  (length 2n+1)
   Computes SA of t, filters positions < n to get cyclic SA of s. */

static void build_suffix_array_sais(const uint8_t* block, uint32_t n,
                                     std::vector<uint32_t>& sa,
                                     std::vector<int32_t>& t_buf,
                                     std::vector<int32_t>& sa_buf_i32)
{
    /* Build doubled string with shifted alphabet (1-256) + sentinel (0) */
    uint32_t tn = 2 * n + 1;
    t_buf.resize(tn);
    for (uint32_t i = 0; i < n; ++i)
        t_buf[i] = t_buf[i + n] = (int32_t)block[i] + 1;
    t_buf[2 * n] = 0; /* sentinel */

    sa_buf_i32.resize(tn);
    sais_impl(t_buf.data(), sa_buf_i32.data(), (int32_t)tn, 257);

    /* Filter: keep only positions < n (cyclic rotations of original string) */
    sa.resize(n);
    uint32_t k = 0;
    for (uint32_t i = 0; i < tn && k < n; ++i)
        if (sa_buf_i32[i] >= 0 && (uint32_t)sa_buf_i32[i] < n)
            sa[k++] = (uint32_t)sa_buf_i32[i];
}

/* ── Cyclic suffix array via prefix doubling with counting sort ─────────────
   Fallback O(n log n) algorithm used for small blocks where SA-IS overhead
   exceeds the gain. */

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

        /* Pass 1: stable sort by second key rank[(sa[i]+gap) mod n]
           sa[i] < n, gap < n => sum < 2n => conditional subtract avoids division */
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t idx = sa[i] + gap; if (idx >= n) idx -= n;
            cnt[(uint32_t)rank[idx] + 1]++;
        }
        for (uint32_t i = 1; i < cnt_size; ++i) cnt[i] += cnt[i-1];
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t idx = sa[i] + gap; if (idx >= n) idx -= n;
            work[cnt[(uint32_t)rank[idx]]++] = sa[i];
        }

        /* Pass 2: stable sort by first key rank[i] */
        cnt.assign(cnt_size, 0);
        for (uint32_t i = 0; i < n; ++i)
            cnt[(uint32_t)rank[work[i]] + 1]++;
        for (uint32_t i = 1; i < cnt_size; ++i) cnt[i] += cnt[i-1];
        for (uint32_t i = 0; i < n; ++i)
            sa[cnt[(uint32_t)rank[work[i]]]++] = work[i];

        /* Reassign ranks based on (rank[sa[i]], rank[(sa[i]+gap) mod n]) pairs */
        rank_tmp[sa[0]] = 0;
        for (uint32_t i = 1; i < n; ++i) {
            uint32_t pi = sa[i]   + gap; if (pi >= n) pi -= n;
            uint32_t qi = sa[i-1] + gap; if (qi >= n) qi -= n;
            bool same = (rank[sa[i]] == rank[sa[i-1]]) && (rank[pi] == rank[qi]);
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

    /* Algorithm selection:
       SA-IS (O(n)) wins for compressible data with repeated patterns where
       counting-sort prefix-doubling needs many iterations.
       For high-entropy (random-like) data, counting sort terminates in 1-2
       iterations via early exit, making SA-IS overhead unwarranted.
       Heuristic: count distinct byte values in the block. ≥ 200 distinct
       values ≈ random/incompressible → use counting sort. */
    static constexpr uint32_t SAIS_MIN_LEN       = 4096;
    static constexpr int      SAIS_MAX_DISTINCT   = 200;

    bool use_sais = (len >= SAIS_MIN_LEN);
    if (use_sais) {
        uint32_t freq[256] = {};
        for (uint32_t i = 0; i < len; ++i) freq[in[i]]++;
        int distinct = 0;
        for (int c = 0; c < 256; ++c) if (freq[c]) ++distinct;
        if (distinct >= SAIS_MAX_DISTINCT) use_sais = false;
    }

    if (use_sais) {
        static thread_local std::vector<int32_t> t_buf;
        static thread_local std::vector<int32_t> sa_i32;

        build_suffix_array_sais(in, len, sa_buf, t_buf, sa_i32);
    } else {
        static thread_local std::vector<int32_t>  rank_buf;
        static thread_local std::vector<int32_t>  rank_tmp_buf;
        static thread_local std::vector<uint32_t> cnt_buf;

        build_suffix_array(in, len, sa_buf, work_buf,
                           rank_buf, rank_tmp_buf, cnt_buf);
    }

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
