# LZMA/LZMA2 구현 상세

## 완료 항목

| 작업 | 상태 |
|------|------|
| LZMA alone 포맷 압축/해제 구현 | ✅ |
| LZMA2 청크 스트림 압축/해제 구현 | ✅ |
| Range 코딩 엔진 (range_coder.hpp) | ✅ |
| 확률 모델 + 상태기계 (lzma_prob_model.hpp) | ✅ |
| C API 진입점 (lzma_api.cpp) | ✅ |
| 단위 테스트 (test_lzma.cpp) | ✅ |
| 벤치마크 (bench_lzma, bench_lzma_compare) | ✅ |

---

## 구현 개요

orot의 LZMA 구현은 범위 코딩(range coding) 기반의 Lempel-Ziv-Markov chain 압축이다.

### 압축 레벨

| 레벨 | 딕셔너리 크기 | nice_len | 설명 |
|------|-------------|----------|------|
| L1   | 256KB       | 16       | 최고속 |
| L2   | 512KB       | 20       | — |
| L3   | 1MB         | 24       | — |
| L4   | 2MB         | 32       | — |
| L5   | 4MB         | 32       | 기본값 |
| L6   | 8MB         | 48       | — |
| L7   | 16MB        | 48       | — |
| L8   | 16MB        | 64       | — |
| L9   | 32MB        | 64       | 최고 압축률 |

### 범위 코딩

- 정밀도: 11비트 확률 테이블 (`kProbBits = 11`, `kProbTotal = 2048`)
- 정규화 임계값: `range < (1u << 24)` 시 바이트 시프트
- 확률 갱신 속도: `kMoveBits = 5`
- 엔코더 플러시: 5바이트 종료 마커

### LZMA 상태기계

- 상태 12개 (리터럴/매치/rep 기반 전이)
- rep 거리 4개 유지 (LZMA 표준)
- 리터럴: 이전 매치 바이트 예측(matched literal)
- 길이: low(3b) + mid(3b) + high(8b) = 최대 273

### LZMA alone 포맷

```
[1 byte: props (lc/lp/pb 인코딩)]
[4 bytes: dict_size (LE)]
[8 bytes: uncompressed_size (LE, 0xFFFFFFFFFFFFFFFF = 미지정)]
[RC data ...]
[end-of-stream 마커 (dist = 0xFFFFFFFF)]
```

### LZMA2 청크 스트림

- 64KB 단위 독립 청크
- 청크 헤더 타입: `0x80|flags` (LZMA), `0x01/0x02` (비압축), `0x00` (EOS)
- 압축 후 크기가 원본 이상이면 비압축 청크로 폴백
- 최대 딕셔너리 32MB 고정 할당 (청크 간 재사용)

---

## 파일 구조

```
include/orot/lzma.h             # C/C++ API 헤더
src/lzma/
├── range_coder.hpp             # 범위 인코더/디코더
├── lzma_prob_model.hpp         # 확률 테이블 + 상태기계 + 길이/거리 코딩
├── lzma_compress.hpp           # 내부 압축 API
├── lzma_compress.cpp           # LZMA alone 압축 구현
├── lzma_decompress.hpp         # 내부 압축해제 API
├── lzma_decompress.cpp         # LZMA alone 압축해제 구현
├── lzma2_compress.cpp          # LZMA2 청크 스트림 압축
└── lzma2_decompress.cpp        # LZMA2 청크 스트림 압축해제
src/api/lzma_api.cpp            # C API 진입점
tests/unit/
└── test_lzma.cpp               # 라운드트립 + 엣지 케이스
tests/bench/
├── bench_lzma.cpp              # 단일 라이브러리 벤치마크
└── bench_lzma_compare.cpp      # liblzma 비교 벤치마크
```

---

## C API

```c
// lzma.h

// 압축 출력 크기 상한
size_t orot_lzma_compress_bound(size_t src_size);
size_t orot_lzma2_compress_bound(size_t src_size);

// LZMA alone 압축 (level: 1~9)
// 반환: 기록된 바이트 수, -1: 오류, -2: 버퍼 부족
int orot_lzma_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         level);

// LZMA alone 압축해제
// uncompressed_size_out: 헤더에서 읽은 원본 크기 (NULL 허용)
// 반환: 기록된 바이트 수, -1: 오류, -2: 버퍼 부족, -3: 데이터 오류
int orot_lzma_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    size_t*     uncompressed_size_out);

// LZMA2 청크 스트림 압축 (level: 1~9)
int orot_lzma2_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         level);

// LZMA2 청크 스트림 압축해제
int orot_lzma2_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap);
```

## C++ 편의 API

```cpp
// namespace orot::lzma_api

// LZMA alone
Container compress(std::span<const uint8_t> src, int level = 5);
Container decompress(std::span<const uint8_t> src, size_t max_size = 0);

// LZMA2
Container compress2(std::span<const uint8_t> src, int level = 5);
Container decompress2(std::span<const uint8_t> src, size_t dst_cap);
```

---

## 빌드 및 검증

```bash
# LZMA 단위 테스트
cmake -B build -DOROT_TESTS=ON
cmake --build build -j
./build/tests/test_lzma

# LZMA 벤치마크
cmake -B build -DOROT_BENCHMARK=ON
cmake --build build -j
./build/tests/bench_lzma

# liblzma 비교 벤치마크
cmake -B build -DOROT_BENCHMARK_COMPARE=ON
cmake --build build -j
./build/tests/bench_lzma_compare
```
