/*
 * test_codec_compat.cpp — Cross-library compatibility tests for codecs other
 * than DEFLATE.
 *
 * Covered:
 *   LZ4 block/frame     ours <-> liblz4
 *   LZMA alone/LZMA2    ours <-> liblzma
 *   Bzip2 .bz2          ours <-> libbz2
 *   Zstandard frame     ours <-> libzstd
 *
 * LZW is intentionally absent here: orot's public LZW stream has a custom
 * 1-byte max_bits header and there is no single broadly adopted system LZW
 * library/format ABI to cross-link against in the way the other codecs have.
 */

#include <bzlib.h>
#include <lz4.h>
#include <lz4frame.h>
#include <lz4hc.h>
#include <lzma.h>
#include <zstd.h>

#include "orot/bzip2.h"
#include "orot/lz4.h"
#include "orot/lzma.h"
#include "orot/zstd.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

static int failures = 0;
static int passes = 0;
static int xfails = 0;
static int xpasses = 0;

using Bytes = std::vector<uint8_t>;
using CompFn = std::function<int(const uint8_t*, size_t, Bytes&, int)>;
using DecompFn = std::function<int(const uint8_t*, size_t, uint8_t*, size_t, int)>;

struct Dataset {
    const char* name;
    Bytes data;
};

static void fail(const char* label, const char* detail) {
    std::fprintf(stderr, "FAIL: %s (%s)\n", label, detail);
    ++failures;
}

static bool is_expected_failure(
    const char* codec,
    const char* fmt,
    const char* dataset,
    const char* comp_lib,
    const char* decomp_lib)
{
    (void)codec;
    (void)fmt;
    (void)dataset;
    (void)comp_lib;
    (void)decomp_lib;
    return false;
}

static void run_case(
    const char* codec,
    const char* fmt,
    const Dataset& ds,
    int level,
    const char* comp_lib,
    const char* decomp_lib,
    const CompFn& comp,
    const DecompFn& decomp)
{
    char label[256];
    std::snprintf(label, sizeof(label),
                  "%-6s fmt=%-10s compress=%-8s decompress=%-8s ds=%-10s L%d",
                  codec, fmt, comp_lib, decomp_lib, ds.name, level);
    const bool expect_fail = is_expected_failure(codec, fmt, ds.name, comp_lib, decomp_lib);
    const auto record_failure = [&](const char* detail) {
        if (expect_fail) {
            std::printf("XFAIL: %s (%s)\n", label, detail);
            ++xfails;
        } else {
            fail(label, detail);
        }
    };

    Bytes cbuf;
    const int clen = comp(ds.data.data(), ds.data.size(), cbuf, level);
    if (clen <= 0) {
        record_failure("compress failed");
        return;
    }
    cbuf.resize(static_cast<size_t>(clen));

    Bytes dbuf(ds.data.size() + 64);
    const int dlen = decomp(cbuf.data(), cbuf.size(), dbuf.data(), dbuf.size(), level);
    if (dlen < 0) {
        record_failure("decompress failed");
        return;
    }
    if (static_cast<size_t>(dlen) != ds.data.size()) {
        record_failure("decompressed size mismatch");
        return;
    }
    if (!ds.data.empty() && std::memcmp(ds.data.data(), dbuf.data(), ds.data.size()) != 0) {
        record_failure("decompressed bytes mismatch");
        return;
    }

    if (expect_fail) {
        std::fprintf(stderr, "XPASS: %s\n", label);
        ++xpasses;
        return;
    }

    std::printf("PASS: %s\n", label);
    ++passes;
}

static Bytes make_text() {
    Bytes out;
    const char* pat = "Cross-library codec compatibility text payload. ";
    const size_t n = std::strlen(pat);
    for (int i = 0; i < 1400; ++i)
        out.insert(out.end(), reinterpret_cast<const uint8_t*>(pat),
                   reinterpret_cast<const uint8_t*>(pat) + n);
    return out;
}

static Bytes make_zeros(size_t n) {
    return Bytes(n, 0);
}

static Bytes make_random(size_t n) {
    Bytes out(n);
    uint32_t x = 0xC0FFEE01u;
    for (auto& b : out) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<uint8_t>(x);
    }
    return out;
}

static Bytes make_code(size_t n) {
    Bytes out(n);
    const char* pat = "int codec_case(int x) { return (x * 33) ^ 0x5a; }\n";
    const size_t plen = std::strlen(pat);
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<uint8_t>(pat[i % plen]);
    return out;
}

/* LZ4 */

static int orot_lz4_block_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    const int cap = orot_lz4_compress_bound(static_cast<int>(sl));
    out.resize(static_cast<size_t>(cap));
    return orot_lz4_compress(s, static_cast<int>(sl), out.data(), cap, level);
}

static int orot_lz4_block_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    return orot_lz4_decompress(s, static_cast<int>(sl), d, static_cast<int>(dc));
}

static int liblz4_block_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    const int cap = LZ4_compressBound(static_cast<int>(sl));
    out.resize(static_cast<size_t>(cap));
    return LZ4_compress_HC(reinterpret_cast<const char*>(s),
                           reinterpret_cast<char*>(out.data()),
                           static_cast<int>(sl), cap, level);
}

static int liblz4_block_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    return LZ4_decompress_safe(reinterpret_cast<const char*>(s),
                               reinterpret_cast<char*>(d),
                               static_cast<int>(sl), static_cast<int>(dc));
}

static int orot_lz4_frame_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    const int cap = orot_lz4f_compress_bound(static_cast<int>(sl));
    out.resize(static_cast<size_t>(cap));
    return orot_lz4f_compress(s, static_cast<int>(sl), out.data(), cap, level);
}

static int orot_lz4_frame_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    return orot_lz4f_decompress(s, static_cast<int>(sl), d, static_cast<int>(dc));
}

static int liblz4_frame_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    LZ4F_preferences_t prefs{};
    prefs.compressionLevel = level;
    const size_t cap = LZ4F_compressFrameBound(sl, &prefs);
    out.resize(cap);
    const size_t n = LZ4F_compressFrame(out.data(), out.size(), s, sl, &prefs);
    return LZ4F_isError(n) ? -1 : static_cast<int>(n);
}

static int liblz4_frame_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    LZ4F_dctx* ctx = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION)))
        return -1;

    const uint8_t* sp = s;
    uint8_t* dp = d;
    size_t sr = sl;
    size_t dr = dc;
    while (sr > 0) {
        size_t sc = sr;
        size_t dc2 = dr;
        const size_t ret = LZ4F_decompress(ctx, dp, &dc2, sp, &sc, nullptr);
        if (LZ4F_isError(ret)) {
            LZ4F_freeDecompressionContext(ctx);
            return -1;
        }
        sp += sc;
        sr -= sc;
        dp += dc2;
        dr -= dc2;
        if (ret == 0)
            break;
        if (sc == 0 && dc2 == 0) {
            LZ4F_freeDecompressionContext(ctx);
            return -1;
        }
    }

    LZ4F_freeDecompressionContext(ctx);
    return static_cast<int>(dc - dr);
}

/* LZMA */

static bool init_liblzma_options(lzma_options_lzma& opt, int level) {
    std::memset(&opt, 0, sizeof(opt));
    return lzma_lzma_preset(&opt, static_cast<uint32_t>(level)) == LZMA_OK;
}

static int orot_lzma_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    out.resize(orot_lzma_compress_bound(sl));
    return orot_lzma_compress(s, sl, out.data(), out.size(), level);
}

static int orot_lzma_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    return orot_lzma_decompress(s, sl, d, dc, nullptr);
}

static int liblzma_alone_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    lzma_options_lzma opt;
    if (!init_liblzma_options(opt, level))
        return -1;

    out.resize(std::max(lzma_stream_buffer_bound(sl) + 128, sl * 2 + 4096));
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_alone_encoder(&strm, &opt) != LZMA_OK)
        return -1;

    strm.next_in = s;
    strm.avail_in = sl;
    strm.next_out = out.data();
    strm.avail_out = out.size();

    lzma_ret ret = LZMA_OK;
    while (ret == LZMA_OK)
        ret = lzma_code(&strm, LZMA_FINISH);
    const int n = (ret == LZMA_STREAM_END) ? static_cast<int>(strm.total_out) : -1;
    lzma_end(&strm);
    return n;
}

static int liblzma_alone_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&strm, UINT64_MAX) != LZMA_OK)
        return -1;

    strm.next_in = s;
    strm.avail_in = sl;
    strm.next_out = d;
    strm.avail_out = dc;

    lzma_ret ret = LZMA_OK;
    while (ret == LZMA_OK)
        ret = lzma_code(&strm, LZMA_RUN);
    const int n = (ret == LZMA_STREAM_END) ? static_cast<int>(strm.total_out) : -1;
    lzma_end(&strm);
    return n;
}

static int orot_lzma2_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    out.resize(orot_lzma2_compress_bound(sl));
    return orot_lzma2_compress(s, sl, out.data(), out.size(), level);
}

static int orot_lzma2_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    return orot_lzma2_decompress(s, sl, d, dc);
}

static int liblzma_lzma2_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    lzma_options_lzma opt;
    if (!init_liblzma_options(opt, level))
        return -1;

    lzma_filter filters[2];
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[0].options = &opt;
    filters[1].id = LZMA_VLI_UNKNOWN;
    filters[1].options = nullptr;

    out.resize(sl * 2 + 4096);
    size_t out_pos = 0;
    const lzma_ret ret = lzma_raw_buffer_encode(filters, nullptr, s, sl,
                                                out.data(), &out_pos, out.size());
    return (ret == LZMA_OK) ? static_cast<int>(out_pos) : -1;
}

static int liblzma_lzma2_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int level) {
    lzma_options_lzma opt;
    if (!init_liblzma_options(opt, level))
        return -1;

    lzma_filter filters[2];
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[0].options = &opt;
    filters[1].id = LZMA_VLI_UNKNOWN;
    filters[1].options = nullptr;

    size_t in_pos = 0;
    size_t out_pos = 0;
    const lzma_ret ret = lzma_raw_buffer_decode(filters, nullptr, s, &in_pos, sl,
                                                d, &out_pos, dc);
    return (ret == LZMA_OK && in_pos == sl) ? static_cast<int>(out_pos) : -1;
}

/* Bzip2 */

static int orot_bzip2_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    out.resize(orot_bzip2_compress_bound(sl));
    return orot_bzip2_compress(s, sl, out.data(), out.size(), level);
}

static int orot_bzip2_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    return orot_bzip2_decompress(s, sl, d, dc, nullptr);
}

static int libbz2_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    out.resize(sl * 2 + 1024);
    unsigned int n = static_cast<unsigned int>(out.size());
    const int ret = BZ2_bzBuffToBuffCompress(
        reinterpret_cast<char*>(out.data()), &n,
        const_cast<char*>(reinterpret_cast<const char*>(s)),
        static_cast<unsigned int>(sl), level, 0, 30);
    return (ret == BZ_OK) ? static_cast<int>(n) : -1;
}

static int libbz2_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    unsigned int n = static_cast<unsigned int>(dc);
    const int ret = BZ2_bzBuffToBuffDecompress(
        reinterpret_cast<char*>(d), &n,
        const_cast<char*>(reinterpret_cast<const char*>(s)),
        static_cast<unsigned int>(sl), 0, 0);
    return (ret == BZ_OK) ? static_cast<int>(n) : -1;
}

/* Zstandard */

static int orot_zstd_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    const int cap = orot_zstd_compress_bound(static_cast<int>(sl));
    out.resize(static_cast<size_t>(cap));
    return orot_zstd_compress(s, static_cast<int>(sl), out.data(), cap, level);
}

static int orot_zstd_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    return orot_zstd_decompress(s, static_cast<int>(sl), d, static_cast<int>(dc));
}

static int libzstd_comp(const uint8_t* s, size_t sl, Bytes& out, int level) {
    out.resize(ZSTD_compressBound(sl));
    const size_t n = ZSTD_compress(out.data(), out.size(), s, sl, level);
    return ZSTD_isError(n) ? -1 : static_cast<int>(n);
}

static int libzstd_decomp(const uint8_t* s, size_t sl, uint8_t* d, size_t dc, int) {
    const size_t n = ZSTD_decompress(d, dc, s, sl);
    return ZSTD_isError(n) ? -1 : static_cast<int>(n);
}

int main() {
    Dataset datasets[] = {
        {"text", make_text()},
        {"zeros", make_zeros(64 * 1024)},
        {"random", make_random(64 * 1024)},
        {"code", make_code(48 * 1024)},
    };

    const int fast_default_best[] = {1, 5, 9};
    const int deflate_like_levels[] = {1, 6, 9};

    for (const auto& ds : datasets) {
        for (int level : deflate_like_levels) {
            run_case("LZ4", "block", ds, level, "orot", "liblz4",
                     orot_lz4_block_comp, liblz4_block_decomp);
            run_case("LZ4", "block", ds, level, "liblz4", "orot",
                     liblz4_block_comp, orot_lz4_block_decomp);
            run_case("LZ4", "frame", ds, level, "orot", "liblz4",
                     orot_lz4_frame_comp, liblz4_frame_decomp);
            run_case("LZ4", "frame", ds, level, "liblz4", "orot",
                     liblz4_frame_comp, orot_lz4_frame_decomp);
        }

        for (int level : fast_default_best) {
            run_case("LZMA", "alone", ds, level, "orot", "liblzma",
                     orot_lzma_comp, liblzma_alone_decomp);
            run_case("LZMA", "alone", ds, level, "liblzma", "orot",
                     liblzma_alone_comp, orot_lzma_decomp);
            run_case("LZMA2", "raw", ds, level, "orot", "liblzma",
                     orot_lzma2_comp, liblzma_lzma2_decomp);
            run_case("LZMA2", "raw", ds, level, "liblzma", "orot",
                     liblzma_lzma2_comp, orot_lzma2_decomp);
            run_case("BZIP2", "bz2", ds, level, "orot", "libbz2",
                     orot_bzip2_comp, libbz2_decomp);
            run_case("BZIP2", "bz2", ds, level, "libbz2", "orot",
                     libbz2_comp, orot_bzip2_decomp);
        }

        for (int level : deflate_like_levels) {
            run_case("ZSTD", "frame", ds, level, "orot", "libzstd",
                     orot_zstd_comp, libzstd_decomp);
            run_case("ZSTD", "frame", ds, level, "libzstd", "orot",
                     libzstd_comp, orot_zstd_decomp);
        }
    }

    std::printf("\n%d passed, %d expected-failed, %d failed, %d unexpected-passed\n",
                passes, xfails, failures, xpasses);
    return (failures == 0 && xpasses == 0) ? 0 : 1;
}
