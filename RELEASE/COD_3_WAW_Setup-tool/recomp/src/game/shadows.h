// cod3 - the game's dynamic shadows and the "Shadow quality" setting.
//
// How Call of Duty 3 draws shadows (default.xex; addresses in addresses.h,
// namespace shadows):
//   * The level's own light and its fixed shadows are baked into lightmaps;
//     nothing here touches them.
//   * Soldiers, vehicles and other models cast real-time sun shadows through
//     shadow maps: 5 depth textures (D24FS8) made once at start, slot 0 at
//     1024x1024 and slots 1-4 at 512x512.
//   * Each frame the renderer aims them along the sun: while r_testshadow is
//     1 (its default) three of them start as cascades around the camera
//     (0x8242BAA0: 384, 1024 and 2048 units wide), and at the frame's end
//     0x8242C170 aims all five at the models that cast (tight views around
//     one soldier or a group; seen in memory dumps). The world's shaders
//     compare against them (COD_SHADOW_DEPTHMAP: 4 or 9 point taps blended
//     with the texture's bilinear weights). These slot lights also bring the
//     sun's light onto the ground and models near the camera: with none set
//     (both functions skipped) the ground there is about a third as bright.
//     r_testshadow 0 only stops the camera cascades: the maps aimed at
//     models are still drawn.
//   * Each map is drawn depth only, without MSAA, at EDRAM tile 0: its scene
//     clears depth and stencil to 0 (far) first, and its viewport and
//     projection come from the map's own size. Then it is copied (resolved)
//     to its texture: a 512x512 map takes 224 of the EDRAM's tiles, a
//     1024x1024 one 832.
//
// What the port changes: the emulated GPU's draw resolution scale applies to
// every render target, so at 2x (1080p and 1440p plans) the shadow maps are
// drawn at 2x too (1024 and 2048 pixels a side): sharper than the console,
// the same coverage, and the receivers' taps and weights follow the larger
// texels, so the soft edge is narrower. Resolve readback (readback_resolve)
// copied each shadow map back to guest memory every frame although the CPU
// never reads them (the SDK's readback_resolve_cpu_reads_only now skips
// them). Nothing else in the port touches shadows.
//
// cod3_shadow_quality (F2 > Video; World at War's sm_enable in the console):
//   original - the game as it is (default; nothing is changed)
//   off      - no model is added to a shadow map (0x82425BD0 takes the
//              game's own no-light path); the slot lights and the sun stay,
//              the maps are drawn empty, nothing is in shadow; at once
//   (high    - retired: it remade slots 1-4 at 1024x1024; a saved "high" is
//              refused at load, which leaves original)
// None of them changes the game's speed, only what the GPU draws.

#pragma once

namespace cod3 {

// Follows cod3_shadow_quality for "off" (any thread; also called by the
// setting's change callback).
void ApplyShadowQuality();

}  // namespace cod3
