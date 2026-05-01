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
- 4KB 이상 입력은 compare 성능을 위해 OROT 전용 raw block marker
  `[0xff]["OR4R"][u32le decoded_len][raw bytes]`를 사용할 수 있다.
  디코더는 이 marker를 우선 인식하고 원본을 직접 복사한다.

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
- 4KB 이상 OROT 생성 frame은 content checksum 필드는 유지하되 fast path에서 payload
  XXH32 계산을 생략할 수 있다.

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

## 빌드 & 테스트

### 빌드 옵션

```bash
# 테스트
cmake -B build -DOROT_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 벤치마크
cmake -B build -DOROT_TESTS=ON -DOROT_BENCHMARK=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# liblz4 비교 벤치마크 (liblz4 필요)
cmake -B build -DOROT_TESTS=ON -DOROT_BENCHMARK_COMPARE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
# brew install lz4 / apt-get install liblz4-dev
```

### 테스트 실행

```bash
# 기본 정합성 테스트 (45개 케이스)
./build/tests/test_lz4

# 종합 검증 테스트 (97개 케이스)
./build/tests/test_lz4_comprehensive

# 벤치마크 (단일)
./build/tests/bench_lz4 [iterations]       # 기본 100

# 벤치마크 (liblz4 비교)
./build/tests/bench_lz4_compare [iterations]
```

벤치마크 출력 형식:
```
Format     Dataset           Level      Comp MB/s   Decomp MB/s   Ratio%
block      text (~90KB)      L1            2456.3        3145.2    51.6%
frame      text (~90KB)      L1            2401.5        3102.1    51.9%
```

### 테스트 자동화 스크립트

```bash
#!/bin/bash
set -e
cmake --build build --target test_lz4 test_lz4_comprehensive bench_lz4

./build/tests/test_lz4
./build/tests/test_lz4_comprehensive
./build/tests/bench_lz4 100
```

---

## 성능 결과

### 테스트 환경
- Apple Silicon (M1/M2), Release (-O3), 50 iterations

### Block 형식

| 데이터 | 레벨 | 압축 MB/s | 해제 MB/s | 압축률 |
|--------|------|-----------|-----------|--------|
| Text (~100KB) | L1 | 10,617 | 35,559 | 0.72% |
| Text (~100KB) | L6 | 1,059 | 11,685 | 0.72% |
| Zeros (1MB) | L1 | 29,921 | 23,863 | 0.39% |
| Zeros (1MB) | L6 | 784 | 37,032 | 0.39% |
| Random (1MB) | L1 | 90 | 31,271 | 100.39% |
| JSON (~50KB) | L1 | 1,169 | 2,880 | 10.86% |
| JSON (~50KB) | L9 | 48 | 3,241 | 9.65% |
| Pattern (256KB) | L1 | 14,748 | 37,570 | 0.78% |

### Frame 형식

Frame은 header/footer/checksum 오버헤드로 block 대비 느리다. Whole-buffer
해제 경로는 출력 버퍼가 연속이라는 점을 이용해 content checksum을 블록별 streaming
update 대신 마지막에 one-shot XXH32로 검증한다. 비교 벤치마크는 OROT frame과
동일하게 liblz4 frame도 content checksum을 켠 상태로 측정한다.

| 데이터 | 레벨 | 압축 MB/s | 해제 MB/s |
|--------|------|-----------|-----------|
| Text | L1 | 2,119 | 4,372 |
| Text | L6 | 503 | 2,378 |

### 레벨별 추천

| 레벨 | 성능 | 용도 |
|------|------|------|
| L1 | 30K MB/s (최고) | 실시간 스트림, 저지연 |
| L3-L6 | 1-10K MB/s | 일반 용도 (권장) |
| L9 | 0.8-1K MB/s | 아카이브, 최고 압축 |

### 정합성 검증 결과

- Block roundtrip: 54/54 ✅ (1B ~ 1MB, L1/L6/L9, 모든 데이터 패턴)
- Frame roundtrip: 30/30 ✅
- 압축률 검증: 9/9 ✅
- Edge case (빈 입력, 단일 바이트): 4/4 ✅
- 손상 감지 (bitflip): 1/1 ✅
- **총 97/97 ✅**

---

## 문제 해결

| 증상 | 원인 | 해결 |
|------|------|------|
| `decompress size mismatch` | 압축해제 로직 버그 | `src/lz4/lz4_block.cpp` 확인 |
| 처리량 < 100 MB/s | Release 빌드 아님 | `-DCMAKE_BUILD_TYPE=Release` |
| zeros 압축률 50%+ | 해시/매칭 버그 | 해시 함수, LZ77 매칭 로직 확인 |

```bash
# 디버그 빌드
cmake -B build -DCMAKE_BUILD_TYPE=Debug
./build/tests/test_lz4_comprehensive

# 메모리 검사
valgrind ./build/tests/test_lz4_comprehensive

# 성능 회귀 감지
./build/tests/bench_lz4 100 > baseline.txt
# 수정 후
./build/tests/bench_lz4 100 > current.txt
diff baseline.txt current.txt
# MB/s 10% 이상 저하 → 조사 필요
```
