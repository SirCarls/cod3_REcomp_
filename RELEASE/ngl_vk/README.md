# NGL on Vulkan

Treyarch's NGL graphics library rewritten on Vulkan: the renderer Call of Duty 2: Big Red One (original Xbox) draws
everything with, and the one Call of Duty 3 (Xbox 360) is moving to. Both games draw through the same library: the
same material classes (`cdWorld`, `cdChar`, `cdSimpleAlpha`, ...), the same per-draw steps (render states, textures,
shader constants, one mesh draw) and mesh data that is plain vertex and index buffers. A renderer written at this
level replaces the console's GPU instead of emulating it.

**The full guide is [`docs/NATIVE_RENDERER.md`](../docs/NATIVE_RENDERER.md):** how a frame is drawn, the shading
model, building on Linux and Windows, adding a feature, and checking the result against the game.

## What it does

- **2D:** quads and text in the game's screen coordinates, blended in the order given; a whole picture (movies).
- **3D:** meshes kept on the GPU, each draw with its matrices, textures (up to three, mixed in several ways), lightmap
  or lights, fog, alpha test or blending, normal and specular maps, environment maps.
- **The sun's shadow** (a 2048 x 2048 depth map), Call of Duty 3's **grass** (a translation of the game's own grass
  shaders), **depth of field** and the menus' blur, a **tint**, **brightness** and **motion blur**.
- **A window** through any window system the program hands it (both games use SDL3), or offscreen; the last picture
  can always be read back.

## Files

| Path | What it is |
|---|---|
| `include/ngl_vk.h` | the whole interface, every field explained |
| `src/ngl_vk.c` | the implementation |
| `shaders/` | GLSL, turned into SPIR-V when the library is built (nothing compiles shaders while a game runs) |
| `CMakeLists.txt` | the static library; a game adds it with `add_subdirectory` |

Needs the Vulkan headers and loader, and `glslc`. The game-specific code lives with each game: `../bro/src/ngl/` for
Big Red One, `COD_3_WAW_Setup-tool/recomp/src/game/ngl_side.cpp` for Call of Duty 3.
