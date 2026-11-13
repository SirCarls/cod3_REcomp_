# The F2 menu (settings window)

[acct2-ocr] 2026-11-07 (owner 22:58: "Also document f2 menu while you're at it"). F2 opens the port's settings
window over the game, in the campaign (`cod3`) and in multiplayer (`cod3mp`); F2 or the window's close button
shuts it. The code is `recomp/src/ui/hud_overlay.cpp` (`SettingsWindow`), plus the sections other files draw
(named below). Every row is a setting saved in `cod3.toml` (multiplayer: `cod3mp.toml`) or a game dvar saved in
the console config (`cod3_config.cfg`); the ~ console takes each by the name in the "setting" column
(docs/DVARS.md has them all).

How the window works:

- **Save** (bottom row) writes the settings file; it shows `Save *` while something is unsaved. Closing F2
  saves too. Some rows save at once (the sound output, voices, text language, the dead zone and the pause
  menu's rows). "Restart the game to apply some changes" appears when a row only applies at the next start.
- **Reset** (top right of most pages) puts that page's settings back to their defaults.
- Pointing at a row shows its tooltip; the faint lines under rows explain them.
- **Per player:** rows marked *per player* have their own value for each split-screen player (cod3mp); a
  player picker or one row per player chooses whose. Owner rule 2026-11-07: every setting a player picks for
  themselves is per player (BOARD.md, at the top of the claims).
- Programs: *SP* the campaign only, *MP* multiplayer only; unmarked rows are in both.

## VIDEO

Reset puts every video page back (resolution, display mode, frame rate limit, colors, quality).

### Display

| row | what it does | setting |
|---|---|---|
| Graphics API | Vulkan (the automatic choice) or Direct3D 12 (Windows). Applies after a restart. | `cod3_graphics_api` |
| Display mode | Window, or borderless fullscreen. At once. | `fullscreen` |
| Display | Which monitor, kept by its name. At once. | `monitor_name` |
| Refresh rate (fullscreen) | The monitor's refresh rate in fullscreen; "Desktop's own" keeps the desktop's. | `cod3_refresh_rate` |
| V-Sync (no tearing) | Shows each picture on a new refresh. Not a frame cap. After a restart. | `vulkan_allow_present_mode_immediate` / `d3d12_allow_variable_refresh_rate_and_tearing` |
| Black bars instead of stretching | Keeps the picture's shape when the window has another shape. | `present_letterbox` |

### Resolution

| row | what it does | setting |
|---|---|---|
| Render resolution | How many pixels the game draws: native (the game's own up to 1920x1080), match the screen, or a size; a custom size can be typed ("Use this size"). | `cod3_render_resolution` |
| How it is drawn | The method used for the render resolution. | `cod3_render_method` |
| Resolution scale | Draws at a percentage of the resolution. | `cod3_render_percent` |
| Change the resolution without restarting | On: a new resolution applies between two frames. | `cod3_live_resolution` |
| Aspect ratio | The picture's shape (custom shapes can be typed: "Use this shape"); the view up and down stays the game's. | `cod3_aspect_ratio` |

### Frame rate

| row | what it does | setting |
|---|---|---|
| Frame rate limit (com_maxfps) | The most frames a second (a cap, not a target). At once. | `com_maxfps` (`cod3_max_fps`) |
| Show FPS (cg_drawFPS) | A frame rate counter top right: off, rate, range, frame times. | `cg_drawFPS` |

### Colors

| row | what it does | setting |
|---|---|---|
| Picture colors | Matched to your display, or the Xbox 360 on an HDTV. At once. | `cod3_color_space` |
| HDR output (HDR10, BT.2020) | For an HDR display (Vulkan); the line under it says what the screen shows now. | `display_hdr` |
| White level, highlight level | How bright white and the brightest highlights are, in nits. | `display_hdr_paper_white`, `display_hdr_peak` |

### Quality

| row | what it does | setting |
|---|---|---|
| Post-process AA | Smooths edges after the picture is drawn (FXAA, Extreme). After a restart. | `swap_post_effect`, `fxaa_extreme` |
| Output scaling | How the picture is fitted to the window: bilinear, AMD CAS, AMD FSR 1. | `present_effect` |
| FSR sharpness, CAS sharpness | How much each sharpens. After a restart. | `present_fsr_sharpness_reduction`, `present_cas_additional_sharpness` |
| Anisotropic filtering | Keeps surfaces seen at an angle sharp. | `anisotropic_override` |
| Shadow quality | The shadows moving things cast: original, sharper, off. | `cod3_shadow_quality` |
| Split-screen shadows (*MP*) | Shadow maps per view in split screen: the game's, nearest group, off. | `cod3mp_split_shadows` |
| Depth of field | The blurred background where a map turns it on. At once. | `cod3_depth_of_field` |
| Motion blur | The screen trails at some moments. At once. | `cod3_motion_blur` |
| HD textures (waifu2x) | The game's textures at twice the detail (made by the setup tool). After a restart. | `cod3_hd_textures` |

## SOUND

| row | what it does | setting |
|---|---|---|
| Master volume | Everything the game plays (music, effects and voices have their own sliders in the game's Options > Sound). | `cod3_volume` (`snd_volume`) |
| Sound output | Which device plays the game's sound; saved at once. | `audio_device` |
| Speakers | What the 5.1 mix is for: automatic, stereo, 5.1, 7.1, headphones (`game/` speaker layout, acct2-decomp). | `audio_output_layout` |
| Voices | The language the soldiers speak (`game/voice_language.cpp`); saved at once. | `cod3_voice_language` |
| Text | The language of every text on screen, 17 languages (`game/text_language.cpp`); "Text by kind" picks it for menus, subtitles, objectives, loading screens, prompts, credits and film title cards on their own. Saved at once. | `cod3_text_language`, `cod3_text_menus`, `cod3_text_subtitles`, `cod3_text_objectives`, `cod3_text_loading`, `cod3_text_prompts`, `cod3_text_credits`, `cod3_text_film_cards` |

## GAME

Reset puts back the field of view (65 in the campaign, 80 in multiplayer), aim assist, the weapon position,
the prompts, the cutscene rows and the split-screen players' own fields of view.

### View

| row | what it does | setting |
|---|---|---|
| Field of view (cg_fov) | How wide the view is, in degrees (65-160); also the game's Options > Gameplay. | `cg_fov` |
| FOV scale (cg_fovScale) | Multiplies the field of view, World at War style (ADS too). cg_fov x scale stays within 65-165. | `cg_fovScale` |
| Split screen: players 2 to 4 (*MP*, *per player*) | Each split-screen player's own field of view and FOV scale, on their own view only; off = player 1's. Also each player's Pause > OPTIONS > GAMEPLAY. | `cod3_fov_p2`..`_p4`, `cod3_fov_scale_p2`..`_p4` |
| Vehicle FOV scale | Multiplies the field of view in a vehicle or on a tank's gun. | `cod3_vehicle_fov_scale` |

### Weapon

| row | what it does | setting |
|---|---|---|
| Preset, Forward / Right / Up | Where the weapon sits on screen, Counter-Strike 2's way (aiming down the sights never moves). | `cod3_viewmodel_presetpos`, `cod3_viewmodel_offset_x` / `_y` / `_z` |
| Left-handed weapon (mirrored) | Mirrors the weapon and hands. A key can flip it too. | `cod3_viewmodel_flip` |
| Player 2 to 4: left-handed weapon (*MP*, *per player*) | Each split-screen player's own. | `cod3_viewmodel_flip_2` .. `_4` |

### Prompts

| row | what it does | setting |
|---|---|---|
| Show keys in button prompts | With the keyboard and mouse the prompts show keys ([R]) instead of buttons. | `cod3_key_prompts` |
| Keys as key caps | Keys as key caps, mouse buttons as a small mouse, other pads' buttons as pictures. | `cod3_key_caps` |
| Key cap size | How big the caps and button pictures are. | `cod3_key_cap_scale` |
| Button style | Each controller's own buttons, or one style for everyone. | `cod3_glyph_style` |
| Colors (original, custom, rainbow) and Each button's color | The caps' and buttons' colors. | `cod3_glyph_color_mode` and the color rows |
| Use: The game's hold (250 ms) / Hold Use for | How long Use is held to pick up, man a gun or enter a vehicle; 0 = a press. | `cod3_use_hold_ms` |

### Cutscenes (*SP*)

| row | what it does | setting |
|---|---|---|
| Skip cutscenes automatically | Skips the logos, briefing films and level intros as soon as the game allows. | `cod3_skip_movies` |
| Black bars in in-game cutscenes | The bars during the levels' scripted scenes. | `cod3_letterbox` |
| Show the skip prompt, its corner | "SPACE Skip" while a briefing can be skipped. | `cod3_skip_prompt`, `cod3_skip_prompt_position` |
| Hold to skip | How long the skip key is held; 0 = a press. | `cod3_skip_hold_ms` |

### Aim (*SP*)

| row | what it does | setting |
|---|---|---|
| Aim assist (Sticky Aim) | The game's Sticky Aim (controller only), the same switch as Options > Gameplay; under it what applies now (`game/sticky_aim.cpp`). In multiplayer it is the controller profile's own row, and the match's rule is the host's. | `cod3_aim_assist` |

### Movement (*SP*)

The same rows as the game's Pause > Options > MOVEMENT (`game/movement/settings.cpp`): sprint and its toggle,
the movement style (Call of Duty 3's or Call of Duty 4's movement, speeds, jumps and sprint) and the movement
rate. Settings: `cod3_sprint`, `cod3_sprint_toggle`, `cod3_cod4_movement`, `cod3_cod4_speeds`, `cod3_cod4_jumps`,
`cod3_cod4_sprint`, `cod3_move_hz`, `cod3_move_hz_sync`. docs/MOVEMENT.md has the details. In multiplayer the
movement is the match's rule (the Server tab).

### Server (only while this machine is the host)

Every rule the host sets for the match (`ui/server_tab.cpp`): movement and aim assist sections; changing one
asks to confirm and tells every player; "your own settings don't change". docs/GAMEPLAY_PATCHES.md and
docs/PC_SERVER.md have the rules.

### Player

| row | what it does | setting |
|---|---|---|
| Player name | Your name in the game (up to 15 characters, ^ colors as in World at War); read at the start. | `user_name` |
| Players 2 to 4 (*per player*) | The split-screen players' names in multiplayer (up to 14 characters). | `user_name_2` .. `_4` |
| Controller order | Which player each controller plays as in split screen; "Reset to connection order". The keyboard and mouse are always player 1. | |

## CROSSHAIR

| row | what it does | setting |
|---|---|---|
| Show crosshair (*per player* in MP) | The game's own crosshair switch (Options > Gameplay > Crosshair); in split screen one per player. | the profile's |
| Use custom crosshair | Draws the crosshair below instead of the game's (fades as you aim down the sights). | `cod3_crosshair` |
| Style, size, gap, thickness, outline, center dot | The custom crosshair's shape, in pixels on a 1080p screen. | `cod3_crosshair_style`, `_size`, `_gap`, `_thickness`, `_outline`, `_outline_thickness`, `_dot` |
| Constant color / Color | One color always, or the game's changing colors. | `cod3_crosshair_constant_color`, `cod3_crosshair_color` |
| My own colors for each case | Your colors over nothing, an enemy, a friendly, a dead body. | `cod3_crosshair_own_colors`, `cod3_crosshair_color_normal` / `_enemy` / `_friendly` / `_dead` |

The custom crosshair is one for every split-screen player for now (it belongs to acct2-decomp's crosshair
work; owner rule: to become per player).

## CONTROLS

### Mouse

| row | what it does | setting |
|---|---|---|
| Keyboard & mouse | Play with the keyboard and mouse (a controller keeps working). | `mnk_mode` |
| Mouse aim | Raw input (World at War's), right-stick emulation, or off. | `cod3_raw_mouse`, `mnk_mouse` |
| Sensitivity (WaW scale) | World at War's sensitivity number (default 5). | `cod3_sensitivity` |
| ADS sensitivity multiplier | Extra factor while aiming down the sights. | `cod3_zoom_sensitivity` |
| Invert vertical | The mouse inverted (the game's Invert Aim flips it too). | `cod3_invert_mouse` |
| Game's horizontal / vertical rows scale the mouse too | Off: those rows are the controller's only. | `cod3_mouse_game_sensitivity` |
| Advanced (m_yaw / m_pitch) | Degrees per mouse count (World at War's 0.022). In the console `m_yaw` / `m_pitch` are these. | `cod3_m_yaw`, `cod3_m_pitch` |

The mouse and keyboard are player 1's.

### Controller

| row | what it does | setting |
|---|---|---|
| Player (*MP*) | Whose rows below show. | |
| Highest sensitivity (both rows, every player) | How far the game's Horizontal and Vertical Sensitivity rows go (the game stopped at 37 and 50). | `cod3_pad_sensitivity_max` |
| Horizontal / Vertical sensitivity (*per player*) | The game's own Options > Controls rows; ticked, the value here is that player's row, unticked the page decides. | `cod3_pad_horizontal_sensitivity`, `cod3_pad_vertical_sensitivity` (`_p2`..`_p4`) |
| Invert aim, Vibration (*per player*) | The Options page's, off or on. | `cod3_pad_invert`, `cod3_pad_vibration` (`_p2`..`_p4`) |
| Stick dead zone (joy_threshold) | How far a stick moves before it counts, every controller (25 of 128 is the game's). | `joy_threshold` |
| Own dead zone (*per player*) | A player's own, in place of the shared one. | `cod3_deadzone_p1` .. `_p4` |
| Stick sensitivity | With Mouse aim on right-stick emulation: how far the mouse pushes the stick. | `mnk_sensitivity` |

The stick and button layouts and Sticky Aim are the game's own rows (Options > Controls / Gameplay;
multiplayer: Pause > OPTIONS > CONTROLLER). The game's pause menu has a CONTROLLER page too (sensitivity limit,
dead zone; the campaign also "vibrate with keyboard", `cod3_rumble_with_keys`).

### Per weapon (*per player*)

Sensitivity per weapon group (rifles, shotguns, snipers, SMGs, LMGs, MGs, pistols, launchers, mounted guns,
other), from the hip and aiming, 0-200% (100 = the game's); the weapon in hand is highlighted. Multiplayer has
a player picker. Mouse and controller. `cod3_weapon_sensitivity` (`_p2`..`_p4`, "group=hip/aiming" items;
`game/aim_sensitivity.cpp`). Also the campaign's Pause > Options > AIM.

### Key bindings

The keys for each action and for each controller button (`game/keybinds/`): the walking keys, the actions
(fire, aim, jump, crouch, ... with up to three keys each, toggle or hold for aiming), and a row per controller
button for the menus and for keys no action has. Separate keys with commas; a row applies on Enter, on a click
elsewhere or when F2 closes. Settings: `cod3_binds_actions`, `cod3_binds_kbm_p1`, `cod3_aim_toggle_kbm_p1`,
`keybind_a` .. `keybind_*`. docs/KEYBINDS.md has the details.

## INTERFACE

| row | what it does | setting |
|---|---|---|
| Theme | The window's colors. | `cod3_ui_theme` |
| Text size | The size of this window's text (the game's menus don't change). | `cod3_ui_text_size` |
| Window opacity | Lower it to see the game behind the window. | `cod3_ui_opacity` |
| Show the version number (top right) | The setup tool's version, small and faint. | `cod3_show_version` |

## PERFORMANCE

| row | what it does | setting |
|---|---|---|
| Use low-end PC settings... | Lists the costliest options (Video, EMULATION, this page) and changes them on Apply. | |
| Log detail | How much goes into `userdata/logs/cod3.log`: normal, verbose, debug. | `cod3_log_detail` |
| Write a report | `userdata/logs/report-<date>-<time>.txt` for a bug report (also `cod3_report`). | |
| Take a screenshot | The game's own picture into `userdata/logs/screenshots/` (also `screenshot`). | |
| Compare HD textures (*SP*) | Two screenshots, with and without the HD textures (also `cod3_hd_compare`). | |
| Debug screenshots | With Log detail on Debug: the picture now and then, when stuck and with reports. | `cod3_debug_screenshots` |
| Show what each frame costs | A readout under the frame rate: the game thread, the GPU emulation, what limits the frame rate. | `cod3_perf_overlay`, `cod3_perf_log` |
| Threads and this PC | Each thread's CPU use and waits. | `cod3_threads` |

## EMULATION

How the Xbox 360 is emulated and which parts run as native PC code; the defaults are the tested, fastest
choices. Most apply after a restart.

- **GPU:** the native renderer switch (not ready yet), render target path, "Draw the game's 2x MSAA as real
  2x" (`native_2x_msaa`), resolve readback (`readback_resolve_cpu_reads_only`,
  `readback_resolve_cpu_pages_only`, `readback_resolve_full_deferred`), "Re-send all memory every frame"
  (`clear_memory_page_state`).
- **Speedups:** `vulkan_reuse_draw_state`, `vulkan_skip_unchanged_fetch_constants`, `vulkan_sampler_cache`,
  `vulkan_reuse_texture_descriptors`, `vulkan_dynamic_constant_offsets`, `vulkan_resolve_into_textures`,
  `vulkan_vertex_refetch`, `vulkan_texture_memo`, `texture_replacement_async`; shader threads,
  `async_shader_compilation`, `store_shaders`.
- **Native code:** memory and text (`cod3_native_crt`), math (`cod3_native_math`), text formatting
  (`cod3_native_printf`), numbers from text (`cod3_native_strtol`), level file unpacking (`cod3_native_lzo`);
  waiting: `cod3_job_wait_sleeps`, `cod3_gpu_wait_sleeps`.
- **Sound:** native reverb, mixing and resampling (`cod3_native_fmod`, `cod3_native_fmod_mix`,
  `cod3_native_resampler`), `xma_restart_without_reopen`, `audio_pace_callbacks`.

## CONSOLE

- **Console:** the ~ console in the window (World at War style: completion, history, `condump`).
- **Problems:** warnings and errors since the start.
- **Saved settings:** what the console keeps in `cod3_config.cfg` (seta lines, `com_maxfps`, `cg_drawFPS`, your
  `cg_fov` / `cg_fovScale`); Reset empties it and puts the game's defaults back.

## Keeping this page right

A new F2 row gets a line here (its label, what it does, its setting, *SP* / *MP*, *per player*). The labels
are the ones in `ui/hud_overlay.cpp`; the settings and their ranges are in docs/DVARS.md.
