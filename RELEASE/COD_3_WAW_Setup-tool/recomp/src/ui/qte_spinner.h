// cod3 - the spinner for stick battle actions (QTEs), and its F2 settings.
//
// osu!-style (an idea only, no code from it): while a stick battle action
// runs (game/qte_actions.h), a ring in the middle of the HUD shows what to
// do. A guide dot circles the way to turn, at the speed that gives the
// action's full speed, with a fading trail; a second dot with its own trail
// is the player's mouse cursor or stick. Toggles show a line with two ends
// to sweep between, rowing the right half of the ring (the stroke) and the
// way back. The ring and the meter inside it are green while it goes well,
// red when the attempt fails (wrong way, stopped, slipped), and white again
// as the next attempt starts. A line under it says what to do with the
// device in use and follows a switch at once.
//
// Style "original" (cod3_qte_style) leaves the game's own arrow; with the
// mouse a small ring then still shows where the cursor is. Everything is
// sized from the HUD's height (cod3_qte_spinner_scale on top), drawn with
// fixed buffers, nothing allocated per frame.

#pragma once

#include <imgui.h>

namespace cod3::ui {

// HudOverlay::OnDraw (UI thread), every frame.
void DrawQteSpinner(ImGuiIO& io);

// F2 > Controls: the "Battle actions" section. Returns true when a setting
// changed (to be saved).
bool DrawQteSettings();

}  // namespace cod3::ui
