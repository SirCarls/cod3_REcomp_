// cod3 - [acct2-main] the controller's own binds in game (owner 2026-11-05:
// "an in-game menu for keybinds. One for controller and one for mouse and
// keyboard", "accounting for splitscreen, MP, and SP", "Allow for up to 3
// binds on the same key", "Please account for splitscreen and fix it").
//
// Each player (split screen players 1-4 in cod3mp; the campaign's player is
// player 1) has cod3_binds_pad_p1 .. _p4: the buttons that differ from the
// controller layout the game runs (xenon_a..d.cfg, mp_xenon_*.cfg), in
// pad_model.h's text ("x=reload,melee;dpad_up="). START stays the pause menu.
//
//  * The first action of a button goes into that player's own bind table
//    (Key_SetBinding, with the game's active local client set to that
//    player), so the game does everything with it as with its own layout: the
//    pad poll's commands, the interactions (a mounted gun, the battle
//    actions), the menus' reads, the button prompts. When the game runs a
//    layout file again (a layout chosen in the options, a map loaded), its
//    bindings are taken as the new layout and the player's changes go on top
//    again; a button reset to the layout gets the layout's binding back.
//  * The second and third actions can't share a key in the game (it calls
//    one command per key), so the port calls them on the button's press and
//    release, for the player that owns the controller: a button command with
//    the button's own key as its holder and the game's time (like the pad
//    poll does), a plain command, or console text executed at once for that
//    player (weaponslot 1, +scores ...: Cbuf_ExecuteText EXEC_NOW while the
//    player is the active local client, so split-screen players 2-4 get
//    their own command, not player 1's). Presses count only while the game
//    takes the player's presses (no menu, in a match) and no interaction
//    runs; releases always go.
// The raw pad state is read in the GetPadState hook (binds::ApplyToPad,
// before the keyboard's buttons go in), so the keyboard never presses these.
// SDK patch 0073 holds a press shorter than a frame for one read, so a quick
// tap reaches the second and third actions too.

#pragma once

#include <cstdint>

#include <rex/ppc.h>

#include <functional>
#include <string>  // [f2-menus] 2026-11-11 SeatLayout

#include "game/keybinds/pad_model.h"  // [f2-menus] 2026-11-11 SeatLayout

namespace cod3::binds {
// Defined in binds.cpp, for pad_binds.cpp (game main thread, at the pad poll):
// the game takes this local client's presses now (no game menu, in a match)
// and no interaction runs;
bool PlayerTakesPresses(PPCContext& ctx, uint8_t* base, uint32_t cl);
// the controller (guest user 0-3) the local client plays with;
uint32_t PadOfPlayer(uint8_t* base, uint32_t cl);
// `fn` run as that local client (the game's active local client and, in
// cod3mp, that client's own copy of the game's buttons).
void AsPlayer(uint8_t* base, uint32_t cl, const std::function<void()>& fn);
}  // namespace cod3::binds

namespace cod3::binds::padbinds {

// The GetPadState hook (any pad the game reads, read without error).
void OnPadState(uint8_t* base, uint32_t user, uint32_t state);
// The pad poll (game main thread): before it, the first actions into the bind
// tables; after it, the second and third actions.
void BeforePoll(PPCContext& ctx, uint8_t* base);
void AfterPoll(PPCContext& ctx, uint8_t* base);
// [f2-menus] 2026-11-11 F2 > CONTROLS > Controller (any thread): seat 0-3's layout as the game last ran it
// (layout_text: kButtonCount strings, the bindings as the game has them); false before that seat's first sync.
bool SeatLayout(int seat, pad::Layout* layout, std::string* layout_text);

}  // namespace cod3::binds::padbinds
