// cod3 - cg_fov / cg_fovScale.
//
// CG_GetViewFov (0x8243A198) is the single place cgame turns cg_fov into the
// horizontal view angle (it also folds in ADS, turret and vehicle zoom). Its
// result feeds the refdef, the zoom sensitivity scale and the viewmodel, so
// scaling the return value here behaves exactly like World at War's
// cg_fovScale: hip-fire and ADS both scale, sensitivity stays consistent.
//
// cg_fov itself already exists in CoD3 (default 65, clamped to [1,160] by the
// game, not cheat protected); the native options slider and the console set it.

#include <algorithm>

#include <rex/hook.h>

#include "game/addresses.h"
#include "game/engine.h"
#include "game/guest.h"
#include "game/mouse_look.h"

REX_EXTERN(__imp__CG_GetViewFov);
REX_HOOK_RAW(CG_GetViewFov) {
  __imp__CG_GetViewFov(ctx, base);

  float scale = 1.0f;
  if (uint32_t scale_cvar = cod3::Engine::Get().fov_scale_cvar()) {
    float v = cod3::guest::ReadF32(base, scale_cvar + cod3::addr::cvar::kValue);
    if (v > 0.0f) scale = std::clamp(v, 0.2f, 2.0f);  // also rejects NaN
  }
  if (scale != 1.0f) ctx.f1.f64 = std::clamp(ctx.f1.f64 * scale, 1.0, 170.0);

  // Zoom ratio for mouse sensitivity while aiming down sights (WaW style):
  // current FOV over the un-zoomed FOV.
  float cg_fov = cod3::guest::ReadF32(base, cod3::addr::kCgFovVmCvar + 8);
  cod3::MouseLook::Get().SetViewFov(static_cast<float>(ctx.f1.f64), cg_fov * scale);
}
