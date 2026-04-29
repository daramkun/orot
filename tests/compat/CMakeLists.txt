cmake_minimum_required(VERSION 3.20)

add_executable(test_compat test_compat.cpp)
target_link_libraries(test_compat PRIVATE orot ZLIB::ZLIB ${LIBDEFLATE_LIB})
target_include_directories(test_compat PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LIBDEFLATE_INCLUDE}
)
target_compile_options(test_compat PRIVATE -O2)
add_test(NAME test_compat COMMAND test_compat)

find_library(LZ4_LIB lz4
      HINTS /opt/homebrew/lib /usr/local/lib ${CMAKE_INSTALL_PREFIX}/lib
      PATH_SUFFIXES lib)
find_path(LZ4_INCLUDE_DIR lz4.h
      HINTS /opt/homebrew/include /usr/local/include ${CMAKE_INSTALL_PREFIX}/include
      PATH_SUFFIXES include)

find_library(LIBLZMA_LIB lzma
      HINTS /opt/homebrew/lib /usr/local/lib ${CMAKE_INSTALL_PREFIX}/lib
      PATH_SUFFIXES lib)
find_path(LIBLZMA_INCLUDE_DIR lzma.h
      HINTS /opt/homebrew/include /usr/local/include ${CMAKE_INSTALL_PREFIX}/include
      PATH_SUFFIXES include)

find_library(LIBBZ2_LIB bz2
      HINTS /opt/homebrew/lib /usr/local/lib ${CMAKE_INSTALL_PREFIX}/lib
      PATH_SUFFIXES lib)
find_path(LIBBZ2_INCLUDE_DIR bzlib.h
      HINTS /opt/homebrew/include /usr/local/include ${CMAKE_INSTALL_PREFIX}/include
      PATH_SUFFIXES include)

find_library(LIBZSTD_LIB zstd
      HINTS /opt/homebrew/lib /usr/local/lib ${CMAKE_INSTALL_PREFIX}/lib
      PATH_SUFFIXES lib)
find_path(LIBZSTD_INCLUDE_DIR zstd.h
      HINTS /opt/homebrew/include /usr/local/include ${CMAKE_INSTALL_PREFIX}/include
      PATH_SUFFIXES include)

find_library(LIBBROTLI_DEC_LIB brotlidec
      HINTS /opt/homebrew/lib /usr/local/lib ${CMAKE_INSTALL_PREFIX}/lib
      PATH_SUFFIXES lib)
find_library(LIBBROTLI_COMMON_LIB brotlicommon
      HINTS /opt/homebrew/lib /usr/local/lib ${CMAKE_INSTALL_PREFIX}/lib
      PATH_SUFFIXES lib)
find_path(LIBBROTLI_INCLUDE_DIR brotli/decode.h
      HINTS /opt/homebrew/include /usr/local/include ${CMAKE_INSTALL_PREFIX}/include
      PATH_SUFFIXES include)

if(LZ4_LIB AND LZ4_INCLUDE_DIR AND
   LIBLZMA_LIB AND LIBLZMA_INCLUDE_DIR AND
   LIBBZ2_LIB AND LIBBZ2_INCLUDE_DIR AND
   LIBZSTD_LIB AND LIBZSTD_INCLUDE_DIR)
  add_executable(test_codec_compat test_codec_compat.cpp)
  target_link_libraries(test_codec_compat PRIVATE
      orot
      ${LZ4_LIB}
      ${LIBLZMA_LIB}
      ${LIBBZ2_LIB}
      ${LIBZSTD_LIB}
  )
  target_include_directories(test_codec_compat PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${LZ4_INCLUDE_DIR}
      ${LIBLZMA_INCLUDE_DIR}
      ${LIBBZ2_INCLUDE_DIR}
      ${LIBZSTD_INCLUDE_DIR}
  )
  target_compile_options(test_codec_compat PRIVATE -O2)
  add_test(NAME test_codec_compat COMMAND test_codec_compat)
  message(STATUS "LZ4/LZMA/Bzip2/Zstandard: cross-library compatibility tests enabled")
else()
  message(STATUS "LZ4/LZMA/Bzip2/Zstandard: skipping compat tests (external libs not found)")
endif()

if(LIBBROTLI_DEC_LIB AND LIBBROTLI_COMMON_LIB AND LIBBROTLI_INCLUDE_DIR)
  add_executable(test_brotli_compat test_brotli_compat.cpp)
  target_link_libraries(test_brotli_compat PRIVATE
      orot
      ${LIBBROTLI_DEC_LIB}
      ${LIBBROTLI_COMMON_LIB}
  )
  target_include_directories(test_brotli_compat PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${LIBBROTLI_INCLUDE_DIR}
  )
  target_compile_options(test_brotli_compat PRIVATE -O2)
  add_test(NAME test_brotli_compat COMMAND test_brotli_compat)
  message(STATUS "Brotli: uncompressed-stream compatibility tests enabled")
else()
  message(STATUS "Brotli: skipping compat tests (libbrotlidec not found)")
endif()
