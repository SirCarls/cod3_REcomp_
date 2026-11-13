// waw - keys the PC way (see waw_keys.h). [f2-menus]

#include "waw_keys.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

#include "generated/default/waw_pch.h"
#include "waw_f2.h"
#include "waw_mouse.h"

REXCVAR_DEFINE_BOOL(waw_pc_keys, true, "waw",
                    "Keys, mouse buttons and the wheel the PC way during gameplay: a key the game has a bind for "
                    "runs that bind (the console's bind command, waw_pc_binds); any other key stays with the "
                    "keyboard pad (keybind_*). The menus always use the keyboard pad");
REXCVAR_DEFINE_STRING(waw_pc_binds,
                      "w=+forward|s=+back|a=+moveleft|d=+moveright|SPACE=+gostand|c=+stance|CTRL=+prone|"
                      "SHIFT=+breath_sprint|MOUSE1=+attack|MOUSE2=+speed_throw|r=+reload|f=+activate|v=+melee|"
                      "MOUSE3=+melee|g=+frag|4=+smoke|1=weapnext|2=weapprev|MWHEELDOWN=weapnext|MWHEELUP=weapprev|"
                      "n=+actionslot 1|6=+actionslot 2|3=+actionslot 3|5=+actionslot 4|q=+leanleft|e=+leanright|"
                      "TAB=+scores",
                      "waw",
                      "The PC's binds, \"KEY=command|KEY=command\", given to each key the game has no bind for when "
                      "gameplay starts (a key you bound yourself keeps your bind). Empty: none");
REXCVAR_DEFINE_STRING(waw_debug_keys, "", "waw",
                      "Debug: PC keys pressed at set times, \"SECONDS:KEY|SECONDS:KEY\" counted from the first frame "
                      "(for example \"20:k|25:SPACE\"); each is let go a tenth of a second later. With a bind from "
                      "waw_debug_command_script (\"15:bind k toggle cg_drawFPS\") it shows the key reached the "
                      "game's binds");
REXCVAR_DEFINE_BOOL(waw_log_keys, false, "waw",
                    "Debug: log every key event handed to the game's CL_KeyEvent (name, number, down or up)");

REXCVAR_DECLARE(bool, mnk_mode);

DECLARE_REX_FUNC(sub_82141C50);  // CL_KeyEvent(local client r3, controller r4, key r5, down r6, time r7)
DECLARE_REX_FUNC(sub_822A3048);  // Sys_Milliseconds() -> r3
DECLARE_REX_FUNC(sub_82238978);  // Cbuf_AddText(local client r3, text r4)

namespace waw::keys {
namespace {

using Clock = std::chrono::steady_clock;
using rex::ui::VirtualKey;

// The game's key names: (name pointer, number) pairs, 8 bytes each, a null name at the end (Key_StringToKeynum
// 0x82142AB0 walks it with I_stricmp).
constexpr uint32_t kKeyNames = 0x82493060u;
constexpr int kMaxNames = 512;
// The key states of local client 0 (CL_KeyEvent): 16 bytes a key from 0x82517954, down +0, repeats +4, the key's two
// binds' text at +8 and +0xC (Key_Bind_f reads 0x8251795C + key * 16).
constexpr uint32_t kKeyBinding = 0x8251795Cu;
constexpr uint32_t kKeyBinding2 = 0x82517960u;
constexpr uint32_t kKeyStride = 0x10;
constexpr int kKeyCount = 256;
// Local client 0's key catchers (CL_KeyEvent tests them; 36 bytes a client): 0x10 a menu takes the keys
// (UI_KeyEvent), as the PC's KEYCATCH_UI.
constexpr uint32_t kKeyCatchers = 0x842F35E8u;
constexpr uint32_t kKeyCatchUi = 0x10u;

std::atomic<uint8_t*> g_base{nullptr};  // guest memory, known from the first frame on

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

std::string Upper(std::string text) {
  for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

std::once_flag g_table_once;
std::map<std::string, int> g_names;  // upper-case name -> the game's number

int FindName(const std::string& upper) {
  const auto it = g_names.find(upper);
  return it == g_names.end() ? -1 : it->second;
}

void ReadTable(const uint8_t* base) {
  uint32_t entry = kKeyNames;
  for (int i = 0; i < kMaxNames; ++i, entry += 8) {
    const uint32_t name = LoadBe32(base, entry);
    if (name < 0x82000000u || name >= 0x84000000u) break;  // the null at the end (or not the table at all)
    const char* text = reinterpret_cast<const char*>(base + name);
    g_names[Upper(std::string(text, strnlen(text, 32)))] = static_cast<int>(LoadBe32(base, entry + 4));
  }
  REXLOG_INFO("waw: keys: the game's key names table has {} names (SPACE {}, ENTER {}, ESCAPE {}, MOUSE1 {}, "
              "MWHEELUP {}, BUTTON_A {}, APAD_UP {})",
              g_names.size(), FindName("SPACE"), FindName("ENTER"), FindName("ESCAPE"), FindName("MOUSE1"),
              FindName("MWHEELUP"), FindName("BUTTON_A"), FindName("APAD_UP"));
}

// The game has a bind for this key (local client 0).
bool HasBind(const uint8_t* base, int key) {
  if (key < 0 || key >= kKeyCount) return false;
  const uint32_t offset = uint32_t(key) * kKeyStride;
  return LoadBe32(base, kKeyBinding + offset) != 0 || LoadBe32(base, kKeyBinding2 + offset) != 0;
}

bool UiTakesKeys(const uint8_t* base) { return base && (LoadBe32(base, kKeyCatchers) & kKeyCatchUi) != 0; }

struct Event {
  std::string name;
  bool down;
};

std::mutex g_queue_mutex;
std::vector<Event> g_queue;
std::atomic<bool> g_has_events{false};

// Calls Sys_Milliseconds and then CL_KeyEvent(0, 0, key, down, time) from inside a translated function: the calls
// make their frames below the current one, and every register is put back afterwards (as QueueConsoleCommand in
// waw_hooks.cpp).
void SendKey(PPCContext& ctx, uint8_t* base, int key, bool down, const std::string& name) {
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - 0x400u) & ~0xFu;
  sub_822A3048(ctx, base);
  const uint32_t time = ctx.r3.u32;
  ctx.r3.u64 = 0;  // local client
  ctx.r4.u64 = 0;  // controller
  ctx.r5.u64 = static_cast<uint32_t>(key);
  ctx.r6.u64 = down ? 1 : 0;
  ctx.r7.u64 = time;
  sub_82141C50(ctx, base);
  ctx = saved;
  if (REXCVAR_GET(waw_log_keys)) {
    REXLOG_INFO("waw: key {} ({}) {} at {} ms", name, key, down ? "down" : "up", time);
  }
}

// Cbuf_AddText(0, text) the same way (the text on the guest stack below the current frame).
void AddText(PPCContext& ctx, uint8_t* base, const std::string& text) {
  constexpr uint32_t kRoom = 0x1000;
  constexpr uint32_t kTextOffset = 0x200;
  const std::string line = text.substr(0, kRoom - kTextOffset - 2);
  const PPCContext saved = ctx;
  const uint32_t frame = (ctx.r1.u32 - kRoom) & ~0xFu;
  std::memcpy(base + frame + kTextOffset, line.c_str(), line.size() + 1);
  ctx.r1.u64 = frame;
  ctx.r3.u64 = 0;
  ctx.r4.u64 = frame + kTextOffset;
  sub_82238978(ctx, base);
  ctx = saved;
}

// waw_pc_binds to every key that has no bind: one "bind" line each, run by the game's own command buffer.
void GiveDefaultBinds(PPCContext& ctx, uint8_t* base) {
  const std::string text = REXCVAR_GET(waw_pc_binds);
  std::string lines;
  int given = 0, kept = 0, unknown = 0;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = std::min(text.find('|', start), text.size());
    const std::string item = text.substr(start, end - start);
    start = end + 1;
    const size_t eq = item.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= item.size()) continue;
    const std::string name = item.substr(0, eq);
    const std::string command = item.substr(eq + 1);
    if (command.find_first_of("\";\n") != std::string::npos) continue;  // one plain command per key
    const int key = KeyNumber(base, name);
    if (key < 0 || key >= kKeyCount) {
      ++unknown;
      continue;
    }
    if (HasBind(base, key)) {
      ++kept;
      continue;
    }
    lines += "bind " + name + " \"" + command + "\"\n";
    ++given;
  }
  if (!lines.empty()) AddText(ctx, base, lines);
  REXLOG_INFO("waw: keys: PC binds given to {} unbound key(s), {} key(s) kept their own bind, {} unknown name(s)",
              given, kept, unknown);
}

struct Timed {
  double seconds;
  std::string name;
};

std::vector<Timed> ParseScript(const std::string& text) {
  std::vector<Timed> script;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = std::min(text.find('|', start), text.size());
    const std::string item = text.substr(start, end - start);
    const size_t colon = item.find(':');
    if (colon != std::string::npos && colon + 1 < item.size()) {
      script.push_back({std::atof(item.substr(0, colon).c_str()), item.substr(colon + 1)});
    }
    start = end + 1;
  }
  std::stable_sort(script.begin(), script.end(),
                   [](const Timed& a, const Timed& b) { return a.seconds < b.seconds; });
  return script;
}

// ---- The window's keys (any thread) ----

// The PC's name of a window key, "" for the keys that stay with the port and the keyboard pad: ESCAPE (the pad's
// START: the pause menu), F1-F12 (F2 is the settings window), and anything without a PC name.
std::string PcName(VirtualKey vk) {
  const auto v = static_cast<uint16_t>(vk);
  if (v >= uint16_t(VirtualKey::kA) && v <= uint16_t(VirtualKey::kZ))
    return std::string(1, char('a' + (v - uint16_t(VirtualKey::kA))));
  if (v >= uint16_t(VirtualKey::k0) && v <= uint16_t(VirtualKey::k9))
    return std::string(1, char('0' + (v - uint16_t(VirtualKey::k0))));
  switch (vk) {
    case VirtualKey::kSpace: return "SPACE";
    case VirtualKey::kReturn: return "ENTER";
    case VirtualKey::kTab: return "TAB";
    case VirtualKey::kBack: return "BACKSPACE";
    case VirtualKey::kShift:
    case VirtualKey::kLShift:
    case VirtualKey::kRShift: return "SHIFT";
    case VirtualKey::kControl:
    case VirtualKey::kLControl:
    case VirtualKey::kRControl: return "CTRL";
    case VirtualKey::kMenu:
    case VirtualKey::kLMenu:
    case VirtualKey::kRMenu: return "ALT";
    case VirtualKey::kCapital: return "CAPSLOCK";
    case VirtualKey::kUp: return "UPARROW";
    case VirtualKey::kDown: return "DOWNARROW";
    case VirtualKey::kLeft: return "LEFTARROW";
    case VirtualKey::kRight: return "RIGHTARROW";
    case VirtualKey::kInsert: return "INS";
    case VirtualKey::kDelete: return "DEL";
    case VirtualKey::kPrior: return "PGUP";
    case VirtualKey::kNext: return "PGDN";
    case VirtualKey::kHome: return "HOME";
    case VirtualKey::kEnd: return "END";
    case VirtualKey::kPause: return "PAUSE";
    case VirtualKey::kNumpad0: return "KP_INS";
    case VirtualKey::kNumpad1: return "KP_END";
    case VirtualKey::kNumpad2: return "KP_DOWNARROW";
    case VirtualKey::kNumpad3: return "KP_PGDN";
    case VirtualKey::kNumpad4: return "KP_LEFTARROW";
    case VirtualKey::kNumpad5: return "KP_5";
    case VirtualKey::kNumpad6: return "KP_RIGHTARROW";
    case VirtualKey::kNumpad7: return "KP_HOME";
    case VirtualKey::kNumpad8: return "KP_UPARROW";
    case VirtualKey::kNumpad9: return "KP_PGUP";
    case VirtualKey::kDecimal: return "KP_DEL";
    case VirtualKey::kDivide: return "KP_SLASH";
    case VirtualKey::kMultiply: return "KP_STAR";
    case VirtualKey::kSubtract: return "KP_MINUS";
    case VirtualKey::kAdd: return "KP_PLUS";
    case VirtualKey::kNumLock: return "KP_NUMLOCK";
    case VirtualKey::kOem1: return "SEMICOLON";  // ';' separates commands, so the PC names it
    case VirtualKey::kOemPlus: return "=";
    case VirtualKey::kOemComma: return ",";
    case VirtualKey::kOemMinus: return "-";
    case VirtualKey::kOemPeriod: return ".";
    case VirtualKey::kOem2: return "/";
    case VirtualKey::kOem3: return "`";
    case VirtualKey::kOem4: return "[";
    case VirtualKey::kOem5: return "\\";
    case VirtualKey::kOem6: return "]";
    case VirtualKey::kOem7: return "'";
    default: return "";
  }
}

std::string MouseName(rex::ui::MouseEvent::Button button) {
  switch (button) {
    case rex::ui::MouseEvent::Button::kLeft: return "MOUSE1";
    case rex::ui::MouseEvent::Button::kRight: return "MOUSE2";
    case rex::ui::MouseEvent::Button::kMiddle: return "MOUSE3";
    case rex::ui::MouseEvent::Button::kX1: return "MOUSE4";
    case rex::ui::MouseEvent::Button::kX2: return "MOUSE5";
    default: return "";
  }
}

std::mutex g_held_mutex;
std::set<std::string> g_held;  // keys sent down to the game and not yet let go
std::atomic<bool> g_any_held{false};
std::atomic<rex::ui::Window*> g_window{nullptr};  // the listener's window (its focus, read once a frame)

bool Active(const rex::ui::Window* window) {
  const uint8_t* base = g_base.load(std::memory_order_acquire);
  return REXCVAR_GET(waw_pc_keys) && REXCVAR_GET(mnk_mode) && base && !UiTakesKeys(base) && window &&
         window->HasFocus() && waw::mouse::InGameplay() && waw::f2ui::GameGetsInput();
}

// A press for the game if it has a bind for the key (true: the event is the game's, the keyboard pad doesn't see it).
bool Press(const std::string& name) {
  if (name.empty()) return false;
  std::lock_guard lock(g_held_mutex);
  if (g_held.count(name)) return true;  // the window's key repeat
  const uint8_t* base = g_base.load(std::memory_order_acquire);
  if (!base || !HasBind(base, KeyNumber(base, name))) return false;
  g_held.insert(name);
  g_any_held.store(true, std::memory_order_release);
  Queue(name, true);
  return true;
}

// The release of a key the game got the press of (whether or not it's still gameplay).
bool Release(const std::string& name) {
  if (name.empty()) return false;
  std::lock_guard lock(g_held_mutex);
  if (!g_held.erase(name)) return false;
  if (g_held.empty()) g_any_held.store(false, std::memory_order_release);
  Queue(name, false);
  return true;
}

void ReleaseAll() {
  std::lock_guard lock(g_held_mutex);
  for (const std::string& name : g_held) Queue(name, false);
  g_held.clear();
  g_any_held.store(false, std::memory_order_release);
}

}  // namespace

bool MenuTakesKeys() { return UiTakesKeys(g_base.load(std::memory_order_acquire)); }

int KeyNumber(const uint8_t* base, const std::string& name) {
  if (name.empty()) return -1;
  if (name.size() == 1) return std::tolower(static_cast<unsigned char>(name[0]));
  if (name.size() == 4 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
    return static_cast<int>(std::strtol(name.c_str() + 2, nullptr, 16));
  }
  std::call_once(g_table_once, ReadTable, base);
  return FindName(Upper(name));
}

void Queue(const std::string& name, bool down) {
  std::lock_guard lock(g_queue_mutex);
  g_queue.push_back({name, down});
  g_has_events.store(true, std::memory_order_release);
}

void RunFrame(PPCContext& ctx, uint8_t* base) {
  g_base.store(base, std::memory_order_release);

  // Gameplay starts: the PC's binds for the keys without one. It ends (or a menu opens): every key the game holds is
  // let go.
  static bool was_playing = false;
  const bool playing = waw::mouse::InGameplay() && !UiTakesKeys(base);
  if (playing != was_playing) {
    was_playing = playing;
    if (playing && REXCVAR_GET(waw_pc_keys)) GiveDefaultBinds(ctx, base);
    if (!playing) ReleaseAll();
  }
  // The window lost the focus with keys down (their releases go to another window): let them go.
  if (g_any_held.load(std::memory_order_acquire)) {
    rex::ui::Window* window = g_window.load(std::memory_order_acquire);
    if (window && !window->HasFocus()) ReleaseAll();
  }

  // waw_debug_keys: presses at set times from the first frame, each let go 100 ms later.
  static const Clock::time_point first_frame = Clock::now();
  static const std::vector<Timed> script = [] {
    std::vector<Timed> parsed = ParseScript(REXCVAR_GET(waw_debug_keys));
    if (!parsed.empty()) REXLOG_INFO("waw: keys: {} scripted key presses (waw_debug_keys)", parsed.size());
    return parsed;
  }();
  static size_t next = 0;
  static std::vector<std::pair<Clock::time_point, std::string>> releases;
  if (next < script.size() || !releases.empty()) {
    const Clock::time_point now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - first_frame).count();
    for (; next < script.size() && script[next].seconds <= seconds; ++next) {
      Queue(script[next].name, true);
      releases.emplace_back(now + std::chrono::milliseconds(100), script[next].name);
    }
    for (auto it = releases.begin(); it != releases.end();) {
      if (now >= it->first) {
        Queue(it->second, false);
        it = releases.erase(it);
      } else {
        ++it;
      }
    }
  }

  if (!g_has_events.load(std::memory_order_acquire)) return;
  std::vector<Event> events;
  {
    std::lock_guard lock(g_queue_mutex);
    events.swap(g_queue);
    g_has_events.store(false, std::memory_order_relaxed);
  }
  static std::set<std::string> unknown;
  for (const Event& event : events) {
    const int key = KeyNumber(base, event.name);
    if (key < 0 || key >= kKeyCount) {
      if (unknown.insert(event.name).second) {
        REXLOG_WARN("waw: keys: the game has no key named \"{}\"", event.name);
      }
      continue;
    }
    SendKey(ctx, base, key, event.down, event.name);
    if (REXCVAR_GET(waw_log_keys)) {
      REXLOG_INFO("waw: keys: key catchers {:#x}, {} bind(s) on {}", LoadBe32(base, kKeyCatchers),
                  HasBind(base, key) ? "with" : "no", event.name);
    }
  }
}

void Listener::OnKeyDown(rex::ui::KeyEvent& e) {
  g_window.store(window, std::memory_order_release);
  const std::string name = PcName(e.virtual_key());
  if (name.empty() || (e.is_alt_pressed() && name != "ALT")) return;  // Alt+key stays the window's
  if (Active(window) && Press(name)) e.set_handled(true);
}

void Listener::OnKeyUp(rex::ui::KeyEvent& e) {
  if (Release(PcName(e.virtual_key()))) e.set_handled(true);
}

void Listener::OnMouseDown(rex::ui::MouseEvent& e) {
  g_window.store(window, std::memory_order_release);
  if (Active(window) && Press(MouseName(e.button()))) e.set_handled(true);
}

void Listener::OnMouseUp(rex::ui::MouseEvent& e) {
  if (Release(MouseName(e.button()))) e.set_handled(true);
}

void Listener::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!Active(window) || e.scroll_y() == 0) return;
  const std::string name = e.scroll_y() > 0 ? "MWHEELUP" : "MWHEELDOWN";
  const uint8_t* base = g_base.load(std::memory_order_acquire);
  if (!base || !HasBind(base, KeyNumber(base, name))) return;
  Queue(name, true);  // a wheel step is a press and a release, as on the PC
  Queue(name, false);
  e.set_handled(true);
}

}  // namespace waw::keys
