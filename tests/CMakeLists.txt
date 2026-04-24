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

macro(add_orot_benchmark TARGET SOURCE)
  add_executable(${TARGET} ${SOURCE})
  target_link_libraries(${TARGET} PRIVATE orot)
  target_include_directories(${TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
  target_compile_options(${TARGET} PRIVATE -O3)
endmacro()

# ── Unit tests ────────────────────────────────────────────────────────────────
add_unit_test(test_deflate_roundtrip  unit/test_deflate_roundtrip.cpp)
add_unit_test(test_deflate_huffman    unit/test_deflate_huffman.cpp)
add_unit_test(test_deflate_formats    unit/test_deflate_formats.cpp)
add_unit_test(test_deflate_levels     unit/test_deflate_levels.cpp)
add_unit_test(test_deflate_streaming  unit/test_deflate_streaming.cpp)
add_unit_test(test_deflate_simd       unit/test_deflate_simd.cpp)
add_unit_test(test_deflate_parallel   unit/test_deflate_parallel.cpp)

if(OROT_LZ4)
  add_unit_test(test_lz4 unit/test_lz4.cpp)
  add_unit_test(test_lz4_comprehensive unit/test_lz4_comprehensive.cpp)
endif()

# ── Fuzz targets (opt-in: -DOROT_DEFLATE_FUZZ=ON, requires clang + libFuzzer) ─────
if(OROT_DEFLATE_FUZZ)
  # Build an instrumented copy of the deflate library so the fuzzer can
  # observe coverage inside the decompressor, not just the harness.
  set(OROT_DEFLATE_FUZZ_SOURCES
        ${CMAKE_SOURCE_DIR}/src/deflate/huffman.cpp
        ${CMAKE_SOURCE_DIR}/src/deflate/lz77.cpp
        ${CMAKE_SOURCE_DIR}/src/deflate/deflate_block.cpp
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

# ── Benchmarks (-DOROT_BENCHMARK=ON) ─────────────────────────────────────────
if(OROT_BENCHMARK)
  add_orot_benchmark(bench_deflate bench/bench_deflate.cpp)
  message(STATUS "Deflate: performance benchmark enabled")
  if(OROT_LZ4)
    add_orot_benchmark(bench_lz4 bench/bench_lz4.cpp)
    message(STATUS "LZ4: performance benchmark enabled")
  endif()
endif()

# ── Comparison benchmarks (-DOROT_BENCHMARK_COMPARE=ON) ──────────────────────
if(OROT_BENCHMARK_COMPARE)
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

  add_executable(bench_deflate_compare bench/bench_deflate_compare.cpp)
  target_link_libraries(bench_deflate_compare PRIVATE orot ZLIB::ZLIB ${LIBDEFLATE_LIB})
  target_include_directories(bench_deflate_compare PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${LIBDEFLATE_INCLUDE}
    )
  target_compile_options(bench_deflate_compare PRIVATE -O3)
  message(STATUS "Deflate: comparison benchmark enabled (zlib + libdeflate)")

  if(OROT_LZ4)
    find_library(LZ4_LIB lz4
          HINTS /opt/homebrew/lib /usr/local/lib ${CMAKE_INSTALL_PREFIX}/lib
          PATH_SUFFIXES lib)
    find_path(LZ4_INCLUDE_DIR lz4.h
          HINTS /opt/homebrew/include /usr/local/include ${CMAKE_INSTALL_PREFIX}/include
          PATH_SUFFIXES include)
    if(NOT LZ4_LIB OR NOT LZ4_INCLUDE_DIR)
      message(FATAL_ERROR "liblz4 not found. Install with: brew install lz4")
    endif()

    add_executable(bench_lz4_compare bench/bench_lz4_compare.cpp)
    target_link_libraries(bench_lz4_compare PRIVATE orot ${LZ4_LIB})
    target_include_directories(bench_lz4_compare PRIVATE
          ${CMAKE_SOURCE_DIR}/include
          ${LZ4_INCLUDE_DIR}
      )
    target_compile_options(bench_lz4_compare PRIVATE -O3)
    message(STATUS "LZ4: comparison benchmark enabled (orot + liblz4)")
  endif()
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
