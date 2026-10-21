// cod3mp: the multiplayer program's own menus (see menus_mp.cpp).

#pragma once

#include <functional>

namespace cod3::mpmenu {

// True while a menu page is on screen (any thread), any player's.
bool MenuActive();

// [mp-menus] True while a menu of the keyboard and mouse's player is up (any
// thread): the PC menus take them (game/pc_menu.h MenuActive); with
// cod3_pc_menus off, any menu page on screen. The mouse is let go then and
// the keys' actions stop; in split screen another player's pause menu
// leaves them alone.
bool KeyboardMenuUp();

// UI thread, every frame: warns once when the game's voice and session
// system didn't start (no audio output device), which a match needs.
void Tick();

}  // namespace cod3::mpmenu
