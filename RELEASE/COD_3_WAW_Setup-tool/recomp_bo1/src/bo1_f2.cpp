// bo1 / bo1mp - Black Ops' F2 settings window (see bo1_f2.h). [menus-native]
//
// Every row edits a setting the port or the SDK already has; the only new ones are the window's own look and the
// controller shortcut's switch. Laid out as Call of Duty 4's window (../recomp_cod4/src/cod4_f2.cpp), without the
// rows that need the game's console or hooks in its code.

#include "bo1_f2.h"

#include <memory>
#include <string>
#include <utility>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/style.h>

#include "f2/f2_common.h"
#include "f2/f2_rows.h"
#include "f2/f2_theme.h"
#include "f2/f2_window.h"

REXCVAR_DEFINE_STRING(bo1_ui_theme, "bo1", "bo1/Interface",
                      "Look of the F2 settings window: bo1 (Black Ops' own), or one of the shared themes");
REXCVAR_DEFINE_INT32(bo1_ui_text_size, 16, "bo1/Interface", "Text size of the F2 settings window, in pixels (14-24)")
    .range(14, 24);
REXCVAR_DEFINE_DOUBLE(bo1_ui_opacity, 0.94, "bo1/Interface",
                      "Opacity of the F2 settings window's background (0.6-1)")
    .range(0.6, 1.0);
REXCVAR_DEFINE_BOOL(bo1_f2_pad_shortcut, true, "bo1/Interface",
                    "Open the F2 settings window with Back + Start held for 1 second on a controller");

namespace bo1::f2ui {

namespace {

// Black Ops' window colors until its disc's menus are counted (as dev/agents/f2-menus/tools/ff_colors.py did for
// Call of Duty 4): Call of Duty 4's black panels and white text, with an amber accent.
constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
  return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
const f2::ThemeColors kBo1Colors = {
    Rgb(8, 9, 10),          // window
    Rgb(14, 15, 17, 0.98f),
    Rgb(28, 29, 32),        // frame
    Rgb(57, 61, 64),        //   hovered
    Rgb(80, 85, 89),        //   pressed
    Rgb(40, 42, 45),        // button
    Rgb(57, 61, 64),
    Rgb(102, 86, 52),       //   pressed: the accent at 0.4 over black
    Rgb(0, 0, 0),           // title
    Rgb(20, 21, 23),
    Rgb(57, 57, 64, 0.8f),  // border
    Rgb(255, 255, 255),     // text
    Rgb(176, 176, 176),     // descriptions
    Rgb(255, 176, 64),      // accent: amber
    Rgb(255, 204, 102),     // headings and check marks
    Rgb(230, 230, 255, 0.07f),  // every other row
    Rgb(132, 200, 110),     // good
    Rgb(255, 204, 102),     // warn
    Rgb(230, 72, 56),       // bad
};

std::unique_ptr<f2::Overlay> g_overlay;

using f2::Applies;
using f2::Help;
using f2::PageContext;
using f2::Section;

// ------------------------------------------------------------------------------------------------- VIDEO
void DrawVideo(PageContext& c) {
  if (f2::ResetButton("reset_video", "video settings")) {
    c.restart |= f2::ResetSettings({"fullscreen", "vsync", "bo1_graphics_api"});
    for (const char* name : f2::kPictureSettings)
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    for (const char* name : f2::kResolutionSettings) c.restart |= f2::ResetSettings({name});
    c.dirty = true;
  }
  Section("Display");
  {
    const char* v[] = {"false", "true"};
    const char* n[] = {"Windowed", "Fullscreen (borderless)"};
    c.dirty |= f2::Choice("Display mode", "fullscreen", v, n, 2, Applies::kAtOnce, &c.restart);
    Help("Play in a window, or fill the whole monitor without changing its resolution (borderless).");
  }
  f2::DrawDisplayRows(c);  // Call of Duty 3's Display and Refresh rate rows
  c.dirty |= f2::Check("V-Sync", "vsync", Applies::kAfterRestart, &c.restart);
  Help("Waits for the monitor between pictures: no tearing, but no more frames than the monitor's refresh rate. "
       "Applies after a restart.");
#ifdef _WIN32
  {
    const char* v[] = {"auto", "vulkan", "d3d12"};
    const char* n[] = {"Automatic (Vulkan, else Direct3D 12)", "Vulkan", "Direct3D 12"};
    c.dirty |= f2::Choice("Graphics API", "bo1_graphics_api", v, n, 3, Applies::kAfterRestart, &c.restart);
    Help("Which graphics API draws the game. Applies after a restart.");
  }
#endif
  f2::DrawResolutionSection(c);  // render scale, picture shape, window size (the same in every game)
  Section("Picture");
  f2::DrawPictureSection(c);  // post-process AA, output scaling and its sharpness, anisotropic filtering, HDR
}

// ------------------------------------------------------------------------------------------------- GAME
void DrawGame(PageContext& c) {
  if (f2::ResetButton("reset_game", "game settings")) {
    for (const char* name : f2::kPlayerSettings)
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    c.dirty = true;
  }
  f2::DrawPlayerSection(c);  // the player's name, split-screen names, which player each controller is, language
}

// ------------------------------------------------------------------------------------------------- CONTROLS
void DrawControls(PageContext& c) { f2::DrawControlsPage(c, "bo1_f2_pad_shortcut", {}, {}); }

// ------------------------------------------------------------------------------------------------- PERFORMANCE, EMULATION
void DrawPerformance(PageContext& c) { f2::DrawPerformanceRows(c); }
void DrawEmulation(PageContext& c) { f2::DrawEmulationPage(c, nullptr, {}); }

}  // namespace

void ConfigureFonts(ImFontAtlas* atlas) {
  f2::LookSettings look;
  look.theme_name = "bo1_ui_theme";
  look.theme = &FLAGS_bo1_ui_theme_storage_;
  look.text_size_name = "bo1_ui_text_size";
  look.text_size = &FLAGS_bo1_ui_text_size_storage_;
  look.opacity_name = "bo1_ui_opacity";
  look.opacity = &FLAGS_bo1_ui_opacity_storage_;
  look.own = {"bo1", "Black Ops", "Black panels, white text, amber highlights.", &kBo1Colors};
  f2::SetLook(look);
  f2::ConfigureFonts(atlas);
  REXLOG_INFO("f2: look {} (text {} px, opacity {:.2f})", f2::CurrentTheme().id, REXCVAR_GET(bo1_ui_text_size),
              REXCVAR_GET(bo1_ui_opacity));
}

void ConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
  f2::ApplyTheme(imgui_style);
  const f2::ThemeColors& c = f2::CurrentColors();
  ui_style.settings.warning = c.warn;
  ui_style.console.completion_highlight = ImVec4(c.accent.x, c.accent.y, c.accent.z, 0.45f);
}

void Attach(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, std::filesystem::path config_path,
            std::function<void(std::function<void()>)> defer) {
  f2::Overlay::Options options;
  options.spec.title = "CALL OF DUTY: BLACK OPS";
#ifdef BO1_MP
  options.spec.id = "bo1mp_settings";
#else
  options.spec.id = "bo1_settings";
#endif
  options.spec.pages = {
      {"VIDEO", DrawVideo},         {"SOUND", f2::DrawSoundPage},         {"GAME", DrawGame},
      {"CONTROLS", DrawControls},   {"INTERFACE", f2::DrawInterfacePage}, {"PERFORMANCE", DrawPerformance},
      {"EMULATION", DrawEmulation},
  };
  options.spec.config_path = std::move(config_path);
  options.pad_shortcut_setting = "bo1_f2_pad_shortcut";
  options.defer = std::move(defer);
  g_overlay = std::make_unique<f2::Overlay>(drawer, window, std::move(options));
  REXLOG_INFO("f2: ready (F2, or Back + Start held 1 s on a controller)");
}

void Detach() { g_overlay.reset(); }

bool GameGetsInput() { return !g_overlay || g_overlay->GameGetsInput(); }

}  // namespace bo1::f2ui
