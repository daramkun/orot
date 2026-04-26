#pragma once

#include <cstdint>

namespace orot { namespace zstd {

static constexpr int HUF_MAX_BITS = 13;
static constexpr int HUF_MAX_SYMS = 256;

struct HufDEntry {
    uint8_t symbol;
    uint8_t nb_bits; /* 0 = invalid / unused entry */
};

struct HufDTable {
    int       max_bits = 0;
    HufDEntry entries[1 << HUF_MAX_BITS];
};

/* Forward bitstream (start→end, MSB-first). Used for Huffman streams. */
struct HufBitStream {
    const uint8_t* ptr;
    const uint8_t* end;
    uint64_t       bits;
    int            bits_avail;

    void init(const uint8_t* s, int len) noexcept {
        ptr = s; end = s + len;
        bits = 0; bits_avail = 0;
        refill();
    }

    void refill() noexcept {
        while (bits_avail <= 56 && ptr < end) {
            bits = (bits << 8) | *ptr++;
            bits_avail += 8;
        }
    }

    uint32_t peek(int n) const noexcept {
        return static_cast<uint32_t>(bits >> (bits_avail - n)) & ((1u << n) - 1u);
    }

    void consume(int n) noexcept {
        bits_avail -= n;
        if (bits_avail < 32) const_cast<HufBitStream*>(this)->refill();
    }

    bool has_bits(int n) const noexcept { return bits_avail >= n; }
};

/* Build Huffman decode table from weights encoded in src.
   src[0] is the header byte:
     >= 128 → direct weights (n_syms = header & 0x7F, 2 nibbles per byte following)
     <  128 → FSE-compressed weights (compressed_size = header)
   bytes_read_out: total bytes consumed (header + data).
   Returns 0 on success, -1 on error. */
int huf_build_dtable(
    HufDTable&     ht,
    const uint8_t* src,
    int            src_len,
    int*           bytes_read_out) noexcept;

/* Decode one Huffman stream.
   src/src_len: compressed bitstream.
   dst/dst_len: output buffer (exact expected output count).
   Returns 0 on success, -1 on error. */
int huf_decode_1stream(
    const HufDTable& ht,
    const uint8_t*   src,
    int              src_len,
    uint8_t*         dst,
    int              dst_len) noexcept;

/* Decode four interleaved Huffman streams (6-byte jump table + 4 streams).
   dst_len: total expected output bytes.
   Returns 0 on success, -1 on error. */
int huf_decode_4stream(
    const HufDTable& ht,
    const uint8_t*   src,
    int              src_len,
    uint8_t*         dst,
    int              dst_len) noexcept;

} } /* namespace orot::zstd */
