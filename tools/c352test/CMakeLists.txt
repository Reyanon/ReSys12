# C352 standalone test (PC). Build:
#   cmake -S tools/c352test -B build-c352 -DCMAKE_BUILD_TYPE=Release && cmake --build build-c352
#   ./build-c352/c352test build-c352
cmake_minimum_required(VERSION 3.16)
project(c352test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(RS12_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_executable(c352test c352test.c ${RS12_ROOT}/src/sound/c352.c)
target_include_directories(c352test PRIVATE ${RS12_ROOT}/src/sound)
target_compile_definitions(c352test PRIVATE _GNU_SOURCE)
target_compile_options(c352test PRIVATE -Wall -Wextra -O2)
target_link_libraries(c352test PRIVATE m)
