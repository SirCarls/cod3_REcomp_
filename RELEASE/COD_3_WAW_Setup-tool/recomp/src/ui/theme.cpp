// cod3 - themes and fonts for the port's own windows (see theme.h).

#include "ui/theme.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(cod3_ui_theme, "cod3", "cod3/Interface",
                      "Colors of the F2 settings and the console: cod3 (the game's black, brass "
                      "and parchment), olive (field olive drab), parchment (light, like a "
                      "briefing map) or contrast (black and white, gold highlights)");
REXCVAR_DEFINE_INT32(cod3_ui_text_size, 16, "cod3/Interface",
                     "Text size of the F2 settings and the console, in pixels (14-24)")
    .range(14, 24);
REXCVAR_DEFINE_DOUBLE(cod3_ui_opacity, 0.94, "cod3/Interface",
                      "How solid the F2 settings window is (0.6 = see the game through it, 1 = "
                      "solid)")
    .range(0.6, 1.0);

// Barlow Semi Condensed, Medium and SemiBold (SIL OFL 1.1, ui/fonts/OFL.txt).
#include "ui/fonts/barlow_medium.inc"
#include "ui/fonts/barlow_semibold.inc"

namespace cod3::ui {

namespace {

constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
  return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

// Everything a theme decides. The others follow from these.
struct Colors {
  ImVec4 window;        // window background (its alpha comes from cod3_ui_opacity)
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
  ImVec4 accent;        // brass gold: check marks, slider grabs, the selected tab's line
  ImVec4 accent_text;   // accent-colored text on the window
  ImVec4 row_alt;       // every other table row
  Palette palette;
};

// Sampled from the game: the title screen's divider (107, 85, 24), the
// "Loading ..." text (168, 130, 78), the "3" emblem's gold (194, 135, 57),
// "PRESS START" (179, 162, 137), and the near-black warm backgrounds
// (6, 4, 3). Brightened where text must stay readable on a monitor.
const Colors kCod3 = {
    Rgb(16, 13, 9),    Rgb(22, 18, 13, 0.98f), Rgb(44, 37, 27),  Rgb(64, 53, 36),
    Rgb(82, 66, 40),   Rgb(56, 46, 31),        Rgb(98, 78, 46),  Rgb(140, 108, 60),
    Rgb(20, 16, 11),   Rgb(38, 30, 19),        Rgb(107, 85, 24, 0.75f),
    Rgb(226, 214, 190), Rgb(150, 136, 112),    Rgb(196, 152, 84), Rgb(214, 172, 104),
    Rgb(255, 236, 200, 0.03f),
    {Rgb(196, 152, 84), Rgb(214, 172, 104), Rgb(150, 136, 112), Rgb(132, 186, 96),
     Rgb(222, 178, 84), Rgb(214, 92, 70)}};

// US Army olive drab panels with khaki text and brass.
const Colors kOlive = {
    Rgb(25, 27, 18),   Rgb(31, 33, 22, 0.98f), Rgb(52, 56, 36),  Rgb(70, 75, 47),
    Rgb(88, 93, 57),   Rgb(62, 66, 41),        Rgb(86, 91, 56),  Rgb(128, 122, 76),
    Rgb(30, 32, 21),   Rgb(52, 56, 35),        Rgb(110, 112, 70, 0.8f),
    Rgb(226, 220, 196), Rgb(150, 148, 120),    Rgb(196, 166, 96), Rgb(212, 186, 120),
    Rgb(255, 255, 220, 0.03f),
    {Rgb(196, 166, 96), Rgb(212, 186, 120), Rgb(150, 148, 120), Rgb(140, 192, 100),
     Rgb(226, 190, 90), Rgb(214, 96, 72)}};

// Light: aged paper with dark brown ink and deep brass, like a briefing map.
const Colors kParchment = {
    Rgb(226, 214, 186), Rgb(232, 222, 198, 0.99f), Rgb(204, 189, 158), Rgb(192, 174, 138),
    Rgb(178, 158, 118), Rgb(196, 178, 140),        Rgb(180, 158, 116), Rgb(150, 120, 70),
    Rgb(200, 186, 152), Rgb(176, 156, 116),        Rgb(140, 112, 64, 0.8f),
    Rgb(44, 32, 18),    Rgb(106, 90, 66),          Rgb(128, 90, 30),   Rgb(110, 74, 20),
    Rgb(80, 50, 10, 0.05f),
    {Rgb(128, 90, 30), Rgb(110, 74, 20), Rgb(106, 90, 66), Rgb(48, 112, 40), Rgb(150, 96, 0),
     Rgb(170, 40, 30)}};

// Black and white for the easiest reading, gold highlights.
const Colors kContrast = {
    Rgb(0, 0, 0),       Rgb(8, 8, 8, 1.0f),  Rgb(36, 36, 36),   Rgb(60, 60, 60),
    Rgb(84, 84, 84),    Rgb(44, 44, 44),     Rgb(76, 76, 76),   Rgb(150, 116, 40),
    Rgb(0, 0, 0),       Rgb(28, 28, 28),     Rgb(210, 170, 70, 0.9f),
    Rgb(255, 255, 255), Rgb(190, 190, 190),  Rgb(255, 200, 80), Rgb(255, 210, 100),
    Rgb(255, 255, 255, 0.05f),
    {Rgb(255, 200, 80), Rgb(255, 210, 100), Rgb(190, 190, 190), Rgb(120, 230, 120),
     Rgb(255, 210, 60), Rgb(255, 100, 90)}};

const ThemeInfo kThemes[] = {
    {"cod3", "Call of Duty 3", "The game's own colors: black, brass gold and parchment."},
    {"olive", "Field olive", "Olive drab panels with khaki text and brass."},
    {"parchment", "Briefing paper", "Light: aged paper and dark ink."},
    {"contrast", "High contrast", "Black and white with gold highlights, for the easiest reading."},
};
const Colors* const kThemeColors[] = {&kCod3, &kOlive, &kParchment, &kContrast};

const Colors& CurrentColors() {
  const std::string id = REXCVAR_GET(cod3_ui_theme);
  for (size_t i = 0; i < std::size(kThemes); ++i)
    if (id == kThemes[i].id) return *kThemeColors[i];
  return kCod3;
}

ImVec4 WithAlpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// The fonts, baked at the sizes the text size setting offers (the SDK's
// renderer uploads the font atlas once, so sizes can't be added later).
// HUD text uses the nearest one (FontFor).
constexpr float kTextSizes[] = {14, 16, 18, 20, 24};
constexpr float kHudSizes[] = {32, 48};
ImFont* g_regular[std::size(kTextSizes) + std::size(kHudSizes)] = {};
ImFont* g_bold[std::size(kTextSizes)] = {};
ImFont* g_mono = nullptr;
std::string g_applied;  // the ui cvars the live style was made from

int TextSizeIndex() {
  const float want = float(REXCVAR_GET(cod3_ui_text_size));
  int best = 0;
  for (int i = 0; i < int(std::size(kTextSizes)); ++i)
    if (std::fabs(kTextSizes[i] - want) < std::fabs(kTextSizes[best] - want)) best = i;
  return best;
}

std::string AppliedKey() {
  return std::string(REXCVAR_GET(cod3_ui_theme)) + "|" + std::to_string(REXCVAR_GET(cod3_ui_text_size)) +
         "|" + std::to_string(REXCVAR_GET(cod3_ui_opacity));
}

}  // namespace

const ThemeInfo* Themes(size_t* count) {
  *count = std::size(kThemes);
  return kThemes;
}

const Palette& CurrentPalette() { return CurrentColors().palette; }

void ConfigureFonts(ImFontAtlas* atlas) {
  // The SDK added its ProggyTiny first; it stays as the fixed-width font.
  g_mono = atlas->Fonts.empty() ? nullptr : atlas->Fonts[0];
  static const ImWchar kRanges[] = {
      0x0020, 0x00FF,  // Basic Latin + Latin-1 (accents: Saint-Lô)
      0x2010, 0x2027,  // dashes, quotes, bullet, ellipsis
      0x2030, 0x203A,  // per mille, angle quotes
      0x20AC, 0x20AC,  // euro
      0,
  };
  auto add = [&](const char* data, float size) {
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    config.PixelSnapH = true;
    return atlas->AddFontFromMemoryCompressedBase85TTF(data, size, &config, kRanges);
  };
  size_t n = 0;
  for (float size : kTextSizes) g_regular[n++] = add(BarlowMedium_compressed_data_base85, size);
  for (float size : kHudSizes) g_regular[n++] = add(BarlowMedium_compressed_data_base85, size);
  for (size_t i = 0; i < std::size(kTextSizes); ++i)
    g_bold[i] = add(BarlowSemiBold_compressed_data_base85, kTextSizes[i]);
  ImGui::GetIO().FontDefault = g_regular[TextSizeIndex()];
}

ImFont* BoldFont() {
  ImFont* bold = g_bold[TextSizeIndex()];
  return bold ? bold : ImGui::GetFont();
}

ImFont* MonoFont() { return g_mono ? g_mono : ImGui::GetFont(); }

ImFont* FontFor(float size) {
  ImFont* best = nullptr;
  float best_size = 0;
  auto consider = [&](ImFont* f, float s) {
    if (!f) return;
    // The smallest bake at least as big as wanted (scaling down stays sharp),
    // else the biggest there is.
    const bool better = best == nullptr || (best_size < size ? s > best_size : (s >= size && s < best_size));
    if (better) best = f, best_size = s;
  };
  for (size_t i = 0; i < std::size(kTextSizes); ++i) consider(g_regular[i], kTextSizes[i]);
  for (size_t i = 0; i < std::size(kHudSizes); ++i)
    consider(g_regular[std::size(kTextSizes) + i], kHudSizes[i]);
  return best ? best : ImGui::GetFont();
}

void ApplyTheme(ImGuiStyle& style) {
  const Colors& c = CurrentColors();
  const float opacity = float(REXCVAR_GET(cod3_ui_opacity));

  // Shape: square-ish like the game's menus, roomier than the SDK default.
  style.WindowRounding = 3.0f;
  style.ChildRounding = 2.0f;
  style.FrameRounding = 2.0f;
  style.PopupRounding = 2.0f;
  style.GrabRounding = 2.0f;
  style.TabRounding = 2.0f;
  style.ScrollbarRounding = 2.0f;
  style.WindowBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.PopupBorderSize = 1.0f;
  style.TabBarBorderSize = 1.0f;
  style.TabBarOverlineSize = 2.0f;
  style.SeparatorTextBorderSize = 1.0f;
  style.SeparatorTextPadding = ImVec2(0.0f, 4.0f);
  style.WindowPadding = ImVec2(16.0f, 12.0f);
  style.FramePadding = ImVec2(8.0f, 4.0f);
  style.ItemSpacing = ImVec2(10.0f, 6.0f);
  style.ItemInnerSpacing = ImVec2(8.0f, 4.0f);
  style.CellPadding = ImVec2(6.0f, 3.0f);
  style.IndentSpacing = 18.0f;
  style.ScrollbarSize = 12.0f;
  style.GrabMinSize = 12.0f;
  style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  style.DisabledAlpha = 0.5f;

  ImVec4* col = style.Colors;
  col[ImGuiCol_Text] = c.text;
  col[ImGuiCol_TextDisabled] = c.text_dim;
  col[ImGuiCol_WindowBg] = WithAlpha(c.window, opacity);
  col[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  col[ImGuiCol_PopupBg] = c.popup;
  col[ImGuiCol_Border] = c.border;
  col[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  col[ImGuiCol_FrameBg] = c.frame;
  col[ImGuiCol_FrameBgHovered] = c.frame_hover;
  col[ImGuiCol_FrameBgActive] = c.frame_active;
  col[ImGuiCol_TitleBg] = c.title;
  col[ImGuiCol_TitleBgActive] = c.title_active;
  col[ImGuiCol_TitleBgCollapsed] = c.title;
  col[ImGuiCol_MenuBarBg] = c.title;
  col[ImGuiCol_ScrollbarBg] = WithAlpha(c.window, 0.5f);
  col[ImGuiCol_ScrollbarGrab] = c.button;
  col[ImGuiCol_ScrollbarGrabHovered] = c.button_hover;
  col[ImGuiCol_ScrollbarGrabActive] = c.accent;
  col[ImGuiCol_CheckMark] = c.accent_text;
  col[ImGuiCol_SliderGrab] = c.accent;
  col[ImGuiCol_SliderGrabActive] = c.accent_text;
  col[ImGuiCol_Button] = c.button;
  col[ImGuiCol_ButtonHovered] = c.button_hover;
  col[ImGuiCol_ButtonActive] = c.button_active;
  col[ImGuiCol_Header] = WithAlpha(c.accent, 0.30f);
  col[ImGuiCol_HeaderHovered] = WithAlpha(c.accent, 0.45f);
  col[ImGuiCol_HeaderActive] = WithAlpha(c.accent, 0.60f);
  col[ImGuiCol_Separator] = c.border;
  col[ImGuiCol_SeparatorHovered] = c.accent;
  col[ImGuiCol_SeparatorActive] = c.accent_text;
  col[ImGuiCol_ResizeGrip] = WithAlpha(c.accent, 0.25f);
  col[ImGuiCol_ResizeGripHovered] = WithAlpha(c.accent, 0.6f);
  col[ImGuiCol_ResizeGripActive] = c.accent_text;
  col[ImGuiCol_InputTextCursor] = c.text;
  col[ImGuiCol_Tab] = c.frame;
  col[ImGuiCol_TabHovered] = c.button_hover;
  col[ImGuiCol_TabSelected] = c.frame_active;
  col[ImGuiCol_TabSelectedOverline] = c.accent_text;
  col[ImGuiCol_TabDimmed] = c.frame;
  col[ImGuiCol_TabDimmedSelected] = c.frame_hover;
  col[ImGuiCol_TabDimmedSelectedOverline] = c.accent;
  col[ImGuiCol_PlotLines] = c.accent;
  col[ImGuiCol_PlotLinesHovered] = c.accent_text;
  col[ImGuiCol_PlotHistogram] = c.accent;
  col[ImGuiCol_PlotHistogramHovered] = c.accent_text;
  col[ImGuiCol_TableHeaderBg] = c.title_active;
  col[ImGuiCol_TableBorderStrong] = c.border;
  col[ImGuiCol_TableBorderLight] = WithAlpha(c.border, 0.4f);
  col[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  col[ImGuiCol_TableRowBgAlt] = c.row_alt;
  col[ImGuiCol_TextLink] = c.accent_text;
  col[ImGuiCol_TextSelectedBg] = WithAlpha(c.accent, 0.35f);
  col[ImGuiCol_TreeLines] = c.border;
  col[ImGuiCol_DragDropTarget] = c.accent_text;
  col[ImGuiCol_DragDropTargetBg] = WithAlpha(c.accent, 0.15f);
  col[ImGuiCol_UnsavedMarker] = c.accent_text;
  col[ImGuiCol_NavCursor] = c.accent_text;
  col[ImGuiCol_NavWindowingHighlight] = WithAlpha(c.accent_text, 0.7f);
  col[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.4f);
  col[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);

  // Text size: the bake for it becomes the default font, at its own size
  // (from the next frame; ImGui keeps the size it started a frame with).
  const int i = TextSizeIndex();
  if (g_regular[i]) ImGui::GetIO().FontDefault = g_regular[i];
  style.FontSizeBase = kTextSizes[i];
  style._NextFrameFontSizeBase = kTextSizes[i];
  g_applied = AppliedKey();
}

void ApplyThemeIfChanged() {
  if (AppliedKey() != g_applied) ApplyTheme(ImGui::GetStyle());
}

void Section(const char* title) {
  ImGui::Spacing();
  ImGui::PushFont(BoldFont(), 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().accent_text);
  ImGui::SeparatorText(title);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

}  // namespace cod3::ui
