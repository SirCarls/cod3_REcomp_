// cod3 - battle actions that ask for the right stick, with the mouse.
//
// Some of the game's battle actions (InteractInputRcvrStickSwirl: "Rotate
// right thumbstick clockwise"; StickToggleHoriz / StickToggleVert: move the
// right stick left and right, or up and down, e.g. the crowbar) read the
// right stick. With keyboard and mouse the mouse turns the view directly
// (game/mouse_look.h) and never moves that stick, so these actions could not
// be finished (owner, 2026-10-13: "softlocks user with mouse and keyboard").
// While one of them is running, mouse movement becomes the right stick:
// circles turn it around, side to side or up and down sweep it. A controller
// is left alone (only mouse movement writes the stick), and switching between
// the two in the middle of an action works.

#pragma once

#include <cstdint>

namespace cod3::stick_actions {

// UI thread: raw mouse motion (from MouseLook::AddDelta).
void AddMouse(float dx, float dy);

// Game main thread, after the game read player 1's pad (the pad-state hook
// in button_prompts.cpp): writes the mouse's stick into it while a stick
// battle action is running. `state` is the guest X_INPUT_STATE.
void ApplyToPad(uint8_t* base, uint32_t state);

}  // namespace cod3::stick_actions
