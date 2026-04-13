cmake_minimum_required(VERSION 3.14)
project(tai-compressor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -ffast-math -DNDEBUG")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS
    src/model/*.cpp
    src/coder/*.cpp
)

add_executable(compress src/main_compress.cpp ${LIB_SOURCES})
target_include_directories(compress PRIVATE src)

add_executable(decompress src/main_decompress.cpp ${LIB_SOURCES})
target_include_directories(decompress PRIVATE src)
