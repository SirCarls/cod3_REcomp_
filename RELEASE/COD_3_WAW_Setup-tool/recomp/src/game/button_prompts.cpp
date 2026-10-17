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

#include "game/button_prompts.h"

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

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/keybinds/binds.h"  // [keybinds]
#include "game/qte_actions.h"
#include "game/pc_menu.h"

REXCVAR_DEFINE_BOOL(cod3_key_prompts, true, "cod3/Mouse",
                    "Show the bound keyboard and mouse keys (e.g. [R]) instead of controller "
                    "buttons in the game's prompts while the keyboard and mouse are in use "
                    "(game/button_prompts.h)");
REXCVAR_DEFINE_STRING(cod3_glyph_style, "xbox", "cod3/Interface",
                      "Controller buttons in the game's prompts: xbox (the game's own pictures), "
                      "playstation or gamecube (button names until pictures of our own exist)")
    .allowed({"xbox", "playstation", "gamecube"});

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
// Keyboard and mouse, or a controller: the last one pressed wins. Only player
// 1 (pad 0) can have the keyboard and mouse (the SDK gives its keyboard to
// guest user 0); players 2-4 always see controller buttons.
// The UI thread counts key and mouse presses; the game thread watches the
// pad state the game reads and calls it a controller when a button goes down
// with no key or mouse press since the last read.
// ---------------------------------------------------------------------------
enum class Device : uint8_t { kKeyboard, kPad };
std::atomic<Device> g_device{Device::kKeyboard};
std::atomic<uint32_t> g_kbm_presses{0};
std::atomic<int64_t> g_kbm_time{0};
std::atomic<bool> g_mnk_mode{false};   // the SDK's mnk_mode setting
std::atomic<bool> g_mouse_aim{false};  // cod3_raw_mouse or mnk_mouse
std::atomic<uint32_t> g_bind_gen{0};   // + 1 whenever a setting the prompts show changes

enum class Style : uint8_t { kXbox, kPlayStation, kGameCube };
std::atomic<Style> g_style{Style::kXbox};

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
  if (SameText(s, "playstation")) return Style::kPlayStation;
  if (SameText(s, "gamecube")) return Style::kGameCube;
  return Style::kXbox;
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

// X_INPUT_STATE in guest memory (big-endian): u32 packet, u16 buttons,
// u8 left / right trigger, s16 LX, LY, RX, RY.
void WatchPad(uint8_t* base, uint32_t state) {
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
  const uint16_t buttons = ReadU16(base, state + 4);
  const uint8_t triggers[2] = {ReadU8(base, state + 6), ReadU8(base, state + 7)};
  int16_t axes[4];
  for (int i = 0; i < 4; ++i) axes[i] = static_cast<int16_t>(ReadU16(base, state + 8 + 2 * i));
  bool pad = (buttons & ~last_buttons) != 0;
  for (int i = 0; i < 2; ++i) pad |= triggers[i] > 30 && last_triggers[i] <= 30;
  // Sticks only from well off center (no drift).
  for (int i = 0; i < 4; ++i) pad |= std::abs(axes[i]) > 16000 && std::abs(last_axes[i]) <= 16000;
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

// ---------------------------------------------------------------------------
// What the prompts show.
// ---------------------------------------------------------------------------
// The game's own pictures (kXbox), a controller's button names, or keys.
enum class Mode : uint8_t { kXbox, kPlayStation, kGameCube, kMenuKeys, kPlayKeys };

bool IsKeys(Mode m) { return m == Mode::kMenuKeys || m == Mode::kPlayKeys; }
// Every mode but the game's own pictures writes names in the text's font.
bool IsText(Mode m) { return m != Mode::kXbox; }

int PromptPlayer(uint8_t* base) {
  const uint32_t p = ReadU32(base, bp::kClientActiveIndex);
  return p < 4 ? int(p) : 0;
}

Mode Wanted(uint8_t* base) {
  if (REXCVAR_GET(cod3_key_prompts) && g_mnk_mode.load(std::memory_order_relaxed) &&
      PromptPlayer(base) == 0 && g_device.load(std::memory_order_relaxed) == Device::kKeyboard) {
    // MenuActive() is false when cod3_pc_menus is off: then the menus take
    // the keybinds too.
    return cod3::pcmenu::MenuActive() ? Mode::kMenuKeys : Mode::kPlayKeys;
  }
  switch (g_style.load(std::memory_order_relaxed)) {
    case Style::kPlayStation: return Mode::kPlayStation;
    case Style::kGameCube: return Mode::kGameCube;
    default: return Mode::kXbox;
  }
}

// One button of the game's button font. Data, so other styles and pictures
// can be added here: the game's character, the keybind (play), the key the
// PC menus take (nullptr: the keybind there too) and the names per style
// (nullptr: that controller has no such button; the Xbox name shows).
struct Button {
  char glyph;
  int bind;  // index into kBindCvars, -1: made of the D-pad binds
  const char* menu_key;
  const char* xbox;
  const char* playstation;
  const char* gamecube;
};
constexpr Button kButtons[] = {
    {'"', 0, "ENTER", "A", "CROSS", "A"},
    {'%', 1, "ESC", "B", "CIRCLE", "B"},
    {'#', 2, nullptr, "X", "SQUARE", "X"},
    {'$', 3, nullptr, "Y", "TRIANGLE", "Y"},
    {'&', 4, nullptr, "LT", "L2", "L"},
    {'!', 5, nullptr, "RT", "R2", "R"},
    {'\'', 6, nullptr, "LB", "L1", nullptr},
    {'(', 7, nullptr, "RB", "R1", "Z"},
    {'-', 8, "ENTER", "START", "START", "START"},
    {'1', 9, nullptr, "BACK", "SELECT", nullptr},
    {'2', 10, nullptr, "LS", "L3", nullptr},
    {'3', 11, nullptr, "RS", "R3", nullptr},
    {')', 12, "UP", "UP", "UP", "UP"},
    {',', 13, "DOWN", "DOWN", "DOWN", "DOWN"},
    {'+', 14, "LEFT", "LEFT", "LEFT", "LEFT"},
    {'*', 15, "RIGHT", "RIGHT", "RIGHT", "RIGHT"},
    {'/', -1, "UP/DOWN", "UP/DOWN", "UP/DOWN", "UP/DOWN"},
    {'.', -1, "LEFT/RIGHT", "LEFT/RIGHT", "LEFT/RIGHT", "LEFT/RIGHT"},
    {'0', -1, "ARROWS", "D-PAD", "D-PAD", "D-PAD"},
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
// and a 256-byte text buffer.
constexpr uint32_t kSlotSize = 32;
constexpr uint32_t kTextSize = 256;
uint32_t g_guest = 0;

uint32_t GuestScratch() {
  if (!g_guest) {
    auto* mem = rex::system::kernel_state()->memory();
    g_guest = mem->SystemHeapAlloc(kSlotSize * (kButtonCount + 1) + kTextSize);
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
  const uint32_t at = scratch + kSlotSize * uint32_t(index);
  std::memcpy(base + at, text.c_str(), text.size() + 1);
  return at;
}

// A guest copy of a text for the game to read right away (one at a time).
uint32_t TempText(uint8_t* base, const std::string& text) {
  const uint32_t scratch = GuestScratch();
  if (!scratch || text.size() >= kTextSize) return 0;
  const uint32_t at = scratch + kSlotSize * (kButtonCount + 1);
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
// Lines laid out with names: they are drawn with the text font too.
std::unordered_map<uint32_t, bool> g_text_lines;
// The last set-text call of each text that had buttons, to lay it out again.
struct TextCall {
  uint32_t slot = 0;        // the virtual slot called (bp::kSet*)
  bool string_arg = false;  // takes a String** (else a const char*)
  std::string text;         // before the game's "[ACTION]" pass
  uint32_t width = 0;
  Mode mode = Mode::kXbox;
  uint32_t gen = 0;         // g_bind_gen it was laid out with
};
std::unordered_map<uint32_t, TextCall> g_texts;

Mode LayoutMode(uint8_t* base) { return t_set_depth ? t_set_mode : Wanted(base); }

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
  call.mode = t_set_mode = Wanted(base);
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
    const Mode want = Wanted(base);
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
  c.Invoke(VirtualSlot(base, obj, call.slot), {obj, arg, call.width});
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
  const std::string look = mouse ? "the mouse" : look_keys.empty() ? "" : "[" + look_keys + "]";
  if (stick == 1) return move.empty() ? "" : "Use [" + move + "]";
  if (stick == 2) return look.empty() ? "" : "Use " + look;
  if (move.empty() || look.empty()) return "";
  return "Use [" + move + "] and " + look;
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
  if (form == 0) return "[" + key + "]";
  std::string verb = GameText(ctx, base, form == 3 ? "INGAME_PRESS_AND_HOLD" : "INGAME_PRESS");
  if (verb.empty()) verb = form == 3 ? "Press and hold" : "Press";
  return verb + " [" + key + "]";
}

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
  const bool keys = IsKeys(Wanted(base));
  if (keys == (shown == keys_text)) return;
  Call c(ctx, base);
  if (keys)
    c.Invoke(VirtualSlot(base, elem, bp::kSetText), {elem, c.PushString(keys_text)});
  else
    c.Invoke(VirtualSlot(base, elem, bp::kSetTextKey), {elem, bp::kTitlePressStartKey});
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
}

void Detach(rex::ui::Window* window) {
  if (window && g_window == window) window->RemoveInputListener(&g_watch);
  g_window = nullptr;
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
  if (user == 0 && ctx.r3.u32 == 0 && Valid(state)) WatchPad(base, state);
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
  const uint32_t out = ctx.r4.u32;
  __imp__sub_824C68E0(ctx, base);
  if (ctx.r3.u32 == 0 || !Valid(out)) return;  // not a button token
  ++t_tokens;
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
    g_text_lines[line] = true;
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
  bool names;
  {
    std::lock_guard lock(g_mutex);
    names = g_text_lines.count(line) != 0;
  }
  const uint32_t font = names ? TextFont(base, ReadU32(base, line + bp::kLineFont)) : 0;
  if (!font) {
    __imp__sub_824D7D88(ctx, base);
    return;
  }
  const double baseline = double(int32_t(ReadU32(base, font + bp::kFontBaseline)));
  ctx.f4.f64 = ctx.f2.f64;                            // the text's x scale
  ctx.f5.f64 = baseline * (ctx.f2.f64 - ctx.f3.f64);  // raise: baselines meet
  WithButtonFont(base, font, [&] { __imp__sub_824D7D88(ctx, base); });
}

// "[ACTION]" -> its button sentence: with keys, the stick words ("Use
// [WASD]", "Use the mouse") and "Press" for "Pull" / "Click".
REX_EXTERN(__imp__sub_824DD9A8);
REX_HOOK_RAW(sub_824DD9A8) {
  const std::string action = Valid(ctx.r3.u32) ? ReadString(base, ctx.r3.u32, 64) : "";
  __imp__sub_824DD9A8(ctx, base);
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

// The whole text after its "[ACTION]" pass (a static 512-byte buffer): with
// keys, "Pull ~r1" -> "Press ~r1".
REX_EXTERN(__imp__sub_824DE290);
REX_HOOK_RAW(sub_824DE290) {
  __imp__sub_824DE290(ctx, base);
  const uint32_t buf = ctx.r3.u32;
  if (!Valid(buf) || !IsKeys(LayoutMode(base))) return;
  const std::string text = ReadString(base, buf, 512);
  const std::string out = KeyVerbs(ctx, base, text);
  if (out != text && out.size() < 510) std::memcpy(base + buf, out.c_str(), out.size() + 1);
}

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

// TitleMenu::Update (every frame on "Press Start").
REX_EXTERN(__imp__sub_824EFB18);
REX_HOOK_RAW(sub_824EFB18) {
  const uint32_t page = ctx.r3.u32;
  __imp__sub_824EFB18(ctx, base);
  if (Valid(page)) UpdateTitle(ctx, base, page);
}
