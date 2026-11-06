// cod3 - [acct2-bro] 2026-11-05 the frame's 2D drawn natively by ngl_vk, beside the GPU emulation (a check; see
// ngl_side.cpp and dev/agents/acct2-bro/COD3-NGL-PLAN.md, step 3).

#pragma once

#include <cstdint>

namespace cod3::ngl_side {

// At the start of nglListSend (0x8213DAA0), on the main thread, before the frame's lists go to the render job.
// Does nothing unless cod3_debug_ngl_side is on.
void OnListSend(uint8_t* base);

// At Direct3D's draws (0x822F3A28 indexed, 0x822F3620 plain), on the render job's threads, before the draw; call_site
// is the draw's return address. Reads Direct3D's device only for a frame after some dumps (see ngl_side.cpp,
// kDevicePointer); otherwise does nothing.
void OnDraw(uint8_t* base, uint32_t call_site);

// At the generic surface draw's pass setup (0x82142F20: shader state, object, surface), on the render job's threads,
// before that pass's draws. side91: notes the object, so a draw of the first-person gun can be probed. Does nothing
// unless cod3_debug_ngl_side is on.
void OnPass(uint8_t* base, uint32_t object);

}  // namespace cod3::ngl_side
