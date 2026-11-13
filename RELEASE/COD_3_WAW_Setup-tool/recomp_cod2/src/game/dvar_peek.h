// cod2 / cod2mp - tests: the game's own dvars, read from the game and written to the log. [f2-menus]
//
// Call of Duty 2's log has no answers to console questions (asking "cg_fov" prints nothing there), so a test can't
// see what a dvar is by asking. With cod2_debug_dvars="name,name,..." this looks each one up with the game's own
// Dvar_FindVar (campaign 0x822E7CB0, multiplayer 0x823069E0: the first call of Dvar_GetInt / Dvar_GetBool) and logs
// its value every cod2_debug_dvars_seconds: "cod2: dvars: jump_height=39 g_speed=190 ...". A dvar the game doesn't
// have shows as "name=?". Used to compare the Xbox 360 program's movement numbers with the PC copy's, and to see
// that an F2 row's dvar really changed.
//
// The dvar (read from Dvar_ValueToString 0x822E54B8): its type is the byte at +6 (0 bool, 1 float, 2 / 3 / 4 that
// many floats, 5 int, 6 a list's index, 7 text, 8 a color's four bytes), its value starts at +8.

#pragma once

#include <cstdint>
#include <string>

#include <rex/hook.h>

namespace cod2::dvar_peek {

// The game's Dvar_FindVar for `name` (0 when the game has no such dvar), from inside a hook on the game's own
// thread; every register is put back.
uint32_t FindDvar(PPCContext& ctx, uint8_t* base, const std::string& name);

// The game's main thread, once a controller read (game/test_start.cpp's hook). Every register is put back.
void OnGameFrame(PPCContext& ctx, uint8_t* base);

}  // namespace cod2::dvar_peek
