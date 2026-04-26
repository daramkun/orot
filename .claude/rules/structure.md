# orot 프로젝트 구조

## 디렉토리 트리

```
orot/
├── include/orot/
│   ├── deflate.h           # DEFLATE C API (whole-buffer, streaming, parallel) + C++ RAII 래퍼
│   ├── deflate_types.h     # 에러코드, enum, allocator 인터페이스
│   ├── lz4.h               # LZ4 C API (block + frame)
│   ├── lzw.h               # LZW C API
│   ├── lzma.h              # LZMA/LZMA2 C/C++ API
│   ├── bzip2.h             # Bzip2 C/C++ API
│   └── zstd.h              # Zstandard C API
├── src/
│   ├── core/               # 핵심 압축 프리미티브
│   │   ├── huffman.{cpp,hpp}        # Huffman 인코딩/디코딩 (Package-Merge)
│   │   ├── lz77.{cpp,hpp}           # LZ77 토크나이저 (레벨 0-12, 적응형 hash_bits)
│   │   ├── deflate_block.{cpp,hpp}  # 블록 타입 선택 (stored/fixed/dynamic)
│   │   ├── bit_writer.hpp           # 비트스트림 출력 버퍼링
│   │   └── bit_reader.hpp           # 비트스트림 입력 파싱
│   ├── compress/           # 압축 컨트롤러
│   │   ├── compressor.{cpp,hpp}       # 스트리밍 압축 상태머신
│   │   ├── block_compressor.{cpp,hpp} # 단일 블록 압축 워커
│   │   └── level_config.hpp           # 압축 레벨 튜닝 파라미터
│   ├── decompress/         # 압축 해제 컨트롤러
│   │   ├── decompressor.{cpp,hpp}  # 스트리밍 압축해제 상태머신
│   │   └── inflate_fast.{cpp,hpp}  # 리터럴/매치 복사 고속 내부 루프
│   ├── formats/            # 컨테이너 포맷 핸들러
│   │   ├── raw_deflate.{cpp,hpp}   # RFC 1951: raw DEFLATE
│   │   ├── zlib_wrapper.{cpp,hpp}  # RFC 1950: zlib 프레이밍 (Adler-32)
│   │   └── gzip_wrapper.{cpp,hpp}  # RFC 1952: gzip 프레이밍 (CRC-32, 헤더)
│   ├── lz4/                # LZ4 구현 
│   │   ├── lz4_block.cpp   # LZ4 raw block 압축/해제
│   │   └── lz4_frame.cpp   # LZ4 frame 포맷 (XXH32 체크섬)
│   ├── lzma/               # LZMA/LZMA2 구현
│   │   ├── range_coder.hpp         # 범위 인코더/디코더
│   │   ├── lzma_prob_model.hpp     # 확률 테이블 + 상태기계
│   │   ├── lzma_compress.{cpp,hpp} # LZMA alone 압축
│   │   ├── lzma_decompress.{cpp,hpp} # LZMA alone 압축해제
│   │   ├── lzma2_compress.cpp      # LZMA2 청크 스트림 압축
│   │   └── lzma2_decompress.cpp    # LZMA2 청크 스트림 압축해제
│   ├── bzip2/              # Bzip2 구현
│   │   ├── bzip2_crc.hpp           # MSB-first CRC32 (constexpr 256-entry table)
│   │   ├── bwt.{cpp,hpp}           # BWT + 역변환 (counting sort prefix doubling)
│   │   ├── mtf.hpp                 # Move-to-Front (in-use 알파벳, rank==0 fast path)
│   │   ├── bzip2_huffman.{cpp,hpp} # 다중 Huffman 테이블 + selector
│   │   ├── bzip2_compress.{cpp,hpp}   # 5단계 압축 파이프라인
│   │   └── bzip2_decompress.{cpp,hpp} # 5단계 압축해제 파이프라인
│   ├── zstd/               # Zstandard 구현
│   │   ├── zstd.hpp        # 내부 API, 상수, 모듈 경계
│   │   ├── fse.{cpp,hpp}   # FSE normalized count/table + reverse bitstream decoder
│   │   ├── huf.{cpp,hpp}   # zstd literal Huffman table/stream decoder
│   │   └── zstd.cpp        # frame/block parser + raw/RLE/compressed 압축해제 + XXH64 checksum
│   ├── api/                # C API 진입점
│   │   ├── deflate_api.cpp # whole-buffer compress/decompress + 체크섬
│   │   ├── stream_api.cpp  # 스트리밍 + 병렬 API
│   │   ├── lz4_api.cpp     # LZ4 C API 진입점
│   │   ├── lzw_api.cpp     # LZW C API 진입점
│   │   ├── lzma_api.cpp    # LZMA/LZMA2 C API 진입점
│   │   ├── bzip2_api.cpp   # Bzip2 C API 진입점
│   │   └── zstd_api.cpp    # Zstandard C API 진입점
│   ├── parallel/           # 멀티스레드 압축 (pigz 스타일)
│   │   ├── thread_pool.{cpp,hpp}           # 고정 스레드 풀 실행기
│   │   ├── parallel_compressor.{cpp,hpp}   # 블록 분할 + 병합 워커
│   │   └── sync_queue.hpp                  # 락-프리 작업 큐
│   ├── simd/               # SIMD 가속 (런타임 디스패치)
│   │   ├── simd_detect.cpp     # CPU 기능 감지 (SSE2/SSE4.2/AVX2/NEON/CRC)
│   │   ├── simd_dispatch.hpp   # 함수 포인터 라우팅
│   │   ├── x86/
│   │   │   ├── hash_sse2.cpp     # 해시 체인 (SSE2)
│   │   │   ├── hash_avx2.cpp     # 해시 체인 (AVX2)
│   │   │   ├── match_sse42.cpp   # LZ77 매치 탐색 (SSE4.2)
│   │   │   ├── crc32_sse42.cpp   # CRC32 (SSE4.2 PCLMULQDQ)
│   │   │   └── adler32_avx2.cpp  # Adler-32 (AVX2)
│   │   └── arm/
│   │       ├── hash_neon.cpp     # 해시 체인 (ARM NEON)
│   │       ├── match_neon.cpp    # LZ77 매치 탐색 (NEON)
│   │       └── crc32_arm.cpp     # CRC32 (ARM CRC32 명령)
│   └── memory/             # 커스텀 allocator & arena
│       ├── arena.hpp           # 선형 arena allocator
│       ├── pool_allocator.hpp  # 스레드 안전 블록 풀
│       └── aligned_alloc.hpp   # 정렬 메모리 헬퍼
├── tests/
│   ├── unit/
│   │   ├── test_roundtrip.cpp        # DEFLATE 압축 ↔ 해제 라운드트립
│   │   ├── test_formats.cpp          # Raw/Zlib/Gzip 포맷
│   │   ├── test_levels.cpp           # 13개 압축 레벨
│   │   ├── test_streaming.cpp        # 스트리밍 API 점진적 공급
│   │   ├── test_huffman.cpp          # Huffman 테이블 정확성
│   │   ├── test_parallel.cpp         # 멀티스레드 압축
│   │   ├── test_simd.cpp             # SIMD 가속 경로
│   │   ├── test_lz4.cpp              # LZ4 block/frame 라운드트립 + 에러 경로
│   │   ├── test_lz4_comprehensive.cpp # LZ4 엣지 케이스 종합
│   │   ├── test_lzma.cpp             # LZMA/LZMA2 라운드트립 + 엣지 케이스
│   │   ├── test_bzip2.cpp            # Bzip2 라운드트립 + 엣지 케이스
│   │   └── test_zstd.cpp             # Zstandard frame/block/compressed sample + 에러 경로
│   ├── bench/
│   │   ├── bench_compress.cpp        # DEFLATE 단일 라이브러리 벤치 (→ bench_deflate)
│   │   ├── bench_compare.cpp         # DEFLATE 비교 벤치 (zlib, libdeflate) (→ bench_deflate_compare)
│   │   ├── bench_lz4.cpp             # LZ4 단일 라이브러리 벤치 (→ bench_lz4)
│   │   ├── bench_lz4_compare.cpp     # LZ4 비교 벤치 (liblz4) (→ bench_lz4_compare)
│   │   ├── bench_lzma.cpp            # LZMA 단일 라이브러리 벤치 (→ bench_lzma)
│   │   ├── bench_lzma_compare.cpp    # LZMA 비교 벤치 (liblzma) (→ bench_lzma_compare)
│   │   ├── bench_bzip2.cpp           # Bzip2 단일 라이브러리 벤치 (→ bench_bzip2)
│   │   └── bench_bzip2_compare.cpp   # Bzip2 비교 벤치 (libbz2) (→ bench_bzip2_compare)
│   ├── compat/
│   │   └── test_compat.cpp      # 교차 라이브러리 호환성 (126 케이스)
│   └── fuzz/
│       ├── fuzz_roundtrip.cpp   # libfuzzer compress+decompress
│       └── fuzz_decompress.cpp  # libfuzzer decompress only
├── cmake/
│   ├── DetectSIMD.cmake      # CPU 기능 프로빙
│   ├── CompilerFlags.cmake   # LTO, 경고, 최적화
│   └── InstallConfig.cmake   # 설치 타겟
├── CMakeLists.txt            # 빌드 설정 (C++20, SIMD 감지)
└── PLAN.md                   # 구현 로드맵 (모든 항목 완료)
```

## CMake 옵션

| 옵션 | 기본값 | 설명 |
|------|--------|------|
| `OROT_USE_SIMD` | ON | SSE2/SSE4.2/AVX2/NEON/CRC 최적화 |
| `OROT_AS_PARALLEL` | ON | 병렬 압축 (pigz 스타일) |
| `OROT_TESTS` | OFF | 유닛 + 퍼즈 테스트 |
| `OROT_BENCHMARK` | OFF | DEFLATE 성능 벤치마크 (→ `bench_deflate`) |
| `OROT_DEFLATE_ZLIB_COMPAT` | ON | zlib 호환 매크로 별칭 (Z_OK 등) |
| `OROT_SHARED` | OFF | 공유 라이브러리 (기본: 정적) |
| `OROT_BENCHMARK_COMPARE` | OFF | DEFLATE 비교 벤치마크 (→ `bench_deflate_compare`, zlib+libdeflate 필요) |
| `OROT_TESTS` | OFF | 교차 라이브러리 호환성 테스트 |
| `OROT_BENCHMARK` | OFF | LZ4 성능 벤치마크 (→ `bench_lz4`) |
| `OROT_BENCHMARK_COMPARE` | OFF | LZ4 비교 벤치마크 (→ `bench_lz4_compare`, liblz4 필요) |

## 핵심 타입

```c
// deflate_types.h / deflate.h
deflate_result    // OK, STREAM_END, NEED_INPUT/OUTPUT, DATA_ERROR, MEM_ERROR
deflate_format    // RAW, ZLIB, GZIP
deflate_flush     // NO_FLUSH, SYNC_FLUSH, FULL_FLUSH, FINISH
deflate_allocator // 커스텀 malloc/free 인터페이스
```

## 핵심 클래스

| 클래스 | 파일 | 역할 |
|--------|------|------|
| `Compressor` | `src/compress/compressor.hpp` | 스트리밍 압축 상태머신 |
| `Decompressor` | `src/decompress/decompressor.hpp` | 스트리밍 압축해제 상태머신 |
| `ParallelCompressor` | `src/parallel/parallel_compressor.hpp` | 멀티스레드 whole-buffer 압축 |
| `HuffEncTable` | `src/core/huffman.hpp` | Huffman 코드 & 길이 인코딩 |
| `HuffDecTable` | `src/core/huffman.hpp` | Huffman 비트스트림 디코딩 |
| `LZ77State` | `src/core/lz77.hpp` | 해시 체인 + 슬라이딩 윈도우 상태 |
| `Token` | `src/core/lz77.hpp` | 리터럴/길이/거리 트리플 |
| `LevelConfig` | `src/compress/level_config.hpp` | 레벨별 hash_bits, max_chain, lazy_min |
| `BitWriter` | `src/core/bit_writer.hpp` | 비트 단위 출력 버퍼링 |
| `BitReader` | `src/core/bit_reader.hpp` | 비트 단위 입력 파싱 |

## C API 함수

```c
// whole-buffer (deflate.h)
deflate_compress_bound()   // 출력 크기 상한
deflate_compress()         // 단일 호출 압축
deflate_decompress()       // 단일 호출 압축해제

// 스트리밍
deflate_stream_new/free()
deflate_stream_compress(flush_mode)
inflate_stream_new/free()
deflate_stream_decompress()

// 병렬
deflate_parallel_new/free()
deflate_parallel_compress()

// 체크섬
deflate_adler32()
deflate_crc32()

// LZ4 block (lz4.h)
orot_lz4_compress_bound()
orot_lz4_compress()
orot_lz4_decompress()

// LZ4 frame (lz4.h)
orot_lz4f_compress()
orot_lz4f_decompress()

// LZMA alone (lzma.h)
orot_lzma_compress_bound()
orot_lzma_compress()
orot_lzma_decompress()

// LZMA2 chunk stream (lzma.h)
orot_lzma2_compress_bound()
orot_lzma2_compress()
orot_lzma2_decompress()

// Bzip2 (bzip2.h)
orot_bzip2_compress_bound()
orot_bzip2_compress()
orot_bzip2_compress_parallel()
orot_bzip2_decompress()

// Zstandard (zstd.h)
orot_zstd_compress_bound()
orot_zstd_compress()
orot_zstd_decompress()
```

## C++ API

```cpp
// deflate.h
deflate::compress<std::vector<uint8_t>>()
deflate::decompress<std::vector<uint8_t>>()
deflate::Compressor::feed(span) / finish(span)
deflate::Decompressor::feed(span, done_flag)
deflate::ParallelCompressor::compress(span)
deflate::adler32/crc32(span)
```

## SIMD 지원

| CPU 기능 | x86 | ARM | 모듈 |
|----------|-----|-----|------|
| SSE2 | ✅ | — | `hash_sse2.cpp` |
| SSE4.2 | ✅ | — | `match_sse42.cpp`, `crc32_sse42.cpp` |
| AVX2 | ✅ | — | `hash_avx2.cpp`, `adler32_avx2.cpp` |
| NEON | — | ✅ | `hash_neon.cpp`, `match_neon.cpp` |
| CRC32 (ARM) | — | ✅ | `crc32_arm.cpp` |

런타임 CPU 프로빙: `simd_detect.cpp` → `simd_dispatch.hpp` 함수 포인터 라우팅

## 압축 레벨

- **L0**: 무압축 (STORED)
- **L1-L3**: 고속 경로 (단일 해시 조회, 최소 체인 탐색)
- **L4-L9**: 균형 (적응형 해시 크기 + 체인 한계)
- **L10-L12**: 최대 압축 (전체 체인 탐색, lazy 매칭)
