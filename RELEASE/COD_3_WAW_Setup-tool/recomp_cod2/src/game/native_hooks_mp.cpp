// cod2mp - the native renderer's hooks in Call of Duty 2's multiplayer program. [waw-decomp] 2026-11-11
//
// The campaign's native_census.cpp hooks its back end for the census, the capture and the native side
// (cod2_debug_native_side). This program has only the native side so far, so only the three hooks it needs: the
// draw of one pass, the pass set up last before it, and the end of a frame. Their addresses and how they were found:
// native_addresses.h (kDrawPass, kSetupPass, kSwapBuffers). With cod2_debug_native_side off (the default) each hook
// only calls the game's own function.

#if defined(COD2_MP)

#include <atomic>
#include <cstdint>

#include <rex/hook.h>

#include "game/native_side.h"

namespace {

std::atomic<uint32_t> g_last_pass{0};  // "RB_SetupPass"'s r3: the pass the next draws use

}  // namespace

// "RB_SetupPass"(pass r3, vertex format r4): the campaign's 0x82368C40.
REX_EXTERN(__imp__sub_823946A8);
REX_HOOK_RAW(sub_823946A8) {
  if (cod2::native::SideOn()) g_last_pass.store(ctx.r3.u32, std::memory_order_relaxed);
  __imp__sub_823946A8(ctx, base);
}

// The draw of one pass (draw arguments r3): the campaign's 0x82368310. The native side keeps the draw before the
// game's own code runs, as in the campaign.
REX_EXTERN(__imp__sub_82393D78);
REX_HOOK_RAW(sub_82393D78) {
  cod2::native::SideDraw(base, ctx.r3.u32, g_last_pass.load(std::memory_order_relaxed));
  __imp__sub_82393D78(ctx, base);
}

// RB_SwapBuffers: the campaign's 0x82372D78. The native side draws a kept frame after the game's own code runs.
REX_EXTERN(__imp__sub_8239E7F0);
REX_HOOK_RAW(sub_8239E7F0) {
  __imp__sub_8239E7F0(ctx, base);
  cod2::native::SideFrameEnd(base);
}

#endif  // COD2_MP
