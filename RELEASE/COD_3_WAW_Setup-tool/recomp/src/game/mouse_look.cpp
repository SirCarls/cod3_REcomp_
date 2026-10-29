// cod3 - raw mouse aim (see mouse_look.h).

#include "game/mouse_look.h"
#include "game/qte_actions.h"

#include <algorithm>
#include <cmath>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/aim_sensitivity.h"  // [claude-9b] sensitivity per weapon
#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_raw_mouse, true, "cod3/Mouse",
                    "Aim with the mouse directly (raw input, World at War style) instead of "
                    "emulating the right stick");
REXCVAR_DEFINE_DOUBLE(cod3_sensitivity, 5.0, "cod3/Mouse",
                      "Mouse sensitivity on the World at War scale (WaW default 5)")
    .range(0.01, 100.0);
REXCVAR_DEFINE_DOUBLE(cod3_m_yaw, 0.022, "cod3/Mouse", "Degrees of yaw per mouse count at sensitivity 1")
    .range(-1.0, 1.0);
REXCVAR_DEFINE_DOUBLE(cod3_m_pitch, 0.022, "cod3/Mouse",
                      "Degrees of pitch per mouse count at sensitivity 1")
    .range(-1.0, 1.0);
REXCVAR_DEFINE_BOOL(cod3_invert_mouse, false, "cod3/Mouse", "Invert vertical mouse look");
REXCVAR_DEFINE_DOUBLE(cod3_zoom_sensitivity, 1.0, "cod3/Mouse",
                      "Extra multiplier while aiming down sights (on top of the FOV ratio)")
    .range(0.05, 4.0);
// [claude-9b] Owner 2026-10-26: "Horizontal and vertical sensitivity sliders should only effect
// controller's sense. Make a new slider for mouse and keyboard that's independent from those two."
// The mouse has cod3_sensitivity (F2 > Controls, Pause > Options > AIM); the game's two rows are
// the controller's. True brings back the earlier behavior (both rows scaled the mouse too).
REXCVAR_DEFINE_BOOL(cod3_mouse_game_sensitivity, false, "cod3/Mouse",
                    "Also scale the mouse by the game's Horizontal / Vertical sensitivity rows "
                    "(the controller's). Off: the mouse uses only its own sensitivity");

// [claude-9b-think] 2026-10-29 UNTESTED (owner 11:35: "the way you turn with kbm now has some judder"; his settings:
// no frame rate limit, 585 fps in the log, a 240 Hz display, tearing allowed). The window thread hands the mouse's
// motion over in batches, as often as it runs its event loop. Every count was applied at the next usercmd, so above
// that rate some game frames turned by nothing and the next by a whole batch: an uneven turn in every displayed
// frame. With this on, each batch is spread over the time since the batch before (at most 8 ms), and a usercmd takes
// the motion up to "now less one batch interval" (on average, at most 8 ms). Every frame then turns by its own share,
// at any frame rate. The total turn is unchanged (every count is applied); the cost is up to one batch interval of delay.
REXCVAR_DEFINE_BOOL(cod3_mouse_resample, false, "cod3/Mouse",
                    "Even mouse turning at high frame rates: the mouse's motion is spread over the time it arrived in, "
                    "so every frame turns by its share (up to a few milliseconds later); off: each frame takes what "
                    "arrived since the last one")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, mnk_mode);

namespace cod3 {

namespace {
// "In gameplay" means the guest built a usercmd recently. The window has to
// ride out frame hitches (a new shader compiling when you flick to a new part
// of the level can stall a frame for a few hundred ms): if the mouse were let
// go during a hitch, the motion in that time would be lost and fast flicks
// would come up short whenever the frame rate dips. Menus and loading screens
// stop usercmds for much longer than this.
constexpr int64_t kInGameWindowNs = 1'500'000'000;
}

MouseLook& MouseLook::Get() {
  static MouseLook instance;
  return instance;
}

bool MouseLook::Enabled() {
  return REXCVAR_GET(cod3_raw_mouse) && REXCVAR_GET(mnk_mode);
}

namespace {
// The profile's Invert aim now (the controls settings are in guest memory
// from the start), else as last seen while aiming.
bool GameInvertsAim(const std::atomic<bool>& last_seen) {
  auto* ks = rex::system::kernel_state();
  if (uint8_t* base = ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr)
    return guest::ReadU8(base, addr::controls::kSettings + addr::controls::kInvertAim) != 0;
  return last_seen.load(std::memory_order_relaxed);
}
}  // namespace

bool MouseLook::MouseInverted() {
  return REXCVAR_GET(cod3_invert_mouse) != GameInvertsAim(Get().game_invert_);
}

void MouseLook::SetMouseInverted(bool inverted) {
  rex::cvar::SetFlagByName("cod3_invert_mouse", inverted != GameInvertsAim(Get().game_invert_) ? "true" : "false");
}

void MouseLook::AddDelta(float dx, float dy) {
  qte::AddMouse(dx, dy);  // the cursor of stick battle actions (game/qte_actions.h)
  std::lock_guard lock(mutex_);
  if (!REXCVAR_GET(cod3_mouse_resample)) {
    dx_ += dx;
    dy_ += dy;
    return;
  }
  // [claude-9b-think] cod3_mouse_resample: this batch covers the time since the one before (at most 8 ms).
  constexpr int64_t kMaxSpanNs = 8'000'000;
  const int64_t now = Clock::now().time_since_epoch().count();
  const int64_t since = last_batch_ns_ ? now - last_batch_ns_ : kMaxSpanNs;
  if (last_batch_ns_ && since > 0 && since < 4 * kMaxSpanNs)
    batch_interval_ns_ += (double(since) - batch_interval_ns_) * 0.1;
  last_batch_ns_ = now;
  const int64_t span = std::clamp<int64_t>(since, 1, kMaxSpanNs);
  if (batches_.size() > 256) batches_.pop_front();  // never read for a while: keep the newest
  batches_.push_back(Batch{now - span, now, dx, dy, 0.0f});
}

void MouseLook::ClearPending() {
  std::lock_guard lock(mutex_);
  dx_ = dy_ = 0.0f;
  batches_.clear();  // [claude-9b-think]
}

bool MouseLook::WantsCapture() const {
  if (!Enabled()) return false;
  int64_t now = Clock::now().time_since_epoch().count();
  // A stick battle action keeps the mouse even if the game stopped building
  // usercmds meanwhile (the mouse is its stick, game/qte_actions.h).
  return now - last_guest_tick_ns_.load(std::memory_order_relaxed) < kInGameWindowNs ||
         qte::MouseOwnsView();
}

void MouseLook::SetViewFov(float fov, float base_fov) {
  float ratio = (base_fov > 1.0f && fov > 0.0f) ? fov / base_fov : 1.0f;
  zoom_ratio_.store(std::clamp(ratio, 0.05f, 1.0f), std::memory_order_relaxed);
}

void MouseLook::ApplyToGuest(uint8_t* base) {
  const int64_t now = Clock::now().time_since_epoch().count();
  const int64_t prev = last_guest_tick_ns_.exchange(now, std::memory_order_relaxed);

  float dx, dy;
  {
    std::lock_guard lock(mutex_);
    dx = dx_;
    dy = dy_;
    dx_ = dy_ = 0.0f;
    // [claude-9b-think] cod3_mouse_resample: the motion of the batches up to now less one batch interval, each batch
    // spread evenly over its own time.
    const int64_t lag = int64_t(std::clamp(batch_interval_ns_, 0.0, 8'000'000.0));
    const int64_t until = now - lag;
    while (!batches_.empty()) {
      Batch& b = batches_.front();
      const float upto = b.end_ns <= until ? 1.0f
                         : b.start_ns >= until
                             ? 0.0f
                             : float(double(until - b.start_ns) / double(std::max<int64_t>(b.end_ns - b.start_ns, 1)));
      if (upto > b.taken) {
        dx += b.dx * (upto - b.taken);
        dy += b.dy * (upto - b.taken);
        b.taken = upto;
      }
      if (b.taken < 1.0f) break;
      batches_.pop_front();
    }
  }
  // Every count that arrived since the last usercmd is applied, however long
  // that frame took, so the turn doesn't depend on the frame rate. Only
  // motion from before gameplay resumed (menus, loading: no usercmds for
  // longer than the window, and the mouse was released meanwhile) is dropped
  // so it can't snap the view.
  if (!Enabled() || now - prev > kInGameWindowNs) return;
  if (dx == 0.0f && dy == 0.0f) return;
  // A stick battle action (valve, crowbar, rowing) takes the mouse as its
  // stick meanwhile (game/qte_actions.h): the view stays still.
  if (qte::MouseOwnsView()) return;

  // Keyboard/mouse drives local player 1 only; pads drive the others.
  const uint32_t local = guest::ReadU32(base, addr::kActiveLocalClient);
  if (local != 0) return;

  float ratio = zoom_ratio_.load(std::memory_order_relaxed);
  double sens = REXCVAR_GET(cod3_sensitivity);
  if (ratio < 0.999f) sens *= ratio * REXCVAR_GET(cod3_zoom_sensitivity);
  // [claude-9b] The weapon in hand's percentage, hip blending into aiming (game/aim_sensitivity.h).
  sens *= aimsens::Multiplier(base, local);

  // The game's own Options > Controls rows: Invert aim flips the mouse too;
  // horizontal / vertical sensitivity are the controller's only, unless
  // cod3_mouse_game_sensitivity asks for them (then 1.0 at the default 25).
  namespace c = addr::controls;
  auto game_scale = [&](uint32_t field) {
    int32_t v = static_cast<int32_t>(guest::ReadU32(base, c::kSettings + field));
    if (v < 0 || v > 1000) return 1.0;  // not set up yet
    return v == 0 ? double(c::kSensZero) : v * double(c::kSensScale);
  };
  const bool rows_too = REXCVAR_GET(cod3_mouse_game_sensitivity);  // [claude-9b] off: controller only
  const double scale_x = rows_too ? game_scale(c::kHorizontalSens) : 1.0;
  const double scale_y = rows_too ? game_scale(c::kVerticalSens) : 1.0;
  const bool game_invert = guest::ReadU8(base, c::kSettings + c::kInvertAim) != 0;
  game_invert_.store(game_invert, std::memory_order_relaxed);

  const uint32_t angles = addr::kClientActive + local * addr::kClientActiveStride + addr::kViewAnglesOffset;
  float pitch = guest::ReadF32(base, angles + 0);
  float yaw = guest::ReadF32(base, angles + 4);
  const double invert = (REXCVAR_GET(cod3_invert_mouse) != game_invert) ? -1.0 : 1.0;
  yaw -= static_cast<float>(dx * sens * scale_x * REXCVAR_GET(cod3_m_yaw));
  pitch += static_cast<float>(dy * sens * scale_y * REXCVAR_GET(cod3_m_pitch) * invert);
  if (std::isfinite(yaw) && std::isfinite(pitch)) {
    guest::WriteF32(base, angles + 0, pitch);
    guest::WriteF32(base, angles + 4, yaw);
  }
}

}  // namespace cod3

// CL_AdjustAngles: let the game apply stick / key turning, then add the mouse.
REX_EXTERN(__imp__CL_AdjustAngles);
REX_HOOK_RAW(CL_AdjustAngles) {
  __imp__CL_AdjustAngles(ctx, base);
  cod3::MouseLook::Get().ApplyToGuest(base);
}
