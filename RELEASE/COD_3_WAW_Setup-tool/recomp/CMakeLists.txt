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
    src/game/crt_native.cpp  # [native-crt] the C runtime's busiest functions as native code
    src/game/fmod_native.cpp  # [native-fmod] FMOD's reverb kernel as native code
    src/game/lzo_native.cpp  # [native-lzo] the game's LZO1X decompressor as native code
    src/game/crt_math_native.cpp  # [native-math] the C runtime's math and character functions as native code
    src/game/fmod_resampler_native.cpp  # [native-resampler] FMOD's linear resampler as native code
    src/game/fmod_mix_native.cpp  # [native-fmod-mix] FMOD's channel mix kernels as native code
    src/game/crt_printf_native.cpp  # [native-printf] the C runtime's formatted output, sprintf and the like, as native code
    src/game/crt_strtol_native.cpp  # [native-strtol] the C runtime's atoi, strtol and strtoul as native code
    src/game/render_resolution.cpp
    src/game/engine.cpp
    src/game/anim_clock.cpp  # [claude-9b] animation clocks in real time (cod3_fix_ui_clock, cod3_fix_frame_param)
    src/game/coronas.cpp  # [claude-9b] light coronas fade at the same speed at any frame rate (cod3_fix_corona_fade)
    src/game/muzzle_light.cpp  # [claude-9b-think] muzzle-flash lights held for a time, not one drawn frame (cod3_muzzle_light_ms)
    src/game/effects_debug.cpp  # [claude-9b] effects not loaded or not created (cod3_debug_effects; barrels)
    src/game/fov.cpp
    src/game/frame_rate.cpp
    src/game/function_profiler.cpp  # [hw-scaling]
    src/game/gpu_wait.cpp  # [hw-scaling]
    src/game/job_wait.cpp  # [hw-scaling]
    src/game/render_job_timing.cpp  # [hw-scaling] the guest render job's time (campaign only)
    src/game/game_api.cpp
    src/game/hd_textures.cpp
    src/game/heap_check.cpp  # [heap-check] a check of the game's C heap (a test switch; campaign only)
    src/game/job_wake.cpp
    src/game/machine_profile.cpp
    src/game/keybinds/binds.cpp  # [keybinds] the keyboard's own actions (campaign only)
    src/game/keybinds/keys.cpp  # [keybinds]
    src/game/keybinds/model.cpp  # [keybinds]
    src/game/keybinds/pad_model.cpp  # [acct2-main] the controller's own binds per player
    src/game/keybinds/pad_binds.cpp  # [acct2-main]
    src/game/menu_fov.cpp
    src/game/menu_aim.cpp  # [claude-9b] Pause > Options > AIM: sensitivity per weapon class, crosshair
    src/game/menu_slider.cpp  # [claude-9b] slider rows: the number box you can type in
    src/game/menu_video.cpp
    src/game/menu_video_rows.cpp  # [menus-categories] the video rows, shared with cod3mp
    src/game/options_pages.cpp  # [menus-categories] which rows each Options category page has, both programs
    src/game/mouse_look.cpp
    src/game/movement/hooks_sp.cpp  # [movement] sprint and the CoD4 movement style (campaign only)
    src/game/movement/menu_movement.cpp  # [movement]
    src/game/movement/rules.cpp  # [movement]
    src/game/movement/settings.cpp  # [movement]
    src/game/movement/sprint_input.cpp  # [movement]
    src/game/multiplayer.cpp
    src/game/protected_dvars.cpp  # [movement] server_sided_protected dvars
    src/game/pc_menu.cpp
    src/game/button_prompts.cpp
    src/game/stick_arrow_keys.cpp  # [claude-9b] the battle actions' stick picture as a MOUSE key with the keyboard (campaign only)
    src/game/rumble.cpp  # [rumble] the first controller doesn't vibrate while the keyboard and mouse are in use
    src/game/perf_stats.cpp
    src/game/physics_step.cpp  # [claude-9b] the physics in fixed steps, 60 a second, whatever the frame rate (campaign only)
    src/game/server_step.cpp  # [acct2-main] the game in fixed updates, 60 a second, drawn smoothly between them (campaign only)
    src/game/ride_bones.cpp  # [claude-9b-think] the ridden vehicle's turret and gun between updates (cod3_blend_ride_bones; campaign only)
    src/game/profile_timers.cpp  # [claude-9b-think] the console build's profiling timers in the scene build and traces skipped (cod3_skip_profile_timers; campaign only)
    src/game/render_list_guard.cpp  # [claude-9b] a freed object in the render lists is skipped and logged, not a crash (campaign only)
    src/game/ai_census.cpp  # [claude-9b-think] cod3_debug_ai_census: AI shots and spawns a second, to find what scales with fps (campaign only)
    src/game/ai_aim.cpp  # [claude-9b-think] the AI's sample of the player's movement taken as at 60 fps; census line 3: hit decisions vs hurt (campaign only)
    src/game/ai_shots.cpp  # [claude-9b-think] cod3_debug_ai_shots: each AI shot near the player with its guest stack, aim error and trace (campaign only)
    src/game/view_cull.cpp  # [claude-9b] the game's on-screen check as wide as the picture: logs, and cod3_cull_match_view (campaign only)
    src/game/ngl_census.cpp  # [acct2-bro] cod3_debug_ngl_census: what NGL draws a frame, for the native renderer at NGL's level (logging only; campaign first)
    src/game/ngl_side.cpp  # [acct2-bro] cod3_debug_ngl_side: the frame's 2D drawn by ngl_vk beside the GPU emulation and saved next to the game's screenshot (a check; campaign first)
    src/game/post_effects.cpp  # [post-effects] depth of field and motion blur on or off, in both programs
    src/game/relaunch.cpp
    src/game/print_hooks.cpp
    src/game/list_memory.cpp  # [claude-9b-think] the render lists' frame memory: room past its end, measured; scene nodes and the effects pool checked (both programs; cod3mp: mp/CMakeLists.txt, without the physical pages and the effects pool)
    src/game/script_fiber.cpp
    src/game/script_threads.cpp
    src/game/settings_sync.cpp
    src/game/qte_actions.cpp
    src/game/screenshot.cpp
    src/game/shadows.cpp
    src/game/sound_stall.cpp  # [voice] voices played twice: the sound library's stuck-sound check, in both programs
    src/game/speedtree_fix.cpp  # [speedtree] the trees' buffers released once, and only those made
    src/game/thread_profiler.cpp
    src/game/ultrawide.cpp
    src/game/viewmodel.cpp  # [claude-9b] weapon offsets with Counter-Strike 2's commands (campaign only for now)
    src/game/viewmodel_flip.cpp  # [claude-9b] the weapon in the left hand (cod3_viewmodel_flip)
    src/game/aim_sensitivity.cpp  # [claude-9b] sensitivity per weapon class, hip and aiming, per player
    src/game/sticky_aim.cpp  # [claude-9b] Sticky Aim at a fixed 60 Hz, the match's aim assist rules
    src/game/voice_language.cpp  # [voice]
    src/game/text_language.cpp  # [acct2-ocr] the game's text in another language, switchable (campaign only)
    src/game/font_fill.cpp  # [acct2-ocr] the glyphs the game's fonts lack, added in memory (campaign only)
    src/ui/text_overlay.cpp  # [acct2-ocr] Russian/Chinese/Japanese/Korean text, drawn by us
    src/ui/text_native.cpp  # [acct2-ocr] the same text drawn through the game's own text path
    src/game/color_text.cpp  # [acct2-ocr] color codes inside a line (names), both programs
    src/ui/film_text.cpp  # [acct2-ocr] the films' burned-in cards in the chosen language (campaign only)
    src/game/watchdog.cpp
    src/render/rust_renderer.cpp  # [rust-renderer] the Rust renderer's library, checked at start (both programs)
    src/render/native_compare.cpp  # [rexglue-native] 2026-11-09 the native renderer and the emulated GPU side by side, live (cod3_native_compare; campaign first)
    src/ui/announcement_feed.cpp
    src/ui/console_pages.cpp
    src/ui/console_view.cpp
    src/ui/dvar_domain.cpp
    src/ui/dvar_help.cpp
    src/ui/game_console.cpp
    src/ui/hud_overlay.cpp
    src/ui/problem_log.cpp
    src/ui/qte_spinner.cpp
    src/ui/server_tab.cpp  # [claude-9b] F2 > Game > Server: the match's rules, host only
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

# [claude-9b-think] The prompts' keyboard-and-mouse texts by string key (config/prompt_text_kbm.tsv), built into the
# game as prompt_text_kbm.inc (game/button_prompts.cpp; userdata/prompt_text_kbm.tsv adds to them at start). An edit
# of the table runs the configure step again.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/prompt_text_kbm.tsv" COD3_PROMPT_TEXT_KBM)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/game/prompt_text_kbm.inc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/prompt_text/prompt_text_kbm.inc" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/config/prompt_text_kbm.tsv")
target_include_directories(cod3 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/prompt_text")

# GPU_PLUGINS stages librexgpu-xenos next to the executable (Vulkan / D3D12
# Xenos emulation). It is loaded at startup; see Cod3App::OnPreSetup.
rexglue_setup_target(cod3 GPU_PLUGINS xenos)

# [rust-renderer] The Rust renderer's static library (engine/crates/cod3_render),
# when the build has Rust for its target; cod3mp links it too (mp/CMakeLists.txt).
include(cmake/cod3_render.cmake)
cod3_render_link(cod3)

# [acct2-bro] ngl_vk (the project's ngl_vk/ folder, the Vulkan renderer Big Red One's port draws with), for the
# native renderer at NGL's level (dev/agents/acct2-bro/COD3-NGL-PLAN.md). Only the cod3_debug_ngl_side check uses it,
# off by default; Linux builds with the Vulkan headers and glslc only, the others build without it (the check then
# only counts). COD3_NGL_VK_DIR defaults to engine/'s neighbor, or the folder two up from this one.
# [rexglue-native] 2026-11-09: Windows builds have it too (below); the setup tool gives both builds COD3_ENGINE_DIR.
set(COD3_NGL_VK_DIR "" CACHE PATH "The project's ngl_vk/ folder (the native renderer's Vulkan library)")
set(_cod3_ngl_vk "${COD3_NGL_VK_DIR}")
if(NOT _cod3_ngl_vk AND COD3_ENGINE_DIR AND EXISTS "${COD3_ENGINE_DIR}/../ngl_vk/CMakeLists.txt")
    get_filename_component(_cod3_ngl_vk "${COD3_ENGINE_DIR}/../ngl_vk" ABSOLUTE)
endif()
if(NOT _cod3_ngl_vk AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../ngl_vk/CMakeLists.txt")
    get_filename_component(_cod3_ngl_vk "${CMAKE_CURRENT_SOURCE_DIR}/../../ngl_vk" ABSOLUTE)
endif()
# [rexglue-native] 2026-11-09: Windows too. ngl_vk opens the Vulkan loader when it starts, so it needs only the Vulkan
# headers and glslc (which runs on the build PC). The Windows build made on Linux takes the headers from its
# CMAKE_PREFIX_PATH, the SDK's install (rexglue-sdk/out/install/mingw/include/vulkan), never the build PC's own.
# [rexglue-native] 2026-11-10 (2.40.0): Windows builds stay without ngl_vk for now (COD3_NGL_VK_WINDOWS=ON to try it):
# the release candidate's Windows game drew the forest's world white under Wine (owner 13:20: "forest within wine
# looks bad now"), so on Windows cod3_native_vulkan is still ignored, and said so in the log, until that is found.
option(COD3_NGL_VK_WINDOWS "Build the native renderer (ngl_vk) into the Windows game too (not ready)" OFF)  # [rexglue-native]
if(_cod3_ngl_vk AND EXISTS "${_cod3_ngl_vk}/CMakeLists.txt" AND (NOT WIN32 OR COD3_NGL_VK_WINDOWS))  # [rexglue-native]
    if(CMAKE_CROSSCOMPILING)  # [rexglue-native]
        find_path(NGL_VK_VULKAN_INCLUDE_DIR vulkan/vulkan.h NO_CMAKE_SYSTEM_PATH NO_SYSTEM_ENVIRONMENT_PATH  # [rexglue-native]
            NO_CMAKE_ENVIRONMENT_PATH)  # [rexglue-native]
    else()  # [rexglue-native]
        find_package(Vulkan QUIET)
        set(NGL_VK_VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIR}")  # [rexglue-native] the headers only (as before: the system's)
    endif()  # [rexglue-native]
    find_program(COD3_NGL_VK_GLSLC NAMES glslc HINTS "${Vulkan_GLSLC_EXECUTABLE}")
    if(NGL_VK_VULKAN_INCLUDE_DIR AND COD3_NGL_VK_GLSLC)  # [rexglue-native] (was Vulkan_FOUND: the loader too)
        add_subdirectory("${_cod3_ngl_vk}" "${CMAKE_CURRENT_BINARY_DIR}/ngl_vk")
        target_link_libraries(cod3 PRIVATE ngl_vk)
        target_compile_definitions(cod3 PRIVATE COD3_NGL_VK=1)
        message(STATUS "cod3: ngl_vk from ${_cod3_ngl_vk} (cod3_debug_ngl_side)")
        # ngl_side.cpp shows its picture through the SDK's Vulkan presenter (cod3_ngl_side_show). Those headers
        # include renderdoc_app.h by its bare name; the SDK keeps it in include/renderdoc (installed) or
        # thirdparty/renderdoc (source tree), neither on the SDK targets' include path.
        get_target_property(_cod3_rex_includes rex::runtime INTERFACE_INCLUDE_DIRECTORIES)
        find_path(COD3_RENDERDOC_INCLUDE_DIR renderdoc_app.h
            HINTS ${_cod3_rex_includes} "${rexglue_DIR}/../../../include" "${REXSDK_DIR}/thirdparty"
            PATH_SUFFIXES renderdoc NO_DEFAULT_PATH)
        if(COD3_RENDERDOC_INCLUDE_DIR)
            target_include_directories(cod3 PRIVATE "${COD3_RENDERDOC_INCLUDE_DIR}")
        else()
            message(WARNING "cod3: the SDK's renderdoc_app.h wasn't found; ngl_side.cpp won't build")
        endif()
    else()
        message(STATUS "cod3: no ngl_vk (Vulkan headers or glslc missing); cod3_debug_ngl_side only counts")
    endif()
else()
    message(STATUS "cod3: no ngl_vk (no ngl_vk/ folder found, or Windows); cod3_debug_ngl_side only counts")
endif()

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
