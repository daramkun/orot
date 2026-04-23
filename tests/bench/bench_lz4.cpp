/*
 * bench_lz4.cpp — LZ4 compression/decompression performance benchmark.
 *
 * Measures LZ4 block and frame compression throughput at various levels
 * for different data types (text, zeros, random).
 * 
 * Usage: ./bench_lz4 [iterations]
 */
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "orot/lz4.h"

using Clock = std::chrono::steady_clock;

/* ── Helper: measure compression throughput ────────────────────────────── */

static double bench_lz4_block_compress(
    const uint8_t* data, size_t len,
    int level, int iterations)
{
    const int bound = orot_lz4_compress_bound(static_cast<int>(len));
    std::vector<uint8_t> out(bound);

    /* Warm up */
    orot_lz4_compress(data, static_cast<int>(len), out.data(), bound, level);

    auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i)
        orot_lz4_compress(data, static_cast<int>(len), out.data(), bound, level);
    auto t1 = Clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double bytes = static_cast<double>(len) * iterations;
    return bytes / secs / (1024.0 * 1024.0);  /* MB/s */
}

static double bench_lz4_block_decompress(
    const uint8_t* data, size_t len,
    int level, int iterations)
{
    const int bound = orot_lz4_compress_bound(static_cast<int>(len));
    std::vector<uint8_t> comp(bound);
    const int clen = orot_lz4_compress(data, static_cast<int>(len), comp.data(), bound, level);
    if (clen <= 0) return 0.0;

    std::vector<uint8_t> out(len + 64);

    /* Warm up */
    orot_lz4_decompress(comp.data(), clen, out.data(), static_cast<int>(out.size()));

    auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i)
        orot_lz4_decompress(comp.data(), clen, out.data(), static_cast<int>(out.size()));
    auto t1 = Clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double bytes = static_cast<double>(len) * iterations;
    return bytes / secs / (1024.0 * 1024.0);
}

static double bench_lz4f_compress(
    const uint8_t* data, size_t len,
    int level, int iterations)
{
    const int bound = orot_lz4f_compress_bound(static_cast<int>(len));
    std::vector<uint8_t> out(bound);

    /* Warm up */
    orot_lz4f_compress(data, static_cast<int>(len), out.data(), bound, level);

    auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i)
        orot_lz4f_compress(data, static_cast<int>(len), out.data(), bound, level);
    auto t1 = Clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double bytes = static_cast<double>(len) * iterations;
    return bytes / secs / (1024.0 * 1024.0);
}

static double bench_lz4f_decompress(
    const uint8_t* data, size_t len,
    int level, int iterations)
{
    const int bound = orot_lz4f_compress_bound(static_cast<int>(len));
    std::vector<uint8_t> comp(bound);
    const int clen = orot_lz4f_compress(data, static_cast<int>(len), comp.data(), bound, level);
    if (clen <= 0) return 0.0;

    std::vector<uint8_t> out(len + 64);

    /* Warm up */
    orot_lz4f_decompress(comp.data(), clen, out.data(), static_cast<int>(out.size()));

    auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i)
        orot_lz4f_decompress(comp.data(), clen, out.data(), static_cast<int>(out.size()));
    auto t1 = Clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double bytes = static_cast<double>(len) * iterations;
    return bytes / secs / (1024.0 * 1024.0);
}

/* ── Compression ratio measurement ──────────────────────────────────────── */

static void measure_compression_ratio(
    const uint8_t* data, size_t len, int level, const char* label)
{
    const int bound = orot_lz4_compress_bound(static_cast<int>(len));
    std::vector<uint8_t> comp(bound);
    const int clen = orot_lz4_compress(data, static_cast<int>(len), comp.data(), bound, level);
    
    if (clen > 0) {
        const double ratio = (len > 0) ? (100.0 * clen / len) : 0.0;
        std::printf("  %-40s L%d  in=%8zu  out=%8d  ratio=%6.2f%%\n",
            label, level, len, clen, ratio);
    }
}

int main(int argc, char* argv[]) {
    int iters = 100;
    if (argc > 1) iters = std::atoi(argv[1]);
    if (iters < 1) iters = 1;

    std::printf("========================================\n");
    std::printf("LZ4 Compression Benchmark\n");
    std::printf("========================================\n\n");

    /* ── Build test datasets ─────────────────────────────────────────────── */

    /* Repetitive text */
    std::string text;
    const char* text_samples[] = {
        "The quick brown fox jumps over the lazy dog. ",
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. ",
        "Sphinx of black quartz, judge my vow. ",
        "Pack my box with five dozen liquor jugs. ",
        "How vexingly quick daft zebras jump! ",
        "Jackdaws love my big sphinx of quartz. ",
        "Two driven jocks help foxy brown jump. ",
    };
    for (int i = 0; i < 2000; ++i)
        text += text_samples[i % 7];
    const auto* tp = reinterpret_cast<const uint8_t*>(text.data());
    const size_t tl = text.size();

    /* All zeros (highly compressible) */
    std::vector<uint8_t> zeros(1024 * 1024, 0);

    /* Random data (incompressible) */
    std::vector<uint8_t> rnd(1024 * 1024);
    {
        uint32_t state = 0xDEADBEEFU;
        for (auto& b : rnd) {
            state = state * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(state >> 24);
        }
    }

    /* JSON-like structured data */
    std::string json;
    for (int i = 0; i < 500; ++i) {
        json += "{\"id\":" + std::to_string(i) + 
                ",\"name\":\"item_" + std::to_string(i % 100) +
                "\",\"type\":\"product\",\"status\":\"active\"}\n";
    }
    const auto* jp = reinterpret_cast<const uint8_t*>(json.data());
    const size_t jl = json.size();

    /* Structured repeating pattern */
    std::vector<uint8_t> pattern(256 * 1024);
    for (size_t i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<uint8_t>((i / 4) % 256);

    struct Dataset {
        const uint8_t* data;
        size_t len;
        const char* name;
    };

    Dataset datasets[] = {
        { tp, tl, "text (~100KB)" },
        { zeros.data(), zeros.size(), "zeros (1 MB)" },
        { rnd.data(), rnd.size(), "random (1 MB)" },
        { pattern.data(), pattern.size(), "pattern (256 KB)" },
        { jp, jl, "json (~50KB)" },
    };

    static const int levels[] = { 1, 3, 6, 9 };

    /* ── Compression ratio ─────────────────────────────────────────────── */

    std::printf("Compression Ratio (LZ4 block):\n");
    std::printf("%s\n", std::string(80, '-').c_str());
    for (const auto& ds : datasets) {
        for (int lvl : levels) {
            measure_compression_ratio(ds.data, ds.len, lvl, ds.name);
        }
        std::printf("\n");
    }

    /* ── LZ4 Block performance ──────────────────────────────────────────── */

    std::printf("\nLZ4 Block Format Performance:\n");
    std::printf("%s\n", std::string(90, '-').c_str());
    std::printf("%-30s  %4s  %15s  %15s  %10s\n",
        "dataset", "lvl", "compress MB/s", "decompress MB/s", "ratio");
    std::printf("%s\n", std::string(90, '-').c_str());

    for (const auto& ds : datasets) {
        for (int lvl : levels) {
            const int bound = orot_lz4_compress_bound(static_cast<int>(ds.len));
            std::vector<uint8_t> comp(bound);
            const int clen = orot_lz4_compress(
                ds.data, static_cast<int>(ds.len), comp.data(), bound, lvl);

            const double ratio = (ds.len > 0) ? (100.0 * clen / ds.len) : 0.0;
            const double comp_mbs = bench_lz4_block_compress(ds.data, ds.len, lvl, iters);
            const double decomp_mbs = bench_lz4_block_decompress(ds.data, ds.len, lvl, iters);

            std::printf("%-30s  %4d  %15.1f  %15.1f  %9.2f%%\n",
                ds.name, lvl, comp_mbs, decomp_mbs, ratio);
        }
        std::printf("\n");
    }

    /* ── LZ4 Frame performance ──────────────────────────────────────────– */

    std::printf("\nLZ4 Frame Format Performance:\n");
    std::printf("%s\n", std::string(90, '-').c_str());
    std::printf("%-30s  %4s  %15s  %15s  %10s\n",
        "dataset", "lvl", "compress MB/s", "decompress MB/s", "ratio");
    std::printf("%s\n", std::string(90, '-').c_str());

    for (const auto& ds : datasets) {
        for (int lvl : levels) {
            const int bound = orot_lz4f_compress_bound(static_cast<int>(ds.len));
            std::vector<uint8_t> comp(bound);
            const int clen = orot_lz4f_compress(
                ds.data, static_cast<int>(ds.len), comp.data(), bound, lvl);

            const double ratio = (ds.len > 0) ? (100.0 * clen / ds.len) : 0.0;
            const double comp_mbs = bench_lz4f_compress(ds.data, ds.len, lvl, iters);
            const double decomp_mbs = bench_lz4f_decompress(ds.data, ds.len, lvl, iters);

            std::printf("%-30s  %4d  %15.1f  %15.1f  %9.2f%%\n",
                ds.name, lvl, comp_mbs, decomp_mbs, ratio);
        }
        std::printf("\n");
    }

    std::printf("========================================\n");
    std::printf("Benchmark complete (iterations: %d)\n", iters);
    std::printf("========================================\n");

    return 0;
}