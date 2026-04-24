/*
 * test_compat.cpp — Cross-library compression compatibility tests.
 *
 * Verifies that compressed data produced by one library can be
 * correctly decompressed by any other library, covering all
 * combinations of:
 *   Libraries : ours / zlib / libdeflate
 *   Formats   : ZLIB (RFC 1950), GZIP (RFC 1952)
 *   RAW       : ours ↔ libdeflate only (zlib's compress2 uses zlib-format)
 *   Levels    : 1 (fast), 6 (default), 9 (best)
 *   Datasets  : text, zeros, random
 */

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

/* ── Library headers ─────────────────────────────────────────────────── */
#include "orot/deflate.h"
#include <zlib.h>
#include <libdeflate.h>

static int failures = 0;
static int passes   = 0;

#define CHECK(cond, label) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s\n", (label)); \
        ++failures; \
    } else { \
        ++passes; \
    } \
} while (0)

/* ══════════════════════════════════════════════════════════════════════
 * Compression wrappers (return compressed size, 0 = error)
 * ══════════════════════════════════════════════════════════════════════ */

/* ── ours ────────────────────────────────────────────────────────────── */
static size_t ours_comp_zlib(const uint8_t* src, size_t slen, std::vector<uint8_t>& out, int lvl) {
    size_t bound = deflate_compress_bound(slen, DEFLATE_FORMAT_ZLIB);
    out.resize(bound);
    size_t n = deflate_compress(src, slen, out.data(), bound, lvl, DEFLATE_FORMAT_ZLIB);
    out.resize(n);
    return n;
}
static size_t ours_comp_gzip(const uint8_t* src, size_t slen, std::vector<uint8_t>& out, int lvl) {
    size_t bound = deflate_compress_bound(slen, DEFLATE_FORMAT_GZIP);
    out.resize(bound);
    size_t n = deflate_compress(src, slen, out.data(), bound, lvl, DEFLATE_FORMAT_GZIP);
    out.resize(n);
    return n;
}
static size_t ours_comp_raw(const uint8_t* src, size_t slen, std::vector<uint8_t>& out, int lvl) {
    size_t bound = deflate_compress_bound(slen, DEFLATE_FORMAT_RAW);
    out.resize(bound);
    size_t n = deflate_compress(src, slen, out.data(), bound, lvl, DEFLATE_FORMAT_RAW);
    out.resize(n);
    return n;
}

/* ── ours decompress ─────────────────────────────────────────────────── */
static size_t ours_decomp_zlib(const uint8_t* src, size_t slen, uint8_t* dst, size_t dcap) {
    size_t actual = 0;
    return (deflate_decompress(src, slen, dst, dcap, &actual, DEFLATE_FORMAT_ZLIB) == DEFLATE_OK)
           ? actual : 0;
}
static size_t ours_decomp_gzip(const uint8_t* src, size_t slen, uint8_t* dst, size_t dcap) {
    size_t actual = 0;
    return (deflate_decompress(src, slen, dst, dcap, &actual, DEFLATE_FORMAT_GZIP) == DEFLATE_OK)
           ? actual : 0;
}
static size_t ours_decomp_raw(const uint8_t* src, size_t slen, uint8_t* dst, size_t dcap) {
    size_t actual = 0;
    return (deflate_decompress(src, slen, dst, dcap, &actual, DEFLATE_FORMAT_RAW) == DEFLATE_OK)
           ? actual : 0;
}

/* ── zlib ────────────────────────────────────────────────────────────── */
static size_t zlib_comp_zlib(const uint8_t* src, size_t slen, std::vector<uint8_t>& out, int lvl) {
    uLongf bound = compressBound(static_cast<uLong>(slen));
    out.resize(bound);
    if (compress2(out.data(), &bound, src, static_cast<uLong>(slen), lvl) != Z_OK) {
        out.clear(); return 0;
    }
    out.resize(bound);
    return bound;
}
static size_t zlib_comp_gzip(const uint8_t* src, size_t slen, std::vector<uint8_t>& out, int lvl) {
    /* Use gzip via deflateInit2 */
    z_stream zs{};
    if (deflateInit2(&zs, lvl, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 0;
    uLong bound = deflateBound(&zs, static_cast<uLong>(slen));
    out.resize(bound);
    zs.next_in  = const_cast<Bytef*>(src);
    zs.avail_in = static_cast<uInt>(slen);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(bound);
    int r = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (r != Z_STREAM_END) { out.clear(); return 0; }
    size_t n = bound - zs.avail_out;
    out.resize(n);
    return n;
}
static size_t zlib_decomp_zlib(const uint8_t* src, size_t slen, uint8_t* dst, size_t dcap) {
    uLongf dl = static_cast<uLongf>(dcap);
    return (uncompress(dst, &dl, src, static_cast<uLong>(slen)) == Z_OK) ? dl : 0;
}
static size_t zlib_decomp_gzip(const uint8_t* src, size_t slen, uint8_t* dst, size_t dcap) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 | 16) != Z_OK) return 0;
    zs.next_in   = const_cast<Bytef*>(src);
    zs.avail_in  = static_cast<uInt>(slen);
    zs.next_out  = dst;
    zs.avail_out = static_cast<uInt>(dcap);
    int r = inflate(&zs, Z_FINISH);
    size_t n = dcap - zs.avail_out;
    inflateEnd(&zs);
    return (r == Z_STREAM_END) ? n : 0;
}

/* ── libdeflate ───────────────────────────────────────────────────────── */
static size_t ldf_comp_zlib(const uint8_t* src, size_t slen, std::vector<uint8_t>& out, int lvl) {
    auto* c = libdeflate_alloc_compressor(lvl);
    if (!c) return 0;
    size_t bound = libdeflate_zlib_compress_bound(c, slen);
    out.resize(bound);
    size_t n = libdeflate_zlib_compress(c, src, slen, out.data(), bound);
    libdeflate_free_compressor(c);
    out.resize(n);
    return n;
}
static size_t ldf_comp_gzip(const uint8_t* src, size_t slen, std::vector<uint8_t>& out, int lvl) {
    auto* c = libdeflate_alloc_compressor(lvl);
    if (!c) return 0;
    size_t bound = libdeflate_gzip_compress_bound(c, slen);
    out.resize(bound);
    size_t n = libdeflate_gzip_compress(c, src, slen, out.data(), bound);
    libdeflate_free_compressor(c);
    out.resize(n);
    return n;
}
static size_t ldf_comp_raw(const uint8_t* src, size_t slen, std::vector<uint8_t>& out, int lvl) {
    auto* c = libdeflate_alloc_compressor(lvl);
    if (!c) return 0;
    size_t bound = libdeflate_deflate_compress_bound(c, slen);
    out.resize(bound);
    size_t n = libdeflate_deflate_compress(c, src, slen, out.data(), bound);
    libdeflate_free_compressor(c);
    out.resize(n);
    return n;
}
static size_t ldf_decomp_zlib(const uint8_t* src, size_t slen, uint8_t* dst, size_t dcap) {
    auto* d = libdeflate_alloc_decompressor();
    if (!d) return 0;
    size_t actual = 0;
    auto r = libdeflate_zlib_decompress(d, src, slen, dst, dcap, &actual);
    libdeflate_free_decompressor(d);
    return (r == LIBDEFLATE_SUCCESS) ? actual : 0;
}
static size_t ldf_decomp_gzip(const uint8_t* src, size_t slen, uint8_t* dst, size_t dcap) {
    auto* d = libdeflate_alloc_decompressor();
    if (!d) return 0;
    size_t actual = 0;
    auto r = libdeflate_gzip_decompress(d, src, slen, dst, dcap, &actual);
    libdeflate_free_decompressor(d);
    return (r == LIBDEFLATE_SUCCESS) ? actual : 0;
}
static size_t ldf_decomp_raw(const uint8_t* src, size_t slen, uint8_t* dst, size_t dcap) {
    auto* d = libdeflate_alloc_decompressor();
    if (!d) return 0;
    size_t actual = 0;
    auto r = libdeflate_deflate_decompress(d, src, slen, dst, dcap, &actual);
    libdeflate_free_decompressor(d);
    return (r == LIBDEFLATE_SUCCESS) ? actual : 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * Test runner
 * ══════════════════════════════════════════════════════════════════════ */

using CompFn   = std::function<size_t(const uint8_t*, size_t, std::vector<uint8_t>&, int)>;
using DecompFn = std::function<size_t(const uint8_t*, size_t, uint8_t*, size_t)>;

static void run_compat_test(
    const char*    ds_name,
    const uint8_t* src, size_t slen,
    int            level,
    const char*    comp_lib,
    const char*    decomp_lib,
    const char*    fmt_name,
    CompFn         comp_fn,
    DecompFn       decomp_fn)
{
    char label[256];
    std::snprintf(label, sizeof(label),
        "compress=%-10s decompress=%-10s fmt=%-5s ds=%-12s L%d",
        comp_lib, decomp_lib, fmt_name, ds_name, level);

    std::vector<uint8_t> comp_buf;
    size_t clen = comp_fn(src, slen, comp_buf, level);
    if (clen == 0) {
        std::fprintf(stderr, "FAIL (compress=0): %s\n", label);
        ++failures;
        return;
    }

    std::vector<uint8_t> decomp_buf(slen + 64);
    size_t dlen = decomp_fn(comp_buf.data(), clen, decomp_buf.data(), decomp_buf.size());

    bool size_ok = (dlen == slen);
    bool data_ok = size_ok && (slen == 0 || std::memcmp(src, decomp_buf.data(), slen) == 0);

    if (size_ok && data_ok) {
        std::printf("PASS: %s\n", label);
        ++passes;
    } else {
        std::fprintf(stderr, "FAIL: %s  (got %zu expected %zu)\n", label, dlen, slen);
        ++failures;
    }
}

int main() {
    /* ── Datasets ───────────────────────────────────────────────────── */
    std::string text;
    for (int i = 0; i < 500; ++i)
        text += "Cross-library compatibility test string with repetitive content. ";
    const auto* tp = reinterpret_cast<const uint8_t*>(text.data());

    std::vector<uint8_t> zeros(4096, 0);

    std::vector<uint8_t> rnd(8192);
    { uint32_t s = 0xDEADBEEF;
      for (auto& b : rnd) { s ^= s<<13; s ^= s>>17; s ^= s<<5; b=(uint8_t)s; } }

    struct DS { const uint8_t* p; size_t len; const char* name; };
    DS datasets[] = {
        { tp,           text.size(),  "text"   },
        { zeros.data(), zeros.size(), "zeros"  },
        { rnd.data(),   rnd.size(),   "random" },
    };

    static const int levels[] = { 1, 6, 9 };

    /* ── ZLIB format combinations ───────────────────────────────────── */
    std::printf("\n=== ZLIB format ===\n");
    struct LibPair {
        const char* cname; CompFn   cfn;
        const char* dname; DecompFn dfn;
    };
    LibPair zlib_pairs[] = {
        { "ours",       ours_comp_zlib, "zlib",       zlib_decomp_zlib },
        { "ours",       ours_comp_zlib, "libdeflate",  ldf_decomp_zlib  },
        { "zlib",       zlib_comp_zlib, "ours",        ours_decomp_zlib },
        { "zlib",       zlib_comp_zlib, "libdeflate",  ldf_decomp_zlib  },
        { "libdeflate", ldf_comp_zlib,  "ours",        ours_decomp_zlib },
        { "libdeflate", ldf_comp_zlib,  "zlib",        zlib_decomp_zlib },
    };
    for (const auto& ds : datasets)
        for (int lv : levels)
            for (const auto& p : zlib_pairs)
                run_compat_test(ds.name, ds.p, ds.len, lv,
                                p.cname, p.dname, "zlib",
                                p.cfn, p.dfn);

    /* ── GZIP format combinations ───────────────────────────────────── */
    std::printf("\n=== GZIP format ===\n");
    LibPair gzip_pairs[] = {
        { "ours",       ours_comp_gzip, "zlib",        zlib_decomp_gzip },
        { "ours",       ours_comp_gzip, "libdeflate",  ldf_decomp_gzip  },
        { "zlib",       zlib_comp_gzip, "ours",        ours_decomp_gzip },
        { "zlib",       zlib_comp_gzip, "libdeflate",  ldf_decomp_gzip  },
        { "libdeflate", ldf_comp_gzip,  "ours",        ours_decomp_gzip },
        { "libdeflate", ldf_comp_gzip,  "zlib",        zlib_decomp_gzip },
    };
    for (const auto& ds : datasets)
        for (int lv : levels)
            for (const auto& p : gzip_pairs)
                run_compat_test(ds.name, ds.p, ds.len, lv,
                                p.cname, p.dname, "gzip",
                                p.cfn, p.dfn);

    /* ── RAW format (ours ↔ libdeflate only) ────────────────────────── */
    std::printf("\n=== RAW format (ours <-> libdeflate) ===\n");
    LibPair raw_pairs[] = {
        { "ours",       ours_comp_raw, "libdeflate", ldf_decomp_raw  },
        { "libdeflate", ldf_comp_raw,  "ours",       ours_decomp_raw },
    };
    for (const auto& ds : datasets)
        for (int lv : levels)
            for (const auto& p : raw_pairs)
                run_compat_test(ds.name, ds.p, ds.len, lv,
                                p.cname, p.dname, "raw",
                                p.cfn, p.dfn);

    /* ── Summary ────────────────────────────────────────────────────── */
    std::printf("\n%d passed, %d failed\n", passes, failures);
    return (failures == 0) ? 0 : 1;
}
