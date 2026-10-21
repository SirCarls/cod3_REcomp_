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

#pragma once

namespace cod3::ui::server_tab {

// Whether the tab is shown now: this machine is the host (or no match runs).
bool Shown();

// The tab's contents (game/hud thread, inside BeginTabItem). True when a
// setting changed (the caller saves the settings).
bool Draw();

}  // namespace cod3::ui::server_tab
