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

# Arithmetic encoder v1
add_executable(g3_v1_c src/coder/g3_v1_c.cpp)
target_include_directories(g3_v1_c PRIVATE src/coder)

add_executable(g3_v1_d src/coder/g3_v1_d.cpp)
target_include_directories(g3_v1_d PRIVATE src/coder)

# Arithmetic encoder v2
add_executable(g3_v2_c src/coder/g3_v2_c.cpp)
target_include_directories(g3_v2_c PRIVATE src/coder)

add_executable(g3_v2_d src/coder/g3_v2_d.cpp)
target_include_directories(g3_v2_d PRIVATE src/coder)
