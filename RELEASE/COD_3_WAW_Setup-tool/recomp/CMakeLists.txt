# cod3 - Call of Duty 3 (Xbox 360) static recompilation, built on the ReXGlue SDK.
#
# SDK boilerplate lives in generated/rexglue.cmake (managed by `rexglue migrate`).

cmake_minimum_required(VERSION 3.25)
project(cod3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# [acct2-bro] 2026-11-22 (the user reports of 2026-11-23): find_package keeps the rexglue_DIR of the first configure
# even when CMAKE_PREFIX_PATH names another SDK now (a moved work folder, a renamed link), so a game could be built
# against an SDK it wasn't given. A cached rexglue_DIR outside every CMAKE_PREFIX_PATH entry is looked up again.
if(rexglue_DIR AND CMAKE_PREFIX_PATH)  # [acct2-bro] 2026-11-22
    set(_cod3_sdk_given OFF)  # [acct2-bro] 2026-11-22
    foreach(_cod3_prefix IN LISTS CMAKE_PREFIX_PATH)  # [acct2-bro] 2026-11-22
        file(TO_CMAKE_PATH "${_cod3_prefix}" _cod3_prefix)  # [acct2-bro] 2026-11-22
        string(FIND "${rexglue_DIR}/" "${_cod3_prefix}/" _cod3_at)  # [acct2-bro] 2026-11-22
        if(_cod3_at EQUAL 0)  # [acct2-bro] 2026-11-22
            set(_cod3_sdk_given ON)  # [acct2-bro] 2026-11-22
        endif()  # [acct2-bro] 2026-11-22
    endforeach()  # [acct2-bro] 2026-11-22
    if(NOT _cod3_sdk_given)  # [acct2-bro] 2026-11-22
        message(STATUS "cod3: the cached SDK (${rexglue_DIR}) isn't in CMAKE_PREFIX_PATH; looking for it again")
        unset(rexglue_DIR CACHE)  # [acct2-bro] 2026-11-22
    endif()  # [acct2-bro] 2026-11-22
endif()  # [acct2-bro] 2026-11-22
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
    src/game/use_hold.cpp  # [acct2-decomp] how long Use is held, and the hints' words (cod3_use_hold_ms)
    src/game/binoc_timing.cpp  # [rexglue-native thread2] 2026-11-24 the binoculars' wait at any frame rate (debug probe)
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
    src/game/text_copy_check.cpp  # [acct2-main] 2026-11-21 dev check: the game's copies of our translated text (cod3_text_copy_check)
    src/game/text_buffers.cpp  # [acct2-main] 2026-11-21 texts longer than the game's fixed buffers, whole (cod3_text_whole)
    src/game/render_resolution.cpp
    src/game/engine.cpp
    src/game/anim_clock.cpp  # [claude-9b] animation clocks in real time (cod3_fix_ui_clock, cod3_fix_frame_param)
    src/game/coronas.cpp  # [claude-9b] light coronas fade at the same speed at any frame rate (cod3_fix_corona_fade)
    src/game/muzzle_light.cpp  # [claude-9b-think] muzzle-flash lights held for a time, not one drawn frame (cod3_muzzle_light_ms)
    src/game/effects_debug.cpp  # [claude-9b] effects not loaded or not created (cod3_debug_effects; barrels)
    src/game/missing_effects.cpp  # [acct2-main] debug only: effect events a level lacks, and how it defines some (cod3_debug_effects)
    src/game/title_update_fixes.cpp  # [rexglue-native] title update 3's campaign fixes, redone on the disc code (cod3_tu_*; campaign only)
    src/game/fx_timing.cpp  # [acct2-cod3-fx] the effects frame by frame (cod3_debug_fx_frames), smoke flicker
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
    src/f2/f2_accel.cpp  # [f2-menus] 2026-11-11 mouse acceleration, sensitivity randomizer and scales (the ports' shared file)
    src/f2/f2_crosshair_cs2.cpp  # [acct2-ocr] 2026-11-14 CS2's crosshair, every option, one per split-screen player
    src/f2/crosshair_cs2_code.cpp  # [acct2-decomp] CS2's share codes (copied in by acct2-ocr)
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
    src/game/ngl_camo.cpp  # [acct2-bro] 2026-11-22 Camo lane: camos on the first-person guns (native)
    src/game/ngl_arms.cpp  # [acct2-bro] 2026-11-23 Camo lane: the gloves
    src/game/ngl_arms_rig.cpp  # [acct2-bro] 2026-11-23 Camo lane: the gloves
    src/game/ngl_gloves.cpp  # [acct2-bro] 2026-11-23 Camo lane: the gloves
    src/game/post_effects.cpp  # [post-effects] depth of field and motion blur on or off, in both programs
    src/game/relaunch.cpp
    src/game/print_hooks.cpp
    src/game/list_memory.cpp  # [claude-9b-think] the render lists' frame memory: room past its end, measured; scene nodes and the effects pool checked (both programs; cod3mp: mp/CMakeLists.txt, without the physical pages and the effects pool)
    src/game/native_memory.cpp  # [memory-360] 2026-11-11 the game's memory past the Xbox 360's limits: a report (cod3_native_memory_report) and more streaming banks (cod3_native_stream_banks_extra), both off; both programs
    src/game/native_memory_shadow.cpp  # [memory-360] 2026-11-11 stage 3's test: which arena-0 allocations reach the graphics side (cod3_native_memory_shadow, off; moves nothing); both programs
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
    src/game/console_budgets.cpp  # [acct2-decomp] 2026-11-22 detail and grass drawn farther than on the 360
    src/game/viewmodel.cpp  # [claude-9b] weapon offsets with Counter-Strike 2's commands (campaign only for now)
    src/game/viewmodel_flip.cpp  # [claude-9b] the weapon in the left hand (cod3_viewmodel_flip)
    src/game/aim_sensitivity.cpp  # [claude-9b] sensitivity per weapon class, hip and aiming, per player
    src/game/pad_settings.cpp  # [acct2-ocr] 2026-11-07 the controller settings as console settings
    src/game/menu_controller.cpp  # [acct2-ocr] 2026-11-07 the pause menu's CONTROLLER page rows
    src/game/menu_port_rows.cpp  # [acct2-ocr] 2026-11-07 the port's settings as the pause menu's Options rows
    src/game/sticky_aim.cpp  # [claude-9b] Sticky Aim at a fixed 60 Hz, the match's aim assist rules
    src/game/voice_language.cpp  # [voice]
    src/game/text_language.cpp  # [acct2-ocr] the game's text in another language, switchable (campaign only)
    src/game/text_rekey.cpp  # [acct2-main] 2026-11-15 a text set from a key follows a change of the text language
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
    src/ui/f2_tour.cpp  # [rexglue-native-f2] 2026-11-11 f2t1 the F2 window's test-only tour (cod3_debug_f2_tour, off)
    src/ui/hud_overlay.cpp
    src/ui/crosshair_texture.cpp  # [acct2-ocr] the game's crosshair drawn sharp (task 125)
    src/ui/problem_log.cpp
    src/ui/qte_spinner.cpp
    src/ui/server_tab.cpp  # [claude-9b] F2 > Game > Server: the match's rules, host only
    src/ui/camo_tab.cpp  # [acct2-bro] 2026-11-22 Camo lane: F2 > Game > Camos, per player and per gun
    src/ui/theme.cpp
    src/ui/waw_dvars.cpp
    src/menukit/menukit_data.cpp  # [waw-decomp/menus2] 2026-11-23 menukit, the native menus' core every port shares
    src/menukit/menukit_nav.cpp  # [waw-decomp/menus2]
    src/ui/menus/cod3_menukit.cpp  # [waw-decomp/menus2] native menus: CoD3's adapter, Pause > Options
    src/ui/menus/cod3_menukit_draw.cpp  # [waw-decomp/menus2] native menus: both looks
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

# [waw-decomp/menus2] 2026-11-23 The native menus' file (assets/menus/cod3/pause_options.toml, beside the code that
# reads it), built into the game as cod3_menus_data.inc (ui/menus/cod3_menukit.cpp) and copied beside the
# program as menus/cod3/pause_options.toml (read first, so it can be changed without a build). An edit
# of it runs the configure step again.
set(COD3_MENUS_TOML "${CMAKE_CURRENT_SOURCE_DIR}/assets/menus/cod3/pause_options.toml")
file(READ "${COD3_MENUS_TOML}" COD3_MENU_PAUSE_OPTIONS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${COD3_MENUS_TOML}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/ui/menus/cod3_menus_data.inc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/menus_data/cod3_menus_data.inc" @ONLY)
add_custom_command(TARGET cod3 POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:cod3>/menus/cod3"
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different "${COD3_MENUS_TOML}"
                           "$<TARGET_FILE_DIR:cod3>/menus/cod3/pause_options.toml")
target_include_directories(cod3 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/menus_data")

# GPU_PLUGINS stages librexgpu-xenos next to the executable (Vulkan / D3D12
# Xenos emulation). It is loaded at startup; see Cod3App::OnPreSetup.
rexglue_setup_target(cod3 GPU_PLUGINS xenos)

# [rust-renderer] The Rust renderer's static library (engine/crates/cod3_render),
# when the build has Rust for its target; cod3mp links it too (mp/CMakeLists.txt).
include(cmake/cod3_render.cmake)
cod3_render_link(cod3)

# [rexglue-native-platform-fix] 2026-11-23: xplat (the project's xplat/ folder, the platform layer every game shares):
# its CMake modules find ngl_vk below and embed the manifest. The setup tool passes XPLAT_DIR (c3w/native_args.py);
# building by hand, it is found as ngl_vk/ is: next to engine/, or two up from this folder.
if(NOT XPLAT_DIR)
    set(_cod3_xplat_hints "${CMAKE_CURRENT_SOURCE_DIR}/../../xplat")
    if(COD3_ENGINE_DIR)
        list(PREPEND _cod3_xplat_hints "${COD3_ENGINE_DIR}/../xplat")
    endif()
    foreach(_cod3_xplat IN LISTS _cod3_xplat_hints)
        if(NOT XPLAT_DIR AND EXISTS "${_cod3_xplat}/cmake/Xplat.cmake")
            get_filename_component(XPLAT_DIR "${_cod3_xplat}" ABSOLUTE)
        endif()
    endforeach()
endif()
if(NOT EXISTS "${XPLAT_DIR}/cmake/Xplat.cmake")
    message(FATAL_ERROR "cod3: the project's xplat/ folder wasn't found (XPLAT_DIR, or next to engine/). The setup "
        "tool passes it; building by hand, configure with -DXPLAT_DIR=<project>/xplat.")
endif()
include("${XPLAT_DIR}/cmake/Xplat.cmake")

# [input-port] 2026-11-24: inputkit (the project's inputkit/ folder, next to xplat/): the keyboard, controller and
# mouse code every game shares (key names, the bind models, mouse resampling; game/keybinds and game/mouse_look call
# it). Found next to xplat/ unless INPUTKIT_DIR is given; cod3mp links the same library (mp/CMakeLists.txt).
if(NOT INPUTKIT_DIR)
    get_filename_component(INPUTKIT_DIR "${XPLAT_DIR}/../inputkit" ABSOLUTE)
endif()
if(NOT EXISTS "${INPUTKIT_DIR}/cmake/Inputkit.cmake")
    message(FATAL_ERROR "cod3: the project's inputkit/ folder wasn't found (INPUTKIT_DIR, or next to xplat/). "
        "Building by hand, configure with -DINPUTKIT_DIR=<project>/inputkit.")
endif()
include("${INPUTKIT_DIR}/cmake/Inputkit.cmake")
inputkit_add_library()
target_link_libraries(cod3 PRIVATE inputkit::inputkit)

# [acct2-bro] ngl_vk (the project's ngl_vk/ folder, the Vulkan renderer Big Red One's port draws with), for the
# native renderer at NGL's level (dev/agents/acct2-bro/COD3-NGL-PLAN.md). [rexglue-native] 2026-11-09/11: Windows
# builds have it too. [acct2-bro] 2026-11-22 (owner 11-21 23:49 "It shouldn't be able to ship without the native
# renderer."): a game without it is not built; COD3_REQUIRE_NGL_VK=OFF builds without it on purpose (a test build),
# COD3_NGL_VK_WINDOWS=OFF builds the Windows game without it, and COD3_NGL_VK_BUILT (read by the setup tool) says
# which it was. [rexglue-native-platform-fix] 2026-11-23: the search (COD3_NGL_VK_DIR, next to engine/, two up),
# the Vulkan headers (a cross build: CMAKE_PREFIX_PATH only; else FindVulkan, else CMAKE_PREFIX_PATH), glslc (the
# setup tool's, or glslang through xplat's wrapper) and the stop are xplat_find_ngl_vk's (xplat/cmake/XplatNglVk.cmake,
# lifted from this file's 11-22 version); the options and COD3_NGL_VK_BUILT keep their names and meaning.
option(COD3_NGL_VK_WINDOWS "Build the native renderer (ngl_vk) into the Windows game too" ON)  # [rexglue-native]
option(COD3_REQUIRE_NGL_VK "Stop when the native renderer (ngl_vk) can't be built into the game" ON)  # [acct2-bro]
if(NOT WIN32 OR COD3_NGL_VK_WINDOWS)
    set(_cod3_ngl_vk_hints "${CMAKE_CURRENT_SOURCE_DIR}/../../ngl_vk")
    if(COD3_ENGINE_DIR)
        list(PREPEND _cod3_ngl_vk_hints "${COD3_ENGINE_DIR}/../ngl_vk")
    endif()
    xplat_find_ngl_vk(COD3 TARGET cod3 DEFINE COD3_NGL_VK REQUIRE HINTS ${_cod3_ngl_vk_hints})
    if(COD3_NGL_VK_BUILT)
        # ngl_side.cpp shows its picture through the SDK's Vulkan presenter (cod3_ngl_side_show), whose headers
        # include renderdoc_app.h by its bare name
        xplat_rex_renderdoc_include(cod3 COD3)
    endif()
else()
    message(STATUS "cod3: no ngl_vk (COD3_NGL_VK_WINDOWS=OFF); cod3_debug_ngl_side only counts")
    set(COD3_NGL_VK_BUILT OFF CACHE INTERNAL "The native renderer (ngl_vk) is in this build")
endif()

# MinGW (the Windows build made on Linux): the SDK's entry point is wWinMain, which MinGW's C runtime only calls
# when the program links with -municode (WMAIN). [rexglue-native-io] 2026-11-16: the application manifest (UTF-8
# code page, long paths; src/win/app.manifest). [rexglue-native-platform-fix] 2026-11-23: embedded by
# xplat_windows_app: MinGW through app.rc as before; the MSVC ABI (clang for x86_64-pc-windows-msvc + lld-link) as
# the linker's /MANIFESTINPUT, merged into the manifest it makes (app.rc there was a second manifest #1, which
# lld-link refuses: "duplicate resource", platform audit R2).
xplat_windows_app(cod3 WMAIN MANIFEST src/win/app.manifest RC src/win/app.rc)

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
