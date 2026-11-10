// cod4 / cod4mp - aim with the mouse through the game's own mouse look (see cod4_mouse.h). [acct2-cod4]

#include "cod4_mouse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#ifdef COD4_MP
#include "generated/default_mp/cod4mp_pch.h"
#else
#include "generated/default/cod4_pch.h"
#endif

// [acct2-cod4] On by default from 2026-11-07 21:43: mousetest2 measured 0.110 degrees of yaw a count (m_yaw 0.022 x
// sensitivity 5) at frame times of 10 to 30 ms alike, so the turn doesn't depend on the frame rate.
REXCVAR_DEFINE_BOOL(cod4_mouse_look, true, "cod4",
                    "Aim with the mouse through the game's own mouse look (its sensitivity, m_yaw, m_pitch and "
                    "m_filter), the pointer held during gameplay; off: the SDK's mouse-to-right-stick (mnk_mouse)");
REXCVAR_DEFINE_STRING(cod4_debug_mouse, "", "cod4",
                      "Debug: mouse counts \"dx,dy\" added at every read, as if the mouse moved (a turn test without "
                      "a mouse); once a second the log shows what the game made of them and how far CL_MouseMove "
                      "turned the view");

REXCVAR_DECLARE(bool, mnk_mode);

namespace cod4::mouse {

namespace {

using Clock = std::chrono::steady_clock;

// The game read mouse counts this recently: gameplay. A menu or a load stops the reads for longer.
constexpr int64_t kInGameWindowNs = 1'500'000'000;

std::atomic<int64_t> g_last_read_ns{0};
std::mutex g_mutex;
float g_dx = 0.0f;  // motion not yet handed to the game; the fractions of a count stay for the next read
float g_dy = 0.0f;

int64_t NowNs() { return Clock::now().time_since_epoch().count(); }

void AddMotion(float dx, float dy) {
  std::lock_guard lock(g_mutex);
  g_dx += dx;
  g_dy += dy;
}

void ClearMotion() {
  std::lock_guard lock(g_mutex);
  g_dx = g_dy = 0.0f;
}

bool InGame() { return NowNs() - g_last_read_ns.load(std::memory_order_relaxed) < kInGameWindowNs; }

}  // namespace

bool Enabled() { return REXCVAR_GET(cod4_mouse_look) && REXCVAR_GET(mnk_mode); }

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
  const bool want = Enabled() && InGame() && window->HasFocus();
  if (want == captured_) return;
  ClearMotion();  // motion from before the switch belongs to the menu, not to the view
  if (want) {
    prev_cursor_ = window->GetCursorVisibility();
    window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    window->CaptureMouse();
    relative_ = window->SetRelativeMouseMode(true);
    if (!relative_) window->WarpMouseToCenter(prev_x_, prev_y_);
    captured_ = true;
    REXLOG_INFO("cod4: mouse look holds the pointer ({})", relative_ ? "relative mode" : "re-centering");
  } else {
    if (relative_) window->SetRelativeMouseMode(false);
    window->ReleaseMouse();
    window->SetCursorVisibility(prev_cursor_);
    captured_ = false;
    relative_ = false;
  }
}

}  // namespace cod4::mouse

namespace {

// clientActive_t (CL_GetMouseMovement's r3): the two slots of counts and the one being filled. The frame's
// milliseconds (the usercmd builder's frame_msec): CL_MouseMove divides the mouse rate by it (mouse acceleration
// only) and caps the turn at a max-yaw/pitch-speed float x it, when that float isn't 0. cod4mp runs the same code
// on a larger clientActive_t (its CL_GetMouseMovement 0x822D14C8 reads these offsets; CL_MouseMove 0x822D3D48
// reads frame_msec five times, as the campaign's does).
#ifdef COD4_MP
constexpr uint32_t kMouseDx = 152768;
constexpr uint32_t kMouseDy = 152776;
constexpr uint32_t kMouseIndex = 152784;
constexpr uint32_t kFrameMsec = 0x82435738u;
#else
constexpr uint32_t kMouseDx = 51520;
constexpr uint32_t kMouseDy = 51528;
constexpr uint32_t kMouseIndex = 51536;
constexpr uint32_t kFrameMsec = 0x824A436Cu;
#endif
// The turn test compares 64 KB of clientActive_t around the mouse slots before and after CL_MouseMove (once a
// usercmd): the view angles come just after the slots (+84 in the campaign), and cod4mp's slots are past the first
// 64 KB of its larger clientActive_t, so the window starts 32 KB before them.
constexpr uint32_t kTurnScanBytes = 0x10000;
constexpr uint32_t kTurnScanStart = kMouseDx > kTurnScanBytes / 2 ? kMouseDx - kTurnScanBytes / 2 : 0;

std::atomic<uint32_t> g_client_active{0};  // CL_GetMouseMovement's r3, for the turn test

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

struct DebugCounts {
  bool on = false;
  int32_t dx = 0;
  int32_t dy = 0;
};

DebugCounts ReadDebugCounts() {
  const std::string text = REXCVAR_GET(cod4_debug_mouse);
  DebugCounts d;
  if (!text.empty() && std::sscanf(text.c_str(), "%d,%d", &d.dx, &d.dy) == 2) d.on = true;
  return d;
}

// A pointer into the caller's stack frame (the game's mx and my), so the debug line only reads real locals.
bool NearStack(uint32_t pointer, uint32_t sp) { return pointer >= sp && pointer - sp < 0x1000; }

}  // namespace

namespace {

// CL_GetMouseMovement(cl, &mx, &my): the host's counts go into the current slot first; `game` runs the game's own.
template <typename Ctx, typename Game>
void GetMouseMovement(Ctx& ctx, uint8_t* base, Game game) {
  using namespace cod4::mouse;
  g_last_read_ns.store(NowNs(), std::memory_order_relaxed);
  const uint32_t cl = ctx.r3.u32;
  g_client_active.store(cl, std::memory_order_relaxed);
  const uint32_t mx_ptr = ctx.r4.u32;
  const uint32_t my_ptr = ctx.r5.u32;
  const uint32_t sp = ctx.r1.u32;

  int32_t dx = 0, dy = 0;
  if (Enabled()) {
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
  if (dx != 0 || dy != 0) {
    const uint32_t slot = (LoadBe32(base, cl + kMouseIndex) & 1u) * 4u;
    StoreBe32(base, cl + kMouseDx + slot, LoadBe32(base, cl + kMouseDx + slot) + uint32_t(dx));
    StoreBe32(base, cl + kMouseDy + slot, LoadBe32(base, cl + kMouseDy + slot) + uint32_t(dy));
  }

  game();

  if (debug.on) {
    static int64_t window_start = 0;
    static int reads = 0;
    ++reads;
    const int64_t now = NowNs();
    if (window_start == 0) window_start = now;
    if (now - window_start >= 1'000'000'000) {
      const float mx = NearStack(mx_ptr, sp) ? LoadBeF32(base, mx_ptr) : NAN;
      const float my = NearStack(my_ptr, sp) ? LoadBeF32(base, my_ptr) : NAN;
      REXLOG_INFO("cod4: mouse debug: {} reads a second, +{},{} counts each; the game's mx,my {:.2f},{:.2f}; "
                  "frame ms {}",
                  reads, debug.dx, debug.dy, mx, my, LoadBe32(base, kFrameMsec));
      window_start = now;
      reads = 0;
    }
  }
}

}  // namespace

namespace {

// The turn test: what CL_MouseMove changed in clientActive_t, per offset, over about a second.
struct TurnChange {
  int times = 0;
  double sum = 0.0;  // of the float differences
};

struct TurnTest {
  std::vector<uint8_t> before;
  std::vector<std::pair<uint32_t, TurnChange>> changes;  // offset, change (a short list: the view angles)
  int calls = 0;
  int64_t msec_sum = 0;
  int64_t window_start = 0;

  TurnChange& At(uint32_t offset) {
    for (auto& [o, c] : changes)
      if (o == offset) return c;
    changes.push_back({offset, TurnChange{}});
    return changes.back().second;
  }
};

TurnTest g_turn;  // CL_MouseMove runs on the game's main thread only

bool IsMouseSlot(uint32_t offset) { return offset >= kMouseDx && offset < kMouseIndex + 4; }

// CL_MouseMove (once a usercmd): with cod4_debug_mouse set, log how far it turned the view for the counts it read.
template <typename Game>
void MouseMove(uint8_t* base, Game game) {
  const uint32_t cl = g_client_active.load(std::memory_order_relaxed);
  const DebugCounts debug = ReadDebugCounts();
  if (!debug.on || cl == 0) {
    game();
    return;
  }
  TurnTest& t = g_turn;
  const uint32_t start = cl + kTurnScanStart;
  t.before.assign(base + start, base + start + kTurnScanBytes);
  game();

  ++t.calls;
  t.msec_sum += LoadBe32(base, kFrameMsec);
  const uint8_t* after = base + start;
  for (uint32_t i = 0; i < kTurnScanBytes; i += 4) {
    const uint32_t off = kTurnScanStart + i;  // from the start of clientActive_t, as the log shows it
    if (std::memcmp(after + i, t.before.data() + i, 4) == 0 || IsMouseSlot(off)) continue;
    const float now = LoadBeF32(after, i);
    const float was = LoadBeF32(t.before.data(), i);
    TurnChange& c = t.At(off);
    ++c.times;
    if (std::isfinite(now) && std::isfinite(was)) c.sum += double(now) - double(was);
    if (t.changes.size() > 32) break;  // not a turn: something else writes here
  }

  const int64_t now_ns = cod4::mouse::NowNs();
  if (t.window_start == 0) t.window_start = now_ns;
  if (now_ns - t.window_start < 1'000'000'000) return;
  std::sort(t.changes.begin(), t.changes.end(),
            [](const auto& a, const auto& b) { return a.second.times > b.second.times; });
  std::string list;
  const int counts = debug.dx != 0 ? debug.dx : debug.dy;
  for (size_t i = 0; i < t.changes.size() && i < 4; ++i) {
    const auto& [off, c] = t.changes[i];
    char item[96];
    std::snprintf(item, sizeof(item), "%s+%u: %d times, %.5f a call, %.6f a count", list.empty() ? "" : "; ", off,
                  c.times, c.sum / t.calls, counts != 0 ? c.sum / t.calls / counts : 0.0);
    list += item;
  }
  REXLOG_INFO("cod4: mouse turn: {} calls, frame ms {:.1f} on average, {} changed words: {}", t.calls,
              double(t.msec_sum) / t.calls, t.changes.size(), list.empty() ? "none" : list);
  t.changes.clear();
  t.calls = 0;
  t.msec_sum = 0;
  t.window_start = now_ns;
}

}  // namespace

#ifdef COD4_MP
REX_EXTERN(__imp__sub_822D14C8);
REX_HOOK_RAW(sub_822D14C8) { GetMouseMovement(ctx, base, [&] { __imp__sub_822D14C8(ctx, base); }); }
REX_EXTERN(__imp__sub_822D3D48);
REX_HOOK_RAW(sub_822D3D48) { MouseMove(base, [&] { __imp__sub_822D3D48(ctx, base); }); }
#else
REX_EXTERN(__imp__sub_822CAFB0);
REX_HOOK_RAW(sub_822CAFB0) { GetMouseMovement(ctx, base, [&] { __imp__sub_822CAFB0(ctx, base); }); }
REX_EXTERN(__imp__sub_822CC838);
REX_HOOK_RAW(sub_822CC838) { MouseMove(base, [&] { __imp__sub_822CC838(ctx, base); }); }
#endif
