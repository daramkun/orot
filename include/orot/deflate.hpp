#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Types & error codes
 * ========================================================================= */

/* ── Error codes ─────────────────────────────────────────────────────────── */
typedef enum deflate_result {
    DEFLATE_OK            =  0,  /* success                                 */
    DEFLATE_STREAM_END    =  1,  /* end of stream reached                   */
    DEFLATE_NEED_INPUT    =  2,  /* need more input data                    */
    DEFLATE_NEED_OUTPUT   =  3,  /* need more output space                  */
    DEFLATE_DATA_ERROR    = -1,  /* invalid/corrupt input data              */
    DEFLATE_MEM_ERROR     = -2,  /* memory allocation failure               */
    DEFLATE_BUF_ERROR     = -3,  /* output buffer too small (whole-buffer)  */
    DEFLATE_VERSION_ERROR = -4,  /* version mismatch                        */
    DEFLATE_PARAM_ERROR   = -5,  /* invalid parameter                       */
} deflate_result;

/* ── Wire format ─────────────────────────────────────────────────────────── */
typedef enum deflate_format {
    DEFLATE_FORMAT_RAW  = 0,  /* RFC 1951: raw DEFLATE bitstream           */
    DEFLATE_FORMAT_ZLIB = 1,  /* RFC 1950: zlib framing (Adler-32)         */
    DEFLATE_FORMAT_GZIP = 2,  /* RFC 1952: gzip framing (CRC-32)           */
} deflate_format;

/* ── Compression levels ──────────────────────────────────────────────────── */
#define DEFLATE_LEVEL_STORE   0   /* store only, no compression             */
#define DEFLATE_LEVEL_FAST    1   /* fastest, lowest ratio                  */
#define DEFLATE_LEVEL_DEFAULT 6   /* good balance                           */
#define DEFLATE_LEVEL_BETTER  9   /* better ratio, slower                   */
#define DEFLATE_LEVEL_MAX    12   /* maximum ratio, slowest                 */

/* ── Flush modes ─────────────────────────────────────────────────────────── */
typedef enum deflate_flush {
    DEFLATE_NO_FLUSH   = 0,  /* accumulate data, no forced output          */
    DEFLATE_SYNC_FLUSH = 1,  /* flush to byte boundary (Z_SYNC_FLUSH)      */
    DEFLATE_FULL_FLUSH = 2,  /* flush + reset LZ77 dict (random access)    */
    DEFLATE_FINISH     = 3,  /* finalize stream                            */
} deflate_flush;

/* ── Custom allocator ────────────────────────────────────────────────────── */
typedef struct deflate_allocator {
    void* (*alloc)(void* opaque, size_t size, size_t alignment);
    void  (*free )(void* opaque, void*  ptr);
    void* opaque;
} deflate_allocator;

/* =========================================================================
 * C API
 * ========================================================================= */

/* ── Custom allocator (global, set before first use) ─────────────────────── */
void deflate_set_allocator(const deflate_allocator* alloc);

/* ── Whole-buffer API ────────────────────────────────────────────────────── */

/** Returns upper bound on compressed output size (safe to allocate). */
size_t deflate_compress_bound(size_t in_size, deflate_format format);

/**
 * Compress entire buffer in one call.
 * Returns compressed size, or 0 if out_capacity < deflate_compress_bound().
 */
size_t deflate_compress(
    const void* in,  size_t in_size,
    void*       out, size_t out_capacity,
    int         level,
    deflate_format format
);

/**
 * Decompress entire buffer in one call.
 * actual_out_size set to decompressed size on DEFLATE_OK.
 */
deflate_result deflate_decompress(
    const void* in,  size_t in_size,
    void*       out, size_t out_capacity,
    size_t*     actual_out_size,
    deflate_format format
);

/* ── Streaming API ───────────────────────────────────────────────────────── */

typedef struct deflate_stream deflate_stream;

/** Create streaming compressor. Returns NULL on allocation failure. */
deflate_stream* deflate_stream_new(int level, deflate_format format);
void            deflate_stream_free(deflate_stream* s);

/**
 * Feed input, consume output. Call repeatedly until all input consumed.
 * On DEFLATE_FINISH flush: returns DEFLATE_STREAM_END when done.
 * next_in / avail_in / next_out / avail_out updated in place.
 */
deflate_result deflate_stream_compress(
    deflate_stream*  s,
    const uint8_t**  next_in,  size_t* avail_in,
    uint8_t**        next_out, size_t* avail_out,
    deflate_flush    flush
);

/** Create streaming decompressor. Returns NULL on allocation failure. */
deflate_stream* inflate_stream_new(deflate_format format);
void            inflate_stream_free(deflate_stream* s);

deflate_result inflate_stream_decompress(
    deflate_stream*  s,
    const uint8_t**  next_in,  size_t* avail_in,
    uint8_t**        next_out, size_t* avail_out
);

/* ── Parallel API ────────────────────────────────────────────────────────── */

typedef struct deflate_parallel_ctx deflate_parallel_ctx;

/**
 * Create parallel compressor context.
 * num_threads: 0 = auto-detect (hardware_concurrency)
 * block_size:  0 = auto (128KB..1MB based on level)
 */
deflate_parallel_ctx* deflate_parallel_new(
    int    level,
    deflate_format format,
    int    num_threads,
    size_t block_size
);
void deflate_parallel_free(deflate_parallel_ctx* ctx);

/**
 * Compress entire buffer using multiple threads.
 * Returns compressed size, or 0 on failure.
 */
size_t deflate_parallel_compress(
    deflate_parallel_ctx* ctx,
    const void* in,  size_t in_size,
    void*       out, size_t out_capacity
);

/* ── Checksum utilities ──────────────────────────────────────────────────── */

uint32_t deflate_adler32(uint32_t initial, const void* data, size_t len);
uint32_t deflate_crc32  (uint32_t initial, const void* data, size_t len);

/* ── zlib-compatible aliases ─────────────────────────────────────────────── */

#ifdef DEFLATE_ZLIB_COMPAT

#define Z_OK            DEFLATE_OK
#define Z_STREAM_END    DEFLATE_STREAM_END
#define Z_DATA_ERROR    DEFLATE_DATA_ERROR
#define Z_MEM_ERROR     DEFLATE_MEM_ERROR
#define Z_BUF_ERROR     DEFLATE_BUF_ERROR
#define Z_NO_FLUSH      DEFLATE_NO_FLUSH
#define Z_SYNC_FLUSH    DEFLATE_SYNC_FLUSH
#define Z_FINISH        DEFLATE_FINISH
#define Z_BEST_SPEED    DEFLATE_LEVEL_FAST
#define Z_BEST_COMPRESSION DEFLATE_LEVEL_BETTER
#define Z_DEFAULT_COMPRESSION DEFLATE_LEVEL_DEFAULT

#endif /* DEFLATE_ZLIB_COMPAT */

#ifdef __cplusplus
} /* extern "C" */
#endif

/* =========================================================================
 * C++ API
 * ========================================================================= */

#ifdef __cplusplus

#include <span>
#include <stdexcept>
#include <vector>

namespace orot { namespace deflate {

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
        deflate_result r = inflate_stream_decompress(
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

} } /* namespace orot::deflate */

#endif /* __cplusplus */
