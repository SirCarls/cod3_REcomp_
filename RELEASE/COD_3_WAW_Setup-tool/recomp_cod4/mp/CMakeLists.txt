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
    ${COD3_RECOMP_SRC}/test_input.cpp
)

if(WIN32)
    add_executable(cod4mp WIN32 ${COD4MP_SOURCES})
else()
    add_executable(cod4mp ${COD4MP_SOURCES})
endif()

target_compile_definitions(cod4mp PRIVATE COD4_MP=1)
target_include_directories(cod4mp PRIVATE ${COD4_SRC} ${COD3_RECOMP_SRC})

rexglue_setup_target(cod4mp GPU_PLUGINS xenos)

if(MINGW)
    target_link_options(cod4mp PRIVATE -municode)
endif()

if(UNIX AND NOT APPLE)
    add_custom_command(TARGET cod4mp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:cod4mp>
        VERBATIM)
endif()
