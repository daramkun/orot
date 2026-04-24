# DEFLATE 상세 테스트 및 검증 가이드

이 문서는 orot의 DEFLATE 구현(Raw/Zlib/Gzip)에 대한 상세한 검증, 성능 측정, 최적화 방법을 설명합니다.

---

## 목차

1. [빌드 설정](#빌드-설정)
2. [단위 테스트](#단위-테스트)
3. [호환성 테스트](#호환성-테스트)
4. [성능 벤치마크](#성능-벤치마크)
5. [성능 최적화](#성능-최적화)
6. [프로파일링](#프로파일링)
7. [문제 해결](#문제-해결)
8. [CI/CD 통합](#cicd-통합)

---

## 빌드 설정

### 환경 요구사항

```
- CMake 3.20+
- C++20 컴파일러 (GCC 10+, Clang 12+, MSVC 2022+)
- (선택) zlib, libdeflate (비교 벤치마크용)
```

### 기본 빌드

```bash
cd orot
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j$(nproc)
```

### 모든 기능 활성화

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_DEFLATE_SIMD=ON \
      -DOROT_DEFLATE_THREADS=ON \
      -DOROT_DEFLATE_TESTS=ON \
      -DOROT_DEFLATE_BENCH=ON \
      -DOROT_DEFLATE_COMPARE_BENCH=ON \
      -DOROT_DEFLATE_COMPAT_TEST=ON
cmake --build build -j$(nproc)
```

### 최적화 빌드

```bash
cmake -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-O3 -march=native -flto" \
      -DOROT_DEFLATE_TESTS=ON
cmake --build build -j$(nproc)
```

### 디버그 빌드

```bash
cmake -B build \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-g -O0 -fsanitize=address" \
      -DOROT_DEFLATE_TESTS=ON
cmake --build build
```

---

## 단위 테스트

### 테스트 실행

모든 테스트 실행:
```bash
ctest --test-dir build --output-on-failure
```

특정 테스트만 실행:
```bash
ctest --test-dir build -R roundtrip --output-on-failure
ctest --test-dir build -R "roundtrip|formats" --output-on-failure
```

개별 테스트 실행:
```bash
./build/tests/unit/test_roundtrip
./build/tests/unit/test_formats
./build/tests/unit/test_levels
./build/tests/unit/test_streaming
./build/tests/unit/test_huffman
./build/tests/unit/test_parallel
./build/tests/unit/test_simd
```

### 테스트 설명

#### 1. test_roundtrip - 압축/해제 라운드트립

**목적:** 모든 데이터에 대해 압축 → 해제 후 원본 일치 확인

**테스트 범위:**
- 크기: 0 바이트 ~ 10 MB
- 형식: Raw, Zlib, Gzip
- 레벨: L0 ~ L12
- 데이터 패턴: zeros, repeating, random, text, mixed

**예상 결과:**
```
test_roundtrip ... passed
```

**실패 원인:**
- 압축 알고리즘 버그
- 형식 처리 오류
- 메모리 손상

#### 2. test_formats - 형식별 기능

**목적:** 각 형식(Raw/Zlib/Gzip)의 특성 검증

**테스트 항목:**
- Raw DEFLATE: 헤더 없음, 최소 크기
- Zlib: 헤더(2바이트), Adler-32 체크섬
- Gzip: 헤더(10바이트), CRC-32 체크섬

**예상 결과:**
```
test_formats ... passed
```

**검증:**
```
Raw  : size = data_size
Zlib : size = data_size + 2 + 4 (헤더 + 체크섬)
Gzip : size = data_size + 10 + 8 (헤더 + 체크섬)
```

#### 3. test_levels - 압축 레벨

**목적:** 모든 레벨(L0~L12)에서 정합성 확인

**테스트 항목:**
- L0: 저장만 (헤더 + 데이터, 압축 없음)
- L1: 최고 속도, 낮은 압축률
- L6: 기본 (속도-압축률 균형)
- L9: 높은 압축률
- L12: 최고 압축률

**예상 결과:**
```
All levels produce valid output
Level 12 ≤ Level 9 ≤ ... ≤ Level 1 (크기 기준)
```

**특징:**
- 레벨이 높을수록 압축률 증가
- 해제는 모든 레벨 동일 속도
- 메모리 사용량 증가

#### 4. test_streaming - 스트리밍 API

**목적:** 점진적 데이터 공급 시 정합성

**테스트 항목:**
- 청크 단위 공급 (다양한 크기)
- 부분 완료 상태 처리
- 최종 완료 처리
- 상태 관리

**예상 결과:**
```
test_streaming ... passed
```

**사용 시나리오:**
- 네트워크 스트림
- 파일 처리
- 메모리 제약 환경
- 대용량 데이터

#### 5. test_huffman - Huffman 인코딩

**목적:** Huffman 테이블 정확성 및 성능

**테스트 항목:**
- 테이블 생성 정확성
- 코드 길이 검증
- 인코딩 결과 확인

**예상 결과:**
```
test_huffman ... passed
```

**성능 지표:**
- 테이블 생성 시간
- 인코딩 시간
- 압축률

#### 6. test_parallel - 멀티스레드 압축

**목적:** 병렬 압축의 정합성 및 성능

**테스트 항목:**
- 스레드 수 변화 (1, 2, 4, 8)
- 블록 크기 변화 (64KB, 256KB, 1MB)
- 큰 파일 (10MB+)
- 속도-메모리 트레이드오프

**예상 결과:**
```
Parallel output matches single-threaded output
Speed up ≈ thread count (이상적)
Memory usage ∝ thread count
```

**성능 향상:**
- 2 스레드: ~1.8x
- 4 스레드: ~3.5x
- 8 스레드: ~6-7x

#### 7. test_simd - SIMD 가속

**목적:** SIMD 경로의 정합성 및 성능

**테스트 항목:**
- 스칼라 vs SSE2 비교
- 스칼라 vs SSE4.2 비교
- 스칼라 vs AVX2 비교
- 스칼라 vs NEON 비교 (ARM)

**예상 결과:**
```
test_simd ... passed
SIMD results match scalar results
Performance: SIMD > Scalar
```

**성능 향상:**
- SSE2/SSE4.2: 1.5-2x
- AVX2: 2-3x
- NEON (ARM): 2-3x

**SIMD 경로 확인:**
```bash
# 플랫폼별 지원 확인
./build/tests/unit/test_simd

# 또는 빌드 로그
cmake --build build -v 2>&1 | grep -i "simd\|sse\|avx\|neon"
```

---

## 호환성 테스트

### 빌드

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_DEFLATE_TESTS=ON \
      -DOROT_DEFLATE_COMPAT_TEST=ON
cmake --build build -j$(nproc)
```

### 실행

```bash
# 모든 호환성 테스트
ctest --test-dir build -R compat --output-on-failure

# 또는 개별 실행
cd build/tests/compat
./compat_orot_zlib
./compat_orot_libdeflate
./compat_roundtrip
```

### 테스트 범위

1. **orot ↔ zlib 호환성**
   - orot 압축 → zlib 해제
   - zlib 압축 → orot 해제
   - 동일 데이터 검증

2. **orot ↔ libdeflate 호환성**
   - orot 압축 → libdeflate 해제
   - libdeflate 압축 → orot 해제
   - 동일 데이터 검증

3. **형식 호환성**
   - Raw DEFLATE
   - Zlib (Adler-32)
   - Gzip (CRC-32)

4. **레벨 호환성**
   - L1 ~ L9
   - 각 레벨 정확성 확인

5. **데이터 호환성**
   - 소규모 (1 바이트)
   - 중규모 (1 MB)
   - 대규모 (100 MB+)

---

## 성능 벤치마크

### 단일 라이브러리 벤치마크

```bash
./build/tests/bench_compress [iterations]
```

**기본 반복:** 100
**권장 반복:** 100-500 (더 정확한 측정)

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
random (1 MB)                      6        980.2          1030.0     101.20%
random (1 MB)                      9        150.5          1020.5     101.20%
```

**메트릭 이해:**
- `compress MB/s`: 입력 데이터 처리 속도
- `decompress MB/s`: 해제 속도 (항상 더 빠름)
- `ratio`: 압축률 (100% = 압축 안됨)

### 비교 벤치마크 (zlib, libdeflate와 비교)

```bash
# 필수: zlib, libdeflate 설치
brew install zlib libdeflate  # macOS
# 또는
sudo apt-get install zlib1g-dev libdeflate-dev  # Ubuntu

# 빌드
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_DEFLATE_TESTS=ON \
      -DOROT_DEFLATE_BENCH=ON \
      -DOROT_DEFLATE_COMPARE_BENCH=ON
cmake --build build -j$(nproc)

# 실행
./build/tests/bench_compare [iterations]
```

**출력 예시:**

```
Compressor          Dataset      Level  MB/s      Ratio
────────────────────────────────────────────────────────
orot                text         1      2456      52%
zlib                text         1      2100      52%
libdeflate          text         1      2300      52%

orot                text         6      892       38%
zlib                text         6      650       38%
libdeflate          text         6      780       38%
```

---

## 성능 최적화

### 1. 빌드 최적화

```bash
# 릴리스 빌드 (필수)
cmake -DCMAKE_BUILD_TYPE=Release ..

# 침략적 최적화
cmake -DCMAKE_CXX_FLAGS="-O3 -march=native -flto" ..

# SIMD 활성화 (기본)
cmake -DOROT_DEFLATE_SIMD=ON ..

# 스레딩 활성화 (기본)
cmake -DOROT_DEFLATE_THREADS=ON ..
```

### 2. 레벨별 선택

```cpp
// 실시간: L1-L3 (속도 우선)
deflate_compress(data, len, out, cap, 1, fmt);

// 일반: L6 (균형)
deflate_compress(data, len, out, cap, 6, fmt);

// 아카이브: L9-L12 (압축률 우선)
deflate_compress(data, len, out, cap, 9, fmt);
```

### 3. 병렬 압축 활용

```cpp
#include <orot/deflate.h>

// 멀티스레드 압축 (자동 스레드 수)
orot::deflate::ParallelCompressor pc(
    orot::deflate::Level::Default,
    orot::deflate::Format::Zlib);

auto compressed = pc.compress(data_span);
```

**성능 향상:**
- 2 코어: 1.8-1.9x
- 4 코어: 3.5-3.8x
- 8 코어: 6-7x

### 4. 스트리밍 사용

```cpp
// 대용량 파일 처리 시 메모리 절약
orot::deflate::Compressor compressor(
    orot::deflate::Level::Default,
    orot::deflate::Format::Zlib);

while (읽기(chunk)) {
    compressor.feed(chunk, output);
    write_output(output);
}
compressor.finish(output);
```

### 5. 데이터 사전 처리

```cpp
// 데이터 특성에 맞는 레벨 선택
if (is_repetitive(data)) {
    level = 1;  // 빠른 처리
} else if (is_random(data)) {
    level = 6;  // 균형
}
```

---

## 프로파일링

### macOS (Instruments)

```bash
# CPU 프로파일링
instruments -t "System Trace" ./build/tests/bench_compress

# Allocations 프로파일링
instruments -t "Allocations" ./build/tests/bench_compress
```

### Linux (perf)

```bash
# CPU 프로파일링
perf record -g ./build/tests/bench_compress 50
perf report

# 불릿 그래프
perf record -g ./build/tests/bench_compress
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### Valgrind (메모리)

```bash
# 메모리 누수 감지
valgrind --leak-check=full --show-leak-kinds=all \
         ./build/tests/unit/test_roundtrip

# 캐시 분석
valgrind --tool=cachegrind ./build/tests/bench_compress
```

### gprof (GCC)

```bash
# 프로파일링 활성화로 빌드
cmake -DCMAKE_CXX_FLAGS="-pg" ..
cmake --build .

# 실행
./build/tests/bench_compress

# 분석
gprof ./build/tests/bench_compress gmon.out | less
```

---

## 문제 해결

### 테스트 실패

#### "decompress size mismatch"
```
원인: 압축/해제 로직 버그
진단:
  1. 압축 데이터 크기 확인
  2. 해제 버퍼 크기 확인
  3. 체크섬 검증
조사: src/decompress/decompressor.cpp
```

#### "checksum mismatch"
```
원인: Adler-32 또는 CRC-32 계산 오류
진단:
  1. 형식 확인 (Zlib? Gzip?)
  2. 체크섬 함수 확인
조사: src/formats/zlib_wrapper.cpp 또는 gzip_wrapper.cpp
```

#### "invalid compressed data"
```
원인: 손상된 데이터 또는 형식 불일치
진단:
  1. 압축 데이터 유효성 확인
  2. 형식 확인 (Raw? Zlib? Gzip?)
  3. 해제 버퍼 크기 확인
해결: 원본 데이터부터 재시작
```

### 성능 저하

#### "압축이 느림 (MB/s 낮음)"
```
원인 1: Release 빌드 아님
해결: cmake -DCMAKE_BUILD_TYPE=Release

원인 2: SIMD 비활성화
확인: cmake -DOROT_DEFLATE_SIMD=ON
테스트: ./build/tests/unit/test_simd

원인 3: 레벨이 너무 높음
조정: L6 또는 L9 대신 L1 또는 L3 사용

원인 4: 컴파일러 최적화 부족
개선: cmake -DCMAKE_CXX_FLAGS="-O3 -march=native"
```

#### "해제가 느림 (해제 MB/s 낮음)"
```
원인: 드물지만, 보통은 정상

확인:
  1. Release 빌드 확인
  2. 컴파일러 버전 확인 (GCC 10+, Clang 12+)
  3. 백그라운드 프로세스 확인

프로파일링:
  perf record ./build/tests/bench_compress
  perf report
```

#### "메모리 사용량 많음"
```
원인: 고레벨 압축 또는 병렬 처리

레벨 낮추기:
  L12 → L9
  L9 → L6

병렬 스레드 줄이기:
  ParallelCompressor(level, format, 2)  // 2 스레드
```

### 빌드 오류

#### "C++20 지원 안 됨"
```bash
# 컴파일러 업그레이드
gcc --version  # GCC 10+ 필요
clang --version  # Clang 12+ 필요
```

#### "zlib/libdeflate 라이브러리 없음"
```bash
# macOS
brew install zlib libdeflate

# Ubuntu
sudo apt-get install zlib1g-dev libdeflate-dev

# 또는 비교 벤치마크 비활성화
cmake -DOROT_DEFLATE_COMPARE_BENCH=OFF ..
```

#### "CMake 3.20 미만"
```bash
# CMake 업그레이드
brew upgrade cmake  # macOS
sudo apt-get upgrade cmake  # Ubuntu
```

---

## CI/CD 통합

### GitHub Actions

```yaml
name: DEFLATE Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        compiler: [gcc, clang]
        build-type: [Release, Debug]
    steps:
      - uses: actions/checkout@v3
      
      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y zlib1g-dev libdeflate-dev
      
      - name: Build
        run: |
          cmake -B build \
            -DCMAKE_BUILD_TYPE=${{ matrix.build-type }} \
            -DOROT_DEFLATE_TESTS=ON \
            -DOROT_DEFLATE_BENCH=ON \
            -DOROT_DEFLATE_COMPARE_BENCH=ON
          cmake --build build -j4
      
      - name: Run tests
        run: ctest --test-dir build --output-on-failure
      
      - name: Benchmark
        if: matrix.build-type == 'Release'
        run: ./build/tests/bench_compress 100
```

### GitLab CI

```yaml
test:deflate:
  image: ubuntu:22.04
  before_script:
    - apt-get update && apt-get install -y
        cmake g++ clang zlib1g-dev libdeflate-dev
  script:
    - cmake -B build -DCMAKE_BUILD_TYPE=Release
        -DOROT_DEFLATE_TESTS=ON -DOROT_DEFLATE_BENCH=ON
    - cmake --build build -j4
    - ctest --test-dir build --output-on-failure
```

### Jenkins

```groovy
pipeline {
  agent any
  
  stages {
    stage('Build') {
      steps {
        sh '''
          cmake -B build -DCMAKE_BUILD_TYPE=Release \
            -DOROT_DEFLATE_TESTS=ON -DOROT_DEFLATE_BENCH=ON
          cmake --build build -j4
        '''
      }
    }
    
    stage('Test') {
      steps {
        sh 'ctest --test-dir build --output-on-failure'
      }
    }
    
    stage('Benchmark') {
      steps {
        sh './build/tests/bench_compress 100'
      }
    }
  }
}
```

---

## 성능 회귀 감지

### 자동 감지 스크립트

```bash
#!/bin/bash
# check_regression.sh

set -e

BASELINE="${1:-baseline.txt}"
CURRENT="current.txt"

if [ ! -f "$BASELINE" ]; then
    echo "Creating baseline..."
    ./build/tests/bench_compress 200 > "$BASELINE"
    echo "Baseline created: $BASELINE"
    exit 0
fi

echo "Running current benchmark..."
./build/tests/bench_compress 200 > "$CURRENT"

echo "Comparing results..."
echo "═════════════════════════════════════════════"

# 압축 처리량 비교
echo "Compression Throughput Comparison:"
grep "compress MB/s" "$BASELINE" > baseline_comp.txt
grep "compress MB/s" "$CURRENT" > current_comp.txt

paste baseline_comp.txt current_comp.txt | while read baseline current; do
    baseline_speed=$(echo "$baseline" | awk '{print $NF}')
    current_speed=$(echo "$current" | awk '{print $NF}')
    
    if (( $(echo "$current_speed < $baseline_speed * 0.9" | bc -l) )); then
        echo "⚠️  REGRESSION: $baseline_speed → $current_speed MB/s"
    else
        echo "✓ OK: $baseline_speed → $current_speed MB/s"
    fi
done

rm -f baseline_comp.txt current_comp.txt
echo "═════════════════════════════════════════════"
```

---

## 성능 기준 유지

### 목표 성능 (MB/s)

| 시나리오 | L1 | L3 | L6 | L9 | L12 |
|---------|-----|-----|-----|-----|------|
| **Text** | 2000+ | 1500+ | 800+ | 400+ | 200+ |
| **Zeros** | 3000+ | 2500+ | 1500+ | 500+ | 200+ |
| **Random** | 1000+ | 900+ | 800+ | 150+ | 50+ |

### 압축률 기준 (%)

| 시나리오 | L1 | L6 | L9 | L12 |
|---------|-----|-----|-----|------|
| **Zeros** | 0.3% | 0.3% | 0.3% | 0.3% |
| **Text** | 50% | 35% | 30% | 28% |
| **Random** | 100% | 100% | 100% | 100% |

---

## 추가 리소스

- **빠른 시작:** [DEFLATE_QUICK_START.md](DEFLATE_QUICK_START.md)
- **검증 보고서:** [DEFLATE_VALIDATION_REPORT.md](DEFLATE_VALIDATION_REPORT.md)
- **RFC 1951:** https://tools.ietf.org/html/rfc1951 (DEFLATE)
- **RFC 1950:** https://tools.ietf.org/html/rfc1950 (Zlib)
- **RFC 1952:** https://tools.ietf.org/html/rfc1952 (Gzip)

---

**마지막 업데이트:** 2024년
**상태:** ✅ Complete
