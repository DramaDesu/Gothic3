cmake_minimum_required(VERSION 3.20)
project(GenomeRuntime CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_library(miniz STATIC third_party/miniz.c third_party/miniz_tinfl.c third_party/miniz_tdef.c third_party/miniz_zip.c)
target_include_directories(miniz PUBLIC third_party)

add_library(genome STATIC src/genome/pak.cpp)
target_include_directories(genome PUBLIC src)
target_link_libraries(genome PRIVATE miniz)

add_executable(g3pak tools/g3pak.cpp)
target_link_libraries(g3pak PRIVATE genome)
