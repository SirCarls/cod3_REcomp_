// cod3 - [claude-9b-think] 2026-10-27 UNTESTED: the turret and gun of the vehicle the player rides drawn between the
// game's updates (cod3_blend_ride_bones), stage 1 of FINDINGS-ANIM-BLEND. Campaign only; server_step.cpp calls it.
#pragma once

#include <cstdint>

#include "game/guest.h"  // PPCContext, guest calls

namespace cod3::ride_bones {

// The end of each game update (server_step's 0x8253F438 hook, after the game's code): the ridden vehicle's bones as
// the update left them, and which of them to blend until the next update. vehicle: its entity, 0 when the player
// rides none. Game thread.
void AfterUpdate(PPCContext& ctx, uint8_t* base, uint32_t vehicle);

// CG_DrawActiveFrame's entry, before the camera: the turret and gun bones `back` of the way from the last update's
// pose back to the one before (server_step's Back(): 1 right after an update, 0 when the next is due), as the hull and
// the camera are drawn. Puts back a blend still in the buffer first. Game thread.
void Blend(uint8_t* base, uint32_t vehicle, float back);

// PutBackDrawn (before any game code): the update's bones back, each only where the buffer still holds the blend.
// Game thread.
void Restore(uint8_t* base);

// The renderer's draw of a scene node (0x8242E450's hook, before it runs): the verify counters. Any thread.
void AtDraw(uint8_t* base, uint32_t node);

// A clock change: the snapshots go (a blend in the buffer is still put back by Restore).
void Forget();

}  // namespace cod3::ride_bones
