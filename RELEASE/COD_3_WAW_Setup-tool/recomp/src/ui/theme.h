// cod3 - the look of the port's own windows (F2 settings, console, HUD text).
//
// The SDK's default ImGui style is Xenia's bright green debug look. These
// themes take their colors from Call of Duty 3's own screens instead (sampled
// from the title and loading screens): near-black warm backgrounds, the
// brass gold of the "Loading ..." text and the title divider, and parchment
// text. The other themes are variations on the same palette.
//
// The text is Barlow Semi Condensed (SIL Open Font License, ui/fonts/), a
// condensed sans close to the game's menu lettering.
//
// Settings (cvars, saved with the others): cod3_ui_theme, cod3_ui_text_size,
// cod3_ui_opacity. They apply at once.

#pragma once

#include <cstddef>

#include <imgui.h>

namespace cod3::ui {

struct ThemeInfo {
  const char* id;           // the cod3_ui_theme value
  const char* label;        // shown in F2
  const char* description;  // shown under it
};

// The themes, in the order F2 lists them. The first is the default.
const ThemeInfo* Themes(size_t* count);

// Colors that aren't ImGui style colors, from the current theme.
struct Palette {
  ImVec4 accent;       // headings, the selected tab's line, "Restart ..." notes
  ImVec4 accent_text;  // text in the accent color, readable on the window
  ImVec4 muted;        // descriptions under options
  ImVec4 good, warn, bad;
};
const Palette& CurrentPalette();

// ReXApp::OnConfigureFonts: adds the theme's fonts and makes the regular one
// the default. The SDK's own small font stays available (MonoFont).
void ConfigureFonts(ImFontAtlas* atlas);
ImFont* BoldFont();  // headings and tabs (the regular font when missing)
ImFont* MonoFont();  // the SDK's ProggyTiny, for fixed-width text
// The regular font baked closest to `size` px (HUD text drawn at a size).
ImFont* FontFor(float size);

// ReXApp::OnConfigureStyle, and again whenever a ui cvar changes.
void ApplyTheme(ImGuiStyle& style);
// Call at the start of a window's drawing: reapplies the theme to the live
// style if a ui cvar changed since it was last applied.
void ApplyThemeIfChanged();

// A section heading in F2: the bold font in the accent color, with a rule.
void Section(const char* title);

}  // namespace cod3::ui
