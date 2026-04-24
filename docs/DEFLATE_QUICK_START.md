# DEFLATE 빠른 시작 가이드

DEFLATE, zlib, gzip 형식의 압축/해제를 빠르게 검증하고 성능을 측정하는 방법을 설명합니다.

## 30초 빠른 테스트

```bash
cd orot
mkdir -p build && cd build
cmake -DOROT_TESTS=ON -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j$(nproc)
ctest --output-on-failure
```

**예상 결과:**
```
Test project build/tests
    100% tests passed, 0 tests failed
```

---

## 빌드 옵션

### 기본 빌드 (라이브러리만)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

결과: `build/liborot.a` (정적 라이브러리)

### 테스트 활성화

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOROT_TESTS=ON
cmake --build build -j$(nproc)
```

### 벤치마크 활성화

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
      -DOROT_DEFLATE_BENCH=ON
cmake --build build -j$(nproc)
```

### 비교 벤치마크 (zlib, libdeflate와 비교)

```bash
# 필수: zlib, libdeflate 설치
# macOS: brew install zlib libdeflate
# Ubuntu: sudo apt-get install zlib1g-dev libdeflate-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
      -DOROT_DEFLATE_BENCH=ON \
      -DOROT_DEFLATE_COMPARE_BENCH=ON
cmake --build build -j$(nproc)
```

### 호환성 테스트

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
      -DOROT_DEFLATE_COMPAT_TEST=ON
cmake --build build -j$(nproc)
```

---

## 세 가지 테스트 스위트

### 1. 기본 유닛 테스트 (test_*)

**목적:** DEFLATE 형식별 기본 기능 검증

```bash
ctest --test-dir build
```

또는 개별 실행:

```bash
# Roundtrip 테스트
./build/tests/test_roundtrip

# 형식 테스트 (Raw, Zlib, Gzip)
./build/tests/test_formats

# 압축 레벨 테스트 (L0-L12)
./build/tests/test_levels

# 스트리밍 API 테스트
./build/tests/test_streaming

# Huffman 정확성 테스트
./build/tests/test_huffman

# 멀티스레드 압축 테스트
./build/tests/test_parallel

# SIMD 가속 테스트
./build/tests/test_simd
```

**통과 기준:**
```
✓ All tests passed
```

### 2. 호환성 테스트 (compat_*)

**목적:** zlib, libdeflate와의 상호운용성 검증

```bash
ctest --test-dir build -R compat
```

**확인 사항:**
- orot로 압축한 데이터를 zlib/libdeflate로 해제 가능
- zlib/libdeflate로 압축한 데이터를 orot로 해제 가능
- 세 라이브러리 간 호환성 완벽

**통과 기준:**
```
✓ Interoperability tests passed
```

### 3. 성능 벤치마크 (bench_compress, bench_compare)

**단일 라이브러리 벤치마크:**

```bash
./build/tests/bench_compress [iterations]
```

**비교 벤치마크:**

```bash
./build/tests/bench_compare [iterations]
```

**출력 분석:**

```
dataset                         lvl    compress MB/s  decompress MB/s    ratio
──────────────────────────────────────────────────────────────────────────────
text (~90KB)                      1       2456.3          3145.2      52.34%
text (~90KB)                      6        892.1          2998.7      38.52%
text (~90KB)                      9        456.5          3102.1      32.40%

zeros (1 MB)                      1       3200.5          8500.2       0.31%
zeros (1 MB)                      6       1800.2          8200.5       0.31%
zeros (1 MB)                      9        500.5          8100.0       0.31%

random (1 MB)                     1       1100.5          1050.2     101.20%
random (1 MB)                     6        980.2          1030.0     101.20%
random (1 MB)                     9        150.5          1020.5     101.20%
```

---

## 세 가지 형식 이해하기

### Raw DEFLATE (RFC 1951)

**특징:**
- 가장 간단한 형식 (헤더 없음)
- 가장 작은 크기
- 내부적으로 다른 형식에 사용됨

**언제 사용:**
- DEFLATE만 필요한 경우
- 다른 형식과 결합할 때
- 임베디드 시스템

**C API 예시:**
```c
deflate_result r = deflate_compress(
    src, src_len, dst, dst_cap,
    level, DEFLATE_FORMAT_RAW);
```

### Zlib (RFC 1950)

**특징:**
- 2바이트 헤더 (정보 포함)
- Adler-32 체크섬
- 가장 널리 사용됨

**언제 사용:**
- 기본 선택 (대부분의 경우)
- 호환성 중요할 때
- 데이터 무결성 검증 필요

**C API 예시:**
```c
deflate_result r = deflate_compress(
    src, src_len, dst, dst_cap,
    level, DEFLATE_FORMAT_ZLIB);
```

### Gzip (RFC 1952)

**특징:**
- 10바이트 이상 헤더
- CRC-32 체크섬 (더 강력)
- 파일 메타데이터 지원
- .gz 파일 형식

**언제 사용:**
- 파일 저장 (.gz)
- HTTP 압축
- 아카이브
- 강력한 오류 검사 필요

**C API 예시:**
```c
deflate_result r = deflate_compress(
    src, src_len, dst, dst_cap,
    level, DEFLATE_FORMAT_GZIP);
```

---

## 압축 레벨 가이드

### 레벨 범위 및 특성

| 레벨 | 상수 | 속도 | 압축률 | 메모리 | 용도 |
|------|------|------|--------|--------|------|
| **0** | DEFLATE_LEVEL_STORE | 최고 | 없음 | 최소 | 이미 압축된 데이터 |
| **1** | DEFLATE_LEVEL_FAST | 매우 빠름 | 낮음 | 낮음 | 실시간 스트림 |
| **3** | - | 빠름 | 중간 | 중간 | 일반 용도 |
| **6** | DEFLATE_LEVEL_DEFAULT | 중간 | 중간 | 중간 | 기본 선택 |
| **9** | DEFLATE_LEVEL_BETTER | 느림 | 높음 | 높음 | 배포 파일 |
| **12** | DEFLATE_LEVEL_MAX | 매우 느림 | 최고 | 최고 | 아카이브 |

### 추천 레벨

```
L1: 실시간 스트림 (웹 서버, 메시지 큐)
L6: 기본 선택 (대부분의 경우)
L9: 일회성 압축 (백업, 배포 파일)
```

---

## 성능 기준표

### Throughput 목표 (MB/s)

| 데이터 | L1 압축 | L6 압축 | L9 압축 | 해제 |
|--------|---------|---------|---------|------|
| **Text** | 2000+ | 800+ | 400+ | 3000+ |
| **Zeros** | 3000+ | 1500+ | 500+ | 8000+ |
| **Random** | 1000+ | 800+ | 150+ | 1000+ |

### 압축률 목표 (%)

| 데이터 | L1 | L6 | L9 | 특징 |
|--------|-----|-----|-----|------|
| **Zeros** | 0.3-1% | 0.3-1% | 0.3-1% | 매우 우수 |
| **Text** | 40-50% | 30-40% | 25-35% | 우수 |
| **JSON** | 50-60% | 40-50% | 35-45% | 중간 |
| **Random** | ~100% | ~100% | ~100% | 압축 불가능 |

---

## C++ API 사용 예시

### Whole-Buffer 압축/해제

```cpp
#include <orot/deflate.h>

// 압축
std::vector<uint8_t> input = /* data */;
std::vector<uint8_t> compressed =
    orot::deflate::compress(
        input,
        orot::deflate::Level::Default,
        orot::deflate::Format::Zlib);

// 해제
std::vector<uint8_t> restored =
    orot::deflate::decompress(
        compressed,
        orot::deflate::Format::Zlib);

assert(input == restored);  // 정합성 확인
```

### 스트리밍 압축

```cpp
#include <orot/deflate.h>

orot::deflate::Compressor compressor(
    orot::deflate::Level::Default,
    orot::deflate::Format::Gzip);

std::vector<uint8_t> chunk_data = /* ... */;
std::array<uint8_t, 65536> out_buffer;

// 데이터 점진적 공급
size_t written = compressor.feed(
    std::span(chunk_data), 
    std::span(out_buffer));

// 완료
size_t final_written = compressor.finish(
    std::span(out_buffer));
```

### 병렬 압축

```cpp
#include <orot/deflate.h>

orot::deflate::ParallelCompressor pc(
    orot::deflate::Level::Default,
    orot::deflate::Format::Zlib,
    0,      // threads: 0 = 자동 감지
    0       // block_size: 0 = 자동 설정
);

std::vector<uint8_t> compressed = pc.compress(
    std::span(input_data));
```

---

## C API 사용 예시

### 기본 압축/해제

```c
#include <orot/deflate.h>
#include <stdint.h>

uint8_t input[] = /* ... */;
size_t input_size = sizeof(input);

// 출력 버퍼 할당
size_t bound = deflate_compress_bound(input_size, DEFLATE_FORMAT_ZLIB);
uint8_t* compressed = malloc(bound);

// 압축
size_t compressed_size = deflate_compress(
    input, input_size,
    compressed, bound,
    6,  // level
    DEFLATE_FORMAT_ZLIB);

// 해제
uint8_t* decompressed = malloc(input_size);
size_t actual_size;
deflate_result result = deflate_decompress(
    compressed, compressed_size,
    decompressed, input_size,
    &actual_size,
    DEFLATE_FORMAT_ZLIB);

if (result == DEFLATE_OK) {
    // 성공
}

free(compressed);
free(decompressed);
```

### 형식 선택

```c
// Raw DEFLATE
deflate_compress(..., DEFLATE_FORMAT_RAW);

// Zlib (권장)
deflate_compress(..., DEFLATE_FORMAT_ZLIB);

// Gzip
deflate_compress(..., DEFLATE_FORMAT_GZIP);
```

---

## 일반적인 질문 (FAQ)

### Q: 어떤 형식을 사용해야 하나?

**A:** 대부분의 경우 **Zlib**을 사용하세요.
- 기본: Zlib
- 파일 저장 (.gz): Gzip
- 최소 크기 필요: Raw DEFLATE

### Q: 어떤 레벨을 사용해야 하나?

**A:**
- 실시간: L1-L3
- 일반: L6 (기본)
- 최고 압축: L9-L12

### Q: 성능이 느리면?

**A:**
1. 빌드 타입 확인: `-DCMAKE_BUILD_TYPE=Release`
2. 레벨 낮추기: L6 → L1
3. SIMD 활성화: `-DOROT_DEFLATE_SIMD=ON` (기본)

### Q: 메모리 사용량이 많으면?

**A:**
1. 레벨 낮추기: L12 → L9
2. 청크 단위 처리: 스트리밍 API 사용

### Q: 다른 라이브러리와 호환되나?

**A:** 네, 완벽하게 호환됩니다.
- zlib과 호환
- libdeflate와 호환
- 공식 DEFLATE 스펙 준수

---

## 테스트 자동화

### CI/CD 통합

```bash
#!/bin/bash
set -e

echo "Building..."
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_TESTS=ON \
      -DOROT_DEFLATE_BENCH=ON
cmake --build build -j$(nproc)

echo "Running tests..."
ctest --test-dir build --output-on-failure

echo "Running benchmark..."
./build/tests/bench_compress 100

echo "✓ All validations passed!"
```

### GitHub Actions 예제

```yaml
- name: Build DEFLATE
  run: |
    cmake -B build -DCMAKE_BUILD_TYPE=Release \
          -DOROT_TESTS=ON -DOROT_DEFLATE_BENCH=ON
    cmake --build build -j4

- name: Run tests
  run: ctest --test-dir build --output-on-failure

- name: Benchmark
  run: ./build/tests/bench_compress 50
```

---

## 성능 회귀 감지

변경 후 성능 비교:

```bash
# 기준선 측정
./build/tests/bench_compress 100 > baseline.txt

# 코드 수정 후 재측정
./build/tests/bench_compress 100 > current.txt

# 비교
diff baseline.txt current.txt

# 상세 분석
grep "compress MB/s" baseline.txt > base.txt
grep "compress MB/s" current.txt > curr.txt
paste base.txt curr.txt
```

**회귀 기준:** MB/s 10% 이상 저하 → 조사 필요

---

## 문제 해결

### 빌드 실패

```bash
# CMake 버전 확인
cmake --version  # 3.20+ 필요

# C++20 컴파일러 확인
gcc --version    # GCC 10+
clang --version  # Clang 12+
```

### 테스트 실패

```bash
# 상세 로그
ctest --test-dir build --output-on-failure -V

# 개별 테스트
./build/tests/test_roundtrip

# 디버그 빌드
cmake -DCMAKE_BUILD_TYPE=Debug ..
gdb ./build/tests/test_roundtrip
```

### 성능 저하

```bash
# 최적화 플래그 확인
cmake -DCMAKE_BUILD_TYPE=Release ..

# 프로파일링 (macOS)
instruments -t "System Trace" ./build/tests/bench_compress

# 프로파일링 (Linux)
perf record ./build/tests/bench_compress
perf report
```

---

## 다음 단계

✅ 기본 테스트 통과 후:

1. **형식 선택**
   - Zlib (대부분의 경우)
   - Gzip (파일 저장)
   - Raw DEFLATE (임베디드)

2. **레벨 선택**
   - L1 (실시간, 스트리밍)
   - L6 (일반, 기본)
   - L9+ (아카이브)

3. **API 선택**
   - C++ RAII (권장)
   - C (C 호환성 필요)
   - 스트리밍 (대용량 데이터)
   - 병렬 (멀티코어)

4. **통합**
   - FetchContent 또는 add_subdirectory
   - 프로젝트에 링크

5. **배포**
   - 성능 측정 (벤치마크)
   - 호환성 테스트
   - 프로덕션 배포

---

## 참고 자료

- **상세 테스트 가이드:** [docs/DEFLATE_TESTING.md](DEFLATE_TESTING.md)
- **검증 보고서:** [docs/DEFLATE_VALIDATION_REPORT.md](DEFLATE_VALIDATION_REPORT.md)
- **DEFLATE 스펙:** RFC 1951
- **Zlib 스펙:** RFC 1950
- **Gzip 스펙:** RFC 1952

---

**마지막 업데이트:** 2024년
**상태:** ✅ Production Ready
