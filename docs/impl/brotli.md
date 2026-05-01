# Brotli 구현 상세

## 완료 항목

| 작업 | 상태 |
|------|------|
| 공개 C/C++ API (`include/orot/brotli.h`) | ✅ |
| 내부 압축/해제 모듈 경계 (`src/brotli`) | ✅ |
| C API 진입점 (`src/api/brotli_api.cpp`) | ✅ |
| CMake 라이브러리/테스트/벤치 연결 | ✅ |
| uncompressed stream 단위 테스트 (`test_brotli.cpp`) | ✅ |
| libbrotlidec uncompressed stream 호환 테스트 | ✅ |
| stream header + meta-block header parser | ✅ |
| uncompressed meta-block encoder/decoder | ✅ |
| simple prefix code parser/decoder | ✅ |
| complex prefix code parser/decoder | ✅ |
| compressed meta-block helper parsers (varlen/block count/context map) | ✅ |
| compressed meta-block header parser 연결 | ✅ |
| single-tree compressed meta-block literal/copy decoder | ✅ |
| compressed meta-block block switching (literal/command/distance) | ✅ |
| literal/distance context map selection (LSB6/MSB6 + copy length) | ✅ |
| UTF8/Signed literal context lookup table | ✅ |
| libbrotlienc low-quality stream -> orot decode 호환 테스트 | ✅ |
| static dictionary byte table + identity/omit/uppercase transforms | ✅ |
| RFC 7932 WBITS decoder mapping | ✅ |
| static dictionary distance ring-buffer 예외 처리 | ✅ |
| static dictionary Shift transform 경로 | ✅ |
| minimal literal-only compressed encoder | ✅ |
| Brotli whole-buffer API fuzz harness | ✅ |
| complex prefix repeated code-length 즉시 적용 | ✅ |
| reference insert-length command table | ✅ |
| libbrotlienc q5 일반 문장 stream decode | ✅ |
| libbrotlienc q5/q9 장문 텍스트 stream decode | ✅ |
| libbrotlienc 다중 block/context 분포 stream decode | ✅ |
| postfix/direct distance parameter compat decode | ✅ |
| 일반 literal alphabet용 literal-only compressed encoder | ✅ |
| 단일 back-reference/copy command compressed encoder | ✅ |
| greedy 다중 back-reference/copy command encoder | ✅ |
| encoder last-distance short code 0 활용 | ✅ |
| hash-table 기반 greedy match 탐색 | ✅ |
| Brotli fuzz seed corpus 및 실행 스크립트 | ✅ |
| Brotli fuzz CI smoke workflow | ✅ |
| Brotli 자체/비교 벤치마크 | ✅ |
| 실제 Brotli decoder | ⬜ |
| 실제 Brotli encoder | ⬜ |
| compressed meta-block decoder | ⬜ |
| compressed meta-block encoder | ✅ |
| 퍼즈 테스트 | ✅ |

---

## 구현 범위

현재 단계는 Brotli 구현 2단계로, stream header와 meta-block header를
파싱하고 uncompressed meta-block으로 구성된 Brotli stream을 압축/해제한다.
compressed meta-block은 단일-tree literal/copy, block switching,
LSB6/MSB6/UTF8/Signed literal context, copy-length distance context를 해제할 수
있다. libbrotlienc의 low-quality 짧은 스트림과 일부 반복 데이터 compressed
stream을 해제할 수 있으며, static dictionary는 전체 byte table과
identity/omit/uppercase transform 경로를 사용한다. Shift transform 등 미지원
dictionary 경로는 Google Brotli와 동일한 UTF-8 scalar shift 동작을 수행한다.
WBITS는 RFC 7932의 variable-length mapping을 따르며, static dictionary 참조
distance는 last-distance ring-buffer에 push하지 않는다. Decoder compat는
libbrotlienc q5/q9 장문 텍스트, 분포 변화가 큰 block/context 샘플,
`NPOSTFIX/NDIRECT`를 강제한 distance parameter 샘플을 포함한다. Encoder는 기존
uncompressed meta-block fallback을 유지하면서, `quality > 0`이고 단일
meta-block으로 표현 가능한 입력은 compressed meta-block을 우선 출력한다.
Literal alphabet이 4개 이하인 경우 simple prefix code 기반 literal-only
compressed block을 쓴다. 일반 literal alphabet 입력은 압축 이득이 작거나 현재
command/distance simple prefix code 범위를 벗어나면 uncompressed meta-block으로
빠르게 fallback한다. 반복이 발견되면 hash-table 기반 greedy match 탐색으로 여러
insert/copy command와 distance code를 출력한다. Command/distance alphabet이 4개
이하이면 simple prefix code를 쓰고, 그보다 크면 uncompressed meta-block으로
fallback한다. Encoder는 last-distance ring buffer의 선두 distance와 일치하는
match에 short distance code 0을 사용한다.
q5 이하에서는 고다양성 바이트 샘플을 압축 불가능한 입력으로 빠르게 판별하고,
긴 match 내부 hash insert는 샘플링해 encode 비용을 낮춘다. Decoder는
uncompressed meta-block을 `memcpy`로 복사하고, copy command 실행도
`memcpy`/반복 확장 copy fast path를 사용한다. Prefix code decoder는 짧은
code용 direct lookup table과 긴 code용 canonical first-code table을 함께 사용해
linear entry scan을 피한다. 단일 block type meta-block은 block switch counter
갱신을 생략하고, all-zero context map은 per-symbol map lookup을 건너뛴다.
Complex prefix code의 repeated
code-length는 Brotli reference와 같이 읽는 즉시 Huffman space와 symbol position에
반영한다. Insert/copy-length command table은 Google Brotli decoder의 `kCmdLut`
생성 규칙과 같은 base/extra 값을 사용한다.

### API

```c
size_t orot_brotli_compress_bound(size_t src_size);

int orot_brotli_compress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    int         quality,
    int         lgwin);

int orot_brotli_decompress(
    const void* src, size_t src_size,
    void*       dst, size_t dst_cap,
    size_t*     uncompressed_size_out);
```

### 반환 코드

| 코드 | 의미 |
|------|------|
| `>= 0` | 출력 바이트 수 |
| `-1` | 잘못된 인자 |
| `-2` | 출력 버퍼 부족 |
| `-3` | 잘못된 입력 스트림 |
| `-4` | compressed meta-block 미구현 |

### 파라미터

| 이름 | 범위 | 기본값 |
|------|------|--------|
| `quality` | 0..11 | 5 |
| `lgwin` | 10..24 | 22 |

---

## Fuzz 실행

macOS Xcode clang은 libFuzzer runtime 링크가 실패할 수 있다. 실행 가능한
LLVM/clang 환경에서는 다음 스크립트로 Brotli seed corpus를 준비하고 fuzz target을
빌드/실행한다.

```sh
BUILD_DIR=build-fuzz MAX_TOTAL_TIME=60 sh tests/fuzz/run_brotli_fuzz.sh
```

## 벤치마크

`bench_brotli_compare`는 `libbrotli`와 OROT의 압축률, encode/decode 처리량,
양방향 교차 decode 호환성을 같은 dataset에서 측정한다.

```sh
./build-bench/tests/bench_brotli_compare --dataset=all --iters=30 --quality=5 --lgwin=22
```

`--dataset`은 `text`, `mixed`, `random`, `all`을 받는다.

## 다음 단계

1. encoder용 빈도 기반 literal prefix code 생성
2. command/distance alphabet 4개 초과 시 canonical prefix code 생성
3. last-distance short code 1..15 및 lazy match scoring 개선
4. libbrotlienc mixed/context-heavy stream의 literal context fast path 보강
5. decoder real-world corpus 확대와 dictionary-heavy stream 보강
