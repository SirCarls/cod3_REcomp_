// cod3 - keyboard/mouse keys and controller styles in the game's button
// prompts (see button_prompts.h; the game's side is in addresses.h, namespace
// prompts).
//
// How: while a prompt shows text instead of the game's button picture, laying
// out a text turns each button token into "[NAME]" instead of the button's
// character, and the button font (font 1) is swapped for the line's own font
// around the layout, the word wrap and the draw of that line, so "[NAME]" is
// measured and drawn in the text's font. Lines laid out that way are
// remembered (g_text_lines); texts whose prompts are out of date (keys <->
// buttons, menu <-> play, a rebind, another style) are laid out again from
// their last text before they are drawn.
//
// [mp-menus] Both programs. In multiplayer (cod3mp) several local players
// share the screen: each text belongs to the player it was made for (the
// game sets the current local client, prompts::kClientActiveIndex, while it
// builds and draws each player's things), and only the keyboard and mouse's
// player (binds::KeyboardClient: the one on controller 0, which they share)
// gets keys; the others keep their controllers' buttons. A text laid out
// again is laid out as its own player's (their binds too).
//
// [mp-menus] cod3_glyph_style auto (the default): each player's buttons are
// named the way their own controller labels them, from the kind of pad the
// SDK says that player is holding (rex::input::InputSystem::PadFamily, SDK
// patch 0067): an Xbox or unknown pad the game's pictures, a PlayStation pad
// [CROSS] ..., a Nintendo one [B] for the bottom button. So two players on
// one screen with a DualSense and an Xbox pad each see their own.
//
// [acct2-decomp] 2026-11-05 (owner 08:56-08:57): a plain FEText's own "~" + character buttons (cod3mp's spawn screen
// "Press ~\" to spawn", the button bars) are named the same way, in brackets, per player (RawButtonsNamed).
//
// [acct2-decomp] 2026-10-25, owner 23:42-23:43: keys are drawn as key caps
// like "SPACE Skip" (cod3_key_caps: the game draws each name invisibly, the
// overlay draws the cap over its room, KeyCap); our own key names (stick
// words, an action's key, the vehicle view hint's "Press [V]") are tokens the
// game's token pass takes like its buttons (KeyToken), so they are caps and
// follow device switches too; the cutscene skip's own controller reads count
// for the switch (NotePadState), and its prompt asks FirstPlayerLook.
//
// [claude-9b-think] 2026-10-27 UNTESTED (owner 12:28-12:37): pictures of our own and the texts by key.
//  * With cod3_key_caps on, a mouse button is a small mouse picture, and the PlayStation (PS4/5 and PS2),
//    Nintendo and GameCube styles' buttons are pictures of our own, drawn by the overlay like the key caps: the
//    game lays out a short "{code}" in the text's font to keep the room and draws it invisibly (NameText,
//    PictureCode, MouseCode, NoteKeyCaps; ui/hud_overlay.cpp DrawPromptPicture). The controller pictures are in
//    colors close to the buttons' own ([claude-setup] 2026-11-03: ui/hud_overlay.cpp cod3_glyph_color_mode, which
//    replaced cod3_glyph_colors).
//  * The keyboard-and-mouse wording of the game's texts by string key (config/prompt_text_kbm.tsv, built in;
//    userdata/prompt_text_kbm.tsv, the player's): the string tables' lookup (0x82527FD0) tells which key a text
//    came from, and the "[ACTION]" pass (0x824DE290) lays out our text instead while keys are shown, English.
//    The stick phrase table stays the fallback (campaign).

#include "game/button_prompts.h"

#include <algorithm>  // [claude-9b] std::min, the key caps' fade
#include <array>      // [rexglue-native qte] the keyboard layout's key names
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>  // [claude-9b-think] userdata/prompt_text_kbm.tsv
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>  // [acct2-ocr]
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>  // [mp-menus] PadFamily (SDK patch 0067)
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <SDL3/SDL_keyboard.h>  // [rexglue-native qte] the keyboard layout's key names (LayoutName)

#include "game/addresses.h"
#include "game/guest.h"
#include "game/keybinds/binds.h"  // [keybinds]
#include "game/qte_actions.h"
#include "game/rumble.h"  // [rumble]
#include "game/text_language.h"  // [acct2-ocr]
#include "game/use_hold.h"  // [acct2-decomp] cod3_use_hold_ms: the Use hold's words in the hints
#include "game/pc_menu.h"

#if !defined(COD3_MP)
// [claude-9b-think] kPromptTextKbm: config/prompt_text_kbm.tsv, built in (CMakeLists.txt, prompt_text_kbm.inc.in).
#include "prompt_text_kbm.inc"
#endif

REXCVAR_DEFINE_BOOL(cod3_key_prompts, true, "cod3/Mouse",
                    "Show the bound keyboard and mouse keys (e.g. [R]) instead of controller "
                    "buttons in the game's prompts while the keyboard and mouse are in use "
                    "(game/button_prompts.h)");
REXCVAR_DEFINE_STRING(cod3_glyph_style, "auto", "cod3/Interface",
                      "Controller buttons in the game's prompts: auto (each player's own controller: "
                      "an Xbox pad the game's pictures, a PlayStation pad the PS4/5 style, a Nintendo "
                      "pad its own), xbox (the game's own pictures), playstation (PS4/5), playstation2 "
                      "(PS2), nintendo or gamecube (pictures of our own while cod3_key_caps is on, else "
                      "the button names)")
    .allowed({"auto", "xbox", "playstation", "playstation2", "nintendo", "gamecube"});
// [acct2-decomp] Owner 2026-10-25 23:42: "the keyboard glyphs need to match
// the 'SPACE to skip' button".
REXCVAR_DEFINE_BOOL(cod3_key_caps, true, "cod3/Interface",
                    "Draw the keys in the game's prompts as key caps, like the \"SPACE Skip\" "
                    "prompt's, mouse buttons as a small mouse and the PlayStation, Nintendo and "
                    "GameCube buttons as pictures of our own (else the names in brackets, [R])");
// [claude-9b-think] 2026-10-27 (owner 12:35: "add toggle for fill color to match the button so they don't look like
// keycaps"): was cod3_glyph_colors here. [claude-setup] 2026-11-03 (owner: by default the buttons as the original, a
// toggle for customized colors, a rainbow option): now cod3_glyph_color_mode in ui/hud_overlay.cpp, with the colors.
// [claude-9b-think] 2026-10-27 UNTESTED (owner 10:27: the key caps fade out a little after their text; stick words and
// pictures still show with the keyboard and mouse): what the key caps and the stick words do, for a test run.
// [claude-9b-think] 2026-10-27 UNTESTED (owner 15:01: "the hint spacing is off with it's new size. Could we allow the
// user to enlarge it if desired? we would need to account for new breaklines etc"): the key caps' and our pictures'
// size; the room the game keeps for each (FittedRoom) grows with it, so the game's own line breaking makes room.
REXCVAR_DEFINE_DOUBLE(cod3_key_cap_scale, 1.0, "cod3/Interface",
                      "Size of the key caps and button pictures in the game's prompts (1: 1.3 times the text's "
                      "capitals tall); the text keeps room for them, so bigger caps can move words to the next "
                      "line. Their height stops short of the lines above and below")
    .range(0.8, 1.8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_key_caps, false, "cod3",
                    "Log the key caps' lines as the game draws them (their text's and keys' opacity, the game "
                    "frame) and as the overlay shows them, and the prompts that still name a stick or show a "
                    "button picture while keys are shown")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace bp = cod3::addr::prompts;
using cod3::guest::Call;
using cod3::guest::ReadString;
using cod3::guest::ReadU16;
using cod3::guest::ReadU32;
using cod3::guest::ReadU8;
using cod3::guest::VirtualSlot;
using cod3::guest::WriteU32;

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }

bool SameText(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Keyboard and mouse, or a controller: the last one pressed wins. Only the
// player on pad 0 can have the keyboard and mouse (the SDK gives them to guest
// user 0 with the first controller; KeyboardPlayer); the others always see
// controller buttons.
// The UI thread counts key and mouse presses; the game thread watches the
// pad state the game reads and calls it a controller when a button goes down
// with no key or mouse press since the last read.
// ---------------------------------------------------------------------------
enum class Device : uint8_t { kKeyboard, kPad };
std::atomic<Device> g_device{Device::kKeyboard};
std::atomic<uint32_t> g_kbm_presses{0};
std::atomic<int64_t> g_kbm_time{0};
// [claude-9b-think] 2026-10-27 UNTESTED the last mouse motion of any size: with mouse aim through the pad (mnk_mouse)
// the mouse moves the right stick, and a short flick could take it past the controller test's 16000 before the 24
// pixels that count as a mouse use (the prompts then showed the controller's pictures with the keyboard and mouse).
std::atomic<int64_t> g_mouse_move_time{0};
std::atomic<bool> g_mnk_mode{false};   // the SDK's mnk_mode setting
std::atomic<bool> g_mouse_aim{false};  // cod3_raw_mouse or mnk_mouse
std::atomic<uint32_t> g_bind_gen{0};   // + 1 whenever a setting the prompts show changes

// [mp-menus] kAuto: each player's own controller's (PadMode).
// [claude-9b-think] kPlayStation the PS4/5 style, kPlayStation2 the PS2 style (a choice only: auto can't tell them
// apart; the SDK names any PlayStation pad kPlayStation).
enum class Style : uint8_t { kAuto, kXbox, kPlayStation, kNintendo, kGameCube, kPlayStation2 };
std::atomic<Style> g_style{Style::kAuto};
// [mp-menus] The app's input system (SetInputSystem): which kind of pad each
// player holds.
std::atomic<rex::input::InputSystem*> g_input_system{nullptr};

void SetDevice(Device d, const char* why) {
  if (g_device.exchange(d, std::memory_order_relaxed) != d)
    REXLOG_INFO("cod3: button prompts now show {} ({})",
                d == Device::kPad ? "controller buttons" : "keyboard/mouse keys", why);
}

void KeyboardUsed(const char* why) {
  g_kbm_presses.fetch_add(1, std::memory_order_relaxed);
  g_kbm_time.store(NowMs(), std::memory_order_relaxed);
  SetDevice(Device::kKeyboard, why);
}

// ---------------------------------------------------------------------------
// [rexglue-native qte] 2026-11-09 (owner 13:19: "check for missing glyphs for keyboard"; main 13:4x: show the key
// printed on the player's keyboard). Keys are bound by their place on the keyboard (SDL scancodes, named as on a
// US keyboard: the SDK's sdl_virtual_key.cpp), so on a French AZERTY keyboard the key bound as "W" is the one
// labeled Z, on a German one "Y" is the key labeled Z, and the prompts said [W] / [Y]. The prompts now name a
// letter or punctuation key by the character the player's own layout gives that place (SDL's keymap, no
// modifiers) when that is one printable ASCII character, which the key caps' font has; otherwise (a Cyrillic or
// Greek layout, a dead key) the US name stays.
// [rexglue-native qte] 2026-11-09 (Wine test, French layout): the number row keeps its digits. Every Latin layout
// prints the digit on those keys, but without Shift AZERTY gives symbols and accented letters there, so naming
// them by the layout showed [&] ["] [(] beside [2] [7] [9] [0] (the accented letters are not ASCII and kept the
// US name): a mixed, confusing row.
// Only what the prompts show changes: the binds and the settings file stay on the keyboard places.
// SDL's keymap functions are not thread safe, so the table is made on the UI thread (when the prompts attach and
// at most once a second while keys are pressed, which catches a layout switch) and read by the game thread.
// ---------------------------------------------------------------------------
std::array<std::atomic<char>, 256> g_layout_char{};  // Windows virtual-key code -> the layout's character, 0 = US

struct LayoutKey {
  uint8_t vk;  // the Windows virtual-key code the SDK gives this place
  SDL_Scancode scancode;
};
constexpr LayoutKey kLayoutPunctuation[] = {
    {0xBA, SDL_SCANCODE_SEMICOLON}, {0xBB, SDL_SCANCODE_EQUALS},       {0xBC, SDL_SCANCODE_COMMA},
    {0xBD, SDL_SCANCODE_MINUS},     {0xBE, SDL_SCANCODE_PERIOD},       {0xBF, SDL_SCANCODE_SLASH},
    {0xC0, SDL_SCANCODE_GRAVE},     {0xDB, SDL_SCANCODE_LEFTBRACKET},  {0xDC, SDL_SCANCODE_BACKSLASH},
    {0xDD, SDL_SCANCODE_RIGHTBRACKET}, {0xDE, SDL_SCANCODE_APOSTROPHE},
};

// The character the player's layout gives `scancode`, as the prompts write it (letters in capitals), or 0 when it
// isn't one printable ASCII character. UI thread.
char LayoutCharOf(SDL_Scancode scancode) {
  const SDL_Keycode key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
  // '~' starts a button token in the game's texts: such a key keeps its US name.
  if (key < 0x21 || key > 0x7E || key == '~') return 0;
  return char(std::toupper(int(key)));
}

// UI thread: the table again; when a name changed, the prompts' texts are laid out again (g_bind_gen).
void RefreshLayout() {
  bool changed = false;
  std::string differs;
  auto set = [&](uint8_t vk, char us, SDL_Scancode scancode) {
    char c = LayoutCharOf(scancode);
    if (c == us) c = 0;  // the same as the US name: nothing to change
    if (g_layout_char[vk].exchange(c, std::memory_order_relaxed) != c) changed = true;
    if (c && differs.size() < 160) differs += std::string(differs.empty() ? "" : ", ") + us + "->" + c;
  };
  for (int i = 0; i < 26; ++i) set(uint8_t('A' + i), char('A' + i), SDL_Scancode(SDL_SCANCODE_A + i));
  // The number row (1-9, 0) is left out on purpose and keeps its digits: see the note above g_layout_char.
  static constexpr char kUsPunctuation[] = ";=,-./`[\\]'";
  for (size_t i = 0; i < std::size(kLayoutPunctuation); ++i)
    set(kLayoutPunctuation[i].vk, kUsPunctuation[i], kLayoutPunctuation[i].scancode);
  if (!changed) return;
  g_bind_gen.fetch_add(1, std::memory_order_relaxed);
  REXLOG_INFO("cod3: button prompts: the keyboard layout names {} keys", differs.empty() ? "no" : differs);
}
void MaybeRefreshLayout() {
  static int64_t next = 0;
  const int64_t now = NowMs();
  if (now < next) return;
  next = now + 1000;
  RefreshLayout();
}

// One key's name as the prompts show it, as the player's layout labels that key (any thread): a single letter or
// punctuation character, or the keys.cpp names SEMICOLON and COMMA. Every other name (the digits 0-9, SPACE,
// MOUSE1, F5, NUM4, SHIFT ...) is the same on every layout and stays. Called once per key where a name comes from
// a bind (BindKey, and the port's own actions' names, PortKeyName), never on a name already shown, so nothing is
// mapped twice.
std::string LayoutName(const std::string& name) {
  int vk = -1;
  if (name == "SEMICOLON") {
    vk = 0xBA;
  } else if (name == "COMMA") {
    vk = 0xBC;
  } else if (name.size() == 1) {
    const char c = name[0];
    if (c >= 'A' && c <= 'Z') vk = uint8_t(c);  // [rexglue-native qte] not the digits: they keep their names
    static constexpr char kUsPunctuation[] = ";=,-./`[\\]'";
    for (size_t i = 0; vk < 0 && i < std::size(kLayoutPunctuation); ++i)
      if (kUsPunctuation[i] == c) vk = kLayoutPunctuation[i].vk;
  }
  if (vk < 0) return name;
  const char mapped = g_layout_char[size_t(vk)].load(std::memory_order_relaxed);
  return mapped ? std::string(1, mapped) : name;
}
// The port's own actions' key names (game/keybinds/binds.h) as the layout labels them.
std::string PortKeyName(const std::string& name) { return LayoutName(name); }

class KeyboardWatch final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!e.prev_state()) KeyboardUsed("a key");
    MaybeRefreshLayout();  // [rexglue-native qte] a layout switch shows at the next key press
  }
  void OnMouseDown(rex::ui::MouseEvent&) override { KeyboardUsed("a mouse button"); }
  void OnMouseWheel(rex::ui::MouseEvent&) override { KeyboardUsed("the mouse wheel"); }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    // A real move: a few pixels of travel within a short time, so a resting
    // mouse or a single nudge doesn't count. The motion the platform
    // reports, else the change of position.
    float step = std::abs(e.dx()) + std::abs(e.dy());
    if (step == 0.0f && have_last_)
      step = float(std::abs(e.x() - last_x_) + std::abs(e.y() - last_y_));
    // The battle action test on the title screen follows the plain pointer
    // (game/qte_actions.h; in play the held mouse goes through MouseLook).
    if (have_last_) cod3::qte::AddPointerMotion(float(e.x() - last_x_), float(e.y() - last_y_));
    last_x_ = e.x(), last_y_ = e.y(), have_last_ = true;
    const int64_t now = NowMs();
    if (step > 0.0f) g_mouse_move_time.store(now, std::memory_order_relaxed);  // [claude-9b-think]
    if (now - moved_since_ > 500) moved_ = 0.0f, moved_since_ = now;
    moved_ += step;
    if (moved_ >= 24.0f) {
      moved_ = 0.0f;
      KeyboardUsed("the mouse");
    }
  }

 private:
  float moved_ = 0.0f;
  int64_t moved_since_ = 0;
  int32_t last_x_ = 0, last_y_ = 0;
  bool have_last_ = false;
};
KeyboardWatch g_watch;
rex::ui::Window* g_window = nullptr;

// The keybind_* settings the prompts show, by bind index.
constexpr const char* kBindCvars[] = {
    "keybind_a",            "keybind_b",              "keybind_x",
    "keybind_y",            "keybind_left_trigger",   "keybind_right_trigger",
    "keybind_left_shoulder", "keybind_right_shoulder", "keybind_start",
    "keybind_back",         "keybind_lstick_press",   "keybind_rstick_press",
    "keybind_dpad_up",      "keybind_dpad_down",      "keybind_dpad_left",
    "keybind_dpad_right",   "keybind_lstick_up",      "keybind_lstick_left",
    "keybind_lstick_down",  "keybind_lstick_right",   "keybind_rstick_up",
    "keybind_rstick_left",  "keybind_rstick_down",    "keybind_rstick_right",
};
enum : int { kBindDpadUp = 12, kBindMoveUp = 16, kBindLookUp = 20 };

bool CvarTrue(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v == "true" || v == "1";
}

Style ParseStyle(std::string_view s) {
  if (SameText(s, "xbox")) return Style::kXbox;
  if (SameText(s, "playstation")) return Style::kPlayStation;
  if (SameText(s, "playstation2")) return Style::kPlayStation2;  // [claude-9b-think]
  if (SameText(s, "nintendo")) return Style::kNintendo;
  if (SameText(s, "gamecube")) return Style::kGameCube;
  return Style::kAuto;
}

// About once a second (and at start): the settings the prompts depend on
// (F2 or the console can change them at any time).
void RefreshSettings() {
  g_mnk_mode.store(CvarTrue("mnk_mode"), std::memory_order_relaxed);
  g_mouse_aim.store(CvarTrue("cod3_raw_mouse") || CvarTrue("mnk_mouse"), std::memory_order_relaxed);
  g_style.store(ParseStyle(rex::cvar::GetFlagByName("cod3_glyph_style")), std::memory_order_relaxed);
  static std::mutex mutex;
  static std::string last;
  std::string now;
  for (const char* c : kBindCvars) now += rex::cvar::GetFlagByName(c) + '\n';
  now += std::to_string(cod3::binds::Generation()) + '\n';  // [keybinds] the port's own action keys
  now += g_mouse_aim.load(std::memory_order_relaxed) ? "mouse" : "keys";
  now += REXCVAR_GET(cod3_key_caps) ? "\ncaps" : "\nnames";  // [claude-9b-think] pictures or names: laid out again
  now += "\n" + rex::cvar::GetFlagByName("cod3_key_cap_scale");  // [claude-9b-think] the caps' room: laid out again
  now += "\nuse " + cod3::use_hold::SettingsKey();  // [acct2-decomp] "Press" or "Press and hold"; [acct2-ocr] every player's
  std::lock_guard lock(mutex);
  if (now != last) {
    last = std::move(now);
    g_bind_gen.fetch_add(1, std::memory_order_relaxed);
  }
}

// The first controller's state, from whichever read saw it: the game's own
// pad reads (WatchPad) or [acct2-decomp] the cutscene skip's (NotePadState;
// the game may not read its pads while a briefing is the loading screen).
// Both see the same pad, so one set of "last" values serves both.
void WatchPadValues(uint16_t buttons, const uint8_t triggers[2], const int16_t axes[4]) {
  static std::mutex mutex;
  static uint16_t last_buttons = 0;
  static uint8_t last_triggers[2] = {};
  static int16_t last_axes[4] = {};
  static uint32_t last_presses = 0;
  static int64_t next_refresh = 0;
  std::lock_guard lock(mutex);
  const int64_t now = NowMs();
  if (now >= next_refresh) {
    RefreshSettings();
    next_refresh = now + 1000;
  }
  bool pad = (buttons & ~last_buttons) != 0;
  for (int i = 0; i < 2; ++i) pad |= triggers[i] > 30 && last_triggers[i] <= 30;
  // Sticks only from well off center (no drift).
  for (int i = 0; i < 2; ++i) pad |= std::abs(axes[i]) > 16000 && std::abs(last_axes[i]) <= 16000;
  // [claude-9b-think] the right stick not while the mouse moves (mouse aim through the pad moves it).
  const bool mouse_moving = now - g_mouse_move_time.load(std::memory_order_relaxed) < 250;
  for (int i = 2; i < 4; ++i) pad |= !mouse_moving && std::abs(axes[i]) > 16000 && std::abs(last_axes[i]) <= 16000;
  last_buttons = buttons;
  for (int i = 0; i < 2; ++i) last_triggers[i] = triggers[i];
  for (int i = 0; i < 4; ++i) last_axes[i] = axes[i];
  // A key or mouse press since the last read (or just now) explains the
  // change: the keyboard/mouse driver feeds the same pad state.
  const uint32_t presses = g_kbm_presses.load(std::memory_order_relaxed);
  const bool keyboard = presses != last_presses ||
                        now - g_kbm_time.load(std::memory_order_relaxed) < 250;
  last_presses = presses;
  if (pad && !keyboard) SetDevice(Device::kPad, "a controller");
}

// X_INPUT_STATE in guest memory (big-endian): u32 packet, u16 buttons,
// u8 left / right trigger, s16 LX, LY, RX, RY.
void WatchPad(uint8_t* base, uint32_t state) {
  const uint16_t buttons = ReadU16(base, state + 4);
  const uint8_t triggers[2] = {ReadU8(base, state + 6), ReadU8(base, state + 7)};
  int16_t axes[4];
  for (int i = 0; i < 4; ++i) axes[i] = static_cast<int16_t>(ReadU16(base, state + 8 + 2 * i));
  WatchPadValues(buttons, triggers, axes);
}

// ---------------------------------------------------------------------------
// What the prompts show.
// ---------------------------------------------------------------------------
// The game's own pictures (kXbox), a controller's button names, or keys.
// [claude-9b-think] kPlayStation the PS4/5 style, kPlayStation2 the PS2 style.
enum class Mode : uint8_t { kXbox, kPlayStation, kNintendo, kGameCube, kPlayStation2, kMenuKeys, kPlayKeys };
constexpr uint32_t kModeCount = 7;

bool IsKeys(Mode m) { return m == Mode::kMenuKeys || m == Mode::kPlayKeys; }
// Every mode but the game's own pictures writes names in the text's font.
bool IsText(Mode m) { return m != Mode::kXbox; }
// [claude-9b-think] 2026-10-27 UNTESTED A controller style drawn as pictures of our own (cod3_key_caps on): the room
// the game keeps for a button is a short "{code}" (PictureCode), the overlay draws the picture over it.
bool PadPictures(Mode m) {
  return (m == Mode::kPlayStation || m == Mode::kPlayStation2 || m == Mode::kNintendo || m == Mode::kGameCube) &&
         REXCVAR_GET(cod3_key_caps);
}
// The lines whose names the overlay draws (key caps, mouse and controller pictures): the game draws them invisibly.
bool OverlayNames(Mode m) { return REXCVAR_GET(cod3_key_caps) && (IsKeys(m) || PadPictures(m)); }
// The overlay's word for a mode (button_prompts.h Look).
cod3::prompts::Look LookOf(Mode m) {
  using cod3::prompts::Look;
  switch (m) {
    case Mode::kPlayStation: return Look::kPlayStation;
    case Mode::kPlayStation2: return Look::kPlayStation2;
    case Mode::kNintendo: return Look::kNintendo;
    case Mode::kGameCube: return Look::kGameCube;
    case Mode::kXbox: return Look::kXbox;
    default: return Look::kKeys;
  }
}

// The local client whose text is being made or drawn now.
uint32_t PromptPlayer(uint8_t* base) {
  const uint32_t p = ReadU32(base, bp::kClientActiveIndex);
  return p < 4 ? p : 0;
}

// Whether the keyboard and mouse are `player`'s: the campaign's player 1;
// [mp-menus] in cod3mp the player on controller 0 (binds::KeyboardClient).
bool KeyboardPlayer(uint8_t* base, uint32_t player) {
#if defined(COD3_MP)
  return player == cod3::binds::KeyboardClient(base);
#else
  (void)base;
  return player == 0;
#endif
}

// [mp-menus] The controller (guest user) `player` plays with: cod3mp's local
// client's; the campaign's player is on user 0.
uint32_t PadUser(uint8_t* base, uint32_t player) {
#if defined(COD3_MP)
  return cod3::binds::ClientPad(base, player);
#else
  (void)base, (void)player;
  return 0;
#endif
}

// [mp-menus] cod3_glyph_style auto: how `player`'s own controller labels its
// buttons. A pad the SDK can't name (or none, or the keyboard alone) gets
// the game's pictures.
Mode UserPadMode(uint32_t user);
Mode PadMode(uint8_t* base, uint32_t player) { return UserPadMode(PadUser(base, player)); }

// [acct2-decomp] The same for a controller (guest user) itself: PadMode's
// part that needs no guest memory, so the overlay can ask (FirstPlayerLook).
Mode UserPadMode(uint32_t user) {
  using rex::input::ControllerFamily;
  auto* input = g_input_system.load(std::memory_order_acquire);
  const ControllerFamily family = input ? input->PadFamily(user) : ControllerFamily::kUnknown;
  static std::atomic<uint8_t> last[4];  // ControllerFamily; static: starts at kUnknown
  if (user < 4 && last[user].exchange(uint8_t(family), std::memory_order_relaxed) != uint8_t(family)) {
    const char* kind = family == ControllerFamily::kPlayStation ? "a PlayStation"
                       : family == ControllerFamily::kNintendo  ? "a Nintendo"
                       : family == ControllerFamily::kXbox      ? "an Xbox"
                                                                : "an unnamed";
    REXLOG_INFO("cod3: controller {} is {} pad: its player's prompts show {} buttons", user + 1, kind,
                family == ControllerFamily::kPlayStation ? "PlayStation"
                : family == ControllerFamily::kNintendo  ? "Nintendo"
                                                         : "Xbox");
  }
  switch (family) {
    case ControllerFamily::kPlayStation: return Mode::kPlayStation;
    case ControllerFamily::kNintendo: return Mode::kNintendo;
    default: return Mode::kXbox;
  }
}

Mode Wanted(uint8_t* base, uint32_t player) {
  if (REXCVAR_GET(cod3_key_prompts) && g_mnk_mode.load(std::memory_order_relaxed) &&
      KeyboardPlayer(base, player) && g_device.load(std::memory_order_relaxed) == Device::kKeyboard) {
    // MenuActive() is false when cod3_pc_menus is off: then the menus take
    // the keybinds too.
    return cod3::pcmenu::MenuActive() ? Mode::kMenuKeys : Mode::kPlayKeys;
  }
  switch (g_style.load(std::memory_order_relaxed)) {
    case Style::kXbox: return Mode::kXbox;
    case Style::kPlayStation: return Mode::kPlayStation;
    case Style::kPlayStation2: return Mode::kPlayStation2;  // [claude-9b-think]
    case Style::kNintendo: return Mode::kNintendo;
    case Style::kGameCube: return Mode::kGameCube;
    default: return PadMode(base, player);
  }
}

// One button of the game's button font. Data, so other styles and pictures
// can be added here: the game's character, the keybind (play), the key the
// PC menus take (nullptr: the keybind there too) and the names per style
// (nullptr: that controller has no such button; the Xbox name shows).
// [mp-menus] Nintendo: the label on the button in that place (the SDK reads
// pads by position: the bottom button is the game's A, labeled B on a Switch
// pad).
// [claude-9b-think] 2026-10-27 two PlayStation styles (owner 12:33): PS4/5 (playstation: OPTIONS / SHARE; the SDK
// doesn't tell a DualSense, CREATE, from a DualShock 4) and PS2 (playstation2: START / SELECT). GameCube's C-stick
// for the right stick.
struct Button {
  char glyph;
  int bind;  // index into kBindCvars, -1: made of the D-pad binds
  const char* menu_key;
  const char* xbox;
  const char* playstation;   // PS4/5
  const char* playstation2;  // [claude-9b-think] PS2
  const char* nintendo;
  const char* gamecube;
};
constexpr Button kButtons[] = {
    {'"', 0, "ENTER", "A", "CROSS", "CROSS", "B", "A"},
    {'%', 1, "ESC", "B", "CIRCLE", "CIRCLE", "A", "B"},
    {'#', 2, nullptr, "X", "SQUARE", "SQUARE", "Y", "X"},
    {'$', 3, nullptr, "Y", "TRIANGLE", "TRIANGLE", "X", "Y"},
    {'&', 4, nullptr, "LT", "L2", "L2", "ZL", "L"},
    {'!', 5, nullptr, "RT", "R2", "R2", "ZR", "R"},
    {'\'', 6, nullptr, "LB", "L1", "L1", "L", nullptr},
    {'(', 7, nullptr, "RB", "R1", "R1", "R", "Z"},
    {'-', 8, "ENTER", "START", "OPTIONS", "START", "+", "START"},
    {'1', 9, nullptr, "BACK", "SHARE", "SELECT", "-", nullptr},
    {'2', 10, nullptr, "LS", "L3", "L3", nullptr, nullptr},
    {'3', 11, nullptr, "RS", "R3", "R3", nullptr, "C"},
    {')', 12, "UP", "UP", "UP", "UP", "UP", "UP"},
    {',', 13, "DOWN", "DOWN", "DOWN", "DOWN", "DOWN", "DOWN"},
    {'+', 14, "LEFT", "LEFT", "LEFT", "LEFT", "LEFT", "LEFT"},
    {'*', 15, "RIGHT", "RIGHT", "RIGHT", "RIGHT", "RIGHT", "RIGHT"},
    {'/', -1, "UP/DOWN", "UP/DOWN", "UP/DOWN", "UP/DOWN", "UP/DOWN", "UP/DOWN"},
    {'.', -1, "LEFT/RIGHT", "LEFT/RIGHT", "LEFT/RIGHT", "LEFT/RIGHT", "LEFT/RIGHT", "LEFT/RIGHT"},
    {'0', -1, "ARROWS", "D-PAD", "D-PAD", "D-PAD", "D-PAD", "D-PAD"},
};
constexpr int kButtonCount = int(sizeof(kButtons) / sizeof(kButtons[0]));

// [keybinds] The game's key number of a button character (its pad poll's
// keys, game/keybinds/binds.cpp), 0 for the combined D-pad ones.
int PadKeyFor(char glyph) {
  switch (glyph) {
    case '"': return 212;  // A
    case '%': return 213;  // B
    case '#': return 211;  // X
    case '$': return 214;  // Y
    case '&': return 216;  // LT
    case '!': return 215;  // RT
    case '\'': return 218;  // LB
    case '(': return 217;  // RB
    case '-': return 27;   // START
    case '1': return 9;    // BACK
    case '2': return 220;  // LS
    case '3': return 219;  // RS
    case ')': return 154;  // D-pad up
    case ',': return 155;  // D-pad down
    case '+': return 156;  // D-pad left
    case '*': return 157;  // D-pad right
    default: return 0;
  }
}

// A key name of the keybind settings (rex/ui/keybinds.h) as the prompt shows
// it: "Escape" -> "ESC", "Shift+Up" -> "SHIFT+UP", "r" -> "R".
std::string KeyName(std::string_view key) {
  struct Name {
    const char* from;
    const char* to;
  };
  static constexpr Name kNames[] = {
      {"Escape", "ESC"},        {"Return", "ENTER"},     {"NumpadEnter", "ENTER"},
      {"Control", "CTRL"},      {"Delete", "DEL"},       {"Insert", "INS"},
      {"PageUp", "PGUP"},       {"PageDown", "PGDN"},    {"Backtick", "`"},
      {"Minus", "-"},           {"Plus", "="},           {"Comma", ","},
      {"Period", "."},          {"Semicolon", ";"},      {"Slash", "/"},
      {"Backslash", "\\"},      {"LBracket", "["},       {"RBracket", "]"},
      {"Quote", "'"},           {"CapsLock", "CAPS"},    {"ScrollLock", "SCRLK"},
      {"PrintScreen", "PRTSC"}, {"NumpadPlus", "NUM+"},  {"NumpadMinus", "NUM-"},
      {"NumpadStar", "NUM*"},   {"NumpadSlash", "NUM/"}, {"MB4", "MOUSE4"},
      {"MM4", "MOUSE4"},        {"MB5", "MOUSE5"},       {"MM5", "MOUSE5"},
  };
  for (const auto& n : kNames)
    if (SameText(key, n.from)) return n.to;
  std::string out(key);
  if (out.size() == 7 && SameText(out.substr(0, 6), "Numpad")) return "NUM" + out.substr(6);
  for (char& c : out) c = char(std::toupper(uint8_t(c)));
  return out;
}

// The first key of a keybind setting ("R,F" -> "R"), "" when unbound.
std::string BindKey(int bind) {
  std::string v = rex::cvar::GetFlagByName(kBindCvars[bind]);
  v = v.substr(0, v.find(','));
  std::string out;
  size_t start = 0;
  while (start <= v.size()) {
    size_t end = v.find('+', start);
    if (end == std::string::npos) end = v.size();
    std::string part = v.substr(start, end - start);
    while (!part.empty() && part.front() == ' ') part.erase(part.begin());
    while (!part.empty() && part.back() == ' ') part.pop_back();
    // [rexglue-native qte] 2026-11-09 each key as the player's keyboard layout labels it (LayoutName).
    if (!part.empty()) out += (out.empty() ? "" : "+") + LayoutName(KeyName(part));
    start = end + 1;
  }
  return out;
}

// Four binds (up, left, down, right) as one name: "WASD" when each is one
// character, else "W/A/S/D"; "" when one is unbound.
std::string FourKeys(int up) {
  std::string keys[4], joined;
  bool single = true;
  for (int i = 0; i < 4; ++i) {
    keys[i] = BindKey(up + i);
    if (keys[i].empty()) return "";
    single &= keys[i].size() == 1;
  }
  for (int i = 0; i < 4; ++i) joined += (i && !single ? "/" : "") + keys[i];
  return joined;
}

std::string NameFor(const Button& b, Mode mode) {
  switch (mode) {
    case Mode::kPlayStation: return b.playstation ? b.playstation : b.xbox;
    case Mode::kPlayStation2: return b.playstation2 ? b.playstation2 : b.xbox;  // [claude-9b-think]
    case Mode::kNintendo: return b.nintendo ? b.nintendo : b.xbox;
    case Mode::kGameCube: return b.gamecube ? b.gamecube : b.xbox;
    case Mode::kXbox: return b.xbox;
    default: break;
  }
  if (mode == Mode::kMenuKeys && b.menu_key) return b.menu_key;
  if (b.bind >= 0) {
    std::string key = BindKey(b.bind);
    return key.empty() ? b.xbox : key;
  }
  // Up/down, left/right, the whole D-pad: the D-pad binds joined.
  static constexpr int kUpDown[] = {0, 1}, kLeftRight[] = {2, 3}, kAll[] = {0, 1, 2, 3};
  const int* parts = b.glyph == '/' ? kUpDown : b.glyph == '.' ? kLeftRight : kAll;
  const int count = b.glyph == '0' ? 4 : 2;
  std::string out;
  for (int i = 0; i < count; ++i) {
    std::string key = BindKey(kBindDpadUp + parts[i]);
    if (key.empty()) return b.xbox;
    out += (i ? "/" : "") + key;
  }
  return out;
}

// [claude-9b-think] 2026-10-27 UNTESTED The picture a controller style shows for a button (PadPictures), as the code
// the game lays out between braces, "{x}"; ui/hud_overlay.cpp DrawPromptPicture draws it. The D-pad is our own arrows
// ("^" up, "v" down, "<" left, ">" right; "^v" up/down, "<>" left/right, "<^v>" all of it), PlayStation's face
// buttons shapes ("x" cross, "o" circle, "s" square, "t" triangle), every other button its name on a cap ("L1",
// "OPTIONS", "SELECT"; GameCube "A" ... "Z", "C" the C-stick; Nintendo "B" ... "ZR", "+", "-").
std::string PictureCode(const Button& b, Mode mode) {
  switch (b.glyph) {
    case ')': return "^";
    case ',': return "v";
    case '+': return "<";
    case '*': return ">";
    case '/': return "^v";
    case '.': return "<>";
    case '0': return "<^v>";
    default: break;
  }
  if (mode == Mode::kPlayStation || mode == Mode::kPlayStation2) {
    switch (b.glyph) {
      case '"': return "x";
      case '%': return "o";
      case '#': return "s";
      case '$': return "t";
      default: break;
    }
  }
  return NameFor(b, mode);
}

// [claude-9b-think] 2026-10-27 UNTESTED (owner 12:28: "the glyphs for mouse1 and mouse2 seem too big": the game kept
// "[MOUSE1]"'s width for the cap). A mouse button is a small mouse picture in a room about as wide as tall: "{1}" ..
// "{5}" the buttons (4 back, 5 forward), "{U}" / "{D}" the wheel up / down (DrawPromptPicture). The names are the
// keybind settings' (LMB, MB4 ...) and the port's own (MOUSE1, MWHEELUP ...; game/keybinds/keys.cpp). Other keys,
// and every key while cod3_key_caps is off, keep "[NAME]".
const char* MouseCode(std::string_view name) {
  struct Mouse {
    const char* name;
    const char* code;
  };
  static constexpr Mouse kMouse[] = {
      {"LMB", "1"},    {"MOUSE1", "1"}, {"MB1", "1"},    {"RMB", "2"},       {"MOUSE2", "2"},      {"MB2", "2"},
      {"MMB", "3"},    {"MOUSE3", "3"}, {"MB3", "3"},    {"MOUSE4", "4"},    {"MB4", "4"},         {"MM4", "4"},
      {"MOUSE5", "5"}, {"MB5", "5"},    {"MM5", "5"},    {"MWHEELUP", "U"},  {"MWHEELDOWN", "D"},
  };
  for (const auto& m : kMouse)
    if (SameText(name, m.name)) return m.code;
  return nullptr;
}
std::string RoomText(const std::string& name, bool keys) {
  if (keys && REXCVAR_GET(cod3_key_caps))
    if (const char* code = MouseCode(name)) return std::string("{") + code + "}";
  return "[" + name + "]";
}

// ---------------------------------------------------------------------------
// [claude-9b-think] 2026-10-27 UNTESTED Room as wide as the cap (owner 15:01: "the hint spacing is off with it's new
// size. Could we allow the user to enlarge it if desired? we would need to account for new breaklines etc").
//
// The game keeps for each name the width of the string we hand it, laid out in the line's font at the text's scale
// (the line layout, the word wrap's measure and the token pass below). That was "[NAME]" in the game's font, wider
// than the smaller key cap the overlay draws now (TAB: ~80 px of room for a ~41 px cap at 1280x720), so the words
// stood off the caps. While the overlay draws the names (OverlayNames), the string is now "[" filler "]" whose width
// in the line's font is the cap's own (FittedRoom), and the names, in their order, are kept for the line
// (g_line_names), so the overlay knows what each room is for and draws the cap across the whole room. The cap size
// (cod3_key_cap_scale) scales the room too, so the game's own word wrap moves words for bigger caps.
//
// The cap, in the line's height h (ui/hud_overlay.cpp DrawPromptKeyCaps): 0.611 h tall at scale 1 (1.3 times the
// capitals, 0.47 h), our name in Barlow Semi Condensed with 0.3 of the cap's height of margin on each side, at least
// square. In the font's own units (the text drawn as wide as tall on the screen, as the HUD's text is): width =
// max(0.5 x the name's width in the game's font + 2 x 0.183 h, 0.611 h) x the scale; one-character pictures (a mouse
// button, a PlayStation shape, a D-pad arrow) square. 0.5: TAB measured ~29 px on the cap against ~60 px in the game's
// font (c9b-test-pr-look-s40). The overlay narrows the name if a font's letters run wider than that.
constexpr float kCapLetters = 0.5f;
constexpr float kCapMargin = 0.183f;
constexpr float kCapTall = 0.611f;

// The font names are laid out in right now (the line layout and the word measure set it), its index, and whether this
// is a line's layout (not a word measure), whose names are kept in order.
thread_local uint32_t t_name_font = 0;
thread_local uint32_t t_name_font_index = 0;
thread_local bool t_line_layout = false;
thread_local std::vector<std::string> t_line_names;

float KeyCapScale() { return std::clamp(float(REXCVAR_GET(cod3_key_cap_scale)), 0.8f, 1.8f); }

// A character's advance in `font` (the layout's pen step; a space steps like an 'a'), or -1 outside the font.
int32_t Advance(uint8_t* base, uint32_t font, uint8_t c) {
  namespace pm = cod3::addr::pc_menu;
  const int32_t first = int32_t(ReadU32(base, font + pm::kFontFirstChar));
  const int32_t count = int32_t(ReadU32(base, font + pm::kFontGlyphCount));
  const uint32_t glyphs = ReadU32(base, font + pm::kFontGlyphs);
  if (c == ' ') c = 'a';
  if (count <= 0 || !Valid(glyphs) || int32_t(c) < first || int32_t(c) >= first + count) return -1;
  return int32_t(ReadU32(base, glyphs + pm::kGlyphSize * uint32_t(int32_t(c) - first) + 4 * 6));
}

// "[filler]" (or "{filler}" for a picture) as wide in `font` as the cap for `name`; "" when the font can't tell.
std::string FittedRoom(uint8_t* base, uint32_t font, const std::string& name, bool picture) {
  namespace pm = cod3::addr::pc_menu;
  const float line = float(int32_t(ReadU32(base, font + pm::kFontLineHeight)));
  if (!(line > 0.0f && line < 512.0f)) return "";
  float letters = 0.0f;
  for (char c : name) {
    const int32_t a = Advance(base, font, uint8_t(c));
    letters += float(std::max(a, 0));
  }
  const bool square = picture && name.size() == 1;
  const float want = KeyCapScale() * (square ? kCapTall * line
                                             : std::max(kCapLetters * letters + 2.0f * kCapMargin * line,
                                                        kCapTall * line));
  const char open = picture ? '{' : '[', close = picture ? '}' : ']';
  const int32_t a_open = Advance(base, font, uint8_t(open)), a_close = Advance(base, font, uint8_t(close));
  if (a_open < 0 || a_close < 0) return "";
  // The filler: characters of the font, widest first, each while it still fits (to within half the narrowest's
  // width); no spaces (the word wrap breaks there), no '~' (a token), no brackets.
  static constexpr char kFill[] = "MWmw@#HNQOGDUCBRKAESVXYZPTLFJ0123456789abdeghnopqsuvxyzckrtfIijl|!.,:;'";
  struct Piece {
    char c;
    int32_t a;
  };
  Piece pieces[sizeof(kFill)];
  int count = 0;
  int32_t narrowest = INT32_MAX;
  for (const char* p = kFill; *p; ++p) {
    const int32_t a = Advance(base, font, uint8_t(*p));
    if (a <= 0) continue;
    pieces[count++] = {*p, a};
    narrowest = std::min(narrowest, a);
  }
  if (!count) return "";
  std::sort(pieces, pieces + count, [](const Piece& x, const Piece& y) { return x.a > y.a; });
  std::string out(1, open);
  float left = want - float(a_open + a_close);
  const float slack = float(narrowest) * 0.5f;
  while (left > slack && out.size() < 29) {
    const Piece* pick = nullptr;
    for (int i = 0; i < count && !pick; ++i)
      if (float(pieces[i].a) <= left + slack) pick = &pieces[i];
    if (!pick) break;
    out += pick->c;
    left -= float(pick->a);
  }
  if (out.size() == 1) out += pieces[count - 1].c;  // at least one (the line's draw note skips "[]")
  out += close;
  return out;
}

// The string the game lays out for a name: the fitted room while the overlay draws the names and the layout's font is
// known (a line's names kept in order), else as before. `shown` is "[NAME]" or "{code}".
std::string LaidOutName(uint8_t* base, const std::string& shown, Mode mode) {
  if (!t_name_font || !OverlayNames(mode) || shown.size() < 3) return shown;
  const bool picture = shown.front() == '{';
  const std::string name = shown.substr(1, shown.size() - 2);
  std::string room = FittedRoom(base, t_name_font, name, picture);
  if (room.empty()) return shown;
  if (t_line_layout && t_line_names.size() < 64) t_line_names.push_back(name);
  return room;
}

// Guest memory for our strings: one 32-byte slot per button (+1 for "[?]")
// per mode, and a 256-byte text buffer. [mp-menus] Per mode, because a laid
// out line keeps pointing at its names: split-screen players with different
// controllers (or one on the keyboard) each keep their own.
// [claude-9b-think] and per font (the fitted rooms differ by font; kFontCount: none known).
constexpr uint32_t kSlotSize = 32;
constexpr uint32_t kTextSize = 256;
constexpr uint32_t kSlotsPerMode = uint32_t(kButtonCount) + 1;
constexpr uint32_t kFontSlots = bp::kFontCount + 1;
constexpr uint32_t kNameSlots = kSlotsPerMode * kModeCount * kFontSlots;
uint32_t g_guest = 0;

uint32_t GuestScratch() {
  if (!g_guest) {
    auto* mem = rex::system::kernel_state()->memory();
    g_guest = mem->SystemHeapAlloc(kSlotSize * kNameSlots + kTextSize);
  }
  return g_guest;
}

// The guest string "[NAME]" for a button character, or 0.
uint32_t NameText(uint8_t* base, char glyph, Mode mode) {
  const uint32_t scratch = GuestScratch();
  if (!scratch) return 0;
  int index = kButtonCount;
  std::string text = "[?]";
  for (int i = 0; i < kButtonCount; ++i) {
    if (kButtons[i].glyph != glyph) continue;
    index = i;
    // [keybinds] In play, a button the game binds to one of the port's own
    // keyboard actions shows that action's key (the left stick click is
    // +binoculars in layout A: [B]; game/keybinds/binds.h).
    std::string name;
    if (mode == Mode::kPlayKeys) name = PortKeyName(cod3::binds::KeyNameForPadKey(base, PadKeyFor(glyph)));
    if (name.empty()) name = NameFor(kButtons[i], mode);
    // [claude-9b-think] a controller style's picture, or a mouse button's (RoomText).
    text = PadPictures(mode) ? "{" + PictureCode(kButtons[i], mode) + "}" : RoomText(name, IsKeys(mode));
    text = LaidOutName(base, text, mode);  // [claude-9b-think] as wide as its cap
    break;
  }
  if (text.size() > kSlotSize - 1) text.resize(kSlotSize - 1);
  const uint32_t mode_slot = uint32_t(mode) < kModeCount ? uint32_t(mode) : 0;
  const uint32_t font_slot = t_name_font && t_name_font_index < bp::kFontCount ? t_name_font_index : bp::kFontCount;
  const uint32_t at =
      scratch + kSlotSize * ((font_slot * kModeCount + mode_slot) * kSlotsPerMode + uint32_t(index));
  std::memcpy(base + at, text.c_str(), text.size() + 1);
  return at;
}

// [acct2-decomp] A key name written into a text by us (stick words "Use
// [WASD]", an action's key "Press [B]", the vehicle view switch): a token of
// our own, "~\x02WASD\x02", which the game's token pass (0x824C68E0, hooked
// below) takes like one of its "~name" buttons. So it is laid out and drawn
// as a name (a key cap, KeyCap), and its text is remembered and laid out
// again when the device changes, as the game's own buttons are.
constexpr char kKeyTokenMark = '\x02';
std::string KeyToken(const std::string& name) {
  return std::string("~") + kKeyTokenMark + name + kKeyTokenMark;
}
// The guest "[NAME]" string of a key token at `text` ("~\x02...\x02") and the
// token's length, or 0. A ring of strings: the layout copies each into its
// line at once.
uint32_t KeyTokenText(uint8_t* base, uint32_t text, uint32_t* length) {
  if (base[text] != '~' || base[text + 1] != kKeyTokenMark) return 0;
  std::string name;
  uint32_t i = 2;
  for (; i < 2 + kSlotSize; ++i) {
    const char c = char(base[text + i]);
    if (c == kKeyTokenMark || c == '\0') break;
    name += c;
  }
  if (base[text + i] != kKeyTokenMark || name.empty()) return 0;
  constexpr uint32_t kRing = 64;
  static uint32_t ring = 0;
  static std::atomic<uint32_t> next{0};
  if (!ring) ring = rex::system::kernel_state()->memory()->SystemHeapAlloc(kRing * kSlotSize);
  if (!ring) return 0;
  std::string shown = RoomText(name, true);  // [claude-9b-think] a mouse button's picture
  shown = LaidOutName(base, shown, Mode::kPlayKeys);  // [claude-9b-think] as wide as its cap (only keys make these)
  if (shown.size() > kSlotSize - 1) shown.resize(kSlotSize - 1);
  const uint32_t at = ring + kSlotSize * (next.fetch_add(1, std::memory_order_relaxed) % kRing);
  std::memcpy(base + at, shown.c_str(), shown.size() + 1);
  *length = i + 1;
  return at;
}

// A guest copy of a text for the game to read right away (one at a time).
uint32_t TempText(uint8_t* base, const std::string& text) {
  const uint32_t scratch = GuestScratch();
  if (!scratch || text.size() >= kTextSize) return 0;
  const uint32_t at = scratch + kSlotSize * kNameSlots;  // [claude-9b-think] after the name slots (per font now)
  std::memcpy(base + at, text.c_str(), text.size() + 1);
  return at;
}

// The game's own words (Localize), cached.
std::string GameText(PPCContext& ctx, uint8_t* base, const std::string& key) {
  static std::mutex mutex;
  static std::unordered_map<std::string, std::string> cache;
  static uint32_t cached_language = ~0u;
  uint32_t language = ReadU32(base, bp::kLanguageMode);
  language ^= cod3::text::Generation() << 8;  // [acct2-ocr] a text language change empties the cache too
  {
    std::lock_guard lock(mutex);
    if (language != cached_language) cache.clear(), cached_language = language;
    if (auto it = cache.find(key); it != cache.end()) return it->second;
  }
  const uint32_t tables = ReadU32(base, bp::kStringTables);
  if (!Valid(tables)) return "";
  Call c(ctx, base);
  const uint32_t k = c.PushString(key);
  const uint32_t text = uint32_t(c.Invoke(bp::kLocalize, {tables, k}));
  std::string out = Valid(text) ? ReadString(base, text, 128) : "";
  if (out == key) out.clear();  // missing
  std::lock_guard lock(mutex);
  cache[key] = out;
  return out;
}

// [acct2-decomp] 2026-11-05 (owner 08:50: "Please make the hints reflect this change"): with a Use hold of 0
// (cod3_use_hold_ms, game/use_hold.h) a text's "Press and hold [USE]" says "Press [USE]" and the campaign's
// "[HOLD_USE]" word "[VERB_USE]", in the game's own words; done before the game's "[ACTION]" pass, so each
// player's own button or key goes in as before. The text itself when the hold isn't 0.
std::string UseHoldWording(PPCContext& ctx, uint8_t* base, const std::string& text) {
  if (text.find("USE]") == std::string::npos || cod3::use_hold::CurrentMs(base) != 0) return text;
  return cod3::use_hold::Pressed(text, GameText(ctx, base, "INGAME_PRESS_AND_HOLD"),
                                 GameText(ctx, base, "INGAME_PRESS"));
}

// [acct2-decomp] A guest copy of a text for one call of the game (a few in turn, so a call made inside another
// keeps the outer one's); 0 when it doesn't fit.
uint32_t CallText(uint8_t* base, const std::string& text) {
  constexpr uint32_t kSize = 2048, kCount = 4;
  static const uint32_t ring = rex::system::kernel_state()->memory()->SystemHeapAlloc(kSize * kCount);
  static std::atomic<uint32_t> next{0};
  if (!ring || text.size() >= kSize) return 0;
  const uint32_t slot = ring + kSize * (next.fetch_add(1, std::memory_order_relaxed) % kCount);
  std::memcpy(base + slot, text.c_str(), text.size() + 1);
  return slot;
}

// ---------------------------------------------------------------------------
// Layout state (the game thread lays out and draws its texts).
// ---------------------------------------------------------------------------
thread_local int t_set_depth = 0;            // inside a set-text call
thread_local Mode t_set_mode = Mode::kXbox;  // and what it lays out
thread_local bool t_names = false;           // the layout in progress writes names
thread_local Mode t_name_mode = Mode::kPlayKeys;
thread_local uint32_t t_tokens = 0;          // button tokens laid out so far

std::mutex g_mutex;
// Lines laid out with names: they are drawn with the text font too. [acct2-
// decomp] The mode they were laid out in: keys are drawn as key caps.
std::unordered_map<uint32_t, Mode> g_text_lines;
// [claude-9b-think] The names of a line laid out with rooms as wide as their caps (FittedRoom), in its buttons' order.
std::unordered_map<uint32_t, std::vector<std::string>> g_line_names;
// The last set-text call of each text that had buttons, to lay it out again.
struct TextCall {
  uint32_t slot = 0;        // the virtual slot called (bp::kSet*)
  bool string_arg = false;  // takes a String** (else a const char*)
  std::string text;         // before the game's "[ACTION]" pass
  uint32_t width = 0;
  Mode mode = Mode::kXbox;
  uint32_t gen = 0;         // g_bind_gen it was laid out with
  uint32_t player = 0;      // the local client it was made for
};
std::unordered_map<uint32_t, TextCall> g_texts;

Mode LayoutMode(uint8_t* base) { return t_set_depth ? t_set_mode : Wanted(base, PromptPlayer(base)); }

// [acct2-decomp] Key caps (button_prompts.h KeyCap): each line drawn with
// keys notes where its key names are, in menu space; the overlay draws them.
struct DrawnCaps {
  int64_t ms = 0;  // when the line was drawn
  std::vector<cod3::prompts::KeyCap> caps;
  // [claude-9b-think] 2026-10-27 UNTESTED the game frame (pc_menu::kFrameCount) it was drawn in, and how many game
  // frames since its draw before (its cadence: 1 when drawn every frame).
  uint32_t frame = 0;
  uint32_t cadence = 1;
};
std::mutex g_cap_mutex;
std::unordered_map<uint32_t, DrawnCaps> g_caps;  // by line
// [claude-9b-think] 2026-10-27 UNTESTED (owner 10:27: "keycap glyphs aren't fading out in time with their objectives
// at the same time, It's a little delayed"). A cap stayed on screen for 50 ms of the clock after its line's last draw
// (CurrentKeyCaps(50)): ten frames at 200 fps at the line's last opacity, and as long for a line laid out again at
// another address (its old entry stayed beside the new one, the older and more opaque cap on top during a fade). Now
// a cap shows only while its line was drawn in the game's newest frame or within its own cadence of it.
std::atomic<uint32_t> g_cap_latest_frame{0};  // the newest game frame a line was drawn in (or the pads read)
void NoteGameFrame(uint8_t* base) {
  const uint32_t frame = ReadU32(base, cod3::addr::pc_menu::kFrameCount);
  uint32_t seen = g_cap_latest_frame.load(std::memory_order_relaxed);
  while (int32_t(frame - seen) > 0 &&
         !g_cap_latest_frame.compare_exchange_weak(seen, frame, std::memory_order_relaxed)) {
  }
}

// The key names of a line laid out with keys, as the line draw places them:
// segment 2k is button k (from its x offset to the next segment's), the line
// starts at its x / y and is the line font's height times the y scale tall.
// The segment table: s16 pairs {string index, x offset} (addresses.h
// pc_menu::kLineSegments; pc_menu.cpp's clickable footers read it the same way).
// [claude-9b-think] 2026-10-28 UNTESTED (owner 06:43 / 06:46: "QTE events are no longer showing keybind glyphs", "The
// Keycaps never showed. I had to toggle them back to controller buttons to see anything"). A battle action's button
// prompt is a line with a button and no words of its own. The cap took the fainter of the text color's and the button
// color's opacity (the 10-26 fade fix), and such a line need not give its text color any opacity: the controller's
// picture is drawn in the button color alone and showed, the cap at 0 did not. Now a line with no visible characters
// besides its names takes the button color's opacity (a line with words still takes the fainter of the two, so a fading
// hint takes its caps along). And a name that ends its line got its right edge from the line's width (+16); where that
// is not past the name's start, the room's own width in the line's font at the x scale is used (else no cap at all).
float RoomWidth(uint8_t* base, uint32_t font, const std::string& room, float x_scale) {
  float width = 0.0f;
  for (char c : room) width += float(std::max(Advance(base, font, uint8_t(c)), 0));
  return width * x_scale;
}
// [claude-9b-think] 2026-10-28 UNTESTED (owner: "the way the glyphs are being mapped in the hints isn't consistently
// centered in the gap"). The cap was centered in its room: the stretch of the line the game keeps for the name, from
// the name's x offset to the next segment's (both fixed when the line is laid out). What the reader sees as the gap is
// something else: from the ink of the word before to the ink of the word after, drawn by the game at the draw's x
// scale. The two agree only when the line is drawn at the scale it was laid out at and the neighbours' spaces and side
// bearings match. A hint drawn larger or smaller than its layout (the draw's f2 against the layout's f1: offsets stay,
// words grow or shrink from their starts) moves the visible gap off the room's middle by a share of the word before's
// width, so the error differs from hint to hint and with each word's length. Now each cap with words on both sides is
// moved to the middle of that visible gap, measured from the font's own glyphs (x offset, width, advance) at the
// draw's x scale; a cap at a line's start or end keeps its room.
struct GlyphBox {
  int32_t xoff = 0, width = 0, advance = 0;
};
// A character's glyph in `font` (font units); a space measures as an 'a' (the game's layout) and has no ink.
bool Glyph(uint8_t* base, uint32_t font, uint8_t c, GlyphBox* g) {
  namespace pm = cod3::addr::pc_menu;
  const int32_t first = int32_t(ReadU32(base, font + pm::kFontFirstChar));
  const int32_t count = int32_t(ReadU32(base, font + pm::kFontGlyphCount));
  const uint32_t glyphs = ReadU32(base, font + pm::kFontGlyphs);
  const uint8_t look = c == ' ' ? uint8_t('a') : c;
  if (count <= 0 || !Valid(glyphs) || int32_t(look) < first || int32_t(look) >= first + count) return false;
  const uint32_t at = glyphs + pm::kGlyphSize * uint32_t(int32_t(look) - first);
  g->width = int32_t(ReadU32(base, at + 8));
  g->xoff = int32_t(ReadU32(base, at + 16));
  g->advance = int32_t(ReadU32(base, at + 24));
  return true;
}
// The ink of `text` drawn from 0 at `x_scale`: its first visible glyph's left edge and its last one's right edge.
// False when it has none (spaces only) or a character the font can't tell.
bool InkSpan(uint8_t* base, uint32_t font, const std::string& text, float x_scale, float* left, float* right) {
  float pen = 0.0f;
  bool any = false;
  for (char ch : text) {
    GlyphBox g;
    if (!Glyph(base, font, uint8_t(ch), &g)) return false;
    if (ch != ' ' && ch != '\t' && g.width > 0) {
      const float l = (pen + float(g.xoff)) * x_scale, r = (pen + float(g.xoff + g.width)) * x_scale;
      *left = any ? std::min(*left, l) : l;
      *right = any ? std::max(*right, r) : r;
      any = true;
    }
    pen += float(g.advance);
  }
  return any;
}
// [acct2-ocr] 2026-11-05 (owner 07:46: "double check the keys don't overlap with the text"): a piece with a character
// past Latin-1 (Russian, Chinese ...) is not drawn from the game font's glyphs (ui/text_native.cpp draws it, and the
// layout measured it with our widths), so InkSpan's reading of its bytes in the game font is meaningless: zh-Hans moved
// [ESC] 6.6 units into the next word. The room is right as it is for such a line.
bool BeyondLatin1(const std::string& text) {
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    const uint8_t c = uint8_t(text[i]);
    if (c >= 0xC4 && c < 0xF5 && (uint8_t(text[i + 1]) & 0xC0) == 0x80) return true;  // a UTF-8 lead of U+0100+
  }
  return false;
}
void NoteKeyCaps(uint8_t* base, uint32_t line, uint32_t font, float x_scale, float y_scale, float text_alpha,
                 float button_alpha, Mode mode) {
  namespace pm = cod3::addr::pc_menu;
  const int32_t n = int32_t(ReadU32(base, line + pm::kLineSegmentCount));
  const uint32_t segments = ReadU32(base, line + pm::kLineSegments);
  const uint32_t string = ReadU32(base, line + pm::kLineString);
  DrawnCaps drawn;
  drawn.ms = NowMs();
  drawn.frame = ReadU32(base, pm::kFrameCount);  // [claude-9b-think]
  NoteGameFrame(base);
  // [claude-9b-think] the line's names when its rooms are as wide as their caps (FittedRoom), in its buttons' order.
  std::vector<std::string> fitted_names;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_line_names.find(line); it != g_line_names.end()) fitted_names = it->second;
  }
  size_t button = 0;
  bool has_text = true;  // [claude-9b-think] visible characters besides the names (unknown: as if there were)
  float max_shift = 0.0f;  // [claude-9b-think] the largest move into a visible gap's middle (menu units)
  if (n > 0 && n <= 64 && Valid(segments) && Valid(string)) {
    const std::string text = ReadString(base, string + bp::kStringChars, 512);
    std::string words = text;  // [claude-9b-think] the line with its names blanked out
    const float x = cod3::guest::ReadF32(base, line + pm::kLineX);
    const float y = cod3::guest::ReadF32(base, line + pm::kLineY);
    const float width = cod3::guest::ReadF32(base, line + pm::kLineWidth);
    const float height = float(int32_t(ReadU32(base, font + pm::kFontLineHeight))) * y_scale;
    auto entry = [&](int i, int field) {
      return int32_t(int16_t(ReadU16(base, segments + 4 * uint32_t(i) + 2 * uint32_t(field))));
    };
    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(width) && height > 0.0f && height < 480.0f) {
      for (int k = 0; k < n; k += 2) {
        const int32_t start = entry(k, 0);
        const int32_t end = k + 1 < n ? entry(k + 1, 0) : int32_t(text.size());
        if (start < 0 || end <= start + 2 || end > int32_t(text.size())) continue;
        std::string name = text.substr(size_t(start), size_t(end - start));
        // [claude-9b-think] "[NAME]" a key cap, "{code}" a picture of our own (PictureCode, MouseCode).
        const bool picture = name.front() == '{' && name.back() == '}';
        if (!picture && (name.front() != '[' || name.back() != ']')) continue;  // not one of our names
        const std::string room = name;  // [claude-9b-think] as the game laid it out (RoomWidth)
        words.replace(size_t(start), room.size(), room.size(), ' ');
        name = name.substr(1, name.size() - 2);
        // [claude-9b-think] a fitted room: the name kept for it at the layout; the cap fills the room.
        const size_t ordinal = button++;
        const bool fitted = ordinal < fitted_names.size();
        if (fitted) name = fitted_names[ordinal];
        float x0 = x + float(entry(k, 1));
        float x1 = k + 1 < n ? x + float(entry(k + 1, 1)) : x + width;
        if (!(x1 > x0) && k + 1 >= n) x1 = x0 + RoomWidth(base, font, room, x_scale);  // [claude-9b-think]
        if (!(x1 > x0)) continue;
        // [claude-9b-think] 2026-10-28 UNTESTED the middle of the visible gap (InkSpan): the word before (the text
        // from the previous segment, or from the line's start, to this name) and the word after (the next segment).
        if (x_scale > 0.0f && k + 1 < n) {
          const int32_t before_at = k >= 1 ? entry(k - 1, 0) : 0;
          const float before_x = k >= 1 ? x + float(entry(k - 1, 1)) : x;
          const int32_t after_end = k + 2 < n ? entry(k + 2, 0) : int32_t(text.size());
          float bl = 0.0f, br = 0.0f, al = 0.0f, ar = 0.0f;
          if (before_at >= 0 && before_at < start && after_end > end && after_end <= int32_t(text.size()) &&
              !BeyondLatin1(text) &&  // [acct2-ocr] our glyphs, not the game font's
              InkSpan(base, font, text.substr(size_t(before_at), size_t(start - before_at)), x_scale, &bl, &br) &&
              InkSpan(base, font, text.substr(size_t(end), size_t(after_end - end)), x_scale, &al, &ar)) {
            const float gap_l = before_x + br, gap_r = x + float(entry(k + 1, 1)) + al;
            const float half = 0.5f * (x1 - x0);
            const float shift = std::clamp(0.5f * (gap_l + gap_r) - 0.5f * (x0 + x1), -half, half);
            if (std::isfinite(shift) && gap_r > gap_l) {
              x0 += shift;
              x1 += shift;
              max_shift = std::max(max_shift, std::fabs(shift));
            }
          }
        }
        drawn.caps.push_back({x0, y, x1, y + height, std::move(name), 0.0f, LookOf(mode), picture, fitted});
      }
    }
    has_text = words.find_first_not_of(" \t\r\n") != std::string::npos;
  }
  // [claude-9b-think] a line of names alone: the button color's opacity; with words, the fainter of the two.
  const float alpha = has_text ? std::min(text_alpha, button_alpha) : button_alpha;
  for (auto& cap : drawn.caps) cap.alpha = alpha;
  std::lock_guard lock(g_cap_mutex);
  if (g_caps.size() > 512) {  // lines that went away: forget the old ones
    const int64_t now = drawn.ms;
    std::erase_if(g_caps, [now](const auto& kv) { return now - kv.second.ms > 1000; });
  }
  // [claude-9b-think] the line's cadence: game frames since its last draw (1 to 8; a line not drawn for longer starts
  // again at 1).
  if (auto it = g_caps.find(line); it != g_caps.end()) {
    const uint32_t since = drawn.frame - it->second.frame;
    drawn.cadence = since >= 1 && since <= 8 ? since : 1;
  }
  if (REXCVAR_GET(cod3_debug_key_caps)) {
    // [claude-9b-think] every draw while the line fades (opacity under 1; up to 400 lines a second), else the first
    // 30 lines a second (the 30-a-second cap alone missed the fades).
    static int64_t window = 0;
    static int lines = 0, fading = 0;
    if (drawn.ms - window >= 1000) window = drawn.ms, lines = 0, fading = 0;
    if (alpha < 0.999f ? fading++ < 400 : lines++ < 30)
      REXLOG_INFO("cod3: key caps: line {:08X} drawn in game frame {} (every {}), {} caps at opacity {:.3f} (text "
                  "{:.3f}, button {:.3f}{}), moved up to {:.2f} units into the gap's middle (x scale {:.3f})",
                  line, drawn.frame, drawn.cadence, drawn.caps.size(), alpha, text_alpha, button_alpha,
                  has_text ? "" : ", names only", max_shift, x_scale);
  }
  g_caps[line] = std::move(drawn);
}

// font* of a font index, 0 for none or the button font.
uint32_t TextFont(uint8_t* base, uint32_t index) {
  if (index >= bp::kFontCount || index == bp::kButtonFont) return 0;
  const uint32_t font = ReadU32(base, bp::kFonts + 4 * index);
  return Valid(font) ? font : 0;
}

// Runs `call` with the button font slot holding `font`.
template <typename F>
void WithButtonFont(uint8_t* base, uint32_t font, F&& call) {
  const uint32_t slot = bp::kFonts + 4 * bp::kButtonFont;
  const uint32_t saved = ReadU32(base, slot);
  WriteU32(base, slot, font);
  call();
  WriteU32(base, slot, saved);
}

// A set-text call of an FEMultiLineText: remembered when it had buttons.
template <typename F>
void SetText(PPCContext& ctx, uint8_t* base, uint32_t slot, bool string_arg, F&& original) {
  const uint32_t obj = ctx.r3.u32;
  TextCall call;
  call.slot = slot;
  call.string_arg = string_arg;
  call.width = ctx.r5.u32;
  if (string_arg) {
    const uint32_t str = Valid(ctx.r4.u32) ? ReadU32(base, ctx.r4.u32) : 0;
    if (Valid(str)) call.text = ReadString(base, str + bp::kStringChars, 2048);
  } else if (Valid(ctx.r4.u32)) {
    call.text = ReadString(base, ctx.r4.u32, 2048);
  }
#if defined(COD3_MP)
  // [acct2-decomp] The Use hold's words (UseHoldWording): cod3mp's "[ACTION]" pass is inside the call (the
  // campaign's is hooked, 0x824DE290). A text call (not a String) takes our copy; call.text stays the game's, so a
  // text laid out again (Refresh comes back through here) follows the setting.
  if (!string_arg && !call.text.empty() && call.text.size() < 2047) {  // a whole text, not one cut at 2048
    const std::string pressed = UseHoldWording(ctx, base, call.text);
    if (pressed != call.text)
      if (const uint32_t at = CallText(base, pressed)) ctx.r4.u64 = at;
  }
#endif
  const Mode outer_mode = t_set_mode;
  call.player = PromptPlayer(base);
  call.mode = t_set_mode = Wanted(base, call.player);
  call.gen = g_bind_gen.load(std::memory_order_relaxed);
  const uint32_t tokens = t_tokens;
  ++t_set_depth;
  original();
  --t_set_depth;
  t_set_mode = outer_mode;
  std::lock_guard lock(g_mutex);
  if (t_tokens != tokens)
    g_texts[obj] = std::move(call);
  else
    g_texts.erase(obj);
}

// Before a text is drawn: lays it out again when its prompts are stale.
void Refresh(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  TextCall call;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_texts.find(obj);
    if (it == g_texts.end()) return;
    const Mode want = Wanted(base, it->second.player);
    if (it->second.mode == want &&
        (!IsText(want) || it->second.gen == g_bind_gen.load(std::memory_order_relaxed)))
      return;
    call = it->second;
  }
  if (call.text.size() > 1000) return;
  Call c(ctx, base);
  const uint32_t text = c.PushString(call.text);
  uint32_t arg = text;
  if (call.string_arg) {
    // A fresh String* on the guest stack (the callee releases it).
    const uint32_t holder = (c.PushString("........") + 3) & ~3u;
    WriteU32(base, holder, 0);
    c.Invoke(bp::kStringFromChars, {holder, text});
    arg = holder;
  }
#if defined(COD3_MP)
  // [mp-menus] Laid out as its own player's: the game reads the current local
  // client for the binds of its "[ACTION]" words, and Wanted() for keys.
  const uint32_t current = ReadU32(base, bp::kClientActiveIndex);
  WriteU32(base, bp::kClientActiveIndex, call.player);
  c.Invoke(VirtualSlot(base, obj, call.slot), {obj, arg, call.width});
  WriteU32(base, bp::kClientActiveIndex, current);
#else
  c.Invoke(VirtualSlot(base, obj, call.slot), {obj, arg, call.width});
#endif
}

void Forget(uint32_t obj) {
  std::lock_guard lock(g_mutex);
  g_texts.erase(obj);
}

// "[USE_LEFT_STICK]" (and _NOLEGACY), "[USE_RIGHT_STICK]", "[USE_BOTH_STICKS]":
// 1 left, 2 right, 3 both, 0 other.
int StickAction(std::string_view action) {
  if (SameText(action, "[USE_LEFT_STICK]") || SameText(action, "[USE_LEFT_STICK_NOLEGACY]"))
    return 1;
  if (SameText(action, "[USE_RIGHT_STICK]") || SameText(action, "[USE_RIGHT_STICK_NOLEGACY]"))
    return 2;
  if (SameText(action, "[USE_BOTH_STICKS]")) return 3;
  return 0;
}

// The stick words for the keyboard and mouse (English: the game's own,
// "Use the left thumbstick", are kept in other languages).
std::string StickWords(int stick) {
  const std::string move = FourKeys(kBindMoveUp);
  const bool mouse = g_mouse_aim.load(std::memory_order_relaxed);
  const std::string look_keys = FourKeys(kBindLookUp);
  // [acct2-decomp] The keys as key tokens (KeyToken), drawn as key caps.
  const std::string look = mouse ? "the mouse" : look_keys.empty() ? "" : KeyToken(look_keys);
  if (stick == 1) return move.empty() ? "" : "Use " + KeyToken(move);
  if (stick == 2) return look.empty() ? "" : "Use " + look;
  if (move.empty() || look.empty()) return "";
  return "Use " + KeyToken(move) + " and " + look;
}

// [keybinds] The game's action words come in four forms (its table at
// 0x82A8DFF8): "[W]" (the button), "[PRESS_W]", "[VERB_W]" (Press / Pull /
// Click + the button) and "[HOLD_W]" (the same + "and hold"). Returns the
// form (0-3) and the bare word, or -1.
int ActionWordForm(std::string_view text, std::string_view* word) {
  if (text.size() < 3 || text.front() != '[' || text.back() != ']') return -1;
  text = text.substr(1, text.size() - 2);
  int form = 0;
  for (const auto& [prefix, f] : {std::pair<std::string_view, int>{"PRESS_", 1}, {"VERB_", 2}, {"HOLD_", 3}})
    if (text.size() > prefix.size() && text.substr(0, prefix.size()) == prefix) {
      text.remove_prefix(prefix.size());
      form = f;
      break;
    }
  *word = text;
  return form;
}

// [keybinds] With keys, a word of an action the keyboard calls directly: the
// game's own verb and the action's key, "Press and hold [B]" for
// "[HOLD_BINOCS]" (the game writes "Click and hold the left stick"); "" when
// the action has no key (the game's text stays).
std::string KeyActionText(PPCContext& ctx, uint8_t* base, std::string_view word, int form) {
  const std::string key = PortKeyName(cod3::binds::KeyNameForActionWord(word));
  if (key.empty()) return "";
  if (form == 0) return KeyToken(key);  // [acct2-decomp] key tokens (KeyToken)
  std::string verb = GameText(ctx, base, form == 3 ? "INGAME_PRESS_AND_HOLD" : "INGAME_PRESS");
  if (verb.empty()) verb = form == 3 ? "Press and hold" : "Press";
  return verb + " " + KeyToken(key);
}

#if !defined(COD3_MP)
// [acct2-decomp] The vehicle view hints (owner 2026-10-25 23:42: "there still
// seems to be 'Click Right Stick For view change' when using mouse and
// keyboard when in vehicles"): plain words in the level's string tables (the
// tank's "... to toggle camera view.", the jeep's "... to toggle view"), in
// the three wordings the tables keep for each, with no button token in them.
// Multiplayer's tables have none. The view switch is the Vehicle action
// veh_view (game/keybinds), so with keys "Press [V] to toggle camera view.".
constexpr const char* kViewSwitchWords[] = {
    "Click the right stick to toggle",
    "Click the right thumbstick to toggle",
    "Press the right analog stick to toggle",
    "Press R3 button to toggle",
};
size_t FindViewSwitch(const std::string& text, size_t* words) {
  for (const char* w : kViewSwitchWords)
    if (const size_t at = text.find(w); at != std::string::npos) {
      *words = std::strlen(w) - std::strlen(" to toggle");
      return at;
    }
  return std::string::npos;
}
// With keys: the button words become the game's own "Press" and the key.
std::string ViewSwitchKeys(PPCContext& ctx, uint8_t* base, std::string text) {
  size_t words = 0;
  const size_t at = FindViewSwitch(text, &words);
  if (at == std::string::npos) return text;
  const std::string key = PortKeyName(cod3::binds::KeyNameForAction("veh_view"));
  if (key.empty()) return text;
  std::string press = GameText(ctx, base, "INGAME_PRESS");
  if (press.empty()) press = "Press";
  text.replace(at, words, press + " " + KeyToken(key));
  return text;
}

// [claude-9b-think] 2026-10-27 UNTESTED (owner 10:27: "glyphs or hints with right stick or left stick either words or
// icon are still showing when using keyboard and mouse"). The stick words that come straight from the string tables
// and not from an "[ACTION]" (button-prompts/NOTES.md, "Stick words that are not [ACTION]s": the battle actions'
// "Rotate right thumbstick clockwise.", the machine gun mount's "Click and hold the left thumbstick to setup your
// weapon", "Rotate Left Thumbstick for Yaw", vehicle hints) are words of the text, like the view switch hint above.
// With keys, English only (other languages keep the game's words, as the stick actions do): a stick's click is the
// key of its button with the game's own "Press" ("Press and hold [B]"), the left stick the movement keys ([WASD]),
// the right stick the mouse (with mouse aim; else its keys). The longest wording first; matched without case.
struct StickPhrase {
  const char* words;
  int stick;   // 1 left, 2 right; [claude-9b] 3 both, the right named first; 4 both, the left first
  int click;   // 0 the stick itself, 1 its click, 2 its click held
};
constexpr StickPhrase kStickPhrases[] = {
    // [claude-9b] 2026-10-27 (claude-9b-test: saint_lo's "Use the right and left sticks to move and look around."
    // kept its wording): both sticks in one phrase, plural.
    {"the right and left thumbsticks", 3, 0},      {"the left and right thumbsticks", 4, 0},
    {"right and left thumbsticks", 3, 0},          {"left and right thumbsticks", 4, 0},
    {"the right and left sticks", 3, 0},           {"the left and right sticks", 4, 0},
    {"right and left sticks", 3, 0},               {"left and right sticks", 4, 0},
    {"both thumbsticks", 4, 0},                    {"both sticks", 4, 0},
    {"click and hold the left thumbstick", 1, 2},  {"click and hold the right thumbstick", 2, 2},
    {"click and hold the left stick", 1, 2},       {"click and hold the right stick", 2, 2},
    {"press and hold the left thumbstick", 1, 2},  {"press and hold the right thumbstick", 2, 2},
    {"click the left thumbstick", 1, 1},           {"click the right thumbstick", 2, 1},
    {"click the left stick", 1, 1},                {"click the right stick", 2, 1},
    {"press the left analog stick", 1, 1},         {"press the right analog stick", 2, 1},
    {"press the left thumbstick", 1, 1},           {"press the right thumbstick", 2, 1},
    {"press l3 button", 1, 1},                     {"press r3 button", 2, 1},
    {"the left thumbstick", 1, 0},                 {"the right thumbstick", 2, 0},
    {"the left analog stick", 1, 0},               {"the right analog stick", 2, 0},
    {"the left stick", 1, 0},                      {"the right stick", 2, 0},
    {"left thumbstick", 1, 0},                     {"right thumbstick", 2, 0},
    {"left analog stick", 1, 0},                   {"right analog stick", 2, 0},
    {"left stick", 1, 0},                          {"right stick", 2, 0},
};
size_t FindNoCase(const std::string& text, std::string_view words, size_t from) {
  for (size_t at = from; at + words.size() <= text.size(); ++at) {
    bool same = true;
    for (size_t i = 0; same && i < words.size(); ++i)
      same = std::tolower(uint8_t(text[at + i])) == std::tolower(uint8_t(words[i]));
    // Whole words only.
    if (same && (at == 0 || !std::isalpha(uint8_t(text[at - 1]))) &&
        (at + words.size() == text.size() || !std::isalpha(uint8_t(text[at + words.size()]))))
      return at;
  }
  return std::string::npos;
}
bool HasStickWords(const std::string& text) {
  for (const auto& p : kStickPhrases)
    if (FindNoCase(text, p.words, 0) != std::string::npos) return true;
  // [claude-9b] Any other wording that names a stick, so cod3_debug_key_caps lists what the table misses.
  for (const char* word : {"stick", "sticks", "thumbstick", "thumbsticks"})
    if (FindNoCase(text, word, 0) != std::string::npos) return true;
  return false;
}
std::string StickPhraseKeys(PPCContext& ctx, uint8_t* base, std::string text) {
  if (ReadU32(base, bp::kLanguageMode) != 0) return text;
  const bool mouse = g_mouse_aim.load(std::memory_order_relaxed);
  for (const auto& p : kStickPhrases) {
    const std::string_view words = p.words;
    for (size_t at = FindNoCase(text, words, 0); at != std::string::npos;
         at = FindNoCase(text, words, at + 1)) {
      std::string with;
      if (p.click) {
        // The stick's button: the port's own action on it first (the left click is +binoculars: [B]), else its keybind.
        std::string key = PortKeyName(cod3::binds::KeyNameForPadKey(base, p.stick == 1 ? 220 : 219));
        if (key.empty()) key = BindKey(p.stick == 1 ? 10 : 11);
        if (key.empty()) continue;
        std::string verb = GameText(ctx, base, p.click == 2 ? "INGAME_PRESS_AND_HOLD" : "INGAME_PRESS");
        if (verb.empty()) verb = p.click == 2 ? "Press and hold" : "Press";
        if (std::islower(uint8_t(text[at])) && !verb.empty()) verb[0] = char(std::tolower(uint8_t(verb[0])));
        with = verb + " " + KeyToken(key);
      } else if (p.stick >= 3) {  // [claude-9b] both sticks: "the mouse and [WASD]" / "[WASD] and the mouse"
        const std::string move = FourKeys(kBindMoveUp);
        const std::string look_keys = FourKeys(kBindLookUp);
        const std::string look = mouse ? "the mouse" : look_keys.empty() ? "" : KeyToken(look_keys);
        if (move.empty() || look.empty()) continue;
        with = p.stick == 3 ? look + " and " + KeyToken(move) : KeyToken(move) + " and " + look;
        if (at == 0 && std::islower(uint8_t(with[0]))) with[0] = char(std::toupper(uint8_t(with[0])));
      } else if (p.stick == 1) {
        const std::string keys = FourKeys(kBindMoveUp);
        if (keys.empty()) continue;
        with = KeyToken(keys);
      } else if (mouse) {
        with = at == 0 ? "The mouse" : "the mouse";  // "Rotate Right Thumbstick for Yaw" -> "Rotate the mouse for Yaw"
      } else {
        const std::string keys = FourKeys(kBindLookUp);
        if (keys.empty()) continue;
        with = KeyToken(keys);
      }
      text.replace(at, words.size(), with);
      at += with.size() > 0 ? with.size() - 1 : 0;
    }
  }
  return text;
}
#endif

#if !defined(COD3_MP)
// [claude-9b-think] 2026-10-27 UNTESTED (owner 12:28: "Please make sure you are cataloging the hints, incase we need
// to edit them"). The keyboard-and-mouse wording of the game's texts by string key: config/prompt_text_kbm.tsv (built
// in, kPromptTextKbm) and userdata/prompt_text_kbm.tsv (the player's, read at start; its rows replace the built-in
// ones). A row is "KEY<tab>TEXT". KEY: the string's key hash as 8 hex digits (the game hashes its keys, h * 33 + each
// lowercase character, 0x820A0118; the level scripts only have the hashes; dev/agents/button-prompts/HINTS-CATALOG.tsv
// lists them), or a key name. TEXT: ours, with the game's "[ACTION]" words and "~button" tokens as they are and our
// placeholders: <MOVE> the movement keys, <LOOK> the mouse (the look keys without mouse aim), <BIND:keybind_x> a
// controller button's key, <ACTION:name> one of the port's actions' key (game/keybinds/binds.h), <KEY:name> a key;
// "\n" a new line. Used while keys are shown (menus and play), English only; other texts keep the phrase table.
// [acct2-ocr] Rows "KEY[,KEY..]@lang": the same for the chosen text language (game/text_language.h), our own words;
// placeholders <MOVE:word> / <LOOK:word> (the word without keys), <LCLICK> / <RCLICK> (a stick's click).
uint32_t KeyHash(std::string_view key) {
  uint32_t h = 0;
  for (char c : key) h = h * 33 + uint32_t(uint8_t(std::tolower(uint8_t(c))));
  return h;
}
std::mutex g_kbm_mutex;
std::unordered_map<uint32_t, std::string> g_kbm_text;    // key hash -> our keyboard-and-mouse text
std::unordered_map<std::string, uint32_t> g_kbm_key_of;  // the game's text (as the tables hold it) -> its key hash
uint32_t g_kbm_buffer = 0;                               // guest copy of our text for the "[ACTION]" pass
// [acct2-ocr] The same in other languages: rows "KEY[,KEY..]@lang" (lang a locale or its start: "ru", "zh-Hant"), our
// own words in UTF-8, used while that language's text is shown (game/text_language.h): language -> key hash -> text.
// Its texts are noted by the lookup below in g_kbm_loc_key_of (the chosen language's text -> its key hash).
std::unordered_map<std::string, std::unordered_map<uint32_t, std::string>> g_kbm_loc;
std::unordered_set<uint32_t> g_kbm_loc_hashes;               // every key some language has a row for
std::unordered_map<std::string, uint32_t> g_kbm_loc_key_of;

// [acct2-ocr] The chosen language's row for a key ("ru-RU", then "ru"), UTF-8, or "".
std::string KbmRowForChosen(uint32_t hash) {
  std::string code = cod3::text::Chosen();
  std::lock_guard lock(g_kbm_mutex);
  while (!code.empty()) {
    if (auto l = g_kbm_loc.find(code); l != g_kbm_loc.end())
      if (auto row = l->second.find(hash); row != l->second.end()) return row->second;
    const size_t dash = code.rfind('-');
    if (dash == std::string::npos) break;
    code.resize(dash);
  }
  return "";
}

void ReadPromptText(std::istream& in, const char* from, std::unordered_map<uint32_t, std::string>& rows,
                    std::unordered_map<std::string, std::unordered_map<uint32_t, std::string>>& localized) {
  std::string line;
  int read = 0, left_out = 0;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0) {
      ++left_out;
      continue;
    }
    std::string keys = line.substr(0, tab);
    std::string text = line.substr(tab + 1);
    for (size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at + 1)) text.replace(at, 2, "\n");
    if (text.empty() || text.size() > 500) {
      ++left_out;
      continue;
    }
    // [acct2-ocr] "KEY,KEY@ru": several keys with one text, in one language.
    std::string lang;
    if (const size_t at = keys.find('@'); at != std::string::npos) lang = keys.substr(at + 1), keys.resize(at);
    for (size_t from_at = 0; from_at <= keys.size();) {
      size_t comma = keys.find(',', from_at);
      if (comma == std::string::npos) comma = keys.size();
      const std::string key = keys.substr(from_at, comma - from_at);
      from_at = comma + 1;
      if (key.empty()) continue;
      bool hex = key.size() == 8;
      for (char c : key) hex = hex && std::isxdigit(uint8_t(c)) != 0;
      const uint32_t hash = hex ? uint32_t(std::strtoul(key.c_str(), nullptr, 16)) : KeyHash(key);
      if (lang.empty())
        rows[hash] = text;
      else
        localized[lang][hash] = text;
    }
    ++read;
  }
  REXLOG_INFO("cod3: prompt texts: {} rows from {} ({} lines left out)", read, from, left_out);
}

// The key of a keybind setting by name ("keybind_back" -> TAB), "" when unknown or unbound.
std::string BindKeyNamed(std::string_view cvar) {
  for (int i = 0; i < int(sizeof(kBindCvars) / sizeof(kBindCvars[0])); ++i)
    if (SameText(cvar, kBindCvars[i])) return BindKey(i);
  return "";
}

// Our placeholders -> key tokens (KeyToken) or words.
std::string ExpandPlaceholders(uint8_t* base, std::string text) {
  for (size_t at = text.find('<'); at != std::string::npos; at = text.find('<', at + 1)) {
    const size_t end = text.find('>', at);
    if (end == std::string::npos) break;
    const std::string tag = text.substr(at + 1, end - at - 1);
    std::string with;
    // [acct2-ocr] "<MOVE:word>", "<LOOK:word>": the word instead of "the movement keys" / "the mouse" (other languages).
    const bool own_word = tag.size() > 5 && tag[4] == ':';
    if (tag == "MOVE" || (own_word && tag.rfind("MOVE", 0) == 0)) {
      const std::string keys = FourKeys(kBindMoveUp);
      with = !keys.empty() ? KeyToken(keys) : own_word ? tag.substr(5) : "the movement keys";
    } else if (tag == "LOOK" || (own_word && tag.rfind("LOOK", 0) == 0)) {
      const std::string keys = FourKeys(kBindLookUp);
      with = !(g_mouse_aim.load(std::memory_order_relaxed) || keys.empty()) ? KeyToken(keys)
             : own_word                                                      ? tag.substr(5)
             : at == 0                                                       ? "The mouse"
                                                                             : "the mouse";
    } else if (tag == "LCLICK" || tag == "RCLICK") {
      // [acct2-ocr] a stick's click: the port's own action on it first (the left one is +binoculars), else its keybind.
      const bool left = tag[0] == 'L';
      std::string key = PortKeyName(cod3::binds::KeyNameForPadKey(base, left ? 220 : 219));
      if (key.empty()) key = BindKey(left ? 10 : 11);
      with = key.empty() ? "" : KeyToken(key);
    } else if (tag.rfind("BIND:", 0) == 0) {
      const std::string key = BindKeyNamed(tag.substr(5));
      with = key.empty() ? "" : KeyToken(key);
    } else if (tag.rfind("ACTION:", 0) == 0) {
      const std::string key = PortKeyName(cod3::binds::KeyNameForAction(tag.substr(7).c_str()));
      with = key.empty() ? "" : KeyToken(key);
    } else if (tag.rfind("KEY:", 0) == 0 && tag.size() > 4) {
      with = KeyToken(tag.substr(4));
    } else {
      continue;  // not ours: left as it is
    }
    text.replace(at, end - at + 1, with);
    if (!with.empty()) at += with.size() - 1;
  }
  return text;
}

// [acct2-ocr] The chosen language's words for "[USE_LEFT_STICK]" (1), "[USE_RIGHT_STICK]" (2) and
// "[USE_BOTH_STICKS]" (3) with keys (rows COD3_KBM_STICK_<n>@lang), or "": no row, or the game's own prompts shown
// (English chosen, cod3_text_prompts off, no font for the script yet).
std::string LocalizedStickWords(uint8_t* base, int stick) {
  static constexpr const char* kSentence[] = {"INGAME_XBOX_USE_LEFT_STICK", "INGAME_XBOX_USE_RIGHT_STICK",
                                              "INGAME_XBOX_USE_BOTH_STICKS"};
  static constexpr const char* kRow[] = {"COD3_KBM_STICK_1", "COD3_KBM_STICK_2", "COD3_KBM_STICK_3"};
  if (stick < 1 || stick > 3) return "";
  if (!cod3::text::GuestTextFor(base, KeyHash(kSentence[stick - 1]))) return "";
  const std::string row = cod3::text::ForChosenFonts(KbmRowForChosen(KeyHash(kRow[stick - 1])));
  return row.empty() ? "" : ExpandPlaceholders(base, row);
}
#else
std::string LocalizedStickWords(uint8_t*, int) { return ""; }  // [acct2-ocr] the campaign's rows only
#endif

#if !defined(COD3_MP)
// With keys, "Pull ~r1" and "Click ~lstick" (the game's words for triggers
// and stick buttons) read "Press ...": every language's own words.
std::string KeyVerbs(PPCContext& ctx, uint8_t* base, std::string text) {
  if (text.find('~') == std::string::npos) return text;
  const std::string press = GameText(ctx, base, "INGAME_PRESS");
  const std::string press_hold = GameText(ctx, base, "INGAME_PRESS_AND_HOLD");
  if (press.empty()) return text;
  struct Verb {
    std::string from, to;
  };
  const Verb verbs[] = {
      {GameText(ctx, base, "INGAME_PULL_AND_HOLD"), press_hold},
      {GameText(ctx, base, "INGAME_CLICK_AND_HOLD"), press_hold},
      {GameText(ctx, base, "INGAME_PULL"), press},
      {GameText(ctx, base, "INGAME_CLICK"), press},
  };
  for (const auto& v : verbs) {
    if (v.from.empty() || v.to.empty()) continue;
    const std::string from = v.from + " ~";
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + 1)) {
      if (at > 0 && std::isalpha(uint8_t(text[at - 1]))) continue;  // inside a word
      text.replace(at, v.from.size(), v.to);
    }
  }
  return text;
}
#endif

#if !defined(COD3_MP)
// The title page's "PRESS START": "PRESS ENTER" with the keyboard (Enter is
// START there, game/pc_menu.h). Checked against the text shown, so a title
// page shown again (Init sets its text anew) is caught too.
void UpdateTitle(PPCContext& ctx, uint8_t* base, uint32_t page) {
  const uint32_t elem = ReadU32(base, page + bp::kTitleHelpbar);
  if (!Valid(elem)) return;
  std::string keys_text = GameText(ctx, base, "FEMENU_TITLE_PRESS_START_XBOX");
  const size_t at = keys_text.find("START");
  if (at == std::string::npos) return;
  keys_text.replace(at, 5, "ENTER");
  const uint32_t str = ReadU32(base, elem + bp::kTextString);
  const std::string shown = Valid(str) ? ReadString(base, str + bp::kStringChars, 128) : "";
  const bool keys = IsKeys(Wanted(base, PromptPlayer(base)));
  if (keys == (shown == keys_text)) return;
  Call c(ctx, base);
  if (keys)
    c.Invoke(VirtualSlot(base, elem, bp::kSetText), {elem, c.PushString(keys_text)});
  else
    c.Invoke(VirtualSlot(base, elem, bp::kSetTextKey), {elem, bp::kTitlePressStartKey});
}
#endif  // !COD3_MP (cod3mp's "Press START" page, PressStartMenu, is never shown)

// "[ACTION]" -> its button sentence, after the game's own (`action` the word,
// ctx.r3 the game's result): with keys, the stick words ("Use [WASD]", "Use
// the mouse") and the keys of the port's own actions.
void AfterActionText(PPCContext& ctx, uint8_t* base, const std::string& action) {
  const int stick = StickAction(action);
  // [keybinds] A word of an action the keyboard calls directly (BINOCS).
  std::string_view word;
  const int form = ActionWordForm(action, &word);
  const bool key_word = form >= 0 && cod3::binds::IsActionWord(word);
  if (stick || key_word) ++t_tokens;  // remembered, so a device switch lays it out again
  if (key_word && IsKeys(LayoutMode(base))) {
    if (const std::string text = KeyActionText(ctx, base, word, form); !text.empty())
      if (const uint32_t at = TempText(base, text)) ctx.r3.u64 = at;
    return;
  }
  if (!stick || !IsKeys(LayoutMode(base)) || ReadU32(base, bp::kLanguageMode) != 0) return;
  // [acct2-ocr] The chosen language's stick words (COD3_KBM_STICK_<n>@lang rows), while its prompts are shown.
  if (const std::string ours = LocalizedStickWords(base, stick); !ours.empty()) {
    if (const uint32_t text = TempText(base, ours)) ctx.r3.u64 = text;
    return;
  }
  if (const uint32_t text = TempText(base, StickWords(stick))) {
    if (base[text]) ctx.r3.u64 = text;
  }
}

#if defined(COD3_MP)
// [mp-menus] cod3mp's line draw squeezes a button picture to 0.75 of its width
// while cg_widescreen is set (0x824EC444: the vmCvar's integer); names drawn
// in the text's font keep the text's width.
constexpr uint32_t kWidescreenInt = 0x82B9F0DC;  // cg_widescreen (vmCvar 0x82B9F0D0) .integer
#endif

// ---------------------------------------------------------------------------
// [acct2-decomp] 2026-11-05 A plain FEText's buttons (owner 08:56: multiplayer's "Press A to respawn" kept the A
// whatever the button style; 08:57: each player's own).
//
// A plain FEText (not an FEMultiLineText) draws "~" and the one character after it as that character of the button
// font, and knows no "~name" tokens: its draw (FEText__vfunc_19: campaign 0x824CFD58, cod3mp 0x824ECCA0) and its
// measure (FEText__vfunc_60: 0x824CFBE8 / 0x824EC9F0) read the text themselves. The texts written that way are
// cod3mp's spawn screen (MPSCRIPT_PRESS_X_TO_SPAWN "Press ~\" to spawn", set on its text_line_04 by SetTextKey in
// 0x8239CC68, and "Press  ~\" to spawn. Spawn time left: ") and the frontend's button bars ("~\" Ready", "~$ Back",
// "~# Sorting by: %s"; both programs' tables). The FEMultiLineText hooks below never see them, so they kept the
// game's Xbox picture. Their set-text (FEText__vfunc_36, where SetTextKey ends too) now takes our copy with each
// such button as its name in brackets, the way the text's player wants it ("[SPACE]", "[CROSS]"; no key caps or
// pictures of our own here, those need the line layout), or the game's own picture while that player wants it.
// A plain FEText's draw sets its text again when that changes (a device switch, another style, a rebind).
// ---------------------------------------------------------------------------
#if defined(COD3_MP)
constexpr uint32_t kPlainSetText = 0x820BBB90;  // FEText__vfunc_36, slot 36 (bp::kSetText) of a plain FEText
#else
constexpr uint32_t kPlainSetText = 0x820ACDD0;
#endif

const Button* RawButton(char c) {
  for (const Button& b : kButtons)
    if (b.glyph == c) return &b;
  return nullptr;
}

bool HasRawButtons(std::string_view text) {
  for (size_t i = 0; i + 1 < text.size(); ++i)
    if (text[i] == '~' && RawButton(text[i + 1])) return true;
  return false;
}

// The text with each "~" + button character as "[NAME]" for `mode`; the text itself for the game's pictures.
std::string RawButtonsNamed(uint8_t* base, const std::string& text, Mode mode) {
  if (!IsText(mode)) return text;
  std::string out;
  out.reserve(text.size() + 16);
  for (size_t i = 0; i < text.size(); ++i) {
    const Button* b = text[i] == '~' && i + 1 < text.size() ? RawButton(text[i + 1]) : nullptr;
    if (!b) {
      out += text[i];
      continue;
    }
    std::string name;
    if (mode == Mode::kPlayKeys) name = PortKeyName(cod3::binds::KeyNameForPadKey(base, PadKeyFor(b->glyph)));
    if (name.empty()) name = NameFor(*b, mode);
    out += "[" + name + "]";
    ++i;
  }
  return out;
}

// The last text with buttons of each plain FEText, to set it again (under g_mutex).
struct PlainCall {
  std::string text;  // the game's own text
  uint32_t player = 0;
  Mode mode = Mode::kXbox;
  uint32_t gen = 0;
};
std::unordered_map<uint32_t, PlainCall> g_plain;

void ForgetPlain(uint32_t obj) {
  std::lock_guard lock(g_mutex);
  g_plain.erase(obj);
}

// Before a plain FEText is drawn: its text is set again when its buttons are out of date.
void RefreshPlain(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  PlainCall call;
  {
    std::lock_guard lock(g_mutex);
    if (g_plain.empty()) return;
    auto it = g_plain.find(obj);
    if (it == g_plain.end()) return;
    const Mode want = Wanted(base, it->second.player);
    if (it->second.mode == want &&
        (!IsText(want) || it->second.gen == g_bind_gen.load(std::memory_order_relaxed)))
      return;
    call = it->second;
  }
  if (call.text.size() > 1000 || VirtualSlot(base, obj, bp::kSetText) != kPlainSetText) {
    ForgetPlain(obj);
    return;
  }
  Call c(ctx, base);
  const uint32_t text = c.PushString(call.text);
#if defined(COD3_MP)
  // As its own player's (Wanted() reads the current local client's).
  const uint32_t current = ReadU32(base, bp::kClientActiveIndex);
  WriteU32(base, bp::kClientActiveIndex, call.player);
  c.Invoke(VirtualSlot(base, obj, bp::kSetText), {obj, text});
  WriteU32(base, bp::kClientActiveIndex, current);
#else
  c.Invoke(VirtualSlot(base, obj, bp::kSetText), {obj, text});
#endif
}
}  // namespace

namespace cod3::prompts {

void Attach(rex::ui::Window* window, size_t z_order) {
  if (!window) return;
  RefreshSettings();
  rex::cvar::RegisterChangeCallback("cod3_glyph_style", [](std::string_view, std::string_view v) {
    g_style.store(ParseStyle(v), std::memory_order_relaxed);
  });
  g_window = window;
  window->AddInputListener(&g_watch, z_order);
  RefreshLayout();  // [rexglue-native qte] 2026-11-09 the keyboard layout's key names (UI thread)
}

void Detach(rex::ui::Window* window) {
  if (window && g_window == window) window->RemoveInputListener(&g_watch);
  g_window = nullptr;
  g_input_system.store(nullptr, std::memory_order_release);
}

void SetInputSystem(rex::input::InputSystem* input) {
  g_input_system.store(input, std::memory_order_release);
}

void NoteKeyboardUse() { KeyboardUsed("a test's key or mouse"); }

// [claude-9b-think] See button_prompts.h.
void LoadPromptText(const std::filesystem::path& user_dir) {
#if !defined(COD3_MP)
  std::unordered_map<uint32_t, std::string> rows;
  std::unordered_map<std::string, std::unordered_map<uint32_t, std::string>> localized;  // [acct2-ocr]
  {
    std::istringstream in{std::string(kPromptTextKbm)};
    ReadPromptText(in, "config/prompt_text_kbm.tsv (built in)", rows, localized);
  }
  std::error_code error;
  const std::filesystem::path user = user_dir / "prompt_text_kbm.tsv";
  if (!user_dir.empty() && std::filesystem::exists(user, error)) {
    std::ifstream in(user);
    if (in) ReadPromptText(in, "userdata/prompt_text_kbm.tsv", rows, localized);
  }
  std::lock_guard lock(g_kbm_mutex);
  g_kbm_text = std::move(rows);
  g_kbm_key_of.clear();
  g_kbm_loc_hashes.clear();  // [acct2-ocr]
  for (const auto& [lang, texts] : localized)
    for (const auto& [hash, text] : texts) g_kbm_loc_hashes.insert(hash);
  g_kbm_loc = std::move(localized);
  g_kbm_loc_key_of.clear();
#else
  (void)user_dir;  // the campaign's texts only
#endif
}

bool KeyboardMouseInUse() {
  return g_mnk_mode.load(std::memory_order_relaxed) &&
         g_kbm_presses.load(std::memory_order_relaxed) != 0 &&
         g_device.load(std::memory_order_relaxed) == Device::kKeyboard;
}

// [acct2-decomp] See button_prompts.h.
Look FirstPlayerLook() {
  if (REXCVAR_GET(cod3_key_prompts) && g_mnk_mode.load(std::memory_order_relaxed) &&
      g_device.load(std::memory_order_relaxed) == Device::kKeyboard)
    return Look::kKeys;
  Mode mode = Mode::kXbox;
  switch (g_style.load(std::memory_order_relaxed)) {
    case Style::kXbox: mode = Mode::kXbox; break;
    case Style::kPlayStation: mode = Mode::kPlayStation; break;
    case Style::kPlayStation2: mode = Mode::kPlayStation2; break;  // [claude-9b-think]
    case Style::kNintendo: mode = Mode::kNintendo; break;
    case Style::kGameCube: mode = Mode::kGameCube; break;
    default: mode = UserPadMode(0); break;  // the first controller (guest user 0)
  }
  return LookOf(mode);  // [claude-9b-think] PS2 too
}

void NotePadState(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, const int16_t axes[4]) {
  const uint8_t triggers[2] = {left_trigger, right_trigger};
  WatchPadValues(buttons, triggers, axes);
}

std::vector<KeyCap> CurrentKeyCaps(int max_age_ms) {
  std::vector<KeyCap> out;
  const int64_t now = NowMs();
  // [claude-9b-think] 2026-10-27 UNTESTED also only lines drawn in the game's newest frame, or within their own
  // cadence of it (g_cap_latest_frame): a line the game stopped drawing loses its caps with the game's next frame.
  const uint32_t latest = g_cap_latest_frame.load(std::memory_order_relaxed);
  size_t lines = 0, stale = 0;
  int64_t oldest_ms = 0;
  std::lock_guard lock(g_cap_mutex);
  for (const auto& [line, drawn] : g_caps) {
    if (now - drawn.ms > max_age_ms) continue;
    if (int32_t(latest - drawn.frame) > int32_t(drawn.cadence)) {
      ++stale;
      continue;
    }
    ++lines;
    oldest_ms = std::max(oldest_ms, now - drawn.ms);
    out.insert(out.end(), drawn.caps.begin(), drawn.caps.end());
  }
  if (REXCVAR_GET(cod3_debug_key_caps) && (lines || stale)) {
    static int64_t last = 0;
    if (now - last >= 250) {
      last = now;
      REXLOG_INFO("cod3: key caps: overlay shows {} lines ({} caps), the oldest drawn {} ms ago; {} lines left out "
                  "(not drawn in the game's newest frame {})",
                  lines, out.size(), oldest_ms, stale, latest);
    }
  }
  return out;
}

}  // namespace cod3::prompts

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// The pads the game reads (user 0: player 1's pads and the keyboard binds).
REX_EXTERN(__imp__sub_82344F98);
REX_HOOK_RAW(sub_82344F98) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_82344F98(ctx, base);
  if (user == 0) NoteGameFrame(base);  // [claude-9b-think] the key caps' newest game frame, also with no line drawn
  if (user == 0 && ctx.r3.u32 == 0 && Valid(state)) WatchPad(base, state);
  // [rumble] The first controller doesn't vibrate while the keyboard and
  // mouse are in use (after WatchPad, which tells them apart; game/rumble.h).
  if (user == 0) cod3::rumble::OnPadRead(ctx, base);
  // [keybinds] Keys of the port's own actions inside an interaction: the
  // controller button the game binds to the action (after WatchPad, so device
  // detection sees only the real pads; game/keybinds/binds.h).
  if (ctx.r3.u32 == 0 && Valid(state)) cod3::binds::ApplyToPad(base, user, state);
  // Stick battle actions (game/qte_actions.h): the mouse as the stick, in
  // whichever pad the game reads.
  if (ctx.r3.u32 == 0 && Valid(state)) cod3::qte::ApplyToPad(base, user, state);
}

// "~name" -> the button's character: the name instead while names are laid out.
REX_EXTERN(__imp__sub_824C68E0);
REX_HOOK_RAW(sub_824C68E0) {
  const uint32_t text = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  __imp__sub_824C68E0(ctx, base);
  // [acct2-decomp] Not one of the game's: one of our key tokens (KeyToken)?
  if (ctx.r3.u32 == 0 && Valid(text) && Valid(out)) {
    uint32_t length = 0;
    if (const uint32_t name = KeyTokenText(base, text, &length)) {
      WriteU32(base, out, name);
      ctx.r3.u64 = length;
      ++t_tokens;
    }
    return;
  }
  if (ctx.r3.u32 == 0 || !Valid(out)) return;  // not a button token
  ++t_tokens;
  // [claude-9b-think] 2026-10-27 UNTESTED a button picture laid out while keys are wanted (a line in the button font
  // itself, or a layout these hooks don't see): the text, for the owner's "icon" (cod3_debug_key_caps).
  if (!t_names && REXCVAR_GET(cod3_debug_key_caps) && IsKeys(LayoutMode(base))) {
    static std::unordered_map<std::string, int> logged;
    const std::string token = ReadString(base, text, 48);
    if (logged.size() < 200 && logged[token]++ == 0)
      REXLOG_INFO("cod3: prompts: a button picture kept with keys: \"{}\" (not laid out by the line layout hook)", token);
  }
  if (!t_names) return;
  const uint32_t glyph = ReadU32(base, out);
  if (!Valid(glyph)) return;
  if (const uint32_t name = NameText(base, char(base[glyph]), t_name_mode)) WriteU32(base, out, name);
}

// Lays out one line: with names, measured in the line's font at the text scale.
REX_EXTERN(__imp__sub_824D7650);
REX_HOOK_RAW(sub_824D7650) {
  const uint32_t line = ctx.r3.u32;
  const Mode mode = LayoutMode(base);
  const uint32_t font = IsText(mode) ? TextFont(base, ReadU32(base, line + bp::kLineFont)) : 0;
  if (!font) {
    __imp__sub_824D7650(ctx, base);
    std::lock_guard lock(g_mutex);
    g_text_lines.erase(line);
    g_line_names.erase(line);  // [claude-9b-think]
    return;
  }
  ctx.f2.f64 = ctx.f1.f64;  // names at the text's scale
  const bool outer_names = t_names;
  const Mode outer_mode = t_name_mode;
  t_names = true, t_name_mode = mode;
  // [claude-9b-think] the font the names are laid out in (FittedRoom), and this line's names in order.
  const uint32_t outer_font = t_name_font, outer_font_index = t_name_font_index;
  const bool outer_line = t_line_layout;
  std::vector<std::string> outer_names_list;
  outer_names_list.swap(t_line_names);
  t_name_font = font, t_name_font_index = ReadU32(base, line + bp::kLineFont), t_line_layout = true;
  const uint32_t tokens = t_tokens;
  WithButtonFont(base, font, [&] { __imp__sub_824D7650(ctx, base); });
  t_names = outer_names, t_name_mode = outer_mode;
  t_name_font = outer_font, t_name_font_index = outer_font_index, t_line_layout = outer_line;
  std::vector<std::string> names;
  names.swap(t_line_names);
  t_line_names.swap(outer_names_list);
  std::lock_guard lock(g_mutex);
  if (t_tokens != tokens) {
    // Practically never this many; a forgotten line only loses its names.
    if (g_text_lines.size() > 50000) g_text_lines.clear(), g_line_names.clear();
    g_text_lines[line] = mode;
  } else {
    g_text_lines.erase(line);
  }
  if (names.empty())
    g_line_names.erase(line);
  else
    g_line_names[line] = std::move(names);
}

// Measures one word for the word wrap: with names like the layout above.
REX_EXTERN(__imp__sub_824D7B48);
REX_HOOK_RAW(sub_824D7B48) {
  const Mode mode = LayoutMode(base);
  const uint32_t font = IsText(mode) ? TextFont(base, ctx.r4.u32) : 0;
  if (!font) {
    __imp__sub_824D7B48(ctx, base);
    return;
  }
  ctx.f2.f64 = ctx.f1.f64;
  const bool outer_names = t_names;
  const Mode outer_mode = t_name_mode;
  t_names = true, t_name_mode = mode;
  // [claude-9b-think] measured with the rooms the line layout will give them (FittedRoom), so the word wrap breaks
  // for the caps' widths; a measure keeps no names.
  const uint32_t outer_font = t_name_font, outer_font_index = t_name_font_index;
  const bool outer_line = t_line_layout;
  t_name_font = font, t_name_font_index = ctx.r4.u32, t_line_layout = false;
  WithButtonFont(base, font, [&] { __imp__sub_824D7B48(ctx, base); });
  t_names = outer_names, t_name_mode = outer_mode;
  t_name_font = outer_font, t_name_font_index = outer_font_index, t_line_layout = outer_line;
}

// Draws one line: names in the line's font at the text's x scale, standing on
// the text's baseline (the text may be drawn taller than wide).
REX_EXTERN(__imp__sub_824D7D88);
REX_HOOK_RAW(sub_824D7D88) {
  const uint32_t line = ctx.r3.u32;
  cod3::pcmenu::NoteLineDrawn(base, line, ctx.f3.f64);  // footer prompts can be clicked (pc_menu.h)
  NoteGameFrame(base);  // [claude-9b-think] any line drawn: the key caps' newest game frame
  bool names;
  bool fitted_rooms = false;  // [claude-9b-think] laid out with rooms of filler (FittedRoom): never shown as text
  Mode mode = Mode::kXbox;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_text_lines.find(line);
    names = it != g_text_lines.end();
    if (names) mode = it->second;
    fitted_rooms = g_line_names.count(line) != 0;
  }
  const uint32_t font = names ? TextFont(base, ReadU32(base, line + bp::kLineFont)) : 0;
  // [acct2-decomp] A line not drawn with keys now has no key caps.
  if (!font || !OverlayNames(mode)) {  // [claude-9b-think] pictures too
    std::lock_guard lock(g_cap_mutex);
    g_caps.erase(line);
  }
  if (!font) {
    __imp__sub_824D7D88(ctx, base);
    return;
  }
  const double baseline = double(int32_t(ReadU32(base, font + bp::kFontBaseline)));
  ctx.f4.f64 = ctx.f2.f64;                            // the text's x scale
  ctx.f5.f64 = baseline * (ctx.f2.f64 - ctx.f3.f64);  // raise: baselines meet
  // [acct2-decomp] Keys as key caps: the game draws the names invisibly (the
  // button color's alpha 0; D3DCOLOR, alpha in the top byte) to keep their
  // room, and the overlay draws a cap there at the color's own opacity.
  // [claude-9b-think] and our pictures (mouse buttons, controller styles): OverlayNames.
  if (OverlayNames(mode)) {
    const uint32_t color = ctx.r6.u32;
    // [claude-9b] The cap fades with the line's text (owner 2026-10-26: the keyboard and mouse glyphs
    // didn't follow their prompt's fade-out; the glyph alone showed for a moment): the game fades a
    // prompt through its text color (r5, the text segments' color in this function) while the
    // button color (r6) can stay opaque, so the cap takes the fainter of the two.
    // [claude-9b-think] 2026-10-28 UNTESTED both opacities: a line of names alone (a battle action's button) takes the
    // button color's (NoteKeyCaps).
    const uint32_t text_alpha = ctx.r5.u32 >> 24, button_alpha = color >> 24;
    NoteKeyCaps(base, line, font, float(ctx.f2.f64), float(ctx.f3.f64), float(text_alpha) / 255.0f,
                float(button_alpha) / 255.0f, mode);
    ctx.r6.u64 = color & 0x00FFFFFFu;
  } else if (fitted_rooms) {
    // [claude-9b-think] key caps just turned off: the filler stays invisible until the text is laid out again (the
    // settings are read about once a second).
    ctx.r6.u64 = ctx.r6.u32 & 0x00FFFFFFu;
  }
#if defined(COD3_MP)
  const uint32_t widescreen = ReadU32(base, kWidescreenInt);
  WriteU32(base, kWidescreenInt, 0);
  WithButtonFont(base, font, [&] { __imp__sub_824D7D88(ctx, base); });
  WriteU32(base, kWidescreenInt, widescreen);
#else
  WithButtonFont(base, font, [&] { __imp__sub_824D7D88(ctx, base); });
#endif
}

#if !defined(COD3_MP)
// "[ACTION]" -> its button sentence (the word in r3): see AfterActionText.
REX_EXTERN(__imp__sub_824DD9A8);
REX_HOOK_RAW(sub_824DD9A8) {
  const std::string action = Valid(ctx.r3.u32) ? ReadString(base, ctx.r3.u32, 64) : "";
  __imp__sub_824DD9A8(ctx, base);
  AfterActionText(ctx, base, action);
  // [acct2-ocr] the chosen language's button verbs put the button where their text says ("~a drücken")
  if (Valid(ctx.r3.u32))
    if (const std::string placed = cod3::text::PlaceButton(ReadString(base, ctx.r3.u32, 512)); !placed.empty())
      if (const uint32_t at = TempText(base, placed)) ctx.r3.u64 = at;
}

// The whole text after its "[ACTION]" pass (a static 512-byte buffer): with
// keys, "Pull ~r1" -> "Press ~r1", and [acct2-decomp] the vehicle view hint's
// words -> "Press [V]" (ViewSwitchKeys).
REX_EXTERN(__imp__sub_824DE290);
REX_HOOK_RAW(sub_824DE290) {
  // [claude-9b-think] A text the keyboard-and-mouse table has, by its key (r3 the text): ours is laid out instead
  // while keys are shown (English), and the text is remembered either way, so a device switch lays it out again.
  bool ours = false;
  uint32_t key = 0;
  if (Valid(ctx.r3.u32)) {
    std::string kbm;
    bool localized = false;  // [acct2-ocr] the chosen language's text: its own row (KEY@lang), never the English one
    {
      const std::string source = ReadString(base, ctx.r3.u32, 512);
      std::lock_guard lock(g_kbm_mutex);
      if (auto it = g_kbm_loc_key_of.find(source); it != g_kbm_loc_key_of.end()) {
        key = it->second;
        localized = true;
      } else if (auto it = g_kbm_key_of.find(source); it != g_kbm_key_of.end()) {
        key = it->second;
        if (auto row = g_kbm_text.find(key); row != g_kbm_text.end()) kbm = row->second;
      }
    }
    if (localized) kbm = cod3::text::ForChosenFonts(KbmRowForChosen(key));
    if (!kbm.empty()) {
      ++t_tokens;
      if (IsKeys(LayoutMode(base)) && ReadU32(base, bp::kLanguageMode) == 0 && kbm.size() < 511) {
        if (!g_kbm_buffer) g_kbm_buffer = rex::system::kernel_state()->memory()->SystemHeapAlloc(512);
        if (g_kbm_buffer) {
          std::memcpy(base + g_kbm_buffer, kbm.c_str(), kbm.size() + 1);
          ctx.r3.u64 = g_kbm_buffer;
          ours = true;
        }
      }
    }
  }
  // [acct2-decomp] The Use hold's words (UseHoldWording), on whichever text goes in (ours or the game's).
  if (Valid(ctx.r3.u32)) {
    const std::string in = ReadString(base, ctx.r3.u32, 512);
    const std::string pressed = in.size() < 511 ? UseHoldWording(ctx, base, in) : in;  // a whole text only
    if (pressed != in)
      if (const uint32_t at = CallText(base, pressed)) ctx.r3.u64 = at;
  }
  __imp__sub_824DE290(ctx, base);
  const uint32_t buf = ctx.r3.u32;
  if (!Valid(buf)) return;
  const std::string shown = ReadString(base, buf, 512);
  // [claude-9b-think] our placeholders (<MOVE>, <BIND:..> ...) -> key tokens, after the game's "[ACTION]" pass.
  const std::string text = ours ? ExpandPlaceholders(base, shown) : shown;
  if (ours && REXCVAR_GET(cod3_debug_key_caps)) {
    static std::unordered_map<uint32_t, int> logged;
    if (logged.size() < 200 && logged[key]++ == 0)
      REXLOG_INFO("cod3: prompts: text {:08X} shown with the keyboard-and-mouse table: \"{}\"", key, text);
  }
  // [acct2-decomp] A view hint is remembered with either device (counted as
  // a button), so a later switch lays it out again.
  size_t words = 0;
  if (FindViewSwitch(text, &words) != std::string::npos) ++t_tokens;
  // [claude-9b-think] 2026-10-27 UNTESTED a text with stick words too (StickPhraseKeys).
  const bool stick_words = HasStickWords(text);
  if (stick_words) ++t_tokens;
  const Mode mode = LayoutMode(base);
  if (!IsKeys(mode)) return;
  std::string out = ViewSwitchKeys(ctx, base, KeyVerbs(ctx, base, text));
  // In play only: the menus' own pages name the controller's sticks (the controls layout).
  if (stick_words && mode == Mode::kPlayKeys) out = StickPhraseKeys(ctx, base, out);
  if (REXCVAR_GET(cod3_debug_key_caps) && HasStickWords(out)) {
    static std::unordered_map<std::string, int> logged;
    if (logged.size() < 200 && logged[out]++ == 0)
      REXLOG_INFO("cod3: prompts: stick words left with keys ({}): \"{}\"", mode == Mode::kPlayKeys ? "play" : "menu",
                  out);
  }
  if (out != shown && out.size() < 510) std::memcpy(base + buf, out.c_str(), out.size() + 1);
}

// [claude-9b-think] 2026-10-27 UNTESTED The string tables' lookup by key hash (r3 the tables, r4 the hash; Localize's
// keys and the level scripts' hashes both end here): which key a text came from, noted for the keys the
// keyboard-and-mouse table has (the "[ACTION]" pass above looks the text up).
REX_EXTERN(__imp__sub_82527FD0);
REX_HOOK_RAW(sub_82527FD0) {
  const uint32_t hash = ctx.r4.u32;
  __imp__sub_82527FD0(ctx, base);
  // [acct2-ocr] The chosen text language (game/text_language.h) answers first; the English
  // keyboard-and-mouse wording below is then left out for this text.
  // [acct2-ocr] r3 is the table entry, its text at +8 (the callers' lwz r3,8(r3)).
  if (const uint32_t ours = cod3::text::GuestEntryFor(base, hash, ctx.r3.u32)) {
    ctx.r3.u64 = ours;
    // [acct2-ocr] a text the keyboard-and-mouse rows have in some language: noted, as the English below.
    const uint32_t text = ReadU32(base, ours + 8);
    if (!Valid(text)) return;
    std::lock_guard lock(g_kbm_mutex);
    if (!g_kbm_loc_hashes.count(hash)) return;
    if (g_kbm_loc_key_of.size() > 4096) g_kbm_loc_key_of.clear();
    g_kbm_loc_key_of[ReadString(base, text, 512)] = hash;
    return;
  }
  if (!Valid(ctx.r3.u32)) return;
  const uint32_t text = ReadU32(base, ctx.r3.u32 + 8);   // [acct2-ocr] was the entry itself
  if (!Valid(text)) return;
  std::lock_guard lock(g_kbm_mutex);
  if (g_kbm_text.find(hash) == g_kbm_text.end()) return;
  if (g_kbm_key_of.size() > 4096) g_kbm_key_of.clear();
  g_kbm_key_of[ReadString(base, text, 512)] = hash;
}
#else
// [mp-menus] cod3mp's "[ACTION]" -> its sentence is FEMultiLineText's slot 94
// (this, word in r4), called for each word by slot 93 (0x824D6C48). Slot 93
// itself, the campaign's "Pull" -> "Press" pass, isn't hooked yet: its
// generated name is the one the campaign's slot 93 alias takes.
REX_EXTERN(__imp__FEMultiLineText__vfunc_94);
REX_HOOK_RAW(FEMultiLineText__vfunc_94) {
  const std::string action = Valid(ctx.r4.u32) ? ReadString(base, ctx.r4.u32, 64) : "";
  __imp__FEMultiLineText__vfunc_94(ctx, base);
  AfterActionText(ctx, base, action);
  // [acct2-ocr] as the campaign's 0x824DD9A8 hook: the chosen language's button verbs put the button where their
  // text says ("~a drücken"); this slot answers in r3 like the campaign's (AfterActionText reads/sets r3 for both)
  if (Valid(ctx.r3.u32))
    if (const std::string placed = cod3::text::PlaceButton(ReadString(base, ctx.r3.u32, 512)); !placed.empty())
      if (const uint32_t at = TempText(base, placed)) ctx.r3.u64 = at;
}
#endif

// FEMultiLineText::Draw: stale prompts are laid out again first.
REX_EXTERN(__imp__FEMultiLineText__vfunc_19);
REX_HOOK_RAW(FEMultiLineText__vfunc_19) {
  Refresh(ctx, base, ctx.r3.u32);
  __imp__FEMultiLineText__vfunc_19(ctx, base);
}

// The set-text calls (SetTextKey and the hint widget's SetText end here).
REX_EXTERN(__imp__FEMultiLineText__vfunc_36);
REX_HOOK_RAW(FEMultiLineText__vfunc_36) {
  SetText(ctx, base, bp::kSetText, false, [&] { __imp__FEMultiLineText__vfunc_36(ctx, base); });
}

REX_EXTERN(__imp__FEMultiLineText__vfunc_91);
REX_HOOK_RAW(FEMultiLineText__vfunc_91) {
  SetText(ctx, base, bp::kSetWrapped, true, [&] { __imp__FEMultiLineText__vfunc_91(ctx, base); });
}

REX_EXTERN(__imp__FEMultiLineText__vfunc_92);
REX_HOOK_RAW(FEMultiLineText__vfunc_92) {
  SetText(ctx, base, bp::kSetLines, false, [&] { __imp__FEMultiLineText__vfunc_92(ctx, base); });
}

REX_EXTERN(__imp__FEMultiLineText__vfunc_93);
REX_HOOK_RAW(FEMultiLineText__vfunc_93) {
  SetText(ctx, base, bp::kSetWrappedRaw, true,
          [&] { __imp__FEMultiLineText__vfunc_93(ctx, base); });
}

// A new or deleted text at an address: its old set-text call goes.
REX_EXTERN(__imp__FEMultiLineText__ctor);
REX_HOOK_RAW(FEMultiLineText__ctor) {
  Forget(ctx.r3.u32);
  __imp__FEMultiLineText__ctor(ctx, base);
}

REX_EXTERN(__imp__FEMultiLineText__vfunc_0);
REX_HOOK_RAW(FEMultiLineText__vfunc_0) {
  Forget(ctx.r3.u32);
  __imp__FEMultiLineText__vfunc_0(ctx, base);
}

// [acct2-decomp] A plain FEText's set-text (SetTextKey ends here too): its "~" + character buttons named for its
// player (RawButtonsNamed), and remembered so its draw can set it again.
REX_EXTERN(__imp__FEText__vfunc_36);
REX_HOOK_RAW(FEText__vfunc_36) {
  const uint32_t obj = ctx.r3.u32;
  std::string text = Valid(ctx.r4.u32) ? ReadString(base, ctx.r4.u32, 1024) : std::string();
  if (text.size() >= 1023 || !HasRawButtons(text)) {  // none, or a text cut at 1024
    ForgetPlain(obj);
    __imp__FEText__vfunc_36(ctx, base);
    return;
  }
  PlainCall call;
  call.player = PromptPlayer(base);
  call.mode = Wanted(base, call.player);
  call.gen = g_bind_gen.load(std::memory_order_relaxed);
  const std::string shown = RawButtonsNamed(base, text, call.mode);
  if (shown != text) {
    if (const uint32_t at = CallText(base, shown)) ctx.r4.u64 = at;
    if (REXCVAR_GET(cod3_debug_key_caps)) {
      static std::unordered_map<std::string, int> logged;  // the game thread's
      if (logged.size() < 200 && logged[text + shown]++ == 0)
        REXLOG_INFO("cod3: prompts: plain text \"{}\" of player {} shown as \"{}\"", text, call.player + 1, shown);
    }
  }
  // Only a plain FEText's own set-text is called again (RefreshPlain); a kind of text that ends here from its own
  // set-text just takes the names.
  const bool plain = VirtualSlot(base, obj, bp::kSetText) == kPlainSetText;
  call.text = std::move(text);
  __imp__FEText__vfunc_36(ctx, base);
  std::lock_guard lock(g_mutex);
  if (plain)
    g_plain[obj] = std::move(call);
  else
    g_plain.erase(obj);
}

// [acct2-decomp] FEText::Draw: a plain text whose buttons are out of date is set again first.
REX_EXTERN(__imp__FEText__vfunc_19);
REX_HOOK_RAW(FEText__vfunc_19) {
  RefreshPlain(ctx, base, ctx.r3.u32);
  __imp__FEText__vfunc_19(ctx, base);
}

// [acct2-decomp] A new or deleted text at an address (every kind of text starts and ends as an FEText): its old
// plain set-text call goes.
REX_EXTERN(__imp__FEText__ctor);
REX_HOOK_RAW(FEText__ctor) {
  ForgetPlain(ctx.r3.u32);
  __imp__FEText__ctor(ctx, base);
}

REX_EXTERN(__imp__FEText__ctor_2);
REX_HOOK_RAW(FEText__ctor_2) {
  ForgetPlain(ctx.r3.u32);
  __imp__FEText__ctor_2(ctx, base);
}

REX_EXTERN(__imp__FEText__dtor);
REX_HOOK_RAW(FEText__dtor) {
  ForgetPlain(ctx.r3.u32);
  __imp__FEText__dtor(ctx, base);
}

#if !defined(COD3_MP)
// TitleMenu::Update (every frame on "Press Start").
REX_EXTERN(__imp__sub_824EFB18);
REX_HOOK_RAW(sub_824EFB18) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_824EFB18(ctx, base);
  if (Valid(page)) UpdateTitle(ctx, base, page);
}
#endif
