// cod3 - the player's field of view: cg_fov, cg_fovScale and their limits.
//
// The player's own cg_fov and cg_fovScale are kept apart from the changes the
// game makes for a moment (see fov.cpp for where those come from):
//
//   * Level scripts set cg_fov for scripted views (Black Baron's event 5
//     zooms in by 40 and back, then puts its saved value back). Those writes
//     come through the level interface's setcvar functions, are never saved
//     as the player's choice, and the player's value comes back when the
//     script restores the view or at the next level start.
//   * The interaction system (vehicles, turrets, melee, levers ...) writes
//     cgame's copy of cg_fov (the vmCvar) directly: it saves the value when an
//     interaction starts and puts that saved value back at the end, which
//     brought back an old FOV when the player had changed it meanwhile. After
//     every interaction end, and whenever no interaction is running, cgame's
//     copy follows the dvar again.
//   * At every level start (SV_SpawnServer, map_restart) both dvars are set to
//     the player's values again.
//   * Aiming down the sights and a vehicle's or turret's own view (computed in
//     CG_GetViewFov) still narrow the view while they last.
//
// The player's values always keep the effective FOV (cg_fov x cg_fovScale)
// within 65-165: F2, the game's Options rows, the console (cg_fov,
// cg_fovScale, fov, set/seta/reset/exec) and the settings file on load all go
// through FitFov / FitScale below. A value outside is limited to the nearest
// allowed one, and the console says so.

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "game/program.h"

struct PPCContext;

namespace cod3::fov {

// The Xbox 360 game's cg_fov: 65 in the campaign, 80 in multiplayer [mp].
inline constexpr float kDefaultFov = kMultiplayer ? 80.0f : 65.0f;
inline constexpr float kDefaultScale = 1.0f;
inline constexpr float kMinFov = 65.0f;        // cg_fov on its own (World at War's lowest)
inline constexpr float kMaxFov = 160.0f;       // the game's own limit in CG_GetViewFov
inline constexpr float kMinScale = 0.2f;       // cg_fovScale on its own (World at War's range)
inline constexpr float kMaxScale = 2.0f;
inline constexpr float kMinEffective = 65.0f;  // cg_fov x cg_fovScale
inline constexpr float kMaxEffective = 165.0f;

// Allowed values of one setting, given the other one. cg_fov bounds are whole
// degrees and cg_fovScale bounds two decimals, both rounded inward, so any
// value in the range keeps the effective FOV within 65-165.
struct Range {
  float min;
  float max;
};
Range FovRange(float scale);  // cg_fov values allowed with this cg_fovScale
Range ScaleRange(float fov);  // cg_fovScale values allowed with this cg_fov

struct Settings {
  float fov;
  float scale;
};
// cg_fov was changed: it is limited to what the scale allows (the scale only
// moves when no cg_fov at all fits it, e.g. 0.2 from a settings file).
Settings FitFov(float fov, float scale);
// cg_fovScale was changed: it is limited to what cg_fov allows.
Settings FitScale(float fov, float scale);

// "cg_fov x cg_fovScale stays within 65 to 165 (with cg_fovScale 1.10:
// cg_fov 65 to 150)" for help texts and console notes. Not for per-frame use.
std::string DescribeFovRange(float scale);
std::string DescribeScaleRange(float fov);

// A level script has the view right now (a scripted zoom); the player's FOV
// comes back after it. Any thread.
bool ScriptedViewActive();

// The player's values come and go through Engine (fov(), SetFov(),
// SetFovScale(), SetFovSettings(), ApplyFovFromGuest(); defined in fov.cpp).

// Engine::LoadConfig: the player's values from the console config's seta
// lines, limited; out-of-range lines are rewritten in `archived`.
void LoadPlayerSettings(std::map<std::string, std::string>& archived);

// Game main thread, every engine frame (Engine::OnGuestFrame): a console or
// config change becomes the player's (limited, saved); cgame's copy of
// cg_fov follows the dvar while no interaction runs.
void Sync(PPCContext& ctx, uint8_t* base);

// Game main thread, after Engine::ResetArchivedCvars put the dvars back to
// their defaults: those are the player's values now (nothing is saved).
void AdoptFromGuest(PPCContext& ctx, uint8_t* base);

// Game main thread: a level starts (SV_SpawnServer, map_restart). The
// player's cg_fov and cg_fovScale are set again and any scripted view ends.
void OnLevelStart(PPCContext& ctx, uint8_t* base);

}  // namespace cod3::fov
