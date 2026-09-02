cmake_minimum_required(VERSION 3.20)
project(GenomeRuntime CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_library(miniz STATIC third_party/miniz.c third_party/miniz_tinfl.c third_party/miniz_tdef.c third_party/miniz_zip.c)
target_include_directories(miniz PUBLIC third_party)

add_library(genome STATIC
  src/genome/pak.cpp
  src/genome/property_set.cpp
  src/genome/mesh.cpp
  src/genome/material.cpp
  src/genome/world.cpp
  src/genome/image.cpp
  src/genome/actor.cpp
  src/genome/motion.cpp
  src/genome/spt.cpp
  src/genome/tree.cpp
  src/genome/lightmap.cpp
  src/genome/residency.cpp)
target_include_directories(genome PUBLIC src)
target_link_libraries(genome PRIVATE miniz)

add_executable(g3pak tools/g3pak.cpp)
target_link_libraries(g3pak PRIVATE genome)

add_executable(g3mesh tools/g3mesh.cpp)
target_link_libraries(g3mesh PRIVATE genome)

add_executable(g3mat tools/g3mat.cpp)
target_link_libraries(g3mat PRIVATE genome)

add_executable(g3img tools/g3img.cpp)
target_link_libraries(g3img PRIVATE genome)

add_executable(g3actor tools/g3actor.cpp)
target_link_libraries(g3actor PRIVATE genome)

add_executable(g3probe tools/g3probe.cpp)
target_link_libraries(g3probe PRIVATE genome)

add_executable(g3anim tools/g3anim.cpp)
target_link_libraries(g3anim PRIVATE genome)

add_executable(g3pose tools/g3pose.cpp)
target_link_libraries(g3pose PRIVATE genome)

add_executable(g3lightmap tools/g3lightmap.cpp)
target_link_libraries(g3lightmap PRIVATE genome)

add_executable(g3tree tools/g3tree.cpp)
target_link_libraries(g3tree PRIVATE genome)

# --- viewer ---------------------------------------------------------------
find_package(Vulkan REQUIRED)

# Shaders are compiled to SPIR-V beside the executable so the viewer can be run
# straight from the build directory.
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "C:/VulkanSDK/1.3.236.0/Bin")
if (NOT GLSLC)
  message(FATAL_ERROR "glslc not found; install the Vulkan SDK")
endif()

set(SHADER_OUTPUTS)
foreach(shader mesh.vert mesh.frag world.vert world.frag)
  set(output ${CMAKE_BINARY_DIR}/shaders/${shader}.spv)
  add_custom_command(
    OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/shaders/${shader} -o ${output}
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${shader}
    COMMENT "Compiling ${shader}")
  list(APPEND SHADER_OUTPUTS ${output})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_OUTPUTS})

add_library(render STATIC
  src/render/window.cpp
  src/render/vulkan.cpp
  src/render/renderer.cpp
  src/render/tree_atlas.cpp
  src/render/world_renderer.cpp
  src/render/occlusion.cpp
  src/render/texture.cpp)
target_include_directories(render PUBLIC src)
target_link_libraries(render PUBLIC genome Vulkan::Vulkan)

# Tracy: zones stream to the external viewer when it connects, and nothing is
# recorded until one does. The client is vendored rather than taken from a
# registry because the protocol is version-locked to the viewer, and registries
# lag releases - the same reason it is vendored in the raytracer, where a client
# two versions behind reported every zone name as ???.
option(G3_ENABLE_TRACY "Compile in Tracy profiler zones" ON)

if (G3_ENABLE_TRACY)
  add_library(TracyClient SHARED ThirdParty/tracy/TracyClient.cpp)
  target_include_directories(TracyClient SYSTEM PUBLIC ThirdParty/tracy)
  target_compile_definitions(TracyClient PRIVATE TRACY_EXPORTS)
  target_compile_definitions(TracyClient INTERFACE TRACY_IMPORTS)
  target_compile_definitions(TracyClient PUBLIC TRACY_ENABLE TRACY_ON_DEMAND)
  if (WIN32)
    target_link_libraries(TracyClient PRIVATE ws2_32 dbghelp)
  endif()
  target_link_libraries(render PUBLIC TracyClient)
  message(STATUS "Tracy zones enabled, client 0.14.1 from ThirdParty/tracy (-DG3_ENABLE_TRACY=OFF to disable)")
endif()

add_executable(g3sector tools/g3sector.cpp)
target_link_libraries(g3sector PRIVATE genome)

add_executable(g3world tools/g3world.cpp)
target_link_libraries(g3world PRIVATE genome render)
add_dependencies(g3world shaders)

add_executable(g3view tools/g3view.cpp)
target_link_libraries(g3view PRIVATE render)
add_dependencies(g3view shaders)
