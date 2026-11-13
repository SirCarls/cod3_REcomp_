// cod2 - the PC copy's movement numbers in the campaign. [f2-menus]
//
// Owner 2026-11-10 15:57: "cod2 should copy what the PC copy has." (asked: all of it; this is the third part, the
// movement feel).
//
// The Xbox 360 campaign and the PC campaign register the same movement dvars. Read side by side (the PC copy's
// defaults from CoD2SP_s.exe with dev/agents/f2-menus/tools/cod2_pc_dvar_defaults.py; the 360 program's from the
// running game with cod2_debug_dvars, run cod2dv1), 28 of 33 are equal: g_gravity 800, jump_height 39,
// jump_stepSize 18, jump_slowdownEnable 0, friction 5.5, stopspeed 100, the fall heights 200 / 350, inertiaMax 50,
// the view bob, the breath time and the rest. Five differ:
//
//   dvar                       Xbox 360   PC copy
//   player_strafeSpeedScale    1          0.8     sideways at four fifths of the forward speed
//   player_backSpeedScale      1          0.7     backwards at seven tenths
//   player_view_pitch_up       70         85      how far the view looks up, in degrees
//   player_view_pitch_down     70         85      and down
//   player_toggleBinoculars    0          1       binoculars stay up after a press, instead of while held
//
// (cg_fov is 65 on the 360 and 80 on the PC, but the 360 widens 65 to 80.7 degrees across its 16:9 picture, so the
// view is the same; nothing to copy there.)
//
// With cod2_pc_movement on (the default) the five dvars are kept at the PC copy's numbers: looked up with the game's
// own Dvar_FindVar and written in the dvar's own value (the PC copy registers them with the cheat flag, 0x80, so
// a console "set" would be refused outside devmap; not tried here); looked at once a second, written only when one
// differs. Switched off, the 360's own five numbers are put back once.
//
// The campaign only (the file is not in cod2mp): cod2mp doesn't start a level in this port yet, so its numbers
// haven't been read.

#pragma once

#include <cstdint>

#include <rex/hook.h>

namespace cod2::pc_movement {

// The game's main thread, once a controller read (game/test_start.cpp's hook). Every register is put back.
void OnGameFrame(PPCContext& ctx, uint8_t* base);

}  // namespace cod2::pc_movement
