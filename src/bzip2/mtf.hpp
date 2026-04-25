#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace orot::bzip2 {

/* MTF over a subset of the alphabet (in-use bytes only, as bzip2 requires).
   inuse_syms: sorted list of in-use byte values, n: count. */
struct MTFState {
    uint8_t sym[256];
    int     n;

    MTFState(const uint8_t* inuse_syms, int count) : n(count) {
        for (int i = 0; i < n; ++i) sym[i] = inuse_syms[i];
    }

    /* Encode one byte, return its MTF rank (0..n-1). */
    int encode(uint8_t c) {
        if (sym[0] == c) return 0;  /* fast path: most common after BWT */
        int rank = 1;
        while (rank < n && sym[rank] != c) ++rank;
        if (rank >= n) return -1; /* not in-use */
        memmove(sym + 1, sym, rank);
        sym[0] = c;
        return rank;
    }

    /* Decode one rank (0..n-1), return the original byte. */
    uint8_t decode(int rank) {
        uint8_t c = sym[rank];
        if (rank != 0) {
            memmove(sym + 1, sym, rank);
            sym[0] = c;
        }
        return c;
    }
};

/* Encode len bytes → MTF ranks. Uses in-use bytes as MTF alphabet.
   inuse_syms: sorted list; count: how many. */
inline void mtf_encode_inuse(uint8_t* data, uint32_t len,
                              const uint8_t* inuse_syms, int count) {
    MTFState s(inuse_syms, count);
    for (uint32_t i = 0; i < len; ++i)
        data[i] = (uint8_t)s.encode(data[i]);
}

/* Decode len MTF ranks → byte values. */
inline void mtf_decode_inuse(uint8_t* data, uint32_t len,
                              const uint8_t* inuse_syms, int count) {
    MTFState s(inuse_syms, count);
    for (uint32_t i = 0; i < len; ++i)
        data[i] = s.decode(data[i]);
}

} // namespace orot::bzip2
