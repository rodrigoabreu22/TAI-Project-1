cmake_minimum_required(VERSION 3.14)
project(tai-compressor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimization flags for Release builds
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Common source files (model, coder, io)
file(GLOB_RECURSE LIB_SOURCES
    src/model/*.cpp
    src/coder/*.cpp
    src/io/*.cpp
)

# Compressor executable
add_executable(compress src/main_compress.cpp ${LIB_SOURCES})
target_include_directories(compress PRIVATE src)

# Decompressor executable
add_executable(decompress src/main_decompress.cpp ${LIB_SOURCES})
target_include_directories(decompress PRIVATE src)
