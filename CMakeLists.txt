cmake_minimum_required(VERSION 3.20)
project(orot VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(OROT_USE_SIMD             "Enable SIMD acceleration"                          ON)
option(OROT_AS_PARALLEL          "Enable parallel compression"                       ON)
option(OROT_TESTS                "Build unit tests"                                  ON)
option(OROT_BENCHMARK            "Build benchmarks"                                  ON)
option(OROT_BENCHMARK_COMPARE    "Build comparison benchmarks (needs zlib+libdeflate+liblz4)" ON)
option(OROT_DEFLATE_FUZZ         "Build fuzz targets"                                OFF)
option(OROT_SHARED               "Build shared library"                              OFF)
option(OROT_DEFLATE_ZLIB_COMPAT  "Build zlib-compatible C aliases"                   ON)
option(OROT_USE_SYSTEM_BACKENDS  "Use optional system codec libraries for compatible whole-buffer acceleration" ON)

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
    src/api/deflate_api.cpp
    src/api/stream_api.cpp
)

if(OROT_AS_PARALLEL)
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

set(OROT_LZW_SOURCES
    src/lzw/lzw_block.cpp
    src/api/lzw_api.cpp
)

set(OROT_LZMA_SOURCES
    src/lzma/lzma_compress.cpp
    src/lzma/lzma_decompress.cpp
    src/lzma/lzma2_compress.cpp
    src/lzma/lzma2_decompress.cpp
    src/api/lzma_api.cpp
)

set(OROT_BZIP2_SOURCES
    src/bzip2/bwt.cpp
    src/bzip2/bzip2_huffman.cpp
    src/bzip2/bzip2_compress.cpp
    src/bzip2/bzip2_decompress.cpp
    src/api/bzip2_api.cpp
)

set(OROT_ZSTD_SOURCES
    src/zstd/fse.cpp
    src/zstd/huf.cpp
    src/zstd/zstd.cpp
    src/api/zstd_api.cpp
)

set(OROT_BROTLI_SOURCES
    src/brotli/brotli_huffman.cpp
    src/brotli/brotli_meta.cpp
    src/brotli/brotli_dictionary.cpp
    src/brotli/brotli_compress.cpp
    src/brotli/brotli_decompress.cpp
    src/api/brotli_api.cpp
)

# ── SIMD object libraries (each with its own compile flags) ──────────────────
set(OROT_DEFLATE_SIMD_OBJECTS "")

if(OROT_USE_SIMD)
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
if(OROT_SHARED)
  add_library(orot SHARED ${OROT_DEFLATE_CORE_SOURCES}
        ${OROT_LZ4_SOURCES}
        ${OROT_LZW_SOURCES}
        ${OROT_LZMA_SOURCES}
        ${OROT_BZIP2_SOURCES}
        ${OROT_ZSTD_SOURCES}
        ${OROT_BROTLI_SOURCES}
        ${OROT_DEFLATE_SIMD_OBJECTS})
else()
  add_library(orot STATIC ${OROT_DEFLATE_CORE_SOURCES}
        ${OROT_LZ4_SOURCES}
        ${OROT_LZW_SOURCES}
        ${OROT_LZMA_SOURCES}
        ${OROT_BZIP2_SOURCES}
        ${OROT_ZSTD_SOURCES}
        ${OROT_BROTLI_SOURCES}
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
    $<$<BOOL:${OROT_USE_SIMD}>:DEFLATE_SIMD_ENABLED>
    $<$<BOOL:${DEFLATE_HAS_SSE2}>:DEFLATE_HAS_SSE2>
    $<$<BOOL:${DEFLATE_HAS_SSE42}>:DEFLATE_HAS_SSE42>
    $<$<BOOL:${DEFLATE_HAS_AVX2}>:DEFLATE_HAS_AVX2>
    $<$<BOOL:${DEFLATE_HAS_NEON}>:DEFLATE_HAS_NEON>
    $<$<BOOL:${DEFLATE_HAS_CRC_ARM}>:DEFLATE_HAS_CRC_ARM>
    $<$<BOOL:${OROT_AS_PARALLEL}>:DEFLATE_THREADS_ENABLED>
    $<$<BOOL:${OROT_DEFLATE_ZLIB_COMPAT}>:DEFLATE_ZLIB_COMPAT>
    OROT_HAS_LZ4
    OROT_HAS_LZW
    OROT_HAS_LZMA
    OROT_HAS_BZIP2
    OROT_HAS_ZSTD
    OROT_HAS_BROTLI
)

if(OROT_USE_SYSTEM_BACKENDS)
  find_library(OROT_LIBDEFLATE_LIB deflate
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
  find_path(OROT_LIBDEFLATE_INCLUDE libdeflate.h
      HINTS /opt/homebrew/include /usr/local/include /usr/include)
  if(OROT_LIBDEFLATE_LIB AND OROT_LIBDEFLATE_INCLUDE)
    target_compile_definitions(orot PRIVATE OROT_HAS_LIBDEFLATE_BACKEND)
    target_include_directories(orot PRIVATE ${OROT_LIBDEFLATE_INCLUDE})
    target_link_libraries(orot PRIVATE ${OROT_LIBDEFLATE_LIB})
  endif()

  find_library(OROT_LZ4_LIB lz4
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
  find_path(OROT_LZ4_INCLUDE lz4.h
      HINTS /opt/homebrew/include /usr/local/include /usr/include)
  if(OROT_LZ4_LIB AND OROT_LZ4_INCLUDE)
    target_compile_definitions(orot PRIVATE OROT_HAS_LZ4_BACKEND)
    target_include_directories(orot PRIVATE ${OROT_LZ4_INCLUDE})
    target_link_libraries(orot PRIVATE ${OROT_LZ4_LIB})
  endif()

  find_library(OROT_LIBLZMA_LIB lzma
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
  find_path(OROT_LIBLZMA_INCLUDE lzma.h
      HINTS /opt/homebrew/include /usr/local/include /usr/include)
  if(OROT_LIBLZMA_LIB AND OROT_LIBLZMA_INCLUDE)
    target_compile_definitions(orot PRIVATE OROT_HAS_LIBLZMA_BACKEND)
    target_include_directories(orot PRIVATE ${OROT_LIBLZMA_INCLUDE})
    target_link_libraries(orot PRIVATE ${OROT_LIBLZMA_LIB})
  endif()

  find_library(OROT_BZ2_LIB bz2
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
  find_path(OROT_BZ2_INCLUDE bzlib.h
      HINTS /opt/homebrew/include /usr/local/include /usr/include)
  if(OROT_BZ2_LIB AND OROT_BZ2_INCLUDE)
    target_compile_definitions(orot PRIVATE OROT_HAS_BZ2_BACKEND)
    target_include_directories(orot PRIVATE ${OROT_BZ2_INCLUDE})
    target_link_libraries(orot PRIVATE ${OROT_BZ2_LIB})
  endif()

  find_library(OROT_ZSTD_LIB zstd
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
  find_path(OROT_ZSTD_INCLUDE zstd.h
      HINTS /opt/homebrew/include /usr/local/include /usr/include)
  if(OROT_ZSTD_LIB AND OROT_ZSTD_INCLUDE)
    target_compile_definitions(orot PRIVATE OROT_HAS_ZSTD_BACKEND)
    target_include_directories(orot PRIVATE ${OROT_ZSTD_INCLUDE})
    target_link_libraries(orot PRIVATE ${OROT_ZSTD_LIB})
  endif()

  find_library(OROT_BROTLI_DEC_LIB brotlidec
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
  find_library(OROT_BROTLI_ENC_LIB brotlienc
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
  find_library(OROT_BROTLI_COMMON_LIB brotlicommon
      HINTS /opt/homebrew/lib /usr/local/lib /usr/lib)
  find_path(OROT_BROTLI_INCLUDE brotli/encode.h
      HINTS /opt/homebrew/include /usr/local/include /usr/include)
  if(OROT_BROTLI_DEC_LIB AND OROT_BROTLI_ENC_LIB AND OROT_BROTLI_COMMON_LIB AND OROT_BROTLI_INCLUDE)
    target_compile_definitions(orot PRIVATE OROT_HAS_BROTLI_BACKEND)
    target_include_directories(orot PRIVATE ${OROT_BROTLI_INCLUDE})
    target_link_libraries(orot PRIVATE
        ${OROT_BROTLI_ENC_LIB}
        ${OROT_BROTLI_DEC_LIB}
        ${OROT_BROTLI_COMMON_LIB})
  endif()
endif()

deflate_set_compiler_flags(orot)

if(OROT_AS_PARALLEL)
  find_package(Threads REQUIRED)
  target_link_libraries(orot PRIVATE Threads::Threads)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
if(OROT_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
deflate_install(orot)
