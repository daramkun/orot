# Brotli 구현 상세

## 완료 항목

| 작업 | 상태 |
|------|------|
| 공개 C/C++ API (`include/orot/brotli.h`) | ✅ |
| 내부 압축/해제 모듈 경계 (`src/brotli`) | ✅ |
| C API 진입점 (`src/api/brotli_api.cpp`) | ✅ |
| CMake 라이브러리/테스트/벤치 연결 | ✅ |
| 스캐폴드 단위 테스트 (`test_brotli.cpp`) | ✅ |
| 실제 Brotli decoder | ⬜ |
| 실제 Brotli encoder | ⬜ |
| libbrotli 교차 호환 테스트 | ⬜ |
| 퍼즈 테스트 | ⬜ |

---

## 구현 범위

현재 단계는 Brotli 구현 1단계로, ABI와 파일 구조를 먼저 고정한다.
압축/해제 함수는 입력 검증과 출력 버퍼 검사를 수행한 뒤 `-4`
(`feature not implemented yet`)를 반환한다.

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
| `-4` | 아직 미구현 |

### 파라미터

| 이름 | 범위 | 기본값 |
|------|------|--------|
| `quality` | 0..11 | 5 |
| `lgwin` | 10..24 | 22 |

---

## 다음 단계

1. Brotli bit reader와 stream header/meta-block parser 구현
2. uncompressed meta-block decoder/encoder 구현
3. Google Brotli 라이브러리와 uncompressed stream 교차 호환 테스트 추가
4. Huffman/meta-block command decoder 구현
5. minimal compressed encoder와 static dictionary 지원 추가
