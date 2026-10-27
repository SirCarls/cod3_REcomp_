// cod3 - [claude-9b] 2026-10-27 the weapon in the left hand (viewmodel_flip.cpp), both programs.
#pragma once

#include <cstdint>

namespace cod3::viewmodel {

// Whether local player `client` (0 to 3) has the weapon mirrored (cod3_viewmodel_flip, _2 to _4).
bool FlipFor(uint32_t client);
// Whether local player `client` is on a mounted gun (a turret, a vehicle's gun): that gun isn't mirrored.
bool OnMountedGun(uint8_t* base, uint32_t client);
// Whether the view model gets its own scene this frame: only then can its culling be turned for the mirror.
bool OwnViewModelScene(uint8_t* base);
// The gun placement (viewmodel.cpp) mirrored this frame's gun for `client`: its scene is marked when built.
void MarkMirrored(uint32_t client);
// [claude-9b] 2026-10-27 The scene back end calls these around each scene it draws (campaign: render_job_timing.cpp's
// hook of 0x8214F770; cod3mp: viewmodel_flip.cpp's hook of 0x82167688). For the marked view model scene the device's
// cull mode is turned for the whole scene; BeginScene says whether it was, for EndScene to turn it back.
bool BeginScene(uint8_t* base);
void EndScene(uint8_t* base, bool turned);

}  // namespace cod3::viewmodel
