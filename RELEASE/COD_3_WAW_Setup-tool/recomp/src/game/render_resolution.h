// cod3 - the resolution the game itself draws at.
//
// Call of Duty 3 doesn't draw at 1280x720: main() picks an entry from a
// table of video modes (see addr::video) and on a widescreen display that is
// 1040x624 with 2x MSAA, which the Xbox 360's video scaler stretched to the
// screen. The emulated GPU can only multiply the game's resolution by a whole
// number, so 1040x624 never lands exactly on a PC screen: every setting was
// scaled up or down on the way out.
//
// Native (the default, cod3_render_method): the game draws the whole picture
// itself, at the window's or screen's own size, without MSAA and without any
// scaling: 1080p is 1920x1080 drawn by the game. Every screen-sized buffer is
// made from the video mode, and the menus and HUD are laid out in a 640x480
// virtual screen that covers whatever size the game draws (checked at
// 1920x1080: title, menus, loading, a level). The limit is the EDRAM, the
// console's 10 MB of render target memory: the color and depth of one
// picture must both fit. The emulated EDRAM has 20 MB on Vulkan (SDK patch
// 0010), which holds up to about 2.1 million pixels (1920x1080, 1920x1200,
// 2048x1152); Direct3D 12 keeps the console's 10 MB (about 1440x810).
//
// Above that (1440p, 4K, 8K, wide 2560x1080) the game draws the largest size
// that fits, divided by a whole number, and the emulated GPU draws every one
// of its pixels as 2x2, 3x3 ... real pixels (draw_resolution_scale). That is
// a real render at the higher resolution, triangle by triangle, not a small
// picture stretched afterwards; what stays at the smaller size is what the
// game computes on its side: the effects' half-size buffers and where 2D is
// placed. Native never turns on the game's MSAA: 1440p = 1280x720 x2, 4K =
// 1920x1080 x2, 8K = 1920x1080 x4, and on Direct3D 12 1080p = 960x540 x2
// (1920x1080 needs more than the console's EDRAM).
//
// Scaled (the older plan, for comparison): the game draws at most 1280x720
// (720 lines when wider than 16:9) with its own 2x MSAA where it fits, and the
// GPU multiplies it: 1080p = 960x540 x2 with MSAA.
//
// Nothing is drawn bigger than the window and then shrunk: a choice larger
// than the window or screen is drawn at the window's size. A resolution
// percentage below 100 (cod3_render_percent) draws fewer pixels and the
// presenter stretches them to the window, for speed; that is the only case
// where the picture is scaled, and F2 and the log say so.

#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

namespace cod3 {

// The Xbox 360's EDRAM holds what the game draws into: 2048 tiles of 80x16
// samples (10 MB). The game's main color and depth surfaces (32 bits each)
// sit one after the other, so both must fit, and the depth surface must start
// below tile 2048 (the registers have 11 bits for it). With SDK patch 0010 the
// emulated EDRAM has 4096 tiles on Vulkan, which lets the game draw 1920x1080
// itself, or 1280x720 with its 2x MSAA; on D3D12 it stays at 2048.
inline constexpr uint32_t kConsoleEdramTiles = 2048;

// How the game reaches the resolution (cod3_render_method, see above).
enum class RenderMethod {
  kNative,  // the game draws it itself, no MSAA; the GPU's scale only above the EDRAM
  kScaled,  // the older plan: at most 1280x720 with the game's MSAA, multiplied
};
RenderMethod CurrentRenderMethod();  // from cod3_render_method
const char* RenderMethodName(RenderMethod method);  // "native" / "scaled"

struct RenderPlan {
  std::string choice;          // "auto", "720p", "1080p", "1440p", "2160p", "4320p", "WxH"
  RenderMethod method = RenderMethod::kNative;
  uint32_t display_width = 0;  // where the presenter shows the picture: the
  uint32_t display_height = 0; // window or screen fitted to the picture's shape
  uint32_t screen_width = 0;   // the size asked for (after the shape and the
  uint32_t screen_height = 0;  // window limit), before the percentage
  uint32_t percent = 100;      // cod3_render_percent
  uint32_t game_width = 0;     // what the game itself draws
  uint32_t game_height = 0;
  uint32_t scale = 1;          // the emulated GPU's whole-number scale
  bool msaa = true;            // the game's own 2x MSAA
  uint32_t edram_tiles = kConsoleEdramTiles;  // what the GPU emulation offers
  std::string note;            // why the plan isn't native, or was limited ("" = none)
  uint32_t output_width() const { return game_width * scale; }
  uint32_t output_height() const { return game_height * scale; }
  bool exact() const { return output_width() == screen_width && output_height() == screen_height; }
  // The game draws every pixel of the picture on screen itself.
  bool native() const {
    return scale == 1 && !msaa && output_width() == display_width && output_height() == display_height;
  }
};

// The older plan's limit (RenderMethod::kScaled): an earlier test at
// 1920x1080 showed the title as a 1280x720 picture in the middle, so that
// plan draws at most 1280x720 and the GPU multiplies it. Retested on
// 2026-10-13 with the game drawing 1920x1080 itself: the title, the menus and
// the 3D view fill the whole picture, laid out like at 1280x720, so native
// has no such limit. A picture wider than 16:9 keeps the HUD and menus in
// its centered 16:9 part (game/ultrawide.h), so there the limit is the
// height: up to 720 lines and as wide as the EDRAM allows.
inline constexpr uint32_t kMaxGameWidth = 1280;
inline constexpr uint32_t kMaxGameHeight = 720;
inline constexpr uint32_t kMaxWideGameWidth = 4096;

// Custom sizes ("WxH", r_mode): the smallest the game is drawn at, and up to
// 8K (32:9 at 4320 lines). The setup tool and the console's r_mode use the
// same limits.
inline constexpr uint32_t kMinCustomWidth = 640;
inline constexpr uint32_t kMinCustomHeight = 360;
inline constexpr uint32_t kMaxCustomWidth = 15360;
inline constexpr uint32_t kMaxCustomHeight = 4320;
// cod3_render_percent: the share of the resolution drawn (100 = all of it).
inline constexpr uint32_t kMinRenderPercent = 25;

// Tiles one 32-bit surface takes (2x MSAA doubles the rows).
uint32_t SurfaceTiles(uint32_t width, uint32_t height, bool msaa);
bool FitsEdram(uint32_t width, uint32_t height, bool msaa, uint32_t edram_tiles);
// The emulated GPU's limit for its scale.
inline constexpr uint32_t kMaxScale = 7;

// Guest memory the game's screen-sized buffers take at this size, in bytes
// (32 bits a pixel, the GPU's 32x32 tiles and 4 KB pages): the two front
// buffers, the back buffer copy, the full-size depth texture, the two
// quarter-size targets and, in a level, the effects' half-size color and
// depth. About 12 MB at the console's 1040x624, 37 MB at 1920x1080.
uint32_t ScreenBufferBytes(uint32_t width, uint32_t height);

// How to draw `width` x `height` with `method`: native draws it at scale 1
// without MSAA when it fits the EDRAM; otherwise (and always with kScaled)
// the smallest whole-number scale whose game resolution fits (exact when the
// size divides evenly; otherwise the picture is a few pixels smaller). Only
// kScaled adds the game's 2x MSAA, where it fits too; native has none.
RenderPlan PlanRenderResolution(uint32_t width, uint32_t height, uint32_t edram_tiles,
                                RenderMethod method = RenderMethod::kScaled);

// The largest 16:9 size inside width x height (the game's picture is 16:9).
void FitSixteenByNine(uint32_t* width, uint32_t* height);
// The largest size of the given shape (width / height, 16:9 or wider; see
// game/ultrawide.h) inside width x height. A size within 1% of the shape
// stays as it is (2560x1080 is 2.37:1, "21:9" 2.33:1).
void FitAspect(uint32_t* width, uint32_t* height, float aspect);

// The resolution choices, in the order F2 > Video, the game's Options > Video
// RESOLUTION row and the setup tool list them (keep the three the same).
struct ResolutionChoice {
  const char* value;  // cod3_render_resolution
  const char* name;   // F2 and the setup tool
  const char* text;   // the game's menu row (its font has capitals)
};
inline constexpr ResolutionChoice kResolutionChoices[] = {
    {"auto", "Match the screen (native)", "AUTO"},
    {"720p", "720p (1280x720)", "1280 X 720"},
    {"1080p", "1080p (1920x1080)", "1920 X 1080"},
    {"1440p", "1440p (2560x1440)", "2560 X 1440"},
    {"2160p", "2160p / 4K (3840x2160)", "3840 X 2160"},
    {"4320p", "4320p / 8K (7680x4320)", "7680 X 4320"},
    {"original", "Xbox 360 original (1040x624, stretched)", "XBOX 360"},
};

// "720p", "1080p", "1440p", "2160p"/"4k", "4320p"/"8k" or "WxH" (within the
// custom limits above); false for "auto", "original", "" or anything else.
bool ParseResolutionChoice(const std::string& choice, uint32_t* width, uint32_t* height);

// The primary display's current mode, in physical pixels.
bool GetScreenSize(uint32_t* width, uint32_t* height);

// 4096 with the larger emulated EDRAM (Vulkan, SDK patch 0010), else 2048.
void SetEdramTilesAvailable(uint32_t tiles);
uint32_t EdramTilesAvailable();

// Set at start, before the GPU starts; the hooks apply it when the game picks
// its video mode. A live switch (below) replaces it once the switch is done,
// from the game thread, so it is handed out as a copy.
void SetRenderPlan(const RenderPlan& plan);
RenderPlan GetRenderPlan();

// What is drawn, in plain words: "1920x1080 itself, no MSAA, no scaling",
// "960x540 with 2x MSAA, drawn 2x by the GPU = 1920x1080", "1440x810
// itself, no MSAA (75%), stretched to 1920x1080 on screen".
std::string DescribePlan(const RenderPlan& plan);

// ---------------------------------------------------------------------------
// Changing the resolution while the game runs
// ---------------------------------------------------------------------------
// The game makes its screen-sized render targets once, from the video mode
// it picked at start, and has no vid_restart. But R_SetMultisample, which
// main() calls once, carries the whole rebuild: it stores the MSAA type in the
// present parameters, resets the D3D device and remakes the screen targets
// from the video mode and the present parameters. So a switch writes the new
// size into both, calls R_SetMultisample, and remakes by hand the few targets
// made elsewhere (the full-size depth texture, the effects' half-size color
// and depth). It runs on the game thread at a frame boundary (the Com_Frame
// hook), after the render job and the GPU have finished the last frame, and
// only when nothing is bound or being recorded; otherwise it waits for the
// next frame. The emulated GPU's scale follows through the live
// draw_resolution_scale settings (SDK patch 0014). Addresses and the
// reasoning: game/addresses.h (namespace live_video).

// The plan for a choice ("auto", "720p", "WxH", "original" ...) shown in a
// window or screen of view_width x view_height, with the picture shape from
// cod3_aspect_ratio, the method from cod3_render_method and the share from
// cod3_render_percent: what the start uses (Cod3App::ApplyRenderResolution)
// and what a live change switches to. A choice bigger than the view is drawn
// at the view's size (note says so). "original" gives game_width 0 (the
// game's own table entry) at scale 1, stretched to the view.
RenderPlan PlanForView(const std::string& choice, uint32_t view_width, uint32_t view_height);

// UI thread, every frame (game/ultrawide.cpp), with the window's size in
// pixels: when the size or the picture's shape changed, or a setting the plan
// depends on did, the new plan is asked for (RequestRenderResolution). The
// first call only notes the size the start planned for.
void NoteWindowSize(uint32_t width, uint32_t height);

// Plans the resolution the settings ask for (cod3_render_resolution,
// cod3_render_method, cod3_render_percent, cod3_aspect_ratio,
// cod3_game_width/height/mode/msaa) for the last noted window, like the plan
// at start, and asks the game thread to switch to it. A request is applied
// once it has stayed the same for a moment (a window being dragged), at the
// next frame boundary. Nothing happens when the plan is the current one.
void RequestRenderResolution();

// Game thread, at a frame boundary (the Com_Frame hook): switch the game to
// width x height, with its 2x MSAA when `msaa` (and it fits the EDRAM).
// Returns true when the game draws at that size now; false when a
// precondition failed (logged), with the game left as it was. The size must
// fit the emulated EDRAM like at start (FitsEdram) and stay within
// kMaxWideGameWidth x 4096.
bool ApplyRenderResolutionNow(PPCContext& ctx, uint8_t* base, uint32_t width, uint32_t height,
                              bool msaa);

// For the settings window: "" when the game draws what the settings ask for,
// else what is happening ("Switching ..." or why it couldn't, which then
// applies at the next start).
std::string RenderResolutionNote();

}  // namespace cod3
