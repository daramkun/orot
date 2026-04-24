#include "orot/deflate.h"
#include "compress/compressor.hpp"
#include "decompress/decompressor.hpp"
#include "parallel/parallel_compressor.hpp"
#include "simd/simd_dispatch.hpp"

#include <new>
#include <cstring>
#include <algorithm>

/* ════════════════════════════════════════════════════════════════════════
 * Format constants
 * ════════════════════════════════════════════════════════════════════════ */

static constexpr uint8_t ZLIB_CMF = 0x78;  /* deflate, window=32KB */
static constexpr uint8_t ZLIB_FLG = 0x9C;  /* (0x78*256+0x9C)%31==0, no dict */

static constexpr uint8_t GZIP_ID1  = 0x1F;
static constexpr uint8_t GZIP_ID2  = 0x8B;
static constexpr uint8_t GZIP_CM   = 8;
static constexpr uint8_t GZIP_OS   = 255;

/* ════════════════════════════════════════════════════════════════════════
 * Gzip header parser: returns header length, or -1 (need more), -2 (bad)
 * ════════════════════════════════════════════════════════════════════════ */
static int gzip_header_size(const uint8_t* buf, int len) {
    if (len < 10) return -1;
    if (buf[0] != GZIP_ID1 || buf[1] != GZIP_ID2) return -2;
    if (buf[2] != GZIP_CM) return -2;
    const uint8_t flags = buf[3];
    int off = 10;
    if (flags & 0x04) {  /* FEXTRA */
        if (len < off + 2) return -1;
        const int xlen = buf[off] | (buf[off+1] << 8);
        off += 2 + xlen;
        if (len < off) return -1;
    }
    if (flags & 0x08) {  /* FNAME */
        while (off < len && buf[off] != 0) ++off;
        if (off >= len) return -1;
        ++off;
    }
    if (flags & 0x10) {  /* FCOMMENT */
        while (off < len && buf[off] != 0) ++off;
        if (off >= len) return -1;
        ++off;
    }
    if (flags & 0x02) {  /* FHCRC */
        off += 2;
        if (len < off) return -1;
    }
    return off;
}

/* ════════════════════════════════════════════════════════════════════════
 * deflate_stream: streaming compress / decompress
 * ════════════════════════════════════════════════════════════════════════ */

struct deflate_stream {
    enum class Kind { Compress, Decompress };
    Kind kind;

    union {
        orot::deflate::Compressor*   compressor;
        orot::deflate::Decompressor* decompressor;
    };

    deflate_format format;

    /* ── Format wrapper state ─────────────────────────────────────── */
    enum class Phase { HEADER, DATA, TRAILER, DONE };
    Phase phase = Phase::HEADER;

    /* Pending output bytes (header or trailer to be drained to caller) */
    static constexpr int FMT_BUF = 32;  /* big enough: max gzip hdr + 8B trailer */
    uint8_t fmt_out_[FMT_BUF] = {};
    int     fmt_out_head_ = 0;  /* next byte to drain */
    int     fmt_out_tail_ = 0;  /* one past last available byte */

    /* Header accumulation for decompress */
    static constexpr int HDR_BUF = 256;  /* handles large gzip headers */
    uint8_t hdr_buf_[HDR_BUF] = {};
    int     hdr_pos_ = 0;

    /* Trailer accumulation for decompress */
    uint8_t trl_buf_[8] = {};
    int     trl_pos_ = 0;

    /* Running checksum (adler32 for zlib, crc32 for gzip) */
    uint32_t checksum_  = 0;
    uint32_t total_out_ = 0;  /* for gzip ISIZE */

    ~deflate_stream() {
        if (kind == Kind::Compress)
            delete compressor;
        else
            delete decompressor;
    }
};

struct deflate_parallel_ctx {
    orot::deflate::ParallelCompressor* impl;
    ~deflate_parallel_ctx() { delete impl; }
};

/* ── helpers ─────────────────────────────────────────────────────────── */
static void stream_push_out(deflate_stream* s, const uint8_t* data, int len) {
    /* Append 'len' bytes to the pending output buffer */
    const int space = deflate_stream::FMT_BUF - s->fmt_out_tail_;
    const int copy  = std::min(len, space);
    std::memcpy(s->fmt_out_ + s->fmt_out_tail_, data, copy);
    s->fmt_out_tail_ += copy;
    /* (copy == len in practice; header+trailer are tiny) */
}

static deflate_result stream_drain_out(deflate_stream* s,
                                       uint8_t** next_out, size_t* avail_out) {
    while (s->fmt_out_head_ < s->fmt_out_tail_ && *avail_out > 0) {
        **next_out = s->fmt_out_[s->fmt_out_head_++];
        ++(*next_out);
        --(*avail_out);
    }
    if (s->fmt_out_head_ < s->fmt_out_tail_) return DEFLATE_NEED_OUTPUT;
    s->fmt_out_head_ = s->fmt_out_tail_ = 0;  /* reset buffer */
    return DEFLATE_OK;
}

extern "C" {

/* ════════════════════════════════════════════════════════════════════════
 * Streaming compression
 * ════════════════════════════════════════════════════════════════════════ */

deflate_stream* deflate_stream_new(int level, deflate_format format) {
    auto* s = new (std::nothrow) deflate_stream;
    if (!s) return nullptr;
    s->kind       = deflate_stream::Kind::Compress;
    s->format     = format;
    s->compressor = new (std::nothrow) orot::deflate::Compressor(level);
    if (!s->compressor) { delete s; return nullptr; }

    /* RAW format: skip header/trailer entirely */
    if (format == DEFLATE_FORMAT_RAW) s->phase = deflate_stream::Phase::DATA;

    /* Initialize checksum */
    s->checksum_ = (format == DEFLATE_FORMAT_ZLIB) ? 1u : 0u;
    return s;
}

void deflate_stream_free(deflate_stream* s) { delete s; }

deflate_result deflate_stream_compress(
    deflate_stream*  s,
    const uint8_t**  next_in,  size_t* avail_in,
    uint8_t**        next_out, size_t* avail_out,
    deflate_flush    flush)
{
    if (!s || s->kind != deflate_stream::Kind::Compress)
        return DEFLATE_PARAM_ERROR;

    for (;;) {
        /* 1. Drain any pending format bytes (header or trailer) */
        deflate_result dr = stream_drain_out(s, next_out, avail_out);
        if (dr == DEFLATE_NEED_OUTPUT) return DEFLATE_NEED_OUTPUT;

        switch (s->phase) {

        case deflate_stream::Phase::HEADER: {
            /* Build and queue the format header */
            if (s->format == DEFLATE_FORMAT_ZLIB) {
                const uint8_t hdr[2] = { ZLIB_CMF, ZLIB_FLG };
                stream_push_out(s, hdr, 2);
            } else {  /* GZIP */
                uint8_t hdr[10] = {
                    GZIP_ID1, GZIP_ID2, GZIP_CM,
                    0,                          /* FLG: no extras */
                    0, 0, 0, 0,                 /* MTIME = 0 */
                    0,                          /* XFL */
                    GZIP_OS
                };
                stream_push_out(s, hdr, 10);
            }
            s->phase = deflate_stream::Phase::DATA;
            continue;  /* loop to drain header then enter DATA */
        }

        case deflate_stream::Phase::DATA: {
            /* Track input to accumulate checksum */
            const uint8_t* in_before = *next_in;
            const size_t   in_before_avail = *avail_in;

            deflate_result r = s->compressor->compress(
                next_in, avail_in, next_out, avail_out, flush);

            /* Accumulate checksum on bytes consumed */
            const size_t consumed = in_before_avail - *avail_in;
            if (consumed > 0) {
                if (s->format == DEFLATE_FORMAT_ZLIB)
                    s->checksum_ = orot::deflate::simd_adler32_fn()(s->checksum_, in_before, consumed);
                else if (s->format == DEFLATE_FORMAT_GZIP)
                    s->checksum_ = orot::deflate::simd_crc32_fn()(s->checksum_, in_before, consumed);
                s->total_out_ += static_cast<uint32_t>(consumed);
            }

            if (r == DEFLATE_STREAM_END) {
                if (s->format == DEFLATE_FORMAT_RAW) return DEFLATE_STREAM_END;
                s->phase = deflate_stream::Phase::TRAILER;
                /* Build trailer into pending buffer */
                if (s->format == DEFLATE_FORMAT_ZLIB) {
                    const uint32_t a = s->checksum_;
                    uint8_t trl[4] = {
                        static_cast<uint8_t>(a >> 24),
                        static_cast<uint8_t>(a >> 16),
                        static_cast<uint8_t>(a >>  8),
                        static_cast<uint8_t>(a)
                    };
                    stream_push_out(s, trl, 4);
                } else {  /* GZIP */
                    const uint32_t c = s->checksum_;
                    const uint32_t n = s->total_out_;
                    uint8_t trl[8] = {
                        static_cast<uint8_t>(c),
                        static_cast<uint8_t>(c >>  8),
                        static_cast<uint8_t>(c >> 16),
                        static_cast<uint8_t>(c >> 24),
                        static_cast<uint8_t>(n),
                        static_cast<uint8_t>(n >>  8),
                        static_cast<uint8_t>(n >> 16),
                        static_cast<uint8_t>(n >> 24),
                    };
                    stream_push_out(s, trl, 8);
                }
                s->phase = deflate_stream::Phase::DONE;
                continue;  /* loop to drain trailer, then return DEFLATE_STREAM_END */
            }
            return r;  /* DEFLATE_OK, DEFLATE_NEED_OUTPUT, or error */
        }

        case deflate_stream::Phase::DONE:
            /* All trailer bytes drained (stream_drain_out cleared pending) */
            return DEFLATE_STREAM_END;

        default:
            return DEFLATE_DATA_ERROR;
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * Streaming decompression
 * ════════════════════════════════════════════════════════════════════════ */

deflate_stream* inflate_stream_new(deflate_format format) {
    auto* s = new (std::nothrow) deflate_stream;
    if (!s) return nullptr;
    s->kind         = deflate_stream::Kind::Decompress;
    s->format       = format;
    s->decompressor = new (std::nothrow) orot::deflate::Decompressor;
    if (!s->decompressor) { delete s; return nullptr; }

    /* RAW: skip header/trailer */
    if (format == DEFLATE_FORMAT_RAW) s->phase = deflate_stream::Phase::DATA;

    /* Checksum init */
    s->checksum_ = (format == DEFLATE_FORMAT_ZLIB) ? 1u : 0u;
    return s;
}

void inflate_stream_free(deflate_stream* s) { delete s; }

deflate_result inflate_stream_decompress(
    deflate_stream*  s,
    const uint8_t**  next_in,  size_t* avail_in,
    uint8_t**        next_out, size_t* avail_out)
{
    if (!s || s->kind != deflate_stream::Kind::Decompress)
        return DEFLATE_PARAM_ERROR;

    for (;;) {
        switch (s->phase) {

        case deflate_stream::Phase::HEADER: {
            /* Accumulate header bytes until we can parse the complete header */
            while (*avail_in > 0 && s->hdr_pos_ < deflate_stream::HDR_BUF) {
                s->hdr_buf_[s->hdr_pos_++] = **next_in;
                ++(*next_in);
                --(*avail_in);

                if (s->format == DEFLATE_FORMAT_ZLIB) {
                    if (s->hdr_pos_ < 2) continue;
                    /* Validate CMF/FLG */
                    if ((s->hdr_buf_[0] & 0x0F) != 8) return DEFLATE_DATA_ERROR;
                    if (((uint32_t)s->hdr_buf_[0] * 256 + s->hdr_buf_[1]) % 31 != 0)
                        return DEFLATE_DATA_ERROR;
                    if (s->hdr_buf_[1] & 0x20) return DEFLATE_DATA_ERROR;  /* no preset dict */
                    s->phase = deflate_stream::Phase::DATA;
                    break;
                } else {  /* GZIP */
                    const int hs = gzip_header_size(s->hdr_buf_, s->hdr_pos_);
                    if (hs == -2) return DEFLATE_DATA_ERROR;
                    if (hs > 0) {
                        s->phase = deflate_stream::Phase::DATA;
                        break;
                    }
                    /* hs == -1: need more bytes */
                }
            }
            if (s->phase == deflate_stream::Phase::HEADER) return DEFLATE_OK;
            continue;
        }

        case deflate_stream::Phase::DATA: {
            const uint8_t* out_before = *next_out;

            deflate_result r = s->decompressor->decompress(
                next_in, avail_in, next_out, avail_out);

            /* Accumulate checksum on decompressed output */
            const size_t produced = static_cast<size_t>(*next_out - out_before);
            if (produced > 0) {
                if (s->format == DEFLATE_FORMAT_ZLIB)
                    s->checksum_ = orot::deflate::simd_adler32_fn()(s->checksum_, out_before, produced);
                else if (s->format == DEFLATE_FORMAT_GZIP)
                    s->checksum_ = orot::deflate::simd_crc32_fn()(s->checksum_, out_before, produced);
                s->total_out_ += static_cast<uint32_t>(produced);
            }

            if (r == DEFLATE_STREAM_END) {
                if (s->format == DEFLATE_FORMAT_RAW) return DEFLATE_STREAM_END;
                s->phase = deflate_stream::Phase::TRAILER;
                continue;
            }
            return r;
        }

        case deflate_stream::Phase::TRAILER: {
            const int needed = (s->format == DEFLATE_FORMAT_ZLIB) ? 4 : 8;
            while (*avail_in > 0 && s->trl_pos_ < needed) {
                s->trl_buf_[s->trl_pos_++] = **next_in;
                ++(*next_in);
                --(*avail_in);
            }
            if (s->trl_pos_ < needed) return DEFLATE_OK;  /* need more trailer bytes */

            /* Verify checksum */
            if (s->format == DEFLATE_FORMAT_ZLIB) {
                const uint32_t expected =
                    (static_cast<uint32_t>(s->trl_buf_[0]) << 24) |
                    (static_cast<uint32_t>(s->trl_buf_[1]) << 16) |
                    (static_cast<uint32_t>(s->trl_buf_[2]) <<  8) |
                     static_cast<uint32_t>(s->trl_buf_[3]);
                if (s->checksum_ != expected) return DEFLATE_DATA_ERROR;
            } else {  /* GZIP */
                const uint32_t exp_crc =
                     static_cast<uint32_t>(s->trl_buf_[0])        |
                    (static_cast<uint32_t>(s->trl_buf_[1]) <<  8) |
                    (static_cast<uint32_t>(s->trl_buf_[2]) << 16) |
                    (static_cast<uint32_t>(s->trl_buf_[3]) << 24);
                const uint32_t exp_isize =
                     static_cast<uint32_t>(s->trl_buf_[4])        |
                    (static_cast<uint32_t>(s->trl_buf_[5]) <<  8) |
                    (static_cast<uint32_t>(s->trl_buf_[6]) << 16) |
                    (static_cast<uint32_t>(s->trl_buf_[7]) << 24);
                if (s->checksum_ != exp_crc) return DEFLATE_DATA_ERROR;
                if (exp_isize != (s->total_out_ & 0xFFFFFFFF)) return DEFLATE_DATA_ERROR;
            }
            s->phase = deflate_stream::Phase::DONE;
            return DEFLATE_STREAM_END;
        }

        case deflate_stream::Phase::DONE:
            return DEFLATE_STREAM_END;

        default:
            return DEFLATE_DATA_ERROR;
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════
 * Parallel API (unchanged)
 * ════════════════════════════════════════════════════════════════════════ */

deflate_parallel_ctx* deflate_parallel_new(
    int level, deflate_format format, int num_threads, size_t block_size)
{
    auto* ctx = new (std::nothrow) deflate_parallel_ctx;
    if (!ctx) return nullptr;
    ctx->impl = new (std::nothrow) orot::deflate::ParallelCompressor(
        level, format, num_threads, block_size);
    if (!ctx->impl) { delete ctx; return nullptr; }
    return ctx;
}

void deflate_parallel_free(deflate_parallel_ctx* ctx) { delete ctx; }

size_t deflate_parallel_compress(
    deflate_parallel_ctx* ctx,
    const void* in,  size_t in_size,
    void*       out, size_t out_capacity)
{
    if (!ctx) return 0;
    return ctx->impl->compress(
        static_cast<const uint8_t*>(in), in_size,
        static_cast<uint8_t*>(out), out_capacity);
}

} /* extern "C" */
