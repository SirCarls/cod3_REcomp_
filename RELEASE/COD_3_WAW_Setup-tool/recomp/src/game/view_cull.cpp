// cod3 - [claude-9b] the game's own view check (what it treats as on screen) made as wide as the
// picture: no pop-in at the sides with a large field of view or a wide picture (campaign).
// Owner 2026-10-21 20:57: "there is pop-in when using a large fov value or wide fov aspect ratio";
// "this could be related" to the checkpoint crash. Built 2026-10-23.
//
// Two views exist each frame:
//   - the picture: RE_RenderScene's scene setup 0x82430210 gives the renderer (ngl) the screen's
//     shape (nglSetAspect at 0x82430340: 16:9, or the wider picture through ultrawide.cpp's hook)
//     and the refdef's vertical field of view (fov_y; 0x8212AEC8). So the picture's horizontal
//     field of view is 2 atan(tan(fov_y / 2) x aspect).
//   - the game's own check: 0x82419470 builds its view volume from the refdef's fov_x and fov_y
//     as RE_RenderScene copied them (tr 0x82AAD790: +636 fov_x, +640 fov_y). Whatever it treats
//     as outside isn't drawn.
// When fov_x is smaller than the picture's horizontal field of view (a picture wider than the
// shape fov_x was made for, or a fov_x worked out for another shape), things at the picture's
// sides are left out until they come into the narrower volume: pop-in.
//
// This hook logs both once for every new combination (always) and, with cod3_cull_match_view on,
// widens fov_x to the picture's for the call only (put back after), so the volume matches what is
// drawn. Only views whose fov_x and fov_y differ are touched (the square renders keep theirs).
// Off by default until tested: the volume may also place things on screen (markers, flares).

#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/ultrawide.h"

REXCVAR_DEFINE_BOOL(cod3_cull_match_view, false, "cod3/Display",
                    "The game's own on-screen check as wide as the picture (no pop-in at the sides "
                    "with a large field of view or a wide picture)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using cod3::guest::ReadF32;
using cod3::guest::WriteF32;

constexpr uint32_t kTr = 0x82AAD790;        // the renderer's copy of the refdef (RE_RenderScene)
constexpr uint32_t kTrFovX = kTr + 636;     // float degrees
constexpr uint32_t kTrFovY = kTr + 640;     // float degrees
constexpr double kPi = 3.14159265358979323846;

double PictureFovX(double fov_y, double aspect) {
  return 2.0 * std::atan(std::tan(fov_y * kPi / 360.0) * aspect) * 180.0 / kPi;  // degrees
}

float g_logged_x = -1.0f, g_logged_y = -1.0f, g_logged_aspect = -1.0f;
bool g_logged_on = false;

}  // namespace

// 0x82419470(): the game's view volume from tr's fov_x / fov_y.
REX_EXTERN(__imp__sub_82419470);
REX_HOOK_RAW(sub_82419470) {
  const float fov_x = ReadF32(base, kTrFovX);
  const float fov_y = ReadF32(base, kTrFovY);
  const float aspect = cod3::DisplayAspect() > 0.0f ? cod3::DisplayAspect() : 16.0f / 9.0f;
  const bool screen_view = std::fabs(fov_x - fov_y) > 0.01f && fov_y > 1.0f && fov_y < 179.0f;
  const double picture_x = screen_view ? PictureFovX(fov_y, aspect) : fov_x;
  const bool on = REXCVAR_GET(cod3_cull_match_view);
  if (screen_view && (fov_x != g_logged_x || fov_y != g_logged_y || aspect != g_logged_aspect ||
                      on != g_logged_on)) {
    g_logged_x = fov_x, g_logged_y = fov_y, g_logged_aspect = aspect, g_logged_on = on;
    REXLOG_INFO("cod3: view check: the game's fov_x {:.1f}, fov_y {:.1f}; the picture ({:.3f}:1) is {:.1f} "
                "wide{}",
                fov_x, fov_y, aspect, picture_x,
                picture_x > fov_x + 0.5 ? (on ? ": widened to it (cod3_cull_match_view)"
                                              : ": things at the sides can pop in (cod3_cull_match_view off)")
                                        : ": they match");
  }
  if (!on || !screen_view || picture_x <= fov_x + 0.01) {
    __imp__sub_82419470(ctx, base);
    return;
  }
  WriteF32(base, kTrFovX, float(picture_x));
  __imp__sub_82419470(ctx, base);
  WriteF32(base, kTrFovX, fov_x);
}
