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
| 실제 Brotli decoder | ⬜ |
| 실제 Brotli encoder | ⬜ |
| compressed meta-block decoder | ⬜ |
| compressed meta-block encoder | ⬜ |
| 퍼즈 테스트 | ⬜ |

---

## 구현 범위

현재 단계는 Brotli 구현 2단계로, stream header와 meta-block header를
파싱하고 uncompressed meta-block으로 구성된 Brotli stream을 압축/해제한다.
compressed meta-block을 만나면 `-4`를 반환한다.

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

1. compressed meta-block command/literal/distance decoder 연결
2. Google Brotli encoder가 만든 compressed stream 해제 호환성 추가
3. minimal compressed encoder 구현
4. static dictionary 지원 추가
5. 퍼즈 테스트 추가
