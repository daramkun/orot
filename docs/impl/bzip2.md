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
| MSB-first CRC32 (bzip2 전용) | ✅ |
| C API (`orot_bzip2_compress/decompress`) | ✅ |
| C++ 편의 API | ✅ |
| 라운드트립 테스트 | ✅ |
| 단일 벤치마크 | ✅ |
| 비교 벤치마크 (libbz2) | ✅ |

## 구현 개요

### 알고리즘 파이프라인

**압축 (5단계)**
1. **RLE1** — 4회 이상 연속 바이트 → 4바이트 + count(0~255). 최대 run = 259.
2. **BWT** — prefix doubling O(n log² n) suffix sort. primary index 저장.
3. **MTF** — in-use 바이트 알파벳만 사용 (전체 256 아님).
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
| BitReader buf를 Huffman decode에 공유 | 헤더 파싱 후 남은 비트 손실 방지 |

### Magic Numbers

- Stream header: `BZh` + level('1'~'9')
- Block start: `0x314159265359` (48-bit)
- Stream end: `0x177245385090` (48-bit)

### CRC32

bzip2 전용 MSB-first CRC32 구현. gzip/zlib CRC32와 동일 다항식(0x04C11DB7)이지만 reflection 방식이 다름 — `deflate_crc32()` 재사용 불가.

블록당 CRC + 스트림 통합 CRC:
```
combined = (combined << 1 | combined >> 31) ^ block_crc
```

### Level 의미

LZ4/LZMA와 다름. Level = 블록 크기 배수:
- L1: 100 KB 블록
- L5: 500 KB 블록
- L9: 900 KB 블록 (기본, 최고 압축)

## 파일 구조

```
src/bzip2/
  bzip2_crc.hpp          # MSB-first CRC32 (inline)
  bwt.{cpp,hpp}          # prefix doubling suffix sort + BWT/IBWT
  mtf.hpp                # Move-to-Front (in-use 알파벳)
  bzip2_huffman.{cpp,hpp}# 다중 Huffman 테이블 + selector
  bzip2_compress.{cpp,hpp}   # 5단계 압축 파이프라인
  bzip2_decompress.{cpp,hpp} # 5단계 압축해제 파이프라인
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

## 성능 결과 (Apple M 계열, 1 MiB, iters=1)

| 데이터 | L1 압축 | L9 압축 | L1 해제 | 압축률 |
|--------|---------|---------|---------|--------|
| zeros | ~11 MB/s | ~9 MB/s | ~5 MB/s | ~0% |
| pattern-7 | ~0.3 MB/s | ~0.3 MB/s | ~3 MB/s | ~0% |
| sequential | ~0.4 MB/s | ~0.3 MB/s | ~3 MB/s | ~1% |
| random | ~0.7 MB/s | ~0.7 MB/s | ~2 MB/s | ~100% |

> **주의:** BWT suffix sort가 O(n log² n) prefix doubling 방식으로 반복 데이터에서 느림. 고성능 필요 시 SA-IS(O(n))으로 교체 권장.

## 알려진 제한

- Randomized block (`rnd_flag=1`) 미지원 (사용 빈도 매우 낮음)
- BWT suffix sort O(n log² n): 대용량 블록에서 참조 구현 대비 느림
- SIMD 가속 없음 (CRC32, Huffman 등)
