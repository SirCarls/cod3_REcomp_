# cod4mp - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation, the multiplayer program, built on the
# ReXGlue SDK: the same SDK, patches and toolchain as Call of Duty 3 (../../recomp/). [acct2-cod4]
# It builds the campaign's sources (../src) with COD4_MP defined, against its own translated code (generated/).
#
# SDK boilerplate lives in generated/rexglue.cmake (written by `rexglue init`; the codegen keeps it).

cmake_minimum_required(VERSION 3.25)
project(cod4mp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# A cross build (the Windows .exe made on Linux) gives the build machine's rexglue for the codegen (as ../).
set(REXGLUE_CODEGEN "" CACHE FILEPATH "rexglue that runs the codegen (default: the SDK's)")
if(REXGLUE_CODEGEN)
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION "${REXGLUE_CODEGEN}"
        IMPORTED_LOCATION_RELEASE "${REXGLUE_CODEGEN}")
endif()

set(COD4_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(COD3_RECOMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../recomp/src" CACHE PATH "Call of Duty 3's recomp/src (test_input.*)")

set(COD4MP_SOURCES
    ${COD4_SRC}/main.cpp
    ${COD4_SRC}/cod4_app.cpp
    ${COD4_SRC}/cod4_hooks.cpp
    ${COD4_SRC}/cod4_census.cpp
    ${COD4_SRC}/cod4_shots.cpp
    ${COD4_SRC}/cod4_mouse.cpp
    ${COD4_SRC}/cod4_profile.cpp
    ${COD4_SRC}/cod4_native_side.cpp  # [waw-decomp-cod4] 2026-11-11 the native renderer's side check (cod4_debug_native_side)
    ${COD4_SRC}/cod4_native_window.cpp  # [waw-decomp-cod4] 2026-11-11 round 6: the native picture as the game's
    ${COD4_SRC}/cod4_script_folder.cpp  # [waw-decomp-cod4] 2026-11-11 game scripts from a folder (cod4_script_folder)
    ${COD3_RECOMP_SRC}/test_input.cpp
    ${COD4_SRC}/cod4_f2.cpp  # [f2-menus] the F2 settings window: Call of Duty 4's pages and colors
    # [f2-menus] The F2 settings window shared by every game's port (../../recomp/src/f2/), built from its own folder.
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
    add_executable(cod4mp WIN32 ${COD4MP_SOURCES})
else()
    add_executable(cod4mp ${COD4MP_SOURCES})
endif()

target_compile_definitions(cod4mp PRIVATE COD4_MP=1)
target_include_directories(cod4mp PRIVATE ${COD4_SRC} ${COD3_RECOMP_SRC})

rexglue_setup_target(cod4mp GPU_PLUGINS xenos)

# [waw-decomp-cod4] 2026-11-11: ngl_vk (the project's ngl_vk/ folder, the Vulkan library Call of Duty 3's, Big Red
# One's and Call of Duty 2's native renderers draw with), for the native renderer's side check
# (src/cod4_native_side.cpp, cod4_debug_native_side, off by default), brought in as Call of Duty 2's project does
# (../recomp_cod2/CMakeLists.txt). It needs the Vulkan headers and glslc; without them, or without the folder, the
# program builds as before and the check only counts. COD4_NGL_VK_DIR names the folder (as a CMake option, or as an
# environment variable for a build script that can't pass options: it is then kept in the cache); by default it is
# the one three up from this one (a build in place).
set(COD4_NGL_VK_DIR "" CACHE PATH "The project's ngl_vk/ folder (the native renderer's Vulkan library)")
if(NOT COD4_NGL_VK_DIR AND DEFINED ENV{COD4_NGL_VK_DIR})
    set(COD4_NGL_VK_DIR "$ENV{COD4_NGL_VK_DIR}" CACHE PATH
        "The project's ngl_vk/ folder (the native renderer's Vulkan library)" FORCE)
endif()
set(_cod4_ngl_vk "${COD4_NGL_VK_DIR}")
if(NOT _cod4_ngl_vk AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../ngl_vk/CMakeLists.txt")
    get_filename_component(_cod4_ngl_vk "${CMAKE_CURRENT_SOURCE_DIR}/../../../ngl_vk" ABSOLUTE)
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
        target_link_libraries(cod4mp PRIVATE ngl_vk)
        target_compile_definitions(cod4mp PRIVATE COD4_NGL_VK=1)
        message(STATUS "cod4mp: ngl_vk from ${_cod4_ngl_vk} (cod4_debug_native_side)")
        # [waw-decomp-cod4] 2026-11-11 round 6: the SDK's renderdoc_app.h for cod4_native_window.cpp (the campaign's
        # ../CMakeLists.txt says why)
        get_target_property(_cod4_rex_includes rex::runtime INTERFACE_INCLUDE_DIRECTORIES)
        find_path(COD4_RENDERDOC_INCLUDE_DIR renderdoc_app.h
            HINTS ${_cod4_rex_includes} "${rexglue_DIR}/../../../include" "${REXSDK_DIR}/thirdparty"
            PATH_SUFFIXES renderdoc NO_DEFAULT_PATH)
        if(COD4_RENDERDOC_INCLUDE_DIR)
            target_include_directories(cod4mp PRIVATE "${COD4_RENDERDOC_INCLUDE_DIR}")
        else()
            message(WARNING "cod4mp: the SDK's renderdoc_app.h wasn't found; cod4_native_window.cpp won't build")
        endif()
    else()
        message(STATUS "cod4mp: no ngl_vk (Vulkan headers or glslc missing); cod4_debug_native_side only counts")
    endif()
else()
    message(STATUS "cod4mp: no ngl_vk (no ngl_vk/ folder found); cod4_debug_native_side only counts")
endif()

if(MINGW)
    target_link_options(cod4mp PRIVATE -municode)
endif()

if(UNIX AND NOT APPLE)
    add_custom_command(TARGET cod4mp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:cod4mp>
        VERBATIM)
endif()
