// cod3 - ultrawide (Hor+) support (see ultrawide.h).

#include "game/ultrawide.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/render_resolution.h"

REXCVAR_DEFINE_STRING(cod3_aspect_ratio, "auto", "cod3/Display",
                      "Picture shape: auto (the window's, or the screen's in fullscreen), 16:9 "
                      "(the game's own), 21:9, 32:9 or your own W:H (e.g. 24:10 or 2.39:1) from "
                      "16:9 to 48:9. Wider than 16:9 shows more to the sides with the same view up "
                      "and down (Hor+); the HUD, menus and movies stay 16:9 in the middle. Applies "
                      "at once (game/ultrawide.h)");
REXCVAR_DEFINE_BOOL(cod3_ultrawide_hud, true, "cod3/Display",
                    "Advanced: keep the HUD, menus and movies 16:9 in the middle of a wider "
                    "picture (off = stretch them across it, for comparison)");

namespace {
namespace uw = cod3::addr::ultrawide;
using cod3::guest::ReadF32;
using cod3::guest::ReadU32;
using cod3::guest::WriteF32;

// Shapes within this of 16:9 count as 16:9.
constexpr float kSame = 0.002f;

// The shape the hooks use (written by the UI thread, read by the game's).
std::atomic<float> g_aspect{cod3::kSixteenByNine};
std::atomic<bool> g_hud_mapping{true};

// The 3D view and the 2D drawn around it (the game's main thread).
std::atomic<int> g_scene_depth{0};   // inside RE_RenderScene
std::atomic<int> g_scene_frames{0};  // > 0: a 3D view was drawn this frame or the last
std::atomic<bool> g_logged_target{false};

bool IsWide(float aspect) { return aspect > cod3::kSixteenByNine + kSame; }

// ---------------------------------------------------------------------------
// The presenter shows the game's picture at the guest display's shape (the
// SDK's VdQueryVideoMode, i.e. video_mode_width / video_mode_height) with
// black bars when the window has another shape, so a wider picture needs a
// display of that shape. The game reads the display only at start (it is
// "widescreen" and "high definition" either way) and the presenter at every
// frame, so it can change live.
//
// The port owns these two settings unless they are given on the command line
// or in the environment (or the config file holds the player's own values).
// The height is an unusual one so a saved value is recognizable as the port's.
// ---------------------------------------------------------------------------
constexpr int kShapeHeight = 1000;
constexpr int kMaxDisplayWidth = 4095;  // the SDK's limit for video_mode_width
bool g_own_display_shape = true;
bool g_display_shape_set = false;
float g_presented_aspect = 0.0f;

bool SetByPlayer(const char* name) {
  const auto source = rex::cvar::GetFlagSource(name);
  return source == rex::cvar::Source::kCommandLine || source == rex::cvar::Source::kEnvironment;
}

void SetPresenterShape(float aspect) {
  if (!g_own_display_shape || !rex::cvar::GetFlagInfo("video_mode_width")) return;
  if (!IsWide(aspect)) {
    // The game's own 16:9: the SDK's defaults, exactly as before.
    if (g_display_shape_set) {
      rex::cvar::ResetToDefault("video_mode_width");
      rex::cvar::ResetToDefault("video_mode_height");
      g_display_shape_set = false;
    }
    return;
  }
  const int width = std::min(kMaxDisplayWidth, int(std::lround(kShapeHeight * aspect)));
  // Height first: once the shape is wide only the width changes, so the
  // presenter never sees half of an update.
  if (rex::cvar::GetFlagByName("video_mode_height") != std::to_string(kShapeHeight))
    rex::cvar::SetFlagByName("video_mode_height", std::to_string(kShapeHeight));
  rex::cvar::SetFlagByName("video_mode_width", std::to_string(width));
  g_display_shape_set = true;
}
}  // namespace

namespace cod3 {

bool ParseAspect(const std::string& text, float* aspect) {
  // "W:H" (24:10, 2.39:1) or a plain number (2.39), kept between 16:9 and
  // kMaxAspect: the game's view is 16:9 or wider.
  if (text.empty() || text.size() > 16) return false;
  float w = 0.0f, h = 1.0f;
  char tail = 0;
  if (std::sscanf(text.c_str(), "%f:%f%c", &w, &h, &tail) != 2) {
    h = 1.0f;
    if (std::sscanf(text.c_str(), "%f%c", &w, &tail) != 1) return false;
  }
  if (!(w > 0.0f && h > 0.0f && std::isfinite(w / h))) return false;
  *aspect = std::clamp(w / h, kSixteenByNine, kMaxAspect);
  return true;
}

float AspectForWindow(uint32_t width, uint32_t height) {
  const std::string& choice = REXCVAR_GET(cod3_aspect_ratio);
  if (choice == "16:9") return kSixteenByNine;
  if (choice == "21:9") return 21.0f / 9.0f;
  if (choice == "32:9") return 32.0f / 9.0f;
  // A shape of the player's own; anything that isn't one counts as "auto".
  if (float custom = 0.0f; choice != "auto" && ParseAspect(choice, &custom)) return custom;
  // "auto": the window's shape, kept between the game's 16:9 (a narrower
  // window gets the 16:9 picture with bars, as before) and kMaxAspect.
  if (!width || !height) return kSixteenByNine;
  const float shape = float(width) / float(height);
  if (shape < kSixteenByNine + kSame) return kSixteenByNine;
  return std::min(shape, kMaxAspect);
}

void UpdateDisplayAspect(uint32_t window_width, uint32_t window_height) {
  if (!window_width || !window_height) return;  // minimized
  // The game's own resolution follows a new size or shape a moment later
  // (a live resolution change, game/render_resolution.h).
  NoteWindowSize(window_width, window_height);
  g_hud_mapping.store(REXCVAR_GET(cod3_ultrawide_hud), std::memory_order_relaxed);
  const float aspect = AspectForWindow(window_width, window_height);
  if (aspect == g_presented_aspect) return;
  const bool first = g_presented_aspect == 0.0f;
  g_presented_aspect = aspect;
  g_aspect.store(aspect, std::memory_order_relaxed);
  SetPresenterShape(aspect);
  if (IsWide(aspect))
    REXLOG_INFO("cod3: picture shape {:.3f}:1 for {}x{} ({}): more to the sides, the HUD and menus "
                "16:9 in the middle",
                aspect, window_width, window_height, REXCVAR_GET(cod3_aspect_ratio));
  else if (!first)
    REXLOG_INFO("cod3: picture shape 16:9 (the game's own) for {}x{} ({})", window_width,
                window_height, REXCVAR_GET(cod3_aspect_ratio));
}

float DisplayAspect() { return g_aspect.load(std::memory_order_relaxed); }

void ForgetSavedDisplayShape() {
  if (!rex::cvar::GetFlagInfo("video_mode_width")) return;
  if (SetByPlayer("video_mode_width") || SetByPlayer("video_mode_height")) {
    g_own_display_shape = false;
    REXLOG_INFO("cod3: video_mode_width/height given at start: the picture keeps their shape");
    return;
  }
  // A saved size with the port's height is the port's own: drop it. Any other
  // value in the config file is the player's and stays.
  const auto height_source = rex::cvar::GetFlagSource("video_mode_height");
  if (height_source == rex::cvar::Source::kConfig) {
    if (rex::cvar::GetFlagByName("video_mode_height") == std::to_string(kShapeHeight)) {
      rex::cvar::ResetToDefault("video_mode_width");
      rex::cvar::ResetToDefault("video_mode_height");
    } else {
      g_own_display_shape = false;
    }
  } else if (rex::cvar::GetFlagSource("video_mode_width") == rex::cvar::Source::kConfig) {
    g_own_display_shape = false;
  }
}

}  // namespace cod3

namespace {
// The list being recorded draws into the back buffer, whose 2D coordinates
// are the 640x480 virtual screen (not into an off-screen texture).
bool RecordingToBackBuffer(uint8_t* base) {
  const uint32_t list = ReadU32(base, uw::kCurrentList);
  if (!list) return false;
  const uint32_t target = ReadU32(base, list + uw::kListTarget);
  const bool virtual_screen = target && (ReadU32(base, target + uw::kTargetFlags) & uw::kTargetVirtual);
  if (!g_logged_target.exchange(true))
    REXLOG_INFO("cod3: ultrawide HUD: first 2D list {:08X} draws into {:08X} (flags {:08X}){}", list,
                target, target ? ReadU32(base, target + uw::kTargetFlags) : 0,
                virtual_screen ? "" : ": not the back buffer, left alone");
  return virtual_screen;
}

// The factor that squeezes the 640-wide virtual screen into the picture's
// centered 16:9 part, or 0 when 2D is drawn as the game draws it.
float HudScale(uint8_t* base) {
  const float aspect = g_aspect.load(std::memory_order_relaxed);
  if (!IsWide(aspect) || !g_hud_mapping.load(std::memory_order_relaxed)) return 0.0f;
  // The 3D view's own 2D (lens flares, full-screen effects) covers the view.
  if (g_scene_depth.load(std::memory_order_relaxed) > 0) return 0.0f;
  if (!RecordingToBackBuffer(base)) return 0.0f;
  return cod3::kSixteenByNine / aspect;
}

float MapX(float x, float scale) { return 320.0f + (x - 320.0f) * scale; }
}  // namespace

// ---------------------------------------------------------------------------
// 3D: mid-asm hook at 0x82430340, the `bl nglSetAspect` of the scene setup
// that RE_RenderScene calls. f1 is 16/9 (cg_widescreen), or 1.0 for the
// game's square renders; only the 16:9 value becomes the picture's shape.
// The vertical field of view (set right after) stays the game's.
// ---------------------------------------------------------------------------
void Cod3SceneAspectHook(PPCRegister& f1) {
  const float aspect = g_aspect.load(std::memory_order_relaxed);
  if (IsWide(aspect) && std::abs(f1.f64 - 16.0 / 9.0) < 1e-3) f1.f64 = aspect;
}

// RE_RenderScene(refdef): the 3D view. 2D recorded inside it belongs to the
// view and keeps the full width; a view drawn this frame also tells the quads
// below that the game is being played (not a menu, movie or loading screen).
REX_EXTERN(__imp__RE_RenderScene);
REX_HOOK_RAW(RE_RenderScene) {
  g_scene_depth.fetch_add(1, std::memory_order_relaxed);
  g_scene_frames.store(2, std::memory_order_relaxed);
  __imp__RE_RenderScene(ctx, base);
  g_scene_depth.fetch_sub(1, std::memory_order_relaxed);
}

REX_EXTERN(__imp__RE_BeginFrame);
REX_HOOK_RAW(RE_BeginFrame) {
  if (g_scene_frames.load(std::memory_order_relaxed) > 0)
    g_scene_frames.fetch_sub(1, std::memory_order_relaxed);
  __imp__RE_BeginFrame(ctx, base);
}

// nglListAddQuad(quad): every 2D picture (RE_StretchPic, the menus' panels,
// the movie) is a quad in virtual coordinates that the list copies. The quad
// can be a struct the game keeps between frames (the movie, menu panels), so
// it is put back as it was after the copy.
REX_EXTERN(__imp__nglListAddQuad);
REX_HOOK_RAW(nglListAddQuad) {
  const uint32_t quad = ctx.r3.u32;
  const float scale = quad ? HudScale(base) : 0.0f;
  if (scale <= 0.0f) {
    __imp__nglListAddQuad(ctx, base);
    return;
  }
  // During play, an edge of the screen stays the picture's edge, so fades,
  // flashes, overlays and cutscene bars cover the whole picture. The movie,
  // menus and loading screens are 16:9 pictures and get bars instead.
  const bool movie = quad == ReadU32(base, uw::kMovieQuadPtr);
  const bool playing = !movie && g_scene_frames.load(std::memory_order_relaxed) > 0;
  float x[4];
  for (int i = 0; i < 4; ++i) {
    const uint32_t at = quad + uw::kQuadVertexStride * i;
    x[i] = ReadF32(base, at);
    const bool edge = x[i] <= 0.5f || x[i] >= 639.5f;
    if (!(playing && edge)) WriteF32(base, at, MapX(x[i], scale));
  }
  __imp__nglListAddQuad(ctx, base);
  for (int i = 0; i < 4; ++i) WriteF32(base, quad + uw::kQuadVertexStride * i, x[i]);
}

// nglListAddString(font, text, f1 x, f2 y, f3 depth, f4 x scale, f5 y scale,
// color): text, laid out from x with glyphs f4 wide. The position and the
// width shrink together, so the text keeps its shape in the middle.
REX_EXTERN(__imp__nglListAddString);
REX_HOOK_RAW(nglListAddString) {
  if (const float scale = HudScale(base); scale > 0.0f) {
    ctx.f1.f64 = MapX(float(ctx.f1.f64), scale);
    ctx.f4.f64 *= scale;
  }
  __imp__nglListAddString(ctx, base);
}
