# cod4 - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation, the campaign program, built on the ReXGlue
# SDK: the same SDK, patches and toolchain as Call of Duty 3 (../recomp/). [acct2-cod4]
# The multiplayer program (cod4mp) is its own project in mp/, built from the same src/.
#
# SDK boilerplate lives in generated/rexglue.cmake (written by `rexglue init`; the codegen keeps it).

cmake_minimum_required(VERSION 3.25)
project(cod4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# A cross build (the Windows .exe made on Linux) can't run the SDK's own rexglue.exe for the codegen, so it gives the
# build machine's rexglue as REXGLUE_CODEGEN. The generated C++ is the same either way. (As in ../recomp.)
set(REXGLUE_CODEGEN "" CACHE FILEPATH "rexglue that runs the codegen (default: the SDK's)")
if(REXGLUE_CODEGEN)
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION "${REXGLUE_CODEGEN}"
        IMPORTED_LOCATION_RELEASE "${REXGLUE_CODEGEN}")
endif()

# Call of Duty 3's scripted test pad (test_input.cpp, a generic SDK input driver) is built here as well, from its
# own folder, not copied.
set(COD3_RECOMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../recomp/src" CACHE PATH "Call of Duty 3's recomp/src (test_input.*)")

set(COD4_SOURCES
    src/main.cpp
    src/cod4_app.cpp
    src/cod4_hooks.cpp
    src/cod4_census.cpp
    src/cod4_shots.cpp
    src/cod4_mouse.cpp
    src/cod4_profile.cpp
    src/cod4_native_side.cpp  # [waw-decomp-cod4] 2026-11-11 the native renderer's side check (cod4_debug_native_side)
    src/cod4_native_window.cpp  # [waw-decomp-cod4] 2026-11-11 round 6: the native picture as the game's
    ${COD3_RECOMP_SRC}/test_input.cpp
    src/cod4_f2.cpp  # [f2-menus] the F2 settings window: Call of Duty 4's pages and colors
    # [f2-menus] The F2 settings window shared by every game's port (../recomp/src/f2/), built from its own folder.
    ${COD3_RECOMP_SRC}/f2/f2_common.cpp
    ${COD3_RECOMP_SRC}/f2/f2_accel.cpp  # [f2-menus] 2026-11-11 mouse acceleration, randomizer, scales (Call of Duty 3 builds it too)
    ${COD3_RECOMP_SRC}/f2/f2_render_size.cpp  # [f2-menus] the game's own 3D size (game_draw_size)
    ${COD3_RECOMP_SRC}/f2/f2_console.cpp
    ${COD3_RECOMP_SRC}/f2/f2_pad.cpp
    ${COD3_RECOMP_SRC}/f2/f2_rows.cpp
    ${COD3_RECOMP_SRC}/f2/f2_theme.cpp
    ${COD3_RECOMP_SRC}/f2/f2_window.cpp
)

if(WIN32)
    add_executable(cod4 WIN32 ${COD4_SOURCES})
else()
    add_executable(cod4 ${COD4_SOURCES})
endif()

target_include_directories(cod4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${COD3_RECOMP_SRC})

# GPU_PLUGINS stages librexgpu-xenos next to the executable (the Xenos GPU emulation: Vulkan, and Direct3D 12 on
# Windows). It is loaded at start; see Cod4App::OnPreSetup.
rexglue_setup_target(cod4 GPU_PLUGINS xenos)

# [waw-decomp-cod4] 2026-11-11: ngl_vk (the project's ngl_vk/ folder, the Vulkan library Call of Duty 3's, Big Red
# One's and Call of Duty 2's native renderers draw with), for the native renderer's side check
# (src/cod4_native_side.cpp, cod4_debug_native_side, off by default), brought in as Call of Duty 2's project does
# (../recomp_cod2/CMakeLists.txt). It needs the Vulkan headers and glslc; without them, or without the folder, the
# program builds as before and the check only counts. COD4_NGL_VK_DIR names the folder (as a CMake option, or as an
# environment variable for a build script that can't pass options: it is then kept in the cache); by default it is
# the one two up from this one (a build in place).
set(COD4_NGL_VK_DIR "" CACHE PATH "The project's ngl_vk/ folder (the native renderer's Vulkan library)")
if(NOT COD4_NGL_VK_DIR AND DEFINED ENV{COD4_NGL_VK_DIR})
    set(COD4_NGL_VK_DIR "$ENV{COD4_NGL_VK_DIR}" CACHE PATH
        "The project's ngl_vk/ folder (the native renderer's Vulkan library)" FORCE)
endif()
set(_cod4_ngl_vk "${COD4_NGL_VK_DIR}")
if(NOT _cod4_ngl_vk AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../ngl_vk/CMakeLists.txt")
    get_filename_component(_cod4_ngl_vk "${CMAKE_CURRENT_SOURCE_DIR}/../../ngl_vk" ABSOLUTE)
endif()
if(_cod4_ngl_vk AND EXISTS "${_cod4_ngl_vk}/CMakeLists.txt")
    if(CMAKE_CROSSCOMPILING)
        # the Windows build made on Linux: the headers from its CMAKE_PREFIX_PATH (the SDK's install) only
        find_path(NGL_VK_VULKAN_INCLUDE_DIR vulkan/vulkan.h NO_CMAKE_SYSTEM_PATH NO_SYSTEM_ENVIRONMENT_PATH
            NO_CMAKE_ENVIRONMENT_PATH)
    else()
        find_package(Vulkan QUIET)
        set(NGL_VK_VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIR}")
    endif()
    find_program(COD4_NGL_VK_GLSLC NAMES glslc HINTS "${Vulkan_GLSLC_EXECUTABLE}")
    if(NGL_VK_VULKAN_INCLUDE_DIR AND COD4_NGL_VK_GLSLC)
        add_subdirectory("${_cod4_ngl_vk}" "${CMAKE_CURRENT_BINARY_DIR}/ngl_vk")
        target_link_libraries(cod4 PRIVATE ngl_vk)
        target_compile_definitions(cod4 PRIVATE COD4_NGL_VK=1)
        message(STATUS "cod4: ngl_vk from ${_cod4_ngl_vk} (cod4_debug_native_side)")
        # [waw-decomp-cod4] 2026-11-11 round 6: cod4_native_window.cpp shows the native picture through the SDK's
        # Vulkan presenter (cod4_native_vulkan). Those headers include renderdoc_app.h by its bare name; the SDK keeps
        # it in include/renderdoc (installed) or thirdparty/renderdoc (source tree), neither on the SDK targets'
        # include path (found as Call of Duty 2's ../recomp_cod2/CMakeLists.txt and Call of Duty 3's ../recomp find
        # it).
        get_target_property(_cod4_rex_includes rex::runtime INTERFACE_INCLUDE_DIRECTORIES)
        find_path(COD4_RENDERDOC_INCLUDE_DIR renderdoc_app.h
            HINTS ${_cod4_rex_includes} "${rexglue_DIR}/../../../include" "${REXSDK_DIR}/thirdparty"
            PATH_SUFFIXES renderdoc NO_DEFAULT_PATH)
        if(COD4_RENDERDOC_INCLUDE_DIR)
            target_include_directories(cod4 PRIVATE "${COD4_RENDERDOC_INCLUDE_DIR}")
        else()
            message(WARNING "cod4: the SDK's renderdoc_app.h wasn't found; cod4_native_window.cpp won't build")
        endif()
    else()
        message(STATUS "cod4: no ngl_vk (Vulkan headers or glslc missing); cod4_debug_native_side only counts")
    endif()
else()
    message(STATUS "cod4: no ngl_vk (no ngl_vk/ folder found); cod4_debug_native_side only counts")
endif()

# MinGW (the Windows build made on Linux): the SDK's entry point is wWinMain, which MinGW's C runtime only calls when
# the program links with -municode.
if(MINGW)
    target_link_options(cod4 PRIVATE -municode)
endif()

# Linux: the SDK links librexruntime.so with an $ORIGIN rpath but does not stage it, so copy it next to the
# executable.
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET cod4 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:cod4>
        VERBATIM)
endif()
