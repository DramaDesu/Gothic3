cmake_minimum_required(VERSION 3.16...3.24)

project(
  g3-improvement
  DESCRIPTION "Gothic 3 improvement project - native script DLLs"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

set(CPM_USE_NAMED_CACHE_DIRECTORIES ON)
include(${CMAKE_SOURCE_DIR}/cmake/CPM.cmake)

CPMAddPackage(
  NAME gothic3sdk
  OPTIONS
    "G3SDK_HEADER_ONLY OFF"
  GITHUB_REPOSITORY georgeto/gothic3sdk
  GIT_TAG 4296b8691d3eb5f18200cc528fd622ca4196a38e
)
include(${gothic3sdk_SOURCE_DIR}/cmake/Setup.cmake)

add_subdirectory(scripts/Script_Mcp)
