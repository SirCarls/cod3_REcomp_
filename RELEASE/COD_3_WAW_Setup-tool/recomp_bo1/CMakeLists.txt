# bo1 - Call of Duty: Black Ops (Xbox 360) static recompilation, the campaign and zombies program, built on the
# ReXGlue SDK: the same SDK, patches and toolchain as Call of Duty 3 (../recomp/). [waw-decomp]
# The multiplayer program (bo1mp) will be its own project in mp/, built from the same src/, as Call of Duty 4's.
#
# SDK boilerplate lives in generated/rexglue.cmake (the same file `rexglue init` writes; the codegen keeps it).

cmake_minimum_required(VERSION 3.25)
project(bo1 LANGUAGES C CXX)

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

set(BO1_SOURCES
    src/main.cpp
    src/bo1_app.cpp
    src/bo1_hooks.cpp
    src/bo1_shots.cpp
    ${COD3_RECOMP_SRC}/test_input.cpp
    src/bo1_f2.cpp  # [menus-native] the F2 settings window: Black Ops' pages and colors
    # [menus-native] The F2 settings window shared by every game's port (../recomp/src/f2/), built from its own folder,
    # the same list as Call of Duty 4's.
    ${COD3_RECOMP_SRC}/f2/f2_common.cpp
    ${COD3_RECOMP_SRC}/f2/f2_accel.cpp
    ${COD3_RECOMP_SRC}/f2/f2_crosshair_cs2.cpp
    ${COD3_RECOMP_SRC}/f2/crosshair_cs2_code.cpp
    ${COD3_RECOMP_SRC}/f2/f2_render_size.cpp
    ${COD3_RECOMP_SRC}/f2/f2_console.cpp
    ${COD3_RECOMP_SRC}/f2/f2_pad.cpp
    ${COD3_RECOMP_SRC}/f2/f2_rows.cpp
    ${COD3_RECOMP_SRC}/f2/f2_theme.cpp
    ${COD3_RECOMP_SRC}/f2/f2_window.cpp
)

if(WIN32)
    add_executable(bo1 WIN32 ${BO1_SOURCES})
else()
    add_executable(bo1 ${BO1_SOURCES})
endif()

target_include_directories(bo1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${COD3_RECOMP_SRC})

# GPU_PLUGINS stages librexgpu-xenos next to the executable (the Xenos GPU emulation: Vulkan, and Direct3D 12 on
# Windows). It is loaded at start; see Bo1App::OnPreSetup.
rexglue_setup_target(bo1 GPU_PLUGINS xenos)

# [menus-native] 2026-11-24: inputkit (the project's inputkit/ folder, the input code every game shares), which the
# shared F2 window's sensitivity scales use (recomp/src/f2/f2_accel.cpp: inputkit/sens_scale.h), found as Call of
# Duty 4's project finds it: the setup tool passes INPUTKIT_DIR; otherwise next to COD3_RECOMP_SRC's
# COD_3_WAW_Setup-tool/, or 2 up from this folder (a build in place).
if(NOT INPUTKIT_DIR)
    foreach(_ik IN ITEMS "${COD3_RECOMP_SRC}/../../../inputkit" "${CMAKE_CURRENT_SOURCE_DIR}/../../inputkit")
        if(NOT INPUTKIT_DIR AND EXISTS "${_ik}/cmake/Inputkit.cmake")
            get_filename_component(INPUTKIT_DIR "${_ik}" ABSOLUTE)
        endif()
    endforeach()
endif()
if(NOT EXISTS "${INPUTKIT_DIR}/cmake/Inputkit.cmake")
    message(FATAL_ERROR "bo1: the project's inputkit/ folder wasn't found (INPUTKIT_DIR, next to "
        "COD3_RECOMP_SRC's tool folder, or 2 up). Configure with -DINPUTKIT_DIR=<project>/inputkit.")
endif()
include("${INPUTKIT_DIR}/cmake/Inputkit.cmake")
inputkit_add_library()
target_link_libraries(bo1 PRIVATE inputkit::inputkit)

# MinGW (the Windows build made on Linux): the SDK's entry point is wWinMain, which MinGW's C runtime only calls when
# the program links with -municode.
if(MINGW)
    target_link_options(bo1 PRIVATE -municode)
endif()

# Linux: the SDK links librexruntime.so with an $ORIGIN rpath but does not stage it, so copy it next to the
# executable.
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET bo1 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:bo1>
        VERBATIM)
endif()
