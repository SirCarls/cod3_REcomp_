# The native renderer: NGL on Vulkan

Both Call of Duty 2: Big Red One (original Xbox) and Call of Duty 3 (Xbox 360) draw through Treyarch's graphics
library, NGL. The native renderer replaces NGL itself with a library of our own on Vulkan, `ngl_vk`. Nothing in it
emulates a console GPU: the game's draw lists are read at NGL's level (meshes, materials, textures, lights, quads,
text) and drawn with Vulkan directly.

This page is for people who want to use the renderer, change it, or carry it to another game. It covers where the
code is, how to build it on Linux and Windows, how a frame is drawn, the shading model, how to add a feature, and how
to check your work against the game.

| Game | How it uses ngl_vk | Platforms |
|---|---|---|
| Big Red One | the only renderer: NGL's functions are replaced by our C versions (`bro/src/ngl/`) | Linux, Windows |
| Call of Duty 3 (campaign and cod3mp) | the GPU emulation still draws the game by default. The game's NGL lists are read from its memory and drawn natively beside it as a check (`game/ngl_side.cpp`, setting `cod3_debug_ngl_side`), or instead of it, in development (`cod3_native_vulkan`) | Linux (Windows: see "Building") |

## Where the code is

| Path | What it is |
|---|---|
| `ngl_vk/include/ngl_vk.h` | the library's whole interface, with every field explained. Start here. |
| `ngl_vk/src/ngl_vk.c` | the implementation: Vulkan device, the target picture, the window's swapchain, textures, meshes, the draw passes and post effects |
| `ngl_vk/shaders/` | GLSL shaders. `glslc` turns each into SPIR-V words in a C array when the library is built; nothing is compiled while a game runs |
| `ngl_vk/CMakeLists.txt` | the library as a static target; a game adds it with `add_subdirectory` |
| `bro/src/ngl/` | Big Red One's NGL written on ngl_vk: `ngl_present.c` (frames, the window, SDL3 events, quitting), `ngl_mesh.c` (mesh files, sections, skinning, lights), `ngl_quad.c` / `ngl_quad_batch.c` (2D), `ngl_font.c` (text), `ngl_texture.c` / `ngl_texture_load.c` (textures), `ngl_scene.c` (scenes and cameras), `ngl_frame_clock.c` (frame pacing) |
| `COD_3_WAW_Setup-tool/recomp/src/game/ngl_side.cpp` | Call of Duty 3: reads NGL's scene lists from the game's memory at the start of `nglListSend` and draws them with ngl_vk. Its header comment lists every structure offset it reads |

The shaders, one pair per kind of draw:

| Shader | Draws |
|---|---|
| `quad.vert` / `quad.frag` | a whole picture (movies, the presenter's first job) |
| `quad2d.vert` / `quad2d.frag` | 2D quads and text in the game's screen coordinates |
| `mesh.vert` / `mesh.frag` | every 3D mesh: lit, lightmapped, mixed, shiny, bumped, fogged, shadowed |
| `shadow.frag` | the sun's shadow map (depth only) |
| `grass.vert` / `grass.frag` | Call of Duty 3's grass, a translation of the game's own grass shaders |
| `dof_blur.frag` / `dof_mix.frag` | depth of field and the menus' blur |
| `tint.frag` | the color tint, brightness and motion blur post effects |

## Building

The library needs the Vulkan headers and loader, and `glslc` (from shaderc or the Vulkan SDK). A game's CMake adds it:

```cmake
find_package(Vulkan)
add_subdirectory(path/to/ngl_vk ${CMAKE_BINARY_DIR}/ngl_vk)
target_link_libraries(your_game PRIVATE ngl_vk)
```

Big Red One also uses SDL3 for its window, sound and controllers: the static SDL3 the rexglue SDK builds for Call of
Duty 3, so both games use one SDL. Pass its headers and library to Big Red One's CMake with `-DBRO_SDL3_INCLUDE_DIR`
and `-DBRO_SDL3_LIBRARY`.

Without `-DBRO_SDL3_*`, Big Red One's CMake takes an SDL3 that `find_package(SDL3)` finds; with no SDL3 at all it
still builds, but draws offscreen, with no sound out and no pads.

### Linux

The library needs only the Vulkan headers, loader and `glslc` from your distribution.

### Windows

Windows builds are cross-compiled on Linux with llvm-mingw, the toolchain the setup tool already uses for Call of
Duty 3's Windows build. Three things are needed:

1. **Vulkan headers:** the rexglue SDK's `thirdparty/vulkan-headers/include`.
2. **An import library for vulkan-1.dll**, made from the SDK's loader definition file:
   ```
   x86_64-w64-mingw32-dlltool -d thirdparty/vulkan-loader/loader/vulkan-1.def -D vulkan-1.dll -l libvulkan-1.a
   ```
   At run time `vulkan-1.dll` comes from the GPU driver (Wine provides its own).
3. **glslc on the build machine:** CMake must find host programs while cross-compiling
   (`CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER` in the toolchain file).

Then give CMake the three paths:

```
-DVulkan_INCLUDE_DIR=<headers> -DVulkan_LIBRARY=<libvulkan-1.a> -DVulkan_GLSLC_EXECUTABLE=<glslc>
```

**The SDK's Windows SDL3 has no Vulkan support** (the SDK's `thirdparty/CMakeLists.txt` turns `SDL_VULKAN` off for
MinGW). On Windows, `bro/src/ngl/ngl_present.c` therefore opens a plain SDL window and makes the Vulkan surface itself
from the window's handle (`vkCreateWin32SurfaceKHR` with `SDL_PROP_WINDOW_WIN32_HWND_POINTER`, and the instance
extensions `VK_KHR_surface` and `VK_KHR_win32_surface`). Any other program using that SDL3 on Windows must do the
same.

Big Red One's Windows build is cross-compiled by hand this way for now; the setup tool builds it for Linux. Call of
Duty 3's CMake leaves ngl_vk out of Windows builds for now (`recomp/CMakeLists.txt`); the steps above are what it
needs to include it.

## How a frame is drawn

1. **Start:** optionally `ngl_vk_set_window(...)` with the window's instance extensions and a function that makes its
   surface; then `ngl_vk_init(width, height)` picks the best GPU and makes the target picture (and the swapchain, with a
   window). Without a window, or when it can't be used, everything is drawn offscreen and can still be read back.
2. **Textures:** `ngl_vk_texture` or `ngl_vk_texture_levels` take linear BGRA8 pixels (decode the console's swizzled,
   tiled or compressed textures first; both games' code shows how). Each returns a number used by draws.
3. **Meshes:** `ngl_vk_mesh` keeps a mesh's corners and 16-bit indices for good. A corner is 11 floats: position x y z,
   texture u v, lightmap u v, a BGRA color in the eighth float's place, normal x y z (0 0 0: use the face's).
   Skinned meshes move their corners on the CPU each frame and hand them over with `ngl_vk_frame_corners`.
4. **Each frame:** fill an array of `struct ngl_vk_mesh_draw` and give it to `ngl_vk_set_meshes`. Optionally set this
   frame's sun shadow (`ngl_vk_set_sun_shadow`), depth of field (`ngl_vk_set_depth_of_field`), tint, brightness and
   motion blur (`ngl_vk_set_tint`, `_brightness`, `_motion_blur`) and grass (`ngl_vk_set_grass`). Then
   `ngl_vk_draw_quads` draws the whole frame in this order:
   - the shadow casters into a 2048 x 2048 depth map;
   - opaque and alpha-tested meshes;
   - grass;
   - blended meshes, in the order given;
   - depth of field, then tint, brightness and motion blur;
   - the 2D quads and text on top;
   - then the picture is shown in the window and copied out.
5. **Reading back:** `ngl_vk_target_pixels` returns the last finished picture (RGBA), for screenshots and comparisons.

Each `ngl_vk_draw_quads` waits for the GPU to finish before it returns. That keeps the code simple (one set of
buffers, nothing in flight); keeping frames in flight is a later step, and the place to start if the CPU waits on the
GPU too long.

## A mesh draw

Every field is explained in `ngl_vk.h`. A zero means "as before" for every optional field, so a game that doesn't
know a field leaves it zero and keeps its look. In groups:

| Group | Fields |
|---|---|
| what to draw | `mesh`, `corners` (this frame's skinned corners, or 0), `index_count` (0: all) |
| where | `world`, `view_projection` (Direct3D row-vector matrices; clip-space z 0..1) |
| surface | `texture`, `tint` (the color without a texture), `alpha` (opaque, tested, blended), `blend` (alpha, add, subtract) |
| light | `shading` (below), `lightmap`, `light_once`, `lightmap_scale` / `lightmap_up`, `lit`, `ambient`, `light_direction` / `light_color` and up to three `extra_light_*` |
| more textures | `blend_texture` / `blend_by_alpha` (two textures mixed), `blend_light` (times a third), `env_texture` / `env_mask` (shiny), `normal_map` / `specular_map` (per-pixel lit) |
| fog | `fog` (start, end, amount at the end), `fog_color` |
| shadows and depth | `shadow_caster`, `sun_shadowed`, `soft_depth` (soft particles) |

## The shading model

`mesh.frag` decides a pixel's light from the draw's **shading**, then its **mode bits**, then its **flags**.

**Shading** (`NGL_VK_SHADE_*`):

| Shading | Light |
|---|---|
| `LIGHT` | without `lit`: one fixed light from above (0.35 + 0.65 x how squarely the face meets it). With `lit`: the ambient plus up to four directional lights (normal against direction, at least 0, times color; the first one is the sun, dimmed by its shadow), optionally held to 1 as Call of Duty 3's vertex shaders saturate it |
| `LIGHTMAP` | the lightmap at the corners' second coordinates: doubled (Big Red One's world) or once (`light_once`); or, with `lit` and `lightmap_scale` above 0, Call of Duty 3's world: the lightmap x (1 + `lightmap_up` x how much the normal faces up) plus the first light (the sun) x the lightmap's alpha (how much of the sun reaches there) x the normal against the sun, all times `lightmap_scale` |
| `VERTEX_COLOR` | the corners' colors, doubled (or once) |
| `UNLIT` | the corners' colors as they are (sky, background); not fogged |

**Mode bits** (`MESH_MODE_*` in `ngl_vk.c`, `MODE_*` in `mesh.frag`; set by `ngl_vk.c` from the draw's fields):
`MIX` two textures mixed, `THIRD` times a third texture, `SHINY` plus an environment map, `MASKED` that map times a
mask, `BY_ALPHA` the mix also by the second texture's alpha, `BUMPED` a normal map bends the normal, `SPECULAR` a
highlight times a specular map, `LIGHTMAP_SUN` Call of Duty 3's lightmapped world (above).

**Flags** (`PUSH_*`): `SCENE_LIGHT` the draw's own lights, `MIRRORED` a mirrored world matrix, `MORE_LIGHTS` how many
extra lights, `HELD` light held to 1, `SHADOWED` the first light dimmed by the sun's shadow map, `SOFT` faded near the
opaque meshes behind it.

A mixed draw (`MIX`) takes its light from its textures (light 1). Before lighting, the alpha test drops pixels whose
alpha is one half or less. After it: the sun's shadow (lightmapped and mixed draws marked shadowed are darkened by half
where the shadow falls; a `LIGHTMAP_SUN` draw takes it in its sun's part instead), the third texture, soft fading,
texture times light plus shine held to 1, then fog (everything but `UNLIT`).

**How the values reach the shader.** Each mesh draw is one indexed draw call:

- the **instance number** carries the tint (27 bits), whether the lightmap or colors multiply once, the shading
  (2 bits), whether the texture is sampled, and the alpha test;
- the **push constants** (32 words, `push_mesh_draw` in `ngl_vk.c`) carry the combined matrix, the fog, the ambient,
  the lights packed as 10-bit numbers, and the flags with the mode bits in bits 8-15;
- the textures are descriptor sets: set 0 the base texture; set 1 the lightmap, second texture, environment map or
  normal map; set 2 the third texture, mask or specular map; set 3 the sun's shadow map.

## Adding a feature

Worked example, from 2026-11-07: Call of Duty 3's world was too bright at night and too dark by day natively.

1. **Find out what the game does, from its own shaders.** Run with the SDK's setting `dump_shaders=<folder>`: every
   shader the GPU emulation translates is written there, its microcode disassembled (`shader_<hash>.ucode.frag`). The
   side check saves the shaders it meets at the game's draws (`ngl-side-draw<N>-ps-header.bin` and `-ps-block.bin`);
   `tools/ps_names.py <that folder> <the dump folder>` matches each one to the dump and lists the names the game's
   compiler left in its constant table (`LightMap_Sampler s1; __80_COD_SHADOW_PROJCOLOR_cmn c18; ...`). With the names,
   the disassembly reads as a formula; the literal constants (c248-c255: the block's first bytes, the last vector
   listed being c255) fill in the numbers. For the world it gave: lightmap x (1 + 0.2 x the normal's up part) + the
   sun's color x the lightmap's alpha x N.L, plus two nearby lights and the specular maps. **A lesson from the same
   day:** guessing from statistics first (the lightmaps' alpha follows the level's light, so color x alpha x a scale)
   cost a build and a test, and was wrong: the alpha is the baked sun shadow. Read the shader before guessing.
2. **Add fields to `struct ngl_vk_mesh_draw`** whose zero keeps today's behavior (`lightmap_scale`, `lightmap_up`). The
   other game then needs no change.
3. **Add a mode bit** in `ngl_vk.c` (`MESH_MODE_LIGHTMAP_SUN`, set in the draw loop from the fields) and the same
   value in `mesh.frag` (`MODE_LIGHTMAP_SUN`). Bits 8-15 of the flags hold up to eight modes; all are now in use, so
   the next mode needs another word or a different encoding.
4. **Get the values to the shader.** Use push-constant slots the draw doesn't otherwise use: here the scale and the up
   share ride in the ambient's place, which a lightmapped draw doesn't read, and the world's up (turned into the mesh's
   space like the lights) in the second light's place; or add one if the 32 words allow.
5. **Set them from the game's code** (`ngl_side.cpp`'s `WorldLight`: the sun from the draw's shader state, the
   lightmap's fetch exponent as the scale), behind a setting while it is being checked
   (`cod3_debug_ngl_side_world_light`).
6. **Check it** against the game (below), in the levels that showed the problem and in one that didn't. Here Falaise's
   night went from 30-50% too bright to within about 10%.
7. **Build both games**, including Big Red One's Windows build: `ngl_vk` is shared.

Rules that keep both games working: new fields default to the old look at zero; shaders compile at build time, so a
shader error is a build error; don't read game structures inside `ngl_vk` (that belongs in each game's code).

## Checking against the game

### Big Red One

Settings are environment variables (`bro/README.md` lists them all). For the renderer:

| Variable | Effect |
|---|---|
| `BRO_VK=1` | draw with Vulkan |
| `BRO_WINDOW=0` | no window even with a display (offscreen) |
| `BRO_VK_SIZE=<w>x<h>` | the target's size |
| `BRO_VK_DUMP=<frame>[,<frame>...]`, `BRO_VK_DUMP_PATH=<file.ppm>` | save the target at those frames (`%lu` in the name becomes the frame number) |
| `BRO_VK_PLACEHOLDERS=1` | quads whose texture has no pixels yet drawn as faint boxes |
| `BRO_NGL_LOG=0/1/2` | log the game's NGL calls |

### Call of Duty 3

The side check runs inside a normal game with the GPU emulation drawing what you see. Settings (`--name=value` on
the command line, or in the game's settings file):

| Setting | Effect |
|---|---|
| `cod3_debug_ngl_side=true` | every 10 s for the first 80 s (frames 600 to 4800), draw the frame natively too and save both pictures |
| `cod3_ngl_side_live=true` | draw every frame natively too (pictures are still saved only every 10 s); the log says how long it takes |
| `cod3_native_vulkan=true` | in development: the GPU emulation is off and every frame is drawn natively in the game's window (takes effect at the next start) |
| `cod3_debug_ngl_side_world_light` | the lightmapped world lit as the game's shaders do (on by default; off: the lightmap doubled, the old way) |
| `cod3_debug_ngl_side_world_shadow` | the lightmapped world takes the native sun shadow in its sun's part (on by default) |
| `cod3_debug_ngl_side_shadow` | the native sun shadow (on by default), cast along the scene's sun (the node shader state's `cod_shadow_position`) |
| `cod3_debug_ngl_side_bumped` | the per-pixel lit models use their normal and specular maps (on by default) |
| `cod3_debug_ngl_side_model_light` | models with their own light set: 0 the set alone, 1 the scene's sun plus the set's lights, 2 the scene's sun in place of the set's first light (default; closest to the game in Saint-Lô) |
| `cod3_debug_skip_color_uv_nodes` | a check: leave the game's own apsColorUVANode draws (small far meshes, campaign Render 0x8210FD10) out of its picture; off by default |
| `cod3_debug_ngl_side_normals` | light models by their vertices' normals (on by default; off: by each triangle's) |
| `cod3_debug_ngl_side_arms` | how the first-person arms are drawn: 0 as the rest, 1 plain white, 2 their texture unlit, 3 plain and lit |
| `cod3_debug_ngl_side_blur` | with `cod3_native_vulkan`, blur the world behind the menus by this much (percent) to check the blur |
| `cod3_debug_ngl_side_world_nearby_lights` | the lightmapped world lit by its object's two nearby point lights too (LIGHT_COLOR x saturate(1 - d / range) x N.L), as the game's shaders do (on by default) |
| `cod3_debug_ngl_side_model_irradiance` | the per-pixel lit models lit by the light the game hands their own draws: its nine irradiance rows (in place of the ambient) and nearest point light. 0 off, 1 every lit model (default), 2 bumped and shiny ones only. Campaign only for now |
| `cod3_debug_ngl_side_water` | the lightmapped world's water and puddles (no color map, an environment and a normal map): 0 left out until their shader is drawn natively (default), 1 drawn as before (opaque, bright) |
| `cod3_debug_ngl_side_clear` | checks: what the native 3D clears to: 0 black (as the game's world scene does), 1 the scene's clear color, 2 its fog color |
| `cod3_debug_ngl_side_sky_drift`, `cod3_debug_ngl_side_sky_base_blended`, `cod3_debug_ngl_side_sky_flat` | checks of the sky layers: their texture drift (on), the base layer blended, layers drawn flat (a mask) |
| `cod3_debug_skip_game_draws`, `cod3_debug_skip_color_uv_nodes` | checks: leave some of the game's own draws out of its picture (off by default) |

Each check frame writes, in `userdata/logs/screenshots/`:

- `ngl-side-<frame>-vk.ppm`: the native picture (3D, then 2D on top);
- `ngl-side-<frame>-3d.ppm`: the native 3D alone, when written;
- `report-<time>.png`: the game's own screenshot of the same moment, named in the log line.

The log says what was drawn and what was left out, and why, frame by frame (search for `ngl side:`).

`tools/ngl_side_compare.py` puts each native picture beside the game's and scores how far apart they are: the mean
difference per channel, the worst region of a 4 x 4 grid and where it is, and each picture's mean brightness. It
compares the whole native picture with the game's, since the game's screenshot has its HUD too (`--3d` for the 3D
alone). It needs Pillow:

```
python3 tools/ngl_side_compare.py <the run's log file> 1800 3000 4200 --out <folder for the side-by-side pictures> [--grid]
```

`--grid` also prints each region's brightness, the game's and the native one's, and both pictures' mean colors: a
region much darker natively points at something not drawn there, the whole grid darker at the light. A difference
under about 10 is close; 20 and up is worth a look; a game picture that is black (a loading screen) makes
any number meaningless, so check the side-by-side picture. Compare right after the run: the next run with the side
check on overwrites the `ngl-side-*` pictures (the game's screenshots are named by the time, so they stay), and the
tool warns when the two pictures it pairs were saved far apart.

## Known gaps (2026-11-07, 15:00)

Call of Duty 3, natively:

- **the sky is about 1.4x darker on every level** (the top of the picture: Island x1.43, Saint-Lô x1.31, Falaise
  x1.3-1.5, Black Burn's far middle x1.6; the same in red, green and blue; the ground matches). Ruled out so far:
  the sky's shaders (texture x corner color, as drawn), the textures' fetch (no gamma, no exponent), a sky pass
  merged away (each layer is drawn once), the fog, the clear color, a gamma curve, the tint. Still open;
- rain; and island's water and puddles, left out for now (`cod3_debug_ngl_side_water`): their shader
  (99861CE203379D5B: two scrolling ripple normal maps, the environment by a Fresnel term, a color and an opacity,
  softened by the depth buffer, blended) isn't drawn natively yet;
- the terrain's second layer (DiffuseMapB / NormalMapB);
- on the lightmapped world: the specular maps;
- the guns' sheen, the trucks' glass reflections, Mayenne's haze, one kind of glow;
- depth of field when aiming down the sights;
- cod3mp: the models' irradiance (it needs cod3mp's addresses for the pass and the draw);
- `cod3_native_vulkan` is not yet the default: the emulation still draws what the player sees.

The running record of what was found and tried is in the project's development notes. This page should be updated
whenever the interface or the shading model changes.
