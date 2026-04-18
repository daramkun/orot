cmake_minimum_required(VERSION 3.20)

add_executable(test_compat test_compat.cpp)
target_link_libraries(test_compat PRIVATE orot ZLIB::ZLIB ${LIBDEFLATE_LIB})
target_include_directories(test_compat PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LIBDEFLATE_INCLUDE}
)
target_compile_options(test_compat PRIVATE -O2)
