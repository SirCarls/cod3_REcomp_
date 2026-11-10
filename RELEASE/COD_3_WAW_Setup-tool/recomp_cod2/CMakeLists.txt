# cod2 - Call of Duty 2 (Xbox 360) recompiled for PC on the ReXGlue SDK, the
# same way and with the same SDK as Call of Duty 3 (../recomp/). [acct2-cod2]
#
# SDK boilerplate lives in generated/rexglue.cmake (written by `rexglue init`;
# never edited by hand). The translated game goes to generated/default/ when
# the cod2_codegen target runs (it is not part of the template).
#
# Graphics: the SDK's Xenos GPU emulation (librexgpu-xenos), Vulkan on Linux,
# Direct3D 12 and Vulkan on Windows (src/cod2_app.cpp picks one).

cmake_minimum_required(VERSION 3.25)
project(cod2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# A cross build (the Windows .exe made on Linux) can't run the SDK's own
# rexglue.exe for the codegen, so it names the build machine's rexglue here,
# as Call of Duty 3's does. The generated C++ is the same either way.
set(REXGLUE_CODEGEN "" CACHE FILEPATH "rexglue that runs the codegen (default: the SDK's)")
if(REXGLUE_CODEGEN)
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION "${REXGLUE_CODEGEN}"
        IMPORTED_LOCATION_RELEASE "${REXGLUE_CODEGEN}")
endif()

set(COD2_SOURCES
    src/main.cpp
    src/cod2_app.cpp
    src/game/audio_fpu.cpp  # the audio callback's floating-point state (SDK host thread)
    src/game/diagnostics.cpp  # the game's console lines, errors, menus and zones in the log
    src/game/debug_shots.cpp  # pictures of the game for test runs (cod2_debug_shot_seconds)
    src/game/test_start.cpp  # test runs: press START, then a console command (cod2_debug_*)
    src/game/native_census.cpp  # tests: what the back end draws a frame (cod2_debug_native_census)
    src/game/native_capture.cpp  # native step N1: each draw's submission recorded (cod2_native_capture)
    src/game/frame_time.cpp  # tests: where a frame's time goes (cod2_debug_frame_time)
    src/game/frame_rate.cpp  # the guest vblank at 1000 Hz and the frame limit (cod2_max_fps)
    src/game/front_end.cpp  # launch to the main menu: keyboard pad, cod2_skip_intro_movies
    src/game/movement.cpp  # the player's movement in fixed steps (cod2_move_hz)
    src/game/motion_audit.cpp  # tests: how entities and the view move per frame (cod2_debug_entity_steps)
)

if(WIN32)
    add_executable(cod2 WIN32 ${COD2_SOURCES})
else()
    add_executable(cod2 ${COD2_SOURCES})
endif()

target_include_directories(cod2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# GPU_PLUGINS stages librexgpu-xenos next to the program; it is loaded at
# start (Cod2App::OnPreSetup).
rexglue_setup_target(cod2 GPU_PLUGINS xenos)

# MinGW: the SDK's entry point is wWinMain, which MinGW's C runtime calls
# only when the program links with -municode.
if(MINGW)
    target_link_options(cod2 PRIVATE -municode)
endif()

# Linux: the SDK links librexruntime.so with an $ORIGIN rpath but doesn't
# put it next to the program, so copy it there.
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET cod2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:cod2>
        VERBATIM)
endif()

# cod2mp, the multiplayer program (default_mp.xex), built next to cod2 from
# mp/ (its own codegen glue), last, as Call of Duty 3's cod3mp is. Left out
# when the disc's multiplayer program isn't in assets/.
option(COD2_BUILD_MP "Build cod2mp, the multiplayer program (default_mp.xex)" ON)
if(COD2_BUILD_MP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/default_mp.xex")
    add_subdirectory(mp)
endif()
