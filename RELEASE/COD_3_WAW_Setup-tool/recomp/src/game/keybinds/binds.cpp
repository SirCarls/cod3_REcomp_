// cod3 - the campaign's own keyboard and mouse actions (see binds.h).
//
// Game side (default.xex; read in the disassembly, notes in dev/agents/keybinds/):
//   sub_82522138           the pad poll, once before every Com_Frame on the
//                          game's main thread (hooked here). It takes button
//                          presses only while in_joystick is on, byte
//                          0x82A2A17D is 0 and no front-end menu blocks the
//                          local client (*(g_FEManager+32): byte +28+cl set and
//                          +12 != 3); releases always pass.
//   IN_BinocularsDown/Up   0x8250CDB0 / 0x8250CD68  (+binoculars, key and time
//                          ignored: raise while held, steady once up)
//   IN_SprintbreathDown/Up 0x8250A040 / 0x8250A038  (empty in the retail game;
//                          game/movement fills them in)
//   IN_HoldbreathDown/Up   0x8250CD30 / 0x8250CCF8
//   IN_SprintDown/Up       0x8250A030 / 0x8250A028  (game/movement)
//   Sys_Milliseconds       0x82468510
//   cmd_functions          0x829EBA10: cmd_function_t {+0 next, +4 name,
//                          +8 type, +12 func} (the startup check)
//   keys                   0x82A59570 + cl * 2052: 256 x {+0 down, +4 char*
//                          binding} (the game's own bind table: controller
//                          buttons only, from xenon_a..d.cfg)
//   pad keys               X 211, A 212, B 213, Y 214, RT 215, LT 216, RB 217,
//                          LB 218, RS 219, LS 220, D-pad 154-157 (up, down,
//                          left, right), START 27, BACK 9 (sub_8251DA00)
//   input                  *0x8290A910: +28 a pad pressed Start, +24 which;
//                          the pad poll reads that pad, else pad 0
//   player state           (*0x82A2AB40)[cl + 1] -> +528 -> +140 the zoom
//                          (1.0 = fully through the sights or the binoculars:
//                          IN_BinocularsDown steadies then), +1340 bit 0 hold
//                          breath; kbutton 15 (sprint) at 0x829C8E68 + 15 * 24,
//                          +16 active
//   action words           "[HOLD_BINOCS]" ... (table 0x82A8DFF8): BINOCS is
//                          +binoculars, SPRINT +sprint

#include "game/keybinds/binds.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "game/addresses.h"
#include "game/cheat_guard.h"
#include "game/decomp_trace.h"
#include "game/guest.h"
#include "game/keybinds/model.h"
#include "game/screenshot.h"
#include "ui/theme.h"

REXCVAR_DEFINE_STRING(cod3_binds_kbm_p1, "", "cod3/Binds",
                      "Keyboard and mouse keys of the game's own actions (binoculars, sprint / hold breath, "
                      "hold breath, sprint), e.g. binoculars=B;sprintbreath=SHIFT;holdbreath=;sprint= "
                      "(empty = the defaults; F2 > Controls or the console's bind command set it)");
REXCVAR_DEFINE_BOOL(cod3_debug_binds, false, "cod3",
                    "Log each key bind action as it reaches the game, and the binoculars, hold breath and "
                    "sprint state (with a screenshot of the game's picture when the binoculars are up)");
REXCVAR_DEFINE_INT32(cod3_binds_version, 0, "cod3",
                     "Which one-time changes of the key binds have run (set by the game)");

namespace cod3::binds {

namespace {

using guest::Call;
using guest::ReadF32;
using guest::ReadS32;
using guest::ReadString;
using guest::ReadU32;
using guest::ReadU8;
using guest::WriteU16;
using guest::WriteU8;

// --- the game (default.xex) ------------------------------------------------------------
constexpr uint32_t kSysMilliseconds = 0x82468510;
constexpr uint32_t kCmdFunctions = 0x829EBA10;
constexpr uint32_t kInJoystick = 0x829C295C;  // dvar_t*; int value at +32
constexpr uint32_t kPadPollOff = 0x82A2A17D;  // byte: the pad poll does nothing
constexpr uint32_t kFeManager = 0x82A8DF2C;   // g_FEManager
constexpr uint32_t kKeys = 0x82A59570;        // qkey_t keys[local client][256]
constexpr uint32_t kKeysStride = 2052;
constexpr uint32_t kInput = 0x8290A910;       // the input object*
constexpr uint32_t kEntityList = 0x82A2AB40;  // gentity*[] (index cl + 1)
constexpr uint32_t kSprintKbutton = 0x829C8E68 + 15 * 24;
constexpr uint32_t kBinocularsRequest = 0x82A578F4;  // bit 2 while +binoculars is held (not zoomed yet)
constexpr uint32_t kActionWordText = 0x824DD9A8;      // "[HOLD_BINOCS]" -> its button sentence
constexpr int kKeyAux1 = 239;  // AUX1, AUX2: key ids no controller sends
constexpr int32_t kBindsVersion = 1;  // cod3_binds_version after the move from keybind_*

struct Handlers {
  uint32_t down, up;
};
// By Action (model.h).
constexpr Handlers kHandlers[kActionCount] = {
    {0x8250CDB0, 0x8250CD68},  // +binoculars
    {0x8250A040, 0x8250A038},  // +sprintbreath
    {0x8250CD30, 0x8250CCF8},  // +holdbreath
    {0x8250A030, 0x8250A028},  // +sprint
};
static_assert(sizeof(kHandlers) / sizeof(kHandlers[0]) == static_cast<size_t>(kActionCount));

// A controller input as the game's pad key and as the XInput state.
struct PadInput {
  int key;
  uint16_t button;  // XInput button bit, 0 for a trigger
  uint8_t trigger;  // 1 left, 2 right
  const char* name;
};
constexpr PadInput kPadInputs[] = {
    {220, 0x0040, 0, "LS"},     {219, 0x0080, 0, "RS"},    {211, 0x4000, 0, "X"},
    {212, 0x1000, 0, "A"},      {213, 0x2000, 0, "B"},     {214, 0x8000, 0, "Y"},
    {215, 0, 2, "RT"},          {216, 0, 1, "LT"},         {217, 0x0200, 0, "RB"},
    {218, 0x0100, 0, "LB"},     {154, 0x0001, 0, "D-pad up"}, {155, 0x0002, 0, "D-pad down"},
    {156, 0x0004, 0, "D-pad left"}, {157, 0x0008, 0, "D-pad right"}, {27, 0x0010, 0, "START"},
    {9, 0x0020, 0, "BACK"},
};

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }

bool SameText(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool Debug() { return REXCVAR_GET(cod3_debug_binds); }

uint32_t LocalClient(uint8_t* base) {
  const uint32_t cl = ReadU32(base, addr::kActiveLocalClient);
  return cl < 4 ? cl : 0;
}

// The game's binding of a controller key ("+binoculars"), "" when none.
std::string Binding(uint8_t* base, uint32_t cl, int pad_key) {
  if (pad_key <= 0 || pad_key > 255) return "";
  const uint32_t text = ReadU32(base, kKeys + cl * kKeysStride + uint32_t(pad_key) * 8 + 4);
  return Valid(text) ? ReadString(base, text, 64) : "";
}

// The game's action words ("[HOLD_BINOCS]" -> BINOCS) of these actions.
struct GameWord {
  const char* word;
  const char* command;
};
constexpr GameWord kGameWords[] = {{"BINOCS", "+binoculars"}, {"SPRINT", "+sprint"}};

// --- the keys (any thread) ---------------------------------------------------------------
std::mutex g_mutex;          // g_set, g_last_text
KbmSet g_set = Defaults();
std::string g_last_text = "\x01";  // the cvar text g_set was read from
std::atomic<uint32_t> g_generation{1};
// KeyCode -> (action + 1) | slot << 8, 0 = no action. Read by the listener.
std::array<std::atomic<uint16_t>, kKeyCodeCount> g_lookup{};
std::once_flag g_started;

void Load(std::string_view text) {
  std::vector<std::string> notes;
  const KbmSet set = Parse(text, &notes);
  {
    std::lock_guard lock(g_mutex);
    if (text == g_last_text) return;
    g_last_text = std::string(text);
    g_set = set;
    for (auto& v : g_lookup) v.store(0, std::memory_order_relaxed);
    for (int a = 0; a < kActionCount; ++a)
      for (int s = 0; s < kSlots; ++s)
        if (const KeyCode k = set.key[a][s]; k != kNoKey && k < kKeyCodeCount)
          g_lookup[k].store(static_cast<uint16_t>((a + 1) | (s << 8)), std::memory_order_relaxed);
  }
  for (const auto& n : notes) REXLOG_WARN("cod3: key binds: {}", n);
  g_generation.fetch_add(1, std::memory_order_release);
  std::string keys;
  for (int a = 0; a < kActionCount; ++a) {
    const std::string k = KeysText(set, static_cast<Action>(a));
    keys += std::string(a ? ", " : "") + Def(static_cast<Action>(a)).id + " " + (k.empty() ? "-" : k);
  }
  REXLOG_INFO("cod3: key binds: {}", keys);
}

void EnsureStarted() {
  std::call_once(g_started, [] {
    rex::cvar::RegisterChangeCallback(kSettingName,
                                      [](std::string_view, std::string_view value) { Load(value); });
    Load(rex::cvar::GetFlagByName(kSettingName));
  });
}

KbmSet CurrentSet() {
  EnsureStarted();
  std::lock_guard lock(g_mutex);
  return g_set;
}

// Writes the keys to the setting (its change callback loads them).
void Store(const KbmSet& set) {
  rex::cvar::SetFlagByName(kSettingName, Serialize(set));
  Load(rex::cvar::GetFlagByName(kSettingName));
}

// "" when the key can be bound, else why not.
std::string ReservedReason(KeyCode key) {
  if (key == kKeyBacktick) return "` / ~ opens the console";
  if (key == kKeyEscape) return "ESCAPE is the pause menu";
  if (key == kKeyF1 + 1) return "F2 opens the settings window";
  for (const char* name : {"bind_debug_overlay", "bind_settings", "bind_achievements", "bind_console"}) {
    const std::string v = rex::cvar::GetFlagByName(name);
    if (!v.empty() && ParseKey(v) == key) return KeyName(key) + " opens an overlay (" + name + ")";
  }
  if (trace::kEnabled && key == kKeyF1 + 7) return "F8 is the trace build's marker key";
  return "";
}

// The keybind_* settings (the keyboard pressing controller buttons) and what
// each button does in the campaign's default layout.
struct OldBindName {
  const char* name;
  const char* label;
  bool moved;  // its keys moved to an action (model.h Migrate)
};
constexpr OldBindName kOldBinds[] = {
    {"keybind_a", "Jump (A)", false},
    {"keybind_b", "Stance (B)", false},
    {"keybind_x", "Use/Reload (X)", false},
    {"keybind_y", "Switch weapon (Y)", false},
    {"keybind_left_trigger", "Aim (LT)", false},
    {"keybind_right_trigger", "Fire (RT)", false},
    {"keybind_left_shoulder", "Smoke grenade (LB)", false},
    {"keybind_right_shoulder", "Grenade (RB)", false},
    {"keybind_lstick_up", "Forward", false},
    {"keybind_lstick_down", "Back", false},
    {"keybind_lstick_left", "Strafe left", false},
    {"keybind_lstick_right", "Strafe right", false},
    {"keybind_lstick_press", "Left stick click (LS)", true},
    {"keybind_rstick_up", "Look up", false},
    {"keybind_rstick_down", "Look down", false},
    {"keybind_rstick_left", "Look left", false},
    {"keybind_rstick_right", "Look right", false},
    {"keybind_rstick_press", "Melee (RS)", false},
    {"keybind_dpad_up", "D-pad up", true},
    {"keybind_dpad_down", "D-pad down", true},
    {"keybind_dpad_left", "D-pad left", true},
    {"keybind_dpad_right", "D-pad right", true},
    {"keybind_back", "Objectives (Back)", false},
    {"keybind_start", "Pause (Start)", false},
    {"keybind_guide", "Guide", false},
};

// The controller row whose keys include `key` (the action takes the key
// first, so that row no longer gets it), or nullptr.
const OldBindName* ControllerRowWith(KeyCode key) {
  for (const OldBindName& b : kOldBinds) {
    const std::string v = rex::cvar::GetFlagByName(b.name);
    size_t start = 0;
    while (start <= v.size()) {
      size_t end = v.find(',', start);
      if (end == std::string::npos) end = v.size();
      const std::string_view token(v.data() + start, end - start);
      if (!IsCombination(token) && ParseKey(token) == key) return &b;
      start = end + 1;
    }
  }
  return nullptr;
}

// --- UI thread -> game thread ----------------------------------------------------------
enum class Kind : uint8_t { kDown, kUp, kTap };
struct Event {
  uint8_t action;
  uint8_t slot;
  Kind kind;
  uint16_t code;
};
// One producer (the UI thread), one consumer (the game thread).
constexpr uint32_t kRingSize = 128;
std::array<Event, kRingSize> g_ring;
std::atomic<uint32_t> g_ring_head{0}, g_ring_tail{0};
// A release that didn't fit in the ring: (action, slot) bits, never lost.
std::atomic<uint32_t> g_release_overflow{0};

void Push(const Event& e) {
  const uint32_t head = g_ring_head.load(std::memory_order_relaxed);
  if (head - g_ring_tail.load(std::memory_order_acquire) >= kRingSize) {
    if (e.kind == Kind::kUp)
      g_release_overflow.fetch_or(1u << (e.action * kSlots + e.slot), std::memory_order_release);
    return;  // a press is dropped when full
  }
  g_ring[head % kRingSize] = e;
  g_ring_head.store(head + 1, std::memory_order_release);
}

bool Pop(Event* out) {
  const uint32_t tail = g_ring_tail.load(std::memory_order_relaxed);
  if (tail == g_ring_head.load(std::memory_order_acquire)) return false;
  *out = g_ring[tail % kRingSize];
  g_ring_tail.store(tail + 1, std::memory_order_release);
  return true;
}

std::atomic<bool> g_mnk_mode{true};

class Listener final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  rex::ui::Window* window = nullptr;
  std::function<bool()> overlay_open;

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    const uint32_t vk = static_cast<uint32_t>(e.virtual_key());
    if (vk == 0 || vk > 0xFF) return;
    if (Down(static_cast<KeyCode>(vk), e.prev_state())) e.set_handled(true);
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    const uint32_t vk = static_cast<uint32_t>(e.virtual_key());
    if (vk == 0 || vk > 0xFF) return;
    Up(static_cast<KeyCode>(vk));  // never taken: the others see every release
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (const KeyCode code = MouseCode(e.button()); code && Down(code, false)) e.set_handled(true);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (const KeyCode code = MouseCode(e.button())) Up(code);
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (e.scroll_y() == 0 || !Acting()) return;
    const KeyCode code = e.scroll_y() > 0 ? kWheelUp : kWheelDown;
    const uint16_t v = g_lookup[code].load(std::memory_order_relaxed);
    if (!v) return;
    int detents = std::abs(e.scroll_y()) / int(rex::ui::MouseEvent::kScrollPerDetent);
    for (int i = 0; i < std::max(1, std::min(detents, 4)); ++i)
      Push({uint8_t((v & 0xFF) - 1), uint8_t(v >> 8), Kind::kTap, code});
    e.set_handled(true);
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override { ReleaseAll("the window lost focus"); }

  // Every UI frame.
  void Update() {
    const bool blocked = !g_mnk_mode.load(std::memory_order_relaxed) || (overlay_open && overlay_open()) ||
                         (window && !window->HasFocus());
    if (blocked && !blocked_) ReleaseAll("an overlay opened, or keyboard and mouse were turned off");
    blocked_ = blocked;
  }

  void ReleaseAll(const char* why) {
    bool any = false;
    for (size_t code = 0; code < kKeyCodeCount; ++code) {
      if (!pressed_[code]) continue;
      Push({uint8_t((pressed_[code] & 0xFF) - 1), uint8_t(pressed_[code] >> 8), Kind::kUp, uint16_t(code)});
      pressed_[code] = 0;
      any = true;
    }
    if (any && Debug()) REXLOG_INFO("cod3: binds: keys let go ({})", why);
  }

 private:
  static KeyCode MouseCode(rex::ui::MouseEvent::Button b) {
    switch (b) {
      case rex::ui::MouseEvent::Button::kLeft: return kMouse1;
      case rex::ui::MouseEvent::Button::kRight: return kMouse2;
      case rex::ui::MouseEvent::Button::kMiddle: return kMouse3;
      case rex::ui::MouseEvent::Button::kX1: return kMouse4;
      case rex::ui::MouseEvent::Button::kX2: return kMouse5;
      default: return kNoKey;
    }
  }

  bool Acting() {
    const bool blocked = !g_mnk_mode.load(std::memory_order_relaxed) || (overlay_open && overlay_open());
    if (blocked && !blocked_) ReleaseAll("an overlay opened");
    blocked_ = blocked || (window && !window->HasFocus());
    return !blocked_;
  }

  // True when the key is an action's (it is taken: nothing below sees it).
  bool Down(KeyCode code, bool repeat) {
    if (!Acting()) return false;
    const uint16_t v = g_lookup[code].load(std::memory_order_relaxed);
    if (!v) return false;
    if (repeat || pressed_[code]) return true;
    pressed_[code] = v;
    Push({uint8_t((v & 0xFF) - 1), uint8_t(v >> 8), Kind::kDown, code});
    return true;
  }

  void Up(KeyCode code) {
    const uint16_t v = pressed_[code];
    if (!v) return;
    pressed_[code] = 0;
    Push({uint8_t((v & 0xFF) - 1), uint8_t(v >> 8), Kind::kUp, code});
  }

  uint16_t pressed_[kKeyCodeCount] = {};  // the action each held key pressed
  bool blocked_ = false;
};
Listener g_listener;

// --- game thread: the pad poll ------------------------------------------------------------
enum class Mode : uint8_t { kNone, kDirect, kPad };
struct Slot {
  Mode mode = Mode::kNone;
  uint16_t buttons = 0;  // kPad: XInput bits
  uint8_t triggers = 0;  // kPad: 1 left, 2 right
  uint32_t since_poll = 0;
  bool release_pending = false;  // kPad: let go after the poll that saw it
  bool tap = false;              // a wheel detent: let go on the next poll
};
Slot g_slots[kActionCount][kSlots];
uint32_t g_poll = 0;
bool g_enabled[kActionCount] = {true, true, true, true};
bool g_checked = false;

struct Delivery {
  uint8_t action, slot;
  bool down;
};
std::array<Delivery, 64> g_deliveries;
size_t g_delivery_count = 0;

std::atomic<uint32_t> g_inject_buttons{0};
std::atomic<uint32_t> g_inject_triggers{0};
std::atomic<int32_t> g_inject_user{-1};

// The pad the pad poll reads.
int ActivePad(uint8_t* base) {
  const uint32_t input = ReadU32(base, kInput);
  if (!Valid(input) || !ReadU8(base, input + 28)) return 0;
  const uint32_t pad = ReadU32(base, input + 24);
  return pad < 4 ? int(pad) : 0;
}

// The pad poll's own rule for presses (see the top of this file).
bool PollTakesPresses(uint8_t* base, uint32_t cl, const char** why) {
  const uint32_t dvar = ReadU32(base, kInJoystick);
  if (Valid(dvar) && ReadS32(base, dvar + 32) == 0) return *why = "in_joystick is 0", false;
  if (ReadU8(base, kPadPollOff)) return *why = "the pad poll is off", false;
  const uint32_t fe = ReadU32(base, kFeManager + 32);
  if (Valid(fe) && ReadU8(base, fe + 28 + cl) && ReadS32(base, fe + 12) != 3)
    return *why = "a game menu is open", false;
  return true;
}

bool InteractionRunning(uint8_t* base, uint32_t cl) {
  const uint32_t it = ReadU32(base, addr::interact::kPerClient + 4 * cl);
  return Valid(it) && ReadU32(base, it + addr::interact::kState) != 0;
}

// The controller input the game binds to the action (its command first,
// then an equivalent), or nullptr.
const PadInput* PadInputFor(uint8_t* base, uint32_t cl, Action action) {
  const ActionDef& def = Def(action);
  for (const PadInput& p : kPadInputs)
    if (SameText(Binding(base, cl, p.key), def.command)) return &p;
  for (const char* eq : def.equivalents) {
    if (!eq) break;
    for (const PadInput& p : kPadInputs)
      if (SameText(Binding(base, cl, p.key), eq)) return &p;
  }
  return nullptr;
}

const char* HolderName(int slot) { return slot == 0 ? "AUX1" : "AUX2"; }

// Once: each handler is the one the game registered for the command.
void CheckHandlers(uint8_t* base) {
  g_checked = true;
  for (int a = 0; a < kActionCount; ++a) {
    const ActionDef& def = Def(static_cast<Action>(a));
    const std::string up = std::string("-") + (def.command + 1);
    uint32_t first_down = 0;
    bool down_ok = false, up_named = false, up_ok = false;
    int guard = 0;
    for (uint32_t cmd = ReadU32(base, kCmdFunctions); Valid(cmd) && guard < 4096; cmd = ReadU32(base, cmd), ++guard) {
      const uint32_t name = ReadU32(base, cmd + 4);
      if (!Valid(name)) continue;
      const std::string n = ReadString(base, name, 32);
      const uint32_t func = ReadU32(base, cmd + 12);
      if (SameText(n, def.command)) {
        if (!first_down) first_down = func;
        down_ok |= func == kHandlers[a].down;
      }
      if (SameText(n, up)) {
        up_named = true;
        up_ok |= func == kHandlers[a].up;
      }
    }
    // The campaign registers -sprint's handler under the name "+sprint" a
    // second time, so a release handler is checked only where its own name
    // exists.
    const bool ok = down_ok && (!up_named || up_ok);
    g_enabled[a] = ok;
    if (!ok)
      REXLOG_WARN("cod3: key binds: {} isn't at {:#x} in this game (found {:#x}); the {} key does nothing",
                  def.command, kHandlers[a].down, first_down, def.label);
    else if (Debug())
      REXLOG_INFO("cod3: binds: {} is the game's {:#x}", def.command, kHandlers[a].down);
  }
}

void Queue(int a, int s, bool down) {
  if (g_delivery_count < g_deliveries.size()) g_deliveries[g_delivery_count++] = {uint8_t(a), uint8_t(s), down};
}

void Press(uint8_t* base, uint32_t cl, int a, int s, bool tap) {
  Slot& slot = g_slots[a][s];
  if (slot.mode != Mode::kNone) return;
  const Action action = static_cast<Action>(a);
  if (!g_enabled[a]) return;
  if (InteractionRunning(base, cl)) {
    // The interaction decides, as for a controller: press the button the
    // game binds to this action.
    if (const PadInput* p = PadInputFor(base, cl, action)) {
      slot = {Mode::kPad, p->button, p->trigger, g_poll, tap, false};
      if (Debug()) REXLOG_INFO("cod3: binds: {} down -> the controller's {} (an interaction runs)", Def(action).id, p->name);
    } else if (Debug()) {
      REXLOG_INFO("cod3: binds: {} not delivered (an interaction runs, no controller button has it)", Def(action).id);
    }
    return;
  }
  const char* why = "";
  if (!PollTakesPresses(base, cl, &why)) {
    if (Debug()) REXLOG_INFO("cod3: binds: {} not delivered ({})", Def(action).id, why);
    return;
  }
  slot = {Mode::kDirect, 0, 0, g_poll, false, tap};
  Queue(a, s, true);
}

void Release(int a, int s) {
  Slot& slot = g_slots[a][s];
  if (slot.mode == Mode::kDirect) {
    // Pressed in this same poll: let go on the next one, so the game sees
    // the press for at least one frame (a controller press that short would
    // be lost entirely).
    if (slot.since_poll == g_poll) {
      slot.tap = true;
      return;
    }
    Queue(a, s, false);
    slot = {};
  } else if (slot.mode == Mode::kPad) {
    slot.release_pending = true;
  }
}

void BeforePadPoll(uint8_t* base) {
  ++g_poll;
  g_delivery_count = 0;
  if (!g_checked) CheckHandlers(base);
  const uint32_t cl = LocalClient(base);
  // Taps from the last poll, and controller buttons that were seen once.
  for (int a = 0; a < kActionCount; ++a)
    for (int s = 0; s < kSlots; ++s) {
      Slot& slot = g_slots[a][s];
      if (slot.mode == Mode::kDirect && slot.tap && slot.since_poll < g_poll) Release(a, s);
      if (slot.mode == Mode::kPad && slot.release_pending && slot.since_poll < g_poll) slot = {};
    }
  if (uint32_t overflow = g_release_overflow.exchange(0, std::memory_order_acquire))
    for (int i = 0; i < kActionCount * kSlots; ++i)
      if (overflow & (1u << i)) Release(i / kSlots, i % kSlots);
  Event e;
  while (Pop(&e)) {
    if (e.action >= kActionCount || e.slot >= kSlots) continue;
    // The campaign has one player; the keyboard is theirs.
    if (e.kind == Kind::kUp) {
      Release(e.action, e.slot);
    } else if (cl == 0) {
      Press(base, cl, e.action, e.slot, e.kind == Kind::kTap);
    }
  }
  uint32_t buttons = 0, triggers = 0;
  for (auto& action : g_slots)
    for (const Slot& slot : action)
      if (slot.mode == Mode::kPad) buttons |= slot.buttons, triggers |= slot.triggers;
  g_inject_user.store(ActivePad(base), std::memory_order_relaxed);
  g_inject_triggers.store(triggers, std::memory_order_relaxed);
  g_inject_buttons.store(buttons, std::memory_order_release);
}

// --- debug: what the game did with it --------------------------------------------------
struct Watch {
  int zoom = -1, breath = -1, sprint = -1, request = -1;
  int64_t up_since = 0;
  bool binoculars_up = false;
  bool second_shot = false;
  uint32_t prompt_gen = 0;  // the key generation the prompt check last ran for
};
Watch g_watch;

void PromptCheck(PPCContext& ctx, uint8_t* base, uint32_t cl) {
  for (const char* word : {"[HOLD_BINOCS]", "[VERB_BINOCS]"}) {
    Call c(ctx, base);
    const uint32_t w = c.PushString(word);
    const uint32_t text = static_cast<uint32_t>(c.Invoke(kActionWordText, {w}));
    std::string out = Valid(text) ? ReadString(base, text, 160) : "";
    for (char& ch : out)
      if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7E) ch = '?';
    REXLOG_INFO("cod3: binds: prompt check: {} reads \"{}\"", word, out);
  }
  for (const PadInput& p : kPadInputs) {
    const std::string b = Binding(base, cl, p.key);
    const std::string key = KeyNameForPadKey(base, p.key);
    if (!key.empty())
      REXLOG_INFO("cod3: binds: prompt check: the controller's {} is bound to {}; with the keyboard its prompts show [{}]",
                  p.name, b, key);
  }
}

void WatchState(PPCContext& ctx, uint8_t* base, uint32_t cl) {
  const uint32_t ents = ReadU32(base, kEntityList);
  const uint32_t ent = Valid(ents) ? ReadU32(base, ents + 4 * (cl + 1)) : 0;
  const uint32_t ps = Valid(ent) ? ReadU32(base, ent + 528) : 0;
  if (!Valid(ps)) return;
  // Once in a level, and again after the keys change.
  if (g_watch.prompt_gen != Generation()) {
    g_watch.prompt_gen = Generation();
    PromptCheck(ctx, base, cl);
  }
  const float frac = ReadF32(base, ps + 140);
  const int zoom = !std::isfinite(frac) || frac <= 0.0f ? 0 : frac >= 1.0f ? 2 : 1;
  const int request = (ReadU32(base, kBinocularsRequest) & 2) ? 1 : 0;
  const int breath = int(ReadU32(base, ps + 1340) & 1);
  const int sprint = ReadS32(base, kSprintKbutton + 16) ? 1 : 0;
  static const char* kZoomNames[] = {"out", "moving", "in"};
  if (zoom != g_watch.zoom || request != g_watch.request)
    REXLOG_INFO("cod3: binds: state: zoom {} ({:.2f}; the sights or the binoculars), binoculars button {}",
                kZoomNames[zoom], std::isfinite(frac) ? frac : 0.0f, request ? "held" : "not held");
  // The binoculars are up: zoomed in while their button is held (aiming down
  // the sights zooms in too, without it). Pictures of the game's own frame:
  // at once and 25 s later (frames are slow in the cloud; the picture comes
  // after the game's state).
  const bool binoculars_up = zoom == 2 && request;
  if (binoculars_up && !g_watch.binoculars_up) {
    g_watch.up_since = NowMs();
    g_watch.second_shot = false;
    REXLOG_INFO("cod3: binds: state: binoculars up");
    const std::string file = shot::Request(shot::Kind::kAsked, "the binoculars are up (cod3_debug_binds)");
    if (!file.empty()) REXLOG_INFO("cod3: binds: screenshot of the binoculars up: {}", file);
  } else if (!binoculars_up && g_watch.binoculars_up) {
    REXLOG_INFO("cod3: binds: state: binoculars lowered");
  }
  if (binoculars_up && !g_watch.second_shot && NowMs() - g_watch.up_since > 25000) {
    g_watch.second_shot = true;
    const std::string file = shot::Request(shot::Kind::kAsked, "the binoculars are still up (cod3_debug_binds)");
    if (!file.empty()) REXLOG_INFO("cod3: binds: screenshot of the binoculars still up: {}", file);
  }
  if (breath != g_watch.breath) REXLOG_INFO("cod3: binds: state: hold breath {}", breath ? "on" : "off");
  if (sprint != g_watch.sprint) REXLOG_INFO("cod3: binds: state: sprint button {}", sprint ? "held" : "not held");
  g_watch.zoom = zoom;
  g_watch.binoculars_up = binoculars_up;
  g_watch.request = request;
  g_watch.breath = breath;
  g_watch.sprint = sprint;
}

void AfterPadPoll(PPCContext& ctx, uint8_t* base) {
  if (g_delivery_count) {
    Call c(ctx, base);
    const uint32_t now = static_cast<uint32_t>(c.Invoke(kSysMilliseconds));
    for (size_t i = 0; i < g_delivery_count; ++i) {
      const Delivery& d = g_deliveries[i];
      const Handlers& h = kHandlers[d.action];
      Call call(ctx, base);
      call.Invoke(d.down ? h.down : h.up, {uint64_t(kKeyAux1 + d.slot), now});
      if (Debug()) {
        const ActionDef& def = Def(static_cast<Action>(d.action));
        REXLOG_INFO("cod3: binds: {} {} ({}{}, holder {}, direct)", def.id, d.down ? "down" : "up", def.command[0] == '+' && !d.down ? "-" : "",
                    d.down ? def.command : def.command + 1, HolderName(d.slot));
      }
    }
    g_delivery_count = 0;
  }
  if (Debug()) WatchState(ctx, base, LocalClient(base));
}

}  // namespace

// ---------------------------------------------------------------------------
// The public side (binds.h)
// ---------------------------------------------------------------------------

void MigrateSettings(const std::filesystem::path& config_path) {
  EnsureStarted();
  if (REXCVAR_GET(cod3_binds_version) >= kBindsVersion) return;
  std::vector<OldBind> moved, others;
  for (const OldBindName& b : kOldBinds) {
    if (!rex::cvar::GetFlagInfo(b.name)) continue;
    OldBind old{b.name, rex::cvar::GetFlagByName(b.name), rex::cvar::GetFlagSource(b.name) == rex::cvar::Source::kConfig};
    (b.moved ? moved : others).push_back(std::move(old));
  }
  const Migration m = Migrate(CurrentSet(), moved, others);
  for (const auto& [name, value] : m.new_values) {
    if (value.empty())
      rex::cvar::ResetToDefault(name);
    else
      rex::cvar::SetFlagByName(name, value);
  }
  if (m.set_changed) Store(m.set);
  for (const auto& line : m.log) REXLOG_INFO("cod3: key binds: {}", line);
  const KbmSet now = CurrentSet();
  REXLOG_INFO("cod3: key binds call the game's own actions now: binoculars {}, sprint / hold breath {}; keys no "
              "longer press the controller's left stick click or D-pad ({} setting(s) moved)",
              KeysText(now, Action::kBinoculars).empty() ? "-" : KeysText(now, Action::kBinoculars),
              KeysText(now, Action::kSprintBreath).empty() ? "-" : KeysText(now, Action::kSprintBreath),
              m.new_values.size());
  REXCVAR_SET(cod3_binds_version, kBindsVersion);
  if (m.new_values.empty() && !m.set_changed) return;
  try {
    rex::cvar::SaveConfig(config_path);
  } catch (const std::exception& e) {
    REXLOG_WARN("cod3: key binds: couldn't save the settings: {}", e.what());
  }
}

void Attach(rex::ui::Window* window, size_t z_order, std::function<bool()> overlay_open) {
  if (!window) return;
  EnsureStarted();
  g_mnk_mode.store(rex::cvar::GetFlagByName("mnk_mode") == "true", std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("mnk_mode", [](std::string_view, std::string_view v) {
    g_mnk_mode.store(v == "true" || v == "1", std::memory_order_relaxed);
  });
  g_listener.window = window;
  g_listener.overlay_open = std::move(overlay_open);
  window->AddInputListener(&g_listener, z_order);
  window->AddListener(&g_listener);
}

void Detach(rex::ui::Window* window) {
  if (!window || g_listener.window != window) return;
  window->RemoveInputListener(&g_listener);
  window->RemoveListener(&g_listener);
  g_listener.window = nullptr;
}

void OnUiFrame() {
  if (g_listener.window) g_listener.Update();
}

void ApplyToPad(uint8_t* base, uint32_t user, uint32_t state) {
  const uint32_t buttons = g_inject_buttons.load(std::memory_order_acquire);
  const uint32_t triggers = g_inject_triggers.load(std::memory_order_relaxed);
  if ((!buttons && !triggers) || int32_t(user) != g_inject_user.load(std::memory_order_relaxed)) return;
  // X_INPUT_STATE (big-endian): u32 packet, u16 buttons, u8 LT, u8 RT, sticks.
  WriteU16(base, state + 4, uint16_t(guest::ReadU16(base, state + 4) | buttons));
  if (triggers & 1) WriteU8(base, state + 6, 255);
  if (triggers & 2) WriteU8(base, state + 7, 255);
}

std::string KeyNameForPadKey(uint8_t* base, int pad_key) {
  return KeyNameForCommand(Binding(base, LocalClient(base), pad_key));
}

bool IsActionWord(std::string_view word) {
  for (const GameWord& w : kGameWords)
    if (SameText(word, w.word)) return true;
  return false;
}

std::string KeyNameForActionWord(std::string_view word) {
  for (const GameWord& w : kGameWords)
    if (SameText(word, w.word)) return KeyNameForCommand(w.command);
  return "";
}

std::string KeyNameForCommand(std::string_view binding) {
  if (binding.empty()) return "";
  const KbmSet set = CurrentSet();
  // The action whose command it is, then one it counts for.
  for (int pass = 0; pass < 2; ++pass)
    for (int a = 0; a < kActionCount; ++a) {
      const ActionDef& def = Def(static_cast<Action>(a));
      bool match = pass == 0 && SameText(binding, def.command);
      for (const char* eq : def.equivalents) {
        if (!eq || pass == 0) break;
        match |= SameText(binding, eq);
      }
      if (match && set.key[a][0] != kNoKey) return PromptName(set.key[a][0]);
    }
  return "";
}

uint32_t Generation() { return g_generation.load(std::memory_order_acquire); }

bool HandleConsoleCommand(const std::vector<std::string>& args, cheats::Source source,
                          const std::function<void(bool error, const std::string& text)>& print,
                          bool* changed) {
  *changed = false;
  if (args.size() < 2) return false;
  const bool bind = SameText(args[0], "bind");
  if (!bind && !SameText(args[0], "unbind")) return false;
  // The game's names for controller buttons stay the game's (its bind table
  // is the controller's): L1 ... R3, SQUARE, CIRCLE, TRIANGLE, JOY*, AUX*,
  // the D-pad's UPARROW ..., and TAB / ESCAPE / ENTER / PAUSE (the pad's
  // BACK and START). Keyboard arrows are Up / Down / Left / Right here.
  const std::string& name = args[1];
  for (const char* pad : {"L1", "R1", "L2", "R2", "L3", "R3", "SQUARE", "CIRCLE", "TRIANGLE", "UPARROW",
                          "DOWNARROW", "LEFTARROW", "RIGHTARROW", "TAB", "ESCAPE", "ENTER", "PAUSE", "START",
                          "SELECT"})
    if (SameText(name, pad)) return false;
  if ((name.size() > 3 && SameText(name.substr(0, 3), "JOY")) || (name.size() > 3 && SameText(name.substr(0, 3), "AUX")))
    return false;
  const KeyCode key = ParseKey(name);
  if (key == kNoKey) return false;
  KbmSet set = CurrentSet();
  int had_slot = 0;
  const Action had = Find(set, key, &had_slot);
  const std::string key_name = KeyName(key);

  if (bind && args.size() == 2) {  // "bind B": what it does
    if (had == Action::kCount) return false;
    print(false, "\"" + key_name + "\" = \"" + Def(had).command + "\" (keyboard and mouse: " + Def(had).label + ")\n");
    return true;
  }
  if (source == cheats::Source::kNetwork) return false;
  std::string command;
  for (size_t i = 2; i < args.size(); ++i) command += (i > 2 ? " " : "") + args[i];
  const Action action = bind ? ActionByCommand(command) : Action::kCount;

  if (action != Action::kCount) {
    if (const std::string why = ReservedReason(key); !why.empty()) {
      print(true, "^1bind: " + key_name + " can't be bound (" + why + ")\n");
      return true;
    }
    if (had == action) {
      print(false, key_name + " is already " + Def(action).label + "\n");
      return true;
    }
    // A free slot, else the second key is replaced.
    const int slot = set.key[int(action)][0] == kNoKey ? 0 : 1;
    const std::string note = Assign(set, action, slot, key);
    Store(set);
    *changed = true;
    std::string text = key_name + " = " + Def(action).label + " (keyboard and mouse; saved)";
    if (!note.empty()) text += "; " + note;
    if (const OldBindName* row = ControllerRowWith(key))
      text += "; it no longer presses the controller's " + std::string(row->label);
    print(false, text + "\n");
    REXLOG_INFO("cod3: key binds: {} from the console", text);
    return true;
  }
  if (had == Action::kCount) return false;
  // unbind B, or B bound to a command the keyboard can't call yet: B leaves
  // its action (the game's own bind of the key does nothing: its keyboard
  // keys never reach the game).
  Assign(set, had, had_slot, kNoKey);
  Store(set);
  *changed = true;
  std::string text = key_name + " is no longer " + std::string(Def(had).label) + " (saved)";
  if (bind)
    text += "; keyboard keys can call only Binoculars, Sprint / hold breath, Hold breath and Sprint so far "
            "(the rest: F2 > Controls > Key bindings)";
  print(false, text + "\n");
  REXLOG_INFO("cod3: key binds: {} from the console", text);
  return !bind;
}

bool DrawF2Rows() {
  EnsureStarted();
  bool changed = false;
  const KbmSet set = CurrentSet();
  static char text[kActionCount][64];
  static std::string notes[kActionCount];
  static uint32_t text_gen = 0;
  if (text_gen != Generation()) {
    text_gen = Generation();
    for (int a = 0; a < kActionCount; ++a)
      std::snprintf(text[a], sizeof(text[a]), "%s", KeysText(set, static_cast<Action>(a)).c_str());
  }
  auto help = [](const char* line) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(line);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
  };
  ImGui::TextUnformatted("Actions");
  help("These keys call the game's own actions directly (not a controller button), so a controller keeps "
       "working at the same time. Up to two keys each, separated by a comma (B, MOUSE4); empty = none; "
       "press Enter to apply. Key names: A-Z, 0-9, SHIFT, CTRL, ALT, SPACE, TAB, ENTER, UPARROW ..., "
       "MOUSE1-MOUSE5, MWHEELUP, MWHEELDOWN. No key combinations (Shift+key). The console's "
       "bind B +binoculars works too.");
  if (ImGui::BeginTable("##bind_actions", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    for (int a = 0; a < kActionCount; ++a) {
      const Action action = static_cast<Action>(a);
      const ActionDef& def = Def(action);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(def.label);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", def.help);
      ImGui::TableNextColumn();
      ImGui::PushID(def.id);
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputText("##keys", text[a], sizeof(text[a]), ImGuiInputTextFlags_EnterReturnsTrue)) {
        KbmSet next = set;
        std::string note;
        bool ok = AssignText(next, action, text[a], &note);
        for (int s = 0; ok && s < kSlots; ++s)
          if (const std::string why = ReservedReason(next.key[a][s]); next.key[a][s] && !why.empty()) {
            note = KeyName(next.key[a][s]) + " can't be bound (" + why + ").";
            ok = false;
          }
        if (ok) {
          Store(next);
          changed = true;
          REXLOG_INFO("cod3: key binds: {} = {} (F2){}", def.id, KeysText(next, action),
                      note.empty() ? "" : " " + note);
        }
        notes[a] = note;
        text_gen = 0;  // show what was stored
      }
      ImGui::PopID();
      for (KeyCode k : set.key[a])
        if (const OldBindName* row = k ? ControllerRowWith(k) : nullptr) {
          const std::string line = KeyName(k) + " is in the controller row " + row->label + " too: " + def.label +
                                   " gets it, that row doesn't.";
          ImGui::PushTextWrapPos(0.0f);
          ImGui::TextColored(ui::CurrentPalette().warn, "%s", line.c_str());
          ImGui::PopTextWrapPos();
        }
      if (!notes[a].empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ui::CurrentPalette().warn, "%s", notes[a].c_str());
        ImGui::PopTextWrapPos();
      }
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
  ImGui::TextUnformatted("Controller buttons");
  return changed;
}

}  // namespace cod3::binds

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// The pad poll (once before every frame, the game's main thread): the keys'
// presses go in first (controller buttons inside an interaction), the game
// reads its pads, then the direct calls.
REX_EXTERN(__imp__sub_82522138);
REX_HOOK_RAW(sub_82522138) {
  cod3::binds::BeforePadPoll(base);
  __imp__sub_82522138(ctx, base);
  cod3::binds::AfterPadPoll(ctx, base);
}
