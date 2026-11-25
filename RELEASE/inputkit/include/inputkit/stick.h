// inputkit - a controller stick's dead zone, for games whose port reads the pad itself (Big Red One, Finest Hour).
//
// [input-port] 2026-11-24, Big Red One's apply_dead_zone (bro/src/input/bro_pad.c, acct2-bro): radial, so diagonals
// keep their direction; inside the dead zone the stick is at rest, outside it the rest of its travel is stretched
// back over the whole range. Call of Duty 3 sets the game's own threshold per player instead (its dead zone function
// reads joy_threshold; game/keybinds/pad_binds.cpp), and so would any game that has one.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* x, y: XInput's -32767..32767 (y up-positive or down, either). dead_zone: 0 (none) .. 0.9 of a full push. */
void inputkit_radial_dead_zone(int *x, int *y, float dead_zone);

#ifdef __cplusplus
}
#endif
