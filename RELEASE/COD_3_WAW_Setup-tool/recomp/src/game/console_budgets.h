// cod3 - [acct2-decomp] 2026-11-22 what is drawn at a distance: the Xbox 360's budgets, raised for the PC.
//
// The owner, 2026-11-22: lift "Detail at a distance" and "Grass at a distance", "On by default" after tests. Both
// programs; both settings put the console's own values back.
//  - cod3_model_detail_distance: how far models keep their detailed versions, times the console's distances. The
//    game picks a model's detail from r_lodscale (smaller: more detail); for each 3D view (RE_RenderScene) it gets
//    r_lodscale divided by this, and its own value back afterwards, so the dvar itself never changes.
//  - cod3_grass_patches: how many grass patches the game keeps around the player, the nearest first. The console
//    kept 88; the game keeps 500 itself when it records a movie (capture_movie). It is set right after the game's
//    grass set-up (sub_823F6A68) chose its own.

#pragma once

#include <cstdint>

namespace cod3::budgets {

// Around one RE_RenderScene call (the game's main thread): r_lodscale for the view, and the value to put back.
// DetailBegin returns the dvar written (0: none, nothing to put back).
uint32_t DetailBegin(uint8_t* base, float* saved);
void DetailEnd(uint8_t* base, uint32_t dvar, float saved);

}  // namespace cod3::budgets
