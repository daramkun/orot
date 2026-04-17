/*
 * test_streaming.cpp — Streaming (incremental) compression API tests.
 * Verifies deflate_stream_compress / deflate_stream_decompress work correctly
 * when data is fed in small chunks.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include "deflate/deflate.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s: %s\n", msg, #cond); \
        ++failures; \
    } \
} while (0)

/*
 * Compress using streaming API with a given chunk size.
 * Returns compressed bytes or empty on error.
 */
static std::vector<uint8_t> stream_compress(
    const uint8_t* data, size_t len,
    int level, deflate_format fmt,
    size_t in_chunk, size_t out_chunk)
{
    deflate_stream* s = deflate_stream_new(level, fmt);
    if (!s) return {};

    std::vector<uint8_t> out;
    out.reserve(len + 256);

    size_t offset = 0;
    bool   done   = false;

    while (!done) {
        const size_t avail_in = std::min(in_chunk, len - offset);
        deflate_flush flush    = (offset + avail_in >= len)
                                 ? DEFLATE_FINISH : DEFLATE_NO_FLUSH;

        const uint8_t* next_in = data + offset;
        size_t         rem_in  = avail_in;

        while (true) {
            uint8_t     tmp[512];
            size_t      avail_out = std::min(out_chunk, sizeof(tmp));
            uint8_t*    next_out  = tmp;
            size_t      rem_out   = avail_out;

            deflate_result r = deflate_stream_compress(
                s, &next_in, &rem_in, &next_out, &rem_out, flush);

            const size_t produced = avail_out - rem_out;
            out.insert(out.end(), tmp, tmp + produced);

            if (r == DEFLATE_STREAM_END) { done = true; break; }
            if (r == DEFLATE_NEED_OUTPUT) continue;
            if (r == DEFLATE_OK && rem_in == 0) break;
            if (r < 0) { deflate_stream_free(s); return {}; }
        }

        offset += avail_in;
        if (offset >= len && flush == DEFLATE_FINISH) break;
    }

    deflate_stream_free(s);
    return out;
}

/*
 * Decompress using streaming API with a given chunk size.
 */
static std::vector<uint8_t> stream_decompress(
    const uint8_t* comp, size_t comp_len,
    deflate_format fmt,
    size_t in_chunk, size_t out_chunk,
    size_t max_out)
{
    deflate_stream* s = inflate_stream_new(fmt);
    if (!s) return {};

    std::vector<uint8_t> out;
    out.reserve(max_out);

    size_t offset = 0;

    while (offset < comp_len) {
        const size_t avail_in  = std::min(in_chunk, comp_len - offset);
        const uint8_t* next_in = comp + offset;
        size_t         rem_in  = avail_in;

        bool chunk_done = false;
        while (!chunk_done) {
            uint8_t  tmp[512];
            size_t   avail_out = std::min(out_chunk, sizeof(tmp));
            uint8_t* next_out  = tmp;
            size_t   rem_out   = avail_out;

            deflate_result r = deflate_stream_decompress(
                s, &next_in, &rem_in, &next_out, &rem_out);

            const size_t produced = avail_out - rem_out;
            out.insert(out.end(), tmp, tmp + produced);

            if (r == DEFLATE_STREAM_END) goto done;
            if (r == DEFLATE_NEED_OUTPUT) continue;   /* output full, drain more */
            if (r < 0) { inflate_stream_free(s); return {}; }
            if (rem_in == 0) chunk_done = true;       /* all input for this chunk consumed */
        }

        offset += avail_in;
    }

    /* After all compressed input is fed, drain any remaining output that
     * the decompressor can still produce from its internal bit buffer. */
    {
        const uint8_t* empty_in  = nullptr;
        size_t         empty_rem = 0;
        for (;;) {
            uint8_t  tmp[512];
            size_t   avail_out = std::min(out_chunk, sizeof(tmp));
            uint8_t* next_out  = tmp;
            size_t   rem_out   = avail_out;
            deflate_result r = deflate_stream_decompress(
                s, &empty_in, &empty_rem, &next_out, &rem_out);
            const size_t produced = avail_out - rem_out;
            out.insert(out.end(), tmp, tmp + produced);
            if (r == DEFLATE_STREAM_END || r < 0) break;
            if (r == DEFLATE_OK && produced == 0) break;
        }
    }

done:
    inflate_stream_free(s);
    return out;
}

static void test_streaming(
    const uint8_t* data, size_t len,
    int level, deflate_format fmt,
    size_t in_chunk, size_t out_chunk,
    const char* label)
{
    auto comp = stream_compress(data, len, level, fmt, in_chunk, out_chunk);
    CHECK(!comp.empty() || len == 0, label);
    if (comp.empty() && len == 0) {
        std::printf("PASS: %s (empty)\n", label);
        return;
    }

    auto decomp = stream_decompress(
        comp.data(), comp.size(), fmt, in_chunk, out_chunk, len + 64);

    CHECK(decomp.size() == len, label);
    if (len > 0)
        CHECK(std::memcmp(data, decomp.data(), len) == 0, label);

    if (failures == 0)
        std::printf("PASS: %s  in=%zu comp=%zu\n", label, len, comp.size());
}

int main() {
    /* Build test data */
    std::string text;
    for (int i = 0; i < 500; ++i)
        text += "Streaming deflate test with repetitive content. ";
    const auto* td = reinterpret_cast<const uint8_t*>(text.data());
    const size_t tlen = text.size();

    std::vector<uint8_t> zeros(16384, 0);

    std::vector<uint8_t> rnd(8192);
    {
        uint32_t st = 0xDEADC0DEU;
        for (auto& b : rnd) {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;
            b = static_cast<uint8_t>(st);
        }
    }

    static const deflate_format formats[] = {
        DEFLATE_FORMAT_RAW, DEFLATE_FORMAT_ZLIB, DEFLATE_FORMAT_GZIP
    };
    static const char* fnames[] = { "raw", "zlib", "gzip" };

    /* Test various chunk sizes */
    static const size_t chunks[] = { 1, 7, 64, 512, 4096 };

    for (int fi = 0; fi < 3; ++fi) {
        for (size_t ci = 0; ci < 5; ++ci) {
            char label[128];
            std::snprintf(label, sizeof(label),
                "text/L6/%s/chunk%zu", fnames[fi], chunks[ci]);
            test_streaming(td, tlen, 6, formats[fi],
                           chunks[ci], chunks[ci], label);

            std::snprintf(label, sizeof(label),
                "zeros/L1/%s/chunk%zu", fnames[fi], chunks[ci]);
            test_streaming(zeros.data(), zeros.size(), 1, formats[fi],
                           chunks[ci], chunks[ci], label);

            std::snprintf(label, sizeof(label),
                "random/L6/%s/chunk%zu", fnames[fi], chunks[ci]);
            test_streaming(rnd.data(), rnd.size(), 6, formats[fi],
                           chunks[ci], chunks[ci], label);
        }
    }

    /* Empty input */
    for (int fi = 0; fi < 3; ++fi) {
        char label[128];
        std::snprintf(label, sizeof(label), "empty/%s", fnames[fi]);
        test_streaming(nullptr, 0, 6, formats[fi], 64, 64, label);
    }

    if (failures == 0) {
        std::printf("\nAll streaming tests PASSED.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
    return 1;
}
