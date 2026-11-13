// cod3 - F2 > Game > Server: the match's rules, for the host only (owner
// 2026-10-20 20:42: "so the server sided dvars should be their own tab, and
// only accessible if they are host").
//
// Every server_sided_protected dvar (game/protected_dvars.h) is on this one
// tab, and nowhere else in F2: the movement rule (alternate_movement and its
// four parts) and aim assist's rule (sticky_aim, aim_slowdown). The tab is
// only there while this machine is the host: in the campaign (where there is
// no match and you are always the host) and when hosting a match. A guest in
// a match doesn't see it at all; the Movement and Aim tabs still say what
// applies now. The console and the protection itself are protected_dvars.cpp's.
//
// Campaign program only for now: cod3mp doesn't build the protected category
// until its transport exists.
// [acct2-ocr] 2026-11-07: cod3mp builds it too (recomp/mp/CMakeLists.txt). There the tab is shown outside a
// match and in a match no other console plays (split screen: game/multiplayer.h IsLocalOnlyMatch); in a system
// link match it stays hidden until a host-to-guest channel exists. cod3mp has no CoD4 movement yet
// (game/movement/hooks_mp.cpp is stage 1), so its tab leaves the movement rule out.

#pragma once

namespace cod3::ui::server_tab {

// Whether the tab is shown now: this machine is the host (or no match runs).
bool Shown();

// The tab's contents (game/hud thread, inside BeginTabItem). True when a
// setting changed (the caller saves the settings).
bool Draw();

}  // namespace cod3::ui::server_tab
