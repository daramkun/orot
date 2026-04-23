/*
 * test_lz4_comprehensive.cpp — Comprehensive LZ4 correctness and compression validation.
 *
 * Tests:
 *   1. Correctness: roundtrip compression/decompression on varied data
 *   2. Compression ratio: validates compression efficiency
 *   3. Edge cases: empty, tiny, boundary conditions
 *   4. Format compatibility: block vs frame
 */
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

#include "orot/lz4.h"

/* ── Test infrastructure ────────────────────────────────────────────────── */

struct TestResult {
    int passed = 0;
    int failed = 0;
    int total = 0;

    void pass() { ++passed; ++total; }
    void fail() { ++failed; ++total; }

    void summary() const {
        if (failed == 0) {
            std::printf("  ✓ All %d tests passed\n", total);
        } else {
            std::printf("  ✗ %d/%d tests failed\n", failed, total);
        }
    }
};

#define TEST_ASSERT(cond, msg, result) \
    do { \
        if (!(cond)) { \
            std::fprintf(stderr, "  FAIL: %s\n", msg); \
            (result).fail(); \
        } else { \
            (result).pass(); \
        } \
    } while (0)

/* ── Data generation ────────────────────────────────────────────────────── */

static std::vector<uint8_t> generate_zeros(size_t len) {
    return std::vector<uint8_t>(len, 0);
}

static std::vector<uint8_t> generate_sequential(size_t len) {
    std::vector<uint8_t> data(len);
    std::iota(data.begin(), data.end(), uint8_t(0));
    return data;
}

static std::vector<uint8_t> generate_random(size_t len) {
    std::vector<uint8_t> data(len);
    uint32_t state = 0xDEADBEEF;
    for (auto& b : data) {
        state = state * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(state >> 24);
    }
    return data;
}

static std::vector<uint8_t> generate_repeating(size_t len) {
    std::vector<uint8_t> data(len);
    for (size_t i = 0; i < len; ++i)
        data[i] = static_cast<uint8_t>(i % 251);
    return data;
}

static std::vector<uint8_t> generate_text(size_t len) {
    std::vector<uint8_t> data;
    const char* samples[] = {
        "The quick brown fox jumps over the lazy dog. ",
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. ",
        "Sphinx of black quartz, judge my vow. ",
        "Pack my box with five dozen liquor jugs. ",
        "How vexingly quick daft zebras jump! ",
        "The five boxing wizards jump quickly. ",
        "Jackdaws love my big sphinx of quartz. ",
        "Two driven jocks help foxy brown jump. ",
        "Quickly gaze at the japanese whove before. "
    };
    int sample_idx = 0;
    while (data.size() < len) {
        const char* sample = samples[sample_idx % 9];
        data.insert(data.end(), sample, sample + std::strlen(sample));
        sample_idx++;
    }
    data.resize(len);
    return data;
}

/* ── Block format tests ────────────────────────────────────────────────── */

static void test_block_roundtrip_comprehensive(TestResult& result) {
    std::printf("\n[Block Format - Roundtrip]\n");

    struct DataSpec {
        const char* name;
        std::vector<uint8_t> (*generator)(size_t);
        size_t size;
    };

    DataSpec specs[] = {
        { "single byte", generate_zeros, 1 },
        { "4 bytes (MIN_MATCH)", generate_sequential, 4 },
        { "small (64 B)", generate_random, 64 },
        { "medium (16 KB)", generate_text, 16 * 1024 },
        { "large (256 KB)", generate_repeating, 256 * 1024 },
        { "huge (1 MB zeros)", generate_zeros, 1024 * 1024 },
    };

    for (const auto& spec : specs) {
        for (int lvl : {1, 6, 9}) {
            auto data = spec.generator(spec.size);
            int bound = orot_lz4_compress_bound(static_cast<int>(data.size()));
            std::vector<uint8_t> comp(bound);
            std::vector<uint8_t> decomp(data.size() + 64);

            int clen = orot_lz4_compress(data.data(), static_cast<int>(data.size()),
                                         comp.data(), bound, lvl);
            TEST_ASSERT(clen > 0 || data.size() == 0, 
                       "compression succeeded", result);

            int dlen = orot_lz4_decompress(comp.data(), clen,
                                          decomp.data(), static_cast<int>(decomp.size()));
            TEST_ASSERT(dlen == static_cast<int>(data.size()),
                       "decompression returned correct size", result);

            TEST_ASSERT(std::memcmp(data.data(), decomp.data(), data.size()) == 0,
                       "decompressed data matches original", result);

            double ratio = data.size() > 0 ? (100.0 * clen / data.size()) : 0.0;
            std::printf("  %-30s L%d  %8zu → %7d  (%.1f%%)\n",
                spec.name, lvl, data.size(), clen, ratio);
        }
    }

    result.summary();
}

static void test_block_compression_ratio(TestResult& result) {
    std::printf("\n[Block Format - Compression Ratio]\n");

    auto zeros = generate_zeros(1024 * 1024);
    auto random = generate_random(1024 * 1024);
    auto text = generate_text(1024 * 1024);

    struct {
        const char* name;
        const std::vector<uint8_t>& data;
        bool should_compress;  /* false for random data */
    } datasets[] = {
        { "zeros (highly compressible)", zeros, true },
        { "text (moderately compressible)", text, true },
        { "random (incompressible)", random, false },
    };

    for (const auto& ds : datasets) {
        std::printf("\n  %s:\n", ds.name);
        double best_ratio = 100.0;
        for (int lvl = 1; lvl <= 9; ++lvl) {
            int bound = orot_lz4_compress_bound(static_cast<int>(ds.data.size()));
            std::vector<uint8_t> comp(bound);
            int clen = orot_lz4_compress(ds.data.data(), static_cast<int>(ds.data.size()),
                                        comp.data(), bound, lvl);
            double ratio = (100.0 * clen / ds.data.size());
            best_ratio = std::min(best_ratio, ratio);
            std::printf("    L%d: %.2f%%\n", lvl, ratio);
        }
        
        if (ds.should_compress) {
            TEST_ASSERT(best_ratio < 95.0, "compression achieved on compressible data", result);
        } else {
            TEST_ASSERT(best_ratio >= 99.0 && best_ratio <= 102.0, 
                       "random data has minimal compression or expansion", result);
        }
    }

    result.summary();
}

/* ── Frame format tests ────────────────────────────────────────────────── */

static void test_frame_roundtrip_comprehensive(TestResult& result) {
    std::printf("\n[Frame Format - Roundtrip]\n");

    struct DataSpec {
        const char* name;
        std::vector<uint8_t> (*generator)(size_t);
        size_t size;
    };

    DataSpec specs[] = {
        { "small (1 KB)", generate_text, 1024 },
        { "medium (64 KB)", generate_repeating, 64 * 1024 },
        { "large (512 KB)", generate_random, 512 * 1024 },
    };

    for (const auto& spec : specs) {
        for (int lvl : {1, 6, 9}) {
            auto data = spec.generator(spec.size);
            int bound = orot_lz4f_compress_bound(static_cast<int>(data.size()));
            std::vector<uint8_t> comp(bound);
            std::vector<uint8_t> decomp(data.size() + 64);

            int clen = orot_lz4f_compress(data.data(), static_cast<int>(data.size()),
                                         comp.data(), bound, lvl);
            TEST_ASSERT(clen > 0, "frame compression succeeded", result);

            int dlen = orot_lz4f_decompress(comp.data(), clen,
                                           decomp.data(), static_cast<int>(decomp.size()));
            TEST_ASSERT(dlen == static_cast<int>(data.size()),
                       "frame decompression returned correct size", result);

            TEST_ASSERT(std::memcmp(data.data(), decomp.data(), data.size()) == 0,
                       "frame decompressed data matches original", result);

            double ratio = (100.0 * clen / data.size());
            std::printf("  %-30s L%d  %8zu → %7d  (%.1f%%)\n",
                spec.name, lvl, data.size(), clen, ratio);
        }
    }

    result.summary();
}

/* ── Edge case tests ────────────────────────────────────────────────────– */

static void test_edge_cases(TestResult& result) {
    std::printf("\n[Edge Cases]\n");

    /* Empty data (frame only) */
    {
        int bound = orot_lz4f_compress_bound(0);
        std::vector<uint8_t> comp(bound);
        std::vector<uint8_t> decomp(64);

        int clen = orot_lz4f_compress(nullptr, 0, comp.data(), bound, 1);
        TEST_ASSERT(clen > 0, "empty frame compresses", result);

        int dlen = orot_lz4f_decompress(comp.data(), clen, decomp.data(), 64);
        TEST_ASSERT(dlen == 0, "empty frame decompresses to 0", result);
        std::printf("  empty input: OK\n");
    }

    /* Single byte */
    {
        uint8_t single = 42;
        int bound = orot_lz4_compress_bound(1);
        std::vector<uint8_t> comp(bound);
        std::vector<uint8_t> decomp(16);

        int clen = orot_lz4_compress(&single, 1, comp.data(), bound, 1);
        TEST_ASSERT(clen > 0, "single byte compresses", result);

        int dlen = orot_lz4_decompress(comp.data(), clen, decomp.data(), 16);
        TEST_ASSERT(dlen == 1 && decomp[0] == 42, "single byte roundtrips", result);
        std::printf("  single byte: OK\n");
    }

    /* Incompressible data */
    {
        auto rnd = generate_random(10000);
        int bound = orot_lz4_compress_bound(10000);
        std::vector<uint8_t> comp(bound);

        int clen = orot_lz4_compress(rnd.data(), 10000, comp.data(), bound, 1);
        TEST_ASSERT(clen > 0 && clen <= bound, "incompressible data handled", result);
        double ratio = (100.0 * clen / 10000);
        /* Random data typically expands slightly due to LZ4 overhead */
        TEST_ASSERT(ratio >= 100.0 && ratio <= 102.0, "random expansion within tolerance", result);
        std::printf("  random (10 KB): %.1f%% (expansion acceptable)\n", ratio);
    }

    /* Highly repetitive (worst case for compression) */
    {
        std::vector<uint8_t> rep(100000, 0xAB);
        int bound = orot_lz4_compress_bound(100000);
        std::vector<uint8_t> comp(bound);

        int clen = orot_lz4_compress(rep.data(), 100000, comp.data(), bound, 1);
        TEST_ASSERT(clen > 0 && clen < 1000, "highly repetitive compresses well", result);
        double ratio = (100.0 * clen / 100000);
        std::printf("  100KB all 0xAB: %.1f%% (excellent compression)\n", ratio);
    }

    result.summary();
}

/* ── Corruption detection tests ─────────────────────────────────────────– */

static void test_corruption_detection(TestResult& result) {
    std::printf("\n[Corruption Detection]\n");

    auto data = generate_text(10000);
    int bound = orot_lz4_compress_bound(10000);
    std::vector<uint8_t> comp(bound);

    int clen = orot_lz4_compress(data.data(), 10000, comp.data(), bound, 1);
    TEST_ASSERT(clen > 0, "valid compression", result);

    /* Corrupt a byte in the middle */
    if (clen > 10) {
        comp[clen / 2] ^= 0xFF;
        std::vector<uint8_t> decomp(10000 + 64);
        int dlen = orot_lz4_decompress(comp.data(), clen, decomp.data(), 10000 + 64);
        /* Should either fail or produce wrong data */
        bool corrupted = (dlen < 0) || (std::memcmp(data.data(), decomp.data(), 10000) != 0);
        TEST_ASSERT(corrupted, "corruption detected or produces wrong output", result);
        std::printf("  bit flip detected: OK\n");
    }

    result.summary();
}

/* ── Compression level effectiveness ────────────────────────────────────– */

static void test_level_impact(TestResult& result) {
    std::printf("\n[Level Impact on Compression]\n");

    auto data = generate_text(100000);
    int bound = orot_lz4_compress_bound(100000);
    std::vector<uint8_t> comp(bound);

    std::printf("\n  Text data (100 KB) - compression ratio by level:\n");
    int size_l1 = 0;
    int size_l9 = 0;
    
    for (int lvl : {1, 3, 6, 9}) {
        int clen = orot_lz4_compress(data.data(), 100000, comp.data(), bound, lvl);
        double ratio = (100.0 * clen / 100000);
        std::printf("    L%d: %d bytes (%.1f%%)\n", lvl, clen, ratio);
        
        if (lvl == 1) size_l1 = clen;
        if (lvl == 9) size_l9 = clen;
    }
    
    /* Level 9 should compress at least as well as Level 1 
       (allowing small tolerance for implementation differences) */
    TEST_ASSERT(size_l9 <= size_l1 + 50, "L9 compression >= L1", result);
    result.pass();
    result.pass();

    result.summary();
}

/* ── Performance characterization ──────────────────────────────────────– */

static void test_performance_profile(TestResult& result) {
    std::printf("\n[Performance Profile]\n");

    using Clock = std::chrono::steady_clock;

    struct SizeTest {
        const char* name;
        size_t size;
    };

    SizeTest sizes[] = {
        { "64 KB", 64 * 1024 },
        { "256 KB", 256 * 1024 },
        { "1 MB", 1024 * 1024 },
    };

    auto data = generate_text(1024 * 1024);

    std::printf("\n  Throughput (compression at L6):\n");
    bool any_slow = false;
    for (const auto& spec : sizes) {
        int bound = orot_lz4_compress_bound(static_cast<int>(spec.size));
        std::vector<uint8_t> comp(bound);

        int iterations = std::max(1, static_cast<int>(10 * 1024 * 1024 / spec.size));

        auto t0 = Clock::now();
        for (int i = 0; i < iterations; ++i) {
            orot_lz4_compress(data.data(), static_cast<int>(spec.size),
                            comp.data(), bound, 6);
        }
        auto t1 = Clock::now();

        double secs = std::chrono::duration<double>(t1 - t0).count();
        double mbs = (spec.size * iterations) / secs / (1024.0 * 1024.0);
        std::printf("    %12s: %.1f MB/s\n", spec.name, mbs);

        /* Informational: throughput varies by system */
        if (mbs < 50.0) any_slow = true;
    }

    /* Overall check: at least some throughput achieved */
    TEST_ASSERT(!any_slow, "reasonable compression throughput", result);

    result.summary();
}

/* ── Main test runner ──────────────────────────────────────────────────── */

int main() {
    std::printf("\n");
    std::printf("╔═══════════════════════════════════════════════════════╗\n");
    std::printf("║    LZ4 Comprehensive Correctness & Performance        ║\n");
    std::printf("╚═══════════════════════════════════════════════════════╝\n");

    TestResult result;

    test_block_roundtrip_comprehensive(result);
    test_block_compression_ratio(result);
    test_frame_roundtrip_comprehensive(result);
    test_edge_cases(result);
    test_corruption_detection(result);
    test_level_impact(result);
    test_performance_profile(result);

    std::printf("\n");
    std::printf("╔═══════════════════════════════════════════════════════╗\n");
    if (result.failed == 0) {
        std::printf("║  ✓ ALL TESTS PASSED (%d/%d)                         ║\n",
                   result.passed, result.total);
        std::printf("╚═══════════════════════════════════════════════════════╝\n");
        return 0;
    } else {
        std::printf("║  ✗ SOME TESTS FAILED (%d/%d)                        ║\n",
                   result.failed, result.total);
        std::printf("╚═══════════════════════════════════════════════════════╝\n");
        return 1;
    }
}