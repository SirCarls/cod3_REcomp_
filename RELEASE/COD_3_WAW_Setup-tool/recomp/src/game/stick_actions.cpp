// cod3 - battle actions that ask for the right stick, with the mouse (see
// stick_actions.h).

#include "game/stick_actions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

namespace cod3::stick_actions {
namespace {

enum class Kind : int { kNone = 0, kSwirl = 1, kHorizontal = 2, kVertical = 3 };

// Which action ran last and until when it counts as running (it runs every
// frame while it waits for input).
std::atomic<int> g_kind{0};
std::atomic<int64_t> g_until_ns{0};

std::mutex g_mutex;
float g_dx = 0.0f, g_dy = 0.0f;   // mouse counts since the last pad read
float g_x = 0.0f, g_y = 0.0f;     // the stick made from them, -1..1
bool g_logged = false;

// Mouse counts for a full push of the stick in one pad read, and how much of
// the stick stays from the read before (so a circle drawn with the mouse
// turns the stick round smoothly instead of flicking).
constexpr float kCountsForFull = 24.0f;
constexpr float kKeep = 0.65f;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void NoteRunning(Kind kind) {
  g_kind.store(int(kind), std::memory_order_relaxed);
  g_until_ns.store(NowNs() + 250'000'000, std::memory_order_relaxed);  // 0.25 s
}

int16_t ToAxis(float v) { return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); }

}  // namespace

void AddMouse(float dx, float dy) {
  if (g_kind.load(std::memory_order_relaxed) == int(Kind::kNone)) return;
  std::lock_guard lock(g_mutex);
  g_dx += dx;
  g_dy += dy;
}

void ApplyToPad(uint8_t* base, uint32_t state) {
  const Kind kind = Kind(g_kind.load(std::memory_order_relaxed));
  if (kind == Kind::kNone) return;
  if (NowNs() > g_until_ns.load(std::memory_order_relaxed)) {
    g_kind.store(int(Kind::kNone), std::memory_order_relaxed);
    std::lock_guard lock(g_mutex);
    g_dx = g_dy = g_x = g_y = 0.0f;
    return;
  }
  float x, y;
  {
    std::lock_guard lock(g_mutex);
    g_x = g_x * kKeep + g_dx / kCountsForFull;
    g_y = g_y * kKeep + g_dy / kCountsForFull;
    g_dx = g_dy = 0.0f;
    const float length = std::hypot(g_x, g_y);
    if (length > 1.0f) {
      g_x /= length;
      g_y /= length;
    }
    x = g_x;
    y = g_y;
  }
  if (kind == Kind::kHorizontal) y = 0.0f;
  if (kind == Kind::kVertical) x = 0.0f;
  // Only mouse movement writes the stick: an idle mouse leaves a controller's
  // stick as it is.
  if (std::hypot(x, y) < 0.08f) return;
  if (!g_logged) {
    g_logged = true;
    REXLOG_INFO("cod3: a battle action asks for the right stick: the mouse moves it");
  }
  // X_INPUT_STATE: s16 RX at +12, RY at +14 (up is positive; the mouse's y
  // grows downward).
  guest::WriteU16(base, state + 12, uint16_t(ToAxis(x)));
  guest::WriteU16(base, state + 14, uint16_t(ToAxis(-y)));
}

}  // namespace cod3::stick_actions

// ---------------------------------------------------------------------------
// The actions' per-frame input step (vtable slot 5): while one runs, the
// mouse is its right stick.
// ---------------------------------------------------------------------------
REX_EXTERN(__imp__InteractInputRcvrStickSwirl__vfunc_5);
REX_HOOK_RAW(InteractInputRcvrStickSwirl__vfunc_5) {
  cod3::stick_actions::NoteRunning(cod3::stick_actions::Kind::kSwirl);
  __imp__InteractInputRcvrStickSwirl__vfunc_5(ctx, base);
}

// StickToggleHoriz and StickToggleVert share this step (0x824F6498); their
// vtables are 0x8206CA28 and 0x8206CA0C.
REX_EXTERN(__imp__sub_824F6498);
REX_HOOK_RAW(sub_824F6498) {
  const uint32_t vtable = cod3::guest::ReadU32(base, ctx.r3.u32);
  cod3::stick_actions::NoteRunning(vtable == 0x8206CA0C ? cod3::stick_actions::Kind::kVertical
                                                        : cod3::stick_actions::Kind::kHorizontal);
  __imp__sub_824F6498(ctx, base);
}
