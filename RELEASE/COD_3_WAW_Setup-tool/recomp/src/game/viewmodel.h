// cod3 - weapon offsets with Counter-Strike 2's commands (owner 2026-10-20:
// "add weapon offests with the same code as CS2"), in the campaign and in
// multiplayer (owner 19:32: "yes", with sliders like the crosshair's).
//
// The port's settings cod3_viewmodel_offset_x / _y / _z and
// cod3_viewmodel_presetpos (F2 > Game > Weapon, cod3.toml), and in the ~
// console under Counter-Strike 2's own names, viewmodel_offset_x/_y/_z and
// viewmodel_presetpos (ui/waw_dvars.cpp), so a CS2 config line can be pasted
// as it is. Counter-Strike 2's axes and ranges: x to the right (-2.5 to
// 2.5), y away from you (-2 to 2), z up (-2 to 2), in game units; all 0 is
// the game's own place. viewmodel_presetpos 1, 2 and 3 are CS2's Desktop,
// Couch and Classic positions, 0 the three offsets.
//
// How (game/viewmodel.cpp): the game places the gun in one function
// (sub_82445118, from CG_AddViewWeapon; cod3mp: sub_8245E3A8), which at its end adds its own
// cheat-protected cg_gunX / cg_gunY / cg_gunZ along the camera's forward,
// left and up axes. The offsets are added to those three values for that
// call only, so no dvar changes and cheat protection doesn't come into it.
// They fade out as the weapon comes up to the sights (the player state's
// fWeaponPosFrac), so aiming down the sights stays centered.
//
// Not done: CS2's viewmodel_fov. Call of Duty 3 draws the gun with the
// view's own field of view (cg_fov / F2), not a separate one.
// Multiplayer reads the same settings (the campaign's cod3.toml) with its
// own addresses (mp/config/campaign_map.tsv, tools/mp_aliases.py).

#pragma once

namespace cod3::viewmodel {

// The settings' names (F2 and the console aliases read them by name, so
// they also build into a program without this module).
inline constexpr const char* kOffsetX = "cod3_viewmodel_offset_x";
inline constexpr const char* kOffsetY = "cod3_viewmodel_offset_y";
inline constexpr const char* kOffsetZ = "cod3_viewmodel_offset_z";
inline constexpr const char* kPreset = "cod3_viewmodel_presetpos";

// Counter-Strike 2's axes: x right, y away from you, z up.
struct Offsets {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

// Counter-Strike 2's viewmodel_presetpos positions (their viewmodel_fov, 60,
// 54 and 68, has no counterpart here). False for 0 (the three settings) or
// anything else.
inline bool PresetOffsets(int preset, Offsets* out) {
  switch (preset) {
    case 1: *out = {1.0, 1.0, -1.0}; return true;   // Desktop
    case 2: *out = {0.0, 0.0, -1.5}; return true;   // Couch
    case 3: *out = {2.5, 0.0, -1.5}; return true;   // Classic
    default: return false;
  }
}

inline const char* PresetName(int preset) {
  switch (preset) {
    case 1: return "Desktop";
    case 2: return "Couch";
    case 3: return "Classic";
    default: return "Custom";
  }
}

}  // namespace cod3::viewmodel
