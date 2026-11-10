# cod2mp - Call of Duty 2's multiplayer program (default_mp.xex), the second
# ReXGlue target next to cod2. [acct2-cod2]
#
# Added by the top-level CMakeLists.txt (add_subdirectory), last, as Call of
# Duty 3's recomp/mp/ is: this folder's generated glue defines the SDK macros
# again for its own target. The program around the game is the campaign's
# (../src/cod2_app.cpp), built with COD2_MP for this program's names.

project(cod2mp LANGUAGES C CXX)

# The campaign's generated source list and include folder come down from the
# parent folder's scope. Before this program's first codegen its own list
# doesn't exist yet, and the glue below would then claim the campaign's
# sources as its outputs too (ninja: "multiple rules generate").
unset(REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
unset(REXGLUE_ENTRYPOINT_INCLUDE_DIR)
unset(GENERATED_SOURCES)

include(generated/rexglue.cmake)

set(COD2_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

set(COD2MP_SOURCES
    ${COD2_SRC}/main_mp.cpp
    ${COD2_SRC}/cod2_app.cpp
    ${COD2_SRC}/game/diagnostics.cpp  # its hooks are the campaign's only, so far
    ${COD2_SRC}/game/debug_shots.cpp
    ${COD2_SRC}/game/test_start.cpp
    ${COD2_SRC}/game/audio_fpu.cpp  # the audio worker's floating-point state (missing here until 2026-11-07 19:36)
    ${COD2_SRC}/game/front_end.cpp  # the keyboard pad (the start-up film hook is the campaign's only)
    ${COD2_SRC}/game/frame_rate.cpp  # the guest vblank at 1000 Hz and the frame limit (cod2_max_fps)
)

if(WIN32)
    add_executable(cod2mp WIN32 ${COD2MP_SOURCES})
else()
    add_executable(cod2mp ${COD2MP_SOURCES})
endif()

target_include_directories(cod2mp PRIVATE ${COD2_SRC})
target_compile_definitions(cod2mp PRIVATE COD2_MP=1)

rexglue_setup_target(cod2mp GPU_PLUGINS xenos)

if(MINGW)
    target_link_options(cod2mp PRIVATE -municode)
endif()

if(UNIX AND NOT APPLE)
    add_custom_command(TARGET cod2mp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:cod2mp>
        VERBATIM)
endif()
