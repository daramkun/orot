cmake_minimum_required(VERSION 3.20)

# ── Helper macro ──────────────────────────────────────────────────────────────
macro(add_unit_test TARGET SOURCE)
  add_executable(${TARGET} ${SOURCE})
  target_link_libraries(${TARGET} PRIVATE orot)
  target_include_directories(${TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
  add_test(NAME ${TARGET} COMMAND ${TARGET})
endmacro()

# ── Unit tests ────────────────────────────────────────────────────────────────
add_unit_test(test_roundtrip  unit/test_roundtrip.cpp)
add_unit_test(test_huffman    unit/test_huffman.cpp)
add_unit_test(test_formats    unit/test_formats.cpp)
add_unit_test(test_levels     unit/test_levels.cpp)
add_unit_test(test_streaming  unit/test_streaming.cpp)
add_unit_test(test_simd       unit/test_simd.cpp)
add_unit_test(test_parallel   unit/test_parallel.cpp)

if(OROT_LZ4)
  add_unit_test(test_lz4 unit/test_lz4.cpp)
  add_unit_test(test_lz4_comprehensive unit/test_lz4_comprehensive.cpp)
endif()

# ── Fuzz targets (opt-in: -DOROT_DEFLATE_FUZZ=ON, requires clang + libFuzzer) ─────
if(OROT_DEFLATE_FUZZ)
  # Build an instrumented copy of the deflate library so the fuzzer can
  # observe coverage inside the decompressor, not just the harness.
  set(OROT_DEFLATE_FUZZ_SOURCES
        ${CMAKE_SOURCE_DIR}/src/core/huffman.cpp
        ${CMAKE_SOURCE_DIR}/src/core/lz77.cpp
        ${CMAKE_SOURCE_DIR}/src/core/deflate_block.cpp
        ${CMAKE_SOURCE_DIR}/src/compress/compressor.cpp
        ${CMAKE_SOURCE_DIR}/src/compress/block_compressor.cpp
        ${CMAKE_SOURCE_DIR}/src/decompress/decompressor.cpp
        ${CMAKE_SOURCE_DIR}/src/decompress/inflate_fast.cpp
        ${CMAKE_SOURCE_DIR}/src/formats/raw_deflate.cpp
        ${CMAKE_SOURCE_DIR}/src/formats/zlib_wrapper.cpp
        ${CMAKE_SOURCE_DIR}/src/formats/gzip_wrapper.cpp
        ${CMAKE_SOURCE_DIR}/src/api/c_api.cpp
        ${CMAKE_SOURCE_DIR}/src/api/stream_api.cpp
        # simd_detect without any SIMD macros → pure scalar fallbacks
        ${CMAKE_SOURCE_DIR}/src/simd/simd_detect.cpp
    )

  add_library(orot_instrumented STATIC ${OROT_DEFLATE_FUZZ_SOURCES})
  target_include_directories(orot_instrumented PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
  target_compile_options(orot_instrumented PRIVATE
        -fsanitize=fuzzer-no-link,address -O1 -g
    )
  # Disable SIMD for instrumented build (keeps it simple)
  target_compile_definitions(orot_instrumented PRIVATE
        DEFLATE_NO_SIMD=1
    )

  foreach(FUZZ_TARGET fuzz_decompress fuzz_roundtrip)
    add_executable(${FUZZ_TARGET} fuzz/${FUZZ_TARGET}.cpp)
    target_link_libraries(${FUZZ_TARGET} PRIVATE orot_instrumented)
    target_include_directories(${FUZZ_TARGET} PRIVATE
            ${CMAKE_SOURCE_DIR}/include
        )
    target_compile_options(${FUZZ_TARGET} PRIVATE
            -fsanitize=fuzzer,address -O1 -g
        )
    target_link_options(${FUZZ_TARGET} PRIVATE
            -fsanitize=fuzzer,address
        )
  endforeach()
endif()

# ── Benchmark (opt-in: -DOROT_DEFLATE_BENCH=ON) ───────────────────────────────────
if(OROT_DEFLATE_BENCH)
  add_executable(bench_compress bench/bench_compress.cpp)
  target_link_libraries(bench_compress PRIVATE orot)
  target_include_directories(bench_compress PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
  target_compile_options(bench_compress PRIVATE -O3)
endif()

# ── LZ4 Benchmark (opt-in: -DOROT_LZ4_BENCH=ON) ────────────────────────────────────
if(OROT_LZ4_BENCH)
  add_executable(bench_lz4 bench/bench_lz4.cpp)
  target_link_libraries(bench_lz4 PRIVATE orot)
  target_include_directories(bench_lz4 PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
  target_compile_options(bench_lz4 PRIVATE -O3)
  message(STATUS "LZ4: performance benchmark enabled")
endif()

# ── Comparison benchmark (opt-in: -DOROT_DEFLATE_COMPARE_BENCH=ON, needs zlib+libdeflate) ──
if(OROT_DEFLATE_COMPARE_BENCH)
  find_package(ZLIB REQUIRED)
  find_library(LIBDEFLATE_LIB deflate
        HINTS /opt/homebrew/lib ${CMAKE_INSTALL_PREFIX}/lib
        PATH_SUFFIXES lib)
  find_path(LIBDEFLATE_INCLUDE libdeflate.h
        HINTS /opt/homebrew/include ${CMAKE_INSTALL_PREFIX}/include
        PATH_SUFFIXES include)
  if(NOT LIBDEFLATE_LIB OR NOT LIBDEFLATE_INCLUDE)
    message(FATAL_ERROR "libdeflate not found. Install with: brew install libdeflate")
  endif()

  add_executable(bench_compare bench/bench_compare.cpp)
  target_link_libraries(bench_compare PRIVATE orot ZLIB::ZLIB ${LIBDEFLATE_LIB})
  target_include_directories(bench_compare PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${LIBDEFLATE_INCLUDE}
    )
  target_compile_options(bench_compare PRIVATE -O3)
  message(STATUS "Deflate: comparison benchmark enabled (zlib + libdeflate)")
endif()

# ── Cross-library compatibility tests (opt-in: -DOROT_DEFLATE_COMPAT_TEST=ON) ────────────
if(OROT_DEFLATE_COMPAT_TEST)
  find_package(ZLIB REQUIRED)
  find_library(LIBDEFLATE_LIB deflate
        HINTS /opt/homebrew/lib ${CMAKE_INSTALL_PREFIX}/lib
        PATH_SUFFIXES lib)
  find_path(LIBDEFLATE_INCLUDE libdeflate.h
        HINTS /opt/homebrew/include ${CMAKE_INSTALL_PREFIX}/include
        PATH_SUFFIXES include)
  if(NOT LIBDEFLATE_LIB OR NOT LIBDEFLATE_INCLUDE)
    message(FATAL_ERROR "libdeflate not found. Install with: brew install libdeflate")
  endif()

  add_subdirectory(compat)
  message(STATUS "Deflate: cross-library compatibility tests enabled")
endif()
