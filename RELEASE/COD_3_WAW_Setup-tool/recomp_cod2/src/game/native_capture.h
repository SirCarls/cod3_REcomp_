// cod2 - the native renderer's step N1: the capture of each draw's submission. [acct2-cod2]
//
// Off by default (cod2_native_capture). The census's hooks (native_census.cpp) call these, so each guest
// function keeps one hook. See dev NATIVE-RENDERER.md 2.10 (the caches read) and section 8 (N1).

#pragma once

#include <cstdint>

namespace cod2::native {

// True while cod2_native_capture is above 0.
bool CaptureOn();

// At the draw 0x82368310 (before the game's own code runs): records the draw whose arguments are at `args`,
// with the pass "RB_SetupPass" 0x82368C40 last set up (`pass`), from the back end's caches.
void CaptureDraw(const uint8_t* base, uint32_t args, uint32_t pass);

// At RB_SwapBuffers 0x82372D78 (after the game's own code runs): the frame being recorded becomes the last
// whole frame, and every cod2_native_capture seconds one summary of it goes to the log.
void CaptureFrameEnd();

}  // namespace cod2::native
