# cod3mp - Call of Duty 3's multiplayer program (codmp_xenonf.xex), the
# second ReXGlue target next to cod3 (docs/MULTIPLAYER.md).
#
# Added by the top-level CMakeLists.txt (add_subdirectory). Its own folder
# because the codegen writes generated/rexglue.cmake next to the manifest:
# this folder's glue builds cod3mp, the top folder's builds cod3. The game
# code is the top folder's src/.

project(cod3mp LANGUAGES C CXX)

include(generated/rexglue.cmake)

set(COD3_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# The campaign's port code (F2, the ~ console, raw mouse aim, the frame
# limiter, FOV, native resolution ...) and this program's own. A shared
# file's hooks and addresses become this program's through
# game/mp/mp_aliases.h and game/mp/addresses_mp.h, which tools/mp_aliases.py
# makes from config/campaign_map.tsv (every campaign function and address
# a shared file uses, checked in both images); campaign-only code (films,
# battle actions, the campaign's own menu pages) is left out or bound to
# nothing (game/mp/campaign_only.cpp).
set(COD3MP_SOURCES
    ${COD3_SRC}/main_mp.cpp
    ${COD3_SRC}/cod3mp_app.cpp
    ${COD3_SRC}/game/button_prompts.cpp  # [mp-menus] keys and controller buttons in the game's prompts, per player; its pad state hook calls the keybinds' too
    ${COD3_SRC}/game/cheat_guard.cpp
    ${COD3_SRC}/game/cheat_watch.cpp
    ${COD3_SRC}/game/color_output.cpp
    ${COD3_SRC}/game/crt_native.cpp  # [native-crt] the C runtime's busiest functions as native code
    ${COD3_SRC}/game/fmod_native.cpp  # [native-fmod] FMOD's reverb kernel as native code
    ${COD3_SRC}/game/lzo_native.cpp  # [native-lzo] the game's LZO1X decompressor as native code
    ${COD3_SRC}/game/crt_math_native.cpp  # [native-math] the C runtime's math and character functions as native code
    ${COD3_SRC}/game/fmod_resampler_native.cpp  # [native-resampler] FMOD's linear resampler as native code
    ${COD3_SRC}/game/fmod_mix_native.cpp  # [native-fmod-mix] FMOD's channel mix kernels as native code
    ${COD3_SRC}/game/crt_printf_native.cpp  # [native-printf] the C runtime's formatted output, sprintf and the like, as native code
    ${COD3_SRC}/game/crt_strtol_native.cpp  # [native-strtol] the C runtime's atoi, strtol and strtoul as native code
    ${COD3_SRC}/game/diagnostics.cpp
    ${COD3_SRC}/game/engine.cpp
    ${COD3_SRC}/game/anim_clock.cpp  # [claude-9b] animation clocks in real time (cod3_fix_ui_clock, cod3_fix_frame_param)
    ${COD3_SRC}/game/coronas.cpp  # [claude-9b] light coronas fade at the same speed at any frame rate (cod3_fix_corona_fade)
    ${COD3_SRC}/game/muzzle_light.cpp  # [claude-9b-think] muzzle-flash lights held for a time, not one drawn frame (cod3_muzzle_light_ms)
    ${COD3_SRC}/game/fov.cpp
    ${COD3_SRC}/game/frame_rate.cpp
    ${COD3_SRC}/game/function_profiler.cpp
    ${COD3_SRC}/game/game_api.cpp
    ${COD3_SRC}/game/gpu_wait.cpp
    ${COD3_SRC}/game/hd_textures.cpp
    ${COD3_SRC}/game/job_wait.cpp
    ${COD3_SRC}/game/job_wake.cpp
    ${COD3_SRC}/game/keybinds/binds.cpp  # [keybinds] keys that call the game's own actions
    ${COD3_SRC}/game/keybinds/keys.cpp  # [keybinds]
    ${COD3_SRC}/game/keybinds/model.cpp  # [keybinds]
    ${COD3_SRC}/game/level_hooks.cpp
    ${COD3_SRC}/game/machine_profile.cpp
    ${COD3_SRC}/game/mouse_look.cpp
    ${COD3_SRC}/game/pc_menu.cpp  # [mp-menus] mouse and keyboard in the game's menus, shared with the campaign
    ${COD3_SRC}/game/perf_stats.cpp
    ${COD3_SRC}/game/post_effects.cpp  # [post-effects] depth of field and motion blur on or off, in both programs
    ${COD3_SRC}/game/menu_video_rows.cpp  # [menus-categories] the video rows of Pause > OPTIONS > VIDEO
    ${COD3_SRC}/game/options_pages.cpp  # [menus-categories] which rows each Options category page has
    ${COD3_SRC}/game/print_hooks.cpp
    ${COD3_SRC}/game/relaunch.cpp
    ${COD3_SRC}/game/rumble.cpp  # [rumble] the first controller doesn't vibrate while the keyboard and mouse are in use
    ${COD3_SRC}/game/render_resolution.cpp
    ${COD3_SRC}/game/screenshot.cpp
    ${COD3_SRC}/game/script_fiber.cpp
    ${COD3_SRC}/game/script_threads.cpp
    ${COD3_SRC}/game/settings_sync.cpp
    ${COD3_SRC}/game/sound_stall.cpp  # [voice] voices played twice: the sound library's stuck-sound check, in both programs
    ${COD3_SRC}/game/shadows.cpp
    ${COD3_SRC}/game/speedtree_fix.cpp  # [speedtree] the trees' buffers released once, and only those made
    ${COD3_SRC}/game/thread_profiler.cpp
    ${COD3_SRC}/game/ultrawide.cpp
    ${COD3_SRC}/game/viewmodel.cpp  # [claude-9b] weapon offsets with Counter-Strike 2's commands
    ${COD3_SRC}/game/viewmodel_flip.cpp  # [claude-9b] the weapon in the left hand (cod3_viewmodel_flip, _2 to _4)
    ${COD3_SRC}/game/aim_sensitivity.cpp  # [claude-9b] sensitivity per weapon class, per player
    ${COD3_SRC}/game/sticky_aim.cpp  # [claude-9b] Sticky Aim at a fixed 60 Hz
    ${COD3_SRC}/game/watchdog.cpp
    ${COD3_SRC}/game/mp/campaign_only.cpp
    ${COD3_SRC}/game/mp/campaign_stubs.cpp
    ${COD3_SRC}/game/mp/controls_mp.cpp
    ${COD3_SRC}/game/mp/hud_mp.cpp
    ${COD3_SRC}/game/mp/menus_mp.cpp
    ${COD3_SRC}/game/mp/options_mp.cpp  # [mp-menus] FIELD OF VIEW and FOV SCALE on Options > Gameplay; [menus-categories] Pause > OPTIONS as category pages
    ${COD3_SRC}/game/mp/session.cpp
    ${COD3_SRC}/game/mp/test_autojoin_mp.cpp  # [claude-9b] tests: each local player joins a team and picks a class (cod3_test_autojoin)
    ${COD3_SRC}/game/mp/pose_debug_mp.cpp  # [claude-9b] rival players' poses between draws, measurement (cod3_debug_pose_mp)
    ${COD3_SRC}/game/mp/server_step_mp.cpp  # [claude-9b] the host's game in fixed updates (cod3mp_server_hz, off until tested)
    ${COD3_SRC}/render/rust_renderer.cpp  # [rust-renderer] the Rust renderer's library, checked at start
    ${COD3_SRC}/test_input.cpp
    ${COD3_SRC}/ui/announcement_feed.cpp
    ${COD3_SRC}/ui/console_pages.cpp
    ${COD3_SRC}/ui/console_view.cpp
    ${COD3_SRC}/ui/dvar_domain.cpp
    ${COD3_SRC}/ui/dvar_help.cpp
    ${COD3_SRC}/ui/game_console.cpp
    ${COD3_SRC}/ui/hud_overlay.cpp
    ${COD3_SRC}/ui/problem_log.cpp
    ${COD3_SRC}/ui/theme.cpp
    ${COD3_SRC}/ui/waw_dvars.cpp
)

if(WIN32)
    add_executable(cod3mp WIN32 ${COD3MP_SOURCES})
else()
    add_executable(cod3mp ${COD3MP_SOURCES})
endif()

# Both programs in one folder: each starts the other (game/relaunch.h), and
# the setup tool packs them together.
set_target_properties(cod3mp PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# COD3_MP: shared files pick this program's generated headers and values
# (game/program.h, game/addresses.h). The alias header goes into this
# target's own sources only, never into the generated code (cod3mp_recomp),
# where it would rename this program's functions.
target_compile_definitions(cod3mp PRIVATE COD3_MP=1)
target_include_directories(cod3mp PRIVATE ${COD3_SRC})
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(cod3mp PRIVATE "/FI${COD3_SRC}/game/mp/mp_aliases.h")
else()
    target_compile_options(cod3mp PRIVATE "-include${COD3_SRC}/game/mp/mp_aliases.h")
endif()

rexglue_setup_target(cod3mp GPU_PLUGINS xenos)
cod3_render_link(cod3mp)  # [rust-renderer] made by the top folder's cmake/cod3_render.cmake

if(MINGW)
    target_link_options(cod3mp PRIVATE -municode)
endif()

if(WIN32)
    # winmm: timeBeginPeriod; shell32: CommandLineToArgvW (game/relaunch.cpp).
    target_link_libraries(cod3mp PRIVATE winmm shell32)
endif()

# Linux: librexruntime.so next to the executable, as for cod3.
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET cod3mp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:rex::runtime> $<TARGET_FILE_DIR:cod3mp>
        VERBATIM)
    if(TARGET rex::amd_fidelityfx_vk)
        add_custom_command(TARGET cod3mp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rex::amd_fidelityfx_vk> $<TARGET_FILE_DIR:cod3mp>
            VERBATIM)
    endif()
endif()
