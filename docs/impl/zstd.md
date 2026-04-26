# Zstandard 구현 상세

## 완료 항목

| 작업 | 상태 |
|------|------|
| 공개 C API 헤더 추가 | ✅ |
| 내부 `src/zstd/` 모듈 구조 추가 | ✅ |
| C API 진입점 추가 | ✅ |
| CMake 및 단위 테스트 연결 | ✅ |
| 기본 함수 시그니처 정의 | ✅ |
| Zstandard frame magic/header 파싱 | ✅ |
| frame descriptor, window descriptor, content size, dictionary id 처리 | ✅ |
| block header 파싱 | ✅ |
| raw block 압축 해제 | ✅ |
| RLE block 압축 해제 | ✅ |
| checksum 옵션 파싱 및 XXH64 기반 검증 | ✅ |
| malformed frame/block 에러 경로 테스트 | ✅ |
| FSE normalized count 파싱 및 decode table 구성 | ✅ |
| reverse FSE bitstream decoder | ✅ |
| literal Huffman table/stream decoder | ✅ |
| compressed block literals/sequences/window copy 경로 | ✅ |
| skippable frame 처리 | ✅ |
| dictionary frame 미지원 에러 처리 | ✅ |
| libzstd 산출물 압축 해제 샘플 테스트 | ✅ |
| zstd frame writer | ✅ |
| raw block encoder | ✅ |
| whole-block RLE encoder | ✅ |
| 레벨 1-9 입력 검증 및 정책 매핑 | ✅ |
| incompressible data raw block fallback | ✅ |
| 128 KiB 단위 chunking | ✅ |
| `orot_zstd_compress` 라운드트립 테스트 | ✅ |

---

## 구현 개요

현재 구현은 Zstandard 공개 API와 내부 모듈 경계를 고정하고, 압축 해제 쪽에서 frame/header, raw/RLE block, compressed block의 literals/sequences/window copy 경로를 처리한다. 압축 쪽은 valid zstd frame writer와 raw/RLE block encoder를 제공한다.

compressed block은 FSE entropy table과 reverse bitstream으로 sequence code를 복원하고, literal section은 raw/RLE/Huffman 형태를 파싱한다. repeat mode를 위해 frame 내 이전 FSE/Huffman table 상태와 repeated offset 상태를 유지한다.

압축기는 레벨 1-9를 검증한 뒤 128 KiB block 단위로 frame을 생성한다. block 전체가 같은 바이트로 구성된 경우 RLE block을 쓰고, 그 외 입력은 raw block으로 fallback한다. 이 경로는 압축률보다 포맷 호환성과 라운드트립 안정성을 우선한다.

## Frame/Header 처리

`orot_zstd_decompress`는 다음 frame header 필드를 파싱한다.

| 필드 | 처리 |
|------|------|
| Magic number | little-endian `0xFD2FB528` 검증 |
| Frame Header Descriptor | FCS, single segment, checksum, dictionary id 플래그 파싱 |
| Reserved/unused bits | 설정된 경우 malformed frame으로 거부 |
| Window Descriptor | non-single-segment frame에서 window size 계산 |
| Dictionary ID | 0/1/2/4바이트 dictionary id 파싱 |
| Frame Content Size | 1/2/4/8바이트 content size 파싱, 2바이트 form은 +256 적용 |

content size가 제공된 frame은 출력 크기가 정확히 일치해야 성공한다.

## Block 처리

block header는 3바이트 little-endian 값으로 파싱한다.

| Block type | 상태 | 동작 |
|------------|------|------|
| Raw block (`0`) | ✅ | block content를 그대로 출력 |
| RLE block (`1`) | ✅ | content 1바이트를 block size만큼 반복 출력 |
| Compressed block (`2`) | ✅ | literals section과 sequences section을 해제하고 window copy 수행 |
| Reserved (`3`) | ✅ | malformed block으로 거부 |

block size는 Zstandard block 최대 크기인 128 KiB를 넘으면 거부한다.

## Compressor

`orot_zstd_compress`는 다음 정책으로 frame을 생성한다.

| 항목 | 처리 |
|------|------|
| Frame header | single segment frame, content size 포함 |
| Content size | 1/2/4바이트 FCS form 선택 |
| Block split | 최대 128 KiB 단위 chunking |
| RLE 감지 | block 전체가 동일 byte일 때 RLE block 출력 |
| Raw fallback | 그 외 모든 입력은 raw block 출력 |
| Level | 1-9 범위 검증 및 내부 정책 매핑 |
| Dictionary/checksum | compressor 경로에서는 아직 생성하지 않음 |

현재 encoder는 entropy-compressed block을 생성하지 않는다. 압축 해제기의 entropy decoder와 sequence executor는 libzstd 샘플 검증에 사용되며, compressor는 raw/RLE fallback으로 포맷 호환 출력을 보장한다.

## Checksum

`Content_Checksum_flag`가 설정된 frame은 해제 완료 후 decoded content에 대해 seed 0의 XXH64를 계산하고, 하위 32비트를 frame trailer의 little-endian checksum과 비교한다. 불일치 시 `-3`을 반환한다.

## Entropy 및 Sequence 처리

`src/zstd/fse.*`는 normalized count 파싱, decode table 구성, reverse bitstream state 갱신을 담당한다. predefined, RLE, compressed, repeat mode를 sequence table별로 처리한다.

`src/zstd/huf.*`는 literal Huffman weight table 복원과 single-stream/4-stream literal decode를 담당한다. compressed literals block은 새 Huffman table을 구성하고, treeless literals block은 이전 table을 재사용한다.

sequence 실행은 literal length, offset code, match length code의 baseline/extra bits를 복원한 뒤 repeated offset 규칙을 적용한다. match copy는 현재 frame 출력 버퍼를 history window로 사용하며 overlapping copy를 byte 단위로 처리한다.

## 파일 구조

```
include/orot/zstd.h        # Zstandard C API
src/zstd/
├── zstd.hpp               # 내부 API, 상수, 모듈 경계
├── fse.hpp/.cpp           # FSE table + reverse bitstream decoder
├── huf.hpp/.cpp           # zstd literal Huffman decoder
└── zstd.cpp               # frame/block parser + raw/RLE compressor + compressed block decompress
src/api/zstd_api.cpp       # C API 진입점
tests/unit/test_zstd.cpp   # frame/block/compressed sample + 에러 경로 테스트
```

## C API

```c
int orot_zstd_compress_bound(int src_size);

int orot_zstd_compress(const void* src, int src_size,
                       void*       dst, int dst_cap,
                       int         level);

int orot_zstd_decompress(const void* src, int src_size,
                         void*       dst, int dst_cap);
```

반환 규칙은 기존 orot 알고리즘 API와 동일하게 유지한다.

- `>= 0`: 출력 바이트 수
- `-1`: 미지원 또는 잘못된 입력
- `-2`: 출력 버퍼 부족
- `-3`: checksum mismatch

## 남은 작업

압축 해제는 기본 libzstd 샘플과 경계 조건을 통과한다. 압축기는 현재 raw/RLE block을 생성하므로, 향후 compressed-block encoder에서 Huffman/FSE table 선택, sequence bitstream 작성, lazy/hash-chain match finder를 실제 압축률 개선 경로에 연결할 필요가 있다. 독립 FSE/Huffman 단위 테스트와 더 다양한 Huffman literal 샘플, 다중 block/window 회귀 테스트도 추가 검증 항목이다.

## 빌드 및 검증

```bash
cmake -B build -DOROT_TESTS=ON
cmake --build build -j
./build/tests/test_zstd
```
