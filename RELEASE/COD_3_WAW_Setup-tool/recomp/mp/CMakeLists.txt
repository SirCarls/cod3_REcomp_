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
    ${COD3_SRC}/game/use_hold.cpp  # [acct2-decomp] how long Use is held, and the hints' words (cod3_use_hold_ms)
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
    ${COD3_SRC}/game/fx_timing.cpp  # [acct2-cod3-fx] the effects frame by frame (cod3_debug_fx_frames), smoke flicker
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
    ${COD3_SRC}/game/keybinds/pad_model.cpp  # [acct2-main] the controller's own binds per player (split screen)
    ${COD3_SRC}/game/keybinds/pad_binds.cpp  # [acct2-main]
    ${COD3_SRC}/game/level_hooks.cpp
    ${COD3_SRC}/game/machine_profile.cpp
    ${COD3_SRC}/game/mouse_look.cpp
    ${COD3_SRC}/f2/f2_accel.cpp  # [f2-menus] 2026-11-11 mouse acceleration, sensitivity randomizer and scales (the ports' shared file)
    ${COD3_SRC}/game/pc_menu.cpp  # [mp-menus] mouse and keyboard in the game's menus, shared with the campaign
    ${COD3_SRC}/game/perf_stats.cpp
    ${COD3_SRC}/game/post_effects.cpp  # [post-effects] depth of field and motion blur on or off, in both programs
    ${COD3_SRC}/game/menu_video_rows.cpp  # [menus-categories] the video rows of Pause > OPTIONS > VIDEO
    ${COD3_SRC}/game/options_pages.cpp  # [menus-categories] which rows each Options category page has
    ${COD3_SRC}/game/print_hooks.cpp
    ${COD3_SRC}/game/relaunch.cpp
    ${COD3_SRC}/game/rumble.cpp  # [rumble] the first controller doesn't vibrate while the keyboard and mouse are in use
    ${COD3_SRC}/game/render_resolution.cpp
    ${COD3_SRC}/game/list_memory.cpp  # [claude-9b-think] the render lists' frame memory: room past its end, measured; scene nodes checked (split screen's 2-4 views share one frame's lists)
    ${COD3_SRC}/game/native_memory.cpp  # [memory-360] 2026-11-11 the game's memory past the Xbox 360's limits: a report (cod3_native_memory_report) and more streaming banks (cod3_native_stream_banks_extra), both off; both programs
    ${COD3_SRC}/game/native_memory_shadow.cpp  # [memory-360] 2026-11-11 stage 3's test: which arena-0 allocations reach the graphics side (cod3_native_memory_shadow, off; moves nothing); both programs
    ${COD3_SRC}/game/ngl_census.cpp  # [acct2-bro] cod3_debug_ngl_census: what NGL draws a frame (logging only)
    ${COD3_SRC}/game/ngl_side.cpp  # [acct2-bro] cod3_debug_ngl_side: the frame drawn by ngl_vk beside the GPU emulation (a check)
    ${COD3_SRC}/game/screenshot.cpp
    ${COD3_SRC}/game/script_fiber.cpp
    ${COD3_SRC}/game/script_threads.cpp
    ${COD3_SRC}/game/settings_sync.cpp
    ${COD3_SRC}/game/sound_stall.cpp  # [voice] voices played twice: the sound library's stuck-sound check, in both programs
    ${COD3_SRC}/game/shadows.cpp
    ${COD3_SRC}/game/speedtree_fix.cpp  # [speedtree] the trees' buffers released once, and only those made
    ${COD3_SRC}/game/thread_profiler.cpp
    ${COD3_SRC}/game/ultrawide.cpp
    ${COD3_SRC}/game/voice_language.cpp  # [acct2-ocr] Voices from another disc (mp/ banks), heard from the next map
    ${COD3_SRC}/game/text_language.cpp  # [acct2-ocr] the game's text in another language, switchable (its own hook of cod3mp's lookup)
    ${COD3_SRC}/game/font_fill.cpp  # [acct2-ocr] the glyphs the game's fonts lack, added in memory
    ${COD3_SRC}/ui/text_overlay.cpp  # [acct2-ocr] Russian/Chinese/Japanese/Korean text, drawn by us
    ${COD3_SRC}/ui/text_native.cpp  # [acct2-ocr] the same text drawn through the game's own text path
    ${COD3_SRC}/game/color_text.cpp  # [acct2-ocr] color codes inside a line (names)
    ${COD3_SRC}/game/viewmodel.cpp  # [claude-9b] weapon offsets with Counter-Strike 2's commands
    ${COD3_SRC}/game/viewmodel_flip.cpp  # [claude-9b] the weapon in the left hand (cod3_viewmodel_flip, _2 to _4)
    ${COD3_SRC}/game/aim_sensitivity.cpp  # [claude-9b] sensitivity per weapon class, per player
    ${COD3_SRC}/game/pad_settings.cpp  # [acct2-ocr] 2026-11-07 the controller settings as console settings
    ${COD3_SRC}/game/menu_controller.cpp  # [acct2-ocr] 2026-11-07 Pause > OPTIONS > CONTROLLER's port rows
    ${COD3_SRC}/game/menu_port_rows.cpp  # [acct2-ocr] 2026-11-07 the port's settings rows of Pause > OPTIONS
    ${COD3_SRC}/game/sticky_aim.cpp  # [claude-9b] Sticky Aim at a fixed 60 Hz
    ${COD3_SRC}/game/movement/rules.cpp  # [claude-9b-think] the movement rules' plain logic (stage 1: the match decision)
    ${COD3_SRC}/game/movement/hooks_mp.cpp  # [claude-9b-think] stage 1: movement settings, decision and logs; moves nothing
    ${COD3_SRC}/game/movement/touch_pace_mp.cpp  # [acct2-ocr] the touches after a move, 60 a game second per player
    ${COD3_SRC}/game/movement/levitate_probe_mp.cpp  # [acct2-ocr] dev: the levitation glitch probe (cod3_debug_levitate, off)
    ${COD3_SRC}/game/movement/stuck_fix_mp.cpp  # [acct2-ocr] the levitation fix: the stuck fix frees players from walls only (levitation_fix)
    ${COD3_SRC}/game/protected_dvars.cpp  # [acct2-bro] for stuck_fix_mp.cpp's levitation_fix rule (only the campaign built it)
    ${COD3_SRC}/game/watchdog.cpp
    ${COD3_SRC}/game/mp/campaign_only.cpp
    ${COD3_SRC}/game/mp/campaign_stubs.cpp
    ${COD3_SRC}/game/mp/controls_mp.cpp
    ${COD3_SRC}/game/mp/hud_mp.cpp
    ${COD3_SRC}/game/mp/menus_mp.cpp
    ${COD3_SRC}/game/mp/options_mp.cpp  # [mp-menus] FIELD OF VIEW and FOV SCALE on Options > Gameplay; [menus-categories] Pause > OPTIONS as category pages
    ${COD3_SRC}/game/mp/session.cpp
    ${COD3_SRC}/game/mp/test_autojoin_mp.cpp  # [claude-9b] tests: each local player joins a team and picks a class (cod3_test_autojoin)
    ${COD3_SRC}/game/mp/test_syslink_mp.cpp  # [claude-9b-think] tests: host or join a system link match from the front end (cod3_test_syslink)
    ${COD3_SRC}/game/mp/pose_debug_mp.cpp  # [claude-9b] rival players' poses between draws, measurement (cod3_debug_pose_mp)
    ${COD3_SRC}/game/mp/server_step_mp.cpp  # [claude-9b] the host's game in fixed updates (cod3mp_server_hz, off until tested)
    ${COD3_SRC}/game/mp/motion_steps_mp.cpp  # [acct2-cod3-motion] how drawn things and the camera move frame to frame, measurement (cod3mp_debug_entity_steps)
    ${COD3_SRC}/game/mp/title_update_hq_mp.cpp  # [rexglue-native] title update 3's Headquarters radio fix, redone on the disc code: cod3_tu_hq_radio
    ${COD3_SRC}/render/rust_renderer.cpp  # [rust-renderer] the Rust renderer's library, checked at start
    ${COD3_SRC}/render/native_compare.cpp  # [rexglue-native] 2026-11-09 ngl_side.cpp's live compare (cod3mp doesn't attach it yet: off)
    ${COD3_SRC}/test_input.cpp
    ${COD3_SRC}/ui/announcement_feed.cpp
    ${COD3_SRC}/ui/console_pages.cpp
    ${COD3_SRC}/ui/console_view.cpp
    ${COD3_SRC}/ui/dvar_domain.cpp
    ${COD3_SRC}/ui/dvar_help.cpp
    ${COD3_SRC}/ui/game_console.cpp
    ${COD3_SRC}/ui/f2_tour.cpp  # [rexglue-native-f2] 2026-11-11 f2t1 the F2 window's test-only tour (cod3_debug_f2_tour, off)
    ${COD3_SRC}/ui/hud_overlay.cpp
    ${COD3_SRC}/ui/problem_log.cpp
    ${COD3_SRC}/ui/server_tab.cpp  # [acct2-ocr] F2 > Game > Server in cod3mp (matches this PC plays alone)
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

# [acct2-bro] ngl_vk, the native renderer's Vulkan library, when the top folder found it (cod3_debug_ngl_side; without
# it the check only counts).
if(TARGET ngl_vk)
    target_link_libraries(cod3mp PRIVATE ngl_vk)
    target_compile_definitions(cod3mp PRIVATE COD3_NGL_VK=1)
    if(COD3_RENDERDOC_INCLUDE_DIR)  # the SDK's renderdoc_app.h, found by the top folder (ngl_side.cpp needs it)
        target_include_directories(cod3mp PRIVATE "${COD3_RENDERDOC_INCLUDE_DIR}")
    endif()
endif()

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
