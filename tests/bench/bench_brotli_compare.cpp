/*
 * bench_brotli_compare.cpp - Brotli benchmark against Google's libbrotli.
 */
#include <brotli/decode.h>
#include <brotli/encode.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <time.h>

#include "orot/brotli.h"

using Clock = std::chrono::steady_clock;

static double cpu_now() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct Dataset {
    const char* label;
    const char* key;
    std::vector<uint8_t> data;
};

struct BenchResult {
    double comp_mbs = 0;
    double decomp_mbs = 0;
    double cross_decomp_mbs = 0;
    double ratio_pct = 0;
    double cpu_ms = 0;
    int compressed_size = 0;
    bool ok = false;
    bool cross_ok = false;
};

struct Options {
    int quality = OROT_BROTLI_QUALITY_DEFAULT;
    int lgwin = OROT_BROTLI_LGWIN_DEFAULT;
    int iters = 0;
    const char* dataset = nullptr;
};

static void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [--quality=N] [--lgwin=N] [--iters=N] [--dataset=NAME]\n"
        "datasets: text, mixed, random, all\n",
        argv0);
}

static bool parse_int_arg(const char* arg, const char* name, int& out) {
    size_t n = std::strlen(name);
    if (std::strncmp(arg, name, n) != 0 || arg[n] != '=')
        return false;
    out = std::atoi(arg + n + 1);
    return true;
}

static bool parse_options(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (parse_int_arg(arg, "--quality", opts.quality) ||
            parse_int_arg(arg, "--lgwin", opts.lgwin) ||
            parse_int_arg(arg, "--iters", opts.iters))
            continue;
        const char* dataset_prefix = "--dataset=";
        size_t dataset_prefix_len = std::strlen(dataset_prefix);
        if (std::strncmp(arg, dataset_prefix, dataset_prefix_len) == 0) {
            opts.dataset = arg + dataset_prefix_len;
            continue;
        }
        std::fprintf(stderr, "unknown option: %s\n", arg);
        return false;
    }
    if (opts.quality < 0 ||
        opts.quality > OROT_BROTLI_QUALITY_MAX ||
        opts.lgwin < OROT_BROTLI_LGWIN_MIN ||
        opts.lgwin > OROT_BROTLI_LGWIN_MAX ||
        opts.iters < 0) {
        std::fprintf(stderr, "invalid option value\n");
        return false;
    }
    return true;
}

static std::vector<uint8_t> make_text() {
    std::vector<uint8_t> out;
    const char* text =
        "Brotli comparison payload with repeated text, paths, headers, "
        "literal contexts, and copy distances. ";
    for (int i = 0; i < 8192; ++i)
        out.insert(out.end(), text, text + std::strlen(text));
    return out;
}

static std::vector<uint8_t> make_mixed() {
    std::vector<uint8_t> out;
    for (int section = 0; section < 128; ++section) {
        const char* header = "HTTP/2 200 OK\ncontent-type: application/json\n\n";
        out.insert(out.end(), header, header + std::strlen(header));
        const char* json = "{\"codec\":\"brotli\",\"path\":\"/assets/app.css\",\"repeat\":";
        out.insert(out.end(), json, json + std::strlen(json));
        out.push_back(static_cast<uint8_t>('0' + (section % 10)));
        out.insert(out.end(), {'}', '\n'});
        for (int i = 0; i < 256; ++i)
            out.push_back(static_cast<uint8_t>((i * 31 + section * 17) & 0xff));
    }
    return out;
}

static std::vector<uint8_t> make_random(size_t n) {
    std::vector<uint8_t> out(n);
    uint32_t x = 0x12345678u;
    for (auto& b : out) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<uint8_t>(x >> 24);
    }
    return out;
}

static double mbps(size_t bytes, double seconds) {
    if (seconds <= 0.0)
        return 0.0;
    return (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds;
}

static bool verify_bytes(
    const std::vector<uint8_t>& expected,
    const std::vector<uint8_t>& actual,
    size_t actual_size)
{
    return actual_size == expected.size() &&
        (expected.empty() ||
         std::memcmp(expected.data(), actual.data(), expected.size()) == 0);
}

static BenchResult run_orot(
    const Dataset& ds,
    const Options& opts,
    int iters,
    std::vector<uint8_t>& compressed)
{
    BenchResult r;
    std::vector<uint8_t> orot_out(orot_brotli_compress_bound(ds.data.size()));
    int orot_size = 0;

    double cpu0 = cpu_now();
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        orot_size = orot_brotli_compress(
            ds.data.data(), ds.data.size(), orot_out.data(), orot_out.size(),
            opts.quality, opts.lgwin);
        if (orot_size <= 0) {
            std::fprintf(stderr, "  [WARN] orot compress failed: %d\n", orot_size);
            return r;
        }
    }
    auto t1 = Clock::now();
    double cpu1 = cpu_now();

    std::vector<uint8_t> decoded(ds.data.size() + 16);
    size_t actual = 0;
    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        int dlen = orot_brotli_decompress(
            orot_out.data(), static_cast<size_t>(orot_size),
            decoded.data(), decoded.size(), &actual);
        if (dlen != static_cast<int>(ds.data.size()) ||
            !verify_bytes(ds.data, decoded, actual)) {
            std::fprintf(stderr, "  [WARN] orot round-trip mismatch\n");
            return r;
        }
    }
    auto t3 = Clock::now();

    compressed.assign(orot_out.begin(), orot_out.begin() + orot_size);

    const double mb = static_cast<double>(ds.data.size() * static_cast<size_t>(iters)) / 1e6;
    r.comp_mbs = mb / std::chrono::duration<double>(t1 - t0).count();
    r.decomp_mbs = mb / std::chrono::duration<double>(t3 - t2).count();
    r.ratio_pct = 100.0 * static_cast<double>(orot_size) / static_cast<double>(ds.data.size());
    r.cpu_ms = (cpu1 - cpu0) * 1000.0 / iters;
    r.compressed_size = orot_size;
    r.ok = true;
    return r;
}

static BenchResult run_libbrotli(
    const Dataset& ds,
    const Options& opts,
    int iters,
    std::vector<uint8_t>& compressed)
{
    BenchResult r;
    std::vector<uint8_t> ref_out(BrotliEncoderMaxCompressedSize(ds.data.size()));
    size_t ref_size = ref_out.size();

    double cpu0 = cpu_now();
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        ref_size = ref_out.size();
        BROTLI_BOOL ok = BrotliEncoderCompress(
            opts.quality, opts.lgwin, BROTLI_MODE_GENERIC,
            ds.data.size(), ds.data.data(), &ref_size, ref_out.data());
        if (!ok) {
            std::fprintf(stderr, "  [WARN] libbrotli compress failed\n");
            return r;
        }
    }
    auto t1 = Clock::now();
    double cpu1 = cpu_now();

    std::vector<uint8_t> decoded(ds.data.size() + 16);
    auto t2 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        size_t decoded_size = decoded.size();
        BrotliDecoderResult ret = BrotliDecoderDecompress(
            ref_size, ref_out.data(), &decoded_size, decoded.data());
        if (ret != BROTLI_DECODER_RESULT_SUCCESS ||
            !verify_bytes(ds.data, decoded, decoded_size)) {
            std::fprintf(stderr, "  [WARN] libbrotli round-trip mismatch\n");
            return r;
        }
    }
    auto t3 = Clock::now();

    compressed.assign(ref_out.begin(), ref_out.begin() + static_cast<std::ptrdiff_t>(ref_size));

    const double mb = static_cast<double>(ds.data.size() * static_cast<size_t>(iters)) / 1e6;
    r.comp_mbs = mb / std::chrono::duration<double>(t1 - t0).count();
    r.decomp_mbs = mb / std::chrono::duration<double>(t3 - t2).count();
    r.ratio_pct = 100.0 * static_cast<double>(ref_size) / static_cast<double>(ds.data.size());
    r.cpu_ms = (cpu1 - cpu0) * 1000.0 / iters;
    r.compressed_size = static_cast<int>(ref_size);
    r.ok = true;
    return r;
}

static double run_libbrotli_decode(
    const Dataset& ds,
    const std::vector<uint8_t>& compressed,
    int iters,
    bool& ok)
{
    std::vector<uint8_t> decoded(ds.data.size() + 16);
    ok = true;
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        size_t cross_size = decoded.size();
        BrotliDecoderResult cross = BrotliDecoderDecompress(
            compressed.size(), compressed.data(),
            &cross_size, decoded.data());
        if (cross != BROTLI_DECODER_RESULT_SUCCESS ||
            !verify_bytes(ds.data, decoded, cross_size)) {
            ok = false;
            break;
        }
    }
    auto t1 = Clock::now();
    return mbps(ds.data.size() * static_cast<size_t>(iters),
                std::chrono::duration<double>(t1 - t0).count());
}

static double run_orot_decode(
    const Dataset& ds,
    const std::vector<uint8_t>& compressed,
    int iters,
    bool& ok)
{
    std::vector<uint8_t> decoded(ds.data.size() + 16);
    ok = true;
    auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) {
        size_t actual = 0;
        int ref_to_orot_len = orot_brotli_decompress(
            compressed.data(), compressed.size(), decoded.data(), decoded.size(), &actual);
        if (ref_to_orot_len != static_cast<int>(ds.data.size()) ||
            !verify_bytes(ds.data, decoded, actual)) {
            ok = false;
            break;
        }
    }
    auto t1 = Clock::now();
    return mbps(ds.data.size() * static_cast<size_t>(iters),
                std::chrono::duration<double>(t1 - t0).count());
}

static void print_row(const char* impl, const BenchResult& r) {
    if (!r.ok) {
        std::printf("  %-10s FAILED\n", impl);
        return;
    }
    std::printf("  %-10s comp=%7.1f MB/s decomp=%7.1f MB/s ratio=%5.1f%% cpu=%.2f ms\n",
                impl, r.comp_mbs, r.decomp_mbs, r.ratio_pct, r.cpu_ms);
}

static int run_dataset(const Dataset& ds, const Options& opts) {
    int iters = opts.iters != 0 ? opts.iters :
        (ds.data.size() < 128 * 1024 ? 100 : 30);

    std::printf("[%s lvl=q%d]\n", ds.label, opts.quality);

    std::vector<uint8_t> orot_stream;
    std::vector<uint8_t> ref_stream;
    BenchResult ro = run_orot(ds, opts, iters, orot_stream);
    BenchResult rb = run_libbrotli(ds, opts, iters, ref_stream);

    if (ro.ok)
        ro.cross_decomp_mbs = run_libbrotli_decode(ds, orot_stream, iters, ro.cross_ok);
    if (rb.ok)
        rb.cross_decomp_mbs = run_orot_decode(ds, ref_stream, iters, rb.cross_ok);

    print_row("orot", ro);
    print_row("libbrotli", rb);
    if (ro.ok && rb.ok) {
        std::printf("  speedup  comp=%.2fx  decomp=%.2fx\n",
                    ro.comp_mbs / rb.comp_mbs,
                    ro.decomp_mbs / rb.decomp_mbs);
        std::printf("  compat   orot->libbrotli=%s (%7.1f MB/s)  libbrotli->orot=%s (%7.1f MB/s)\n",
                    ro.cross_ok ? "ok" : "FAIL", ro.cross_decomp_mbs,
                    rb.cross_ok ? "ok" : "FAIL", rb.cross_decomp_mbs);
    }
    std::putchar('\n');
    return (ro.ok && rb.ok && ro.cross_ok && rb.cross_ok) ? 0 : 1;
}

int main(int argc, char** argv) {
    Options opts;
    if (!parse_options(argc, argv, opts)) {
        print_usage(argv[0]);
        return 2;
    }

    std::printf("Brotli comparison benchmark  iters=%s quality=%d lgwin=%d\n",
                opts.iters == 0 ? "auto" : "fixed", opts.quality, opts.lgwin);
    std::printf("  orot:      orot Brotli\n");
    std::printf("  libbrotli: Google Brotli Encoder/Decoder\n\n");

    Dataset datasets[] = {
        {"text (~800KB)", "text", make_text()},
        {"mixed (~45KB)", "mixed", make_mixed()},
        {"random (1MB)", "random", make_random(1 << 20)},
    };

    for (const auto& ds : datasets) {
        if (opts.dataset &&
            std::strcmp(opts.dataset, "all") != 0 &&
            std::strcmp(opts.dataset, ds.key) != 0)
            continue;
        if (run_dataset(ds, opts) != 0)
            return 1;
    }
    return 0;
}
