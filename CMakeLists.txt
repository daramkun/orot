cmake_minimum_required(VERSION 3.20)
project(orot VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(OROT_DEFLATE_SIMD         "Enable SIMD acceleration"                         ON)
option(OROT_DEFLATE_THREADS      "Enable parallel compression"                       ON)
option(OROT_DEFLATE_TESTS        "Build unit tests"                                  OFF)
option(OROT_DEFLATE_BENCH        "Build benchmarks"                                  OFF)
option(OROT_DEFLATE_FUZZ         "Build fuzz targets"                                OFF)
option(OROT_DEFLATE_SHARED       "Build shared library"                              OFF)
option(OROT_DEFLATE_ZLIB_COMPAT  "Build zlib-compatible C aliases"                   ON)
option(OROT_DEFLATE_COMPARE_BENCH "Build comparison benchmarks (needs zlib+libdeflate)" OFF)
option(OROT_DEFLATE_COMPAT_TEST  "Cross-library compatibility tests (needs zlib+libdeflate)" OFF)
option(OROT_LZ4                  "Enable LZ4 compression support"                           ON)
option(OROT_LZ4_BENCH            "Build LZ4 benchmarks"                               OFF)
option(OROT_LZ4_COMPARE_BENCH    "Build LZ4 comparison benchmarks (needs liblz4)"     OFF)

include(cmake/DetectSIMD.cmake)
include(cmake/CompilerFlags.cmake)
include(cmake/InstallConfig.cmake)

# ── Core sources (no special SIMD flags) ─────────────────────────────────────
set(OROT_DEFLATE_CORE_SOURCES
    src/deflate/huffman.cpp
    src/deflate/lz77.cpp
    src/deflate/deflate_block.cpp
    src/compress/compressor.cpp
    src/compress/block_compressor.cpp
    src/decompress/decompressor.cpp
    src/decompress/inflate_fast.cpp
    src/formats/raw_deflate.cpp
    src/formats/zlib_wrapper.cpp
    src/formats/gzip_wrapper.cpp
    src/api/c_api.cpp
    src/api/stream_api.cpp
)

if(OROT_DEFLATE_THREADS)
  list(APPEND OROT_DEFLATE_CORE_SOURCES
        src/parallel/thread_pool.cpp
        src/parallel/parallel_compressor.cpp
        src/decompress/parallel_decompress.cpp
    )
endif()

set(OROT_LZ4_SOURCES
    src/lz4/lz4_block.cpp
    src/lz4/lz4_frame.cpp
    src/api/lz4_api.cpp
)

# ── SIMD object libraries (each with its own compile flags) ──────────────────
set(OROT_DEFLATE_SIMD_OBJECTS "")

if(OROT_DEFLATE_SIMD)
  add_library(orot_deflate_simd_detect OBJECT src/simd/simd_detect.cpp)
  target_include_directories(orot_deflate_simd_detect PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
  target_compile_definitions(orot_deflate_simd_detect PRIVATE
        $<$<BOOL:${DEFLATE_HAS_SSE2}>:DEFLATE_HAS_SSE2>
        $<$<BOOL:${DEFLATE_HAS_SSE42}>:DEFLATE_HAS_SSE42>
        $<$<BOOL:${DEFLATE_HAS_AVX2}>:DEFLATE_HAS_AVX2>
        $<$<BOOL:${DEFLATE_HAS_NEON}>:DEFLATE_HAS_NEON>
        $<$<BOOL:${DEFLATE_HAS_CRC_ARM}>:DEFLATE_HAS_CRC_ARM>
    )
  list(APPEND OROT_DEFLATE_SIMD_OBJECTS $<TARGET_OBJECTS:orot_deflate_simd_detect>)

  if(DEFLATE_HAS_SSE2)
    add_library(orot_deflate_simd_sse2 OBJECT
            src/simd/x86/hash_sse2.cpp
        )
    target_include_directories(orot_deflate_simd_sse2 PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
        )
    target_compile_options(orot_deflate_simd_sse2 PRIVATE -msse2)
    target_compile_definitions(orot_deflate_simd_sse2 PRIVATE DEFLATE_HAS_SSE2)
    list(APPEND OROT_DEFLATE_SIMD_OBJECTS $<TARGET_OBJECTS:orot_deflate_simd_sse2>)
  endif()

  if(DEFLATE_HAS_SSE42)
    add_library(orot_deflate_simd_sse42 OBJECT
            src/simd/x86/match_sse42.cpp
            src/simd/x86/crc32_sse42.cpp
        )
    target_include_directories(orot_deflate_simd_sse42 PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
        )
    target_compile_options(orot_deflate_simd_sse42 PRIVATE -msse4.2)
    target_compile_definitions(orot_deflate_simd_sse42 PRIVATE DEFLATE_HAS_SSE42)
    list(APPEND OROT_DEFLATE_SIMD_OBJECTS $<TARGET_OBJECTS:orot_deflate_simd_sse42>)
  endif()

  if(DEFLATE_HAS_AVX2)
    add_library(orot_deflate_simd_avx2 OBJECT
            src/simd/x86/hash_avx2.cpp
            src/simd/x86/adler32_avx2.cpp
        )
    target_include_directories(orot_deflate_simd_avx2 PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
        )
    target_compile_options(orot_deflate_simd_avx2 PRIVATE -mavx2)
    target_compile_definitions(orot_deflate_simd_avx2 PRIVATE DEFLATE_HAS_AVX2)
    list(APPEND OROT_DEFLATE_SIMD_OBJECTS $<TARGET_OBJECTS:orot_deflate_simd_avx2>)
  endif()

  if(DEFLATE_HAS_NEON OR DEFLATE_HAS_CRC_ARM)
    set(OROT_DEFLATE_ARM_NEON_SOURCES
            src/simd/arm/hash_neon.cpp
            src/simd/arm/match_neon.cpp
            src/simd/arm/crc32_arm.cpp
        )
    add_library(orot_deflate_simd_neon OBJECT ${OROT_DEFLATE_ARM_NEON_SOURCES})
    target_include_directories(orot_deflate_simd_neon PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
        )
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
      target_compile_options(orot_deflate_simd_neon PRIVATE -mcpu=apple-m1)
    else()
      target_compile_options(orot_deflate_simd_neon PRIVATE -march=armv8-a+crc+simd)
    endif()
    target_compile_definitions(orot_deflate_simd_neon PRIVATE
            $<$<BOOL:${DEFLATE_HAS_NEON}>:DEFLATE_HAS_NEON>
            $<$<BOOL:${DEFLATE_HAS_CRC_ARM}>:DEFLATE_HAS_CRC_ARM>
        )
    list(APPEND OROT_DEFLATE_SIMD_OBJECTS $<TARGET_OBJECTS:orot_deflate_simd_neon>)
  endif()
endif()

# ── Main library target ───────────────────────────────────────────────────────
if(OROT_DEFLATE_SHARED)
  add_library(orot SHARED ${OROT_DEFLATE_CORE_SOURCES}
        $<$<BOOL:${OROT_LZ4}>:${OROT_LZ4_SOURCES}>
        ${OROT_DEFLATE_SIMD_OBJECTS})
else()
  add_library(orot STATIC ${OROT_DEFLATE_CORE_SOURCES}
        $<$<BOOL:${OROT_LZ4}>:${OROT_LZ4_SOURCES}>
        ${OROT_DEFLATE_SIMD_OBJECTS})
endif()

target_include_directories(orot
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(orot PRIVATE
    $<$<BOOL:${OROT_DEFLATE_SIMD}>:DEFLATE_SIMD_ENABLED>
    $<$<BOOL:${DEFLATE_HAS_SSE2}>:DEFLATE_HAS_SSE2>
    $<$<BOOL:${DEFLATE_HAS_SSE42}>:DEFLATE_HAS_SSE42>
    $<$<BOOL:${DEFLATE_HAS_AVX2}>:DEFLATE_HAS_AVX2>
    $<$<BOOL:${DEFLATE_HAS_NEON}>:DEFLATE_HAS_NEON>
    $<$<BOOL:${DEFLATE_HAS_CRC_ARM}>:DEFLATE_HAS_CRC_ARM>
    $<$<BOOL:${OROT_DEFLATE_THREADS}>:DEFLATE_THREADS_ENABLED>
    $<$<BOOL:${OROT_DEFLATE_ZLIB_COMPAT}>:DEFLATE_ZLIB_COMPAT>
    $<$<BOOL:${OROT_LZ4}>:OROT_HAS_LZ4>
)

deflate_set_compiler_flags(orot)

if(OROT_DEFLATE_THREADS)
  find_package(Threads REQUIRED)
  target_link_libraries(orot PRIVATE Threads::Threads)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(OROT_DEFLATE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
deflate_install(orot)
