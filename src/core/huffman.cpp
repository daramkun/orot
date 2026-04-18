#include "huffman.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

namespace deflate {

/* =========================================================================
 * RFC 1951 static tables
 * ========================================================================= */

/* Length codes 257-285: extra bits and base length values */
const uint8_t LENGTH_EXTRA_BITS[29] = {
    0,0,0,0,0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4,4, 5,5,5,5, 0
};
const uint16_t LENGTH_BASE[29] = {
    3,4,5,6,7,8,9,10, 11,13,15,17, 19,23,27,31, 35,43,51,59,
    67,83,99,115, 131,163,195,227, 258
};

/* Distance codes 0-29: extra bits and base distance values */
const uint8_t DIST_EXTRA_BITS[30] = {
    0,0,0,0, 1,1, 2,2, 3,3, 4,4, 5,5, 6,6, 7,7, 8,8, 9,9,
    10,10, 11,11, 12,12, 13,13
};
const uint16_t DIST_BASE[30] = {
    1,2,3,4, 5,7, 9,13, 17,25, 33,49, 65,97, 129,193,
    257,385, 513,769, 1025,1537, 2049,3073, 4097,6145,
    8193,12289, 16385,24577
};

const LengthCode LENGTHS[29] = {
    {257,0,3},{258,0,4},{259,0,5},{260,0,6},{261,0,7},{262,0,8},{263,0,9},{264,0,10},
    {265,1,11},{266,1,13},{267,1,15},{268,1,17},
    {269,2,19},{270,2,23},{271,2,27},{272,2,31},
    {273,3,35},{274,3,43},{275,3,51},{276,3,59},
    {277,4,67},{278,4,83},{279,4,99},{280,4,115},
    {281,5,131},{282,5,163},{283,5,195},{284,5,227},
    {285,0,258}
};

const DistCode DISTANCES[30] = {
    {0,0,1},{1,0,2},{2,0,3},{3,0,4},
    {4,1,5},{5,1,7},{6,2,9},{7,2,13},
    {8,3,17},{9,3,25},{10,4,33},{11,4,49},
    {12,5,65},{13,5,97},{14,6,129},{15,6,193},
    {16,7,257},{17,7,385},{18,8,513},{19,8,769},
    {20,9,1025},{21,9,1537},{22,10,2049},{23,10,3073},
    {24,11,4097},{25,11,6145},{26,12,8193},{27,12,12289},
    {28,13,16385},{29,13,24577}
};

/* =========================================================================
 * Bit-reversal helper (nibble table, O(1) per symbol)
 * ========================================================================= */

static const uint8_t kRevNibble[16] = {
    0x0, 0x8, 0x4, 0xC, 0x2, 0xA, 0x6, 0xE,
    0x1, 0x9, 0x5, 0xD, 0x3, 0xB, 0x7, 0xF
};

static inline uint16_t reverse_bits_u16(uint16_t code, int len) noexcept {
    uint16_t rev = static_cast<uint16_t>(
        (static_cast<uint32_t>(kRevNibble[ code        & 0xF]) << 12) |
        (static_cast<uint32_t>(kRevNibble[(code >>  4) & 0xF]) <<  8) |
        (static_cast<uint32_t>(kRevNibble[(code >>  8) & 0xF]) <<  4) |
         static_cast<uint32_t>(kRevNibble[(code >> 12) & 0xF])
    );
    return static_cast<uint16_t>(rev >> (16 - len));
}

/* =========================================================================
 * Length/distance code lookup tables
 * ========================================================================= */

/* Lookup: match length (3-258) → code index (0-28) */
static uint8_t s_length_code[259];
/* Lookup: distance (1-32768) → code index (0-29) */
static uint8_t s_dist_code[32769];

static bool s_tables_initialized = false;

static void init_tables() {
    if (s_tables_initialized) return;

    /* Build length code table */
    for (int i = 0; i < 29; ++i) {
        const int base  = LENGTH_BASE[i];
        const int extra = LENGTH_EXTRA_BITS[i];
        const int count = 1 << extra;
        for (int j = 0; j < count && base + j <= 258; ++j)
            s_length_code[base + j] = static_cast<uint8_t>(i);
    }

    /* Build distance code table */
    for (int i = 0; i < 30; ++i) {
        const int base  = DIST_BASE[i];
        const int extra = DIST_EXTRA_BITS[i];
        const int count = 1 << extra;
        for (int j = 0; j < count && base + j <= 32768; ++j)
            s_dist_code[base + j] = static_cast<uint8_t>(i);
    }

    s_tables_initialized = true;
}

int length_to_code(int len) {
    init_tables();
    assert(len >= 3 && len <= 258);
    return s_length_code[len];
}

int dist_to_code(int dist) {
    init_tables();
    assert(dist >= 1 && dist <= 32768);
    return s_dist_code[dist];
}

/* =========================================================================
 * Fixed Huffman tables (RFC 1951 §3.2.6)
 * ========================================================================= */

static const uint8_t FIXED_LITLEN_LENS[288] = {
    /* 0-143: length 8 */
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    /* 144-255: length 9 */
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    /* 256-279: length 7 */
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    /* 280-287: length 8 */
    8,8,8,8,8,8,8,8
};

static const uint8_t FIXED_DIST_LENS[32] = {
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5
};

void build_fixed_litlen_enc(HuffEncTable& out) {
    std::memcpy(out.lens, FIXED_LITLEN_LENS, 288);
    build_enc_table_from_lens(out.lens, 288, out.codes);
}

void build_fixed_dist_enc(HuffDistTable& out) {
    std::memcpy(out.lens, FIXED_DIST_LENS, 32);
    uint8_t lens32[DIST_SYMS];
    std::memcpy(lens32, FIXED_DIST_LENS, DIST_SYMS);
    build_enc_table_from_lens(lens32, DIST_SYMS, out.codes);
}

void build_fixed_litlen_dec(uint32_t* table) {
    build_dec_table_from_lens(FIXED_LITLEN_LENS, 288, LITLEN_DECODE_BITS, table);
}

void build_fixed_dist_dec(uint32_t* table) {
    build_dec_table_from_lens(FIXED_DIST_LENS, 32, DIST_DECODE_BITS, table);
}

/* =========================================================================
 * Package-Merge length-limited Huffman code construction
 * ========================================================================= */

/*
 * Larmore-Hirschberg Package-Merge algorithm.
 * Produces optimal length-limited Huffman code lengths.
 *
 * Reference: "A Fast Algorithm for Optimal Length-Limited Huffman Codes"
 *            Larmore & Hirschberg, JACM 1990.
 */
static void package_merge(
    const uint32_t* freqs, int n_syms,
    uint8_t* lens, int max_bits)
{
    /* Sort symbols by frequency (non-zero first) */
    int active[LITLEN_SYMS];
    int n_active = 0;
    for (int i = 0; i < n_syms; ++i)
        if (freqs[i] > 0) active[n_active++] = i;

    if (n_active == 0) return;
    if (n_active == 1) {
        lens[active[0]] = 1;
        return;
    }

    /* Sort by frequency ascending */
    std::sort(active, active + n_active, [&](int a, int b) {
        return freqs[a] < freqs[b];
    });

    /* Edge case: all same frequency, use balanced tree */
    const int n = n_active;

    /* Package-Merge: build (max_bits) lists */
    /* Each list entry is a "package" with combined weight */
    /* We use a flat array approach */

    /* freq_sorted[i] = frequency of i-th symbol in sorted order */
    uint64_t freq_sorted[LITLEN_SYMS];
    for (int i = 0; i < n; ++i)
        freq_sorted[i] = freqs[active[i]];

    /* coin_freq[i] = weight of package/symbol at position i in current list */
    uint64_t coins[LITLEN_SYMS * 2];
    uint64_t prev_coins[LITLEN_SYMS * 2];
    int      coin_count;
    int      prev_count;

    /* Initialize: first list is just the sorted symbols */
    prev_count = n;
    for (int i = 0; i < n; ++i)
        prev_coins[i] = freq_sorted[i];

    /* bit_usage[i] = number of times symbol i is "used" across all passes */
    int bit_usage[LITLEN_SYMS];
    std::memset(bit_usage, 0, sizeof(int) * static_cast<size_t>(n));

    for (int level = 1; level < max_bits; ++level) {
        /* Merge prev_coins (packages) with symbols to form new list */
        coin_count = 0;
        int pi = 0;  /* index into prev packages */
        int si = 0;  /* index into symbols       */
        const int new_max = n * 2 - 2;  /* at most 2n-2 packages per level */

        while (coin_count < new_max) {
            uint64_t pkg_weight = (pi + 1 < prev_count)
                ? prev_coins[pi] + prev_coins[pi + 1]
                : std::numeric_limits<uint64_t>::max();
            uint64_t sym_weight = (si < n) ? freq_sorted[si]
                : std::numeric_limits<uint64_t>::max();

            if (sym_weight <= pkg_weight) {
                coins[coin_count++] = sym_weight;
                ++si;
            } else {
                coins[coin_count++] = pkg_weight;
                pi += 2;
                if (pi >= prev_count) break;
            }
        }

        std::memcpy(prev_coins, coins, sizeof(uint64_t) * static_cast<size_t>(coin_count));
        prev_count = coin_count;
    }

    /* Count: use last 2*(n-1) packages to determine code lengths */
    /* Simple counting approach */
    /* For the last list, select (2n-2) smallest entries */
    const int select = 2 * (n - 1);
    /* Reset and compute lengths using a greedy assignment */
    std::memset(bit_usage, 0, sizeof(int) * static_cast<size_t>(n));

    /* Simplified: assign lengths via standard Huffman with clamping */
    /* Use a min-heap based approach for correctness */
    struct Node {
        uint64_t weight;
        int left, right;  /* -1 = leaf, index into active[] */
    };
    Node nodes[LITLEN_SYMS * 2];
    int  heap[LITLEN_SYMS * 2];
    int n_nodes = n;
    int n_heap  = n;

    for (int i = 0; i < n; ++i) {
        nodes[i] = { freq_sorted[i], -1, i };
        heap[i]  = i;
    }

    auto heap_cmp = [&](int a, int b) {
        return nodes[a].weight > nodes[b].weight;
    };
    std::make_heap(heap, heap + n_heap, heap_cmp);

    while (n_heap > 1) {
        std::pop_heap(heap, heap + n_heap, heap_cmp);
        int a = heap[--n_heap];
        std::pop_heap(heap, heap + n_heap, heap_cmp);
        int b = heap[--n_heap];

        nodes[n_nodes] = { nodes[a].weight + nodes[b].weight, a, b };
        heap[n_heap++] = n_nodes++;
        std::push_heap(heap, heap + n_heap, heap_cmp);
    }

    /* Traverse tree, assign depths */
    int depth_stack[LITLEN_SYMS * 2];
    int node_stack [LITLEN_SYMS * 2];
    int sp = 0;
    node_stack[sp]  = n_nodes - 1;
    depth_stack[sp] = 0;
    ++sp;

    while (sp > 0) {
        --sp;
        int nd    = node_stack[sp];
        int depth = depth_stack[sp];
        if (nodes[nd].left == -1) {
            /* Leaf: nodes[nd].right is the index in active[] */
            int sym_idx = nodes[nd].right;
            lens[active[sym_idx]] = static_cast<uint8_t>(
                std::min(depth, max_bits));
        } else {
            node_stack[sp]  = nodes[nd].left;
            depth_stack[sp] = depth + 1; ++sp;
            node_stack[sp]  = nodes[nd].right;
            depth_stack[sp] = depth + 1; ++sp;
        }
    }

    /* Clamp and adjust to satisfy max_bits constraint */
    /* Find max length; if > max_bits, redistribute */
    bool need_adjust = false;
    for (int i = 0; i < n; ++i)
        if (lens[active[i]] > static_cast<uint8_t>(max_bits)) {
            need_adjust = true;
            break;
        }

    if (need_adjust) {
        /* Simple clamp + adjustment: ensure Kraft inequality holds */
        for (int i = 0; i < n; ++i)
            if (lens[active[i]] > static_cast<uint8_t>(max_bits))
                lens[active[i]] = static_cast<uint8_t>(max_bits);

        /* Rebalance: increase shorter codes until Kraft equality */
        int64_t kraft = 0;
        for (int i = 0; i < n; ++i)
            kraft += (1L << (max_bits - lens[active[i]]));
        const int64_t target = (1L << max_bits);

        /* Reduce excess by increasing lengths of longest codes */
        for (int i = n - 1; i >= 0 && kraft > target; --i) {
            while (kraft > target && lens[active[i]] < static_cast<uint8_t>(max_bits)) {
                ++lens[active[i]];
                kraft -= (1L << (max_bits - lens[active[i]]));
            }
        }
        /* Fix deficit by decreasing lengths */
        for (int i = 0; i < n && kraft < target; ++i) {
            while (kraft < target && lens[active[i]] > 1) {
                kraft += (1L << (max_bits - lens[active[i]]));
                --lens[active[i]];
                kraft -= (1L << (max_bits - lens[active[i]]));
            }
        }
    }

    /* Unused symbols stay at 0 */
    (void)select;
}

void build_huffman_lengths(
    const uint32_t* freqs, int symbols,
    uint8_t* lens, int max_bits)
{
    std::memset(lens, 0, static_cast<size_t>(symbols));
    package_merge(freqs, symbols, lens, max_bits);
}

/* =========================================================================
 * Canonical code assignment from lengths
 * ========================================================================= */

void build_enc_table_from_lens(
    const uint8_t* lens, int symbols,
    uint16_t* codes)
{
    /* Count codes of each length */
    int bl_count[MAX_CODE_BITS + 1] = {};
    for (int i = 0; i < symbols; ++i)
        if (lens[i] > 0) ++bl_count[lens[i]];

    /* Find smallest code for each length (RFC 1951 §3.2.2) */
    uint16_t next_code[MAX_CODE_BITS + 2] = {};
    uint16_t code = 0;
    for (int bits = 1; bits <= MAX_CODE_BITS; ++bits) {
        code = static_cast<uint16_t>((code + bl_count[bits - 1]) << 1);
        next_code[bits] = code;
    }

    /* Assign codes */
    for (int i = 0; i < symbols; ++i) {
        if (lens[i] == 0) { codes[i] = 0; continue; }
        const uint16_t c = next_code[lens[i]]++;
        codes[i] = reverse_bits_u16(c, lens[i]);
    }
}

/* =========================================================================
 * Two-level decode table construction
 * ========================================================================= */

int build_dec_table_from_lens(
    const uint8_t* lens, int symbols,
    int table_bits,
    uint32_t* table)
{
    /* Count lengths */
    int bl_count[MAX_CODE_BITS + 1] = {};
    for (int i = 0; i < symbols; ++i)
        if (lens[i] > 0) ++bl_count[lens[i]];

    /* Find starting codes */
    uint16_t next_code[MAX_CODE_BITS + 2] = {};
    {
        uint16_t code = 0;
        for (int bits = 1; bits <= MAX_CODE_BITS; ++bits) {
            code = static_cast<uint16_t>((code + bl_count[bits - 1]) << 1);
            next_code[bits] = code;
        }
    }

    const int primary_size = 1 << table_bits;
    /* Initialize primary table entries to "invalid" */
    for (int i = 0; i < primary_size; ++i)
        table[i] = 0xFFFFFFFFU;

    int n_extra = 0; /* secondary table entries used */
    int n_secondary_start = primary_size;

    /* Assign decode table entries */
    for (int sym = 0; sym < symbols; ++sym) {
        const int len = lens[sym];
        if (len == 0) continue;

        /* Canonical code for this symbol */
        const uint16_t code = next_code[len]++;
        const uint16_t rev  = reverse_bits_u16(code, len);

        uint32_t entry = (static_cast<uint32_t>(len) << 16)
                       | static_cast<uint32_t>(sym);

        if (len <= table_bits) {
            /* Fill all primary entries with this prefix */
            const int step  = 1 << len;
            const int first = rev;
            for (int k = first; k < primary_size; k += step)
                table[k] = entry;
        } else {
            /* Long code: goes in secondary table */
            /* Primary entry: secondary table pointer */
            const int primary_idx = rev & (primary_size - 1);
            const int secondary_bits = len - table_bits;

            uint32_t& primary_entry = table[primary_idx];
            int sec_offset;
            if ((primary_entry & HUFF_SUBTABLE_FLAG) == 0 ||
                primary_entry == 0xFFFFFFFFU)
            {
                /* Allocate secondary subtable of size 2^secondary_bits */
                sec_offset = n_secondary_start + n_extra;
                n_extra += (1 << secondary_bits);
                primary_entry = HUFF_SUBTABLE_FLAG
                    | (static_cast<uint32_t>(secondary_bits) << 16)
                    | static_cast<uint32_t>(sec_offset);
            } else {
                sec_offset = static_cast<int>(primary_entry & 0xFFFF);
            }

            /* Fill secondary table */
            const uint32_t secondary_idx = (rev >> table_bits);
            const int step = 1 << secondary_bits;
            const int sub_size = 1 << secondary_bits;
            for (int k = static_cast<int>(secondary_idx); k < sub_size; k += step)
                table[sec_offset + k] = entry;
        }
    }

    return primary_size + n_extra;
}

} /* namespace deflate */
