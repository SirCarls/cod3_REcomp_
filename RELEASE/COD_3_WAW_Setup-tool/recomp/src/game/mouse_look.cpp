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
  dx_ += dx;
  dy_ += dy;
}

void MouseLook::ClearPending() {
  std::lock_guard lock(mutex_);
  dx_ = dy_ = 0.0f;
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

  // The game's own Options > Controls rows apply to the mouse too:
  // horizontal / vertical sensitivity scale it the way they scale the stick
  // (1.0 at the default 25), and Invert aim flips it.
  namespace c = addr::controls;
  auto game_scale = [&](uint32_t field) {
    int32_t v = static_cast<int32_t>(guest::ReadU32(base, c::kSettings + field));
    if (v < 0 || v > 1000) return 1.0;  // not set up yet
    return v == 0 ? double(c::kSensZero) : v * double(c::kSensScale);
  };
  const double scale_x = game_scale(c::kHorizontalSens);
  const double scale_y = game_scale(c::kVerticalSens);
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
