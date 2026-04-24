# LZ4 검증 및 성능 측정 빠른 시작 가이드

LZ4 구현의 정합성(correctness)과 성능(performance)을 빠르게 검증하는 방법을 설명합니다.

## 30초 빠른 테스트

```bash
cd orot
mkdir -p build && cd build
cmake -DOROT_TESTS=ON -DOROT_LZ4=ON -DOROT_BENCHMARK=ON -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target test_lz4_comprehensive bench_lz4
./tests/test_lz4_comprehensive
```

**예상 결과:**
```
✓ ALL TESTS PASSED (97/97)
```

## 세 가지 테스트 스위트

### 1. 기본 정합성 테스트 (test_lz4)
**목적:** LZ4 block/frame 형식의 기본 기능 검증

```bash
./tests/test_lz4
```

**확인 사항:**
- Block 형식 roundtrip (L1-L9)
- Frame 형식 roundtrip
- Edge case (빈 입력, 단일 바이트 등)
- 손상된 입력 처리

**통과 기준:**
```
All LZ4 tests passed.
```

---

### 2. 종합 검증 테스트 (test_lz4_comprehensive)
**목적:** 정합성, 압축률, 성능 특성 상세 검증

```bash
./tests/test_lz4_comprehensive
```

**테스트 항목:**
- ✓ Block/Frame roundtrip (1B ~ 1MB)
- ✓ 다양한 데이터 패턴 (zeros, text, random, repeating)
- ✓ 모든 압축 레벨 (L1-L9)
- ✓ Edge case (빈 입력, 고반복도 데이터)
- ✓ 손상 감지 (bitflip 처리)
- ✓ 성능 프로파일 (다양한 크기별 처리량)

**통과 기준:**
```
✓ ALL TESTS PASSED (97/97)
```

**예상 성능:**
```
Throughput (compression at L6):
    64 KB: 400-700 MB/s
   256 KB: 400-900 MB/s
     1 MB: 400-700 MB/s
```

---

### 3. 성능 벤치마크 (bench_lz4, bench_lz4_compare)
**목적:** 압축/해제 처리량과 압축률 측정

```bash
# 50 반복 (빠름, ~10초)
./tests/bench_lz4 50

# 200 반복 (정확함, ~40초)
./tests/bench_lz4 200

# 500 반복 (매우 정확함, ~100초)
./tests/bench_lz4 500
```

`liblz4`와 직접 비교하려면:

```bash
cmake -DOROT_TESTS=ON -DOROT_LZ4=ON -DOROT_BENCHMARK_COMPARE=ON -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target bench_lz4_compare
./tests/bench_lz4_compare 100
```

**출력 분석:**

#### 단일 라이브러리 출력 형식
```
Format     Dataset           Level      Comp MB/s   Decomp MB/s   Ratio%
block      text (~90KB)      L1            2456.3        3145.2    51.6%
frame      text (~90KB)      L1            2401.5        3102.1    51.9%
```

#### 비교 벤치 출력 형식
```
Library    Format   Dataset           Level      Comp MB/s   Decomp MB/s   Ratio%    CPU ms  RSS dKB
orot       block    text (~90KB)      L1            2400.0        3100.0    51.6%     0.040        0
liblz4     block    text (~90KB)      L1            2550.0        3200.0    50.9%     0.038        0
```

**성능 해석:**
- 압축이 해제보다 느림 (정상, LZ4는 빠른 압축이 목표)
- Zeros는 가장 빠르게 압축됨 (높은 반복도)
- Random은 느리게 압축됨 (압축 불가능 데이터)
- 해제는 모든 경우에 빠름 (LZ4의 특징)

---

## 성능 기준표

| 시나리오 | 압축 MB/s | 해제 MB/s | 압축률 | 평가 |
|---------|-----------|-----------|--------|------|
| **Zeros** | 1000+ | 8000+ | <1% | ✓ 우수 |
| **Text** | 300+ | 1500+ | 15-30% | ✓ 우수 |
| **JSON** | 100+ | 1000+ | 10-15% | ✓ 우수 |
| **Random** | 50+ | 1000+ | ~100% | ✓ 예상대로 |

---

## 테스트 결과 해석

### ✓ 모든 테스트 통과하는 경우

**무엇을 의미하는가?**
- LZ4 구현이 정합성 있음
- 압축/해제가 올바르게 작동
- 성능이 합리적 수준
- Edge case 처리 정상

**다음 단계:**
- 프로덕션 배포 가능
- 다른 시스템과의 호환성 검증 시작

### ✗ 테스트 실패하는 경우

**문제 진단:**

1. **Roundtrip 실패** (`decompress size mismatch`)
   ```
   원인: 압축/해제 로직 버그
   확인: src/lz4/lz4_block.cpp 또는 lz4_frame.cpp
   ```

2. **압축률 저하** (예: zeros가 50% 이상)
   ```
   원인: 압축 알고리즘 문제
   확인: 해시 함수, LZ77 매칭 로직
   ```

3. **성능 저하** (MB/s가 매우 낮음)
   ```
   원인: 최적화 플래그 부족
   해결: cmake -DCMAKE_BUILD_TYPE=Release
   ```

4. **Edge case 처리 실패** (단일 바이트, 빈 입력)
   ```
   원인: 경계 조건 처리 누락
   확인: 최소 크기 입력 처리 로직
   ```

---

## 데이터 패턴별 기대 결과

### Zeros (1 MB of 0x00)
```
압축률:  0.3-0.5%  (매우 우수)
처리량:  1000+ MB/s (압축), 8000+ MB/s (해제)
특징:    LZ4의 최적 시나리오
```

### Text (반복적인 문장)
```
압축률:  0.5-2%    (우수)
처리량:  300-500 MB/s (압축), 1500+ MB/s (해제)
특징:    실제 사용 사례
```

### JSON (구조화된 데이터)
```
압축률:  10-15%    (우수)
처리량:  100-500 MB/s (압축), 1000+ MB/s (해제)
특징:    실무 데이터
```

### Random (무작위 데이터)
```
압축률:  100-102%  (압축 불가능)
처리량:  50-100 MB/s (압축), 1000+ MB/s (해제)
특징:    최악의 경우
```

---

## 자동화 테스트 스크립트

프로젝트 CI/CD에 추가할 수 있는 스크립트:

```bash
#!/bin/bash
# test_lz4_all.sh

set -e

echo "╔════════════════════════════════════════════╗"
echo "║   LZ4 Full Validation Suite                ║"
echo "╚════════════════════════════════════════════╝"

# 빌드
echo ""
echo "📦 Building..."
cd build
cmake --build . --target test_lz4 test_lz4_comprehensive bench_lz4

# 테스트 1: 기본 정합성
echo ""
echo "🧪 Test 1: Basic Correctness"
./tests/test_lz4 > /tmp/test_lz4.log
if grep -q "All LZ4 tests passed" /tmp/test_lz4.log; then
    echo "✓ PASSED"
else
    echo "✗ FAILED"
    cat /tmp/test_lz4.log
    exit 1
fi

# 테스트 2: 종합 검증
echo ""
echo "🧪 Test 2: Comprehensive Validation"
./tests/test_lz4_comprehensive > /tmp/test_comprehensive.log
if grep -q "ALL TESTS PASSED" /tmp/test_comprehensive.log; then
    echo "✓ PASSED"
else
    echo "✗ FAILED"
    cat /tmp/test_comprehensive.log
    exit 1
fi

# 테스트 3: 성능 벤치마크
echo ""
echo "⚡ Test 3: Performance Benchmark"
./tests/bench_lz4 100 > /tmp/bench_lz4.log

# 결과 요약
echo ""
echo "📊 Benchmark Results Summary:"
grep "text.*L1.*MB/s" /tmp/bench_lz4.log | head -1 || echo "  (results available in /tmp/bench_lz4.log)"

echo ""
echo "╔════════════════════════════════════════════╗"
echo "║  ✓ All validations completed successfully  ║"
echo "╚════════════════════════════════════════════╝"
```

실행:
```bash
chmod +x test_lz4_all.sh
./test_lz4_all.sh
```

---

## 성능 회귀 감지

변경 후 성능 비교:

```bash
# 기준선 측정
./tests/bench_lz4 100 > baseline.txt

# 코드 수정 후
./tests/bench_lz4 100 > current.txt

# 비교
diff baseline.txt current.txt

# 상세 분석
grep "MB/s" baseline.txt > base_perf.txt
grep "MB/s" current.txt > curr_perf.txt
paste base_perf.txt curr_perf.txt
```

**회귀 기준:** MB/s 10% 이상 저하 → 조사 필요

---

## 문제 해결 가이드

### 빌드 실패
```bash
# LZ4 지원 확인
cmake -DOROT_LZ4=ON -DOROT_BENCHMARK=ON ..

# 컴파일러 확인
cmake --build . -v 2>&1 | grep error

# 의존성 확인
pkg-config --cflags --libs liblz4
```

### 테스트 실패
```bash
# 상세 출력
./tests/test_lz4_comprehensive 2>&1 | head -100

# 메모리 검사
valgrind ./tests/test_lz4_comprehensive

# 디버그 빌드
cmake -DCMAKE_BUILD_TYPE=Debug ..
./tests/test_lz4_comprehensive
```

### 성능 저하
```bash
# 최적화 확인
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j$(nproc)

# CPU 성능 모드 확인 (macOS)
sysctl -a | grep hw.

# 백그라운드 프로세스 확인
ps aux | grep -E "chrome|node"
```

---

## 다음 단계

✅ 모든 테스트 통과 후:

1. **호환성 검증**
   - 공식 lz4 도구와 호환성 확인
   - ```bash
     brew install lz4  # 또는 apt-get install lz4
     ```

2. **통합 테스트**
   - 프로덕션 데이터로 테스트
   - 다양한 플랫폼에서 검증

3. **배포**
   - CI/CD 파이프라인에 추가
   - 버전 관리 및 릴리스 노트 작성

---

## 참고 자료

- **LZ4 공식 문서:** https://github.com/lz4/lz4
- **테스트 상세 가이드:** `docs/LZ4_TESTING.md`
- **구현 상세:** `src/lz4/lz4_block.cpp`, `src/lz4/lz4_frame.cpp`
- **API 문서:** `include/orot/lz4.h`

---

## 일반적인 질문 (FAQ)

**Q: 모든 테스트가 통과하면 구현이 완벽한가?**
A: 정합성과 성능이 확인되었다는 의미입니다. 프로덕션 준비 상태이지만, 
   특정 use case에 대해 추가 검증이 필요할 수 있습니다.

**Q: 압축률이 다른 LZ4와 다른 이유?**
A: LZ4 스펙 내에서 구현 차이가 발생합니다. 정합성만 보장되면 정상입니다.

**Q: 성능이 기준표보다 낮으면?**
A: 시스템 환경 (CPU, 메모리), 컴파일 최적화, 백그라운드 프로세스 등을 확인하세요.

**Q: 특정 데이터로 테스트하려면?**
A: `bench_lz4.cpp`를 수정하여 커스텀 데이터 생성 함수를 추가하세요.

```
