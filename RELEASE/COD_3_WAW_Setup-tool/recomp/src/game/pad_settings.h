// cod3 - [acct2-ocr] 2026-11-07 the controller's settings, per player (owner 22:00: "controller sensitivity ...
// vertical and horizontal don't have the same caps"; 22:08: "same cap for both", "make sure there's a DVAR for
// this. Also please make sure we get controller related DVARS in the game"; 22:22: "a controller tab in every
// settings menu"; 22:24: "Make sure these controller options account for split-screen ... for everything").
//
// The settings (cod3.toml, category cod3/Controller; the ~ console takes them by name). Player 1's have no suffix,
// split-screen players 2 to 4 end in _p2 .. _p4 (as cod3_weapon_sensitivity and cod3_deadzone_p1 .. _p4 do):
//   cod3_pad_sensitivity_max          the highest Horizontal and Vertical Sensitivity on the game's Options >
//                                     Controls rows, both rows, every player (37 to 100, default 50). The game's
//                                     own pages stopped Horizontal at 37 and Vertical at 50: the campaign's in
//                                     the page's left / right (game/menu_fov.cpp), cod3mp's in its Pause > OPTIONS
//                                     Init, the gauges' top steps (game/mp/options_mp.cpp).
//   cod3_pad_horizontal_sensitivity   the player's own Options > Controls rows: -1 (the default) leaves them to the
//   cod3_pad_vertical_sensitivity     page; set (0 or more), the setting is the row's value, written into that
//   cod3_pad_invert                   player's control settings every frame it differs (a profile read at a
//   cod3_pad_vibration                start or a sign-in included); a change made on the page becomes the
//                                     setting's.
// How fast the stick turns: the game's CL_MouseMove (campaign 0x8250FF48, cod3mp 0x8252B578) turns the view by
// the row x 0.04 (0 counts as 0.1) x the game's own m_yaw / m_pitch (0.7; they are the stick's, not the mouse's:
// the port's mouse has cod3_m_yaw / cod3_m_pitch, and World at War's names m_yaw / m_pitch in the ~ console are
// those) x the stick x the frame's time, within the game's turn limits. cl_yawspeed / cl_pitchspeed (140) are
// the +left / +right / +lookup / +lookdown keys' (CL_AdjustAngles), not the stick's.
// Players: the campaign has player 1 only (its one block of control settings, 0x82A8E610); cod3mp's local
// player N (split screen) uses the controller profile its client names (*(0x82C7A270 + 180 + client * 3208), as
// the Pause > OPTIONS refresh 0x824EB138 reads it; profile blocks 0x82C6B730 + profile * 6984).

#pragma once

#include <cstdint>

struct PPCContext;

namespace cod3::pad {

inline constexpr int kGameHorizontalMax = 37;  // the game's own pages
inline constexpr int kGameVerticalMax = 50;
inline constexpr int kSensitivityCeiling = 100;
inline constexpr int kMaxPlayers = 4;

// In the order of the campaign's Options > Controls rows 0 to 3 (Invert Aim, Vibration, Horizontal Sensitivity,
// Vertical Sensitivity).
enum Setting : int { kInvert, kVibration, kHorizontal, kVertical, kSettingCount };

// 1 in the campaign, 4 in cod3mp.
int Players();
// Any thread: cod3_pad_sensitivity_max, 37 to 100.
int SensitivityMax();
// Any thread. player 0 to 3.
const char* SettingName(int player, int setting);
int Most(int setting);  // 1, or the sensitivity cap
// The setting (-1: the Options page's).
int Wanted(int player, int setting);
// Sets it (-1 hands it back to the page); the caller saves (F2 does, the in-game pages call SaveSettings).
void SetWanted(int player, int setting, int value);
// What the player's Options row holds now (-1: not known yet, or no such player now).
int Live(int player, int setting);
// Every player's settings back to -1 and the cap to 50 (F2's Reset).
void ResetAll();

// Game main thread, every frame (Engine::OnGuestFrame): the settings that are set, into each player's controls.
void Sync(PPCContext& ctx, uint8_t* base);
// Game main thread: an Options page changed `setting` of `player` (0-based) to `value`; the setting, when set,
// takes it (and the settings are saved).
void FollowPage(int player, int setting, int value);

// F2 > Controls > Controller (ui/hud_overlay.cpp): the section; true when a setting changed (F2 saves).
bool DrawF2Section();

}  // namespace cod3::pad
