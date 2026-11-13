// cod3 - ultrawide (Hor+) support (see ultrawide.h).

#include "game/ultrawide.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/font_fill.h"  // [acct2-ocr]
#include "ui/text_overlay.h"  // [acct2-ocr]
#include "game/color_text.h"  // [acct2-ocr]
#include "ui/text_native.h"  // [acct2-ocr]
#include "game/text_language.h"  // [acct2-ocr] RawTextFor
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
#if !defined(COD3_MP)
// [claude-9b-think] 2026-10-27 UNTESTED owner 13:19: "add a toggle in the f2 menu for the in-game cinematic
// letterboxing", 13:18: "the cinematic letterboxing in-game seems like it's missing a pixel on the left and right of the
// screen". The in-mission cinematics' bars come from the level scripts (each level's _letterbox module, _letterbox.bro:
// letter_box, take_weapons_away, the flag letterbox_enabled), drawn as 2D quads through nglListAddQuad like the HUD. Here
// a quad that is an opaque black bar across the whole width, touching the top or the bottom and at most a third of the
// height, while the 3D view is drawn, counts as a cinematic bar (CinematicBar below). claude-9b-test's bisect: the bars
// stop one pixel short of the right edge at every size (x = W-1 shows the scene; 2 px with half_pixel_offset off), in
// the game's own screenshots too, so the bars' quads end just short of the width; the pause menu's full-screen dim
// doesn't. Measured in its frames (c9b-test-lb-*): at 1280x720 native the bars are rows 0-89 and 630-719 (60 of the
// 480 units each) and only column 1279 shows the scene; at 1920x1013 the last row 1012 does too (half_pixel_offset on;
// off: the bottom is covered and the right gap is 2 px), so the right edge sits about half a unit short of 640 (a
// quad ending at 639.5 gives exactly 1 px with the offset and 2 without at 1920) and the bottom one short of the
// target's last row at some heights. The Xbox 360 draws with the same pixel centers the offset emulates, so it most
// likely showed the same column at 720p (a TV's overscan hides it). A bar's edges near the space's edges are now taken
// to the edges themselves (x 0 and the width, y 0 and the height). Campaign only: multiplayer has no cinematics.
REXCVAR_DEFINE_BOOL(cod3_letterbox, true, "cod3/Game",
                    "The black bars above and below the in-game cinematics (scripted scenes inside a mission). Off: "
                    "no bars, the whole picture. The game's own skip and subtitles are not changed")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_letterbox, false, "cod3",
                    "Log the 2D quads that span the screen's width and touch its top or bottom (the cinematic bars "
                    "and anything like them): their corners, colors and the rest of the quad, each distinct one once")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif
// [acct2-ocr] 2026-11-07 owner 15:32 "there might be other effects that have this. Please check" (after bug 16, the
// cinematic bars' last column and row, fixed in the bars' snap below). The chats' 8,885 run screenshots measured at
// their edges (dev/agents/acct2-ocr/tools/seam_scan.py, numbers only): the game's full-screen fades and overlays leave
// the picture's last column, and often its last row, uncovered as well, in native mode too. Hostage with today's build
// (run acct2-ocr-swapF-cycle): the last column 18-27 brighter than the one next to it in 56-64% of the rows; during a
// fade to black (tag2-fr-false, acct2-bro-side142-hostage) 63-69 brighter in 98-100% of the rows, the last row 47.
// They are quads to x 639.5 / y 479.5 like the bars were (claude-9b-test's finding above), and with the game's own 2x
// MSAA even 640 / 480 is short (bug 16). Both programs (split-screen views' outer edges too).
REXCVAR_DEFINE_INT32(cod3_edge_fill, 1, "cod3/Display",
                     "2D drawn across the whole picture (fades, flashes, overlays, menu backgrounds, the movie) reaches "
                     "its last column and row: a quad that spans the screen's (or a split-screen view's) width or height "
                     "and ends on an outer edge is drawn 2 units past it. 0: as the game draws them (the last column and "
                     "row can show what is under them); 1: the screen's 2D; 2: only 2D drawn into the game's own "
                     "off-screen pictures (in their pixels); 3: both")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_edge_fill, false, "cod3",
                    "Log each kind of 2D quad that spans the screen's width or height and ends on an edge, once (at "
                    "most 60): its space, corners, colors, texture corners and whether cod3_edge_fill moved it; and "
                    "every 5 s how many were moved")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#if defined(COD3_MP)
// [claude-9b] 2026-10-27 owner 06:28 "4 player splitscreen ... FPS way consistently bad", 06:43 "the way it calculates
// what the player can see/should render in seems flawed" (FINDINGS-SPLITSCREEN-PERF G1).
REXCVAR_DEFINE_BOOL(cod3mp_split_lod, false, "cod3/Display",
                    "Split screen: models' detail chosen for each view's own size. The game picks it from the "
                    "view's field of view only, so a half or quarter view (half the lines) gets the detail of a "
                    "2x zoom and draws more than its pixels show")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [claude-9b-think] 2026-10-27 UNTESTED (claude-9b-test s147: shadows are ~3% of 4P draws; the draws are in the
// views' own lists; FINDINGS-ANIMSPEED: grass costs a draw per visible patch and its cull ~6% of the main thread in 4P).
REXCVAR_DEFINE_INT32(cod3mp_split_grass, 100, "cod3/Display",
                     "Split screen: how far grass is drawn, in percent of the level's own distance (100: as the game; "
                     "60: grass ends at 60% of it, fading out as usual)")
    .range(30, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3mp_debug_split_grass, false, "cod3",
                    "Log once a second the grass patches each split-screen view drew and the distance used");
// [claude-9b] 2026-10-27 owner 07:14: "the safe area is affecting where various hud elements are drawn" (see the two
// hooks at the end of this file).
REXCVAR_DEFINE_BOOL(cod3mp_split_hud_tv_insets, false, "cod3/Display",
                    "Split screen: place each view's HUD as the console did, pulled toward the middle of the TV "
                    "(the left views' HUD further right, the top views' lower). Off: every view's HUD sits the same "
                    "way in its own view, with the same margins on both sides. Takes effect the next time the screen "
                    "splits")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [claude-9b] 2026-10-27 owner 07:58: "The hud still seems squished together. Perhaps we can have the user define
// where the safe area is and have each splitscreen inherit that."
REXCVAR_DEFINE_INT32(cod3_hud_safe_area, 0, "cod3/Display",
                     "Split screen: the HUD's safe area in percent, how much of each player's view the HUD "
                     "(compass, ammo, score, timer) spreads over, the same share in every view. 0 = the game's "
                     "one-player layout on the whole screen (about 84 percent); 90 = 5 percent in from each edge; "
                     "at most 93 (more would push the compass off the view; two players' halves at most 91 up and "
                     "down). One player's screen and the campaign keep the game's layout for now. Takes effect the "
                     "next time the screen splits")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [claude-9b-think] 2026-10-27 coordinator: name the split HUD's elements and their places in the log (tests).
REXCVAR_DEFINE_BOOL(cod3_debug_hud_layout, false, "cod3",
                    "Split screen: log where the HUD's elements and the players' own panels are placed when the "
                    "screen splits or a player joins or leaves (each distinct place once; tests)")
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
void RefreshSplitHudSettings(uint8_t* base);          // [claude-9b-think] below

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
#if defined(COD3_MP)
  RefreshSplitHudSettings(base);  // [claude-9b-think] the split HUD's settings, taken while every view is whole
#endif
  __imp__RE_BeginFrame(ctx, base);
}

#if !defined(COD3_MP)
namespace {
// [claude-9b-think] 2026-10-27 UNTESTED cod3_letterbox (see the setting). The size of the space the list's 2D is in:
// the 640x480 virtual screen, or a target's own pixels (0x8212BC10 reads them at the target record's +0 / +4).
bool QuadSpace(uint8_t* base, float* w, float* h) {
  const uint32_t list = ReadU32(base, uw::kCurrentList);
  const uint32_t target = list ? ReadU32(base, list + uw::kListTarget) : 0;
  if (!target) return false;
  if (ReadU32(base, target + uw::kTargetFlags) & uw::kTargetVirtual) {
    *w = 640.0f;
    *h = 480.0f;
  } else {
    *w = float(ReadU32(base, target + 0));
    *h = float(ReadU32(base, target + 4));
  }
  return *w >= 1.0f && *h >= 1.0f;
}

// Opaque black in either byte order of the vertex color (ARGB or RGBA), with some alpha.
bool OpaqueBlack(uint32_t c) {
  return ((c & 0x00FFFFFFu) == 0 && (c >> 24) >= 0x80) || ((c & 0xFFFFFF00u) == 0 && (c & 0xFFu) >= 0x80);
}

// [claude-9b-think] 2026-10-28 v3 UNTESTED (claude-9b-test ob / ob3, saint_lo: the bands stayed with cod3_letterbox off;
// v2 left out nothing). The ob3 frames (1920x1013) have black bands of rows 0-125 and 886-1012: 12.5% each, i.e. 60 of
// the 480 units, uncropped picture between them. The quads the ob run logged are exactly that size: 60 units tall,
// across the width (x -0.5..639.5 or ..644.5), sliding from the top and bottom edges, WHITE vertex colors (the dark is
// their texture). v1 looked for cg_letterbox (no reader draws bars: CG_CalcVrect's one is never set here); v2 also took
// any quad while the dvar letterbox_enabled was set, read through the pointer 0x82AB96E4 that only the vehicle camera
// code (0x82447D48) fills, so it was 0 and nothing matched. The dvar's other readers (CG_Draw2D's 0x8243D848, the
// console text widget 0x824E85A8, melee 0x8250D898, a use action 0x82496AA8) and the map_restart resets change no view
// rect, scissor, fov or post setting; the level script sets it with its setcvar (Cvar_Get + Cvar_Set2, force).
// Now: the dvar is found by its name the game's own way (Cvar_Get 0x8252BBE0 with the game's strings, as 0x8243D848
// does every frame), and a bar is also a quad of the bars' own size (55-65 units tall at an edge, the full width) of any
// color. The path that matched is logged once a second.
constexpr uint32_t kCvarGet = 0x8252BBE0;              // (name, default, flags) -> cvar_t*
constexpr uint32_t kLetterboxName = 0x820694E0;         // "letterbox_enabled"
constexpr uint32_t kZeroText = 0x82065B94;              // "0"
std::atomic<uint32_t> g_letterbox_dvar{0};
std::atomic<uint32_t> g_bars_by_dvar{0}, g_bars_by_black{0}, g_bars_by_size{0};
// The dvar's value (+32), looked up once per run (cvar_t*s don't move); false before the level registers it.
bool GameLetterbox(PPCContext& ctx, uint8_t* base) {
  uint32_t dvar = g_letterbox_dvar.load(std::memory_order_relaxed);
  if (!dvar) {
    cod3::guest::Call c(ctx, base);
    dvar = uint32_t(c.Invoke(kCvarGet, {kLetterboxName, kZeroText, 0}));
    if (dvar < 0x10000 || (dvar & 3) != 0) return false;
    g_letterbox_dvar.store(dvar, std::memory_order_relaxed);
  }
  return ReadU32(base, dvar + 32) != 0;
}

// [claude-9b-think] 2026-10-28 v4 UNTESTED (the owner's logs, 08:06:53-54 and 08:40:56-57: up to 288 "bar" quads a
// second left out while the pause menu was open). The game's own "a menu is open" test: the in-game menu system's draw
// (InGameMenuSystem vfunc_14, 0x824CD9E0) draws nothing while its current page (+12, s32) is below 0, else that page.
// Its one instance is at g_FEManager + 32 (0x824DCDxx: operator new(52), the ctor, stored there). It holds the pause
// menu, its options, mission success / failure and the objectives. While one of them is open no quad is a bar: the
// menu's panels are never left out or moved (a cinematic's bars show behind a pause menu opened during it).
constexpr uint32_t kInGameMenuSystemPtr = 0x82A8DF2C + 32;  // InGameMenuSystem*
constexpr uint32_t kMenuSystemCurrent = 12;                 // s32: the open page, < 0 = none
std::atomic<uint32_t> g_bars_menu_kept{0};

bool InGameMenuOpen(uint8_t* base) {
  const uint32_t system = ReadU32(base, kInGameMenuSystemPtr);
  return system >= 0x10000 && (system & 3) == 0 && static_cast<int32_t>(ReadU32(base, system + kMenuSystemCurrent)) >= 0;
}

// Once a second at most, while bars would be left out or with cod3_debug_letterbox: the bar-shaped quads kept.
void NoteMenuKept() {
  g_bars_menu_kept.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(cod3_letterbox) && !REXCVAR_GET(cod3_debug_letterbox)) return;
  static int64_t last = 0;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now - last < 1000) return;
  last = now;
  REXLOG_INFO("cod3: letterbox: {} bar-shaped quads kept in the last second: a menu is open (the game's own test)",
              g_bars_menu_kept.exchange(0, std::memory_order_relaxed));
}

// A cinematic bar: during play (a 3D view drawn this frame or the last), not the movie, an opaque black quad that
// reaches within 2 units of both side edges of its space and touches its top or bottom, at most a third of its height.
// [claude-9b-think] v3: or any color while letterbox_enabled is set, or the bars' own 60-unit size.
bool CinematicBar(PPCContext& ctx, uint8_t* base, uint32_t quad, float w, float h, bool log) {
  if (quad == ReadU32(base, uw::kMovieQuadPtr) || g_scene_frames.load(std::memory_order_relaxed) <= 0) return false;
  float x[4], y[4];
  uint32_t color[4];
  for (int i = 0; i < 4; ++i) {
    x[i] = ReadF32(base, quad + uw::kQuadVertexStride * i);
    y[i] = ReadF32(base, quad + uw::kQuadVertexStride * i + 4);
    color[i] = ReadU32(base, quad + uw::kQuadVertexStride * i + 16);
  }
  const float x0 = std::min({x[0], x[1], x[2], x[3]}), x1 = std::max({x[0], x[1], x[2], x[3]});
  const float y0 = std::min({y[0], y[1], y[2], y[3]}), y1 = std::max({y[0], y[1], y[2], y[3]});
  const float slack = w > 640.5f ? 2.0f : 2.0f * w / 640.0f;  // 2 units of the virtual screen, or 2 pixels
  const bool across = x0 <= slack && x1 >= w - slack;
  const bool at_edge = y0 <= slack || y1 >= h - slack;
  const bool thin = (y1 - y0) <= h / 3.0f && (y1 - y0) > 0.0f;
  if (!(across && at_edge && thin)) return false;
  if (InGameMenuOpen(base)) {  // [claude-9b-think] v4: never a menu's panel
    NoteMenuKept();
    return false;
  }
  const bool black = OpaqueBlack(color[0]) && OpaqueBlack(color[1]) && OpaqueBlack(color[2]) && OpaqueBlack(color[3]);
  if (log) {
    // Each distinct one once (rounded to a unit), at most 40 lines.
    static std::mutex mutex;
    static std::unordered_set<uint64_t> seen;
    const uint64_t key = (uint64_t(uint32_t(x0 * 4.0f) & 0xFFFF) << 48) ^ (uint64_t(uint32_t(x1 * 4.0f) & 0xFFFF) << 32) ^
                         (uint64_t(uint32_t(y0 * 4.0f) & 0xFFFF) << 16) ^ uint64_t(uint32_t(y1 * 4.0f) & 0xFFFF) ^
                         (uint64_t(color[0]) << 8);
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() < 40 && seen.insert(key).second) {
      REXLOG_INFO("cod3: letterbox candidate in {:.0f}x{:.0f}: x {:.3f}..{:.3f} y {:.3f}..{:.3f} colors {:08X} {:08X} "
                  "{:08X} {:08X} black {} rest {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  w, h, x0, x1, y0, y1, color[0], color[1], color[2], color[3], black ? 1 : 0, ReadU32(base, quad + 80),
                  ReadU32(base, quad + 84), ReadU32(base, quad + 88), ReadU32(base, quad + 92), ReadU32(base, quad + 96),
                  ReadU32(base, quad + 100));
    }
  }
  // [claude-9b-think] v3: which test matched, for the once-a-second line (NoteBarLeftOut).
  if (black) {
    g_bars_by_black.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  const float tall = (y1 - y0) * 480.0f / h;  // in the virtual screen's units
  if (tall >= 55.0f && tall <= 65.0f) {
    g_bars_by_size.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  if (GameLetterbox(ctx, base)) {
    g_bars_by_dvar.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

// [claude-9b-think] 2026-10-28 v3 UNTESTED once a second while bars are left out (cod3_letterbox off): how many, by
// which test, and the game's letterbox_enabled.
void NoteBarLeftOut(PPCContext& ctx, uint8_t* base) {
  static int64_t last = 0;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now - last < 1000) return;
  last = now;
  REXLOG_INFO("cod3: letterbox: cinematic bar quads left out in the last second: {} by size, {} black, {} by "
              "letterbox_enabled (now {}); cod3_letterbox off",
              g_bars_by_size.exchange(0, std::memory_order_relaxed),
              g_bars_by_black.exchange(0, std::memory_order_relaxed),
              g_bars_by_dvar.exchange(0, std::memory_order_relaxed), GameLetterbox(ctx, base) ? 1 : 0);
}
}  // namespace
#endif

// [acct2-ocr] 2026-11-07 cod3_edge_fill (see the setting).
namespace {
// The space a quad is in: the 640x480 virtual screen (the back buffer) or a target's own pixels (as QuadSpace).
bool EdgeSpace(uint8_t* base, float* w, float* h, bool* screen) {
  const uint32_t list = ReadU32(base, uw::kCurrentList);
  const uint32_t target = list ? ReadU32(base, list + uw::kListTarget) : 0;
  if (!target) return false;
  *screen = (ReadU32(base, target + uw::kTargetFlags) & uw::kTargetVirtual) != 0;
  if (*screen) {
    *w = 640.0f;
    *h = 480.0f;
  } else {
    *w = float(ReadU32(base, target + 0));
    *h = float(ReadU32(base, target + 4));
  }
  return *w >= 1.0f && *h >= 1.0f && *w <= 16384.0f && *h <= 16384.0f;
}

std::atomic<uint32_t> g_edge_moved_screen{0}, g_edge_moved_pixels{0};

// A quad's corners before cod3_edge_fill moved them, put back after the list's copy (the game keeps some quads
// between frames: the movie, menu panels).
struct EdgeFill {
  bool moved = false;
  float x[4] = {}, y[4] = {};
  void PutBack(uint8_t* base, uint32_t quad) const {
    if (!moved) return;
    for (int i = 0; i < 4; ++i) {
      WriteF32(base, quad + uw::kQuadVertexStride * i, x[i]);
      WriteF32(base, quad + uw::kQuadVertexStride * i + 4, y[i]);
    }
  }
};

void NoteEdgeFill() {
  static int64_t last = 0;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (now - last < 5000) return;
  last = now;
  REXLOG_INFO("cod3: edge fill: {} screen quads and {} off-screen quads drawn past the edges in the last 5 s",
              g_edge_moved_screen.exchange(0, std::memory_order_relaxed),
              g_edge_moved_pixels.exchange(0, std::memory_order_relaxed));
}

// mapped: the ultrawide HUD mapping moves this quad's x afterwards (HudScale > 0). Then only a quad drawn during play
// keeps its sides on the screen's (AtEdge), so only those sides go past; a menu's or the movie's are inside the picture.
// Corners only ever go outwards (a quad already past an edge, like the top cinematic bar's 644.5, stays).
void EdgeFillApply(uint8_t* base, uint32_t quad, bool mapped, EdgeFill* fill) {
  const int mode = REXCVAR_GET(cod3_edge_fill);
  const bool debug = REXCVAR_GET(cod3_debug_edge_fill);
  if (mode == 0 && !debug) return;
  float w = 0.0f, h = 0.0f;
  bool screen = false;
  if (!EdgeSpace(base, &w, &h, &screen)) return;
  float x[4], y[4];
  for (int i = 0; i < 4; ++i) {
    x[i] = ReadF32(base, quad + uw::kQuadVertexStride * i);
    y[i] = ReadF32(base, quad + uw::kQuadVertexStride * i + 4);
  }
  const float x0 = std::min({x[0], x[1], x[2], x[3]}), x1 = std::max({x[0], x[1], x[2], x[3]});
  const float y0 = std::min({y[0], y[1], y[2], y[3]}), y1 = std::max({y[0], y[1], y[2], y[3]});
  if (!(std::isfinite(x0) && std::isfinite(x1) && std::isfinite(y0) && std::isfinite(y1))) return;
  constexpr float kNear = 0.5f;  // as AtEdge: the game's own quads end at -0.5 and 639.5
  constexpr float kPast = 2.0f;  // as the bars: 2 units (or pixels) past the edge; the target clips the rest
  const float vx0 = screen ? g_view_x0.load(std::memory_order_relaxed) : 0.0f;
  const float vx1 = screen ? g_view_x1.load(std::memory_order_relaxed) : w;
  const bool across = (x0 <= kNear && x1 >= w - kNear) || (x0 <= vx0 + kNear && x1 >= vx1 - kNear);
  const bool down = y0 <= kNear && y1 >= h - kNear;
  const bool at_edge = x0 <= kNear || x1 >= w - kNear || y0 <= kNear || y1 >= h - kNear;
  if (!(across || down) || !at_edge) return;
  const bool movie = !cod3::kMultiplayer && quad == ReadU32(base, uw::kMovieQuadPtr);
  const bool playing = !movie && g_scene_frames.load(std::memory_order_relaxed) > 0;
  const bool sides = !(mapped && !playing);
  const bool on = (mode & (screen ? 1 : 2)) != 0;
  bool moved = false;
  if (on) {
    for (int i = 0; i < 4; ++i) {
      float nx = x[i], ny = y[i];
      if (sides) {
        if (x[i] <= kNear) nx = std::min(x[i], -kPast);
        else if (x[i] >= w - kNear) nx = std::max(x[i], w + kPast);
      }
      if (y[i] <= kNear) ny = std::min(y[i], -kPast);
      else if (y[i] >= h - kNear) ny = std::max(y[i], h + kPast);
      if (nx != x[i] || ny != y[i]) {
        if (!moved) {
          for (int k = 0; k < 4; ++k) {
            fill->x[k] = x[k];
            fill->y[k] = y[k];
          }
        }
        moved = true;
        WriteF32(base, quad + uw::kQuadVertexStride * i, nx);
        WriteF32(base, quad + uw::kQuadVertexStride * i + 4, ny);
      }
    }
    fill->moved = moved;
    if (moved) (screen ? g_edge_moved_screen : g_edge_moved_pixels).fetch_add(1, std::memory_order_relaxed);
  }
  if (!debug) return;
  NoteEdgeFill();
  // Each distinct one once (rounded to a quarter unit), at most 60 lines.
  uint32_t color[4];
  float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
  for (int i = 0; i < 4; ++i) {
    const uint32_t at = quad + uw::kQuadVertexStride * i;
    color[i] = ReadU32(base, at + 16);
    const float u = ReadF32(base, at + 8), v = ReadF32(base, at + 12);
    u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
  }
  // The color's alpha left out of the key: a fade is one quad whose alpha changes every frame (run
  // acct2-ocr-edge-host-off: Hostage's opening fade filled all 60 lines).
  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  const auto q = [](float f) { return uint64_t(uint32_t(int32_t(f * 4.0f)) & 0xFFFF); };
  const uint64_t rgb = ((color[0] >> 24) == 0xFF || (color[0] >> 24) == 0 ? color[0] : color[0] & 0x00FFFFFFu);
  const uint64_t key = (q(x0) << 48) ^ (q(x1) << 32) ^ (q(y0) << 16) ^ q(y1) ^ (rgb << 20) ^ (uint64_t(q(w)) << 8) ^
                       (playing ? 1u : 0u) ^ (movie ? 2u : 0u) ^ (g_scene_depth.load(std::memory_order_relaxed) > 0 ? 4u : 0u);
  std::lock_guard<std::mutex> lock(mutex);
  if (seen.size() >= 60 || !seen.insert(key).second) return;
  REXLOG_INFO("cod3: edge fill: {} quad x {:.2f}..{:.2f} y {:.2f}..{:.2f} colors {:08X} {:08X} {:08X} {:08X} "
              "u {:.3f}..{:.3f} v {:.3f}..{:.3f}; playing {} in the 3D view {} movie {} mapped {}: {}",
              screen ? std::string("screen") : std::to_string(int(w)) + "x" + std::to_string(int(h)), x0, x1, y0,
              y1, color[0], color[1], color[2], color[3], u0, u1, v0, v1, playing ? 1 : 0,
              g_scene_depth.load(std::memory_order_relaxed) > 0 ? 1 : 0, movie ? 1 : 0, mapped ? 1 : 0,
              moved ? "moved past the edges" : (on ? "already past" : "left (cod3_edge_fill)"));
}
}  // namespace

// nglListAddQuad(quad): every 2D picture (RE_StretchPic, the menus' panels,
// the movie) is a quad in virtual coordinates that the list copies. The quad
// can be a struct the game keeps between frames (the movie, menu panels), so
// it is put back as it was after the copy.
REX_EXTERN(__imp__nglListAddQuad);
REX_HOOK_RAW(nglListAddQuad) {
  const uint32_t quad = ctx.r3.u32;
#if !defined(COD3_MP)
  // [claude-9b-think] 2026-10-27 UNTESTED a cinematic bar: left out with cod3_letterbox off, else taken to the
  // space's edges (its corners put back after the copy, with the mapping's below).
  float bar_x[4], bar_y[4];
  bool bar = false;
  if (quad) {
    float w = 0.0f, h = 0.0f;
    if (QuadSpace(base, &w, &h) && CinematicBar(ctx, base, quad, w, h, REXCVAR_GET(cod3_debug_letterbox))) {
      if (!REXCVAR_GET(cod3_letterbox)) {  // no bar
        NoteBarLeftOut(ctx, base);  // [claude-9b-think] v3
        return;
      }
      bar = true;
      const float slack_x = w > 640.5f ? 2.0f : 2.0f * w / 640.0f;
      const float slack_y = w > 640.5f ? 2.0f : 2.0f * h / 480.0f;
      for (int i = 0; i < 4; ++i) {
        const uint32_t at = quad + uw::kQuadVertexStride * i;
        bar_x[i] = ReadF32(base, at);
        bar_y[i] = ReadF32(base, at + 4);
        // [acct2-ocr] 2026-11-07 past the edges by the slack, not onto them (bug 16, owner 15:09: "the cinematic
        // letterboxing Needs a pass. It's missing a pixel on the bottom right and top right"). With the game's own 2x
        // MSAA (cod3_render_method scaled, or a render size below the window's) a bar ending exactly on the space's
        // right or bottom edge left the picture's last column and last row uncovered: runs acct2-ocr-lb6-scaled (and
        // the owner's two screenshots of it) and -lb7, the top bar's last column 26-60 against 0, the last row 31-39.
        // Native (no MSAA) at 1280x720, 1920x1013, 1600x900 and 150% had none (lb1-lb5). The target clips the rest.
        if (bar_x[i] <= slack_x) WriteF32(base, at, -slack_x);
        else if (bar_x[i] >= w - slack_x) WriteF32(base, at, w + slack_x);
        if (bar_y[i] <= slack_y) WriteF32(base, at + 4, -slack_y);
        else if (bar_y[i] >= h - slack_y) WriteF32(base, at + 4, h + slack_y);
      }
    }
  }
  auto put_bar_back = [&] {
    if (!bar) return;
    for (int i = 0; i < 4; ++i) {
      WriteF32(base, quad + uw::kQuadVertexStride * i, bar_x[i]);
      WriteF32(base, quad + uw::kQuadVertexStride * i + 4, bar_y[i]);
    }
  };
#else
  auto put_bar_back = [] {};
  const bool bar = false;  // [acct2-ocr] no cinematic bars in cod3mp
#endif
  const float scale = quad ? HudScale(base) : 0.0f;
  EdgeFill fill;  // [acct2-ocr] cod3_edge_fill
  if (quad && !bar) EdgeFillApply(base, quad, scale > 0.0f, &fill);
  if (scale <= 0.0f) {
    __imp__nglListAddQuad(ctx, base);
    put_bar_back();
    fill.PutBack(base, quad);  // [acct2-ocr]
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
  put_bar_back();  // [claude-9b-think]
  fill.PutBack(base, quad);  // [acct2-ocr]
}

// nglListAddString(font, text, f1 x, f2 y, f3 depth, f4 x scale, f5 y scale,
// color): text, laid out from x with glyphs f4 wide. The position and the
// width shrink together, so the text keeps its shape in the middle.
REX_EXTERN(__imp__nglListAddString);
REX_HOOK_RAW(nglListAddString) {
  cod3::font_fill::OnFontUsed(base, ctx.r3.u32);  // [acct2-ocr] a font's added glyphs, before its first string
  if (const uint32_t t = cod3::text::RawTextFor(base, ctx.r4.u32)) ctx.r4.u64 = t;  // [acct2-ocr] raw English: ours
  cod3::text_overlay::OnString(base, ctx);  // [acct2-ocr] Russian/CJK lines: noted (unmapped place), drawn by us
  if (const float scale = HudScale(base); scale > 0.0f) {
    ctx.f1.f64 = MapX(float(ctx.f1.f64), scale);
    ctx.f4.f64 *= scale;
  }
  // [acct2-ocr] Russian/CJK lines: drawn as runs of our glyph pages through the game's own text (ui/text_native.h)
  if (cod3::text_native::DrawLine(base, ctx, [](PPCContext& c, uint8_t* b) { __imp__nglListAddString(c, b); })) return;
  // [acct2-ocr] ^0-^9 / ^: inside the line (names): drawn as colored runs (game/color_text.h)
  if (cod3::color_text::DrawRuns(base, ctx, [](PPCContext& c, uint8_t* b) { __imp__nglListAddString(c, b); })) return;
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

// [claude-9b] 2026-10-27 owner 07:14: "So I think the safe area is affecting where various hud elements are drawn.
// The compass when there are 4 player splitscreen; the players on the left side of the screen have their compass
// squashed towards the middle of the screen. The bottom and top hud elements also get skewd."
// The game places a split view's HUD elements with two leaf functions (f1 the position on the 640x480 screen, f2 the
// element's own offset, r4 the client's layout, r5 the element's flags; the result in f1), called when a view's
// layout changes. Both pull each view's HUD toward the TV's middle, a title-safe inset on the whole screen: x
// (0x8244A7C8) squeezes quarter views by 0.921875 around 320 -/+ 25 and moves flagged elements 25 more; y
// (0x8244A710) puts the bottom views' HUD at +200 instead of +240 and moves the top views' flagged elements 20
// lower. Test mv21 measured it at 16:9 too: the left quarters' HUD 110 px further right than the right quarters'
// (1600x900), the top quarters' 75 px lower (FINDINGS-SPLITSCREEN-VIEW, HUD inset).
// [claude-9b] 07:58 owner: "The hud still seems squished together. Perhaps we can have the user define where the
// safe area is and have each splitscreen inherit that." The game lays its HUD out on the 640x480 screen inside x
// 50-590 and y 40-440 (84.4 x 83.3 percent: its one-player safe area). For a split view it then shrinks that layout
// by 0.84375 x 0.83333 once more (x 0.421875 and y 0.41666666 of the screen's in a 320x240 view), so a quarter
// view's HUD covered only 71 x 69 percent of the view while its elements keep 0.65 of their size: crowded toward
// the view's middle. Now every view gets the same safe area: cod3_hud_safe_area percent of its own size (0: the
// game's one-player 84.4 x 83.3 percent), centered in the view. Element offsets keep the game's scale (0.65 in
// quarters, y 1 in halves). cod3mp_split_hud_tv_insets on: the game's own placement.
// [claude-9b] 08:40 test mv22: at 95 the compass ran 3 px off a quarter (its art reaches 27 px past the layout's
// edge), so at most 93; and one player's layout 0 isn't changed: there only the compass comes through these two
// functions (the rest keep their panel positions), so it alone moved away from the stance icon and chevron.
// [claude-9b-think] 2026-10-27 The split HUD, completed (coordinator after test mv24; FINDINGS in c9b-think/hud):
// - Moving an element from one layout to another (0x824E0F98, FEMultiLineText vfunc 0x824DFD58) first takes it back
//   to its one-player place with the inverse functions (x 0x8244A8B0, y 0x8244A660), then to the new layout with the
//   two above. Only the forward ones were ours, so a player joining or leaving in a match (4 to 3 players: layouts
//   5->3, 6->7, 7->8, 8->0; 2 to 1: 3->0) undid our placement with the console's formula: a left quarter's x 60 came
//   back as -50.6, a bottom-left y 420 as 544, off the screen. The inverses are now the exact reverse of the forward
//   ones, under the same conditions.
// - All of these read one copy of cod3_hud_safe_area and cod3mp_split_hud_tv_insets, taken while every view is whole
//   (all four players' layouts and previous layouts 0; RE_BeginFrame), so what was placed with one value is never
//   taken back with another (both settings change at once in F2).
// - Two players' halves: in mv24 the compass touched the bottom of both halves at 93 (drawn 0.75 of its size there,
//   0.65 in quarters), so their share up and down is at most 91 (about 4 px of room at 1600x900).
// - The two-player icon row (IGOWarStatusWidget, the War mode's flags; its layout change 0x824E3800 turns 3 into 5
//   and 4 into 7) was placed as a left quarter, so it sat at a quarter of the whole-width view's width. It now gets the
//   quarter's placement 160 further right, centered in the half. A change between a half and the quarter on the same
//   side, which the game skips (5 to 5), is made as two: back to the one-player place, then into the new layout.
// - The weapon name's box (IGOWeaponNameWidget vfunc 0x824F9F38, when the weapon changes: x 545 - (width + 16) to 545,
//   y 365 to 385) had both corners mapped, so it was A * (width + 16) wide around a name drawn at its own width: the
//   name stuck out on the left. Its left edge is now the mapped right edge less the name's width (measured at the
//   size it is drawn, FEText vfunc 0x824EC9F0) and the padding at the positions' scale.
// - The players' own panels (a menu class's update 0x8239D4A0 -> 0x824F7C98, FESlider 0x824F6678): their quads
//   (0x824E1C60) and texts (FEText vfunc_73 0x824D7680) are laid out in the top-left view and copied to the others 280
//   to the right and 200 down, the TV inset again (the right views' copies 40 units left, the bottom ones' 40 up).
//   Now 320 and 240, at any safe area: every view an exact copy of the top-left one. Halves keep their x (the game
//   moves none).
// - Not hooked: 0x824E1DE0 (half the size, plus 320 / 240). Its users follow the view's middle or a point in the
//   world (IGOGrenadeCookWidget, IGOTankReticleWidget, IGOHeadIcons, IGOItemIcons, panels through 0x824F7D48) and
//   the compass's +0x88 quad sets, of which mv24 saw none out of line at 93.
// - cod3_debug_hud_layout logs each distinct placement through the forward functions (with the game's own value) and
//   each panel quad and text moved.
namespace {
constexpr float kHudLayoutWidth = 540.0f, kHudLayoutHeight = 400.0f;  // x 50-590, y 40-440 of 640x480
constexpr float kGameHudShareX = kHudLayoutWidth / 640.0f, kGameHudShareY = kHudLayoutHeight / 480.0f;
constexpr float kSplitHudQuarterOffset = 0.65f;

// The share of a view the HUD spreads over (0 when the setting isn't given: the game's own).
constexpr int32_t kMaxHudSafePercent = 93;       // 94: a quarter's compass touches the view's edge (mv22)
constexpr int32_t kMaxHudSafePercentHalfY = 91;  // [claude-9b-think] halves up and down (mv24: 93 touched)
float HudSafeShare() {
  const int32_t percent = REXCVAR_GET(cod3_hud_safe_area);
  return percent <= 0 ? 0.0f : float(std::clamp(percent, 50, kMaxHudSafePercent)) / 100.0f;
}

void LogSplitHud(int axis, uint32_t layout, float share) {
  static std::atomic<uint32_t> logged{0};
  const uint32_t bit = 1u << ((layout & 15) + (axis ? 16 : 0));
  if (logged.fetch_or(bit, std::memory_order_relaxed) & bit) return;
  REXLOG_INFO("cod3mp: HUD {} of layout {}: spread over {:.1f}% of its view, centered (cod3_hud_safe_area {})",
              axis ? "y" : "x", layout, share * 100.0f, REXCVAR_GET(cod3_hud_safe_area));
}

// [claude-9b-think] The settings the split HUD is placed and taken back with.
struct SplitHud {
  float safe = 0.0f;       // HudSafeShare() when taken
  bool tv_insets = false;  // cod3mp_split_hud_tv_insets when taken
};
std::atomic<uint32_t> g_split_hud_safe{0};  // the share's bits
std::atomic<bool> g_split_hud_tv{false};
std::atomic<bool> g_split_hud_taken{false};

void TakeSplitHudSettings() {
  const float safe = HudSafeShare();
  const bool tv = REXCVAR_GET(cod3mp_split_hud_tv_insets);
  uint32_t bits = 0;
  std::memcpy(&bits, &safe, sizeof(bits));
  const bool changed = !g_split_hud_taken.load(std::memory_order_relaxed) ||
                       g_split_hud_safe.load(std::memory_order_relaxed) != bits ||
                       g_split_hud_tv.load(std::memory_order_relaxed) != tv;
  if (!changed) return;
  g_split_hud_safe.store(bits, std::memory_order_relaxed);
  g_split_hud_tv.store(tv, std::memory_order_relaxed);
  g_split_hud_taken.store(true, std::memory_order_release);
  REXLOG_INFO("cod3mp: split HUD: safe area {} ({:.1f}% of each view across, {:.1f}% up and down, halves at most "
              "{}%), TV insets {}; kept until every view is whole again",
              REXCVAR_GET(cod3_hud_safe_area), (safe > 0.0f ? safe : kGameHudShareX) * 100.0f,
              (safe > 0.0f ? safe : kGameHudShareY) * 100.0f, kMaxHudSafePercentHalfY, tv ? "on" : "off");
}

SplitHud Hud() {
  if (!g_split_hud_taken.load(std::memory_order_acquire)) TakeSplitHudSettings();
  SplitHud h;
  const uint32_t bits = g_split_hud_safe.load(std::memory_order_relaxed);
  std::memcpy(&h.safe, &bits, sizeof(bits));
  h.tv_insets = g_split_hud_tv.load(std::memory_order_relaxed);
  return h;
}

bool IsQuarter(uint32_t layout) { return layout >= 5 && layout <= 8; }
bool IsHalf(uint32_t layout) { return layout == 3 || layout == 4; }

// Ours: quarters' x always (centered in the view), halves' x only with the setting given; y in halves and quarters.
bool OursX(const SplitHud& h, uint32_t layout) {
  return !h.tv_insets && (IsQuarter(layout) || (IsHalf(layout) && h.safe > 0.0f));
}
bool OursY(const SplitHud& h, uint32_t layout) { return !h.tv_insets && (IsQuarter(layout) || IsHalf(layout)); }

float ShareX(const SplitHud& h) { return h.safe > 0.0f ? h.safe : kGameHudShareX; }
float ShareY(const SplitHud& h, uint32_t layout) {
  if (h.safe <= 0.0f) return kGameHudShareY;
  return IsHalf(layout) ? std::min(h.safe, float(kMaxHudSafePercentHalfY) / 100.0f) : h.safe;
}
// View units per one-player unit.
float SlopeX(const SplitHud& h, uint32_t layout) {
  return (IsQuarter(layout) ? 320.0f : 640.0f) * ShareX(h) / kHudLayoutWidth;
}
float SlopeY(const SplitHud& h, uint32_t layout) { return 240.0f * ShareY(h, layout) / kHudLayoutHeight; }
// A quarter's left edge; a half-view War row: the quarter's placement centered in the half.
float QuarterX0(uint32_t layout, bool centered_in_half) {
  if (centered_in_half) return 160.0f;
  return (layout == 6 || layout == 8) ? 320.0f : 0.0f;
}
float ViewY0(uint32_t layout) { return (layout == 4 || layout == 7 || layout == 8) ? 240.0f : 0.0f; }
float OffsetScaleY(uint32_t layout) { return IsHalf(layout) ? 1.0f : kSplitHudQuarterOffset; }

float ForwardX(const SplitHud& h, uint32_t layout, float x, float offset, bool centered_in_half) {
  if (IsQuarter(layout))
    return QuarterX0(layout, centered_in_half) + 160.0f + (x - 320.0f) * SlopeX(h, layout) +
           kSplitHudQuarterOffset * offset;
  return 320.0f + (x - 320.0f) * SlopeX(h, layout);  // halves: as the game, no offset
}
float InverseX(const SplitHud& h, uint32_t layout, float x, float offset, bool centered_in_half) {
  if (IsQuarter(layout))
    return 320.0f +
           (x - QuarterX0(layout, centered_in_half) - 160.0f - kSplitHudQuarterOffset * offset) / SlopeX(h, layout);
  return 320.0f + (x - 320.0f) / SlopeX(h, layout);
}
float ForwardY(const SplitHud& h, uint32_t layout, float y, float offset) {
  return ViewY0(layout) + 120.0f + (y - 240.0f) * SlopeY(h, layout) + OffsetScaleY(layout) * offset;
}
float InverseY(const SplitHud& h, uint32_t layout, float y, float offset) {
  return 240.0f + (y - ViewY0(layout) - 120.0f - OffsetScaleY(layout) * offset) / SlopeY(h, layout);
}

// The War row's layout change in progress (main thread): its real layouts were halves.
struct WarRowChange {
  bool active = false, new_half = false, old_half = false;
};
thread_local WarRowChange t_war_row;
bool WarRowInHalf(uint32_t layout, bool half) { return t_war_row.active && half && (layout == 5 || layout == 7); }

// The weapon name's update in progress: its first x is the box's left edge, 545 - (width + 16).
constexpr float kWeaponBoxRight = 545.0f, kWeaponBoxPad = 16.0f;
struct WeaponNameUpdate {
  bool active = false;
  int x_calls = 0;
};
thread_local WeaponNameUpdate t_weapon_name;

// cod3_debug_hud_layout: each distinct placement once (a few hundred lines at most).
bool DebugHudLayout() { return REXCVAR_GET(cod3_debug_hud_layout); }
bool FirstTime(uint64_t key) {
  static std::mutex mutex;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard<std::mutex> lock(mutex);
  if (seen.size() >= 4000) return false;
  return seen.insert(key).second;
}
void LogPlacement(char axis, uint32_t layout, uint32_t flags, float in, float offset, float ours, float game) {
  if (!DebugHudLayout() || layout < 3 || layout > 8) return;
  const uint64_t key = (uint64_t(uint8_t(axis)) << 56) | (uint64_t(layout & 0xFF) << 48) |
                       (uint64_t(flags & 0xFF) << 40) | (uint64_t(uint32_t(std::lround(in * 4.0f)) & 0xFFFFF) << 20) |
                       (uint64_t(uint32_t(std::lround(offset * 4.0f)) & 0xFFFFF));
  if (!FirstTime(key)) return;
  REXLOG_INFO("cod3mp: HUD layout: {} of layout {} flags {:#x}: {:.2f} offset {:.2f} -> {:.2f} (the game's {:.2f})",
              axis, layout, flags, in, offset, ours, game);
}
// The game's own value of a forward function, for the log (an isolated call; the functions only compute).
float GameValue(PPCFunc* fn, PPCContext& parent, uint8_t* base, float in, float offset, uint32_t layout,
                uint32_t flags) {
  rex::CallFrame frame(parent);
  frame.ctx.f1.f64 = double(in);
  frame.ctx.f2.f64 = double(offset);
  frame.ctx.r4.u64 = layout;
  frame.ctx.r5.u64 = flags;
  fn(frame.ctx, base);
  return float(frame.ctx.f1.f64);
}

// The players' own panels: how far an element laid out in the top-left view is moved for a layout. The game: 280 and
// 200 (the TV inset); ours: 320 and 240. Halves keep their x.
float PanelShiftX(const SplitHud& h, uint32_t layout) {
  return (layout == 6 || layout == 8) ? (h.tv_insets ? 280.0f : 320.0f) : 0.0f;
}
float PanelShiftY(const SplitHud& h, uint32_t layout) {
  return (layout == 4 || layout == 7 || layout == 8) ? (h.tv_insets ? 200.0f : 240.0f) : 0.0f;
}
// A panel quad's corners (0x824E1C60): x at +0x10, y at +0x14, 0x14 apart.
constexpr uint32_t kPanelQuadX = 0x10, kPanelQuadStride = 0x14;
// FEText's own: x (its vfunc at +0xEC), y (+0x30), place (+0x94, f1 x, f2 y); its string (+0x1C: length at +6, the
// characters at +0xC), as FEText vfunc 0x824EC9F0 reads it.
constexpr uint32_t kTextGetX = 0xEC, kTextGetY = 0x30, kTextSetPos = 0x94, kTextString = 0x1C;

std::string PanelText(uint8_t* base, uint32_t text) {
  const uint32_t s = ReadU32(base, text + kTextString);
  if (s < 0x10000) return {};
  const uint32_t length = std::min<uint32_t>(cod3::guest::ReadU16(base, s + 6), 40);
  std::string out = cod3::guest::ReadString(base, s + 0xC, length);
  for (char& c : out)
    if (c < 0x20 || c > 0x7E) c = '?';
  return out;
}

// [claude-9b-think] Taken while every view is whole (RE_BeginFrame, every frame in menus and with one player).
void RefreshSplitHudSettings(uint8_t* base) {
  for (uint32_t k = 0; k < 4; ++k) {
    const uint32_t at = kMpClientLayout + k * kMpClientStride;
    if (ReadU32(base, at) != 0 || ReadU32(base, at - 4) != 0) return;  // at - 4: the previous layout
  }
  TakeSplitHudSettings();
}
}  // namespace

REX_EXTERN(__imp__sub_8244A7C8);
REX_HOOK_RAW(sub_8244A7C8) {  // x: f1 x (640 wide), f2 offset, r4 layout, r5 element flags
  const uint32_t layout = ctx.r4.u32, flags = ctx.r5.u32;
  const float x = float(ctx.f1.f64), offset = float(ctx.f2.f64);
  const int weapon_name_call = t_weapon_name.active ? t_weapon_name.x_calls++ : -1;
  const SplitHud h = Hud();
  if (!OursX(h, layout)) {
    __imp__sub_8244A7C8(ctx, base);
    LogPlacement('x', layout, flags, x, offset, float(ctx.f1.f64), float(ctx.f1.f64));
    return;
  }
  float placed = ForwardX(h, layout, x, offset, WarRowInHalf(layout, t_war_row.new_half));
  if (weapon_name_call == 0 && flags == 0) {
    // [claude-9b-think] The weapon name's box: from its mapped right edge, the name's width and the padding.
    const float width = std::max(0.0f, kWeaponBoxRight - kWeaponBoxPad - x);
    placed = ForwardX(h, layout, kWeaponBoxRight, offset, false) - (width + kWeaponBoxPad * SlopeX(h, layout));
  }
  if (DebugHudLayout())
    LogPlacement('x', layout, flags, x, offset, placed,
                 GameValue(__imp__sub_8244A7C8, ctx, base, x, offset, layout, flags));
  ctx.f1.f64 = double(placed);
  LogSplitHud(0, layout, ShareX(h));
}

REX_EXTERN(__imp__sub_8244A710);
REX_HOOK_RAW(sub_8244A710) {  // y: f1 y (480 high), f2 offset, r4 layout, r5 element flags
  const uint32_t layout = ctx.r4.u32, flags = ctx.r5.u32;
  const float y = float(ctx.f1.f64), offset = float(ctx.f2.f64);
  const SplitHud h = Hud();
  if (!OursY(h, layout)) {
    __imp__sub_8244A710(ctx, base);
    LogPlacement('y', layout, flags, y, offset, float(ctx.f1.f64), float(ctx.f1.f64));
    return;
  }
  const float placed = ForwardY(h, layout, y, offset);
  if (DebugHudLayout())
    LogPlacement('y', layout, flags, y, offset, placed,
                 GameValue(__imp__sub_8244A710, ctx, base, y, offset, layout, flags));
  ctx.f1.f64 = double(placed);
  LogSplitHud(1, layout, ShareY(h, layout));
}

// [claude-9b-think] The inverses: the exact reverse of the two above, under the same conditions (the game's own
// otherwise, which reverses the game's own forward ones).
REX_EXTERN(__imp__sub_8244A8B0);
REX_HOOK_RAW(sub_8244A8B0) {  // x back: f1 x on the view, f2 offset, r4 layout, r5 element flags
  const uint32_t layout = ctx.r4.u32;
  const SplitHud h = Hud();
  if (!OursX(h, layout)) {
    __imp__sub_8244A8B0(ctx, base);
    return;
  }
  ctx.f1.f64 = double(InverseX(h, layout, float(ctx.f1.f64), float(ctx.f2.f64),
                               WarRowInHalf(layout, t_war_row.old_half)));
}

REX_EXTERN(__imp__sub_8244A660);
REX_HOOK_RAW(sub_8244A660) {  // y back: f1 y on the view, f2 offset, r4 layout, r5 element flags
  const uint32_t layout = ctx.r4.u32;
  const SplitHud h = Hud();
  if (!OursY(h, layout)) {
    __imp__sub_8244A660(ctx, base);
    return;
  }
  ctx.f1.f64 = double(InverseY(h, layout, float(ctx.f1.f64), float(ctx.f2.f64)));
}

// [claude-9b-think] The War row's layout change (r3 the widget, r4 the new layout, r5 the old): with a half on either
// side, the forward and inverse x above center the quarter placement it asks for in the half.
REX_EXTERN(__imp__IGOWarStatusWidget__vfunc_7);
REX_HOOK_RAW(IGOWarStatusWidget__vfunc_7) {
  const uint32_t widget = ctx.r3.u32, to = ctx.r4.u32, from = ctx.r5.u32;
  const SplitHud h = Hud();
  if (h.tv_insets || (!IsHalf(to) && !IsHalf(from))) {
    __imp__IGOWarStatusWidget__vfunc_7(ctx, base);
    return;
  }
  auto as_quarter = [](uint32_t layout) { return layout == 3 ? 5u : layout == 4 ? 7u : layout; };
  auto change = [&](uint32_t new_layout, uint32_t old_layout) {
    t_war_row = WarRowChange{true, IsHalf(new_layout), IsHalf(old_layout)};
    ctx.r3.u64 = widget;
    ctx.r4.u64 = new_layout;
    ctx.r5.u64 = old_layout;
    __imp__IGOWarStatusWidget__vfunc_7(ctx, base);
    t_war_row = WarRowChange{};
  };
  if (to != from && as_quarter(to) == as_quarter(from)) {
    change(0, from);  // the game would skip it (5 to 5): back to the one-player place first
    change(to, 0);
  } else {
    change(to, from);
  }
}

// [claude-9b-think] The weapon name's update (the box's x computed in the forward x above).
REX_EXTERN(__imp__IGOWeaponNameWidget__vfunc_2);
REX_HOOK_RAW(IGOWeaponNameWidget__vfunc_2) {
  t_weapon_name = WeaponNameUpdate{true, 0};
  __imp__IGOWeaponNameWidget__vfunc_2(ctx, base);
  t_weapon_name = WeaponNameUpdate{};
}

// [claude-9b-think] A player's panel quad moved between layouts (r3 the element, r4 the new layout, r5 the old).
REX_EXTERN(__imp__sub_824E1C60);
REX_HOOK_RAW(sub_824E1C60) {
  const uint32_t element = ctx.r3.u32, to = ctx.r4.u32, from = ctx.r5.u32;
  const SplitHud h = Hud();
  if (h.tv_insets || element < 0x10000) {
    __imp__sub_824E1C60(ctx, base);
    return;
  }
  if (to == from) return;
  const float dx = PanelShiftX(h, to) - PanelShiftX(h, from), dy = PanelShiftY(h, to) - PanelShiftY(h, from);
  float lo_x = 1e9f, hi_x = -1e9f, lo_y = 1e9f, hi_y = -1e9f;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t at = element + kPanelQuadX + kPanelQuadStride * i;
    const float x = ReadF32(base, at), y = ReadF32(base, at + 4);
    lo_x = std::min(lo_x, x);
    hi_x = std::max(hi_x, x);
    lo_y = std::min(lo_y, y);
    hi_y = std::max(hi_y, y);
    WriteF32(base, at, x + dx);
    WriteF32(base, at + 4, y + dy);
  }
  if (DebugHudLayout()) {
    const SplitHud game{h.safe, true};  // the game's own moves, for comparison
    REXLOG_INFO("cod3mp: HUD layout: panel quad {:08X} layout {} -> {}: x {:.1f}-{:.1f} y {:.1f}-{:.1f} moved {:+.0f} "
                "{:+.0f} (the game's {:+.0f} {:+.0f})",
                element, from, to, lo_x, hi_x, lo_y, hi_y, dx, dy, PanelShiftX(game, to) - PanelShiftX(game, from),
                PanelShiftY(game, to) - PanelShiftY(game, from));
  }
}

// [claude-9b-think] A player's panel text moved between layouts (FEText vfunc_73: r3 the text, r4 the new layout, r5
// the old), the same way as its quads.
REX_EXTERN(__imp__FEText__vfunc_73);
REX_HOOK_RAW(FEText__vfunc_73) {
  const uint32_t text = ctx.r3.u32, to = ctx.r4.u32, from = ctx.r5.u32;
  const SplitHud h = Hud();
  if (h.tv_insets || text < 0x10000) {
    __imp__FEText__vfunc_73(ctx, base);
    return;
  }
  if (to == from) return;
  using cod3::guest::VirtualSlot;
  cod3::guest::Call call(ctx, base);
  call.Invoke(VirtualSlot(base, text, kTextGetX), {text});
  const float x = float(call.ResultF1());
  call.Invoke(VirtualSlot(base, text, kTextGetY), {text});
  const float y = float(call.ResultF1());
  const float new_x = float(float(x - PanelShiftX(h, from)) + PanelShiftX(h, to));
  const float new_y = float(float(y - PanelShiftY(h, from)) + PanelShiftY(h, to));
  call.Invoke(VirtualSlot(base, text, kTextSetPos), {text}, {double(new_x), double(new_y)});
  if (DebugHudLayout())
    REXLOG_INFO("cod3mp: HUD layout: panel text {:08X} \"{}\" layout {} -> {}: ({:.1f}, {:.1f}) -> ({:.1f}, {:.1f})",
                text, PanelText(base, text), from, to, x, y, new_x, new_y);
}
#endif

#if defined(COD3_MP)
// [claude-9b-think] 2026-10-27 UNTESTED cod3mp_split_grass. cod3mp's grass (campaign addresses in brackets):
//  * Each view sorts and culls the grass (0x8240C840 [0x823F2DB0] calls 0x8240C138 [0x823F26A8], r3 the grass
//    manager). The cull walks the level's blocks and keeps those within kGrassDistance (plus the block's size) of the
//    camera. For each patch of a kept block, grassmanager::FrustumCullGPatch (0x8240AB38 [0x823F10A8]) sets its
//    density: 1 at the camera, falling linearly to the factor 0x825D541C at kGrassFullDensity, then as 1/distance^2.
//    Every patch it keeps is a draw (kGrassVisible counts them; the view's list is sized from it right after).
//  * The level's load (0x82411848 [0x823F6A68]) sets both distances from the level's grass (its +0x54; 2000 units
//    without it) and gives the grass material its fade: *(*(manager + 0xB8) + 0x14) = {1 / (D x 0.33), D, 0}.
// With 2-4 local players and cod3mp_split_grass below 100, each view's cull runs with both distances scaled
// (restored right after: the game's values never change), and the material's fade is scaled the same way while
// split screen lasts (restored when it ends), so the grass still fades out before it ends.
namespace {
constexpr uint32_t kGrassDistance = 0x825D5440;     // float: blocks farther than this aren't looked at
constexpr uint32_t kGrassFullDensity = 0x825D5418;  // float: the distance over which density falls to the factor
constexpr uint32_t kGrassVisible = 0x82BCCAB0;      // u32: the patches the last cull kept
constexpr uint32_t kGrassFadeParam = 0xB8;          // manager + 0xB8: the fade parameter; its +0x14: float[3]

struct GrassFade {
  uint32_t floats = 0;           // where the fade floats are (0: not seen yet)
  float original[2] = {};        // the game's values
  float written[2] = {};         // ours, while split screen lasts
  bool ours = false;
};
GrassFade g_grass_fade;
uint64_t g_grass_patches[4] = {}, g_grass_culls = 0;
int64_t g_grass_next_log = 0;

uint32_t GrassFadeFloats(uint8_t* base, uint32_t manager) {
  if (manager < 0x10000 || (manager & 3)) return 0;
  const uint32_t param = ReadU32(base, manager + kGrassFadeParam);
  if (param < 0x10000 || (param & 3)) return 0;
  const uint32_t floats = ReadU32(base, param + 0x14);
  return (floats >= 0x10000 && !(floats & 3)) ? floats : 0;
}

// The grass material's fade for distance `d` (0: the game's own back).
void SetGrassFade(uint8_t* base, uint32_t manager, float d) {
  GrassFade& f = g_grass_fade;
  const uint32_t floats = GrassFadeFloats(base, manager);
  if (!floats) return;
  const float now[2] = {ReadF32(base, floats), ReadF32(base, floats + 4)};
  if (floats != f.floats || !f.ours || now[0] != f.written[0] || now[1] != f.written[1]) {
    // A new level, or the game set it again: what is there now is the game's.
    f.floats = floats;
    f.original[0] = now[0];
    f.original[1] = now[1];
    f.ours = false;
  }
  if (d <= 0.0f) {
    if (f.ours) {
      WriteF32(base, floats, f.original[0]);
      WriteF32(base, floats + 4, f.original[1]);
      f.ours = false;
    }
    return;
  }
  f.written[0] = 1.0f / (d * 0.33f);
  f.written[1] = d;
  WriteF32(base, floats, f.written[0]);
  WriteF32(base, floats + 4, f.written[1]);
  f.ours = true;
}
}  // namespace

REX_EXTERN(__imp__sub_8240C138);
REX_HOOK_RAW(sub_8240C138) {
  const uint32_t manager = ctx.r3.u32;
  const int percent = REXCVAR_GET(cod3mp_split_grass);
  const bool split = percent < 100 && ReadU32(base, kMpLocalPlayers) >= 2;
  if (!split) {
    SetGrassFade(base, manager, 0.0f);
    __imp__sub_8240C138(ctx, base);
  } else {
    const float k = float(std::clamp(percent, 30, 100)) / 100.0f;
    const float distance = ReadF32(base, kGrassDistance), full_density = ReadF32(base, kGrassFullDensity);
    const bool sane = distance > 1.0f && distance < 100000.0f && full_density > 1.0f && full_density < 100000.0f;
    if (sane) {
      WriteF32(base, kGrassDistance, distance * k);
      WriteF32(base, kGrassFullDensity, full_density * k);
      SetGrassFade(base, manager, distance * k);
    }
    __imp__sub_8240C138(ctx, base);
    if (sane) {
      WriteF32(base, kGrassDistance, distance);
      WriteF32(base, kGrassFullDensity, full_density);
    }
    static bool logged = false;
    if (!logged && sane) {
      logged = true;
      REXLOG_INFO("cod3mp: split-screen grass: drawn to {:.0f} of the level's {:.0f} units", distance * k, distance);
    }
  }
  if (REXCVAR_GET(cod3mp_debug_split_grass)) {
    const uint32_t client = ReadU32(base, kMpActiveLocalClient);
    g_grass_patches[client & 3] += ReadU32(base, kGrassVisible);
    ++g_grass_culls;
    const int64_t now = int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
    if (now >= g_grass_next_log) {
      if (g_grass_next_log)
        REXLOG_INFO("cod3mp: grass in the last second: {} culls, patches drawn by view {} / {} / {} / {} "
                    "(cod3mp_split_grass {}%)",
                    g_grass_culls, g_grass_patches[0], g_grass_patches[1], g_grass_patches[2], g_grass_patches[3],
                    percent);
      g_grass_culls = 0;
      for (uint64_t& p : g_grass_patches) p = 0;
      g_grass_next_log = now + 1000;
    }
  }
}
#endif  // COD3_MP
