// cod2 / cod2mp - the PC copy's keys and mouse (see cod2_pc_input.h). [f2-menus]

#include "cod2_pc_input.h"

#include "cod2_f2.h"  // the keys and the pointer are let go while the F2 window is open
// [f2-menus] 2026-11-11 04:00 (waw-decomp, board 03:46): only with the F2 folder (COD2_HAVE_F2, set by the CMake
// lists with the F2 sources), so a build without it compiles as before the mouse acceleration.
#if defined(COD2_HAVE_F2)
#include "f2/f2_accel.h"  // f2::accel: mouse acceleration and the sensitivity randomizer inside the game
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(cod2_mouse_look, true, "cod2/Controls",
                    "Aim with the mouse through the game's own PC mouse look (its sensitivity, m_yaw, m_pitch and "
                    "m_filter), the pointer held while a level plays; off: the SDK's mouse-to-right-stick");
REXCVAR_DEFINE_DOUBLE(cod2_mouse_sensitivity, 5.0, "cod2/Controls",
                      "Mouse sensitivity, the game's own (the PC copy's default is 5, its slider 1 to 30)")
    .range(0.01, 100.0);  // [f2-menus] 03:20 wider than its slider: a sensitivity typed on another game's scale
REXCVAR_DEFINE_BOOL(cod2_mouse_match_pc, true, "cod2/Controls",
                    "The mouse turns the view as far as in the PC copy at the same sensitivity (0.022 degrees a "
                    "count times the sensitivity, from the hip); off: the Xbox 360 program's own turn, about a "
                    "quarter more on its wide screen");
REXCVAR_DEFINE_BOOL(cod2_mouse_invert, false, "cod2/Controls",
                    "Invert the mouse: pushing it away looks down (the game's m_pitch, negative)");
REXCVAR_DEFINE_BOOL(cod2_mouse_smooth, false, "cod2/Controls",
                    "Smooth the mouse: each turn is the average of the last two reads (the game's m_filter)");
REXCVAR_DEFINE_BOOL(cod2_pc_keys, true, "cod2/Controls",
                    "The PC copy's keys while a level plays: each key in cod2_pc_binds runs its console command, as "
                    "the PC game's binds do; off: every key presses a controller button (the SDK's keyboard pad)");
REXCVAR_DEFINE_STRING(cod2_pc_binds,
                      "W=+forward;S=+back;A=+moveleft;D=+moveright;Q=+leanleft;E=+leanright;Space=+gostand;"
                      "C=gocrouch;Ctrl=goprone;Mouse1=+attack;Mouse2=toggleads;1=weapnext;2=weapnext;R=+reload;"
                      "F=+activate;G=+frag;Mouse3=+frag;4=+smoke;B=+binoculars;Tab=+scores;V=+melee;"
                      "Shift=+holdbreath",
                      "cod2/Controls",
                      "The keys of cod2_pc_keys, \"KEY=command;KEY=command\": a letter or digit, Shift, Ctrl, Alt, "
                      "Space, Tab, Enter, CapsLock, Backspace, Up, Down, Left, Right, Ins, Del, Home, End, PgUp, "
                      "PgDn, F1 to F12 (not F2), Mouse1 to Mouse5, MWheelUp, MWheelDown; the command is one of the "
                      "game's own (+forward, gocrouch, toggleads, ...)");
REXCVAR_DEFINE_STRING(cod2_debug_mouse, "", "cod2/Debug",
                      "Debug: mouse counts \"dx,dy\" added at every usercmd, as if the mouse moved (a turn test "
                      "without a mouse); once a second the log shows how far the game turned the view for them");
// [f2-menus] 2026-11-11 03:30 (owner 03:24: "just so we are clear FPS cannot affect mouse input."): a test mouse
// that moves at its own rate, whatever the frame rate.
REXCVAR_DEFINE_STRING(cod2_debug_mouse_events, "", "cod2/Debug",
                      "Debug: \"dx,dy,hz\": mouse events of dx,dy counts sent hz times a second from a thread of "
                      "their own, as a mouse sends them whatever the frame rate (a test that the turn a second is "
                      "the same at any frame rate); the log shows the turn as for cod2_debug_mouse");
REXCVAR_DEFINE_STRING(cod2_debug_keys, "", "cod2/Debug",
                      "Debug: keys pressed and let go without a keyboard, \"<seconds>:<KEY>+|<seconds>:<KEY>-|...\", "
                      "the seconds counted from the first moment a level plays; once a second the log shows the "
                      "usercmd the game made of them");

REXCVAR_DECLARE(bool, mnk_mode);

namespace cod2::pc_input {

namespace {

using Clock = std::chrono::steady_clock;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// The game made a usercmd this recently: a level is running. A load or the main menu stops them for longer.
constexpr int64_t kPlayingWindowNs = 1'500'000'000;

std::atomic<int64_t> g_last_cmd_ns{0};
std::atomic<bool> g_playing{false};  // a level plays, no menu or console is open, the F2 window is closed
std::atomic<uint32_t> g_catchers{0};
std::atomic<uint32_t> g_commands_sent{0};

// ------------------------------------------------------------------------------------------------- guest memory
uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void StoreBe32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

float LoadBeF32(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadBe32(base, address);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

// Where the game keeps what this file reads and writes (cod2_pc_input.h).
#if defined(COD2_MP)
constexpr uint32_t kClientPointer = 0x824D37E0u;  // the pointer to the client's state
constexpr uint32_t kFrameMsec = 0x82F55CB0u;
constexpr uint32_t kDvarSensitivity = 0x82F4AB1Cu;  // each a pointer to the dvar; its value is at +8
constexpr uint32_t kDvarMPitch = 0x82F4AB34u;
constexpr uint32_t kDvarMFilter = 0x82F55CFCu;
constexpr uint32_t kDvarFreelook = 0x8353A65Cu;
constexpr uint32_t kDvarCgFov = 0x82D6CA38u;
#else
constexpr uint32_t kFrameMsec = 0x8289A9D8u;
constexpr uint32_t kDvarSensitivity = 0x8289AD50u;
constexpr uint32_t kDvarMPitch = 0x8289AD60u;
constexpr uint32_t kDvarMFilter = 0x828E082Cu;
constexpr uint32_t kDvarFreelook = 0x828E0838u;
constexpr uint32_t kDvarCgFov = 0x827F6DD0u;
#endif

struct Addresses {
  bool ok = false;
  uint32_t dx = 0, dy = 0, index = 0;  // mouseDx[2], mouseDy[2], mouseIndex
  uint32_t pitch = 0, yaw = 0;         // the view angles CL_MouseMove turns
  uint32_t scale = 0;                  // what the game multiplies the sensitivity by (the field of view's share)
  uint32_t catchers = 0;               // the key catchers: 1 the console, 8 a menu (0x10 a chat line in multiplayer)
};

Addresses Resolve(const uint8_t* base) {
  Addresses a;
#if defined(COD2_MP)
  const uint32_t cl = LoadBe32(base, kClientPointer);
  if (cl == 0) return a;
  a = {true, cl + 0x85D8u, cl + 0x85E0u, cl + 0x85E8u, cl + 0x8620u, cl + 0x8624u, cl + 0x8608u, cl + 4u};
#else
  (void)base;
  a = {true, 0x828E9EB8u, 0x828E9EC0u, 0x828E9EC8u, 0x828E9F08u, 0x828E9F0Cu, 0x828E9EE8u, 0x828E0884u};
#endif
  return a;
}

bool KeyboardPlays() { return REXCVAR_GET(mnk_mode); }
bool MouseLookOn() { return REXCVAR_GET(cod2_mouse_look) && KeyboardPlays(); }
bool KeysOn() {
  return REXCVAR_GET(cod2_pc_keys) && KeyboardPlays() && g_playing.load(std::memory_order_relaxed);
}

// ------------------------------------------------------------------------------------------------- mouse motion
std::mutex g_motion_mutex;
float g_dx = 0.0f;  // motion not yet handed to the game; the fractions of a count stay for the next usercmd
float g_dy = 0.0f;

void AddMotion(float dx, float dy) {
  // [f2-menus] 2026-11-11 03:10 mouse acceleration and the sensitivity randomizer inside the game (f2_accel,
  // f2_sens_random; both off by default): each mouse event's counts with the time since the event before it, as a
  // driver would see them, so a flick comes out the same at any frame rate. Mouse events come on one thread.
  {
    thread_local int64_t last_ns = 0;  // 03:30 a thread's own: the test mouse (cod2_debug_mouse_events) has its own
    const int64_t now = NowNs();
    const double ms = last_ns ? double(now - last_ns) / 1e6 : 0.0;
    last_ns = now;
#if defined(COD2_HAVE_F2)
    f2::accel::Apply(&dx, &dy, ms);
#else
    (void)ms;
#endif
  }
  std::lock_guard lock(g_motion_mutex);
  g_dx += dx;
  g_dy += dy;
}

void ClearMotion() {
  std::lock_guard lock(g_motion_mutex);
  g_dx = g_dy = 0.0f;
}

// [f2-menus] 2026-11-11 03:30 cod2_debug_mouse_events: started once, at the first usercmd that finds the setting.
std::atomic<bool> g_test_mouse_on{false};
void StartTestMouseOnce() {
  static std::atomic<bool> tried{false};
  if (tried.exchange(true)) return;
  const std::string text = REXCVAR_GET(cod2_debug_mouse_events);
  int dx = 0, dy = 0, hz = 0;
  if (text.empty() || std::sscanf(text.c_str(), "%d,%d,%d", &dx, &dy, &hz) != 3 || hz < 1 || hz > 8000) return;
  g_test_mouse_on.store(true, std::memory_order_relaxed);
  REXLOG_INFO("cod2: test mouse: {},{} counts {} times a second from its own thread", dx, dy, hz);
  std::thread([dx, dy, hz] {
    const auto period = std::chrono::nanoseconds(1'000'000'000LL / hz);
    auto next = std::chrono::steady_clock::now();
    for (;;) {
      next += period;
      std::this_thread::sleep_until(next);
      AddMotion(float(dx), float(dy));
    }
  }).detach();
}

// ------------------------------------------------------------------------------------------------- keys
// A key is its Windows virtual-key number (the SDK's VirtualKey), left and right Shift, Ctrl and Alt as one; the
// mouse's buttons and wheel have numbers of their own above them.
constexpr uint32_t kMouseButton = 0x1000;  // + rex::ui::MouseEvent::Button (1 left, 2 right, 3 middle, 4, 5)
constexpr uint32_t kWheelUp = 0x1011;
constexpr uint32_t kWheelDown = 0x1012;

uint32_t Normalize(uint32_t virtual_key) {
  switch (virtual_key) {
    case 0xA0: case 0xA1: return 0x10;  // Shift
    case 0xA2: case 0xA3: return 0x11;  // Ctrl
    case 0xA4: case 0xA5: return 0x12;  // Alt
    default: return virtual_key;
  }
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

uint32_t KeyFromName(const std::string& raw) {
  std::string n = Trim(raw);
  for (char& c : n) c = char(std::toupper(static_cast<unsigned char>(c)));
  if (n.size() == 1) {
    const unsigned char c = static_cast<unsigned char>(n[0]);
    return ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : 0;
  }
  struct Name {
    const char* name;
    uint32_t key;
  };
  static const Name kNames[] = {
      {"SHIFT", 0x10},     {"CTRL", 0x11},      {"CONTROL", 0x11},   {"ALT", 0x12},        {"SPACE", 0x20},
      {"TAB", 0x09},       {"ENTER", 0x0D},     {"RETURN", 0x0D},    {"CAPSLOCK", 0x14},   {"BACKSPACE", 0x08},
      {"LEFT", 0x25},      {"UP", 0x26},        {"RIGHT", 0x27},     {"DOWN", 0x28},       {"INS", 0x2D},
      {"DEL", 0x2E},       {"HOME", 0x24},      {"END", 0x23},       {"PGUP", 0x21},       {"PGDN", 0x22},
      {"MOUSE1", 0x1001},  {"MOUSE2", 0x1002},  {"MOUSE3", 0x1003},  {"MOUSE4", 0x1004},   {"MOUSE5", 0x1005},
      {"MWHEELUP", kWheelUp},                   {"MWHEELDOWN", kWheelDown},
  };
  for (const Name& k : kNames)
    if (n == k.name) return k.key;
  if (n.size() >= 2 && n.size() <= 3 && n[0] == 'F') {
    const int f = std::atoi(n.c_str() + 1);
    if (f >= 1 && f <= 12 && f != 2 && n == "F" + std::to_string(f)) return 0x70u + uint32_t(f) - 1u;  // not F2
  }
  return 0;
}

// One of the game's console commands and nothing else: no quotes, no separators, no line ends.
bool PlainCommand(const std::string& command) {
  if (command.empty() || command.size() > 60) return false;
  for (const char c : command) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) || c == '+' || c == '-' || c == '_' || c == ' ' || c == '.')) return false;
  }
  return true;
}

struct Bind {
  uint32_t key;
  std::string command;
};

std::mutex g_keys_mutex;  // everything below
bool g_binds_read = false;
std::string g_binds_text;
std::vector<Bind> g_binds;
std::map<uint32_t, std::string> g_held;  // a key taken while down -> its "+" command ("" for a one-shot command)
std::map<std::string, int> g_active;     // a "+" command -> how many keys hold it
std::vector<std::string> g_out;          // for the game's command buffer

void PushLocked(std::string command) {
  if (g_out.size() < 256) g_out.push_back(std::move(command));
}

void RefreshBindsLocked() {
  const std::string text = REXCVAR_GET(cod2_pc_binds);
  if (g_binds_read && text == g_binds_text) return;
  g_binds_read = true;
  g_binds_text = text;
  g_binds.clear();
  int left_out = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(';', start);
    if (end == std::string::npos) end = text.size();
    const std::string item = Trim(text.substr(start, end - start));
    start = end + 1;
    if (item.empty()) continue;
    const size_t eq = item.find('=');
    const uint32_t key = eq == std::string::npos ? 0 : KeyFromName(item.substr(0, eq));
    const std::string command = eq == std::string::npos ? std::string() : Trim(item.substr(eq + 1));
    if (key == 0 || !PlainCommand(command)) {
      ++left_out;
      continue;
    }
    g_binds.push_back({key, command});
  }
  REXLOG_INFO("cod2: pc keys: {} keys bound ({} left out: a key name or command that isn't one)", g_binds.size(),
              left_out);
}

// A key went down. True when it has a command (also for the repeats of a key already taken).
bool Press(uint32_t key) {
  std::lock_guard lock(g_keys_mutex);
  RefreshBindsLocked();
  if (g_held.find(key) != g_held.end()) return true;
  for (const Bind& b : g_binds) {
    if (b.key != key) continue;
    if (b.command[0] == '+') {
      if (++g_active[b.command] == 1) PushLocked(b.command);
      g_held[key] = b.command;
    } else {
      PushLocked(b.command);
      g_held[key] = std::string();
    }
    return true;
  }
  return false;
}

void Release(uint32_t key) {
  std::lock_guard lock(g_keys_mutex);
  const auto held = g_held.find(key);
  if (held == g_held.end()) return;
  const std::string command = held->second;
  g_held.erase(held);
  if (command.empty()) return;
  const auto active = g_active.find(command);
  if (active == g_active.end()) return;
  if (--active->second <= 0) {
    g_active.erase(active);
    PushLocked("-" + command.substr(1));
  }
}

// A menu opened, the level ended or the F2 window came up: every held command is let go.
void ReleaseAll() {
  std::lock_guard lock(g_keys_mutex);
  for (const auto& [command, count] : g_active) PushLocked("-" + command.substr(1));
  g_active.clear();
  g_held.clear();
}

// ------------------------------------------------------------------------------------------------- the game's dvars
// The game's own mouse dvars follow the port's settings (the 360 game has no menu for them): looked at once a
// second, and a console line is sent only for one that differs.
void SyncMouseDvars(const uint8_t* base, int64_t now) {
  static int64_t next = 0;
  static int logged = 0;
  if (now < next || !MouseLookOn()) return;
  next = now + 1'000'000'000;
  char text[64];
  auto send = [&](const char* line) {
    std::lock_guard lock(g_keys_mutex);
    PushLocked(line);
    if (logged < 8) {
      ++logged;
      REXLOG_INFO("cod2: mouse look: {}", line);
    }
  };
  if (const uint32_t d = LoadBe32(base, kDvarSensitivity)) {
    const float want = float(REXCVAR_GET(cod2_mouse_sensitivity));
    if (std::fabs(LoadBeF32(base, d + 8) - want) > 0.0005f) {
      std::snprintf(text, sizeof(text), "set sensitivity %g", double(want));
      send(text);
    }
  }
  if (const uint32_t d = LoadBe32(base, kDvarMPitch)) {
    const float want = REXCVAR_GET(cod2_mouse_invert) ? -0.022f : 0.022f;
    if (std::fabs(LoadBeF32(base, d + 8) - want) > 0.00005f) {
      std::snprintf(text, sizeof(text), "set m_pitch %g", double(want));
      send(text);
    }
  }
  if (const uint32_t d = LoadBe32(base, kDvarMFilter)) {
    const bool want = REXCVAR_GET(cod2_mouse_smooth);
    if ((base[d + 8] != 0) != want) send(want ? "set m_filter 1" : "set m_filter 0");
  }
  if (const uint32_t d = LoadBe32(base, kDvarFreelook)) {
    if (base[d + 8] == 0) send("set cl_freelook 1");  // the PC copy's default: the mouse looks up and down
  }
}

float DvarFloat(const uint8_t* base, uint32_t global) {
  const uint32_t d = LoadBe32(base, global);
  return d ? LoadBeF32(base, d + 8) : NAN;
}

int DvarBool(const uint8_t* base, uint32_t global) {
  const uint32_t d = LoadBe32(base, global);
  return d ? int(base[d + 8] != 0) : -1;
}

// The Xbox 360 program multiplies the mouse's turn by (the picture's real horizontal field of view / cg_fov): its
// wide screen shows more than cg_fov (65 degrees becomes 80.7 at 16:9), so from the hip a count turns 1.2414 times
// the PC copy's sensitivity x m_yaw (measured, run cod2pc1: 0.13655 degrees a count at sensitivity 5, where the PC
// copy turns 0.11). This is that share's inverse for the current cg_fov: the counts are multiplied by it, so from
// the hip a count turns what it does on the PC, and aiming down the sights slows the mouse by the same share as it
// narrows the view.
float PcTurnShare(const uint8_t* base) {
  const float fov = DvarFloat(base, kDvarCgFov);
  if (!(fov >= 1.0f && fov <= 170.0f)) return 1.0f;
  const double half = double(fov) * 3.14159265358979323846 / 360.0;
  const double wide = 2.0 * std::atan(std::tan(half) * (4.0 / 3.0));  // the 16:9 picture's horizontal view
  return float(2.0 * half / wide);
}

// ------------------------------------------------------------------------------------------------- tests
struct DebugCounts {
  bool on = false;
  int32_t dx = 0;
  int32_t dy = 0;
};

DebugCounts ReadDebugCounts() {
  const std::string text = REXCVAR_GET(cod2_debug_mouse);
  DebugCounts d;
  if (!text.empty() && std::sscanf(text.c_str(), "%d,%d", &d.dx, &d.dy) == 2) d.on = true;
  return d;
}

struct KeyStep {
  double seconds = 0.0;
  uint32_t key = 0;
  bool down = false;
  std::string name;
  bool done = false;
};

std::atomic<bool> g_key_script_on{false};

// cod2_debug_keys, on the game's main thread: the steps whose time has come.
void RunKeyScript(int64_t now, bool playing) {
  static bool parsed = false;
  static std::vector<KeyStep> steps;
  static int64_t start = 0;
  if (!parsed) {
    parsed = true;
    const std::string text = REXCVAR_GET(cod2_debug_keys);
    size_t at = 0;
    while (at <= text.size() && !text.empty()) {
      size_t end = text.find('|', at);
      if (end == std::string::npos) end = text.size();
      const std::string item = Trim(text.substr(at, end - at));
      at = end + 1;
      const size_t colon = item.find(':');
      if (colon == std::string::npos || item.size() < colon + 3) continue;
      KeyStep s;
      s.seconds = std::strtod(item.c_str(), nullptr);
      s.down = item.back() == '+';
      s.name = Trim(item.substr(colon + 1, item.size() - colon - 2));
      s.key = KeyFromName(s.name);
      if (s.key != 0 && (item.back() == '+' || item.back() == '-')) steps.push_back(s);
    }
    g_key_script_on.store(!steps.empty());
    if (!steps.empty()) REXLOG_INFO("cod2: pc keys: test script of {} steps", steps.size());
  }
  if (steps.empty()) return;
  if (start == 0) {
    if (!playing) return;
    start = now;
  }
  const double t = double(now - start) / 1e9;
  for (KeyStep& s : steps) {
    if (s.done || t < s.seconds) continue;
    s.done = true;
    bool taken = true;
    if (s.down)
      taken = playing && REXCVAR_GET(cod2_pc_keys) && Press(s.key);
    else
      Release(s.key);
    REXLOG_INFO("cod2: pc keys: test: {} {} at {:.1f} s{}", s.name, s.down ? "down" : "up", t,
                taken ? "" : " (not taken: no command for it, or no level playing)");
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------- the window
void Listener::OnKeyDown(rex::ui::KeyEvent& e) {
  if (!KeysOn()) return;
  if (Press(Normalize(static_cast<uint32_t>(e.virtual_key())))) e.set_handled(true);
}

// A key let go is never taken: the SDK's driver may have seen it go down in a menu.
void Listener::OnKeyUp(rex::ui::KeyEvent& e) { Release(Normalize(static_cast<uint32_t>(e.virtual_key()))); }

void Listener::OnMouseDown(rex::ui::MouseEvent& e) {
  UpdateCapture();
  if (!KeysOn()) return;
  if (Press(kMouseButton + static_cast<uint32_t>(e.button()))) e.set_handled(true);
}

void Listener::OnMouseUp(rex::ui::MouseEvent& e) { Release(kMouseButton + static_cast<uint32_t>(e.button())); }

void Listener::OnMouseWheel(rex::ui::MouseEvent& e) {
  if (!KeysOn() || e.scroll_y() == 0) return;
  const uint32_t key = e.scroll_y() > 0 ? kWheelUp : kWheelDown;
  if (Press(key)) {  // a notch is a press and a release at once
    Release(key);
    e.set_handled(true);
  }
}

void Listener::OnMouseMove(rex::ui::MouseEvent& e) {
  UpdateCapture();
  if (!captured_) return;
  // The motion is the game's alone while it holds the pointer: the SDK's driver doesn't turn the right stick too.
  e.set_handled(true);
  if (relative_) {
    AddMotion(float(e.dx()), float(e.dy()));
    return;
  }
  // No pointer lock: measure from the window's center and put the cursor back there.
  AddMotion(float(e.x() - prev_x_), float(e.y() - prev_y_));
  int32_t cx = 0, cy = 0;
  if (window && window->WarpMouseToCenter(cx, cy)) {
    prev_x_ = cx;
    prev_y_ = cy;
  } else {
    prev_x_ = e.x();
    prev_y_ = e.y();
  }
}

void Listener::UpdateCapture() {
  if (!window) return;
  const bool want = MouseLookOn() && g_playing.load(std::memory_order_relaxed) && window->HasFocus();
  if (want == captured_) return;
  ClearMotion();  // motion from before the switch belongs to the menu, not to the view
  if (want) {
    prev_cursor_ = window->GetCursorVisibility();
    window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    window->CaptureMouse();
    relative_ = window->SetRelativeMouseMode(true);
    if (!relative_) window->WarpMouseToCenter(prev_x_, prev_y_);
    captured_ = true;
    REXLOG_INFO("cod2: mouse look holds the pointer ({})", relative_ ? "relative mode" : "re-centering");
  } else {
    if (relative_) window->SetRelativeMouseMode(false);
    window->ReleaseMouse();
    window->SetCursorVisibility(prev_cursor_);
    captured_ = false;
    relative_ = false;
  }
}

// ------------------------------------------------------------------------------------------------- the game's thread
void OnGameFrame(const uint8_t* base) {
  const int64_t now = NowNs();
  const Addresses a = Resolve(base);
  const uint32_t catchers = a.ok ? LoadBe32(base, a.catchers) : 0xFFFFFFFFu;
  g_catchers.store(catchers, std::memory_order_relaxed);
  const bool playing = catchers == 0 && now - g_last_cmd_ns.load(std::memory_order_relaxed) < kPlayingWindowNs &&
                       cod2::f2ui::GameGetsInput();
  if (g_playing.exchange(playing) && !playing) ReleaseAll();
  SyncMouseDvars(base, now);
  RunKeyScript(now, playing);
}

// [f2-menus] 2026-11-11 00:15 for the F2 window (f2::SetLevelRuns): the game made a usercmd within the last second
// and a half, whatever menu is open.
bool LevelRuns() { return NowNs() - g_last_cmd_ns.load(std::memory_order_relaxed) < kPlayingWindowNs; }

bool TakeCommands(std::vector<std::string>& out) {
  std::lock_guard lock(g_keys_mutex);
  if (g_out.empty()) return false;
  g_commands_sent.fetch_add(uint32_t(g_out.size()), std::memory_order_relaxed);
  for (std::string& command : g_out) out.push_back(std::move(command));
  g_out.clear();
  return true;
}

}  // namespace cod2::pc_input

namespace {

// CL_MouseMove(usercmd r3), once a usercmd: the pointer's motion goes into the game's current slot of counts first;
// `game` runs the game's own, which turns the view by them.
template <typename Ctx, typename Game>
void MouseMove(Ctx& ctx, uint8_t* base, Game game) {
  using namespace cod2::pc_input;
  const int64_t now = NowNs();
  g_last_cmd_ns.store(now, std::memory_order_relaxed);
  const uint32_t cmd = ctx.r3.u32;
  const Addresses a = Resolve(base);

  // The pointer's motion since the last usercmd (and the test's counts), as whole counts for the game; what is
  // left of a count stays for the next usercmd.
  static float carry_x = 0.0f, carry_y = 0.0f;
  static bool bad_index_logged = false;
  float fx = 0.0f, fy = 0.0f;
  StartTestMouseOnce();  // [f2-menus] 03:30 cod2_debug_mouse_events
  const bool test_mouse = g_test_mouse_on.load(std::memory_order_relaxed);
  if ((MouseLookOn() && g_playing.load(std::memory_order_relaxed)) || test_mouse) {
    std::lock_guard lock(g_motion_mutex);
    fx = g_dx;
    fy = g_dy;
    g_dx = g_dy = 0.0f;
  }
  const DebugCounts debug = ReadDebugCounts();
  if (debug.on) {
    fx += float(debug.dx);
    fy += float(debug.dy);
  }
  // [f2-menus] 2026-11-11 02:55 mouse acceleration inside the game (f2_accel, off by default): the counts of this
  // usercmd and the time since the last one give the speed; Raw Accel's curve turns them into the counts the game
  // gets. Before the game's own sensitivity, as the driver's would be.
  // 03:10: the pointer's own motion is done event by event in AddMotion (waw-decomp, board 02:5x: a flick must be
  // the same at any frame rate); here only the test's counts, which arrive once a usercmd, with the usercmd's time.
  {
    static int64_t last_ns = 0;
    const double ms = last_ns ? double(now - last_ns) / 1e6 : 0.0;
    last_ns = now;
    if (debug.on) {
      float tx = float(debug.dx), ty = float(debug.dy);
#if defined(COD2_HAVE_F2)
      f2::accel::Apply(&tx, &ty, ms);
#else
      (void)ms;
#endif
      fx += tx - float(debug.dx);
      fy += ty - float(debug.dy);
    }
  }
  const float share = REXCVAR_GET(cod2_mouse_match_pc) ? PcTurnShare(base) : 1.0f;
  fx = fx * share + carry_x;
  fy = fy * share + carry_y;
  const int32_t dx = static_cast<int32_t>(std::trunc(fx));
  const int32_t dy = static_cast<int32_t>(std::trunc(fy));
  carry_x = fx - float(dx);
  carry_y = fy - float(dy);
  if (a.ok && (dx != 0 || dy != 0)) {
    const uint32_t index = LoadBe32(base, a.index);
    if (index <= 1) {  // the game only ever has 0 or 1 here: anything else isn't the place this file thinks it is
      const uint32_t slot = index * 4u;
      StoreBe32(base, a.dx + slot, LoadBe32(base, a.dx + slot) + uint32_t(dx));
      StoreBe32(base, a.dy + slot, LoadBe32(base, a.dy + slot) + uint32_t(dy));
    } else if (!bad_index_logged) {
      bad_index_logged = true;
      REXLOG_WARN("cod2: mouse look: the game's mouse slot index is {:#x}, not 0 or 1: nothing written", index);
    }
  }

  const bool log = a.ok && (debug.on || test_mouse || g_key_script_on.load(std::memory_order_relaxed));
  const float pitch_before = log ? LoadBeF32(base, a.pitch) : 0.0f;
  const float yaw_before = log ? LoadBeF32(base, a.yaw) : 0.0f;

  game();

  if (!log) return;
  static int64_t window_start = 0;
  static int calls = 0;
  static double yaw_sum = 0.0, pitch_sum = 0.0;
  ++calls;
  yaw_sum += double(LoadBeF32(base, a.yaw)) - double(yaw_before);
  pitch_sum += double(LoadBeF32(base, a.pitch)) - double(pitch_before);
  if (window_start == 0) window_start = now;
  if (now - window_start < 1'000'000'000) return;
  const int forward = cmd ? int(static_cast<int8_t>(base[cmd + 0x18])) : 0;
  const int right = cmd ? int(static_cast<int8_t>(base[cmd + 0x19])) : 0;
  const uint32_t buttons = cmd ? LoadBe32(base, cmd + 4) : 0;
  REXLOG_INFO("cod2: pc input: {} usercmds a second; mouse +{},{} counts each: yaw {:.4f}, pitch {:.4f} degrees a "
              "usercmd; forward {} right {} buttons {:#x}; sensitivity {:.2f}, m_pitch {:.3f}, m_filter {}, "
              "cl_freelook {}; the game's turn share {:.4f}, ours {:.4f}; key catchers {:#x}; frame ms {}; {} key "
              "commands sent so far; the window {:.1f} ms",
              calls, debug.dx, debug.dy, yaw_sum / calls, pitch_sum / calls, forward, right, buttons,
              DvarFloat(base, kDvarSensitivity), DvarFloat(base, kDvarMPitch), DvarBool(base, kDvarMFilter),
              DvarBool(base, kDvarFreelook), LoadBeF32(base, a.scale), share,
              g_catchers.load(std::memory_order_relaxed), LoadBe32(base, kFrameMsec),
              g_commands_sent.load(std::memory_order_relaxed), double(now - window_start) / 1e6);
  window_start = now;
  calls = 0;
  yaw_sum = pitch_sum = 0.0;
}

}  // namespace

#if defined(COD2_MP)
REX_EXTERN(__imp__sub_8241A738);
REX_HOOK_RAW(sub_8241A738) { MouseMove(ctx, base, [&] { __imp__sub_8241A738(ctx, base); }); }
#else
REX_EXTERN(__imp__sub_8242B4D0);
REX_HOOK_RAW(sub_8242B4D0) { MouseMove(ctx, base, [&] { __imp__sub_8242B4D0(ctx, base); }); }
#endif
