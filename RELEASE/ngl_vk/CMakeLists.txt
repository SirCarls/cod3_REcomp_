# NGL on Vulkan: the library both ports link. A game's own build adds it with add_subdirectory.
#
# Needs the Vulkan headers and glslc (it turns the shaders into SPIR-V when the library is built; nothing compiles
# shaders while a game runs).
# [rexglue-native] 2026-11-09: not the Vulkan loader: ngl_vk opens it when it starts (src/ngl_vk_functions.h), so the
# Windows build made on Linux, whose llvm-mingw has no Vulkan import library, builds it too. Code that calls Vulkan
# itself (ngl_vk_low.c) links the loader itself (Vulkan::Vulkan); it isn't passed on from here any more.
cmake_minimum_required(VERSION 3.20)
project(ngl_vk C)

# [rexglue-native] 2026-11-09: the Vulkan headers' folder: NGL_VK_VULKAN_INCLUDE_DIR when the including build gives it
# (Call of Duty 3's does), else FindVulkan's as before (the system's), else, for a build for another system (Windows
# made on Linux), the first vulkan/vulkan.h in its CMAKE_PREFIX_PATH (the SDK's install), never the build PC's own.
if(NOT NGL_VK_VULKAN_INCLUDE_DIR)                                                         # [rexglue-native]
    if(CMAKE_CROSSCOMPILING)                                                              # [rexglue-native]
        find_path(NGL_VK_VULKAN_INCLUDE_DIR vulkan/vulkan.h NO_CMAKE_SYSTEM_PATH          # [rexglue-native]
                  NO_SYSTEM_ENVIRONMENT_PATH NO_CMAKE_ENVIRONMENT_PATH)                   # [rexglue-native]
    else()                                                                                # [rexglue-native]
        find_package(Vulkan QUIET)                                                        # [rexglue-native]
        set(NGL_VK_VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIR}")                            # [rexglue-native]
    endif()                                                                               # [rexglue-native]
endif()                                                                                   # [rexglue-native]
if(NOT NGL_VK_VULKAN_INCLUDE_DIR)                                                         # [rexglue-native]
    message(FATAL_ERROR "ngl_vk: the Vulkan headers (vulkan/vulkan.h) weren't found")      # [rexglue-native]
endif()                                                                                   # [rexglue-native]
find_program(NGL_VK_GLSLC NAMES glslc HINTS "${Vulkan_GLSLC_EXECUTABLE}" REQUIRED)

# Each shader becomes a C list of SPIR-V words that src/ngl_vk.c includes.
set(NGL_VK_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(NGL_VK_SHADERS)
foreach(shader quad.vert quad.frag quad2d.vert quad2d.frag mesh.vert mesh.frag grass.vert grass.frag
               shadow.frag dof_blur.frag dof_mix.frag tint.frag output.frag)
    set(spirv "${NGL_VK_SHADER_DIR}/${shader}.inc")
    add_custom_command(
        OUTPUT "${spirv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${NGL_VK_SHADER_DIR}"
        COMMAND "${NGL_VK_GLSLC}" -mfmt=c -o "${spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"
        COMMENT "Compiling ${shader} to SPIR-V")
    list(APPEND NGL_VK_SHADERS "${spirv}")
endforeach()
# [rexglue-native] 2026-11-09: mesh.frag again without the uniforms' fabric (its descriptor sets 4 and 5), for a device
# that binds fewer than six sets (src/ngl_vk.c make_mesh_pipeline picks one)
set(NGL_VK_FOUR_SETS_SPIRV "${NGL_VK_SHADER_DIR}/mesh_four_sets.frag.inc")                  # [rexglue-native]
add_custom_command(                                                                         # [rexglue-native]
    OUTPUT "${NGL_VK_FOUR_SETS_SPIRV}"                                                      # [rexglue-native]
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${NGL_VK_SHADER_DIR}"                     # [rexglue-native]
    COMMAND "${NGL_VK_GLSLC}" -mfmt=c -DNGL_VK_FOUR_SETS -fshader-stage=frag                # [rexglue-native]
            -o "${NGL_VK_FOUR_SETS_SPIRV}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.frag"  # [rexglue-native]
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.frag"                                 # [rexglue-native]
    COMMENT "Compiling mesh.frag without the fabric to SPIR-V")                             # [rexglue-native]
list(APPEND NGL_VK_SHADERS "${NGL_VK_FOUR_SETS_SPIRV}")                                     # [rexglue-native]
# [rexglue-native] 2026-11-10: the effect layer's two shaders (ngl_vk.h effect_layer; src/ngl_vk.c make_effect_layer)
# and the sky glow's three (ngl_vk.h ngl_vk_set_glow; src/ngl_vk.c make_glow)
foreach(shader effect_depth.frag effect_back.frag glow_first.frag glow_blur.frag glow_add.frag)  # [rexglue-native]
    set(spirv "${NGL_VK_SHADER_DIR}/${shader}.inc")                                         # [rexglue-native]
    add_custom_command(                                                                     # [rexglue-native]
        OUTPUT "${spirv}"                                                                   # [rexglue-native]
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${NGL_VK_SHADER_DIR}"                 # [rexglue-native]
        COMMAND "${NGL_VK_GLSLC}" -mfmt=c -o "${spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"  # [rexglue-native]
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"                             # [rexglue-native]
        COMMENT "Compiling ${shader} to SPIR-V")                                            # [rexglue-native]
    list(APPEND NGL_VK_SHADERS "${spirv}")                                                  # [rexglue-native]
endforeach()                                                                                # [rexglue-native]
# [rexglue-native-fx] 2026-11-11 fx1: the screen warp's two shaders (ngl_vk.h ngl_vk_set_screen_warp; src/ngl_vk.c make_heat)
foreach(shader heat_warp.vert heat_warp.frag)                                               # [rexglue-native-fx]
    set(spirv "${NGL_VK_SHADER_DIR}/${shader}.inc")                                         # [rexglue-native-fx]
    add_custom_command(                                                                     # [rexglue-native-fx]
        OUTPUT "${spirv}"                                                                   # [rexglue-native-fx]
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${NGL_VK_SHADER_DIR}"                 # [rexglue-native-fx]
        COMMAND "${NGL_VK_GLSLC}" -mfmt=c -o "${spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"  # [rexglue-native-fx]
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}"                             # [rexglue-native-fx]
        COMMENT "Compiling ${shader} to SPIR-V")                                            # [rexglue-native-fx]
    list(APPEND NGL_VK_SHADERS "${spirv}")                                                  # [rexglue-native-fx]
endforeach()                                                                                # [rexglue-native-fx]

add_library(ngl_vk STATIC src/ngl_vk.c ${NGL_VK_SHADERS})
target_include_directories(ngl_vk PUBLIC include PRIVATE "${NGL_VK_SHADER_DIR}")
# [rexglue-native] 2026-11-09: the headers for ngl_vk and for what includes ngl_vk_backend.h (and Big Red One's window
# code), as Vulkan::Vulkan gave them; dlopen's library on Linux (the C library itself on newer ones)
target_include_directories(ngl_vk SYSTEM PUBLIC "${NGL_VK_VULKAN_INCLUDE_DIR}")          # [rexglue-native]
target_link_libraries(ngl_vk PRIVATE ${CMAKE_DL_LIBS})                                    # [rexglue-native]
set_target_properties(ngl_vk PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
