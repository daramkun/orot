# OROT (무손실 압축 라이브러리) 구현 현황

모든 구현 항목 완료. 알고리즘별 상세 내용은 `docs/impl/` 참조.

| 알고리즘 | 문서 | 상태 |
|---------|------|------|
| DEFLATE (LZ77 + Huffman) | [docs/impl/deflate.md](docs/impl/deflate.md) | ✅ |
| LZ4 (block + frame) | [docs/impl/lz4.md](docs/impl/lz4.md) | ✅ |
| LZW (variable-width) | [docs/impl/lzw.md](docs/impl/lzw.md) | ✅ |
| LZMA/LZMA2 (range coding) | [docs/impl/lzma.md](docs/impl/lzma.md) | ✅ |

## 예정: Zstandard (zstd) 구현 계획

Zstandard는 포맷 호환 압축 해제를 먼저 완성한 뒤, 검증 가능한 단위로 압축기와 성능 기능을 확장한다. 신규 구현 시 `docs/impl/zstd.md`를 추가하고, API/테스트/벤치마크는 기존 알고리즘 구조를 따른다.

### 1단계: 골격 및 공개 API

- [ ] `include/orot/zstd.h` 추가
- [ ] `src/zstd/` 내부 모듈 구조 추가
- [ ] `src/api/zstd_api.cpp` C API 진입점 추가
- [ ] `CMakeLists.txt`, `tests/CMakeLists.txt`에 zstd 소스/테스트 연결
- [ ] `orot_zstd_compress_bound`, `orot_zstd_compress`, `orot_zstd_decompress` 기본 시그니처 정의
- [ ] `docs/impl/zstd.md` 초기 문서 추가

### 2단계: Frame 및 Block 파서

- [ ] Zstandard frame magic/header 파싱
- [ ] frame descriptor, window descriptor, content size, dictionary id 처리
- [ ] block header 파싱
- [ ] raw block 압축 해제
- [ ] RLE block 압축 해제
- [ ] checksum 옵션 파싱 및 XXH64 기반 검증
- [ ] malformed frame/block 에러 경로 테스트

### 3단계: Entropy Decoder

- [ ] FSE 테이블 복원 로직 구현
- [ ] FSE bitstream decoder 구현
- [ ] literal Huffman 테이블 복원 로직 구현
- [ ] single-stream/multi-stream Huffman literal decode 구현
- [ ] repeat mode 및 이전 entropy table 상태 관리
- [ ] 독립 FSE/Huffman 단위 테스트 추가

### 4단계: Sequence 및 Window Decoder

- [ ] literals section 파싱
- [ ] sequences section 파싱
- [ ] literal length, match length, offset code decode
- [ ] repeated offset rules 구현
- [ ] history/window copy 구현
- [ ] block 간 window 유지
- [ ] 공식 zstd로 생성한 샘플 압축 해제 호환 테스트 추가

### 5단계: Decompressor 완성

- [ ] `orot_zstd_decompress` 전체 frame 처리 완성
- [ ] content size known/unknown 케이스 처리
- [ ] skippable frame 처리
- [ ] dictionary 미지원 케이스 명확한 에러 반환
- [ ] boundary, truncated input, dst 부족 케이스 테스트
- [ ] libzstd CLI/라이브러리 출력물과 교차 호환 테스트 추가

### 6단계: 기본 Compressor

- [ ] zstd frame writer 구현
- [ ] raw block encoder 구현
- [ ] RLE block encoder 구현
- [ ] 단순 hash-table 기반 LZ77 match finder 구현
- [ ] literals/sequences 생성
- [ ] 기본 entropy encoding 경로 구현
- [ ] `orot_zstd_compress` 라운드트립 테스트 추가

### 7단계: 압축률 및 레벨 확장

- [ ] 압축 레벨 1-9 매핑 정의
- [ ] greedy/lazy match 전략 추가
- [ ] hash chain 또는 binary tree match finder 추가
- [ ] FSE/Huffman table 선택 최적화
- [ ] incompressible data 감지 및 raw block fallback
- [ ] 대용량 입력 chunking 및 window 정책 개선

### 8단계: Dictionary 및 Streaming

- [ ] dictionary id/header 처리
- [ ] raw content dictionary 압축 해제 지원
- [ ] dictionary 기반 압축 지원
- [ ] streaming decompress API 설계 및 구현
- [ ] streaming compress API 설계 및 구현
- [ ] dictionary/streaming 호환성 테스트 추가

### 9단계: 검증, 벤치마크, 문서화

- [ ] zstd 전용 유닛 테스트 확장
- [ ] fuzz target 추가
- [ ] `tests/bench/bench_zstd.cpp` 추가
- [ ] `tests/bench/bench_zstd_compare.cpp`로 libzstd 비교 벤치마크 추가
- [ ] README 기능/빌드/테스트 섹션 갱신
- [ ] `.claude/rules/structure.md` 프로젝트 구조 갱신
- [ ] `docs/impl/zstd.md` 구현 상세 문서 완성
