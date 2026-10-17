// cod3mp: the multiplayer program's own menus (see menus_mp.cpp).

#pragma once

#include <functional>

namespace cod3::mpmenu {

// True while a menu page is on screen (any thread); the mouse is let go then.
bool MenuActive();

// What QUIT TO DESKTOP does once confirmed (save and close the program); set
// by Cod3MpApp. Called on the game's thread.
void SetQuitHandler(std::function<void()> quit);

// UI thread, every frame: warns once when the game's voice and session
// system didn't start (no audio output device), which a match needs.
void Tick();

}  // namespace cod3::mpmenu
