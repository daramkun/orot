# orot

C++20 무손실 압축/해제 라이브러리. Raw DEFLATE (RFC 1951), zlib (RFC 1950), gzip (RFC 1952) 포맷 지원.

## 특징

- **DEFLATE 지원:** Raw DEFLATE, zlib, gzip 형식 (RFC 1951/1950/1952)
  - C API (whole-buffer, streaming, parallel) + C++ RAII 래퍼
  - 압축 레벨 0–12 (L0=store, L1=fast, L6=default, L9=better, L12=max)
  - 자세한 내용: [DEFLATE 빠른 시작](docs/DEFLATE_QUICK_START.md) | [상세 가이드](docs/DEFLATE_TESTING.md) | [검증 보고서](docs/DEFLATE_VALIDATION_REPORT.md)
- **LZ4 지원:** 빠른 압축/해제
  - Block 및 Frame 포맷 지원
  - 압축 레벨 1–9
  - 자세한 내용: [LZ4 빠른 시작](docs/LZ4_QUICK_START.md) | [상세 가이드](docs/LZ4_TESTING.md) | [검증 보고서](docs/LZ4_VALIDATION_REPORT.md)
- **LZW/LZMA/Bzip2/Zstandard 지원:** whole-buffer 압축/해제
  - Zstandard frame raw/RLE 압축, raw/RLE/compressed block 해제
  - Zstandard raw content dictionary API 및 buffered streaming API
  - 구현 상세: [Zstandard 구현 문서](docs/impl/zstd.md)
- **Brotli 지원:** compressed/uncompressed meta-block 압축/해제, libbrotli 호환 테스트
  - 구현 상세: [Brotli 구현 문서](docs/impl/brotli.md)
- SIMD 가속: SSE2 / SSE4.2 / AVX2 (x86), NEON / CRC32 (ARM)
- 멀티스레드 압축 (pigz 스타일)
- 커스텀 allocator 지원

## 요구사항

- CMake 3.20+
- C++20 컴파일러 (GCC 10+, Clang 12+, MSVC 2022+)
- (비교 벤치마크/호환성 테스트/선택적 가속 백엔드용) zlib, libdeflate, liblz4, liblzma, libbz2, libzstd, libbrotli

## 빌드

### 라이브러리만 빌드

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

정적 라이브러리 `build/liborot.a` 생성.

### 공유 라이브러리로 빌드

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOROT_SHARED=ON
cmake --build build -j$(nproc)
```

### 설치

```bash
cmake --install build --prefix /usr/local
```

헤더는 `include/orot/`, 라이브러리는 `lib/`에 설치.

## CMake 옵션

| 옵션 | 기본값 | 설명 |
|------|--------|------|
| `OROT_USE_SIMD` | ON | SSE2/SSE4.2/AVX2/NEON/CRC 가속 |
| `OROT_AS_PARALLEL` | ON | 병렬 압축 (pigz 스타일) |
| `OROT_TESTS` | OFF | 유닛 테스트 빌드 |
| `OROT_BENCHMARK` | OFF | 단일 라이브러리 벤치마크 |
| `OROT_BENCHMARK_COMPARE` | OFF | zlib/libdeflate/liblz4/liblzma/libbz2/libzstd/libbrotli 비교 벤치마크 |
| `OROT_TESTS` | OFF | 교차 라이브러리 호환성 테스트 |
| `OROT_DEFLATE_FUZZ` | OFF | libFuzzer 퍼즈 타겟 |
| `OROT_SHARED` | OFF | 공유 라이브러리 (기본: 정적) |
| `OROT_DEFLATE_ZLIB_COMPAT` | ON | `Z_OK` 등 zlib 호환 매크로 |
| `OROT_USE_SYSTEM_BACKENDS` | ON | 설치된 기준 라이브러리로 호환 whole-buffer 경로 가속 |
| `OROT_BENCHMARK` | OFF | 알고리즘별 단일 라이브러리 벤치마크 |
| `OROT_BENCHMARK_COMPARE` | OFF | 알고리즘별 외부 라이브러리 비교 벤치마크 |

## 테스트

### DEFLATE 테스트

#### 빌드
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOROT_TESTS=ON
cmake --build build -j$(nproc)
```

#### 실행
```bash
# 전체 테스트
ctest --test-dir build

# 상세 출력
ctest --test-dir build --output-on-failure

# 특정 테스트만
ctest --test-dir build -R roundtrip
ctest --test-dir build -R "roundtrip|formats|levels"
```

#### 테스트 항목
| 테스트 | 내용 |
|--------|------|
| `test_roundtrip` | 압축 ↔ 해제 라운드트립 |
| `test_formats` | Raw / Zlib / Gzip 포맷 |
| `test_levels` | 레벨 0–12 전체 |
| `test_streaming` | 스트리밍 API 점진적 공급 |
| `test_huffman` | Huffman 테이블 정확성 |
| `test_parallel` | 멀티스레드 압축 |
| `test_simd` | SIMD 가속 경로 |

자세한 가이드: [DEFLATE 테스트 완전 가이드](docs/DEFLATE_TESTING.md)

### DEFLATE 호환성 테스트 (zlib/libdeflate 필요)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON 
cmake --build build -j$(nproc)
ctest --test-dir build -R compat
```

**호환성 검증:**
- orot ↔ zlib 완벽 호환
- orot ↔ libdeflate 완벽 호환
- RFC 1951/1950/1952 준수

자세한 내용: [DEFLATE 호환성 테스트](docs/DEFLATE_TESTING.md#호환성-테스트)

### LZ4 테스트 (선택)

#### 빌드
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
     
cmake --build build -j$(nproc)
```

#### 실행
```bash
# 기본 LZ4 정합성 테스트
./build/tests/test_lz4

# 종합 LZ4 검증 (정합성, 압축률, 성능)
./build/tests/test_lz4_comprehensive
```

#### 테스트 항목
| 테스트 | 내용 |
|--------|------|
| `test_lz4` | LZ4 block/frame 정합성 |
| `test_lz4_comprehensive` | LZ4 종합 검증 (97개 테스트) |

자세한 가이드: [LZ4 테스트 완전 가이드](docs/LZ4_TESTING.md)

### Zstandard 테스트

#### 빌드
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOROT_TESTS=ON
cmake --build build -j$(nproc)
```

#### 실행
```bash
./build/tests/test_zstd
ctest --test-dir build -R test_zstd --output-on-failure
```

#### 테스트 항목
| 테스트 | 내용 |
|--------|------|
| `test_zstd` | frame/block 파싱, compressed block 샘플, dictionary API, streaming API, 에러 경로 |

## 벤치마크

### DEFLATE 성능 벤치마크

#### 단일 라이브러리 벤치마크
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
      -DOROT_BENCHMARK=ON
cmake --build build -j$(nproc)
./build/tests/bench_compress [iterations]
```

#### 비교 벤치마크 (zlib, libdeflate 포함)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
      -DOROT_BENCHMARK=ON \
      -DOROT_BENCHMARK_COMPARE=ON
cmake --build build -j$(nproc)
./build/tests/bench_compare [iterations]
```

**성능 결과 (전형적):**
```
Zeros (L1):   3200+ MB/s 압축, 8500+ MB/s 해제
Text (L6):     900+ MB/s 압축, 3100+ MB/s 해제
Random (L1):  1100+ MB/s 압축, 1000+ MB/s 해제
```

자세한 해석: [DEFLATE 성능 가이드](docs/DEFLATE_QUICK_START.md#성능-기준표)

### LZ4 성능 벤치마크

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
      \
      -DOROT_BENCHMARK=ON
cmake --build build -j$(nproc)
./build/tests/bench_lz4 [iterations]
```

**성능 결과 (전형적):**
```
Zeros (L1):   30K+ MB/s 압축, 24K+ MB/s 해제
Text (L1):    10K+ MB/s 압축, 35K+ MB/s 해제
Random (L1):    90 MB/s 압축, 31K+ MB/s 해제
```

자세한 해석: [LZ4 성능 가이드](docs/LZ4_QUICK_START.md#성능-기준표)

### LZ4 비교 벤치마크 (liblz4와 비교)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
      \
      -DOROT_BENCHMARK_COMPARE=ON
cmake --build build -j$(nproc)
./build/tests/bench_lz4_compare [iterations]
```

### Zstandard 성능 벤치마크

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
      -DOROT_BENCHMARK=ON
cmake --build build -j$(nproc)
./build/tests/bench_zstd [iterations]
```

### Zstandard 비교 벤치마크 (libzstd와 비교)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
      -DOROT_BENCHMARK_COMPARE=ON
cmake --build build -j$(nproc)
./build/tests/bench_zstd_compare [iterations]
```

## CMake 프로젝트에서 사용

### FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(orot
    GIT_REPOSITORY https://github.com/yourname/orot.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(orot)

target_link_libraries(your_target PRIVATE orot)
```

### add_subdirectory

```cmake
add_subdirectory(orot)
target_link_libraries(your_target PRIVATE orot)
```

## API 사용 예시

### C++ (whole-buffer)

```cpp
#include <orot/deflate.h>

// 압축
std::vector<uint8_t> input = ...;
std::vector<uint8_t> compressed =
    orot::deflate::compress(input, orot::deflate::Level::Default, orot::deflate::Format::Gzip);

// 해제
std::vector<uint8_t> restored =
    orot::deflate::decompress(compressed, orot::deflate::Format::Gzip);
```

### C++ (streaming)

```cpp
#include <orot/deflate.h>

// 압축
orot::deflate::Compressor cmp(orot::deflate::Level::Default, orot::deflate::Format::Zlib);
std::array<uint8_t, 65536> outbuf;
size_t n = cmp.feed(input_span, outbuf);
size_t final_n = cmp.finish(outbuf);

// 해제
orot::deflate::Decompressor dec(orot::deflate::Format::Zlib);
bool done = false;
size_t written = dec.feed(compressed_span, outbuf, done);
```

### C++ (병렬 압축)

```cpp
#include <orot/deflate.h>

orot::deflate::ParallelCompressor pc(
    orot::deflate::Level::Default,
    orot::deflate::Format::Gzip,
    0,      // threads: 0 = 자동
    0       // block_size: 0 = 자동
);
auto out = pc.compress(input_span);
```

### C API

```c
#include <orot/deflate.h>

// 압축
size_t bound = deflate_compress_bound(in_size, DEFLATE_FORMAT_GZIP);
uint8_t* out = malloc(bound);
size_t out_size = deflate_compress(
    in, in_size, out, bound, 6, DEFLATE_FORMAT_GZIP);

// 해제
size_t actual;
deflate_result r = deflate_decompress(
    in, in_size, out, out_capacity, &actual, DEFLATE_FORMAT_GZIP);
```

## 압축 레벨 가이드

| 레벨 | 상수 | 속도 | 압축률 | 용도 |
|------|------|------|--------|------|
| 0 | `DEFLATE_LEVEL_STORE` | 최고 | 없음 | 이미 압축된 데이터 |
| 1 | `DEFLATE_LEVEL_FAST` | 매우 빠름 | 낮음 | 실시간 스트림 |
| 6 | `DEFLATE_LEVEL_DEFAULT` | 균형 | 중간 | 일반 용도 |
| 9 | `DEFLATE_LEVEL_BETTER` | 느림 | 높음 | 배포 파일 |
| 12 | `DEFLATE_LEVEL_MAX` | 매우 느림 | 최고 | 아카이브 |

## 지원 포맷

| 포맷 | 상수 | 체크섬 | RFC |
|------|------|--------|-----|
| Raw DEFLATE | `DEFLATE_FORMAT_RAW` | 없음 | 1951 |
| zlib | `DEFLATE_FORMAT_ZLIB` | Adler-32 | 1950 |
| gzip | `DEFLATE_FORMAT_GZIP` | CRC-32 | 1952 |
```

이제 PLAN.md도 확인하고 업데이트하겠습니다.
