#include "parallel_compressor.hpp"
#include "../core/deflate_block.hpp"
#include "../core/lz77.hpp"
#include "../core/bit_writer.hpp"
#include "../compress/level_config.hpp"
#include "../simd/simd_dispatch.hpp"

#include <algorithm>
#include <cstring>
#include <vector>
#include <memory>

namespace deflate {

/* Choose default block size based on compression level */
static size_t default_block_size(int level) {
    if (level <= 3) return 131072;   /* 128 KB */
    if (level <= 6) return 524288;   /* 512 KB */
    return 1048576;                  /* 1 MB   */
}

ParallelCompressor::ParallelCompressor(
    int level, deflate_format format, int num_threads, size_t block_size)
    : level_(level)
    , format_(format)
    , block_size_(block_size > 0 ? block_size : default_block_size(level))
    , pool_(num_threads)
{}

ParallelCompressor::~ParallelCompressor() = default;

int ParallelCompressor::thread_count() const noexcept {
    return pool_.thread_count();
}

size_t ParallelCompressor::compress(
    const uint8_t* src, size_t src_len,
    uint8_t*       dst, size_t dst_capacity)
{
    if (src_len == 0) {
        /* Produce a valid empty stream */
        const size_t need = deflate_compress_bound_internal(0);
        if (dst_capacity < need) return 0;
        /* Use single-block path below by falling through with n_blocks=1 */
    }

    const CompressConfig cfg = compress_config_for_level(level_);

    /* Split into blocks */
    const size_t n_blocks  = src_len > 0
        ? (src_len + block_size_ - 1) / block_size_
        : 1;

    /* ── Phase 1: parallel LZ77 + block stats ───────────────────────────── */
    struct BlockInfo {
        std::unique_ptr<LZ77State>  state;
        std::vector<Token>          tokens;
        size_t                      n_tokens = 0;
        BlockStats                  stats;
        const uint8_t*              src      = nullptr;
        size_t                      src_len  = 0;
    };
    std::vector<BlockInfo> blocks(n_blocks);

    for (size_t i = 0; i < n_blocks; ++i) {
        const size_t off  = i * block_size_;
        const size_t blen = src_len > 0
            ? std::min(block_size_, src_len - off)
            : 0;
        blocks[i].src     = src + off;
        blocks[i].src_len = blen;
        blocks[i].tokens.resize(blen > 0 ? blen : 1);
        blocks[i].state.reset(new LZ77State{});
        blocks[i].state->reset(cfg.lz77.hash_bits, cfg.lz77.bt4);

        pool_.submit([&, i] {
            auto& b = blocks[i];
            b.n_tokens = lz77_compress(
                b.src, b.src_len, b.tokens.data(),
                *b.state, cfg.lz77);
            compute_block_stats(b.tokens.data(), b.n_tokens, b.stats);
        });
    }
    pool_.wait_all();

    /* ── Phase 2: write format header ───────────────────────────────────── */
    uint8_t* p = dst;
    size_t   written = 0;

    /* Header */
    if (format_ == DEFLATE_FORMAT_ZLIB) {
        if (dst_capacity < 6) return 0;
        p[0] = 0x78; p[1] = 0x9C;
        p += 2; written += 2;
    } else if (format_ == DEFLATE_FORMAT_GZIP) {
        static const uint8_t hdr[10] = {
            0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 255
        };
        if (dst_capacity < sizeof(hdr)) return 0;
        std::memcpy(p, hdr, sizeof(hdr));
        p += sizeof(hdr); written += sizeof(hdr);
    }

    /* Reserve trailer space */
    const size_t trailer_size = (format_ == DEFLATE_FORMAT_ZLIB) ? 4
                              : (format_ == DEFLATE_FORMAT_GZIP)  ? 8
                              : 0;
    if (written + trailer_size > dst_capacity) return 0;
    const size_t raw_capacity = dst_capacity - written - trailer_size;

    /* ── Phase 3: sequential DEFLATE encoding into single BitWriter ──────── */
    BitWriter bw(p, raw_capacity);

    for (size_t i = 0; i < n_blocks; ++i) {
        const bool last = (i == n_blocks - 1);
        auto& b = blocks[i];
        encode_block(b.tokens.data(), b.n_tokens,
                     b.src, b.src_len,
                     bw, last, cfg.block_hint);
    }
    /* encode_block with is_last=true already flushed bw to byte boundary */

    const size_t raw_size = bw.bytes_written();
    p       += raw_size;
    written += raw_size;

    /* ── Phase 4: write format trailer ──────────────────────────────────── */
    if (format_ == DEFLATE_FORMAT_ZLIB) {
        const uint32_t adler = simd_adler32_fn()(1, src, src_len);
        *p++ = static_cast<uint8_t>(adler >> 24);
        *p++ = static_cast<uint8_t>(adler >> 16);
        *p++ = static_cast<uint8_t>(adler >>  8);
        *p++ = static_cast<uint8_t>(adler);
        written += 4;
    } else if (format_ == DEFLATE_FORMAT_GZIP) {
        const uint32_t crc   = simd_crc32_fn()(0, src, src_len);
        const uint32_t isize = static_cast<uint32_t>(src_len);
        *p++ = static_cast<uint8_t>(crc);
        *p++ = static_cast<uint8_t>(crc >>  8);
        *p++ = static_cast<uint8_t>(crc >> 16);
        *p++ = static_cast<uint8_t>(crc >> 24);
        *p++ = static_cast<uint8_t>(isize);
        *p++ = static_cast<uint8_t>(isize >>  8);
        *p++ = static_cast<uint8_t>(isize >> 16);
        *p++ = static_cast<uint8_t>(isize >> 24);
        written += 8;
    }

    return written;
}

/* Internal helper used above (forward declaration not needed — defined here) */
size_t ParallelCompressor::deflate_compress_bound_internal(size_t n) const {
    /* raw bound + worst-case overhead for format framing */
    const size_t raw = n + (n >> 12) + (n >> 14) + (n >> 25) + 13;
    if (format_ == DEFLATE_FORMAT_ZLIB) return raw + 6;
    if (format_ == DEFLATE_FORMAT_GZIP) return raw + 18;
    return raw;
}

} /* namespace deflate */
