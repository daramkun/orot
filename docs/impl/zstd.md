# Zstandard 구현 상세

## 완료 항목

| 작업 | 상태 |
|------|------|
| 공개 C API 헤더 추가 | ✅ |
| 내부 `src/zstd/` 모듈 구조 추가 | ✅ |
| C API 진입점 추가 | ✅ |
| CMake 및 단위 테스트 연결 | ✅ |
| 기본 함수 시그니처 정의 | ✅ |

---

## 구현 개요

현재 단계는 Zstandard 구현의 1단계 골격이다. 공개 API와 내부 모듈 경계를 먼저 고정하고, 실제 포맷 호환 압축/해제는 후속 단계에서 frame, block, entropy, sequence 처리 순서로 확장한다.

압축기와 압축해제기는 아직 zstd 스트림을 생성하거나 해석하지 않는다. 두 함수는 기능 미구현을 명확히 표현하기 위해 `-1`을 반환한다.

## 파일 구조

```
include/orot/zstd.h        # Zstandard C API
src/zstd/
├── zstd.hpp               # 내부 API, 상수, 모듈 경계
└── zstd.cpp               # stage-1 기본 구현
src/api/zstd_api.cpp       # C API 진입점
tests/unit/test_zstd.cpp   # stage-1 API 계약 테스트
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

## 다음 단계

2단계에서 Zstandard frame magic/header, block header, raw block, RLE block, checksum 옵션 파싱을 추가한다. 그 시점부터 `orot_zstd_decompress`는 제한된 zstd frame을 실제로 해제하기 시작한다.

## 빌드 및 검증

```bash
cmake -B build -DOROT_TESTS=ON
cmake --build build -j
./build/tests/test_zstd
```
