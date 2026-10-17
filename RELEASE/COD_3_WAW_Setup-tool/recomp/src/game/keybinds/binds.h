// cod3 - the campaign's own keyboard and mouse actions (first step of the key
// binds plan, dev/agents/keybinds/PLAN.md): binoculars, sprint / hold breath, hold
// breath and sprint are keys of their own instead of controller buttons the
// keyboard pretends to press (the actions and the stored form are in
// game/keybinds/model.h).
//
// How a key reaches the game:
//  * A window listener (UI thread, z 197: below the PC menus, above ImGui and
//    the SDK's keyboard/mouse driver) takes a bound key, so the driver never
//    sees it: B no longer presses anything on the emulated controller, and
//    Shift no longer presses the left stick click or makes Shift+arrows the
//    D-pad.
//  * The game's pad poll (sub_82522138, once before every frame, game main
//    thread) delivers it: the action's own command handler (IN_BinocularsDown
//    ...) is called with a key id of its own (AUX1 / AUX2, keys the
//    controller never sends) and Sys_Milliseconds, the way the controller's
//    button would call it. Only when the pad poll takes button presses itself
//    (in_joystick on, no game menu over the game); a release always goes.
//  * While an interaction runs (a mounted gun, a vehicle, a battle action) the
//    key presses the controller button the game binds to that action instead
//    (the pad's L3 for +binoculars in layout A), in the pad state the game
//    reads, so the interaction decides as it does for a controller: never
//    more than the controller can do.
// The controller is unchanged: its layout (xenon_a..d.cfg) and its buttons.
//
// Settings (cod3.toml): cod3_binds_kbm_p1 (model.h), cod3_debug_binds (log
// each delivery and the binoculars / hold breath / sprint state, with a
// screenshot of the game's picture when the binoculars are up).
// Campaign only: cod3mp keeps the controller-button keys (its L3 is
// +sprintbreath, and it has no binoculars).

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::ui {
class Window;
}

namespace cod3::cheats {
enum class Source;
}

namespace cod3::binds {

// --- cod3_app.cpp --------------------------------------------------------------------
// OnPreSetup, after the settings file is read and the defaults are set: the
// one-time move of the old controller-button keys (model.h Migrate), saved.
void MigrateSettings(const std::filesystem::path& config_path);
// UI thread. `overlay_open`: the console or F2 has the keyboard (no action
// then; keys held are let go).
void Attach(rex::ui::Window* window, size_t z_order, std::function<bool()> overlay_open);
void Detach(rex::ui::Window* window);
// UI thread, every frame: an overlay opened or the window lost focus -> keys
// held are let go.
void OnUiFrame();

// --- game/button_prompts.cpp -----------------------------------------------------------
// The GetPadState hook, after device detection saw the real state: adds the
// controller buttons keys press inside an interaction (any thread).
void ApplyToPad(uint8_t* base, uint32_t user, uint32_t state);
// The key a prompt shows for a controller button in play: the key of the
// action the game binds to that button (pad key 220, the left stick click,
// is +binoculars in layout A -> "B"). "" when it isn't one of these actions
// or the action has no key (the prompt keeps its old name). Game thread.
std::string KeyNameForPadKey(uint8_t* base, int pad_key);
// The same for a game command ("+binoculars" -> "B"; any thread).
std::string KeyNameForCommand(std::string_view command);
// The game's action words ("[HOLD_BINOCS]" -> BINOCS, the table its hints use)
// that name one of these actions, and that action's key ("BINOCS" -> "B"; ""
// when it has none). The game writes such a word as the controller button in
// words ("Click and hold the left stick"); with the keyboard the prompts write
// the key instead ("Press and hold [B]").
bool IsActionWord(std::string_view word);
std::string KeyNameForActionWord(std::string_view word);
// + 1 whenever a key changes (the prompts lay their texts out again).
uint32_t Generation();

// --- ui/game_console.cpp ---------------------------------------------------------------
// "bind <key> +binoculars", "unbind <key>", "bind <key>" for keyboard and
// mouse keys and these actions; everything else (controller buttons: L3,
// UPARROW ...) stays the game's. True when handled; `changed` when a key
// changed (the caller saves the settings).
bool HandleConsoleCommand(const std::vector<std::string>& args, cheats::Source source,
                          const std::function<void(bool error, const std::string& text)>& print,
                          bool* changed);

// --- ui/hud_overlay.cpp ----------------------------------------------------------------
// F2 > Controls > Key bindings: one row per action. True when a key changed.
bool DrawF2Rows();
// The setting F2's Reset puts back ("" = the defaults).
inline constexpr const char* kSettingName = "cod3_binds_kbm_p1";

}  // namespace cod3::binds
