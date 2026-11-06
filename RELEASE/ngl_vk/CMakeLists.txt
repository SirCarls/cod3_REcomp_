# NGL on Vulkan: the library both ports link. A game's own build adds it with add_subdirectory.
#
# Needs the Vulkan headers and loader, and glslc (it turns the shaders into SPIR-V when the library is built; nothing
# compiles shaders while a game runs).
cmake_minimum_required(VERSION 3.20)
project(ngl_vk C)

find_package(Vulkan REQUIRED)
find_program(NGL_VK_GLSLC NAMES glslc HINTS "${Vulkan_GLSLC_EXECUTABLE}" REQUIRED)

# Each shader becomes a C list of SPIR-V words that src/ngl_vk.c includes.
set(NGL_VK_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(NGL_VK_SHADERS)
foreach(shader quad.vert quad.frag quad2d.vert quad2d.frag mesh.vert mesh.frag grass.vert grass.frag
               shadow.frag)
    set(spirv "${NGL_VK_SHADER_DIR}/${shader}.inc")
    add_custom_command(
        OUTPUT "${spirv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${NGL_VK_SHADER_DIR}"
        COMMAND "${NGL_VK_GLSLC}" -mfmt=c -o "${spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
        COMMENT "Compiling ${shader} to SPIR-V")
    list(APPEND NGL_VK_SHADERS "${spirv}")
endforeach()

add_library(ngl_vk STATIC src/ngl_vk.c ${NGL_VK_SHADERS})
target_include_directories(ngl_vk PUBLIC include PRIVATE "${NGL_VK_SHADER_DIR}")
target_link_libraries(ngl_vk PUBLIC Vulkan::Vulkan)
set_target_properties(ngl_vk PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
