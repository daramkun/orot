# LZ4 구현 상세

## 완료 항목

| 작업 | 상태 |
|------|------|
| K-1: LZ4 알고리즘 지원 추가 (raw block + LZ4 frame, 레벨 1-9, XXH32 체크섬) | ✅ |

---

## 구현 개요

orot의 LZ4 구현은 raw block 포맷과 LZ4 frame 포맷을 모두 지원한다.

### Raw Block

- 최소 매치 길이: 4바이트
- 슬라이딩 윈도우: 64KB (`LZ4_WIN_SIZE = 65536`)
- 해시 테이블: 16-bit (`LZ4_HASH_BITS = 16`, 64K 슬롯)
- 마지막 5바이트는 항상 리터럴 (`LZ4_LAST_LIT = 5`)
- 마지막 매치는 스트림 끝 12바이트 이전에 종료 (`LZ4_LAST_MATCH = 12`)

### 압축 레벨

| 레벨 | 전략 | max_chain | miss_limit | nice_len |
|------|------|-----------|------------|---------|
| L1-L3 | fast path (head-only) | — | — | — |
| L4-L6 | HC (hash chain) | 8 | 4 | 64 |
| L7-L9 | HC (hash chain) | 32 | 6 | 128 |

- L1-L3: `head[]` 단일 조회, chain traversal 없음, `prev[]` 업데이트 생략
- L4+: hash chain traversal + miss_limit으로 고엔트로피 조기 종료

### Frame 포맷

LZ4 frame 포맷 (LZ4F):
- 매직 넘버 `0x184D2204`
- Frame Descriptor: FLG + BD + Content Size(선택) + Header Checksum(XXH32 상위 1바이트)
- 데이터 블록: 4바이트 block size + 압축 데이터
- End Mark: `0x00000000`
- Content Checksum: XXH32 (FLG에서 C_Checksum 플래그 설정 시)

---

## 파일 구조

```
include/orot/lz4.h          # C API (block + frame)
src/lz4/
├── lz4_block.hpp           # 내부 LZ4 block API + LZ4State + LZ4Config
├── lz4_block.cpp           # raw block 압축/해제 구현
├── lz4_frame.hpp           # 내부 frame 포맷 상수
└── lz4_frame.cpp           # frame 포맷 압축/해제 구현
src/api/lz4_api.cpp         # C API 진입점
tests/unit/
├── test_lz4.cpp            # block/frame 라운드트립 기본 테스트
└── test_lz4_comprehensive.cpp  # 엣지 케이스 종합
tests/bench/
├── bench_lz4.cpp           # 단일 라이브러리 벤치마크
└── bench_lz4_compare.cpp   # liblz4 비교 벤치마크
```

---

## C API

```c
// lz4.h

// block
int orot_lz4_compress_bound(int src_size);
int orot_lz4_compress(const void* src, int src_size,
                      void* dst, int dst_cap,
                      int level);
int orot_lz4_decompress(const void* src, int src_size,
                        void* dst, int dst_cap);

// frame
int orot_lz4f_compress(const void* src, int src_size,
                       void* dst, int dst_cap,
                       int level);
int orot_lz4f_decompress(const void* src, int src_size,
                         void* dst, int dst_cap);
```

반환값: 양수 = 기록된 바이트 수, 음수 = 오류 (-1: 일반 오류, -2: 출력 버퍼 부족).

---

## 빌드 및 검증

```bash
# LZ4 단위 테스트
cmake -B build -DOROT_TESTS=ON
cmake --build build -j
./build/tests/test_lz4
./build/tests/test_lz4_comprehensive

# LZ4 벤치마크
cmake -B build -DOROT_BENCHMARK=ON
cmake --build build -j
./build/tests/bench_lz4

# liblz4 비교 벤치마크 (liblz4 필요)
cmake -B build -DOROT_BENCHMARK_COMPARE=ON
cmake --build build -j
./build/tests/bench_lz4_compare
```
