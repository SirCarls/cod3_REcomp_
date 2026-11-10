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
    ${COD3_RECOMP_SRC}/test_input.cpp
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
