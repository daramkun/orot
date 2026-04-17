cmake_minimum_required(VERSION 3.20)
project(deflate VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Options
option(DEFLATE_SIMD         "Enable SIMD acceleration"                         ON)
option(DEFLATE_THREADS      "Enable parallel compression"                       ON)
option(DEFLATE_TESTS        "Build unit tests"                                  OFF)
option(DEFLATE_BENCH        "Build benchmarks"                                  OFF)
option(DEFLATE_FUZZ         "Build fuzz targets"                                OFF)
option(DEFLATE_SHARED       "Build shared library"                              OFF)
option(DEFLATE_ZLIB_COMPAT  "Build zlib-compatible C aliases"                   ON)
option(DEFLATE_COMPARE_BENCH "Build comparison benchmarks (needs zlib+libdeflate)" OFF)
option(DEFLATE_COMPAT_TEST  "Cross-library compatibility tests (needs zlib+libdeflate)" OFF)

include(cmake/DetectSIMD.cmake)
include(cmake/CompilerFlags.cmake)
include(cmake/InstallConfig.cmake)

# ── Core sources (no special SIMD flags) ─────────────────────────────────────
set(DEFLATE_CORE_SOURCES
    src/core/huffman.cpp
    src/core/lz77.cpp
    src/core/deflate_block.cpp
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

if(DEFLATE_THREADS)
    list(APPEND DEFLATE_CORE_SOURCES
        src/parallel/thread_pool.cpp
        src/parallel/parallel_compressor.cpp
    )
endif()

# ── SIMD object libraries (each with its own compile flags) ──────────────────
set(DEFLATE_SIMD_OBJECTS "")

if(DEFLATE_SIMD)
    add_library(deflate_simd_detect OBJECT src/simd/simd_detect.cpp)
    target_include_directories(deflate_simd_detect PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_definitions(deflate_simd_detect PRIVATE
        $<$<BOOL:${DEFLATE_HAS_SSE2}>:DEFLATE_HAS_SSE2>
        $<$<BOOL:${DEFLATE_HAS_SSE42}>:DEFLATE_HAS_SSE42>
        $<$<BOOL:${DEFLATE_HAS_AVX2}>:DEFLATE_HAS_AVX2>
        $<$<BOOL:${DEFLATE_HAS_NEON}>:DEFLATE_HAS_NEON>
        $<$<BOOL:${DEFLATE_HAS_CRC_ARM}>:DEFLATE_HAS_CRC_ARM>
    )
    list(APPEND DEFLATE_SIMD_OBJECTS $<TARGET_OBJECTS:deflate_simd_detect>)

    if(DEFLATE_HAS_SSE2)
        add_library(deflate_simd_sse2 OBJECT
            src/simd/x86/hash_sse2.cpp
        )
        target_include_directories(deflate_simd_sse2 PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_options(deflate_simd_sse2 PRIVATE -msse2)
        list(APPEND DEFLATE_SIMD_OBJECTS $<TARGET_OBJECTS:deflate_simd_sse2>)
    endif()

    if(DEFLATE_HAS_SSE42)
        add_library(deflate_simd_sse42 OBJECT
            src/simd/x86/match_sse42.cpp
            src/simd/x86/crc32_sse42.cpp
        )
        target_include_directories(deflate_simd_sse42 PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_options(deflate_simd_sse42 PRIVATE -msse4.2)
        list(APPEND DEFLATE_SIMD_OBJECTS $<TARGET_OBJECTS:deflate_simd_sse42>)
    endif()

    if(DEFLATE_HAS_AVX2)
        add_library(deflate_simd_avx2 OBJECT
            src/simd/x86/hash_avx2.cpp
            src/simd/x86/adler32_avx2.cpp
        )
        target_include_directories(deflate_simd_avx2 PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_options(deflate_simd_avx2 PRIVATE -mavx2)
        list(APPEND DEFLATE_SIMD_OBJECTS $<TARGET_OBJECTS:deflate_simd_avx2>)
    endif()

    if(DEFLATE_HAS_NEON OR DEFLATE_HAS_CRC_ARM)
        set(DEFLATE_ARM_NEON_SOURCES
            src/simd/arm/hash_neon.cpp
            src/simd/arm/match_neon.cpp
            src/simd/arm/crc32_arm.cpp
        )
        add_library(deflate_simd_neon OBJECT ${DEFLATE_ARM_NEON_SOURCES})
        target_include_directories(deflate_simd_neon PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/src
        )
        if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
            target_compile_options(deflate_simd_neon PRIVATE -mcpu=apple-m1)
        else()
            target_compile_options(deflate_simd_neon PRIVATE -march=armv8-a+crc+simd)
        endif()
        target_compile_definitions(deflate_simd_neon PRIVATE
            $<$<BOOL:${DEFLATE_HAS_NEON}>:DEFLATE_HAS_NEON>
            $<$<BOOL:${DEFLATE_HAS_CRC_ARM}>:DEFLATE_HAS_CRC_ARM>
        )
        list(APPEND DEFLATE_SIMD_OBJECTS $<TARGET_OBJECTS:deflate_simd_neon>)
    endif()
endif()

# ── Main library target ───────────────────────────────────────────────────────
if(DEFLATE_SHARED)
    add_library(deflate SHARED ${DEFLATE_CORE_SOURCES} ${DEFLATE_SIMD_OBJECTS})
else()
    add_library(deflate STATIC ${DEFLATE_CORE_SOURCES} ${DEFLATE_SIMD_OBJECTS})
endif()

target_include_directories(deflate
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(deflate PRIVATE
    $<$<BOOL:${DEFLATE_SIMD}>:DEFLATE_SIMD_ENABLED>
    $<$<BOOL:${DEFLATE_HAS_SSE2}>:DEFLATE_HAS_SSE2>
    $<$<BOOL:${DEFLATE_HAS_SSE42}>:DEFLATE_HAS_SSE42>
    $<$<BOOL:${DEFLATE_HAS_AVX2}>:DEFLATE_HAS_AVX2>
    $<$<BOOL:${DEFLATE_HAS_NEON}>:DEFLATE_HAS_NEON>
    $<$<BOOL:${DEFLATE_HAS_CRC_ARM}>:DEFLATE_HAS_CRC_ARM>
    $<$<BOOL:${DEFLATE_THREADS}>:DEFLATE_THREADS_ENABLED>
    $<$<BOOL:${DEFLATE_ZLIB_COMPAT}>:DEFLATE_ZLIB_COMPAT>
)

deflate_set_compiler_flags(deflate)

if(DEFLATE_THREADS)
    find_package(Threads REQUIRED)
    target_link_libraries(deflate PRIVATE Threads::Threads)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(DEFLATE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
deflate_install(deflate)
