// cod3 - sensitivity per weapon class, from the hip and aiming down the
// sights (owner 2026-10-20: "also allow for ADS sensitivity scaling per weapon
// class. (percentage based from 0-200%)", "also regular sensitivity not just
// ADS", "will require submenu in-game and f2", "MP needs per player logic").
//
// The settings (cod3.toml, category cod3/Aim), one line per player:
//   cod3_weapon_sensitivity       player 1 (the campaign's player, and
//                                 multiplayer's first: keyboard and mouse or
//                                 the first controller)
//   cod3_weapon_sensitivity_p2 .. _p4
//                                 multiplayer's split-screen players 2 to 4
// each a list of "group=hip/aiming" percentages, 0 to 200, for example
//   "smg=80/60 sniper=100/40"
// A group left out is 100/100: the game's own feel. "group=N" sets both.
//
// The groups follow the game's own weapon classes (its weapon table's
// weaponClass, plus its ammoType for shotguns and its scope for sniper
// rifles): rifles, shotguns, sniper rifles (scoped), submachine guns, light
// machine guns, machine guns, pistols, launchers, mounted guns, and
// everything else (grenades, binoculars ...).
//
// How (game/aim_sensitivity.cpp): the weapon in the player's hands is read
// the way the game's own crosshair and gun placement read it, and how far it
// is up to the sights (0 at the hip, 1 at the sights) blends the hip and the
// aiming percentage. The mouse multiplies its turn by the result
// (game/mouse_look.cpp). The stick's turn is the game's own CL_MouseMove
// (campaign 0x8250FF48, cod3mp 0x8252B578); the turn it made this frame,
// finished with its own limits, is multiplied the same way, so a percentage
// holds at full stick too. Aim assist is never touched (owner 2026-10-20:
// "The aim assist cannot be increased in any way"): the game's Sticky Aim
// moves the player state's delta_angles in the movement code, not this turn.
// 100/100 changes nothing. The FOV, the zoom and aiming down the sights
// themselves are never touched.

#pragma once

#include <cstdint>

struct PPCContext;

namespace cod3::aimsens {

enum Group : int {
  kRifle,
  kShotgun,
  kSniper,
  kSmg,
  kLmg,
  kMg,
  kPistol,
  kLauncher,
  kMounted,
  kOther,
  kGroupCount
};

inline constexpr int kPlayers = 4;  // multiplayer's split screen; the campaign has player 1
inline constexpr int kMinPercent = 0;
inline constexpr int kMaxPercent = 200;
inline constexpr int kMenuStep = 5;  // one step left or right in the game's menus

const char* GroupKey(int group);    // the setting's word: "rifle", "shotgun" ...
const char* GroupLabel(int group);  // "Rifles", "Shotguns" ...
const char* SettingName(int player);  // "cod3_weapon_sensitivity", "..._p2" ...

// Any thread. player 0 to 3; aiming false = from the hip.
int Percent(int player, int group, bool aiming);
void SetPercent(int player, int group, bool aiming, int percent);
void ResetPlayer(int player);

// Guest thread: what local client `client` holds now (kOther when it can't be
// read) and how far it is up to the sights (0 to 1).
int HeldGroup(uint8_t* base, uint32_t client, float* aim);
// Guest thread: the multiplier for local client `client` now (1 at 100/100).
double Multiplier(uint8_t* base, uint32_t client);

// F2 > Controls (ui/hud_overlay.cpp): the section; true when a setting changed.
bool DrawF2Section();

}  // namespace cod3::aimsens
