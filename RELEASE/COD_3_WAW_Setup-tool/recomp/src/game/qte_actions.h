// cod3 - stick battle actions (QTEs) that work with any input.
//
// Some of the game's battle actions ask for the right (or left) stick:
//   * StickSwirl: "Rotate right thumbstick clockwise." (valves, cranes and
//     bombs in Falaise and others),
//   * StickToggleHoriz / StickToggleVert: "Toggle right thumbstick left and
//     right.", "Move right thumbstick up and down to move crowbar.",
//   * Rowboat: "Rotate right thumbstick in a half circle to row." (Mayenne).
// With keyboard and mouse the mouse turns the view and never moves a stick,
// so these could not be finished (owner, 2026-10-13: "softlocks user with
// mouse and keyboard"). This module (the game side; the HUD is
// ui/qte_spinner.h) makes them work with every input, at every frame rate:
//
//   * The mouse moves a virtual cursor (osu! style) while such an action
//     runs, and the cursor becomes the stick the action reads: its direction
//     for swirls and rowing, its position for toggles. The view stays still
//     meanwhile. Rowing needs only circles: the right half of the circle is
//     the stroke, the left half the way back.
//   * A controller works as on the console. Whichever of the two moved last
//     drives the action; switching in the middle keeps all progress (the
//     game keeps it), and the cursor starts where the stick was.
//   * The swirl's progress no longer drops with the frame rate (the game's
//     own math gives 60²/N² of the console's progress at N frames a second);
//     at 60 and below nothing changes. The swirl turret (cranes) likewise.
//   * Style (cod3_qte_style): "spinner" replaces the game's stick arrow with
//     the spinner (a guide dot circling the way to go, green while it goes
//     well, red when it fails); "original" keeps the game's arrow. The input
//     fixes above apply in both.
//   * The game's stick hints read "Circle the mouse clockwise." and the like
//     while the mouse drives (English).
//   * Each action's definition is logged once by name, so a play log tells
//     which action a level's crane, valve, bomb or boat is.
//   * cod3_qte_demo fakes an action (the game's math, no level needed) to
//     test the spinner and the input anywhere, the title screen included.
//
// Hooks (default.xex): InteractInputRcvr Start 0x824F2D88 and Update
// 0x824F16D8 (every frame for the live receiver), the four stick receivers'
// Stop, StickSwirl's step 0x82508238, the swirl turret 0x8250F648, the stick
// arrow's Show 0x824C0EB0. The pad state hook is in game/button_prompts.cpp,
// the mouse in game/mouse_look.cpp, the hint texts in game/menu_fov.cpp's
// Localize hook.

#pragma once

#include <cstdint>
#include <string>

namespace cod3::qte {

enum class Kind : uint8_t { kNone, kSwirl, kToggleHorizontal, kToggleVertical, kRow };
enum class Device : uint8_t { kPad, kMouse };
enum class Verdict : uint8_t { kNeutral, kGood, kFail };
enum class Style : uint8_t { kSpinner, kOriginal };

// What the HUD draws, copied out under a lock (ui/qte_spinner.cpp).
struct Snapshot {
  bool active = false;  // a stick action runs (or the demo)
  bool demo = false;
  Kind kind = Kind::kNone;
  bool clockwise = false;   // swirl: the way to turn
  bool left_stick = false;  // the action reads the left stick
  bool ready = true;        // the game takes input (false: "get ready")
  Device device = Device::kPad;
  Verdict verdict = Verdict::kNeutral;
  const char* reason = "";  // why it failed (static text), "" otherwise
  float quality = 0.0f;     // 0..1: how well it goes (the meter)
  float stick_x = 0.0f;     // the stick the game reads (its scale / 128), up positive
  float stick_y_up = 0.0f;
  float pad_x = 0.0f;       // the real stick (before the mouse is written), -1..1
  float pad_y_up = 0.0f;
  float guide_speed = 0.0f;  // swirl: radians a second; toggles: flips a second; rowing: pace (1 normal)
  float row_phase = 0.0f;    // rowing: the game's stroke phase, 0..1
  uint32_t serial = 0;       // new for every action
  uint32_t events = 0;       // flips or strokes so far (flashes)
};

Style CurrentStyle();
// Whether the spinner (not the game's arrow) shows for the current driver.
bool SpinnerShown();
Snapshot CurrentSnapshot();

// UI thread: raw mouse counts while the mouse is held by the game
// (MouseLook::AddDelta), and plain pointer motion otherwise (the demo on the
// title screen, game/button_prompts.cpp).
void AddMouse(float dx, float dy);
void AddPointerMotion(float dx, float dy);
// UI thread: the virtual cursor in spinner radii (y down); false when no
// action runs.
bool CursorPosition(float* x, float* y);
// UI thread (ui/qte_spinner.cpp): the spinner's radius as drawn, in pixels.
void NoteDrawnRadius(float pixels);

// Game thread, the pad state hook (game/button_prompts.cpp): every read of
// a pad. While the mouse drives an action, its stick goes into the state of
// the pad the game reads. `state` is the guest X_INPUT_STATE.
void ApplyToPad(uint8_t* base, uint32_t user, uint32_t state);

// Game thread (game/mouse_look.cpp): true while a stick action owns the
// mouse, so it doesn't turn the view.
bool MouseOwnsView();

// Game thread, the Localize hook (game/menu_fov.cpp): given the text the
// game's Localize returned, a guest string to show instead (a stick hint
// while the mouse drives, English only), else 0.
uint32_t LocalizeHint(uint8_t* base, uint32_t text);

// The settings row in the game's menus (game/menu_video.cpp, the in-game
// video options): value text, one step, help.
std::string MenuValue();
void MenuStep();
std::string MenuHelp();

}  // namespace cod3::qte
