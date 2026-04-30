#!/bin/sh
set -eu

BUILD_DIR="${BUILD_DIR:-build-fuzz}"
CORPUS_DIR="${CORPUS_DIR:-tests/fuzz/corpus/brotli}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)}"
MAX_TOTAL_TIME="${MAX_TOTAL_TIME:-60}"

mkdir -p "$CORPUS_DIR"

printf '\017\001\200abc\003' > "$CORPUS_DIR/q0_abc.br"
printf '\037\026\000\000\044\100\152\020\145\352\360\234\076' > "$CORPUS_DIR/q5_hello.br"
printf '\037\037\000\000\044\302\242\231\100\002' > "$CORPUS_DIR/q5_repeated_a.br"

cmake -S . -B "$BUILD_DIR" \
  -DOROT_TESTS=ON \
  -DOROT_DEFLATE_FUZZ=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build "$BUILD_DIR" --target fuzz_brotli

"$BUILD_DIR/tests/fuzz_brotli" "$CORPUS_DIR" \
  -jobs="$JOBS" \
  -workers="$JOBS" \
  -max_total_time="$MAX_TOTAL_TIME"
