#include "decompressor.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace orot { namespace deflate {

/* Code-length alphabet order (RFC 1951) */
static const int CL_ORDER[CODELEN_SYMS] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

Decompressor::Decompressor()
    : arena_buf_(ARENA_SIZE)
    , arena_(arena_buf_.data(), ARENA_SIZE)
    , window_(WIN_SIZE)
{
    tables_ = arena_.alloc<InflateTables>();
}

void Decompressor::reset() {
    state_      = State::BLOCK_HEADER;
    is_final_   = false;
    bits_       = 0;
    bit_count_  = 0;
    win_pos_    = 0;
    codelens_i_ = 0;
    arena_.reset();
    tables_ = arena_.alloc<InflateTables>();
    std::memset(window_.data(), 0, WIN_SIZE);
    out_origin_       = nullptr;
    out_expected_end_ = nullptr;
}

/*
 * Append `len` bytes from `buf` into the circular window_ at win_pos_.
 * Used after inflate_fast to keep window_ coherent for the fallback path.
 */
void Decompressor::sync_window_from_buf(const uint8_t* buf, size_t len) noexcept {
    if (len == 0) return;
    if (len > WIN_SIZE) {
        buf += len - WIN_SIZE;
        len  = WIN_SIZE;
    }
    const size_t dst = win_pos_ & (WIN_SIZE - 1);
    const size_t first = std::min(len, WIN_SIZE - dst);
    std::memcpy(window_.data() + dst, buf, first);
    if (first < len)
        std::memcpy(window_.data(), buf + first, len - first);
    win_pos_ += len;
}

bool Decompressor::build_tables_from_combined() {
    build_dec_table_from_lens(
        combined_lens_, hlit_,
        LITLEN_DECODE_BITS, tables_->litlen);
    build_dec_table_from_lens(
        combined_lens_ + hlit_, hdist_,
        DIST_DECODE_BITS, tables_->dist);
    return true;
}

/* ── Bit I/O helpers ─────────────────────────────────────────────────────── */

/*
 * Fill bit buffer from avail_in as much as possible (up to 64 bits).
 * Non-blocking: returns without reading if avail_in == 0.
 */
#define FILL_BITS_MAX()                                                 \
    do {                                                                \
        while (bit_count_ <= 56 && avail_in > 0) {                     \
            bits_      |= static_cast<uint64_t>(*next_in++) << bit_count_; \
            bit_count_ += 8;                                            \
            --avail_in;                                                 \
        }                                                               \
    } while (0)

/* Fill bit buffer used in decode hot path (same as FILL_BITS_MAX). */
#define FILL_BITS()  FILL_BITS_MAX()

#define PEEK_BITS(n)  (static_cast<uint32_t>(bits_) & ((1U << (n)) - 1))
#define DROP_BITS(n)  do { bits_ >>= (n); bit_count_ -= (n); } while (0)

deflate_result Decompressor::decompress(
    const uint8_t** next_in_,  size_t* avail_in_,
    uint8_t**       next_out_, size_t* avail_out_)
{
    const uint8_t*& next_in   = *next_in_;
    size_t&         avail_in  = *avail_in_;
    uint8_t*&       next_out  = *next_out_;
    size_t&         avail_out = *avail_out_;

    if (state_ == State::DONE)  return DEFLATE_STREAM_END;
    if (state_ == State::ERROR) return DEFLATE_DATA_ERROR;

    /* Track contiguous output buffer for inflate_fast back-reference resolution. */
    const bool out_contiguous =
        (out_origin_ != nullptr) && (next_out == out_expected_end_);
    if (out_origin_ == nullptr || !out_contiguous) {
        out_origin_ = next_out;
    }
    /* RAII: update out_expected_end_ on every return path. */
    struct OutEndTracker {
        uint8_t*& expected;
        uint8_t*& cur;
        ~OutEndTracker() { expected = cur; }
    } out_tracker{out_expected_end_, next_out};

loop:
    switch (state_) {

    /* ── Block header ──────────────────────────────────────────────────── */
    case State::BLOCK_HEADER: {
        /*
         * Read just enough bytes to determine block type (3 bits needed).
         * For stored blocks (btype=0) we must NOT over-read avail_in because
         * STORED_COPY reads the payload directly from avail_in — any extra
         * bytes consumed here via FILL_BITS would be silently lost.
         * For dynamic Huffman (btype=2) we additionally need 14 more bits
         * (5+5+4 for HLIT/HDIST/HCLEN), so keep reading until we have 17.
         */
        while (bit_count_ < 3 && avail_in > 0) {
            bits_      |= static_cast<uint64_t>(*next_in++) << bit_count_;
            bit_count_ += 8;
            --avail_in;
        }
        if (bit_count_ < 3) return DEFLATE_OK;

        /* Peek at btype (bits 1-2) without consuming */
        const uint32_t btype_peek = (static_cast<uint32_t>(bits_) >> 1) & 3;

        /* Dynamic Huffman needs 17 bits total: read more bytes if required */
        if (btype_peek == 2) {
            while (bit_count_ < 17 && avail_in > 0) {
                bits_      |= static_cast<uint64_t>(*next_in++) << bit_count_;
                bit_count_ += 8;
                --avail_in;
            }
            if (bit_count_ < 17) return DEFLATE_OK;
        }

        /* Now safe: consume BFINAL + BTYPE */
        const uint32_t bfinal = PEEK_BITS(1); DROP_BITS(1);
        const uint32_t btype  = PEEK_BITS(2); DROP_BITS(2);
        is_final_ = (bfinal != 0);

        if (btype == 0) {
            /* Stored: skip to byte boundary */
            const int pad = bit_count_ & 7;
            if (pad) DROP_BITS(pad);
            state_ = State::STORED_LEN;
        } else if (btype == 1) {
            build_fixed_litlen_dec(tables_->litlen);
            build_fixed_dist_dec  (tables_->dist);
            state_ = State::DECODE_LITLEN;
        } else if (btype == 2) {
            /* Dynamic: consume HLIT + HDIST + HCLEN (already checked 17 bits) */
            const uint32_t hlit  = PEEK_BITS(5); DROP_BITS(5);
            const uint32_t hdist = PEEK_BITS(5); DROP_BITS(5);
            const uint32_t hclen = PEEK_BITS(4); DROP_BITS(4);
            hlit_  = static_cast<int>(hlit)  + 257;
            hdist_ = static_cast<int>(hdist) + 1;
            hclen_ = static_cast<int>(hclen) + 4;
            cl_idx_      = 0;
            codelens_i_  = 0;
            std::memset(cl_lens_, 0, sizeof(cl_lens_));
            std::memset(combined_lens_, 0, sizeof(combined_lens_));
            state_ = State::CODE_LENGTHS;
        } else {
            state_ = State::ERROR;
            return DEFLATE_DATA_ERROR;
        }
        goto loop;
    }

    /* ── Code-length decoding ─────────────────────────────────────────── */
    case State::CODE_LENGTHS: {
        /* Phase 1: read hclen_ 3-bit code-length lengths.
         * cl_idx_ is a member, so this loop is resumable. */
        while (cl_idx_ < hclen_) {
            FILL_BITS_MAX();
            if (bit_count_ < 3) return DEFLATE_OK;
            const uint32_t v = PEEK_BITS(3); DROP_BITS(3);
            cl_lens_[CL_ORDER[cl_idx_++]] = static_cast<uint8_t>(v);
        }

        /* Build code-length decode table */
        static uint32_t cl_table[128 + 64];
        build_dec_table_from_lens(cl_lens_, CODELEN_SYMS, 7, cl_table);

        /* Phase 2: decode litlen + dist code lengths.
         * codelens_i_ is a member for resumability.
         * We use a pre-check approach: fill bits, peek sym, determine
         * total bits needed (sym code + possible extra), only consume
         * if we have enough — so partial failures never corrupt state. */
        const int total = hlit_ + hdist_;
        while (codelens_i_ < total) {
            FILL_BITS_MAX();
            if (bit_count_ < 1) return DEFLATE_OK;

            /* Look up the next code-length symbol (up to 7-bit table). */
            const int     peek  = (bit_count_ >= 7) ? 7 : bit_count_;
            const uint32_t e    = cl_table[PEEK_BITS(peek)];
            const int      sym  = static_cast<int>(e & 0xFFFF);
            const int      ebits = static_cast<int>((e >> 16) & 0xFF);

            /* Extra bits required beyond the symbol code itself */
            int extra_needed = 0;
            if      (sym == 16) extra_needed = 2;
            else if (sym == 17) extra_needed = 3;
            else if (sym == 18) extra_needed = 7;

            /* Ensure we have the complete atom before consuming anything */
            if (bit_count_ < ebits + extra_needed) return DEFLATE_OK;

            /* Consume symbol code */
            DROP_BITS(ebits);

            if (sym < 16) {
                combined_lens_[codelens_i_++] = static_cast<uint8_t>(sym);
            } else if (sym == 16) {
                const uint32_t rep_v = PEEK_BITS(2); DROP_BITS(2);
                const int rep = static_cast<int>(rep_v) + 3;
                if (codelens_i_ == 0) { state_ = State::ERROR; return DEFLATE_DATA_ERROR; }
                const uint8_t prev = combined_lens_[codelens_i_ - 1];
                for (int r = 0; r < rep && codelens_i_ < total; ++r)
                    combined_lens_[codelens_i_++] = prev;
            } else if (sym == 17) {
                const uint32_t rep_v = PEEK_BITS(3); DROP_BITS(3);
                const int rep = static_cast<int>(rep_v) + 3;
                for (int r = 0; r < rep && codelens_i_ < total; ++r)
                    combined_lens_[codelens_i_++] = 0;
            } else {  /* sym == 18 */
                const uint32_t rep_v = PEEK_BITS(7); DROP_BITS(7);
                const int rep = static_cast<int>(rep_v) + 11;
                for (int r = 0; r < rep && codelens_i_ < total; ++r)
                    combined_lens_[codelens_i_++] = 0;
            }
        }

        if (!build_tables_from_combined()) {
            state_ = State::ERROR;
            return DEFLATE_DATA_ERROR;
        }
        state_ = State::DECODE_LITLEN;
        goto loop;
    }

    /* ── Stored block length ──────────────────────────────────────────── */
    case State::STORED_LEN: {
        /* Need exactly 32 bits (LEN 16 + NLEN 16).
         * Read only as many bytes as needed to reach 32 bits — do NOT
         * over-read, because STORED_COPY reads the payload from avail_in. */
        while (bit_count_ < 32 && avail_in > 0) {
            bits_      |= static_cast<uint64_t>(*next_in++) << bit_count_;
            bit_count_ += 8;
            --avail_in;
        }
        if (bit_count_ < 32) return DEFLATE_OK;

        const uint32_t len_lo  = PEEK_BITS(8); DROP_BITS(8);
        const uint32_t len_hi  = PEEK_BITS(8); DROP_BITS(8);
        const uint32_t nlen_lo = PEEK_BITS(8); DROP_BITS(8);
        const uint32_t nlen_hi = PEEK_BITS(8); DROP_BITS(8);
        const uint16_t len  = static_cast<uint16_t>(len_lo  | (len_hi  << 8));
        const uint16_t nlen = static_cast<uint16_t>(nlen_lo | (nlen_hi << 8));
        if ((len ^ 0xFFFFU) != nlen) {
            state_ = State::ERROR;
            return DEFLATE_DATA_ERROR;
        }
        stored_len_ = len;
        stored_pos_ = 0;
        state_      = State::STORED_COPY;
        goto loop;
    }

    /* ── Stored block copy ────────────────────────────────────────────── */
    case State::STORED_COPY: {
        while (stored_pos_ < stored_len_) {
            if (avail_in  == 0) return DEFLATE_OK;
            if (avail_out == 0) return DEFLATE_NEED_OUTPUT;
            const size_t copy = std::min<size_t>(
                static_cast<size_t>(stored_len_ - stored_pos_),
                std::min(avail_in, avail_out));
            std::memcpy(next_out, next_in, copy);
            /* Contiguous mode: back-references resolved from out_origin_, no
             * need to copy into circular window_.  Streaming mode still syncs. */
            if (out_origin_ != nullptr)
                win_pos_ += copy;
            else
                sync_window_from_buf(next_in, copy);
            next_in    += copy; avail_in  -= copy;
            next_out   += copy; avail_out -= copy;
            stored_pos_ = static_cast<uint16_t>(stored_pos_ + static_cast<uint16_t>(copy));
        }
        if (is_final_) { state_ = State::DONE; return DEFLATE_STREAM_END; }
        state_ = State::BLOCK_HEADER;
        goto loop;
    }

    /* ── Huffman decode ───────────────────────────────────────────────── */
    case State::DECODE_LITLEN: {
        /* Fast path: hand off to inflate_fast when:
         *  - enough raw input for safe 8-byte bulk refills
         *  - enough output headroom for the longest possible match (258 bytes)
         *  - all history (up to WIN_SIZE bytes) is accessible in the contiguous
         *    output buffer [out_origin_, next_out), so inflate_fast can resolve
         *    back-references without consulting window_[]
         * After inflate_fast, sync window_ so the slow-path fallback works.
         */
        if (out_origin_ != nullptr && avail_in >= 10 && avail_out >= 258) {
            const size_t history_avail =
                static_cast<size_t>(next_out - out_origin_);
            if (history_avail >= std::min(win_pos_, WIN_SIZE)) {
                uint8_t* const fast_out_start = next_out;
                BitReader br(next_in, avail_in, bits_, bit_count_);

                const bool ended =
                    inflate_fast(br, const_cast<uint8_t*>(out_origin_),
                                 next_out, next_out + avail_out, *tables_);

                /* Sync raw input state back. */
                const size_t in_consumed =
                    static_cast<size_t>(br.current_ptr() - next_in);
                next_in    += in_consumed;
                avail_in   -= in_consumed;
                bits_       = br.raw_bits();
                bit_count_  = br.raw_bit_count();

                /* Track produced output.  In contiguous mode, back-references
                 * are resolved directly from out_origin_, so window_ sync is
                 * deferred; only win_pos_ needs updating.  Streaming mode still
                 * syncs so the slow-path COPY_MATCH has valid window state. */
                const size_t produced =
                    static_cast<size_t>(next_out - fast_out_start);
                avail_out -= produced;
                win_pos_  += produced;
                /* Sync window_ so slow-path COPY_MATCH can resolve back-refs.
                 * Skip when out_origin_ already covers the full WIN_SIZE history:
                 * slow-path resolves from out_origin_ directly in that case. */
                if (produced > 0) {
                    const size_t history_after =
                        static_cast<size_t>(next_out - out_origin_);
                    if (history_after < WIN_SIZE) {
                        const uint8_t* wsrc = fast_out_start;
                        size_t         wsz  = produced;
                        if (wsz > WIN_SIZE) { wsrc += wsz - WIN_SIZE; wsz = WIN_SIZE; }
                        const size_t dst = (win_pos_ - wsz) & (WIN_SIZE - 1);
                        const size_t f   = std::min(wsz, WIN_SIZE - dst);
                        std::memcpy(window_.data() + dst, wsrc, f);
                        if (f < wsz)
                            std::memcpy(window_.data(), wsrc + f, wsz - f);
                    }
                }

                if (ended) {
                    if (is_final_) {
                        const int pb = bit_count_ >> 3;
                        next_in -= pb; avail_in += static_cast<size_t>(pb);
                        bits_ = 0; bit_count_ = 0;
                        state_ = State::DONE; return DEFLATE_STREAM_END;
                    }
                    state_ = State::BLOCK_HEADER;
                    goto loop;
                }
                /* inflate_fast exited due to output/input margin: fall through
                 * to slow path for remaining symbols in this block. */
            }
        }

        while (avail_out > 0) {
            /* Fill bit buffer; near end-of-stream last symbol may be shorter
             * than LITLEN_DECODE_BITS so we cannot require a full refill. */
            FILL_BITS();
            if (bit_count_ < 1) return DEFLATE_OK;

            const int peek = (bit_count_ >= LITLEN_DECODE_BITS)
                             ? LITLEN_DECODE_BITS : bit_count_;
            uint32_t e = tables_->litlen[PEEK_BITS(peek)];

            if (e & HUFF_SUBTABLE_FLAG) {
                const int sec_bits   = static_cast<int>((e >> 16) & 0xFF);
                const int sec_offset = static_cast<int>(e & 0xFFFF);
                const int needed     = LITLEN_DECODE_BITS + sec_bits;
                if (bit_count_ < needed) return DEFLATE_OK;
                e = tables_->litlen[sec_offset +
                    (PEEK_BITS(needed) >> LITLEN_DECODE_BITS)];
            }

            const int ebits = static_cast<int>((e >> 16) & 0xFF);
            if (ebits > bit_count_) return DEFLATE_OK;

            if (e & HUFF_LITERAL_FLAG) {
                /* Literal: sym in bits[7:0], no extra bits */
                DROP_BITS(ebits);
                const uint8_t byte = static_cast<uint8_t>(e);
                *next_out++ = byte;
                --avail_out;
                /* In contiguous mode, COPY_MATCH reads from out_origin_ directly.
                 * Only write into circular window_ in streaming mode. */
                window_.data()[win_pos_ & (WIN_SIZE - 1)] = byte;
                ++win_pos_;
            } else {
                const int sym = static_cast<int>(e & 0xFFFF);
                if (sym == 256) {
                    /* End-of-block: no extra bits needed */
                    DROP_BITS(ebits);
                    if (is_final_) {
                        const int pb = bit_count_ >> 3;
                        next_in -= pb; avail_in += static_cast<size_t>(pb);
                        bits_ = 0; bit_count_ = 0;
                        state_ = State::DONE; return DEFLATE_STREAM_END;
                    }
                    state_ = State::BLOCK_HEADER;
                    goto loop;
                } else {
                    /* Back-reference: sym 257-285 only; 286-287 are invalid */
                    if (sym > 285) { state_ = State::ERROR; return DEFLATE_DATA_ERROR; }
                    const int li         = sym - 257;
                    const int extra_bits = LENGTH_EXTRA_BITS[li];
                    if (bit_count_ < ebits + extra_bits) return DEFLATE_OK;

                    DROP_BITS(ebits);
                    match_len_ = LENGTH_BASE[li];
                    if (extra_bits > 0) {
                        const uint32_t extra = PEEK_BITS(extra_bits);
                        DROP_BITS(extra_bits);
                        match_len_ += static_cast<int>(extra);
                    }
                    state_ = State::DECODE_DIST;
                    goto loop;
                }
            }
        }
        return DEFLATE_NEED_OUTPUT;
    }

    case State::DECODE_DIST: {
        FILL_BITS();
        if (bit_count_ < 1) return DEFLATE_OK;

        const int dpeek = (bit_count_ >= DIST_DECODE_BITS)
                          ? DIST_DECODE_BITS : bit_count_;
        uint32_t de = tables_->dist[PEEK_BITS(dpeek)];

        if (de & HUFF_SUBTABLE_FLAG) {
            const int sec_bits   = static_cast<int>((de >> 16) & 0xFF);
            const int sec_offset = static_cast<int>(de & 0xFFFF);
            const int needed     = DIST_DECODE_BITS + sec_bits;
            if (bit_count_ < needed) return DEFLATE_OK;
            de = tables_->dist[sec_offset +
                (PEEK_BITS(needed) >> DIST_DECODE_BITS)];
        }

        const int di     = static_cast<int>(de & 0xFFFF);
        const int debits = static_cast<int>((de >> 16) & 0xFF);

        /* Validate distance code (0-29 are the only defined codes; DIST_SYMS=32 is padded) */
        if (di >= 30) { state_ = State::ERROR; return DEFLATE_DATA_ERROR; }

        /* Pre-check: need distance code + DIST_EXTRA_BITS atomically */
        const int dist_extra = DIST_EXTRA_BITS[di];
        if (bit_count_ < debits + dist_extra) return DEFLATE_OK;

        DROP_BITS(debits);
        match_dist_ = DIST_BASE[di];
        if (dist_extra > 0) {
            const uint32_t extra = PEEK_BITS(dist_extra);
            DROP_BITS(dist_extra);
            match_dist_ += static_cast<int>(extra);
        }
        state_ = State::COPY_MATCH;
        [[fallthrough]];
    }

    case State::COPY_MATCH: {
        while (match_len_ > 0 && avail_out > 0) {
            const size_t back = static_cast<size_t>(match_dist_);
            const size_t len  = std::min(static_cast<size_t>(match_len_), avail_out);

            /* Contiguous mode: back-reference falls within the output buffer we
             * already wrote.  Resolve directly without touching window_[]. */
            if (out_origin_ != nullptr &&
                static_cast<size_t>(next_out - out_origin_) >= back) {

                const uint8_t* src_ptr = next_out - back;
                if (back == 1) {
                    std::memset(next_out, src_ptr[0], len);
                } else if (back >= len) {
                    std::memcpy(next_out, src_ptr, len);
                } else {
                    /* Overlapping: doubling expansion in-place */
                    size_t filled = back;
                    std::memcpy(next_out, src_ptr, filled);
                    while (filled + filled <= len) {
                        std::memcpy(next_out + filled, next_out, filled);
                        filled += filled;
                    }
                    if (filled < len)
                        std::memcpy(next_out + filled, next_out, len - filled);
                }
                /* Sync window_ before advancing win_pos_. */
                {
                    const size_t dst = win_pos_ & (WIN_SIZE - 1);
                    const size_t f   = std::min(len, WIN_SIZE - dst);
                    std::memcpy(window_.data() + dst, next_out, f);
                    if (f < len)
                        std::memcpy(window_.data(), next_out + f, len - f);
                }
                next_out   += len;
                avail_out  -= len;
                win_pos_   += len;
                match_len_ -= static_cast<int>(len);

            } else if (back == 1) {
                /* Single-byte RLE: memset the repeated byte. */
                const uint8_t byte = window_.data()[(win_pos_ - 1) & (WIN_SIZE - 1)];
                std::memset(next_out, byte, len);
                /* Update circular window. */
                const size_t dst = win_pos_ & (WIN_SIZE - 1);
                const size_t f   = std::min(len, WIN_SIZE - dst);
                std::memset(window_.data() + dst, byte, f);
                if (f < len) std::memset(window_.data(), byte, len - f);
                next_out   += len;
                avail_out  -= len;
                win_pos_   += len;
                match_len_ -= static_cast<int>(len);
            } else if (back >= len) {
                /* Non-overlapping: safe to bulk-copy from window → out. */
                const size_t src = (win_pos_ - back) & (WIN_SIZE - 1);
                /* Read from circular window (may wrap). */
                const size_t f1 = std::min(len, WIN_SIZE - src);
                std::memcpy(next_out, window_.data() + src, f1);
                if (f1 < len)
                    std::memcpy(next_out + f1, window_.data(), len - f1);
                /* Write produced bytes into circular window. */
                const size_t dst = win_pos_ & (WIN_SIZE - 1);
                const size_t f2  = std::min(len, WIN_SIZE - dst);
                std::memcpy(window_.data() + dst, next_out, f2);
                if (f2 < len)
                    std::memcpy(window_.data(), next_out + f2, len - f2);
                next_out   += len;
                avail_out  -= len;
                win_pos_   += len;
                match_len_ -= static_cast<int>(len);
            } else {
                /* Overlapping (back < len, back > 1): doubling memcpy expansion.
                 * Read base pattern from circular window, then double until len bytes. */
                uint8_t pattern[258];
                const size_t src_pos = (win_pos_ - back) & (WIN_SIZE - 1);
                const size_t f0 = std::min(back, WIN_SIZE - src_pos);
                std::memcpy(pattern, window_.data() + src_pos, f0);
                if (f0 < back) std::memcpy(pattern + f0, window_.data(), back - f0);

                size_t b = back;
                while (b + b <= len) {
                    std::memcpy(pattern + b, pattern, b);
                    b += b;
                }
                if (b < len) std::memcpy(pattern + b, pattern, len - b);

                std::memcpy(next_out, pattern, len);
                next_out  += len;
                avail_out -= len;

                const size_t dst = win_pos_ & (WIN_SIZE - 1);
                const size_t fw = std::min(len, WIN_SIZE - dst);
                std::memcpy(window_.data() + dst, pattern, fw);
                if (fw < len) std::memcpy(window_.data(), pattern + fw, len - fw);

                win_pos_   += len;
                match_len_ -= static_cast<int>(len);
            }
        }
        if (match_len_ > 0) return DEFLATE_NEED_OUTPUT;
        state_ = State::DECODE_LITLEN;
        goto loop;
    }

    case State::DONE:   return DEFLATE_STREAM_END;
    case State::ERROR:  return DEFLATE_DATA_ERROR;

    } /* switch */

    return DEFLATE_DATA_ERROR;  /* unreachable */
}

#undef FILL_BITS
#undef FILL_BITS_MAX
#undef PEEK_BITS
#undef DROP_BITS

} } /* namespace orot::deflate */
