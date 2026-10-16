// cod3 - the resolution the game itself draws at (see render_resolution.h).

#include "game/render_resolution.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include "game/addresses.h"
#include "game/guest.h"
#include "game/ultrawide.h"

// For testing other sizes by hand; 0 / -1 = use the plan.
REXCVAR_DEFINE_INT32(cod3_game_width, 0, "cod3/Display",
                     "Advanced: width the game itself draws at (0 = from the render resolution)")
    .range(0, 4096);
REXCVAR_DEFINE_INT32(cod3_game_height, 0, "cod3/Display",
                     "Advanced: height the game itself draws at (0 = from the render resolution)")
    .range(0, 4096);
REXCVAR_DEFINE_INT32(cod3_game_mode, -1, "cod3/Display",
                     "Advanced (research): use entry N of the game's video mode table as is "
                     "(-1 = no)")
    .range(-1, 7);
REXCVAR_DEFINE_INT32(cod3_game_msaa, -1, "cod3/Display",
                     "Advanced: the game's own 2x MSAA (-1 = as the render method plans it: off "
                     "for native, on where it fits for scaled; 0 = off; 1 = on where it fits the "
                     "EDRAM)")
    .range(-1, 1);
// The live switch rebuilds the game's render targets while it runs (see
// ApplyPending); about 40 ms on a desktop GPU. Off: a change applies at the
// next start, as before.
REXCVAR_DEFINE_BOOL(cod3_live_resolution, true, "cod3/Display",
                    "Change the render resolution without restarting (off = a change applies at the "
                    "next start)");
REXCVAR_DEFINE_STRING(cod3_render_method, "native", "cod3/Display",
                      "native: the game draws the render resolution itself, no MSAA, no scaling "
                      "(above what the emulated EDRAM holds, the GPU's draw scale adds the rest); "
                      "scaled: the older plan, at most 1280x720 with the game's 2x MSAA, multiplied "
                      "by the GPU (game/render_resolution.h). Applies at once")
    .allowed({"native", "scaled"});
REXCVAR_DEFINE_INT32(cod3_render_percent, 100, "cod3/Display",
                     "Resolution scale in percent: 100 draws every pixel; less draws fewer pixels "
                     "and stretches them to the window, for speed. Applies at once")
    .range(25, 100);

namespace cod3 {

namespace {
std::mutex g_plan_mutex;
RenderPlan g_plan;  // game_width 0 until Cod3App sets it
}  // namespace

RenderMethod CurrentRenderMethod() {
  return REXCVAR_GET(cod3_render_method) == "scaled" ? RenderMethod::kScaled : RenderMethod::kNative;
}

const char* RenderMethodName(RenderMethod method) {
  return method == RenderMethod::kScaled ? "scaled" : "native";
}

uint32_t ScreenBufferBytes(uint32_t width, uint32_t height) {
  // One 32-bit texture: whole 32x32 tiles, rounded up to a 4 KB page.
  auto texture = [](uint32_t w, uint32_t h) -> uint64_t {
    const uint64_t bytes = uint64_t((w + 31) & ~31u) * ((h + 31) & ~31u) * 4;
    return (bytes + 4095) & ~uint64_t(4095);
  };
  const uint64_t full = texture(width, height);
  const uint64_t total = 4 * full +                                          // fronts, copy, depth
                         2 * texture((width + 3) / 4, (height + 3) / 4) +    // quarter targets
                         2 * texture(width / 2, height / 2);                 // effects (a level)
  return uint32_t(std::min<uint64_t>(total, UINT32_MAX));
}

void FitSixteenByNine(uint32_t* width, uint32_t* height) {
  // The game's picture is 16:9; a wider or taller screen gets black bars.
  if (uint64_t(*width) * 9 > uint64_t(*height) * 16)
    *width = *height * 16 / 9;
  else
    *height = *width * 9 / 16;
}

void FitAspect(uint32_t* width, uint32_t* height, float aspect) {
  if (!(aspect > 16.0f / 9.0f + 0.002f)) {
    FitSixteenByNine(width, height);
    return;
  }
  const float shape = float(*width) / float(*height);
  if (std::abs(shape - aspect) <= aspect * 0.01f) return;
  if (shape > aspect)
    *width = uint32_t(float(*height) * aspect + 0.5f);
  else
    *height = uint32_t(float(*width) / aspect + 0.5f);
}

uint32_t SurfaceTiles(uint32_t width, uint32_t height, bool msaa) {
  const uint32_t columns = (width + 79) / 80;
  const uint32_t rows = (height * (msaa ? 2 : 1) + 15) / 16;
  return columns * rows;
}

bool FitsEdram(uint32_t width, uint32_t height, bool msaa, uint32_t edram_tiles) {
  const uint32_t tiles = SurfaceTiles(width, height, msaa);
  return tiles < kConsoleEdramTiles && 2 * tiles <= edram_tiles;
}

namespace {
// Wider than 16:9: the HUD and menus use the centered 16:9 part only.
uint32_t MaxGameWidth(uint32_t width, uint32_t height) {
  return uint64_t(width) * 9 > uint64_t(height) * 16 ? kMaxWideGameWidth : kMaxGameWidth;
}

// A game resolution the older plan may pick: within 1280x720 and the EDRAM.
bool UsableScaled(uint32_t width, uint32_t height, uint32_t edram_tiles) {
  return width <= MaxGameWidth(width, height) && height <= kMaxGameHeight &&
         FitsEdram(width, height, false, edram_tiles);
}

// A game resolution native may pick: whatever the EDRAM holds without MSAA.
bool UsableNative(uint32_t width, uint32_t height, uint32_t edram_tiles) {
  return width <= kMaxWideGameWidth && height <= 4096 && FitsEdram(width, height, false, edram_tiles);
}

// Tiles one surface may take: its depth starts right after its color, below
// tile 2048, and both share the EDRAM.
uint32_t SurfaceRoom(uint32_t edram_tiles) {
  return std::min(kConsoleEdramTiles - 1, edram_tiles / 2);
}
}  // namespace

RenderPlan PlanRenderResolution(uint32_t width, uint32_t height, uint32_t edram_tiles,
                                RenderMethod method) {
  RenderPlan plan;
  plan.method = method;
  plan.edram_tiles = edram_tiles;
  plan.screen_width = std::max<uint32_t>(width, 320);
  plan.screen_height = std::max<uint32_t>(height, 240);
  const uint32_t w = plan.screen_width, h = plan.screen_height;
  const bool native = method == RenderMethod::kNative;
  auto usable = [&](uint32_t gw, uint32_t gh) {
    return native ? UsableNative(gw, gh, edram_tiles) : UsableScaled(gw, gh, edram_tiles);
  };

  // Native: the game draws all of it itself, at scale 1, without MSAA.
  if (native && usable(w, h)) {
    plan.game_width = w;
    plan.game_height = h;
    plan.scale = 1;
    plan.msaa = false;
    return plan;
  }
  if (native) {
    plan.note = fmt::format(
        "{}x{} is too big to draw natively: its color and depth need {} EDRAM tiles each and the "
        "emulated GPU has room for {} each{}, so the GPU's draw scale makes up the rest.",
        w, h, SurfaceTiles(w, h, false), SurfaceRoom(edram_tiles),
        edram_tiles <= kConsoleEdramTiles ? " (Direct3D 12 keeps the console's 10 MB; Vulkan has 20 MB)"
                                          : "");
  }

  // Exact first: the smallest scale that divides both sides and leaves a
  // game resolution that fits the EDRAM.
  for (uint32_t s = 1; s <= kMaxScale && !plan.game_width; ++s) {
    if (w % s || h % s || !usable(w / s, h / s)) continue;
    plan.scale = s;
    plan.game_width = w / s;
    plan.game_height = h / s;
  }
  // No exact fit (an odd size such as 1366x768): the smallest scale that
  // fits, rounding the game resolution down; the presenter stretches the
  // few missing pixels.
  for (uint32_t s = 1; s <= kMaxScale && !plan.game_width; ++s) {
    if (!usable(w / s, h / s) && s < kMaxScale) continue;
    plan.scale = s;
    plan.game_width = std::min(w / s, native ? kMaxWideGameWidth : MaxGameWidth(w, h));
    plan.game_height = std::min(h / s, native ? 4096u : kMaxGameHeight);
  }
  // Native never uses the game's MSAA (owner: "NATIVE (without using
  // MSAA)"); the scaled plan keeps it where it fits. R_SetMultisample refuses
  // MSAA at 1920 wide.
  plan.msaa = !native && plan.game_width != 1920 &&
              FitsEdram(plan.game_width, plan.game_height, true, edram_tiles);
  return plan;
}

bool ParseResolutionChoice(const std::string& choice, uint32_t* width, uint32_t* height) {
  struct Named {
    const char* name;
    uint32_t w, h;
  };
  static const Named kNamed[] = {{"720p", 1280, 720},   {"1080p", 1920, 1080}, {"1440p", 2560, 1440},
                                 {"2160p", 3840, 2160}, {"4k", 3840, 2160},    {"4320p", 7680, 4320},
                                 {"8k", 7680, 4320}};
  for (const auto& n : kNamed) {
    if (choice == n.name) {
      *width = n.w;
      *height = n.h;
      return true;
    }
  }
  unsigned w = 0, h = 0;
  char tail = 0;
  if (choice.size() <= 16 && std::sscanf(choice.c_str(), "%ux%u%c", &w, &h, &tail) == 2 &&
      w >= kMinCustomWidth && h >= kMinCustomHeight && w <= kMaxCustomWidth && h <= kMaxCustomHeight) {
    *width = w;
    *height = h;
    return true;
  }
  return false;  // "auto", "original", "" or unknown
}

bool GetScreenSize(uint32_t* width, uint32_t* height) {
#if defined(_WIN32)
  // The primary display's current mode in real pixels, whatever the DPI
  // scaling. (The game's own SDL lives in the runtime DLL; a second, unused
  // copy here couldn't be asked.)
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) || !mode.dmPelsWidth ||
      !mode.dmPelsHeight)
    return false;
  *width = mode.dmPelsWidth;
  *height = mode.dmPelsHeight;
  return true;
#else
  // The runtime library exports SDL, so this is the instance it set up.
  const SDL_DisplayID display = SDL_GetPrimaryDisplay();
  if (!display) return false;
  const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(display);
  if (!mode || mode->w <= 0 || mode->h <= 0) return false;
  // Wayland reports the mode in logical pixels with a pixel density.
  const float density = mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
  *width = uint32_t(float(mode->w) * density + 0.5f);
  *height = uint32_t(float(mode->h) * density + 0.5f);
  return true;
#endif
}

void SetRenderPlan(const RenderPlan& plan) {
  std::lock_guard lock(g_plan_mutex);
  g_plan = plan;
}

namespace {
uint32_t g_edram_tiles = kConsoleEdramTiles;
}  // namespace
void SetEdramTilesAvailable(uint32_t tiles) { g_edram_tiles = tiles; }
uint32_t EdramTilesAvailable() { return g_edram_tiles; }

RenderPlan GetRenderPlan() {
  std::lock_guard lock(g_plan_mutex);
  return g_plan;
}

namespace {
// Where the presenter shows a picture of this shape in a window or screen of
// view_width x view_height, with its rounding (the SDK's ui/presenter.cpp,
// letterboxing on): the game's 16:9, or round(1000 x shape) x 1000 while the
// picture is wider (game/ultrawide.cpp). "auto" draws exactly this size.
void DisplayRect(uint32_t view_width, uint32_t view_height, float aspect, uint32_t* width,
                 uint32_t* height) {
  const bool wide = aspect > kSixteenByNine + 0.002f;
  const uint64_t ax = wide ? uint64_t(std::min(4095L, std::lround(1000.0f * aspect))) : 16;
  const uint64_t ay = wide ? 1000 : 9;
  const uint64_t w = std::max<uint32_t>(view_width, 1), h = std::max<uint32_t>(view_height, 1);
  // Without letterboxing (present_letterbox off) the picture fills the
  // window's width or height and its other side runs past the edges.
  const bool bars = rex::cvar::GetFlagByName("present_letterbox") != "false";
  if (w * ay > h * ax) {
    // Wider than the picture: the height decides, bars at the sides.
    const uint64_t out_h = (w * ay + ax / 2) / ax;
    const bool fit = bars && out_h > h;
    *height = uint32_t(fit ? h : out_h);
    *width = fit ? uint32_t((h * ax + ay / 2) / ay) : uint32_t(w);
  } else {
    const uint64_t out_w = (h * ax + ay / 2) / ay;
    const bool fit = bars && out_w > w;
    *width = uint32_t(fit ? w : out_w);
    *height = fit ? uint32_t((w * ay + ax / 2) / ax) : uint32_t(h);
  }
}

std::string Size(uint32_t width, uint32_t height) { return fmt::format("{}x{}", width, height); }
}  // namespace

RenderPlan PlanForView(const std::string& choice, uint32_t view_width, uint32_t view_height) {
  if (!view_width || !view_height) {
    view_width = 1280;
    view_height = 720;
  }
  // The picture's shape: 16:9 (bars on other windows) or wider, and where the
  // presenter shows it.
  const float aspect = AspectForWindow(view_width, view_height);
  uint32_t display_width = 0, display_height = 0;
  DisplayRect(view_width, view_height, aspect, &display_width, &display_height);
  if (choice == "original") {
    // The Xbox 360's own table entry (1040x624 with its 2x MSAA) at scale 1,
    // stretched to the screen like the console's video scaler did; a bigger
    // scale would draw more than the screen and shrink it.
    RenderPlan plan;
    plan.choice = "original";
    plan.scale = 1;
    plan.display_width = display_width;
    plan.display_height = display_height;
    plan.edram_tiles = EdramTilesAvailable();
    return plan;
  }

  const bool typed = choice.find('x') != std::string::npos;
  uint32_t width = 0, height = 0;
  std::string note;
  if (!ParseResolutionChoice(choice, &width, &height)) {
    width = display_width;  // "auto": the window or screen, pixel for pixel
    height = display_height;
  } else {
    // A named resolution counts lines: 1440p on a 21:9 screen is 3440x1440.
    if (!typed && aspect > kSixteenByNine) width = uint32_t(float(height) * aspect + 0.5f);
    // The picture's shape (a typed size within 1% of it stays as typed).
    FitAspect(&width, &height, aspect);
    // Never bigger than the window: it would only be shrunk to fit.
    if (width > display_width || height > display_height) {
      note = fmt::format("{} is bigger than the {} ({}), so the game draws the {}'s size: a bigger "
                         "picture would only be shrunk to fit.",
                         typed ? Size(width, height) : choice,
                         rex::cvar::GetFlagByName("fullscreen") == "true" ? "screen" : "window",
                         Size(display_width, display_height),
                         rex::cvar::GetFlagByName("fullscreen") == "true" ? "screen" : "window");
      width = display_width;
      height = display_height;
    }
  }
  const uint32_t asked_width = width, asked_height = height;

  // A resolution percentage draws fewer pixels; the presenter stretches them.
  const uint32_t percent =
      uint32_t(std::clamp<int32_t>(REXCVAR_GET(cod3_render_percent), int32_t(kMinRenderPercent), 100));
  if (percent < 100) {
    width = (width * percent + 50) / 100;
    height = (height * percent + 50) / 100;
  }
  // The smallest the game draws, 320x240, keeping the shape (a tiny window).
  if (width < 320 || height < 240) {
    const double grow = std::max(320.0 / std::max<uint32_t>(width, 1), 240.0 / std::max<uint32_t>(height, 1));
    width = uint32_t(std::ceil(width * grow));
    height = uint32_t(std::ceil(height * grow));
  }

  RenderPlan plan = PlanRenderResolution(width, height, EdramTilesAvailable(), CurrentRenderMethod());
  plan.choice = choice.empty() ? "auto" : choice;
  plan.display_width = display_width;
  plan.display_height = display_height;
  plan.screen_width = asked_width;
  plan.screen_height = asked_height;
  plan.percent = percent;
  // cod3_game_msaa (the console's r_aaSamples) turns the game's 2x MSAA on or
  // off by hand, still only where it fits (R_SetMultisample refuses 1920 wide).
  if (const int32_t force = REXCVAR_GET(cod3_game_msaa); force >= 0)
    plan.msaa = force != 0 && plan.game_width != 1920 &&
                FitsEdram(plan.game_width, plan.game_height, true, plan.edram_tiles);
  if (!note.empty()) plan.note = plan.note.empty() ? note : note + " " + plan.note;
  return plan;
}

std::string DescribePlan(const RenderPlan& plan) {
  if (!plan.game_width) return "the Xbox 360's own resolution";
  std::string s = Size(plan.game_width, plan.game_height);
  if (plan.scale <= 1) {
    s += plan.msaa ? " itself with its 2x MSAA" : " itself, no MSAA";
  } else {
    s += plan.msaa ? " with its 2x MSAA" : ", no MSAA";
    s += fmt::format(", drawn {}x by the GPU = {}", plan.scale,
                     Size(plan.output_width(), plan.output_height()));
  }
  if (plan.percent < 100) s += fmt::format(" ({}%)", plan.percent);
  // How it reaches the screen.
  const uint32_t ow = plan.output_width(), oh = plan.output_height();
  if (!plan.display_width || (ow == plan.display_width && oh == plan.display_height))
    s += plan.scale <= 1 ? ", no scaling" : ", pixel for pixel";
  else if (ow <= plan.display_width && oh <= plan.display_height)
    s += ", stretched to " + Size(plan.display_width, plan.display_height) + " on screen";
  else
    s += ", shrunk to " + Size(plan.display_width, plan.display_height) + " on screen";
  return s;
}

}  // namespace cod3

namespace {
namespace v = cod3::addr::video;
using cod3::guest::ReadU32;
using cod3::guest::WriteU32;

// The game's 2x MSAA at this size: cod3_game_msaa, else what the plan for
// this size decided (native: off), else (a size set by hand) off for native
// and on where it fits for the scaled plan and the Xbox 360's own size (the
// console's 2048 tiles until a plan says the GPU has more).
bool WantGameMsaa(uint32_t width, uint32_t height, const cod3::RenderPlan& plan) {
  const uint32_t tiles = plan.game_width ? plan.edram_tiles : cod3::kConsoleEdramTiles;
  // R_SetMultisample refuses MSAA at 1920 wide.
  const bool fits = width != 1920 && cod3::FitsEdram(width, height, true, tiles);
  // cod3_game_msaa 1 (r_aaSamples 2) asks for it, still only where it fits.
  if (const int32_t force = REXCVAR_GET(cod3_game_msaa); force >= 0) return force != 0 && fits;
  if (plan.game_width == width && plan.game_height == height) return plan.msaa;
  if (plan.game_width && plan.method == cod3::RenderMethod::kNative) return false;
  return fits;
}

// Set while the live switch remakes the targets: it has decided the MSAA.
bool g_switching = false;

// Guest physical memory not in use, in MB: the physical base heap holds
// every physical allocation, whatever its page size.
uint32_t FreeGuestMb() {
  if (auto* ks = rex::system::kernel_state(); ks && ks->memory()) {
    if (auto* heap = ks->memory()->GetPhysicalHeap())
      return uint32_t(uint64_t(heap->GetUnreservedPageCount()) * heap->page_size() >> 20);
  }
  return 0;
}

// Logs what the game draws now and the guest memory its screen-sized
// buffers take (the console's 512 MB hold everything).
void LogDrawn(const char* when, uint32_t width, uint32_t height, bool msaa) {
  const cod3::RenderPlan plan = cod3::GetRenderPlan();
  std::string what;
  if (plan.game_width == width && plan.game_height == height && plan.msaa == msaa)
    what = cod3::DescribePlan(plan);
  else
    what = fmt::format("{}x{}{}", width, height, msaa ? " with its 2x MSAA" : ", no MSAA");
  REXLOG_INFO("cod3: {}: the game draws {} (screen buffers about {} MB of guest memory; the "
              "console's 1040x624 took {} MB; {} MB of guest physical memory free)",
              when, what, (cod3::ScreenBufferBytes(width, height) + (1u << 19)) >> 20,
              (cod3::ScreenBufferBytes(1040, 624) + (1u << 19)) >> 20, FreeGuestMb());
}
}  // namespace

// main() -> SelectVideoMode: the game copies the chosen mode table entry to
// addr::video::kVideoMode. Replace its size with the plan's.
REX_EXTERN(__imp__R_SelectVideoMode);
REX_HOOK_RAW(R_SelectVideoMode) {
  __imp__R_SelectVideoMode(ctx, base);
  if (const int32_t mode = REXCVAR_GET(cod3_game_mode); mode >= 0) {
    for (uint32_t i = 0; i < 16; i += 4)
      WriteU32(base, v::kVideoMode + i, ReadU32(base, v::kModeTable + uint32_t(mode) * 16 + i));
    WriteU32(base, v::kModeId, uint32_t(mode));
  }
  const uint32_t w = ReadU32(base, v::kVideoMode), h = ReadU32(base, v::kVideoMode + 4);
  uint32_t want_w = uint32_t(std::max(0, REXCVAR_GET(cod3_game_width)));
  uint32_t want_h = uint32_t(std::max(0, REXCVAR_GET(cod3_game_height)));
  if (!want_w || !want_h) {
    want_w = cod3::GetRenderPlan().game_width;
    want_h = cod3::GetRenderPlan().game_height;
  }
  if (want_w && want_h) {
    WriteU32(base, v::kVideoMode, want_w);
    WriteU32(base, v::kVideoMode + 4, want_h);
  }
  REXLOG_INFO("cod3: the game's video mode is {}x{} (its own choice: {}x{})",
              ReadU32(base, v::kVideoMode), ReadU32(base, v::kVideoMode + 4), w, h);
}

// R_SetMultisample(type): main() asks for 2x (type 1). Turn it off when the
// plan says so (native) or the game's resolution doesn't leave room for it in
// the EDRAM. A live switch has decided already.
REX_EXTERN(__imp__R_SetMultisample);
REX_HOOK_RAW(R_SetMultisample) {
  const uint32_t asked = ctx.r3.u32;
  const uint32_t width = ReadU32(base, v::kVideoMode), height = ReadU32(base, v::kVideoMode + 4);
  if (asked != 0 && !g_switching) {
    const bool on = WantGameMsaa(width, height, cod3::GetRenderPlan());
    if (!on) ctx.r3.u64 = 0;
  }
  __imp__R_SetMultisample(ctx, base);
  // main() calls it once at start (the device's first call passes 0).
  if (asked != 0 && !g_switching)
    LogDrawn("render resolution at start", width, height, ReadU32(base, cod3::addr::live_video::kPresentParams + 16) != 0);
}

// ===========================================================================
// Changing the resolution while the game runs (render_resolution.h)
// ===========================================================================
namespace {
namespace lv = cod3::addr::live_video;
using cod3::guest::Call;
using Clock = std::chrono::steady_clock;

// A request is applied once it has stayed the same this long, so dragging a
// window's edge makes one switch at the end instead of one per frame.
constexpr auto kSettleTime = std::chrono::milliseconds(300);
// Frames to wait for a safe moment before leaving it to the next start.
constexpr int kMaxWaitFrames = 600;

// What the settings ask for. A table entry's size ("original",
// cod3_game_mode) is read on the game thread.
struct Request {
  cod3::RenderPlan plan;
  int table_entry = -1;       // -1: the plan's size, -2: the game's own choice, else that entry
  uint32_t force_width = 0;   // cod3_game_width/height, both or neither
  uint32_t force_height = 0;
  int msaa_setting = -1;      // cod3_game_msaa
  bool operator==(const Request& o) const {
    return plan.choice == o.plan.choice && plan.game_width == o.plan.game_width &&
           plan.game_height == o.plan.game_height && plan.scale == o.plan.scale &&
           plan.msaa == o.plan.msaa && plan.method == o.plan.method &&
           plan.percent == o.plan.percent && plan.display_width == o.plan.display_width &&
           plan.display_height == o.plan.display_height && table_entry == o.table_entry &&
           force_width == o.force_width && force_height == o.force_height &&
           msaa_setting == o.msaa_setting;
  }
};

std::mutex g_live_mutex;     // guards everything up to g_note
Request g_pending;
bool g_has_pending = false;
uint64_t g_pending_serial = 0;
Clock::time_point g_pending_since;
Request g_last_asked;        // the last request (or what the start planned)
bool g_have_last = false;
std::string g_note;          // for the settings window
std::atomic<bool> g_pending_flag{false};

// UI thread.
std::atomic<uint32_t> g_window_width{0};
std::atomic<uint32_t> g_window_height{0};
float g_window_aspect = 0.0f;
bool g_watching = false;
std::atomic<bool> g_settings_changed{false};

// Game thread.
int g_wait_frames = 0;
std::string g_wait_reason;
// The list kBeginNextFrame opened after the last present, and where its
// memory stood then: while both are unchanged, nothing has been recorded.
uint32_t g_fresh_list = 0;
uint32_t g_fresh_cursor = 0;

enum class Outcome { kDone, kUnchanged, kWait, kRefused };

Request MakeRequest() {
  Request r;
  std::string choice = rex::cvar::GetFlagByName("cod3_render_resolution");
  if (choice.empty()) choice = "auto";
  uint32_t width = g_window_width.load(std::memory_order_relaxed);
  uint32_t height = g_window_height.load(std::memory_order_relaxed);
  if (!width || !height) {
    const cod3::RenderPlan now = cod3::GetRenderPlan();
    width = now.screen_width ? now.screen_width : 1280;
    height = now.screen_height ? now.screen_height : 720;
  }
  r.plan = cod3::PlanForView(choice, width, height);
  if (const int32_t mode = REXCVAR_GET(cod3_game_mode); mode >= 0)
    r.table_entry = mode;
  else if (choice == "original")
    r.table_entry = -2;
  const int32_t force_w = REXCVAR_GET(cod3_game_width), force_h = REXCVAR_GET(cod3_game_height);
  if (force_w > 0 && force_h > 0) {
    r.force_width = uint32_t(force_w);
    r.force_height = uint32_t(force_h);
  }
  r.msaa_setting = REXCVAR_GET(cod3_game_msaa);
  return r;
}

std::string Describe(const Request& r) {
  if (r.force_width) return fmt::format("{}x{} (cod3_game_width/height)", r.force_width, r.force_height);
  if (r.table_entry >= 0) return fmt::format("video mode table entry {}", r.table_entry);
  if (r.table_entry == -2) return "the Xbox 360's own resolution";
  return cod3::DescribePlan(r.plan);
}

std::string DescribeSize(uint32_t width, uint32_t height, bool msaa) {
  return fmt::format("{}x{}{}", width, height, msaa ? " with 2x MSAA" : "");
}

// The draw resolution scale, as the start sets it; the GPU emulation rebuilds
// for it at the end of a frame (SDK patch 0014).
void SetScale(uint32_t scale) {
  if (!scale || !rex::cvar::GetFlagInfo("resolution_scale")) return;
  const std::string value = std::to_string(scale);
  for (const char* name : {"resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y"})
    if (rex::cvar::GetFlagByName(name) != value) rex::cvar::SetFlagByName(name, value);
}

uint32_t CurrentScale() {
  const int scale = std::atoi(rex::cvar::GetFlagByName("draw_resolution_scale_x").c_str());
  return uint32_t(std::max(1, scale));
}

// Remakes everything the game sized from its video mode, in the order the
// game itself makes it; see render_resolution.h and addresses.h (live_video).
Outcome SwitchGame(PPCContext& ctx, uint8_t* base, uint32_t width, uint32_t height, bool msaa,
                   std::string* why) {
  const uint32_t tiles = cod3::EdramTilesAvailable();
  if (width < 64 || height < 64 || width > cod3::kMaxWideGameWidth || height > 4096) {
    *why = fmt::format("{}x{} is outside what the game can draw", width, height);
    return Outcome::kRefused;
  }
  if (!cod3::FitsEdram(width, height, false, tiles)) {
    *why = fmt::format("{}x{} doesn't fit the emulated EDRAM ({} tiles)", width, height, tiles);
    return Outcome::kRefused;
  }
  // R_SetMultisample refuses MSAA at 1920 wide and then resets nothing.
  if (msaa && (width == 1920 || !cod3::FitsEdram(width, height, true, tiles))) msaa = false;

  if (!ReadU32(base, lv::kDevice) || !ReadU32(base, lv::kFrontBufferA + lv::kRecordTexture)) {
    *why = "the renderer isn't up yet";
    return Outcome::kWait;
  }
  const uint32_t old_width = ReadU32(base, v::kVideoMode);
  const uint32_t old_height = ReadU32(base, v::kVideoMode + 4);
  const bool old_msaa = ReadU32(base, lv::kPresentParams + 16) != 0;
  if (old_width == width && old_height == height && old_msaa == msaa) return Outcome::kUnchanged;
  if (ReadU32(base, cod3::addr::movie::kHandle)) {
    *why = "a movie is playing";
    return Outcome::kWait;
  }

  const auto start = Clock::now();
  const uint32_t free_before_mb = FreeGuestMb();
  Call call(ctx, base);
  // The last frame's render job, and the level effects' jobs.
  call.Invoke(lv::kWaitJobGroup, {ReadU32(base, lv::kRenderJobGroup)});
  const uint32_t effects_color = ReadU32(base, lv::kEffectsColor);
  const uint32_t effects_depth = ReadU32(base, lv::kEffectsDepth);
  const bool level = effects_color || effects_depth;
  if (level) call.Invoke(lv::kWaitJobGroup, {ReadU32(base, lv::kEffectsJobGroup)});
  // The next frame's list is open already, but nothing may be in it yet.
  const uint32_t list = ReadU32(base, lv::kCurrentList);
  if (list && (list != g_fresh_list || ReadU32(base, lv::kListCursor) != g_fresh_cursor)) {
    *why = "a frame is being recorded";
    return Outcome::kWait;
  }
  if (ReadU32(base, lv::kTargetStackCount)) {
    *why = "a render target is bound";
    return Outcome::kWait;
  }
  // Unbind everything and wait until the GPU has finished.
  call.Invoke(lv::kUnbindAndIdle);

  const uint32_t quarter_a = ReadU32(base, lv::kQuarterTargetA + lv::kRecordTexture);
  const uint32_t quarter_b = ReadU32(base, lv::kQuarterTargetB + lv::kRecordTexture);
  const uint32_t full_depth = ReadU32(base, lv::kFullDepth);
  const uint32_t depth_in_use = ReadU32(base, lv::kDepthInUse);

  // The new size where the device reset and the screen targets read it.
  WriteU32(base, v::kVideoMode, width);
  WriteU32(base, v::kVideoMode + 4, height);
  WriteU32(base, lv::kPresentParams, width);
  WriteU32(base, lv::kPresentParams + 4, height);
  // Reset the device and remake the screen targets (our hook leaves the MSAA
  // decided above as it is).
  g_switching = true;
  call.Invoke(v::kSetMultisample, {msaa ? 1u : 0u});
  g_switching = false;

  // The quarter-size targets were remade without releasing the old ones.
  if (quarter_a && quarter_a != ReadU32(base, lv::kQuarterTargetA + lv::kRecordTexture))
    call.Invoke(lv::kResourceRelease, {quarter_a});
  if (quarter_b && quarter_b != ReadU32(base, lv::kQuarterTargetB + lv::kRecordTexture))
    call.Invoke(lv::kResourceRelease, {quarter_b});

  // The full-size depth texture, which Sys_Main makes once.
  uint32_t new_full_depth = full_depth;
  if (full_depth) {
    call.Invoke(lv::kReleaseTexture, {full_depth});
    WriteU32(base, lv::kFullDepth, 0);
    call.Invoke(lv::kMakeFullDepth);
    new_full_depth = ReadU32(base, lv::kFullDepth);
  }

  // In a level: the effects' half-size color and depth, made like
  // CG_RegisterGraphics makes them (half the back buffer's size).
  uint32_t new_color = effects_color, new_depth = effects_depth;
  if (level) {
    if (effects_color) call.Invoke(lv::kReleaseTexture, {effects_color});
    if (effects_depth) call.Invoke(lv::kReleaseTexture, {effects_depth});
    WriteU32(base, lv::kEffectsColor, 0);
    WriteU32(base, lv::kEffectsDepth, 0);
    const int32_t half_w = int32_t(ReadU32(base, lv::kBackBufferCopy)) / 2;
    const int32_t half_h = int32_t(ReadU32(base, lv::kBackBufferCopy + 4)) / 2;
    WriteU32(base, lv::kEffectsWidth, uint32_t(half_w));
    WriteU32(base, lv::kEffectsHeight, uint32_t(half_h));
    if (effects_color) {
      new_color = uint32_t(call.Invoke(lv::kCreateTexture, {lv::kEffectsColorFlags, lv::kEffectsColorFormat,
                                                            uint64_t(half_w), uint64_t(half_h), 1, 1}));
      WriteU32(base, lv::kEffectsColor, new_color);
    }
    if (effects_depth) {
      new_depth = uint32_t(call.Invoke(lv::kCreateTexture, {lv::kEffectsDepthFlags, lv::kDepthFormat,
                                                            uint64_t(half_w), uint64_t(half_h), 0, 1}));
      WriteU32(base, lv::kEffectsDepth, new_depth);
    }
  }

  // The depth texture the shaders read: the same one as before, remade.
  uint32_t in_use = depth_in_use;
  if (depth_in_use == full_depth)
    in_use = new_full_depth;
  else if (level && depth_in_use == effects_depth)
    in_use = new_depth;
  WriteU32(base, lv::kDepthInUse, in_use);
  // A device reset can drop the gamma ramp; the next present sets the game's.
  cod3::guest::WriteU8(base, lv::kGammaDirty, 1);

  const bool made = ReadU32(base, lv::kFrontBufferA + lv::kRecordTexture) &&
                    ReadU32(base, lv::kFrontBufferB + lv::kRecordTexture) &&
                    ReadU32(base, lv::kBackBufferCopy + lv::kRecordTexture) &&
                    ReadU32(base, lv::kQuarterTargetA + lv::kRecordTexture) &&
                    ReadU32(base, lv::kQuarterTargetB + lv::kRecordTexture) &&
                    (!full_depth || new_full_depth) && (!effects_color || new_color) &&
                    (!effects_depth || new_depth);
  const bool now_msaa = ReadU32(base, lv::kPresentParams + 16) != 0;
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  if (!made) {
    *why = fmt::format("the game couldn't make its render targets at {}", DescribeSize(width, height, msaa));
    REXLOG_ERROR("cod3: render resolution: {}; going back to {}", *why,
                 DescribeSize(old_width, old_height, old_msaa));
    std::string ignored;
    SwitchGame(ctx, base, old_width, old_height, old_msaa, &ignored);
    return Outcome::kRefused;
  }
  // The old targets' memory goes back to the guest (free before -> after).
  REXLOG_INFO("cod3: render resolution changed live: the game's targets are {} (were {}){} in {:.0f} "
              "ms; guest physical memory free {} MB -> {} MB",
              DescribeSize(width, height, now_msaa), DescribeSize(old_width, old_height, old_msaa),
              level ? ", level effects remade" : "", ms, free_before_mb, FreeGuestMb());
  return Outcome::kDone;
}

// Game thread, at the start of every frame while a request waits.
void ApplyPending(PPCContext& ctx, uint8_t* base) {
  Request r;
  uint64_t serial = 0;
  {
    std::lock_guard lock(g_live_mutex);
    if (!g_has_pending) {
      g_pending_flag.store(false, std::memory_order_relaxed);
      return;
    }
    if (Clock::now() - g_pending_since < kSettleTime) return;
    r = g_pending;
    serial = g_pending_serial;
  }
  // The size: the plan's, a table entry's or the one set by hand, as the
  // R_SelectVideoMode hook picks it at start.
  uint32_t width = r.plan.game_width, height = r.plan.game_height;
  int entry = r.table_entry == -2 ? int(ReadU32(base, v::kModeId)) : r.table_entry;
  if (entry >= 0) {
    entry = std::min(entry, 7);
    width = ReadU32(base, v::kModeTable + uint32_t(entry) * 16);
    height = ReadU32(base, v::kModeTable + uint32_t(entry) * 16 + 4);
  }
  if (r.force_width) {
    width = r.force_width;
    height = r.force_height;
  }
  std::string why;
  Outcome outcome = SwitchGame(ctx, base, width, height, WantGameMsaa(width, height, r.plan), &why);
  if (outcome == Outcome::kWait) {
    if (why != g_wait_reason) {
      REXLOG_INFO("cod3: render resolution {}x{}: waiting ({})", width, height, why);
      g_wait_reason = why;
    }
    if (++g_wait_frames < kMaxWaitFrames) return;
    why = "no safe moment came: " + why;
    outcome = Outcome::kRefused;
  }
  g_wait_frames = 0;
  g_wait_reason.clear();

  std::string note;
  if (outcome == Outcome::kRefused) {
    REXLOG_WARN("cod3: render resolution {}x{} not changed now: {}; it applies at the next start",
                width, height, why);
    note = "Couldn't change it now (" + why + "); it applies at the next start.";
  } else {
    const std::string before = cod3::DescribePlan(cod3::GetRenderPlan());
    // The GPU's scale follows (1 for "original").
    SetScale(r.plan.scale);
    cod3::RenderPlan plan = r.plan;
    plan.game_width = ReadU32(base, v::kVideoMode);
    plan.game_height = ReadU32(base, v::kVideoMode + 4);
    plan.msaa = ReadU32(base, lv::kPresentParams + 16) != 0;
    plan.scale = r.plan.scale ? r.plan.scale : CurrentScale();
    plan.edram_tiles = cod3::EdramTilesAvailable();
    if (!plan.screen_width) {
      plan.screen_width = plan.output_width();
      plan.screen_height = plan.output_height();
    }
    cod3::SetRenderPlan(plan);
    if (outcome == Outcome::kDone)
      LogDrawn("render resolution changed", plan.game_width, plan.game_height, plan.msaa);
    else if (const std::string now = cod3::DescribePlan(plan); now != before)
      // Same render targets, shown differently (a window of another size).
      REXLOG_INFO("cod3: render resolution changed: the game draws {} (its render targets stay)", now);
  }
  std::lock_guard lock(g_live_mutex);
  if (outcome == Outcome::kRefused) g_have_last = false;  // the same choice may be tried again
  if (g_pending_serial == serial) {
    g_has_pending = false;
    g_pending_flag.store(false, std::memory_order_relaxed);
    g_note = note;
  }
}

void WatchSettings() {
  for (const char* name : {"cod3_render_resolution", "cod3_render_method", "cod3_render_percent",
                           "cod3_game_width", "cod3_game_height", "cod3_game_mode", "cod3_game_msaa",
                           "cod3_aspect_ratio", "present_letterbox"})
    rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) {
      g_settings_changed.store(true, std::memory_order_relaxed);
    });
}
}  // namespace

namespace cod3 {

void NoteWindowSize(uint32_t width, uint32_t height) {
  if (!width || !height) return;  // minimized
  const float aspect = AspectForWindow(width, height);
  const bool changed = width != g_window_width.load(std::memory_order_relaxed) ||
                       height != g_window_height.load(std::memory_order_relaxed) ||
                       aspect != g_window_aspect;
  g_window_width.store(width, std::memory_order_relaxed);
  g_window_height.store(height, std::memory_order_relaxed);
  g_window_aspect = aspect;
  if (!g_watching) {
    // The first size is the one the start planned for.
    g_watching = true;
    WatchSettings();
    const Request start = MakeRequest();
    std::lock_guard lock(g_live_mutex);
    g_last_asked = start;
    g_have_last = true;
    return;
  }
  if (g_settings_changed.exchange(false, std::memory_order_relaxed) || changed)
    RequestRenderResolution();
}

void RequestRenderResolution() {
  const Request r = MakeRequest();
  std::lock_guard lock(g_live_mutex);
  if (g_have_last && r == g_last_asked) return;
  g_last_asked = r;
  g_have_last = true;
  if (!REXCVAR_GET(cod3_live_resolution)) {
    // Not switching live: the start applies the new plan.
    g_note = "Applies at the next start (cod3_live_resolution turns the live change on).";
    REXLOG_INFO("cod3: render resolution {} ({} method) asked for: the game draws {} (at the next "
                "start; live changes are off)",
                r.plan.choice, cod3::RenderMethodName(r.plan.method), Describe(r));
    return;
  }
  g_pending = r;
  g_has_pending = true;
  ++g_pending_serial;
  g_pending_since = Clock::now();
  g_note = "Changing to " + Describe(r) + "...";
  g_pending_flag.store(true, std::memory_order_release);
  REXLOG_INFO("cod3: render resolution {} ({} method) asked for: the game draws {} (at the next frame){}",
              r.plan.choice, cod3::RenderMethodName(r.plan.method), Describe(r),
              r.plan.note.empty() ? "" : ". " + r.plan.note);
}

bool ApplyRenderResolutionNow(PPCContext& ctx, uint8_t* base, uint32_t width, uint32_t height,
                              bool msaa) {
  std::string why;
  const Outcome outcome = SwitchGame(ctx, base, width, height, msaa, &why);
  if (outcome == Outcome::kWait || outcome == Outcome::kRefused)
    REXLOG_WARN("cod3: render resolution {}x{} not changed now: {}", width, height, why);
  return outcome == Outcome::kDone || outcome == Outcome::kUnchanged;
}

std::string RenderResolutionNote() {
  std::lock_guard lock(g_live_mutex);
  return g_note;
}

}  // namespace cod3

// After a present: note the list opened for the next frame (see
// addr::live_video::kBeginNextFrame).
REX_EXTERN(__imp__sub_82130808);
REX_HOOK_RAW(sub_82130808) {
  __imp__sub_82130808(ctx, base);
  g_fresh_list = ReadU32(base, lv::kCurrentList);
  g_fresh_cursor = ReadU32(base, lv::kListCursor);
}

// Com_Frame: the top of the game's frame, where the last frame has been
// drawn and shown. A waiting resolution change runs here.
REX_EXTERN(__imp__Com_Frame);
REX_HOOK_RAW(Com_Frame) {
  if (g_pending_flag.load(std::memory_order_acquire)) ApplyPending(ctx, base);
  __imp__Com_Frame(ctx, base);
}
