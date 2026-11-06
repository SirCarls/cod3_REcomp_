# NGL on Vulkan

Treyarch's NGL graphics library rewritten on Vulkan, for Call of Duty 2: Big Red One (original Xbox) first and
Call of Duty 3 after it. Both games draw through the same library: the same material classes (`cdWorld`, `cdChar`,
`cdSimpleAlpha`, ...), the same per-draw steps (render states, textures, shader constants, one mesh draw) and mesh
data that is plain vertex and index buffers. A renderer written at this level replaces the console's GPU instead of
emulating it.

Status: the presenter builds (`src/ngl_vk.c`): it draws one 32-bit picture, scaled to fit, into a picture of its
own that the host can read. Big Red One shows its movie frames through it (`../bro/src/ngl/ngl_present.c`).
No window yet; the rest is planned.

## Pieces

- **Frames and scenes:** device, swapchain, a scene per render pass.
- **2D:** quads, quad batches and text (HUD and menus).
- **Meshes:** vertex formats from each material's layout table, index buffers as they are on the disc.
- **Shaders:** each game's precompiled shaders converted once at build time (Big Red One: 43 vertex programs and 79
  register-combiner setups to SPIR-V). Nothing translates shaders while the game runs.
- **Constants:** one uniform block per draw at the game's own constant numbering.

The game-specific hooks live with each game (`../bro/` for Big Red One).
