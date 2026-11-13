// f2 - the look of the F2 settings window, shared by every game's port. [f2-menus]
//
// Owner 2026-11-09 20:05 (the F2 plan's first question, dev/agents/acct2-bro/F2-MENU-PLAN.md): "CoD3's window, own
// colors". So every game draws Call of Duty 3's window (the same shapes, spacing, fonts and rules as
// recomp/src/ui/theme.cpp) and starts in colors taken from its own menus. Call of Duty 3's four themes stay
// selectable in every game.
//
// Each game tells the shell, once at start (SetLook), where its three look settings live (its own names, such as
// waw_ui_theme, saved in its own settings file) and what its own colors are.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <imgui.h>

namespace f2 {

// Everything a theme decides; the rest of the style follows from these (as Call of Duty 3's themes).
struct ThemeColors {
  ImVec4 window;        // window background (its alpha comes from the opacity setting)
  ImVec4 popup;         // combo lists, tooltips, dialogs
  ImVec4 frame;         // check boxes, sliders, combo boxes
  ImVec4 frame_hover;
  ImVec4 frame_active;
  ImVec4 button;
  ImVec4 button_hover;
  ImVec4 button_active;
  ImVec4 title;         // title bar
  ImVec4 title_active;
  ImVec4 border;
  ImVec4 text;
  ImVec4 text_dim;      // descriptions, disabled options
  ImVec4 accent;        // check marks, slider grabs, the selected tab's line
  ImVec4 accent_text;   // accent-colored text on the window (headings)
  ImVec4 row_alt;       // every other table row
  ImVec4 good, warn, bad;
};

struct Theme {
  const char* id;           // the theme setting's value
  const char* label;        // shown on the INTERFACE page
  const char* description;  // shown under it
  const ThemeColors* colors;
};

// Where a game keeps its look settings: each one's name (for SetFlagByName, so a change is saved like any other)
// and its storage (REXCVAR_DECLARE's function, read every frame without copying).
struct LookSettings {
  const char* theme_name = nullptr;
  std::string& (*theme)() = nullptr;
  const char* text_size_name = nullptr;
  int32_t& (*text_size)() = nullptr;
  const char* opacity_name = nullptr;
  double& (*opacity)() = nullptr;
  Theme own{};  // the game's own colors: listed first, and the default
};

// Once, before the fonts are made (the game's OnConfigureFonts).
void SetLook(const LookSettings& look);
const LookSettings& Look();

// The themes in the order the INTERFACE page lists them: the game's own, then Call of Duty 3's four.
const Theme* Themes(size_t* count);
const Theme& CurrentTheme();
const ThemeColors& CurrentColors();

// ReXApp::OnConfigureFonts: Barlow Semi Condensed (as Call of Duty 3's window), the SDK's small font kept for
// fixed-width text.
void ConfigureFonts(ImFontAtlas* atlas);
ImFont* BoldFont();
ImFont* MonoFont();

// ReXApp::OnConfigureStyle, and again whenever a look setting changes.
void ApplyTheme(ImGuiStyle& style);
// At the start of every window that uses the look: reapplies it when a look setting changed.
void ApplyThemeIfChanged();

// A section heading: the bold font in the accent color, with a rule (Call of Duty 3's Section).
void Section(const char* title);

}  // namespace f2
