// cod3 - keyboard and mouse keys in the game's button prompts.
//
// The game names controller buttons in its texts: menu footers ("(A) SELECT
// (B) BACK"), message boxes, in-game hints ("Press and hold (X) to swap for
// Kar98k"), battle actions ("Press (A) to swing."). They are pictures from
// the game's button font (addresses.h, namespace prompts). While the keyboard
// and mouse are in use (mnk_mode on and no controller button pressed since
// the last key or mouse press) each picture becomes the key that presses
// that button, in brackets and in the text's own font: "[ENTER] SELECT
// [ESC] BACK", "Press and hold [R] to swap for Kar98k".
//  * In play the key is the first key of the button's keybind_* setting
//    (keybind_x = "R,F" shows [R]), so rebinding changes the prompts. A
//    button the game binds to one of the port's own keyboard actions
//    (game/keybinds/binds.h) shows that action's key instead: the left stick
//    click, +binoculars, shows [B].
//  * In the menus (cod3_pc_menus, game/pc_menu.h) A is [ENTER], B [ESC],
//    START [ENTER] and the D-pad the arrow keys, the keys the PC menus take.
//  * Stick actions read "Use [WASD]" / "Use the mouse" (English), and the
//    game's "Pull" / "Click" verbs its own "Press"; the title says "PRESS
//    ENTER".
// A controller button press brings the controller's buttons back; a key or
// mouse press shows the keys again. Texts already on screen follow at once
// (they are laid out again). Only the player on the first controller can
// show keys (the keyboard and mouse are guest user 0); the others always see
// their controllers' buttons.
// cod3_glyph_style picks how controller buttons look: auto (the default;
// [mp-menus] each player's own controller, as the SDK names it, SDK patch
// 0067: an Xbox pad or one it can't name shows the game's own pictures, a
// PlayStation pad "[CROSS]", a Nintendo pad the label in that place, "[B]"
// for the bottom button), or one style for everyone: xbox (the game's own
// pictures), playstation, nintendo or gamecube (button names until pictures
// of our own exist; one data table, kButtons). cod3_key_prompts turns the
// keys off. Nothing else changes: no timing, no input, only what the
// prompts show.
// [claude-9b-think] 2026-10-27 UNTESTED: with cod3_key_caps on, mouse buttons
// are a small mouse picture and the PlayStation (playstation: PS4/5,
// playstation2: PS2), Nintendo and GameCube styles pictures of our own
// (simple shapes and plain letters, in colors close to the controller's own:
// ui/hud_overlay.cpp cod3_glyph_color_mode); the texts' keyboard-and-mouse wording can
// be set by string key (config/prompt_text_kbm.tsv, userdata's copy).

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>  // [rexglue-native] 2026-11-17 P0 FrameLatch
#include <string>
#include <vector>

namespace rex::ui {
class Window;
}
namespace rex::input {
class InputSystem;
}

namespace cod3::prompts {

// UI thread: watch the window's keyboard and mouse (never takes an event).
void Attach(rex::ui::Window* window, size_t z_order);
void Detach(rex::ui::Window* window);
// [mp-menus] A keyboard or mouse use another listener made up (the PC
// menus' test script, cod3_test_menu_input): the prompts show keys.
void NoteKeyboardUse();
// [mp-menus] The app's input system (after setup; nullptr at shutdown): which
// kind of controller each player holds, for cod3_glyph_style auto.
void SetInputSystem(rex::input::InputSystem* input);
// [claude-9b-think] The keyboard-and-mouse texts by string key: the built-in
// config/prompt_text_kbm.tsv and `user_dir`/prompt_text_kbm.tsv (the
// player's rows replace the built-in ones). At start, after the user folder
// is known; again to reload. Campaign only (nothing in cod3mp).
void LoadPromptText(const std::filesystem::path& user_dir);
// [rumble] Whether the keyboard and mouse are in use now: mnk_mode on, and a
// key or mouse press since the last controller button (what the first
// controller's prompts follow, whatever cod3_key_prompts says). Any thread.
bool KeyboardMouseInUse();

// [acct2-decomp] 2026-10-25 (owner 23:42/23:43: the keys should look like the
// "SPACE Skip" key cap; that prompt should follow the controller too):

// What the first player's prompts show right now: keys, or a controller's
// buttons in one style (the same choice the game's prompts make: mnk_mode,
// cod3_key_prompts, the last device used, cod3_glyph_style). Any thread.
// [claude-9b-think] kPlayStation the PS4/5 style, kPlayStation2 the PS2 style.
enum class Look : uint8_t { kKeys, kXbox, kPlayStation, kNintendo, kGameCube, kPlayStation2 };
Look FirstPlayerLook();
// [acct2-main] 2026-11-21 (owner 12:28: "on the glyphs we now have more controller support, we should have automatic
// glyph detection as well. Please make sure the controls tab also knows"): what F2's Controls tab shows of the
// controller guest user `user` (0-3) holds, the same choice the prompts make for it (cod3_glyph_style; auto: that
// controller's own, the SDK's family of it and whether its A B X Y go by their printed letters). Any thread.
// What the SDK says the controller is ("a GameCube controller", "a PlayStation controller" ...).
std::string PadDescription(uint32_t user);
// The style its buttons show in ("GameCube", "Nintendo Switch (by letter)", "Xbox 360 (the game's own)" ...).
const char* PadStyleName(uint32_t user);
// cod3_glyph_style is auto (each controller its own).
bool PadStyleIsAuto();
// The name the prompts give the controller button the game numbers `game_key` (keybinds/pad_model.h GameKey: A 212,
// B 213 ... START 27) in that style: "CROSS", "B", "L1", "Z" ...; "" for a number that is no controller button.
std::string PadButtonName(uint32_t user, int game_key);
// A read of the first controller's state outside the game's own pad reads
// (the cutscene skip reads it while the game's pad reads may not run): a
// button pressed there with no key or mouse press brings the controller's
// buttons back, as in play. Any thread.
void NotePadState(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, const int16_t axes[4]);
// The keys the game's prompts show, as key caps: while keys are shown, each
// key name a text line draws ("[ENTER]", "[WASD]") is drawn invisibly by the
// game and noted here, in the game's 640x480 menu space, for the overlay to
// draw as a key cap like "SPACE Skip"'s (ui/hud_overlay.cpp). Entries are
// those drawn in the last `max_age_ms`. UI thread.
// [claude-9b-think] Also our pictures: `picture` set, `name` the code between
// the braces the game laid out ("1" a mouse button, "x" a PlayStation cross,
// "L1" a labeled cap ...; button_prompts.cpp PictureCode and MouseCode),
// drawn in `look`'s style (kKeys: the mouse pictures).
// [claude-9b-think] 2026-10-27 UNTESTED `fitted`: the room is as wide as the cap (the game laid out a string of the
// cap's width, button_prompts.cpp FittedRoom; cod3_key_cap_scale scales both): the overlay draws the cap across it.
struct KeyCap {
  float x0, y0, x1, y1;  // menu space
  std::string name;      // without the brackets
  float alpha;           // the text's opacity as the game draws it (fades)
  Look look = Look::kKeys;
  bool picture = false;
  bool fitted = false;
};
std::vector<KeyCap> CurrentKeyCaps(int max_age_ms);

// [rexglue-native] 2026-11-17 P0 (owner 16:48: "text flickers at low FPS", menus, HUD, subtitles, both renderers; 16:52: "EVERY HUD
// element needs to draw at every FPS properly"; main 17:00 / 17:35, claude/native-hud-text-plan-2026-11-17.md): the
// overlays latched per game frame. What an overlay draws over the game's picture follows what the game asked for in
// its latest complete frame (one its frame counter has moved past), until the game's next frame is complete: at any
// frame rate, through hitches. The clock is only the safety net for a game that stops making frames (a loading
// screen). Before: each overlay kept what the game asked for a fixed time on the clock (the key caps and the text
// layer 50 ms, the crosshair 150 ms), and a game frame longer than that (20 fps and under, any hitch) left presents
// without it: the flicker. cod3_overlay_frame_latch (cod3/Display, on) turns it off: the clock alone, as before.
//
// Whether the latch is on (cod3_overlay_frame_latch). Any thread.
bool OverlayFrameLatchOn();
// The game's frame counter now (pc_menu::kFrameCount: Com_Frame adds one every frame), read from guest memory once a
// hook has seen it (the pad reads, the line draws); 0 before that. Any thread.
uint32_t GameFrameNow();
// In a hook where the game draws (a line or a string): the counter (as GameFrameNow), and the note that the game drew
// now (a loading screen draws while the counter stands still). Game thread.
uint32_t GameFrameIn(uint8_t* base);
// The latch's clock, read once for a set of decisions.
struct LatchState {
  uint32_t counter = 0;        // GameFrameNow()
  int64_t still_ms = 0;        // since the counter last moved (0: not seen moving yet)
  int64_t stall_ms = 500;      // the game makes no frames when it stands still longer (3 of its longest recent
                               // frames, at least 500 ms)
  int64_t safety_ms = 1000;    // nothing older holds (3 of its longest recent frames, 1 to 20 s)
  bool drawing_still = false;  // the game drew after its counter stood still for stall_ms (a loading screen)
};
LatchState ReadLatchState();
// One copy of an overlay element as the game asked for it: in game frame `frame`, `age_ms` ago; the element is asked
// for every `cadence` frames (1: each frame).
struct LatchCopy {
  uint32_t frame = 0;
  int64_t age_ms = 0;
  uint32_t cadence = 1;
};
// Which of an element's two newest copies (from two different game frames) the latest complete game frame had: 1 the
// newest, 2 the one before, 0 neither. The newest copy from a complete frame, while the latest complete frame is within
// its cadence of it and it is younger than the safety net. With its frame unknown (0: no counter yet), by the clock:
// age_ms <= fallback_ms. While the game draws with its counter standing still (a loading screen), only copies drawn
// since, while drawn again within max(fallback_ms, stall_ms).
int LatchPick(const LatchState& state, const LatchCopy& newest, const LatchCopy* before, int fallback_ms);
// The safety net alone (ReadLatchState().safety_ms). Any thread.
int OverlaySafetyMs();
// An overlay element in one place (a crosshair, a countdown, the menu's preview): Note() where the game asks for it
// (its draw hook); Holds() where the overlay draws it: the latch is on, the game's frame is known and its latest
// complete frame had the element (LatchPick on its last two game frames). The overlay then refreshes the element's
// last-seen time, so the element's own clock check passes. Any thread.
class FrameLatch {
 public:
  void Note();
  bool Holds() const;

 private:
  mutable std::mutex mutex_;
  LatchCopy newest_, before_;         // their age_ms unused here: the times are below
  int64_t newest_ms_ = 0, before_ms_ = 0;
};
// [rexglue-native flicker 2026-11-17] cod3_debug_overlay_latch (debug, off): a change of what an overlay finally shows (`what`: its name;
// `hash` of what it shows, `count` its lines or caps), logged with the game's frame counter; a second change at one
// counter value is flagged (the P0 test: every present between two game frames shows the same). The 4-argument form
// takes the counter its decision was made at.
void NoteOverlayShown(const char* what, uint64_t hash, size_t count);
void NoteOverlayShown(const char* what, uint64_t hash, size_t count, const LatchState& state);
// The key caps latched per game frame: each line's newest copy from a complete game frame (LatchPick, the copy before
// while the line is being drawn again); the clock (fallback_ms, CurrentKeyCaps's rule) while the frame is unknown.
// CurrentKeyCaps returns this while the latch is on. UI thread.
std::vector<KeyCap> LatchedKeyCaps(int fallback_ms);

// [rexglue-native flicker 2026-11-17] cod3_debug_overlay_presents (debug, off; main 19:38, P0's proof): per present, whether each
// overlay element was drawn, against whether the game's latest complete frame (its counter less one) asked for it,
// with the latch on or off alike (it reads the latch's frame counter only). A line every 5 s per element: the
// presents while the game makes frames, those the latest complete frame asked for the element in, those it was drawn
// in, and the missing ones (asked for, not drawn: the flicker) with the game frames they fell in. The game's side:
// PresentProbeAsked where the game asks for it (its draw hook, with the frame it drew in); the overlay's: one
// PresentProbeShown a present. Any thread.
enum class ProbeElement : int { kKeyCaps = 0, kText = 1, kCrosshair = 2 };
void PresentProbeAsked(ProbeElement element, uint32_t frame);
void PresentProbeShown(ProbeElement element, bool shown);

}  // namespace cod3::prompts
