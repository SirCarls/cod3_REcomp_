// waw - aim with the mouse through the game's own mouse look (see waw_mouse.h). [f2-menus]
//
// The same way as Call of Duty 4's (recomp_cod4/src/cod4_mouse.cpp, acct2-cod4's, measured there: 0.110 degrees of
// yaw a count at m_yaw 0.022 x sensitivity 5, the same at 10 and 30 ms frames), with World at War's addresses.

#include "waw_mouse.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>  // [f2-menus] the test mouse

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/waw_pch.h"
#include "waw_f2.h"
#include "f2/f2_common.h"  // [f2-menus] 2026-11-11 f2::accel: mouse acceleration and the sensitivity randomizer inside the game
#include "waw_keys.h"

REXCVAR_DEFINE_BOOL(waw_mouse_look, true, "waw",
                    "Aim with the mouse through the game's own mouse look (its sensitivity, m_yaw, m_pitch, m_filter "
                    "and cl_mouseAccel), the pointer held during gameplay; off: the SDK's mouse-to-right-stick "
                    "(mnk_mouse)");
REXCVAR_DEFINE_STRING(waw_debug_mouse, "", "waw",
                      "Debug: mouse counts \"dx,dy\" added at every read, as if the mouse moved (a turn test without a "
                      "mouse); once a second the log shows what the game made of them");
// [f2-menus] 2026-11-11 04:00 (owner 03:24: "just so we are clear FPS cannot affect mouse input."): a test mouse
// that sends events at its own rate, for the check that the frame rate has no part in the mouse.
REXCVAR_DEFINE_STRING(waw_debug_mouse_events, "", "waw",
                      "Debug: a test mouse \"dx,dy,hz\": dx,dy counts hz times a second from a thread of its own, as "
                      "a mouse sends them whatever the game draws; once a second the log shows the counts handed to "
                      "the game");

REXCVAR_DECLARE(bool, mnk_mode);

namespace {

// clientActive_t (CL_GetMouseMovement's r3): the two slots of counts and the one being filled. The usercmd's frame
// milliseconds, CL_MouseMove's divisor for the mouse rate (cl_mouseAccel only).
constexpr uint32_t kMouseDx = 0x27128;
constexpr uint32_t kMouseDy = 0x27130;
constexpr uint32_t kMouseIndex = 0x27138;
constexpr uint32_t kFrameMsec = 0x842F34E4u;

using Clock = std::chrono::steady_clock;

// The game read mouse counts this recently: gameplay. A menu or a load stops the reads for longer.
constexpr int64_t kInGameWindowNs = 1'500'000'000;

std::atomic<int64_t> g_last_read_ns{0};
std::mutex g_mutex;
float g_dx = 0.0f;  // motion not yet handed to the game; the fractions of a count stay for the next read
float g_dy = 0.0f;

int64_t NowNs() { return Clock::now().time_since_epoch().count(); }

void AddMotion(float dx, float dy) {
  // [f2-menus] 2026-11-11 03:20 mouse acceleration and the sensitivity randomizer inside the game (f2_accel,
  // f2_sens_random; both off by default, and then this changes nothing): each mouse event's counts with the time
  // since the event before it, so a flick comes out the same at any frame rate. Mouse events come on one thread.
  {
    thread_local int64_t last_ns = 0;  // [f2-menus] 04:00 a thread's own: the test mouse has its own
    const int64_t now = NowNs();
    const double ms = last_ns ? double(now - last_ns) / 1e6 : 0.0;
    last_ns = now;
    f2::accel::Apply(&dx, &dy, ms);
  }
  std::lock_guard lock(g_mutex);
  g_dx += dx;
  g_dy += dy;
}

// [f2-menus] 2026-11-11 04:00 waw_debug_mouse_events: started once, at the first read that finds the setting.
std::atomic<bool> g_test_mouse_on{false};
void StartTestMouseOnce() {
  static std::atomic<bool> tried{false};
  if (tried.exchange(true)) return;
  const std::string text = REXCVAR_GET(waw_debug_mouse_events);
  int dx = 0, dy = 0, hz = 0;
  if (text.empty() || std::sscanf(text.c_str(), "%d,%d,%d", &dx, &dy, &hz) != 3 || hz < 1 || hz > 8000) return;
  g_test_mouse_on.store(true, std::memory_order_relaxed);
  REXLOG_INFO("waw: test mouse: {},{} counts {} times a second from its own thread", dx, dy, hz);
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

void ClearMotion() {
  std::lock_guard lock(g_mutex);
  g_dx = g_dy = 0.0f;
}

bool InGame() { return NowNs() - g_last_read_ns.load(std::memory_order_relaxed) < kInGameWindowNs; }

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void StoreBe32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

float LoadBeF32(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadBe32(base, address);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

struct DebugCounts {
  bool on = false;
  int32_t dx = 0;
  int32_t dy = 0;
};

DebugCounts ReadDebugCounts() {
  const std::string text = REXCVAR_GET(waw_debug_mouse);
  DebugCounts d;
  if (!text.empty() && std::sscanf(text.c_str(), "%d,%d", &d.dx, &d.dy) == 2) d.on = true;
  return d;
}

// A pointer into the caller's stack frame (the game's mx and my), so the debug line only reads real locals.
bool NearStack(uint32_t pointer, uint32_t sp) { return pointer >= sp && pointer - sp < 0x1000; }

// Keyboard and mouse as a PC shooter's, through the SDK's keyboard pad (its default is off). The 360 game reads a pad,
// so each key presses the button that does the same in the 360's default layout; the same keys as Call of Duty 4's
// (cod4_app.cpp) and Call of Duty 3's (World at War's PC style), the D-pad's equipment on N, 3, 5 and 6.
struct InputDefault {
  const char* flag;
  const char* value;
};
constexpr InputDefault kInputDefaults[] = {
    {"mnk_mode", "true"},
    {"mnk_mouse", "true"},                // the menus' right stick; in play waw_mouse_look takes the motion
    {"keybind_right_trigger", "LMB"},     // fire
    {"keybind_left_trigger", "RMB"},      // aim down the sights
    {"keybind_a", "Space"},               // jump; select in menus
    {"keybind_b", "C,Backspace"},         // crouch (hold: prone); back in menus
    {"keybind_x", "R,F"},                 // reload; hold: use
    {"keybind_y", "1,2"},                 // switch weapon
    {"keybind_right_shoulder", "G"},      // frag grenade
    {"keybind_left_shoulder", "4"},       // special grenade
    {"keybind_lstick_press", "Shift"},    // sprint; hold breath while scoped
    {"keybind_rstick_press", "V,MMB"},    // melee
    {"keybind_dpad_up", "N"},             // the D-pad's equipment slots
    {"keybind_dpad_left", "3"},
    {"keybind_dpad_right", "5"},
    {"keybind_dpad_down", "6"},
    {"keybind_back", "Tab"},              // objectives, scores
    {"keybind_start", "Escape,Return"},   // pause menu; START at the title screen
};

}  // namespace

namespace waw::mouse {

bool Enabled() { return REXCVAR_GET(waw_mouse_look) && REXCVAR_GET(mnk_mode); }

bool InGameplay() { return InGame(); }

// Each only while nobody chose: the command line, the environment or the settings file keeps its value.
void ApplyInputDefaults() {
  for (const auto& d : kInputDefaults) {
    if (rex::cvar::GetFlagInfo(d.flag) && rex::cvar::GetFlagSource(d.flag) == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName(d.flag, d.value);
  }
  REXLOG_INFO("waw: keyboard pad {}, mouse look {} (mnk_mouse {}), fire {}, aim {}, START {}",
              rex::cvar::GetFlagByName("mnk_mode"), REXCVAR_GET(waw_mouse_look) ? "on" : "off",
              rex::cvar::GetFlagByName("mnk_mouse"), rex::cvar::GetFlagByName("keybind_right_trigger"),
              rex::cvar::GetFlagByName("keybind_left_trigger"), rex::cvar::GetFlagByName("keybind_start"));
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

void Listener::OnMouseDown(rex::ui::MouseEvent& e) {
  (void)e;  // the buttons go on to the SDK's driver (fire, aim)
  UpdateCapture();
}

void Listener::UpdateCapture() {
  if (!window) return;
  const bool want = Enabled() && InGame() && !waw::keys::MenuTakesKeys() && window->HasFocus() &&
                    waw::f2ui::GameGetsInput();
  if (want == captured_) return;
  ClearMotion();  // motion from before the switch belongs to the menu, not to the view
  if (want) {
    prev_cursor_ = window->GetCursorVisibility();
    window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    window->CaptureMouse();
    relative_ = window->SetRelativeMouseMode(true);
    if (!relative_) window->WarpMouseToCenter(prev_x_, prev_y_);
    captured_ = true;
    REXLOG_INFO("waw: mouse look holds the pointer ({})", relative_ ? "relative mode" : "re-centering");
  } else {
    if (relative_) window->SetRelativeMouseMode(false);
    window->ReleaseMouse();
    window->SetCursorVisibility(prev_cursor_);
    captured_ = false;
    relative_ = false;
  }
}

}  // namespace waw::mouse

// CL_GetMouseMovement(cl, &mx, &my) (0x822C9068): the host's counts go into the current slot first, then the game's
// own code reads them (with m_filter) and clears the next slot.
REX_EXTERN(__imp__sub_822C9068);
REX_HOOK_RAW(sub_822C9068) {
  g_last_read_ns.store(NowNs(), std::memory_order_relaxed);
  const uint32_t cl = ctx.r3.u32;
  const uint32_t mx_ptr = ctx.r4.u32;
  const uint32_t my_ptr = ctx.r5.u32;
  const uint32_t sp = ctx.r1.u32;

  int32_t dx = 0, dy = 0;
  StartTestMouseOnce();  // [f2-menus] 04:00 waw_debug_mouse_events
  const bool test_mouse = g_test_mouse_on.load(std::memory_order_relaxed);
  if (waw::mouse::Enabled() || test_mouse) {
    std::lock_guard lock(g_mutex);
    dx = static_cast<int32_t>(std::trunc(g_dx));
    dy = static_cast<int32_t>(std::trunc(g_dy));
    g_dx -= float(dx);
    g_dy -= float(dy);
  }
  const DebugCounts debug = ReadDebugCounts();
  if (debug.on) {
    dx += debug.dx;
    dy += debug.dy;
  }
  // [f2-menus] 2026-11-11 03:20 the test's counts go through the same curve once a read, with the read's time, so
  // a scripted run can check it (what the curve adds or takes is kept to whole counts, the rest carried).
  if (debug.on) {
    static int64_t last_ns = 0;
    static float carry_x = 0.0f, carry_y = 0.0f;
    const int64_t now = NowNs();
    const double ms = last_ns ? double(now - last_ns) / 1e6 : 0.0;
    last_ns = now;
    float tx = float(debug.dx), ty = float(debug.dy);
    f2::accel::Apply(&tx, &ty, ms);
    tx += carry_x - float(debug.dx);
    ty += carry_y - float(debug.dy);
    const int32_t more_x = static_cast<int32_t>(std::trunc(tx)), more_y = static_cast<int32_t>(std::trunc(ty));
    carry_x = tx - float(more_x);
    carry_y = ty - float(more_y);
    dx += more_x;
    dy += more_y;
  }
  if (cl != 0 && (dx != 0 || dy != 0)) {
    const uint32_t slot = (LoadBe32(base, cl + kMouseIndex) & 1u) * 4u;
    StoreBe32(base, cl + kMouseDx + slot, LoadBe32(base, cl + kMouseDx + slot) + uint32_t(dx));
    StoreBe32(base, cl + kMouseDy + slot, LoadBe32(base, cl + kMouseDy + slot) + uint32_t(dy));
  }

  // [f2-menus] 04:00 the test mouse: the counts handed to the game each second (the same at any frame rate).
  if (test_mouse) {
    static int64_t events_start = 0;
    static int64_t sum_x = 0, sum_y = 0;
    static int events_reads = 0;
    const int64_t now = NowNs();
    if (events_start == 0) {
      events_start = now;
    } else {
      ++events_reads;
      sum_x += dx;
      sum_y += dy;
      if (now - events_start >= 1'000'000'000) {
        REXLOG_INFO("waw: test mouse: {} reads, {},{} counts handed to the game in {:.1f} ms", events_reads, sum_x,
                    sum_y, double(now - events_start) / 1e6);
        events_start = now;
        sum_x = sum_y = 0;
        events_reads = 0;
      }
    }
  }

  __imp__sub_822C9068(ctx, base);

  if (debug.on) {
    static int64_t window_start = 0;
    static int reads = 0;
    ++reads;
    const int64_t now = NowNs();
    if (window_start == 0) window_start = now;
    if (now - window_start >= 1'000'000'000) {
      const float mx = NearStack(mx_ptr, sp) ? LoadBeF32(base, mx_ptr) : NAN;
      const float my = NearStack(my_ptr, sp) ? LoadBeF32(base, my_ptr) : NAN;
      REXLOG_INFO("waw: mouse debug: {} reads a second, +{},{} counts each; the game's mx,my {:.2f},{:.2f}; "
                  "frame ms {}",
                  reads, debug.dx, debug.dy, mx, my, LoadBe32(base, kFrameMsec));
      window_start = now;
      reads = 0;
    }
  }
}
