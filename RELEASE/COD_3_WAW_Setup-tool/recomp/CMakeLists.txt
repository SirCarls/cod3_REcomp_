# cod3 - Call of Duty 3 (Xbox 360) static recompilation, built on the ReXGlue SDK.
#
# SDK boilerplate lives in generated/rexglue.cmake (managed by `rexglue migrate`).

cmake_minimum_required(VERSION 3.25)
project(cod3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# A cross build (the Windows .exe made on Linux) can't run the SDK's own
# rexglue.exe for the codegen, so it gives the build machine's rexglue as
# REXGLUE_CODEGEN. The generated C++ is the same either way.
# (generated/rexglue.cmake is rewritten by the codegen, so this lives here.)
set(REXGLUE_CODEGEN "" CACHE FILEPATH "rexglue that runs the codegen (default: the SDK's)")
if(REXGLUE_CODEGEN)
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION "${REXGLUE_CODEGEN}"
        IMPORTED_LOCATION_RELEASE "${REXGLUE_CODEGEN}")
endif()

# Each level DLL module (libcod3_<level>) gets the functions its mid-asm
# hooks call; see src/game/level_hooks.cpp and config/<level>.toml.
set(COD3_LEVELS blkbrn chambois credits crssrds falaise forest fuelplnt hostage
    island laison mace2 mayenne nightd saint_lo stbert)
foreach(level IN LISTS COD3_LEVELS)
    if(TARGET cod3_${level})
        target_sources(cod3_${level} PRIVATE src/game/level_hooks.cpp)
    endif()
endforeach()

set(COD3_SOURCES
    src/main.cpp
    src/cod3_app.cpp
    src/game/aim_assist.cpp
    src/game/cutscene_skip.cpp
    src/game/diagnostics.cpp
    src/game/cheat_guard.cpp
    src/game/cheat_watch.cpp
    src/game/color_output.cpp
    src/game/render_resolution.cpp
    src/game/engine.cpp
    src/game/fov.cpp
    src/game/frame_rate.cpp
    src/game/function_profiler.cpp  # [hw-scaling]
    src/game/gpu_wait.cpp  # [hw-scaling]
    src/game/job_wait.cpp  # [hw-scaling]
    src/game/render_job_timing.cpp  # [hw-scaling] the guest render job's time (campaign only)
    src/game/game_api.cpp
    src/game/hd_textures.cpp
    src/game/job_wake.cpp
    src/game/machine_profile.cpp
    src/game/keybinds/binds.cpp  # [keybinds] the keyboard's own actions (campaign only)
    src/game/keybinds/keys.cpp  # [keybinds]
    src/game/keybinds/model.cpp  # [keybinds]
    src/game/menu_fov.cpp
    src/game/menu_video.cpp
    src/game/mouse_look.cpp
    src/game/movement/hooks_sp.cpp  # [movement] sprint and the CoD4 movement style (campaign only)
    src/game/movement/mantle.cpp  # [movement]
    src/game/movement/menu_movement.cpp  # [movement]
    src/game/movement/rules.cpp  # [movement]
    src/game/movement/settings.cpp  # [movement]
    src/game/movement/sprint_input.cpp  # [movement]
    src/game/multiplayer.cpp
    src/game/protected_dvars.cpp  # [movement] server_sided_protected dvars
    src/game/pc_menu.cpp
    src/game/button_prompts.cpp
    src/game/perf_stats.cpp
    src/game/relaunch.cpp
    src/game/print_hooks.cpp
    src/game/script_fiber.cpp
    src/game/script_threads.cpp
    src/game/settings_sync.cpp
    src/game/qte_actions.cpp
    src/game/screenshot.cpp
    src/game/shadows.cpp
    src/game/thread_profiler.cpp
    src/game/ultrawide.cpp
    src/game/voice_language.cpp  # [voice]
    src/game/watchdog.cpp
    src/ui/announcement_feed.cpp
    src/ui/console_pages.cpp
    src/ui/console_view.cpp
    src/ui/dvar_domain.cpp
    src/ui/game_console.cpp
    src/ui/hud_overlay.cpp
    src/ui/problem_log.cpp
    src/ui/qte_spinner.cpp
    src/ui/theme.cpp
    src/ui/waw_dvars.cpp
    src/test_input.cpp
)

if(WIN32)
    add_executable(cod3 WIN32 ${COD3_SOURCES})
else()
    add_executable(cod3 ${COD3_SOURCES})
endif()

target_include_directories(cod3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# GPU_PLUGINS stages librexgpu-xenos next to the executable (Vulkan / D3D12
# Xenos emulation). It is loaded at startup; see Cod3App::OnPreSetup.
rexglue_setup_target(cod3 GPU_PLUGINS xenos)

# MinGW (the Windows build made on Linux): the SDK's entry point is wWinMain,
# which MinGW's C runtime only calls when the program links with -municode.
if(MINGW)
    target_link_options(cod3 PRIVATE -municode)
endif()

# Windows: timeBeginPeriod (1 ms timer resolution, see Cod3App::OnPreSetup).
if(WIN32)
    # winmm: timeBeginPeriod (1 ms sleeps); shell32: CommandLineToArgvW
    # (game/relaunch.cpp keeps the command line when the game reboots to its menu).
    target_link_libraries(cod3 PRIVATE winmm shell32)
endif()

# Linux: the SDK links librexruntime.so with an $ORIGIN rpath but does not
# stage it, so copy it next to the executable.
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET cod3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:cod3>
        VERBATIM)
    # An SDK built with FidelityFX links librexruntime.so against it, so it
    # has to sit next to the executable too. (The setup tool builds the SDK
    # without FidelityFX on Linux; this covers SDKs built by hand.)
    if(TARGET rex::amd_fidelityfx_vk)
        add_custom_command(TARGET cod3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rex::amd_fidelityfx_vk> $<TARGET_FILE_DIR:cod3>
            VERBATIM)
    endif()
endif()

# The decomp trace build (docs/DECOMP_TRACE.md): every recompiled function
# notes when it first runs and who calls it, for mapping unnamed functions.
# Only in a build folder of its own; normal builds are unchanged.
option(COD3_DECOMP_TRACE "Build the decomp trace version of the game (slower; maps functions)" OFF)
if(COD3_DECOMP_TRACE)
    include(src/game/trace/decomp_trace.cmake)
endif()

# [cod3mp] The multiplayer program, codmp_xenonf.xex, built next to cod3 from
# mp/ (its own codegen glue; docs/MULTIPLAYER.md). Last, after cod3's setup:
# mp/'s generated glue defines the SDK macros again for its own target.
# Left out when the disc's multiplayer executable isn't in assets/.
option(COD3_BUILD_MP "Build cod3mp, the multiplayer program (codmp_xenonf.xex)" ON)
if(COD3_BUILD_MP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/codmp_xenonf.xex")
    add_subdirectory(mp)
endif()
