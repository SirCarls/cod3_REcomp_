# waw - Call of Duty: World at War (Xbox 360) static recompilation, built on the ReXGlue SDK: the same SDK, patches
# and toolchain as Call of Duty 3 (../recomp/). [acct2-waw]
#
# SDK boilerplate lives in generated/rexglue.cmake (written by `rexglue init`; the codegen keeps it).

cmake_minimum_required(VERSION 3.25)
project(waw LANGUAGES C CXX)

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

set(WAW_SOURCES
    src/main.cpp
    src/waw_app.cpp
    src/waw_hooks.cpp
    src/waw_shots.cpp
    src/waw_tu_fixes.cpp  # [waw-decomp] title update 7's fixes, redone as our own hooks
    src/waw_render_fixes.cpp  # [waw-decomp] drawing problems worked around in the game's code
    src/waw_f2.cpp  # [f2-menus] the F2 settings window: World at War's pages and colors
    src/waw_mouse.cpp  # [f2-menus] mouse look through the game's own PC code, the keyboard pad's defaults
    src/waw_keys.cpp  # [f2-menus] keys the PC way: the game's own binds (dev/agents/f2-menus/BINDINGS-PLAN.md)
    src/waw_native.cpp  # [waw-decomp-waw] 2026-11-11 the native renderer's hooks and the struct map's capture
    src/waw_native_side.cpp  # [waw-decomp-waw] 2026-11-11 a frame drawn natively (ngl_vk) beside the game's own
    src/waw_native_textures.cpp  # [waw-decomp-waw] 2026-11-11 Xbox 360 textures decoded for the native side
    src/waw_native_window.cpp  # [waw-decomp-waw] 2026-11-11 round 5 the native picture shown (waw_native_vulkan)
    ${COD3_RECOMP_SRC}/test_input.cpp
    # [f2-menus] The F2 settings window shared by every game's port (../recomp/src/f2/), built from its own folder.
    ${COD3_RECOMP_SRC}/f2/f2_common.cpp  # [f2-menus] the F2 rows every port shares
    ${COD3_RECOMP_SRC}/f2/f2_accel.cpp  # [f2-menus] 2026-11-11 mouse acceleration, randomizer, scales (Call of Duty 3 builds it too)
    ${COD3_RECOMP_SRC}/f2/f2_render_size.cpp  # [f2-menus] the game's own 3D size (game_draw_size)
    ${COD3_RECOMP_SRC}/f2/f2_console.cpp
    ${COD3_RECOMP_SRC}/f2/f2_pad.cpp
    ${COD3_RECOMP_SRC}/f2/f2_rows.cpp
    ${COD3_RECOMP_SRC}/f2/f2_theme.cpp
    ${COD3_RECOMP_SRC}/f2/f2_window.cpp
)

if(WIN32)
    add_executable(waw WIN32 ${WAW_SOURCES})
else()
    add_executable(waw ${WAW_SOURCES})
endif()

target_include_directories(waw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${COD3_RECOMP_SRC})

# GPU_PLUGINS stages librexgpu-xenos next to the executable (the Xenos GPU emulation: Vulkan, and Direct3D 12 on
# Windows). It is loaded at start; see WawApp::OnPreSetup.
rexglue_setup_target(waw GPU_PLUGINS xenos)

# [waw-decomp-waw] 2026-11-11: ngl_vk (the project's ngl_vk/ folder, the Vulkan library Call of Duty 3's, Big Red
# One's and Call of Duty 2's native renderers draw with) for waw_debug_native_side (src/waw_native_side.cpp), a check
# that is off by default. It needs the Vulkan headers and glslc, found as Call of Duty 2's build finds them; without
# them the check only counts, and the port builds as before. WAW_NGL_VK_DIR names the folder; by default it is the one
# two up from this one (a build in place; a build from a copy elsewhere names it).
set(WAW_NGL_VK_DIR "" CACHE PATH "The project's ngl_vk/ folder (the native renderer's Vulkan library)")
set(_waw_ngl_vk "${WAW_NGL_VK_DIR}")
if(NOT _waw_ngl_vk AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../ngl_vk/CMakeLists.txt")
    get_filename_component(_waw_ngl_vk "${CMAKE_CURRENT_SOURCE_DIR}/../../ngl_vk" ABSOLUTE)
endif()
if(_waw_ngl_vk AND EXISTS "${_waw_ngl_vk}/CMakeLists.txt")
    if(CMAKE_CROSSCOMPILING)
        # the Windows build made on Linux: the headers from its CMAKE_PREFIX_PATH (the SDK's install) only
        find_path(NGL_VK_VULKAN_INCLUDE_DIR vulkan/vulkan.h NO_CMAKE_SYSTEM_PATH NO_SYSTEM_ENVIRONMENT_PATH
            NO_CMAKE_ENVIRONMENT_PATH)
    else()
        find_package(Vulkan QUIET)
        set(NGL_VK_VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIR}")
    endif()
    find_program(WAW_NGL_VK_GLSLC NAMES glslc HINTS "${Vulkan_GLSLC_EXECUTABLE}")
    if(NGL_VK_VULKAN_INCLUDE_DIR AND WAW_NGL_VK_GLSLC)
        add_subdirectory("${_waw_ngl_vk}" "${CMAKE_CURRENT_BINARY_DIR}/ngl_vk")
        target_link_libraries(waw PRIVATE ngl_vk)
        target_compile_definitions(waw PRIVATE WAW_NGL_VK=1)
        message(STATUS "waw: ngl_vk from ${_waw_ngl_vk} (waw_debug_native_side)")
        # [waw-decomp-waw] 2026-11-11 round 5: waw_native_window.cpp shows the native picture through the SDK's Vulkan
        # presenter (waw_native_vulkan). Those headers include renderdoc_app.h by its bare name; the SDK keeps it in
        # include/renderdoc (installed) or thirdparty/renderdoc (source tree), neither on the SDK targets' include path
        # (found as Call of Duty 2's ../recomp_cod2/CMakeLists.txt and Call of Duty 3's ../recomp/CMakeLists.txt find
        # it).
        get_target_property(_waw_rex_includes rex::runtime INTERFACE_INCLUDE_DIRECTORIES)
        find_path(WAW_RENDERDOC_INCLUDE_DIR renderdoc_app.h
            HINTS ${_waw_rex_includes} "${rexglue_DIR}/../../../include" "${REXSDK_DIR}/thirdparty"
            PATH_SUFFIXES renderdoc NO_DEFAULT_PATH)
        if(WAW_RENDERDOC_INCLUDE_DIR)
            target_include_directories(waw PRIVATE "${WAW_RENDERDOC_INCLUDE_DIR}")
        else()
            message(WARNING "waw: the SDK's renderdoc_app.h wasn't found; waw_native_window.cpp won't build")
        endif()
    else()
        message(STATUS "waw: no ngl_vk (Vulkan headers or glslc missing); waw_debug_native_side only counts")
    endif()
else()
    message(STATUS "waw: no ngl_vk (no ngl_vk/ folder found); waw_debug_native_side only counts")
endif()

# MinGW (the Windows build made on Linux): the SDK's entry point is wWinMain, which MinGW's C runtime only calls when
# the program links with -municode.
if(MINGW)
    target_link_options(waw PRIVATE -municode)
endif()

# Linux: the SDK links librexruntime.so with an $ORIGIN rpath but does not stage it, so copy it next to the
# executable.
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET waw POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:waw>
        VERBATIM)
endif()
