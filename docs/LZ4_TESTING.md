# LZ4 Testing & Benchmarking Guide

## Overview

The OROT project includes comprehensive testing and performance benchmarking for the LZ4 compression implementation. Four main targets are provided:

1. **test_lz4**: Original unit tests for block and frame formats
2. **test_lz4_comprehensive**: Extended validation tests covering correctness, compression ratios, and edge cases
3. **bench_lz4**: OROT-only benchmark for block/frame throughput and compression ratio
4. **bench_lz4_compare**: Cross-library benchmark comparing OROT with `liblz4`

---

## Building Tests

### Prerequisite: Enable LZ4 Support

First, ensure LZ4 is enabled in your CMake build:

```bash
cd orot
mkdir -p build
cd build
cmake .. -DOROT_TESTS=ON -DOROT_LZ4=ON
```

### Build Unit Tests

```bash
cmake --build . --target test_lz4 test_lz4_comprehensive
```

Or build all tests:

```bash
cmake --build .
```

### Build Performance Benchmarks

```bash
cmake .. -DOROT_TESTS=ON -DOROT_LZ4=ON -DOROT_BENCHMARK=ON
cmake --build . --target bench_lz4
```

Build the comparison benchmark against `liblz4`:

```bash
cmake .. -DOROT_TESTS=ON -DOROT_LZ4=ON -DOROT_BENCHMARK_COMPARE=ON
cmake --build . --target bench_lz4_compare
```

---

## Running Tests

### Basic Unit Tests

Run the original LZ4 tests:

```bash
./tests/unit/test_lz4
```

Expected output:
```
PASS block zeros-1MB L1                in=1048576  out=   1055  ratio=0.10
PASS block repeat-64KB L1                in= 65536  out=   2156  ratio=3.29
...
All LZ4 tests passed.
```

### Comprehensive Validation Tests

Run extended tests with detailed output:

```bash
./tests/unit/test_lz4_comprehensive
```

This tests:
- **Roundtrip integrity**: Compression/decompression correctness on various data types
- **Compression ratios**: Efficiency validation across compression levels
- **Edge cases**: Single byte, empty data, boundary conditions
- **Corruption detection**: Bitflip handling
- **Performance characterization**: Throughput measurement

Expected output:
```
╔═══════════════════════════════════════════════════════╗
║    LZ4 Comprehensive Correctness & Performance        ║
╚═══════════════════════════════════════════════════════╝

[Block Format - Roundtrip]
  single byte                    L1  1      → 17  (1700.00%)
  4 bytes (MIN_MATCH)            L1  4      → 18  (450.00%)
  ...
  ✓ All 18 tests passed
```

---

## Performance Benchmarking

### Running Benchmark

```bash
./tests/bench/bench_lz4 [iterations]
```

Default: 100 iterations
Suggested: 100-500 iterations for stable measurements

Example:

```bash
./tests/bench/bench_lz4 200
```

### Output Format

The benchmark produces a compact table similar to the DEFLATE benchmarks:

```
Format     Dataset           Level      Comp MB/s   Decomp MB/s   Ratio%
-----------------------------------------------------------------------
block      text (~90KB)      L1            2456.3        3145.2    51.6%
frame      text (~90KB)      L1            2401.5        3102.1    51.9%
block      zeros (1MB)       L6           30000.0       24000.0     0.4%
...
```

### Running Comparison Benchmark

```bash
./build/tests/bench_lz4_compare [iterations]
```

Example:

```bash
./build/tests/bench_lz4_compare 100
```

The comparison benchmark mirrors `bench_compare` from DEFLATE and adds a `Format` column:

```
Library    Format   Dataset           Level      Comp MB/s   Decomp MB/s   Ratio%    CPU ms  RSS dKB
------------------------------------------------------------------------------------------------------
orot       block    text (~90KB)      L1            2400.0        3100.0    51.6%     0.040        0
liblz4     block    text (~90KB)      L1            2550.0        3200.0    50.9%     0.038        0
orot       frame    text (~90KB)      L1            2350.0        3050.0    51.9%     0.041        0
liblz4     frame    text (~90KB)      L1            2480.0        3180.0    51.2%     0.039        0
```

---

## Understanding Results

### Compression Ratio Interpretation

| Dataset | L1 Ratio | L9 Ratio | Interpretation |
|---------|----------|----------|----------------|
| Zeros | < 0.5% | < 0.5% | Excellent (highly repetitive) |
| Text | 40-60% | 30-50% | Good (natural language) |
| Random | ~100% | ~100% | None (incompressible) |

**Note**: LZ4 is a fast compression codec, not designed for best compression. Ratios are typically 40-80% for compressible data.

### Performance Metrics

- **Compress MB/s**: Throughput when compressing data
  - L1 (fast): Typically 1000-3000 MB/s
  - L9 (best): Typically 100-500 MB/s
  
- **Decompress MB/s**: Throughput when decompressing data
  - All levels: Typically 2000-5000 MB/s (decompression is always fast)

- **Block vs Frame**: 
  - Block is slightly faster (no header/footer overhead)
  - Frame is more compatible with lz4 CLI tools

### Performance Characterization

Check relative performance on various data sizes:

```bash
./tests/bench/bench_lz4 50
```

The final section shows:
```
[Performance Profile]
  
  Throughput (compression at L6):
    4 KB: 1200.5 MB/s
    64 KB: 2100.3 MB/s
    256 KB: 2340.1 MB/s
    1 MB: 2280.5 MB/s
```

Healthy pattern: Throughput increases with larger blocks then plateaus.

---

## Data Types Tested

### Text (Repetitive)
- Sample: English prose with common patterns
- Expected: 45-65% compression ratio
- Good cache locality

### Zeros (Highly Compressible)
- Sample: 1 MB of all zeros
- Expected: < 1% compression ratio
- Tests sparse data handling

### Random (Incompressible)
- Sample: High-entropy pseudo-random data
- Expected: ~100% compression ratio (worst case)
- Tests incompressible data handling

### Repeating Pattern
- Sample: Cyclic byte pattern (e.g., 0..250 repeating)
- Expected: 20-40% compression ratio
- Tests structured repetition

---

## Validation Checklist

After building and running tests, verify:

- [ ] `test_lz4` runs without crashes and reports all tests PASS
- [ ] `test_lz4_comprehensive` completes with 0 failures
- [ ] `bench_lz4` shows consistent throughput across runs
- [ ] `bench_lz4_compare` completes successfully when `liblz4` is installed
- [ ] Compression ratios for zeros data are < 1%
- [ ] Compression ratios for text data are 40-70%
- [ ] Decompression is consistently faster than compression
- [ ] Level 9 compression is never worse than Level 1 for same data
- [ ] No crashes on corrupted or edge-case inputs

---

## Advanced: Custom Data Testing

To test with your own data, modify `bench_lz4.cpp`:

1. Add your data generation function:
```cpp
static std::vector<uint8_t> generate_custom(size_t len) {
    std::vector<uint8_t> data(len);
    // Fill with your data
    return data;
}
```

2. Add to datasets array:
```cpp
Dataset datasets[] = {
    // ... existing datasets ...
    { custom.data(), custom.size(), "my-data" },
};
```

3. Rebuild and run:
```bash
cmake --build . --target bench_lz4
./tests/bench/bench_lz4 100
```

---

## Troubleshooting

### Test Fails: "decompress size mismatch"
- Likely implementation bug in decompression
- Check `orot_lz4_decompress` return value handling
- Verify compressed data is not corrupted

### Benchmark Shows Low Throughput (< 100 MB/s)
- Check build flags: Ensure `-O3` optimization is enabled
- Check platform: Some systems/compilers run slower
- Compare with reference lz4: `lz4 -b -i 100 testfile`

### Memory Usage Spikes
- Benchmark allocates `(len + sizeof(bound))` for each test
- For 1 MB data at multiple levels: ~100 MB peak memory normal
- Each iteration is independent; no accumulation

### Platform-Specific Issues
- **macOS**: May require `brew install lz4` for reference comparisons
- **Linux**: Standard system packages usually available
- **Windows**: MSVC build requires C++17 or later

---

## Continuous Integration

### GitHub Actions Example

```yaml
- name: Build LZ4 tests
  run: |
    cmake -B build -DOROT_LZ4=ON -DOROT_BENCHMARK=ON
    cmake --build build

- name: Run unit tests
  run: ./build/tests/unit/test_lz4_comprehensive

- name: Run benchmark
  run: ./build/tests/bench/bench_lz4 50
```

---

## Performance Regression Detection

Compare benchmark runs to detect regressions:

```bash
# Baseline
./tests/bench/bench_lz4 100 > baseline.txt

# After changes
./tests/bench/bench_lz4 100 > current.txt

# Compare
diff baseline.txt current.txt
```

Look for > 10% deviation in MB/s as potential regression.

---

## References

- [LZ4 Official](https://github.com/lz4/lz4)
- [LZ4 Frame Format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md)
- [LZ4 Block Format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md)

---

## Test Maintenance

When modifying LZ4 implementation:

1. **Run all unit tests** to ensure correctness
2. **Run comprehensive tests** for edge cases
3. **Run benchmark** to measure performance impact
4. **Compare ratios** across compression levels
5. **Validate corruption handling** still works

```bash
#!/bin/bash
# Quick validation script

echo "Building..."
cmake --build . --target test_lz4 test_lz4_comprehensive bench_lz4

echo "Running unit tests..."
./tests/unit/test_lz4
./tests/unit/test_lz4_comprehensive

echo "Running benchmark..."
./tests/bench/bench_lz4 50

echo "All validation complete!"
```
