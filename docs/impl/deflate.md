# DEFLATE 구현 상세

## 완료 항목

| 작업 | 상태 |
|------|------|
| CMake + 타입 + 아레나 + BitWriter/Reader | ✅ |
| 스칼라 허프만 (Package-Merge) + LZ77 (레벨 0-12) | ✅ |
| DEFLATE 블록 타입 선택 (stored/fixed/dynamic) | ✅ |
| 스트리밍 압축기 (Compressor) | ✅ |
| 스트리밍 압축해제기 (Decompressor, 상태머신) | ✅ |
| zlib/gzip 래퍼 (whole-buffer) | ✅ |
| ARM NEON + CRC32 SIMD | ✅ |
| ARM64 NEON 성능 최적화 (hash 4→8 pos/iter, adler32 16→32 byte/iter, crc32 4× 언롤) | ✅ |
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
| QW-1: simd_match_length_fn() 함수 포인터 호이스팅 (루프 밖 1회 취득) | ✅ |
| QW-2: LZ77State::reset(hash_bits) — L1-L3에서 head[] zeroing 4-32KB로 축소 | ✅ |
| QW-3: Huffman 비트 역전 nibble 룩업 테이블 (루프 최대 15회 → O(1)) | ✅ |
| QW-4: fast-path 매치 후 pos+1 hash 삽입 (ratio +0.5-1.5%) | ✅ |
| M-1: lazy matching max_chain/2 (체인 탐색 비용 절반) | ✅ |
| M-2: AVX2 hash_insert_bulk — 8 스칼라 로드 → 2×128bit load + alignr | ✅ |
| M-3: package_merge/encode_code_lengths static 배열 제거 (스레드 안전) | ✅ |
| M-4: AVX2 chain insert — prefetch + prev[] 16-byte 연속 스토어 | ✅ |
| M-5: SSE4.2/NEON match tail — 4-byte XOR+ctz 스텝 추가 (7→3 scalar 최대) | ✅ |
| D-1: BitWriter 고정 4-byte flush (bit_count≥32 시 memcpy(4)+>>32) | ✅ |
| D-2: inflate_fast 단일 unconditional refill_fast + 내부 refill_safe 3곳 제거 | ✅ |
| D-3: copy_match 16바이트 word-at-a-time fast path (len≤16 && dist≥16) | ✅ |
| D-4: LZ77 체인 탐색 고엔트로피 early-exit (max_chain/4 단계 후 min-match 없으면 종료) | ✅ |
| D-5: 디코드 테이블 HUFF_LITERAL_FLAG (bit[24]) — sym<256 비교 → 비트 테스트 전환 | ✅ |
| D-6: HUFF_SUBTABLE_FLAG bit[24]→bit[25] 이동 (HUFF_LITERAL_FLAG와 구분) | ✅ |
| D-7: inflate_fast NEON 1+3 투기적 리터럴 배치 (ebits0 stride, OOO 병렬 load) | ✅ |
| C-1: LITLEN_DECODE_BITS 9→11 (8KB primary table, 2차 테이블 조회 감소) | ✅ |
| C-2: 128-bit 비트 어큐뮬레이터 + 1+8 NEON/x86 리터럴 배치 디코드 | ✅ |
| C-3: copy_match NEON 64/128-byte + SSE2 32-byte non-overlapping 확장 | ✅ |
| C-4: 멀티멤버 gzip 병렬 압축해제 (parallel_gzip_decompress, ThreadPool 재사용) | ✅ |
| E-1: inflate_fast NEON 1+5 fallback ev0 재사용 (vcombine×2 절감) | ✅ |
| E-2: copy_match dist 2-7 vqtbl1q_u8 splat (ARM64 오버래핑 패턴 NEON 가속) | ✅ |
| E-3: neon_hash_insert_bulk 16 pos/iter 확장 (lz77에서 미호출 — dead code) | ✅ |
| E-4: match_neon 32-byte unroll + kBits 중복 제거 (매치 탐색 throughput 향상) | ✅ |
| F-2: 전체 리터럴 블록 STORED 직행 (compute_block_stats + Huffman estimation 생략) | ✅ |
| F-3: BitWriter >= 56bit 시 7-byte flush (branch 빈도 감소) | ✅ |
| F-4: BT4 match_find_bt4 redundant bounds check 제거 (avail=max_match 항등) | ✅ |
| H-1: inflate_fast 오버래핑 copy_match 선형→더블링 O(log(len/dist)) | ✅ |
| H-2: neon_adler32 32B/iter→64B/iter 듀얼 어큐뮬레이터 (의존성 스톨 제거) | ✅ |
| H-3: post-inflate_fast 윈도우 싱크 조건부 스킵 (history≥WIN_SIZE 시 32KB memcpy 제거) | ✅ |
| H-4: DIST_DECODE_BITS 8→11 (2차 거리 테이블 조회 거의 0으로 감소) | ✅ |
| I-1: inflate_fast prev_ebits0 투기적 스트라이드 로드 (sg1~sg8 사전 발행, L1 latency 은닉) | ✅ |
| I-2: Decompressor 증분 Adler-32 (STORED 블록 adler_exact 추적, Huffman 재계산 회피) | ✅ |
| I-3: raw_decompress_ex + zlib_wrapper adler_exact 경로 (STORED-only 스트림 Adler 재계산 제거) | ✅ |
| J-1: match_find 4-byte 미스 카운트 전환 (첫 바이트→4바이트 단위 consec_misses, 고엔트로피 체인 조기 종료) | ✅ |
| J-2: 레벨별 miss_limit 차등 적용 (L4-6: 5, L7-9: 6, L10-12: 8) | ✅ |

---

## 알고리즘 개요

### 형식

#### Raw DEFLATE (RFC 1951)
- 헤더 없음, 최소 크기, 다른 형식에 내부적으로 사용
- C API: `DEFLATE_FORMAT_RAW`

#### Zlib (RFC 1950)
- 2바이트 헤더 + Adler-32 체크섬 (32비트)
- 가장 널리 사용; C API: `DEFLATE_FORMAT_ZLIB`

#### Gzip (RFC 1952)
- 10바이트+ 헤더 + CRC-32 체크섬, .gz 파일 형식
- C API: `DEFLATE_FORMAT_GZIP`

### 압축 레벨

| 레벨 | 상수 | 용도 |
|------|------|------|
| 0 | `DEFLATE_LEVEL_STORE` | 무압축 (이미 압축된 데이터) |
| 1 | `DEFLATE_LEVEL_FAST` | 실시간 스트림, 최고 속도 |
| 6 | `DEFLATE_LEVEL_DEFAULT` | 기본 선택 (속도-압축률 균형) |
| 9 | `DEFLATE_LEVEL_BETTER` | 배포 파일 |
| 12 | `DEFLATE_LEVEL_MAX` | 아카이브, 최고 압축률 |

---

## API 사용 예시

### C API

```c
#include <orot/deflate.h>

// 출력 버퍼 상한 계산
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
```

### C++ API — Whole-Buffer

```cpp
#include <orot/deflate.h>

std::vector<uint8_t> input = /* data */;
auto compressed = orot::deflate::compress(
    input,
    orot::deflate::Level::Default,
    orot::deflate::Format::Zlib);
auto restored = orot::deflate::decompress(
    compressed,
    orot::deflate::Format::Zlib);
assert(input == restored);
```

### C++ API — 스트리밍

```cpp
orot::deflate::Compressor compressor(
    orot::deflate::Level::Default,
    orot::deflate::Format::Gzip);

std::array<uint8_t, 65536> out_buffer;
size_t written = compressor.feed(std::span(chunk), std::span(out_buffer));
size_t final   = compressor.finish(std::span(out_buffer));
```

### C++ API — 병렬

```cpp
orot::deflate::ParallelCompressor pc(
    orot::deflate::Level::Default,
    orot::deflate::Format::Zlib,
    0,   // threads: 0 = 자동 감지
    0);  // block_size: 0 = 자동 설정
auto compressed = pc.compress(std::span(input_data));
```

---

## 빌드 & 테스트

### 빌드 옵션

```bash
# 기본 라이브러리
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 테스트 포함
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOROT_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# 전체 (SIMD + 병렬 + 벤치마크 + 비교)
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DOROT_USE_SIMD=ON -DOROT_AS_PARALLEL=ON \
      -DOROT_TESTS=ON -DOROT_BENCHMARK=ON \
      -DOROT_BENCHMARK_COMPARE=ON
cmake --build build -j$(nproc)
```

환경 요구사항: CMake 3.20+, C++20 컴파일러 (GCC 10+, Clang 12+)  
비교 벤치마크: `brew install zlib libdeflate` / `apt-get install zlib1g-dev libdeflate-dev`

### 테스트 실행

```bash
# 전체 테스트
ctest --test-dir build --output-on-failure

# 개별 실행
./build/tests/test_roundtrip    # 압축 ↔ 해제 라운드트립 (0B~10MB, L0~L12)
./build/tests/test_formats      # Raw/Zlib/Gzip 형식 검증
./build/tests/test_levels       # 13개 레벨 정합성
./build/tests/test_streaming    # 스트리밍 API 점진적 공급
./build/tests/test_huffman      # Huffman 테이블 정확성
./build/tests/test_parallel     # 멀티스레드 압축 (1/2/4/8 스레드)
./build/tests/test_simd         # SIMD vs 스칼라 동등성

# 벤치마크
./build/tests/bench_deflate [iterations]          # 단일 라이브러리
./build/tests/bench_deflate_compare [iterations]  # zlib/libdeflate 비교

# 호환성 테스트 (zlib + libdeflate 필요)
./build/tests/test_compat  # 126 케이스 (3 라이브러리 × 6 조합 × 3 레벨 × 3 데이터셋)
```

---

## 성능 결과

### 최종 벤치마크 (Apple M-series, Release, Zlib 형식)

#### 형식별 성능 (MB/s)

| 데이터 | L1 압축 | L6 압축 | L9 압축 | 해제 |
|--------|---------|---------|---------|------|
| Text (~90KB) | 2400+ | 900+ | 450+ | 3100+ |
| Zeros (1MB) | 3200+ | 1800+ | 500+ | 8500+ |
| Random (1MB) | 1100+ | 1000+ | 150+ | 1050+ |

#### 압축률

| 데이터 | L1 | L6 | L9 |
|--------|-----|-----|-----|
| Text | ~50% | ~38% | ~32% |
| Zeros | 0.31% | 0.31% | 0.31% |
| Random | ~100% | ~100% | ~100% |

#### zlib/libdeflate 비교 (L6, Text)

| 라이브러리 | 압축 MB/s | 해제 MB/s |
|-----------|-----------|-----------|
| orot | 950 | 3100 |
| zlib | 550 | 2900 |
| libdeflate | 850 | 3000 |

### 최적화 전/후 비교 (Apple M-series)

#### 초기 → 3차 최적화 (inflate_fast 연결, STORED_COPY bulk, LZ77 적응형 비교폭)

| 데이터셋 | 이전 comp | 이후 comp | 이전 decomp | 이후 decomp |
|---------|-----------|-----------|-------------|-------------|
| text fast | 79.1 MB/s | 680.0 MB/s | 87.8 MB/s | 1075.4 MB/s |
| zeros fast | 50.0 MB/s | 1357.4 MB/s | 93.7 MB/s | 1684.5 MB/s |
| random fast | 10.3 MB/s | 330.7 MB/s | 124.8 MB/s | 1733.3 MB/s |

#### 4차 (NEON splat/unroll, BitWriter 56-bit flush, STORED 직행, BT4 bounds 제거)

| 데이터셋 | 3차 decomp | 4차 decomp |
|---------|------------|------------|
| text fast | 1075 MB/s | 1646 MB/s (+53%) |
| zeros fast | 1685 MB/s | 2521 MB/s (+50%) |
| random fast | 1733 MB/s | 3278 MB/s (+89%) |

#### 5차 (H-1~H-4: 오버래핑 더블링, neon_adler32 64B/iter, DIST_DECODE_BITS 11)

| 데이터셋 | 4차 decomp | 5차 decomp | 개선 |
|---------|------------|------------|------|
| text fast | 1646 MB/s | **3019 MB/s** | +83% |
| zeros fast | 2521 MB/s | **5980 MB/s** | +137% |
| random fast | 3278 MB/s | **8439 MB/s** | +157% |
| code fast | 1876 MB/s | **3763 MB/s** | +100% |

### 교차 호환성

**126/126 PASS** (ZLIB×54 + GZIP×54 + RAW×18, 3개 라이브러리 × 6조합 × 3레벨 × 3데이터셋)

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

## 문제 해결

### 테스트 실패

| 에러 | 원인 | 조사 |
|------|------|------|
| `decompress size mismatch` | 압축/해제 로직 버그 | `src/decompress/decompressor.cpp` |
| `checksum mismatch` | Adler-32/CRC-32 계산 오류 | `src/formats/zlib_wrapper.cpp` 또는 `gzip_wrapper.cpp` |
| `invalid compressed data` | 데이터 손상 또는 형식 불일치 | 형식 확인 (Raw/Zlib/Gzip) 후 재시작 |

```bash
# 상세 로그
ctest --test-dir build --output-on-failure -V
# 디버그 빌드
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address"
```

### 성능 저하

| 증상 | 원인 | 해결 |
|------|------|------|
| 압축 느림 | Release 빌드 아님 | `-DCMAKE_BUILD_TYPE=Release` |
| 압축 느림 | SIMD 비활성화 | `-DOROT_USE_SIMD=ON` |
| 압축 느림 | 레벨 너무 높음 | L6→L1 또는 L3 |
| 메모리 많음 | 고레벨 압축 | L12→L9, 또는 스트리밍 API |

```bash
# 프로파일링
perf record -g ./build/tests/bench_deflate && perf report  # Linux
instruments -t "System Trace" ./build/tests/bench_deflate  # macOS
```

### 성능 회귀 감지

```bash
./build/tests/bench_deflate 200 > baseline.txt
# 코드 수정 후
./build/tests/bench_deflate 200 > current.txt
diff baseline.txt current.txt
# MB/s 10% 이상 저하 → 조사 필요
```
