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
| 실제 Brotli decoder | ⬜ |
| 실제 Brotli encoder | ⬜ |
| compressed meta-block decoder | ⬜ |
| compressed meta-block encoder | ⬜ |
| 퍼즈 테스트 | ⬜ |

---

## 구현 범위

현재 단계는 Brotli 구현 2단계로, stream header와 meta-block header를
파싱하고 uncompressed meta-block으로 구성된 Brotli stream을 압축/해제한다.
compressed meta-block은 단일-tree literal/copy, block switching,
LSB6/MSB6/UTF8/Signed literal context, copy-length distance context를 해제할 수
있다. libbrotlienc의 low-quality 짧은 스트림과 일부 반복 데이터 compressed
stream을 해제할 수 있으며, static dictionary는 전체 byte table과
identity/omit/uppercase transform 경로를 사용한다. Shift transform 등 미지원
dictionary 경로는 `-4`를 반환한다.

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

## 다음 단계

1. static dictionary Shift transform 지원 추가
2. Google Brotli encoder가 만든 일반 compressed stream 호환성 확대
3. minimal compressed encoder 구현
4. 퍼즈 테스트 추가
