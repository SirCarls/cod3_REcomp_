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
    ${COD2_SRC}/game/native_side.cpp  # [waw-decomp] cod2_debug_native_side (2026-11-11: this program's addresses too)
    ${COD2_SRC}/game/native_hooks_mp.cpp  # [waw-decomp] 2026-11-11 the native side's three hooks in this program
    ${COD2_SRC}/game/native_compare.cpp  # [waw-decomp] 2026-11-11 the native picture beside the game's, live
    ${COD2_SRC}/game/native_window.cpp  # [waw-decomp] 2026-11-11 4z61 the native picture as the game's (native Vulkan)
    ${COD2_F2_SOURCES}  # [f2-menus] the F2 settings window (the parent CMakeLists.txt's list; may be empty)
    ${COD2_SRC}/cod2_pc_input.cpp  # [f2-menus] the PC copy's keys and the game's own mouse look
    ${COD2_SRC}/game/dvar_peek.cpp  # [f2-menus] tests: the game's own dvars in the log (cod2_debug_dvars)
)

if(WIN32)
    add_executable(cod2mp WIN32 ${COD2MP_SOURCES})
else()
    add_executable(cod2mp ${COD2MP_SOURCES})
endif()

target_include_directories(cod2mp PRIVATE ${COD2_SRC})
if(COD2_F2_SOURCES)  # [f2-menus]
    target_include_directories(cod2mp PRIVATE ${COD3_RECOMP_SRC})
    target_compile_definitions(cod2mp PRIVATE COD2_HAVE_F2=1)
endif()
target_compile_definitions(cod2mp PRIVATE COD2_MP=1)
# [waw-decomp] 2026-11-11: the native renderer (cod2_debug_native_side) here too, from the ngl_vk the parent folder
# found (its CMakeLists.txt; without it this program only counts, as the campaign does).
if(TARGET ngl_vk)
    target_link_libraries(cod2mp PRIVATE ngl_vk)
    target_compile_definitions(cod2mp PRIVATE COD2_NGL_VK=1)
    if(COD2_RENDERDOC_INCLUDE_DIR)  # [waw-decomp] 4z61: the SDK's renderdoc_app.h, found by the parent folder
        target_include_directories(cod2mp PRIVATE "${COD2_RENDERDOC_INCLUDE_DIR}")
    endif()
endif()

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
