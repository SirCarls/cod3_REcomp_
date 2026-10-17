// cod3 - one setting, two places.
//
// The crosshair has a console dvar (cg_drawCrosshair) and a row in the game's
// own Options > Gameplay page (a byte in the player's profile settings). The
// game draws the crosshair only while both are on (CG_DrawCrosshair,
// 0x82439270), so a change made in one place was hidden by the other, and the
// profile brought its old value back at the next mission. Here the two follow
// each other: whichever changed last wins, and the player's last choice is
// kept in the console config and applied again at every level start (and in
// multiplayer, which reads the same config).

#pragma once

#include <cstdint>
#include <functional>

struct PPCContext;

namespace cod3::settings {

// A port setting changed outside F2 (the game's own menus, a hook): cod3.toml
// is saved like F2's Save does, so the change is the one the next start uses.
// Cod3App sets how (on the UI thread); any thread may ask.
void SetSaveHandler(std::function<void()> save);
void RequestSave();

// Game main thread, every frame (Engine::OnGuestFrame).
void SyncCrosshair(PPCContext& ctx, uint8_t* base);

// A level starts (the SV_SpawnServer hook): the last choice is applied again.
void NoteLevelStart();

}  // namespace cod3::settings
