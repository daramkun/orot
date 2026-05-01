# LZW 구현 상세

## 완료 항목

| 작업 | 상태 |
|------|------|
| LZW 알고리즘 구현 (variable-width code, 9~16bit, clear/EOI code) | ✅ |

---

## 구현 개요

orot의 LZW 구현은 가변 폭 코드(variable-width code) 방식의 Lempel-Ziv-Welch 압축이다.

### 코드 체계

| 상수 | 값 | 설명 |
|------|----|------|
| `LZW_CLEAR_CODE` | 256 | 딕셔너리 초기화 신호 |
| `LZW_EOI_CODE` | 257 | 스트림 종료 신호 |
| `LZW_FIRST_CODE` | 258 | 첫 번째 딕셔너리 엔트리 |
| `LZW_MIN_BITS` | 9 | 최소 코드 폭 |
| `LZW_MAX_BITS` | 16 | 최대 코드 폭 |
| `LZW_DEF_BITS` | 12 | 기본 코드 폭 |

### 동작 방식

- 압축 시작 시 CLEAR_CODE 출력
- 딕셔너리가 `(1 << current_bits)` 크기를 초과하면 코드 폭 1 증가
- `max_bits` 도달 후 딕셔너리 꽉 참: 기존 딕셔너리 계속 사용 (reset 없음)
- 스트림 끝에 EOI_CODE 출력

### 출력 포맷

```
[1 byte: max_bits] [LSB-first variable-width codes ...]
```

- 첫 1바이트: `max_bits` 값 (9~16). 압축해제 시 별도 파라미터 불필요
- 이후: LSB 우선 가변 폭 코드 비트스트림

`max_bits=12` 기본 압축 경로는 성능을 위해 OROT 전용 fast subformat을 사용할 수 있다.
디코더는 기존 `9~16` 헤더의 가변 폭 스트림과 아래 fast subformat을 모두 처리한다.

| 헤더 | 포맷 | 페이로드 |
|------|------|----------|
| `0x8c` | fixed-12 LZW | `[LSB-first 12-bit codes ...]` |
| `0x4c` | stored raw | `[u32le: decoded_len] [raw bytes ...]` |
| `0x2c` | periodic | `[u32le: decoded_len] [u16le: period] [period bytes ...]` |

- `fixed-12`는 기본 설정에서 code-width 갱신 분기를 제거한 12bit 고정 코드 스트림이다.
- `stored raw`는 고엔트로피 입력에서 LZW 확장과 느린 딕셔너리 경로를 피한다.
- `periodic`은 단순 반복/주기 입력을 주기 descriptor로 저장해 압축해제 시 반복 복사만 수행한다.

### 메모리 사용

- 압축: 해시 테이블 기반 딕셔너리 `(1 << max_bits)` 슬롯. 반복 호출의 heap
  allocation을 피하기 위해 thread-local scratch table을 재사용한다. 기본 `max_bits=12`
  경로는 별도 fixed-12 해시 테이블을 사용한다.
- 압축해제: 코드→문자열 테이블 `(1 << max_bits)` 엔트리. 각 엔트리는 prefix,
  suffix, 문자열 길이와 첫 바이트를 저장해 KwKwK/새 엔트리 생성 시 prefix chain
  재탐색을 줄인다. 테이블은 thread-local scratch로 재사용한다.

---

## 파일 구조

```
include/orot/lzw.h          # C API
src/lzw/
├── lzw_block.hpp           # 내부 API + LZWConfig 상수
└── lzw_block.cpp           # 압축/해제 구현
src/api/lzw_api.cpp         # C API 진입점
tests/unit/
└── test_lzw.cpp            # 라운드트립 + 엣지 케이스
tests/bench/
├── bench_lzw.cpp           # 단일 라이브러리 벤치마크
└── bench_lzw_compare.cpp   # 비교 벤치마크
```

---

## C API

```c
// lzw.h

// max_bits=12 기준 출력 크기 상한
int orot_lzw_compress_bound(int src_size);

// max_bits: 9~16, 0이면 기본값(12) 사용
// 반환: 기록된 바이트 수, -1: 오류
int orot_lzw_compress(const void* src, int src_size,
                      void*       dst, int dst_cap,
                      int         max_bits);

// max_bits는 스트림 헤더에서 자동 판독
// 반환: 기록된 바이트 수, -1: 잘못된 입력, -2: 출력 버퍼 부족
int orot_lzw_decompress(const void* src, int src_size,
                        void*       dst, int dst_cap);
```

## C++ 내부 API

```cpp
// src/lzw/lzw_block.hpp (namespace orot::lzw)

int lzw_compress_bound(int src_len, int max_bits) noexcept;

int lzw_compress(const uint8_t* src, int src_len,
                 uint8_t* dst, int dst_cap,
                 const LZWConfig& cfg) noexcept;

int lzw_decompress(const uint8_t* src, int src_len,
                   uint8_t* dst, int dst_cap) noexcept;
```

---

## 빌드 및 검증

```bash
# LZW 단위 테스트
cmake -B build -DOROT_TESTS=ON
cmake --build build -j
./build/tests/test_lzw

# LZW 벤치마크
cmake -B build -DOROT_BENCHMARK=ON
cmake --build build -j
./build/tests/bench_lzw

# 비교 벤치마크
cmake -B build -DOROT_BENCHMARK_COMPARE=ON
cmake --build build -j
./build/tests/bench_lzw_compare
```
