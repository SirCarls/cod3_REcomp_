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
// [acct2-decomp] 2026-10-25, owner 23:42-23:43: keys are drawn as key caps
// like "SPACE Skip" (cod3_key_caps: the game draws each name invisibly, the
// overlay draws the cap over its room, KeyCap); our own key names (stick
// words, an action's key, the vehicle view hint's "Press [V]") are tokens the
// game's token pass takes like its buttons (KeyToken), so they are caps and
// follow device switches too; the cutscene skip's own controller reads count
// for the switch (NotePadState), and its prompt asks FirstPlayerLook.

#include "game/button_prompts.h"

#include <algorithm>  // [claude-9b] std::min, the key caps' fade
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
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

#include "game/addresses.h"
#include "game/guest.h"
#include "game/keybinds/binds.h"  // [keybinds]
#include "game/qte_actions.h"
#include "game/rumble.h"  // [rumble]
#include "game/pc_menu.h"

REXCVAR_DEFINE_BOOL(cod3_key_prompts, true, "cod3/Mouse",
                    "Show the bound keyboard and mouse keys (e.g. [R]) instead of controller "
                    "buttons in the game's prompts while the keyboard and mouse are in use "
                    "(game/button_prompts.h)");
REXCVAR_DEFINE_STRING(cod3_glyph_style, "auto", "cod3/Interface",
                      "Controller buttons in the game's prompts: auto (each player's own controller: "
                      "an Xbox pad the game's pictures, a PlayStation or Nintendo pad its button "
                      "names), xbox (the game's own pictures), playstation, nintendo or gamecube "
                      "(button names until pictures of our own exist)")
    .allowed({"auto", "xbox", "playstation", "nintendo", "gamecube"});
// [acct2-decomp] Owner 2026-10-25 23:42: "the keyboard glyphs need to match
// the 'SPACE to skip' button".
REXCVAR_DEFINE_BOOL(cod3_key_caps, true, "cod3/Interface",
                    "Draw the keys in the game's prompts as key caps, like the \"SPACE Skip\" "
                    "prompt's (else the key's name in brackets, [R])");
// [claude-9b-think] 2026-10-27 UNTESTED (owner 10:27: the key caps fade out a little after their text; stick words and
// pictures still show with the keyboard and mouse): what the key caps and the stick words do, for a test run.
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
enum class Style : uint8_t { kAuto, kXbox, kPlayStation, kNintendo, kGameCube };
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

class KeyboardWatch final : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!e.prev_state()) KeyboardUsed("a key");
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
enum class Mode : uint8_t { kXbox, kPlayStation, kNintendo, kGameCube, kMenuKeys, kPlayKeys };
constexpr uint32_t kModeCount = 6;

bool IsKeys(Mode m) { return m == Mode::kMenuKeys || m == Mode::kPlayKeys; }
// Every mode but the game's own pictures writes names in the text's font.
bool IsText(Mode m) { return m != Mode::kXbox; }

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
struct Button {
  char glyph;
  int bind;  // index into kBindCvars, -1: made of the D-pad binds
  const char* menu_key;
  const char* xbox;
  const char* playstation;
  const char* nintendo;
  const char* gamecube;
};
constexpr Button kButtons[] = {
    {'"', 0, "ENTER", "A", "CROSS", "B", "A"},
    {'%', 1, "ESC", "B", "CIRCLE", "A", "B"},
    {'#', 2, nullptr, "X", "SQUARE", "Y", "X"},
    {'$', 3, nullptr, "Y", "TRIANGLE", "X", "Y"},
    {'&', 4, nullptr, "LT", "L2", "ZL", "L"},
    {'!', 5, nullptr, "RT", "R2", "ZR", "R"},
    {'\'', 6, nullptr, "LB", "L1", "L", nullptr},
    {'(', 7, nullptr, "RB", "R1", "R", "Z"},
    {'-', 8, "ENTER", "START", "START", "+", "START"},
    {'1', 9, nullptr, "BACK", "SELECT", "-", nullptr},
    {'2', 10, nullptr, "LS", "L3", nullptr, nullptr},
    {'3', 11, nullptr, "RS", "R3", nullptr, nullptr},
    {')', 12, "UP", "UP", "UP", "UP", "UP"},
    {',', 13, "DOWN", "DOWN", "DOWN", "DOWN", "DOWN"},
    {'+', 14, "LEFT", "LEFT", "LEFT", "LEFT", "LEFT"},
    {'*', 15, "RIGHT", "RIGHT", "RIGHT", "RIGHT", "RIGHT"},
    {'/', -1, "UP/DOWN", "UP/DOWN", "UP/DOWN", "UP/DOWN", "UP/DOWN"},
    {'.', -1, "LEFT/RIGHT", "LEFT/RIGHT", "LEFT/RIGHT", "LEFT/RIGHT", "LEFT/RIGHT"},
    {'0', -1, "ARROWS", "D-PAD", "D-PAD", "D-PAD", "D-PAD"},
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
    if (!part.empty()) out += (out.empty() ? "" : "+") + KeyName(part);
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

// Guest memory for our strings: one 32-byte slot per button (+1 for "[?]")
// per mode, and a 256-byte text buffer. [mp-menus] Per mode, because a laid
// out line keeps pointing at its names: split-screen players with different
// controllers (or one on the keyboard) each keep their own.
constexpr uint32_t kSlotSize = 32;
constexpr uint32_t kTextSize = 256;
constexpr uint32_t kSlotsPerMode = uint32_t(kButtonCount) + 1;
uint32_t g_guest = 0;

uint32_t GuestScratch() {
  if (!g_guest) {
    auto* mem = rex::system::kernel_state()->memory();
    g_guest = mem->SystemHeapAlloc(kSlotSize * kSlotsPerMode * kModeCount + kTextSize);
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
    if (mode == Mode::kPlayKeys) name = cod3::binds::KeyNameForPadKey(base, PadKeyFor(glyph));
    if (name.empty()) name = NameFor(kButtons[i], mode);
    text = "[" + name + "]";
    break;
  }
  if (text.size() > kSlotSize - 1) text.resize(kSlotSize - 1);
  const uint32_t mode_slot = uint32_t(mode) < kModeCount ? uint32_t(mode) : 0;
  const uint32_t at = scratch + kSlotSize * (mode_slot * kSlotsPerMode + uint32_t(index));
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
  std::string shown = "[" + name + "]";
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
  const uint32_t at = scratch + kSlotSize * kSlotsPerMode * kModeCount;
  std::memcpy(base + at, text.c_str(), text.size() + 1);
  return at;
}

// The game's own words (Localize), cached.
std::string GameText(PPCContext& ctx, uint8_t* base, const std::string& key) {
  static std::mutex mutex;
  static std::unordered_map<std::string, std::string> cache;
  static uint32_t cached_language = ~0u;
  const uint32_t language = ReadU32(base, bp::kLanguageMode);
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
void NoteKeyCaps(uint8_t* base, uint32_t line, uint32_t font, float y_scale, float alpha) {
  namespace pm = cod3::addr::pc_menu;
  const int32_t n = int32_t(ReadU32(base, line + pm::kLineSegmentCount));
  const uint32_t segments = ReadU32(base, line + pm::kLineSegments);
  const uint32_t string = ReadU32(base, line + pm::kLineString);
  DrawnCaps drawn;
  drawn.ms = NowMs();
  drawn.frame = ReadU32(base, pm::kFrameCount);  // [claude-9b-think]
  NoteGameFrame(base);
  if (n > 0 && n <= 64 && Valid(segments) && Valid(string)) {
    const std::string text = ReadString(base, string + bp::kStringChars, 512);
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
        if (name.front() != '[' || name.back() != ']') continue;  // not one of our names
        name = name.substr(1, name.size() - 2);
        const float x0 = x + float(entry(k, 1));
        const float x1 = k + 1 < n ? x + float(entry(k + 1, 1)) : x + width;
        if (!(x1 > x0)) continue;
        drawn.caps.push_back({x0, y, x1, y + height, std::move(name), alpha});
      }
    }
  }
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
    static int64_t window = 0;
    static int lines = 0;
    if (drawn.ms - window >= 1000) window = drawn.ms, lines = 0;
    if (lines++ < 30)
      REXLOG_INFO("cod3: key caps: line {:08X} drawn in game frame {} (every {}), {} caps at opacity {:.3f}", line,
                  drawn.frame, drawn.cadence, drawn.caps.size(), alpha);
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
  const std::string key = cod3::binds::KeyNameForActionWord(word);
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
  const std::string key = cod3::binds::KeyNameForAction("veh_view");
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
        std::string key = cod3::binds::KeyNameForPadKey(base, p.stick == 1 ? 220 : 219);
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
    case Style::kNintendo: mode = Mode::kNintendo; break;
    case Style::kGameCube: mode = Mode::kGameCube; break;
    default: mode = UserPadMode(0); break;  // the first controller (guest user 0)
  }
  switch (mode) {
    case Mode::kPlayStation: return Look::kPlayStation;
    case Mode::kNintendo: return Look::kNintendo;
    case Mode::kGameCube: return Look::kGameCube;
    default: return Look::kXbox;
  }
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
    return;
  }
  ctx.f2.f64 = ctx.f1.f64;  // names at the text's scale
  const bool outer_names = t_names;
  const Mode outer_mode = t_name_mode;
  t_names = true, t_name_mode = mode;
  const uint32_t tokens = t_tokens;
  WithButtonFont(base, font, [&] { __imp__sub_824D7650(ctx, base); });
  t_names = outer_names, t_name_mode = outer_mode;
  std::lock_guard lock(g_mutex);
  if (t_tokens != tokens) {
    // Practically never this many; a forgotten line only loses its names.
    if (g_text_lines.size() > 50000) g_text_lines.clear();
    g_text_lines[line] = mode;
  } else {
    g_text_lines.erase(line);
  }
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
  WithButtonFont(base, font, [&] { __imp__sub_824D7B48(ctx, base); });
  t_names = outer_names, t_name_mode = outer_mode;
}

// Draws one line: names in the line's font at the text's x scale, standing on
// the text's baseline (the text may be drawn taller than wide).
REX_EXTERN(__imp__sub_824D7D88);
REX_HOOK_RAW(sub_824D7D88) {
  const uint32_t line = ctx.r3.u32;
  cod3::pcmenu::NoteLineDrawn(base, line, ctx.f3.f64);  // footer prompts can be clicked (pc_menu.h)
  NoteGameFrame(base);  // [claude-9b-think] any line drawn: the key caps' newest game frame
  bool names;
  Mode mode = Mode::kXbox;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_text_lines.find(line);
    names = it != g_text_lines.end();
    if (names) mode = it->second;
  }
  const uint32_t font = names ? TextFont(base, ReadU32(base, line + bp::kLineFont)) : 0;
  // [acct2-decomp] A line not drawn with keys now has no key caps.
  if (!font || !IsKeys(mode) || !REXCVAR_GET(cod3_key_caps)) {
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
  if (IsKeys(mode) && REXCVAR_GET(cod3_key_caps)) {
    const uint32_t color = ctx.r6.u32;
    // [claude-9b] The cap fades with the line's text (owner 2026-10-26: the keyboard and mouse glyphs
    // didn't follow their prompt's fade-out; the glyph alone showed for a moment): the game fades a
    // prompt through its text color (r5, the text segments' color in this function) while the
    // button color (r6) can stay opaque, so the cap takes the fainter of the two.
    const uint32_t text_alpha = ctx.r5.u32 >> 24, button_alpha = color >> 24;
    NoteKeyCaps(base, line, font, float(ctx.f3.f64), float(std::min(text_alpha, button_alpha)) / 255.0f);
    ctx.r6.u64 = color & 0x00FFFFFFu;
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
}

// The whole text after its "[ACTION]" pass (a static 512-byte buffer): with
// keys, "Pull ~r1" -> "Press ~r1", and [acct2-decomp] the vehicle view hint's
// words -> "Press [V]" (ViewSwitchKeys).
REX_EXTERN(__imp__sub_824DE290);
REX_HOOK_RAW(sub_824DE290) {
  __imp__sub_824DE290(ctx, base);
  const uint32_t buf = ctx.r3.u32;
  if (!Valid(buf)) return;
  const std::string text = ReadString(base, buf, 512);
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
  if (out != text && out.size() < 510) std::memcpy(base + buf, out.c_str(), out.size() + 1);
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

#if !defined(COD3_MP)
// TitleMenu::Update (every frame on "Press Start").
REX_EXTERN(__imp__sub_824EFB18);
REX_HOOK_RAW(sub_824EFB18) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_824EFB18(ctx, base);
  if (Valid(page)) UpdateTitle(ctx, base, page);
}
#endif
