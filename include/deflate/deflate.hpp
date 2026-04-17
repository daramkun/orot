#pragma once

#include "deflate.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace deflate {

/* ── Enums mirroring C types ─────────────────────────────────────────────── */

enum class Format : int {
    Raw  = DEFLATE_FORMAT_RAW,
    Zlib = DEFLATE_FORMAT_ZLIB,
    Gzip = DEFLATE_FORMAT_GZIP,
};

struct Level {
    static constexpr int Store   = DEFLATE_LEVEL_STORE;
    static constexpr int Fast    = DEFLATE_LEVEL_FAST;
    static constexpr int Default = DEFLATE_LEVEL_DEFAULT;
    static constexpr int Better  = DEFLATE_LEVEL_BETTER;
    static constexpr int Max     = DEFLATE_LEVEL_MAX;
};

/* ── Exception type ──────────────────────────────────────────────────────── */

class Error : public std::runtime_error {
public:
    explicit Error(deflate_result code, const char* msg)
        : std::runtime_error(msg), code_(code) {}
    deflate_result code() const noexcept { return code_; }
private:
    deflate_result code_;
};

/* ── Whole-buffer API ────────────────────────────────────────────────────── */

/**
 * Compress input, return compressed bytes.
 * Throws deflate::Error on failure.
 */
inline std::vector<uint8_t> compress(
    std::span<const uint8_t> input,
    int    level  = Level::Default,
    Format format = Format::Zlib)
{
    const size_t bound = deflate_compress_bound(
        input.size(), static_cast<deflate_format>(format));
    std::vector<uint8_t> out(bound);
    const size_t n = deflate_compress(
        input.data(), input.size(),
        out.data(),   out.size(),
        level, static_cast<deflate_format>(format));
    if (n == 0)
        throw Error(DEFLATE_BUF_ERROR, "deflate::compress: output buffer overflow");
    out.resize(n);
    return out;
}

/**
 * Decompress input, return decompressed bytes.
 * max_output guards against decompression bombs (default: 256 MB).
 * Throws deflate::Error on failure.
 */
inline std::vector<uint8_t> decompress(
    std::span<const uint8_t> input,
    Format format     = Format::Zlib,
    size_t max_output = 256ULL * 1024 * 1024)
{
    std::vector<uint8_t> out(max_output);
    size_t actual = 0;
    const deflate_result r = deflate_decompress(
        input.data(), input.size(),
        out.data(),   out.size(),
        &actual, static_cast<deflate_format>(format));
    if (r != DEFLATE_OK)
        throw Error(r, "deflate::decompress failed");
    out.resize(actual);
    return out;
}

/* ── RAII Streaming Compressor ───────────────────────────────────────────── */

class Compressor {
public:
    explicit Compressor(int level = Level::Default, Format fmt = Format::Zlib)
        : s_(deflate_stream_new(level, static_cast<deflate_format>(fmt)))
    {
        if (!s_) throw Error(DEFLATE_MEM_ERROR, "Compressor: allocation failed");
    }
    ~Compressor() { deflate_stream_free(s_); }

    Compressor(const Compressor&)            = delete;
    Compressor& operator=(const Compressor&) = delete;

    /**
     * Feed input bytes, write compressed bytes to out.
     * Returns number of bytes written to out.
     * Call finish() to emit the final block.
     */
    size_t feed(std::span<const uint8_t> in, std::span<uint8_t> out) {
        const uint8_t* next_in  = in.data();
        size_t         avail_in = in.size();
        uint8_t*       next_out = out.data();
        size_t         avail_out = out.size();
        deflate_result r = deflate_stream_compress(
            s_, &next_in, &avail_in, &next_out, &avail_out, DEFLATE_NO_FLUSH);
        if (r < 0) throw Error(r, "Compressor::feed failed");
        return out.size() - avail_out;
    }

    /** Flush and finalize. Returns bytes written to out. */
    size_t finish(std::span<uint8_t> out) {
        const uint8_t* next_in  = nullptr;
        size_t         avail_in = 0;
        uint8_t*       next_out = out.data();
        size_t         avail_out = out.size();
        deflate_result r = deflate_stream_compress(
            s_, &next_in, &avail_in, &next_out, &avail_out, DEFLATE_FINISH);
        if (r != DEFLATE_OK && r != DEFLATE_STREAM_END)
            throw Error(r, "Compressor::finish failed");
        return out.size() - avail_out;
    }

private:
    deflate_stream* s_;
};

/* ── RAII Streaming Decompressor ─────────────────────────────────────────── */

class Decompressor {
public:
    explicit Decompressor(Format fmt = Format::Zlib)
        : s_(inflate_stream_new(static_cast<deflate_format>(fmt)))
    {
        if (!s_) throw Error(DEFLATE_MEM_ERROR, "Decompressor: allocation failed");
    }
    ~Decompressor() { inflate_stream_free(s_); }

    Decompressor(const Decompressor&)            = delete;
    Decompressor& operator=(const Decompressor&) = delete;

    /**
     * Feed compressed input, write decompressed output.
     * Returns bytes written. Returns 0 and sets done=true at stream end.
     */
    size_t feed(std::span<const uint8_t> in, std::span<uint8_t> out,
                bool& done) {
        const uint8_t* next_in   = in.data();
        size_t         avail_in  = in.size();
        uint8_t*       next_out  = out.data();
        size_t         avail_out = out.size();
        deflate_result r = deflate_stream_decompress(
            s_, &next_in, &avail_in, &next_out, &avail_out);
        if (r == DEFLATE_STREAM_END) { done = true; }
        else if (r < 0) throw Error(r, "Decompressor::feed failed");
        else            { done = false; }
        return out.size() - avail_out;
    }

private:
    deflate_stream* s_;
};

/* ── Parallel Compressor ─────────────────────────────────────────────────── */

class ParallelCompressor {
public:
    explicit ParallelCompressor(
        int    level      = Level::Default,
        Format fmt        = Format::Gzip,
        int    threads    = 0,
        size_t block_size = 0)
        : ctx_(deflate_parallel_new(
            level, static_cast<deflate_format>(fmt), threads, block_size))
    {
        if (!ctx_) throw Error(DEFLATE_MEM_ERROR,
            "ParallelCompressor: allocation failed");
    }
    ~ParallelCompressor() { deflate_parallel_free(ctx_); }

    ParallelCompressor(const ParallelCompressor&)            = delete;
    ParallelCompressor& operator=(const ParallelCompressor&) = delete;

    std::vector<uint8_t> compress(std::span<const uint8_t> input) {
        const size_t bound = deflate_compress_bound(
            input.size(),
            static_cast<deflate_format>(Format::Gzip));
        std::vector<uint8_t> out(bound);
        const size_t n = deflate_parallel_compress(
            ctx_, input.data(), input.size(), out.data(), out.size());
        if (n == 0) throw Error(DEFLATE_BUF_ERROR,
            "ParallelCompressor::compress: buffer overflow");
        out.resize(n);
        return out;
    }

private:
    deflate_parallel_ctx* ctx_;
};

/* ── Checksum helpers ────────────────────────────────────────────────────── */

inline uint32_t adler32(std::span<const uint8_t> data, uint32_t init = 1) {
    return deflate_adler32(init, data.data(), data.size());
}

inline uint32_t crc32(std::span<const uint8_t> data, uint32_t init = 0) {
    return deflate_crc32(init, data.data(), data.size());
}

} /* namespace deflate */
