// cod3 - keyboard and mouse keys that call the game's own actions (the key
// binds plan, dev/agents/keybinds/PLAN.md, step 2): fire, aim, use, reload,
// grenades, melee, binoculars, sprint / hold breath, lean, jump, crouch,
// prone, weapons ... each a key of its own instead of a controller button
// the keyboard pretends to press (the actions and the stored form are in
// game/keybinds/model.h). Both programs (the campaign and cod3mp).
//
// How a key reaches the game:
//  * A window listener (UI thread, z 197: below the PC menus, above ImGui and
//    the SDK's keyboard/mouse driver) takes a bound key while you play, so
//    the driver never sees it: R reloads and F uses instead of both pressing
//    the controller's X. In the game's menus (and the pause menu), and for
//    keys no action has, keys go on to the driver and press the controller
//    buttons of their keybind_* rows, as before: Space is A, Escape START.
//  * The game's pad poll (once before every frame, game main thread)
//    delivers it: the action's own command handler (+attack, togglecrouch
//    ...; found by name in the game's command list) is called with a key id
//    of its own (AUX1 / AUX2, keys the controller never sends) and
//    Sys_Milliseconds, the way the controller's button calls it; console
//    text for the cgame's commands (weapnext, weaponslot 1, +scores). Only
//    when the pad poll takes button presses itself (no game menu over the
//    game); a release always goes.
//  * While an interaction runs (a mounted gun, a vehicle, a battle action) the
//    key presses the controller button the game binds to that action instead
//    (RT for +attack in layout A), in the pad state the game reads, so the
//    interaction decides as it does for a controller: never more than the
//    controller can do. A key held when one starts or ends goes on as a held
//    controller button would (it does nothing new until pressed again).
// The controller is unchanged: its layout (xenon_a..d.cfg) and its buttons,
// at the same time as the keys (two holders per game button).
//
// Settings (cod3.toml): cod3_binds_kbm_p1 (model.h), cod3_binds_actions (the
// switch: false = every key presses its row's controller button, as before),
// cod3_aim_toggle_kbm_p1 (the aim key holds or toggles), cod3_debug_binds
// (log each delivery and, in the campaign, the binoculars / hold breath /
// sprint state, with a screenshot of the game's picture when the binoculars
// are up).
// cod3mp: the same keys for player 1 (its pad poll goes through the local
// clients once a frame, each with its own copy of the game's buttons; keys are
// actions only while player 1 is in a match); the console's bind command
// there is still the game's own (controller buttons) for now.

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
// one-time moves of the old controller-button keys (model.h Migrate and
// MigrateRows), saved. Both programs (cod3mp: MigrateRows only, every changed
// row keeping its keys; cod3mp_binds_version).
void MigrateSettings(const std::filesystem::path& config_path);
// UI thread. `overlay_open`: the console or F2 has the keyboard (no action
// then; keys held are let go).
void Attach(rex::ui::Window* window, size_t z_order, std::function<bool()> overlay_open);
void Detach(rex::ui::Window* window);
// UI thread, every frame: an overlay or a game menu opened, or the window
// lost focus -> keys held are let go.
void OnUiFrame();
// The keys are the actions' now (in play, no menu over the game; any thread).
bool InPlay();

// --- game/button_prompts.cpp (campaign), game/keybinds/pad_state_mp.cpp (cod3mp) ---------
// The GetPadState hook, after device detection saw the real state: adds the
// controller buttons keys press inside an interaction (any thread).
void ApplyToPad(uint8_t* base, uint32_t user, uint32_t state);
// The key a prompt shows for a controller button in play: the key of the
// action the game binds to that button (pad key 215, the right trigger, is
// +attack in layout A -> "MOUSE1"). "" when it isn't one of these actions
// or the action has no key (the prompt keeps its old name). Game thread.
std::string KeyNameForPadKey(uint8_t* base, int pad_key);
// The same for a game command ("+binoculars" -> "B"; any thread).
std::string KeyNameForCommand(std::string_view command);
// The game's action words ("[HOLD_USE]" -> USE, the table its hints use)
// that name one of these actions, and that action's key ("USE" -> "F"; ""
// when it has none). The game writes such a word as the controller button in
// words ("Press and hold X"); with the keyboard the prompts write the key
// instead ("Press and hold [F]").
bool IsActionWord(std::string_view word);
std::string KeyNameForActionWord(std::string_view word);
// + 1 whenever a key changes (the prompts lay their texts out again).
uint32_t Generation();

// --- ui/game_console.cpp ---------------------------------------------------------------
// "bind K +reload" (or "bind K reload"), "unbind K", "bind K" for keyboard and
// mouse keys and these actions; everything else (controller buttons: L3,
// UPARROW ...) stays the game's. True when handled; `changed` when a key
// changed (the caller saves the settings).
bool HandleConsoleCommand(const std::vector<std::string>& args, cheats::Source source,
                          const std::function<void(bool error, const std::string& text)>& print,
                          bool* changed);

// --- ui/hud_overlay.cpp ----------------------------------------------------------------
// F2 > Controls > Key bindings: the switch, the aim toggle and one row per
// action. True when a setting changed.
bool DrawF2Rows();
// The setting F2's Reset puts back ("" = the defaults).
inline constexpr const char* kSettingName = "cod3_binds_kbm_p1";

}  // namespace cod3::binds
