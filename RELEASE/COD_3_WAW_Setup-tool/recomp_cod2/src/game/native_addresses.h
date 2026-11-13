// cod2 - the guest addresses the native renderer reads, for each of Call of Duty 2's two programs. [waw-decomp]
//
// The campaign's (default.xex) were read in its decompile and checked by runs; native_side.cpp's comments say where
// each comes from. The multiplayer program's (default_mp.xex), found 2026-11-11 without a run:
//   - every function the campaign's hooks use was found in it by `results/waw-decomp-tools/cod2mp/align.py` (in the
//     work folder): the same size, the same sizes of its callees and about as many callers, near where the functions
//     named in both programs around it put it. Each scored 1.00 (all callees alike); the next best 0.56 or less.
//   - every global by the matched functions' references (Ghidra's globals.tsv of both programs): the multiplayer
//     global they use most. Each block moved as one piece: the tess buffers and state by 0x475680, the render state
//     caches and the constant mirror by 0x475880 (every offset inside them as in the campaign).
//   - checked by eye in the decompile: the multiplayer R_SetViewport 0x82390AB8 keeps the viewport at 0x841A6D2C and
//     hands it to Direct3D with the device from 0x8380B5E0, as the campaign's 0x82365660 does with 0x83D314AC and
//     0x83630E60.
// The image ends where the codegen's REX_IMAGE_SIZE says: 0x82000000 + 0x2290000 (campaign), + 0x27C0000 (multiplayer).

#pragma once

#include <cstdint>

namespace cod2::native::guest {

#if defined(COD2_MP)
// Hooked functions (native_hooks_mp.cpp)
inline constexpr uint32_t kDrawPass = 0x82393D78u;    // the draw of one pass (campaign 0x82368310)
inline constexpr uint32_t kSetupPass = 0x823946A8u;   // "RB_SetupPass" (campaign 0x82368C40)
inline constexpr uint32_t kSwapBuffers = 0x8239E7F0u; // RB_SwapBuffers (campaign 0x82372D78)
// Data
inline constexpr uint32_t kImageEnd = 0x847C0000u;
inline constexpr uint32_t kTessVertices = 0x8415DF00u;
inline constexpr uint32_t kTessIndexPointer = 0x8416DF14u;
inline constexpr uint32_t kTessMaterial = 0x8416DF20u;
inline constexpr uint32_t kTessTechniqueType = 0x8416DF24u;
inline constexpr uint32_t kRenderState = 0x841A6D00u;  // the render state caches: the state, then words 0 and 1
inline constexpr uint32_t kStateWord0 = 0x841A6D08u;
inline constexpr uint32_t kStateWord1 = 0x841A6D0Cu;
inline constexpr uint32_t kTargetId = 0x841A6D14u;
inline constexpr uint32_t kTargetWidth = 0x841A6D18u;
inline constexpr uint32_t kTargetHeight = 0x841A6D1Cu;
inline constexpr uint32_t kViewportCache = 0x841A6D2Cu;
inline constexpr uint32_t kIndexCache = 0x841A6D48u;
inline constexpr uint32_t kStreamCache = 0x841A6D4Cu;
inline constexpr uint32_t kTextureCache = 0x841A6D7Cu;
inline constexpr uint32_t kConstantMirror = 0x841A4D00u;
inline constexpr uint32_t kDevicePointer = 0x8380B5E0u;
#else
inline constexpr uint32_t kDrawPass = 0x82368310u;
inline constexpr uint32_t kSetupPass = 0x82368C40u;
inline constexpr uint32_t kSwapBuffers = 0x82372D78u;
inline constexpr uint32_t kImageEnd = 0x84290000u;
inline constexpr uint32_t kTessVertices = 0x83CE8880u;
inline constexpr uint32_t kTessIndexPointer = 0x83CF8894u;
inline constexpr uint32_t kTessMaterial = 0x83CF88A0u;
inline constexpr uint32_t kTessTechniqueType = 0x83CF88A4u;
inline constexpr uint32_t kRenderState = 0x83D31480u;
inline constexpr uint32_t kStateWord0 = 0x83D31488u;
inline constexpr uint32_t kStateWord1 = 0x83D3148Cu;
inline constexpr uint32_t kTargetId = 0x83D31494u;
inline constexpr uint32_t kTargetWidth = 0x83D31498u;
inline constexpr uint32_t kTargetHeight = 0x83D3149Cu;
inline constexpr uint32_t kViewportCache = 0x83D314ACu;
inline constexpr uint32_t kIndexCache = 0x83D314C8u;
inline constexpr uint32_t kStreamCache = 0x83D314CCu;
inline constexpr uint32_t kTextureCache = 0x83D314FCu;
inline constexpr uint32_t kConstantMirror = 0x83D2F480u;
inline constexpr uint32_t kDevicePointer = 0x83630E60u;
#endif

}  // namespace cod2::native::guest
