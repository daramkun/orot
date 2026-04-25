# Bzip2 구현 문서

## 완료 항목

| 항목 | 상태 |
|------|------|
| RLE1 인코딩/디코딩 | ✅ |
| BWT (Burrows-Wheeler Transform) | ✅ |
| MTF (Move-to-Front) | ✅ |
| RLE2 (RUNA/RUNB) | ✅ |
| 다중 Huffman 테이블 (2~6개) | ✅ |
| Selector MTF 인코딩 | ✅ |
| MSB-first CRC32 (256-entry 테이블) | ✅ |
| C API (`orot_bzip2_compress/decompress`) | ✅ |
| C API 병렬 압축 (`orot_bzip2_compress_parallel`) | ✅ |
| C++ 편의 API | ✅ |
| 라운드트립 테스트 | ✅ |
| 단일 벤치마크 | ✅ |
| 비교 벤치마크 (libbz2) | ✅ |

## 구현 개요

### 알고리즘 파이프라인

**압축 (5단계)**
1. **RLE1** — 4회 이상 연속 바이트 → 4바이트 + count(0~255). 최대 run = 259.
2. **BWT** — counting sort 기반 prefix doubling O(n log n). primary index 저장.
3. **MTF** — in-use 바이트 알파벳만 사용 (전체 256 아님). rank==0 fast path.
4. **RLE2** — 0 rank run → RUNA(bit 0)/RUNB(bit 1), LSB-first 이진 인코딩.
5. **Huffman** — 2~6개 테이블, 50심볼마다 selector 전환. iterative k-means 최적화.

**압축 해제 (역순)**
Huffman → RLE2 → MTF-1 → BWT-1 → RLE1 → 원본

### 핵심 설계 결정

| 결정 | 이유 |
|------|------|
| MTF를 in-use 알파벳으로 제한 | bzip2 표준; 전체 256 사용 시 rank가 n_in_use 초과 |
| Huffman 트리를 alpha_size 기준으로 빌드 | BZ_MAX_ALPHA_SIZE(258)로 빌드 시 코드 불일치 |
| RLE1 decode에서 run_cnt 리셋 | count byte 소비 후 카운터 초기화 필수 |
| BitReader/Writer를 64-bit buf로 | 바이트 단위 bulk 처리로 per-bit 루프 제거 |
| BWT에 counting sort | std::sort O(n log n per iter) → counting sort O(n per iter) |
| 병렬 압축에 bit-merge | bzip2 블록 비바이트정렬, 블록 독립 압축 후 비트 결합 |
| thread_local rank/cnt 버퍼 | 병렬 BWT 시 스레드별 작업 공간 분리 |

### Magic Numbers

- Stream header: `BZh` + level('1'~'9')
- Block start: `0x314159265359` (48-bit)
- Stream end: `0x177245385090` (48-bit)

### CRC32

bzip2 전용 MSB-first CRC32. gzip/zlib CRC32와 동일 다항식(0x04C11DB7)이지만 reflection 방식이 다름 — `deflate_crc32()` 재사용 불가.

256-entry constexpr 테이블 사용 (컴파일 타임 생성):
```
crc = (crc << 8) ^ table[(crc >> 24) ^ byte]
```

블록당 CRC + 스트림 통합 CRC:
```
combined = (combined << 1 | combined >> 31) ^ block_crc
```

### Level 의미

LZ4/LZMA와 다름. Level = 블록 크기 배수:
- L1: 100 KB 블록
- L5: 500 KB 블록
- L9: 900 KB 블록 (기본, 최고 압축)

### 병렬 압축

`orot_bzip2_compress_parallel`: 블록별 독립 압축 후 bit-merge.

```
1. 입력 → N블록 분할
2. ThreadPool: 각 블록 독립 압축 (thread_local BWT 버퍼)
3. bit-merge: header + block0_bits + block1_bits + ... + footer
4. combined CRC = crc32_combine(block_crcs...) — 직렬
```

bzip2 블록은 바이트 경계 없음 → 병렬 압축 결과를 직접 비트 레벨에서 결합.

## 파일 구조

```
src/bzip2/
  bzip2_crc.hpp          # MSB-first CRC32 (constexpr 256-entry table)
  bwt.{cpp,hpp}          # counting sort prefix doubling + BWT/IBWT
  mtf.hpp                # Move-to-Front (rank==0 fast path)
  bzip2_huffman.{cpp,hpp}# 다중 Huffman 테이블 + selector (64-bit decode buf)
  bzip2_compress.{cpp,hpp}   # 5단계 압축 파이프라인 (64-bit BitWriter)
  bzip2_decompress.{cpp,hpp} # 5단계 압축해제 (64-bit BitReader, direct dst write)
src/api/
  bzip2_api.cpp          # C API 래퍼
include/orot/
  bzip2.h                # C/C++ 공개 API
```

## C API

```c
size_t orot_bzip2_compress_bound(size_t src_size);

// 반환: 양수=바이트 수, -1=에러, -2=버퍼 부족, -3=데이터 에러
int orot_bzip2_compress(const void* src, size_t src_size,
                        void* dst, size_t dst_cap, int level);

int orot_bzip2_compress_parallel(const void* src, size_t src_size,
                                  void* dst, size_t dst_cap,
                                  int level, int n_threads); /* 0=hw_concurrency */

int orot_bzip2_decompress(const void* src, size_t src_size,
                          void* dst, size_t dst_cap,
                          size_t* uncompressed_size_out);
```

## 빌드 & 테스트

```bash
# 빌드
cmake -B build -DOROT_TESTS=ON -DOROT_BENCHMARK=ON
cmake --build build -j

# 단위 테스트
./build/tests/test_bzip2

# 벤치마크
./build/tests/bench_bzip2
./build/tests/bench_bzip2 5   # 5회 반복

# 비교 벤치마크 (libbz2 필요)
cmake -B build -DOROT_BENCHMARK_COMPARE=ON
cmake --build build --target bench_bzip2_compare
./build/tests/bench_bzip2_compare 5
```

## 성능 결과 (Apple Silicon, iters=10, libbz2 비교)

| 데이터 | orot 압축 | libbz2 압축 | speedup | orot 해제 | libbz2 해제 | speedup |
|--------|-----------|-------------|---------|-----------|-------------|---------|
| zeros 1MB | ~120 MB/s | ~93 MB/s | **1.3x** | ~155 MB/s | ~1360 MB/s | 0.11x |
| random 1MB | ~6 MB/s | ~8 MB/s | 0.75x | ~20 MB/s | ~18 MB/s | **1.1x** |
| text 90KB | ~3 MB/s | ~7 MB/s | 0.4x | ~68 MB/s | ~180 MB/s | 0.4x |
| code 512KB | ~2.5 MB/s | ~5 MB/s | 0.5x | ~56 MB/s | ~160 MB/s | 0.35x |

## 알려진 제한

- Randomized block (`rnd_flag=1`) 미지원 (사용 빈도 매우 낮음)
- zeros 해제 속도: libbz2가 trivial Huffman 코드에 특화 최적화 보유, orot 0.11x
- SIMD 가속 없음 (MSB-first CRC32는 표준 hw 명령 직접 사용 불가)
