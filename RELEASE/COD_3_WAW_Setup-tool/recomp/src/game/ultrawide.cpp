// cod3 - ultrawide (Hor+) support (see ultrawide.h).

#include "game/ultrawide.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/program.h"
#include "game/guest.h"
#include "game/render_resolution.h"

REXCVAR_DEFINE_STRING(cod3_aspect_ratio, "auto", "cod3/Display",
                      "Picture shape: auto (the window's, or the screen's in fullscreen), "
                      "resolution (the shape of cod3_render_resolution's size, e.g. 2560x1080), "
                      "4:3, 16:10, 16:9 (the game's own), 21:9, 32:9 or your own W:H (e.g. 24:10 or "
                      "2.39:1) from 4:3 to 48:9. The view up and down stays the game's (Hor+): wider "
                      "than 16:9 shows more to the sides with the HUD, menus and movies 16:9 in the "
                      "middle; narrower shows less, with the HUD filling the picture as on a 4:3 TV. "
                      "Applies at once (game/ultrawide.h)");
REXCVAR_DEFINE_BOOL(cod3_ultrawide_hud, true, "cod3/Display",
                    "Advanced: keep the HUD, menus and movies 16:9 in the middle of a wider "
                    "picture (off = stretch them across it, for comparison)");
#if defined(COD3_MP)
// [claude-9b] 2026-10-27 owner 06:28 "4 player splitscreen ... FPS way consistently bad", 06:43 "the way it calculates
// what the player can see/should render in seems flawed" (FINDINGS-SPLITSCREEN-PERF G1).
REXCVAR_DEFINE_BOOL(cod3mp_split_lod, false, "cod3/Display",
                    "Split screen: models' detail chosen for each view's own size. The game picks it from the "
                    "view's field of view only, so a half or quarter view (half the lines) gets the detail of a "
                    "2x zoom and draws more than its pixels show")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif

namespace {
namespace uw = cod3::addr::ultrawide;
using cod3::guest::ReadF32;
using cod3::guest::ReadU32;
using cod3::guest::WriteF32;

// The shape the hooks use (written by the UI thread, read by the game's).
std::atomic<float> g_aspect{cod3::kSixteenByNine};
std::atomic<bool> g_hud_mapping{true};

// The 3D view and the 2D drawn around it (the game's main thread).
std::atomic<int> g_scene_depth{0};   // inside RE_RenderScene
std::atomic<int> g_scene_frames{0};  // > 0: a 3D view was drawn this frame or the last
std::atomic<bool> g_logged_target{false};

// [claude-9b] cod3mp split screen (owner 2026-10-27 05:49: "splitscreen hud drawing seems off, like it's
// not scaling correct"; dev/agents/claude-9b-think/FINDINGS-SPLITSCREEN-VIEW.md): the part of the
// 640-wide virtual screen whose 2D is being drawn. The game draws each local player's 2D (CG_Draw2D)
// in full-screen coordinates and puts a quarter view's HUD in its own half of the width itself
// (0x82448ED8: layouts 5 and 7 x * 0.5, 6 and 8 x * 0.5 + 320), so the squeeze is centered on that
// view. Everything else (menus, the divider bars, the campaign) keeps the whole width.
std::atomic<float> g_view_x0{0.0f};
std::atomic<float> g_view_x1{640.0f};

bool IsWide(float aspect) { return aspect > cod3::kSixteenByNine + cod3::kSameShape; }

// ---------------------------------------------------------------------------
// The presenter shows the game's picture at the guest display's shape (the
// SDK's VdQueryVideoMode, i.e. video_mode_width / video_mode_height) with
// black bars when the window has another shape, so a wider or narrower
// picture needs a display of that shape (cod3::ShapeRatio). The game reads
// the display only at start (it is "widescreen" and "high definition" either
// way) and the presenter at every frame, so it can change live.
//
// The port owns these two settings unless they are given on the command line
// or in the environment. A saved pair is dropped at start
// (ForgetSavedDisplayShape).
// ---------------------------------------------------------------------------
bool g_own_display_shape = true;
bool g_display_shape_set = false;
float g_presented_aspect = 0.0f;

bool SetByPlayer(const char* name) {
  const auto source = rex::cvar::GetFlagSource(name);
  return source == rex::cvar::Source::kCommandLine || source == rex::cvar::Source::kEnvironment;
}

void SetPresenterShape(float aspect) {
  if (!g_own_display_shape || !rex::cvar::GetFlagInfo("video_mode_width")) return;
  if (cod3::IsSixteenByNine(aspect)) {
    // The game's own 16:9: the SDK's defaults, exactly as before.
    if (g_display_shape_set) {
      rex::cvar::ResetToDefault("video_mode_width");
      rex::cvar::ResetToDefault("video_mode_height");
      g_display_shape_set = false;
    }
    return;
  }
  uint32_t width = 0, height = 0;
  cod3::ShapeRatio(aspect, &width, &height);
  // Height first: between two shapes other than 16:9 usually only the width
  // changes, so the presenter doesn't see half of an update.
  if (rex::cvar::GetFlagByName("video_mode_height") != std::to_string(height))
    rex::cvar::SetFlagByName("video_mode_height", std::to_string(height));
  rex::cvar::SetFlagByName("video_mode_width", std::to_string(width));
  g_display_shape_set = true;
}

// The shape of the render resolution's size, for kMatchResolution.
float ResolutionAspect(uint32_t window_width, uint32_t window_height) {
  const std::string choice = rex::cvar::GetFlagByName("cod3_render_resolution");
  uint32_t w = 0, h = 0;
  float shape = cod3::kSixteenByNine;  // the named sizes, "original" (shown as 16:9)
  if (choice.find('x') != std::string::npos && cod3::ParseResolutionChoice(choice, &w, &h))
    shape = float(w) / float(h);  // a size of the player's own
  else if (!cod3::ParseResolutionChoice(choice, &w, &h) && choice != "original" && window_width &&
           window_height)
    shape = float(window_width) / float(window_height);  // "auto": the window, pixel for pixel
  if (cod3::IsSixteenByNine(shape)) return cod3::kSixteenByNine;
  return std::clamp(shape, cod3::kMinAspect, cod3::kMaxAspect);
}

// The setting as the log says it ("resolution" names the size).
std::string ChoiceText() {
  const std::string& choice = REXCVAR_GET(cod3_aspect_ratio);
  if (choice != cod3::kMatchResolution) return choice;
  return "match the render resolution, " + rex::cvar::GetFlagByName("cod3_render_resolution");
}
}  // namespace

namespace cod3 {

bool ParseAspectText(const std::string& text, float* aspect) {
  // "W:H" (24:10, 2.39:1) or a plain number (2.39).
  if (text.empty() || text.size() > 16) return false;
  float w = 0.0f, h = 1.0f;
  char tail = 0;
  if (std::sscanf(text.c_str(), "%f:%f%c", &w, &h, &tail) != 2) {
    h = 1.0f;
    if (std::sscanf(text.c_str(), "%f%c", &w, &tail) != 1) return false;
  }
  if (!(w > 0.0f && h > 0.0f && std::isfinite(w / h))) return false;
  *aspect = w / h;
  return true;
}

bool ParseAspect(const std::string& text, float* aspect) {
  // Kept between 4:3 (the console's own view) and three 16:9 screens.
  float shape = 0.0f;
  if (!ParseAspectText(text, &shape)) return false;
  *aspect = IsSixteenByNine(shape) ? kSixteenByNine : std::clamp(shape, kMinAspect, kMaxAspect);
  return true;
}

void ShapeRatio(float aspect, uint32_t* ax, uint32_t* ay) {
  if (IsSixteenByNine(aspect)) {
    *ax = 16;
    *ay = 9;
    return;
  }
  // 1000 lines tall, or fewer above 4.095:1: video_mode_width is at most
  // 4095 (the SDK's limit), and a clamped width would squeeze the picture
  // (48:9 is 4091x767). The width is rounded up below 16:9 so that 4:3 (1334
  // wide) stays "widescreen" for the game (the SDK's width * 3 >= height * 4).
  const float shape = std::clamp(aspect, kMinAspect, kMaxAspect);
  const uint32_t lines = std::min<uint32_t>(1000, uint32_t(4095.0f / shape));
  const float width = float(lines) * shape;
  const float rounded = shape < kSixteenByNine ? std::ceil(width) : std::round(width);
  *ax = std::min<uint32_t>(4095, uint32_t(rounded));
  *ay = lines;
}

PictureRect PictureInView(uint32_t view_width, uint32_t view_height, float aspect) {
  uint32_t shape_x = 16, shape_y = 9;
  ShapeRatio(aspect, &shape_x, &shape_y);
  const uint64_t ax = shape_x, ay = shape_y;
  const uint64_t w = std::max<uint32_t>(view_width, 1), h = std::max<uint32_t>(view_height, 1);
  const bool bars = rex::cvar::GetFlagByName("present_letterbox") != "false";
  const bool overscan = rex::cvar::GetFlagByName("present_allow_overscan_cutoff") == "true";
  auto safe_area = [overscan](const char* name) -> uint64_t {
    const int percent = std::atoi(rex::cvar::GetFlagByName(name).c_str());
    return overscan && percent > 0 && percent < 100 ? uint64_t(percent) : 100;
  };
  // value * scale / old_scale, rounded to the nearest (the presenter's).
  auto rescale = [](uint64_t value, uint64_t scale, uint64_t old_scale) {
    return (value * scale + old_scale / 2) / old_scale;
  };
  PictureRect r;
  if (w * ay > h * ax) {
    // A window wider than the picture: full width, the height cropped at most
    // to the safe area, then bars at the sides or stretched across.
    const uint64_t safe = safe_area("present_safe_area_y");
    uint64_t out_h = rescale(w, ay, ax);
    bool limited = false;
    if (out_h * safe > h * 100) {
      out_h = rescale(h, 100, safe);
      limited = true;
    }
    r.width = uint32_t(limited && bars ? rescale(h * 100, ax, ay * safe) : w);
    r.height = uint32_t(out_h);
  } else {
    // Taller: full height, the width cropped at most to the safe area, then
    // bars above and below or stretched.
    const uint64_t safe = safe_area("present_safe_area_x");
    uint64_t out_w = rescale(h, ax, ay);
    bool limited = false;
    if (out_w * safe > w * 100) {
      out_w = rescale(w, 100, safe);
      limited = true;
    }
    r.width = uint32_t(out_w);
    r.height = uint32_t(limited && bars ? rescale(w * 100, ay, ax * safe) : h);
  }
  r.x = (int32_t(w) - int32_t(r.width)) / 2;
  r.y = (int32_t(h) - int32_t(r.height)) / 2;
  return r;
}

float AspectForWindow(uint32_t width, uint32_t height) {
  const std::string& choice = REXCVAR_GET(cod3_aspect_ratio);
  if (choice == "16:9") return kSixteenByNine;
  if (choice == kMatchResolution) return ResolutionAspect(width, height);
  // A listed shape (4:3, 16:10, 21:9, 32:9) or one of the player's own;
  // anything that isn't one counts as "auto".
  if (float custom = 0.0f; choice != "auto" && ParseAspect(choice, &custom)) return custom;
  // "auto": the window's shape, kept between the game's 16:9 (a narrower
  // window gets the 16:9 picture with bars, as before) and kMaxAspect.
  if (!width || !height) return kSixteenByNine;
  const float shape = float(width) / float(height);
  if (shape < kSixteenByNine + kSameShape) return kSixteenByNine;
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
                aspect, window_width, window_height, ChoiceText());
  else if (!IsSixteenByNine(aspect))
    REXLOG_INFO("cod3: picture shape {:.3f}:1 for {}x{} ({}): less to the sides than 16:9, the same "
                "view up and down; the HUD and menus fill the picture",
                aspect, window_width, window_height, ChoiceText());
  else if (!first)
    REXLOG_INFO("cod3: picture shape 16:9 (the game's own) for {}x{} ({})", window_width,
                window_height, ChoiceText());
}

float DisplayAspect() { return g_aspect.load(std::memory_order_relaxed); }

void ForgetSavedDisplayShape() {
  if (!rex::cvar::GetFlagInfo("video_mode_width")) return;
  if (SetByPlayer("video_mode_width") || SetByPlayer("video_mode_height")) {
    g_own_display_shape = false;
    REXLOG_INFO("cod3: video_mode_width/height given at start: the picture keeps their shape");
    return;
  }
  // A saved size is dropped: the port's own (its height is 1000) is made again
  // from the picture shape, and any other came from the SDK's own settings
  // window (F4) or an older build. Kept, it silently stopped the port from
  // shaping the picture, so F2's picture shape did nothing (the owner's
  // cod3.toml had 1920x1080). F2 > Video > Picture shape is the setting.
  const bool saved = rex::cvar::GetFlagSource("video_mode_width") == rex::cvar::Source::kConfig ||
                     rex::cvar::GetFlagSource("video_mode_height") == rex::cvar::Source::kConfig;
  if (!saved) return;
  const std::string w = rex::cvar::GetFlagByName("video_mode_width");
  const std::string h = rex::cvar::GetFlagByName("video_mode_height");
  rex::cvar::ResetToDefault("video_mode_width");
  rex::cvar::ResetToDefault("video_mode_height");
  // A size the port made itself (ShapeRatio) is dropped quietly.
  const uint32_t saved_w = uint32_t(std::atoi(w.c_str())), saved_h = uint32_t(std::atoi(h.c_str()));
  uint32_t port_w = 0, port_h = 0;
  if (saved_h) ShapeRatio(float(saved_w) / float(saved_h), &port_w, &port_h);
  if (!saved_h || port_w != saved_w || port_h != saved_h)
    REXLOG_INFO("cod3: video_mode_width/height {}x{} from cod3.toml not used: the picture's shape is "
                "F2 > Video > Picture shape (give them on the command line to keep a size)",
                w, h);
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

float MapX(float x, float scale) {
  const float center = 0.5f * (g_view_x0.load(std::memory_order_relaxed) + g_view_x1.load(std::memory_order_relaxed));
  return center + (x - center) * scale;
}

// An edge of the screen, or of the view whose 2D is being drawn when the quad covers that whole view
// (a quarter view's own overlays: fades, flashes). [claude-9b] 2026-10-27 Only for quads spanning the view:
// run c9b-test-ss-4p-219 showed the top-right player's compass spokes stretched out of its circle, their
// ends at x = 320 (that quarter's left edge) kept while the rest was squeezed.
bool AtEdge(float x, bool spans_view) {
  if (x <= 0.5f || x >= 639.5f) return true;
  return spans_view && (std::abs(x - g_view_x0.load(std::memory_order_relaxed)) <= 0.5f ||
                        std::abs(x - g_view_x1.load(std::memory_order_relaxed)) <= 0.5f);
}

#if defined(COD3_MP)
// [claude-9b] cod3mp addresses (MP only, so not in addresses.h's table).
constexpr uint32_t kMpActiveLocalClient = 0x82BCBE9C;
constexpr uint32_t kMpClientLayout = 0x82C7A31C;  // + client * 3208: 0 whole screen, 3/4 halves, 5-8 quarters
constexpr uint32_t kMpClientStride = 3208;
constexpr uint32_t kMpCgRefdef = 0x82C7D490 + 4928;  // + client * 6368: x, y, width, height (640x480 units)
constexpr uint32_t kMpCgStride = 6368;
uint8_t* g_mp_base = nullptr;  // the guest memory, for the 3D shape's log line (set by RE_RenderScene)
constexpr uint32_t kMpLocalPlayers = 0x82BCC878;  // local players in the match (split screen: 2-4)
// r_lodscale (default 1): RE_RenderScene's detail scale is min(r_lodscale, 4) * tan(min(fov_x, fov_y) / 2) / tan(40
// deg), smaller meaning more detail; the dvar's pointer, its value a float at +0x1C (FINDINGS-SPLITSCREEN-PERF 1.5).
constexpr uint32_t kMpLodScaleDvar = 0x82BA333C;
constexpr uint32_t kDvarValue = 0x1C;

// G1: for one RE_RenderScene call of a split-screen view (the active local client's layout: 3/4 halves, 5-8
// quarters, each with half the picture's lines), r_lodscale times 2: the same detail per pixel as one player's view.
// [claude-9b] By layout, not the refdef: bench c9b-splitab1 never engaged with a 640x480-unit height test (the
// renderer's refdef is in pixels). Returns the dvar written (0: none) and its value before.
uint32_t MpLayout(uint8_t* base, uint32_t client);  // below

uint32_t SplitLodBegin(uint8_t* base, float* saved) {
  if (!REXCVAR_GET(cod3mp_split_lod)) return 0;
  const uint32_t client = ReadU32(base, kMpActiveLocalClient);
  const uint32_t layout = MpLayout(base, client);
  static bool explained = false;
  auto explain = [&](const char* why) {
    if (explained) return;
    explained = true;
    REXLOG_INFO("cod3mp: split-screen detail: not applied ({}; client {}, layout {}, players {})", why, client, layout,
                ReadU32(base, kMpLocalPlayers));
  };
  if (layout < 3 || layout > 8) return 0;  // one player's whole picture
  const uint32_t dvar = ReadU32(base, kMpLodScaleDvar);
  if (dvar < 0x10000 || (dvar & 3)) {
    explain("no r_lodscale dvar");
    return 0;
  }
  *saved = ReadF32(base, dvar + kDvarValue);
  if (!(*saved > 0.01f && *saved < 16.0f)) {  // not the value it should be: leave it alone
    explain("r_lodscale's value isn't where it should be");
    return 0;
  }
  constexpr float kHalfTheLines = 2.0f;
  WriteF32(base, dvar + kDvarValue, *saved * kHalfTheLines);
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("cod3mp: split-screen detail: r_lodscale {:.2f} x 2 for a view of layout {}", *saved, layout);
  }
  return dvar;
}

uint32_t MpLayout(uint8_t* base, uint32_t client) {
  return client < 4 ? ReadU32(base, kMpClientLayout + client * kMpClientStride) : 0;
}

// The 3D view's shape in the log, once per player and change (a player joining or leaving, a new
// picture shape): the projection the port gives the game against the view's shape on the picture.
void LogViewShape(double game, double used) {
  if (!g_mp_base) return;
  const uint32_t client = ReadU32(g_mp_base, kMpActiveLocalClient);
  if (client >= 4) return;
  const uint32_t at = kMpCgRefdef + client * kMpCgStride;
  const int32_t w = int32_t(ReadU32(g_mp_base, at + 8)), h = int32_t(ReadU32(g_mp_base, at + 12));
  static float logged[4] = {};
  static int32_t logged_w[4] = {}, logged_h[4] = {};
  if (float(used) == logged[client] && w == logged_w[client] && h == logged_h[client]) return;
  static int lines = 0;  // the scene setup's second call (a dvar's own shape) could alternate with the first
  if (++lines > 32) return;
  logged[client] = float(used);
  logged_w[client] = w;
  logged_h[client] = h;
  const double shown = h > 0 ? double(w) / double(h) * 0.75 * g_aspect.load(std::memory_order_relaxed) : 0.0;
  REXLOG_INFO("cod3mp: player {}'s 3D view (layout {}, {}x{} of 640x480): projection shape {:.4f} (the "
              "game's {:.4f}), shape on the picture {:.4f}",
              client + 1, MpLayout(g_mp_base, client), w, h, used, game, shown);
}
#endif
}  // namespace

// ---------------------------------------------------------------------------
// 3D: mid-asm hook at 0x82430340, the `bl nglSetAspect` of the scene setup
// that RE_RenderScene calls. f1 is 16/9 (cg_widescreen), 4/3 without it, or
// 1.0 for the game's square renders; only the screen's value becomes the
// picture's shape. The vertical field of view (set right after) stays the
// game's.
// ---------------------------------------------------------------------------
//
// [claude-9b] cod3mp's half views (layouts 3 and 4: two players, or the top one of three) are twice
// as wide: the game gives 32/9 (8/3 without cg_widescreen), which becomes twice the picture's shape.
// Quarter views have the picture's shape (they get the 16/9 above). Before, a half view kept 32/9 on
// any picture: stretched on wider ones, squashed on 16:10 and 4:3 (FINDINGS-SPLITSCREEN-VIEW).
void Cod3SceneAspectHook(PPCRegister& f1) {
  const float aspect = g_aspect.load(std::memory_order_relaxed);
  [[maybe_unused]] const double game = f1.f64;
  if (!cod3::IsSixteenByNine(aspect)) {
    if (std::abs(f1.f64 - 16.0 / 9.0) < 1e-3 || std::abs(f1.f64 - 4.0 / 3.0) < 1e-3)
      f1.f64 = aspect;
    else if (cod3::kMultiplayer && (std::abs(f1.f64 - 32.0 / 9.0) < 1e-3 || std::abs(f1.f64 - 8.0 / 3.0) < 1e-3))
      f1.f64 = 2.0 * aspect;
  }
#if defined(COD3_MP)
  // Only the screen's views (not the game's square renders).
  const bool screen = std::abs(game - 16.0 / 9.0) < 1e-3 || std::abs(game - 4.0 / 3.0) < 1e-3 ||
                      std::abs(game - 32.0 / 9.0) < 1e-3 || std::abs(game - 8.0 / 3.0) < 1e-3;
  if (screen && g_scene_depth.load(std::memory_order_relaxed) > 0) LogViewShape(game, f1.f64);
#endif
}

// RE_RenderScene(refdef): the 3D view. 2D recorded inside it belongs to the
// view and keeps the full width; a view drawn this frame also tells the quads
// below that the game is being played (not a menu, movie or loading screen).
REX_EXTERN(__imp__RE_RenderScene);
REX_HOOK_RAW(RE_RenderScene) {
#if defined(COD3_MP)
  g_mp_base = base;
  float lod_saved = 0.0f;
  const uint32_t lod_dvar = SplitLodBegin(base, &lod_saved);  // [claude-9b] cod3mp_split_lod
#endif
  g_scene_depth.fetch_add(1, std::memory_order_relaxed);
  g_scene_frames.store(2, std::memory_order_relaxed);
  __imp__RE_RenderScene(ctx, base);
  g_scene_depth.fetch_sub(1, std::memory_order_relaxed);
#if defined(COD3_MP)
  if (lod_dvar) WriteF32(base, lod_dvar + kDvarValue, lod_saved);
#endif
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
  const bool movie = !cod3::kMultiplayer && quad == ReadU32(base, uw::kMovieQuadPtr);  // [mp] no films
  const bool playing = !movie && g_scene_frames.load(std::memory_order_relaxed) > 0;
  float x[4];
  for (int i = 0; i < 4; ++i) x[i] = ReadF32(base, quad + uw::kQuadVertexStride * i);
  const float lo = std::min({x[0], x[1], x[2], x[3]}), hi = std::max({x[0], x[1], x[2], x[3]});
  const bool spans_view = lo <= g_view_x0.load(std::memory_order_relaxed) + 0.5f &&
                          hi >= g_view_x1.load(std::memory_order_relaxed) - 0.5f;
  for (int i = 0; i < 4; ++i) {
    const bool edge = AtEdge(x[i], spans_view);
    if (!(playing && edge)) WriteF32(base, quad + uw::kQuadVertexStride * i, MapX(x[i], scale));
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

#if defined(COD3_MP)
// [claude-9b] CG_Draw2D (cod3mp 0x82460620; campaign 0x82446238, one view there): once per local
// player's view, with that player as the game's active local client. Its 2D is squeezed around the
// middle of its own part of the screen (a quarter view's half of the width); the whole width again
// after it. The part chosen is logged when it changes.
REX_EXTERN(__imp__sub_82460620);
REX_HOOK_RAW(sub_82460620) {
  const uint32_t client = ReadU32(base, kMpActiveLocalClient);
  const uint32_t layout = MpLayout(base, client);
  float x0 = 0.0f, x1 = 640.0f;
  if (layout == 5 || layout == 7) x1 = 320.0f;
  if (layout == 6 || layout == 8) x0 = 320.0f;
  static uint32_t logged[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  if (client < 4 && logged[client] != layout) {
    logged[client] = layout;
    REXLOG_INFO("cod3mp: player {}'s HUD in x {:.0f}-{:.0f} of 640 (layout {}){}", client + 1, x0, x1, layout,
                IsWide(g_aspect.load(std::memory_order_relaxed)) ? ", 16:9 in its middle" : "");
  }
  g_view_x0.store(x0, std::memory_order_relaxed);
  g_view_x1.store(x1, std::memory_order_relaxed);
  __imp__sub_82460620(ctx, base);
  g_view_x0.store(0.0f, std::memory_order_relaxed);
  g_view_x1.store(640.0f, std::memory_order_relaxed);
}
#endif
