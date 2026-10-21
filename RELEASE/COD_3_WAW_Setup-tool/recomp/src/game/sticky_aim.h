// cod3 - Sticky Aim, the game's aim assist: the same strength at any frame
// rate, and the match's rules for it (owner 2026-10-20: "The aim assist
// cannot be increased in any way", "sticky aim and aim slowdown need to be
// server protected dvars", "aim assist cannot scale up or down based on
// FPS. it needs to remain the same despite the differences.").
//
// What Sticky Aim is (read from the game, 2026-10-20): Call of Duty 3's only
// aim assist. While a player turns or moves near an enemy, the game keeps an
// aim point on that enemy and pulls the view toward it a fixed share of the
// way each time it runs, turning the player state's view angles and
// delta_angles. It runs in the player movement code, once per usercmd:
// PM_UpdateViewAngles (campaign 0x8248DF18) calls it (campaign 0x8248D138,
// cod3mp 0x824ABCC8) with the player state, the usercmd and the one before.
// The client makes one usercmd per frame, so at 144 fps it pulled 2.4 times as
// often as at the console's 60 and was that much stronger; at 30 fps weaker.
// There is no separate slowdown: the pull is what slows a sweep past an enemy.
// The turn limits CL_MouseMove applies are shellshock and script look
// control, not aim assist (game/aim_sensitivity.h).
//
// What this does (game/sticky_aim.cpp):
//   - Frame rate: the pull runs at the console's 60 Hz, counted from the
//     usercmds' own times (serverTime), never from this machine's clock: a
//     usercmd that crosses no 60 Hz step pulls nothing, one that crosses two
//     (a slow frame) pulls twice. The server and every client's prediction
//     see the same usercmds, so they agree to the bit.
//   - The match's rules, two server_sided_protected dvars
//     (game/protected_dvars.h): sticky_aim (0 off for everyone, 1 each
//     player's own Options > Sticky Aim choice) and aim_slowdown (0-100: the
//     share of each pull that is kept; 100 is the game's own, nothing is
//     ever stronger). In a match the host's values apply to everyone;
//     F2 > Game > Server sets them, shown only to the host.
//     cod3mp doesn't build that category yet (it is campaign-only until the
//     peer-to-peer transport exists), so cod3mp keeps the game's own rule:
//     each player's choice, the full pull, at the fixed 60 Hz.
// Nothing here can make the pull stronger than the game's at 60 fps.

#pragma once

namespace cod3::sticky_aim {

// What applies now: 0 = Sticky Aim does nothing, 1 = the game's own pull
// (the match's rules above; the player's own Options choice still decides
// whether the game runs it at all).
double PullShare();

#if !defined(COD3_MP)
// F2 > Game > Aim (ui/hud_overlay.cpp), under the player's own Sticky Aim
// switch: one line with what applies now and where the rule is set. The
// rule's rows are on F2 > Game > Server, shown only to the host
// (ui/server_tab.h).
void DrawF2Section();
#endif

}  // namespace cod3::sticky_aim
