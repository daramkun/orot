# DEFLATE 라이브러리 구현 계획

## 완료 현황

| 작업 | 상태 |
|------|------|
| CMake + 타입 + 아레나 + BitWriter/Reader | ✅ |
| 스칼라 허프만 (Package-Merge) + LZ77 (레벨 0-12) | ✅ |
| DEFLATE 블록 타입 선택 (stored/fixed/dynamic) | ✅ |
| 스트리밍 압축기 (Compressor) | ✅ |
| 스트리밍 압축해제기 (Decompressor, 상태머신) | ✅ |
| zlib/gzip 래퍼 (whole-buffer) | ✅ |
| ARM NEON + CRC32 SIMD | ✅ |
| 병렬 압축기 (pigz 방식) | ✅ |
| C API + C++ RAII API | ✅ |
| 단위 테스트 7/7 통과 | ✅ |
| Fuzz 테스트 (버그 3개 수정) | ✅ |
| 단일 라이브러리 벤치마크 | ✅ |
| 비교 벤치마크 (ours vs zlib vs libdeflate) | ✅ |
| 교차 호환성 테스트 (압축/해제 라이브러리 교환) | ✅ |
| 스트리밍 ZLIB/GZIP 포맷 지원 | ✅ |
| LZ77 압축 속도 최적화 (L1 fast path + hash_bits) | ✅ |
| 압축해제 copy_match 최적화 (doubling memcpy) | ✅ |
| inflate_fast safe zone literal bounds check 제거 | ✅ |
| encode_block estimate 중복 제거 (build_huffman_lengths 2→1회) | ✅ |
| inflate_fast 연결 (decompressor 상태머신 → inflate_fast fast path) | ✅ |
| COPY_MATCH 벌크 복사 (circular window bulk memcpy, dist==1 memset) | ✅ |
| LZ77 match_find() best_len 빠른 거부 체크 (SIMD 호출 80-90% 절감) | ✅ |
| STORED_COPY window 업데이트 bulk 전환 (바이트 루프 → circular memcpy) | ✅ |
| LZ77 match_find() 적응형 비교 폭 (best_len≥4→uint32, ≥8→uint64) | ✅ |

---

## 발견 및 수정된 버그

1. **`decompressor.cpp`: distance code OOB** — `di >= 30` 시 `DIST_EXTRA_BITS[di]` 배열 초과.
   → `di >= 30` 검사 후 `DEFLATE_DATA_ERROR` 반환.

2. **`lz77.cpp`: `lz77_hash4` 4-byte OOB** — 입력 끝 3바이트 미만에서 4바이트 읽기.
   → `pos + 4 > src_len` 시 hash 삽입/검색 건너뜀. `lz77_insert_dict`도 동일.

3. **`deflate_block.cpp`: `estimate_dynamic_bits` 과소평가** — code-length 테이블 RLE 비용 무시
   → 작은 입력에서 dynamic Huffman 잘못 선택 → `deflate_compress_bound` 초과.
   → `3 * (LITLEN_SYMS + DIST_SYMS)` bits 추가.

---

## 벤치마크 결과 (스칼라 + ARM NEON, Apple M-series)

| 데이터셋 | 레벨 | 포맷 | 압축 MB/s | 압축해제 MB/s |
|---------|------|------|-----------|------------|
| 텍스트 (~90KB) | 1 | raw | 115 | 119 |
| 텍스트 (~90KB) | 6 | raw | 54 | 122 |
| zeros (1MB) | 1 | raw | 59 | 122 |
| 랜덤 (1MB) | 1 | raw | 10 | 209 |
| 랜덤 (1MB) | 6 | raw | 1.4 | 190 |

---

## LZ77 압축 속도 최적화 (L1 fast path)

**변경 내용** (`src/core/lz77.cpp`, `src/core/lz77.hpp`):
- L1-L3: `match_find_fast()` — head[] 단일 조회만, chain traversal 없음, prev[] 업데이트 생략
- L1-L3: match 후 covered positions hash insert loop 생략
- 런타임 hash_bits: L1=12bit(4KB), L2-L3=14bit(32KB), L4+=16bit(128KB)
- 단위 테스트 7/7, 교차 호환성 126/126 회귀 없음

| 데이터셋 | 이전 L1 | 이후 L1 | 개선 |
|---------|--------|--------|------|
| text (~90KB) | 79 MB/s | 176 MB/s | +2.2x |
| zeros (1MB) | 50 MB/s | 75 MB/s | +1.5x |
| random (1MB) | 10 MB/s | 28 MB/s | +2.8x |
| code (~512KB) | 33 MB/s | 50 MB/s | +1.5x |

L6 속도 변화 없음 (±2% 노이즈 범위).  
Software prefetch는 Apple M-series 하드웨어 prefetcher와 충돌하여 제거.

---

## 비교 벤치마크 결과 (Apple M-series, 50 iters, ZLIB 포맷)

### 최적화 전

| 라이브러리 | 데이터셋 | 레벨 | 압축 MB/s | 압축해제 MB/s | 압축률% |
|-----------|---------|------|-----------|-------------|--------|
| ours | text (~90KB) | fast | 79.1 | 87.6 | 2.1% |
| zlib | text (~90KB) | fast | 962.0 | 4503.2 | 0.7% |
| libdeflate | text (~90KB) | fast | 872.0 | 3478.4 | 0.4% |
| ours | text (~90KB) | default | 44.8 | 88.5 | 0.9% |
| zlib | text (~90KB) | default | 362.0 | 3227.3 | 0.4% |
| libdeflate | text (~90KB) | default | 622.2 | 3985.2 | 0.4% |
| ours | zeros (1MB) | fast | 50.0 | 93.7 | 0.6% |
| zlib | zeros (1MB) | fast | 624.3 | 6321.3 | 0.4% |
| libdeflate | zeros (1MB) | fast | 977.1 | 7691.6 | 0.1% |
| ours | random (1MB) | fast | 10.3 | 124.8 | 100.0% |
| zlib | random (1MB) | fast | 38.8 | 8199.2 | 100.0% |
| libdeflate | random (1MB) | fast | 96.4 | 18305.2 | 100.0% |

### 최적화 후 3차 (STORED_COPY bulk + LZ77 적응형 비교폭)

| 라이브러리 | 데이터셋 | 레벨 | 압축 MB/s | 압축해제 MB/s | 압축률% |
|-----------|---------|------|-----------|-------------|--------|
| ours | text (~90KB) | fast | 680.0 | 1075.4 | 2.1% |
| zlib | text (~90KB) | fast | 826.5 | 5900.4 | 0.7% |
| libdeflate | text (~90KB) | fast | 810.3 | 3556.0 | 0.4% |
| ours | text (~90KB) | default | 264.0 | 1156.1 | 0.9% |
| zlib | text (~90KB) | default | 364.0 | 3066.1 | 0.4% |
| libdeflate | text (~90KB) | default | 626.0 | 4066.8 | 0.4% |
| ours | zeros (1MB) | fast | 1357.4 | 1684.5 | 1.0% |
| zlib | zeros (1MB) | fast | 627.9 | 6035.2 | 0.4% |
| libdeflate | zeros (1MB) | fast | 967.8 | 7846.4 | 0.1% |
| ours | random (1MB) | fast | 330.7 | 1733.3 | 100.0% |
| zlib | random (1MB) | fast | 38.5 | 8061.6 | 100.0% |
| libdeflate | random (1MB) | fast | 95.1 | 18322.6 | 100.0% |

#### 압축 개선 요약 (vs 최초 기준)

| 데이터셋 | 이전 | 이후 | 개선 |
|---------|------|------|------|
| text fast comp | 79.1 MB/s | 680.0 MB/s | +8.6x |
| text fast decomp | 87.8 MB/s | 1075.4 MB/s | +12.2x |
| zeros fast comp | 50.0 MB/s | 1357.4 MB/s | +27.1x |
| zeros fast decomp | 93.7 MB/s | 1684.5 MB/s | +18.0x |
| random fast comp | 10.3 MB/s | 330.7 MB/s | +32.1x |
| random fast decomp | 124.8 MB/s | 1733.3 MB/s | +13.9x |

> 압축률%: compressed/original×100 (낮을수록 좋음). 랜덤 데이터는 압축 불가(100%).  
> 주요 변경: inflate_fast 연결 (decomp 12x), STORED_COPY bulk (random decomp 13.9x), LZ77 best_len+적응형 비교.  
> 잔존 격차: Huffman decomp zlib 대비 3-4x (random/stored는 4.6x).

---

## 교차 호환성 결과

126/126 PASS (ZLIB×54 + GZIP×54 + RAW×18, 3개 라이브러리 × 6조합 × 3레벨 × 3데이터셋)

---

## 작업 1: 비교 벤치마크

**파일**: `tests/bench/bench_compare.cpp`  
**빌드 옵션**: `-DDEFLATE_COMPARE_BENCH=ON` (zlib + libdeflate 필요)

### 측정 지표
- 압축/압축해제 처리량 (MB/s)
- 압축률 (%)
- CPU 시간 (`CLOCK_PROCESS_CPUTIME_ID`)
- 메모리 RSS delta (`getrusage`)

### 비교 라이브러리
| 라이브러리 | 버전 | 레벨 매핑 |
|-----------|------|---------|
| ours | — | 1 / 6 / 9 |
| zlib | 내장 | 1 / 6 / 9 |
| libdeflate | 1.25 (Homebrew) | 1 / 6 / 9 |

---

## 작업 2: 교차 호환성 테스트

**파일**: `tests/compat/test_compat.cpp`  
**빌드 옵션**: `-DDEFLATE_COMPAT_TEST=ON` (zlib + libdeflate 필요)

### 테스트 매트릭스
압축 × 압축해제 × 포맷(ZLIB, GZIP) 전체 조합.

---

## 작업 3: 스트리밍 ZLIB/GZIP 포맷

**파일**: `src/api/stream_api.cpp` — 완료

Phase 상태머신 (HEADER→DATA→TRAILER→DONE) 구현:
- 압축: 포맷 헤더 출력 → raw 압축 + checksum 누적 → 트레일러 출력
- 압축해제: 헤더 파싱/검증 → raw 압축해제 + checksum 누적 → 트레일러 검증
- ZLIB: adler32 (big-endian 4B trailer)
- GZIP: crc32 + isize (little-endian 8B trailer), 가변길이 헤더 파싱 포함

---

## 검증 명령어

```bash
# 단위 테스트
ctest --test-dir build --output-on-failure

# 비교 벤치마크
cmake -B build -DDEFLATE_TESTS=ON -DDEFLATE_COMPARE_BENCH=ON
cmake --build build -j
./build/tests/bench_compare

# 호환성 테스트
cmake -B build -DDEFLATE_TESTS=ON -DDEFLATE_COMPAT_TEST=ON
cmake --build build -j
./build/tests/test_compat
```
