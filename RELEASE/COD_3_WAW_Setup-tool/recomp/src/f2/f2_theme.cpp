// f2 - the look of the F2 settings window (see f2_theme.h). [f2-menus]
//
// The shapes, spacing and the four shared themes are Call of Duty 3's (recomp/src/ui/theme.cpp, its colors sampled
// from that game's title and loading screens), so every game's window looks and behaves like Call of Duty 3's.

#include "f2/f2_theme.h"

#include <cmath>
#include <iterator>
#include <string_view>

#include <rex/cvar.h>

// Barlow Semi Condensed, Medium and SemiBold (SIL OFL 1.1, ../ui/fonts/OFL.txt), as Call of Duty 3's window.
#include "ui/fonts/barlow_medium.inc"
#include "ui/fonts/barlow_semibold.inc"

namespace f2 {

namespace {

constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
  return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

// Call of Duty 3's four themes, unchanged (recomp/src/ui/theme.cpp).
const ThemeColors kCod3 = {
    Rgb(16, 13, 9),     Rgb(22, 18, 13, 0.98f), Rgb(44, 37, 27),   Rgb(64, 53, 36),
    Rgb(82, 66, 40),    Rgb(56, 46, 31),        Rgb(98, 78, 46),   Rgb(140, 108, 60),
    Rgb(20, 16, 11),    Rgb(38, 30, 19),        Rgb(107, 85, 24, 0.75f),
    Rgb(226, 214, 190), Rgb(150, 136, 112),     Rgb(196, 152, 84), Rgb(214, 172, 104),
    Rgb(255, 236, 200, 0.03f),
    Rgb(132, 186, 96),  Rgb(222, 178, 84),      Rgb(214, 92, 70)};

const ThemeColors kOlive = {
    Rgb(25, 27, 18),    Rgb(31, 33, 22, 0.98f), Rgb(52, 56, 36),   Rgb(70, 75, 47),
    Rgb(88, 93, 57),    Rgb(62, 66, 41),        Rgb(86, 91, 56),   Rgb(128, 122, 76),
    Rgb(30, 32, 21),    Rgb(52, 56, 35),        Rgb(110, 112, 70, 0.8f),
    Rgb(226, 220, 196), Rgb(150, 148, 120),     Rgb(196, 166, 96), Rgb(212, 186, 120),
    Rgb(255, 255, 220, 0.03f),
    Rgb(140, 192, 100), Rgb(226, 190, 90),      Rgb(214, 96, 72)};

const ThemeColors kParchment = {
    Rgb(226, 214, 186), Rgb(232, 222, 198, 0.99f), Rgb(204, 189, 158), Rgb(192, 174, 138),
    Rgb(178, 158, 118), Rgb(196, 178, 140),        Rgb(180, 158, 116), Rgb(150, 120, 70),
    Rgb(200, 186, 152), Rgb(176, 156, 116),        Rgb(140, 112, 64, 0.8f),
    Rgb(44, 32, 18),    Rgb(106, 90, 66),          Rgb(128, 90, 30),   Rgb(110, 74, 20),
    Rgb(80, 50, 10, 0.05f),
    Rgb(48, 112, 40),   Rgb(150, 96, 0),           Rgb(170, 40, 30)};

const ThemeColors kContrast = {
    Rgb(0, 0, 0),       Rgb(8, 8, 8, 1.0f),  Rgb(36, 36, 36),   Rgb(60, 60, 60),
    Rgb(84, 84, 84),    Rgb(44, 44, 44),     Rgb(76, 76, 76),   Rgb(150, 116, 40),
    Rgb(0, 0, 0),       Rgb(28, 28, 28),     Rgb(210, 170, 70, 0.9f),
    Rgb(255, 255, 255), Rgb(190, 190, 190),  Rgb(255, 200, 80), Rgb(255, 210, 100),
    Rgb(255, 255, 255, 0.05f),
    Rgb(120, 230, 120), Rgb(255, 210, 60),   Rgb(255, 100, 90)};

constexpr Theme kShared[] = {
    {"cod3", "Call of Duty 3", "Call of Duty 3's colors: black, brass gold and parchment.", &kCod3},
    {"olive", "Field olive", "Olive drab panels with khaki text and brass.", &kOlive},
    {"parchment", "Briefing paper", "Light: aged paper and dark ink.", &kParchment},
    {"contrast", "High contrast", "Black and white with gold highlights, for the easiest reading.", &kContrast},
};
constexpr size_t kThemeCount = 1 + std::size(kShared);

LookSettings g_look;
Theme g_themes[kThemeCount];  // the game's own first (SetLook)

// The fonts, baked at the sizes the text size setting offers (the SDK's renderer uploads the atlas once).
constexpr float kTextSizes[] = {14, 16, 18, 20, 24};
ImFont* g_regular[std::size(kTextSizes)] = {};
ImFont* g_bold[std::size(kTextSizes)] = {};
ImFont* g_mono = nullptr;

// What the live style was made from; compared every frame without copying.
std::string g_applied_theme;
int32_t g_applied_size = -1;
double g_applied_opacity = -1.0;

int32_t TextSize() { return g_look.text_size ? g_look.text_size() : 16; }
double Opacity() { return g_look.opacity ? g_look.opacity() : 0.94; }

int TextSizeIndex() {
  const float want = float(TextSize());
  int best = 0;
  for (int i = 0; i < int(std::size(kTextSizes)); ++i)
    if (std::fabs(kTextSizes[i] - want) < std::fabs(kTextSizes[best] - want)) best = i;
  return best;
}

ImVec4 WithAlpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }

}  // namespace

void SetLook(const LookSettings& look) {
  g_look = look;
  g_themes[0] = look.own.colors ? look.own : kShared[0];
  for (size_t i = 0; i < std::size(kShared); ++i) g_themes[1 + i] = kShared[i];
}

const LookSettings& Look() { return g_look; }

const Theme* Themes(size_t* count) {
  // Without a game theme (SetLook not called) the list starts with Call of Duty 3's, once.
  if (!g_themes[0].colors) {
    *count = std::size(kShared);
    return kShared;
  }
  // A game whose own theme is Call of Duty 3's lists it once.
  const bool own_is_shared = std::string_view(g_themes[0].id) == kShared[0].id;
  *count = own_is_shared ? std::size(kShared) : kThemeCount;
  return own_is_shared ? g_themes + 1 : g_themes;
}

const Theme& CurrentTheme() {
  size_t count = 0;
  const Theme* themes = Themes(&count);
  if (g_look.theme) {
    const std::string& id = g_look.theme();
    for (size_t i = 0; i < count; ++i)
      if (id == themes[i].id) return themes[i];
  }
  return themes[0];
}

const ThemeColors& CurrentColors() { return *CurrentTheme().colors; }

void ConfigureFonts(ImFontAtlas* atlas) {
  // The SDK added its ProggyTiny first; it stays as the fixed-width font.
  g_mono = atlas->Fonts.empty() ? nullptr : atlas->Fonts[0];
  static const ImWchar kRanges[] = {
      0x0020, 0x00FF,  // Basic Latin + Latin-1 (accents)
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
  for (size_t i = 0; i < std::size(kTextSizes); ++i) {
    g_regular[i] = add(BarlowMedium_compressed_data_base85, kTextSizes[i]);
    g_bold[i] = add(BarlowSemiBold_compressed_data_base85, kTextSizes[i]);
  }
  ImGui::GetIO().FontDefault = g_regular[TextSizeIndex()];
}

ImFont* BoldFont() {
  ImFont* bold = g_bold[TextSizeIndex()];
  return bold ? bold : ImGui::GetFont();
}

ImFont* MonoFont() { return g_mono ? g_mono : ImGui::GetFont(); }

void ApplyTheme(ImGuiStyle& style) {
  const ThemeColors& c = CurrentColors();
  const float opacity = float(Opacity());

  // Shape: Call of Duty 3's window (square-ish like the games' menus, roomier than the SDK default).
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

  // Text size: the bake for it becomes the default font, at its own size (from the next frame).
  const int i = TextSizeIndex();
  if (g_regular[i]) ImGui::GetIO().FontDefault = g_regular[i];
  style.FontSizeBase = kTextSizes[i];
  style._NextFrameFontSizeBase = kTextSizes[i];

  g_applied_theme = g_look.theme ? g_look.theme() : std::string();
  g_applied_size = TextSize();
  g_applied_opacity = Opacity();
}

void ApplyThemeIfChanged() {
  const bool theme_changed = g_look.theme && g_look.theme() != g_applied_theme;
  if (theme_changed || TextSize() != g_applied_size || Opacity() != g_applied_opacity) ApplyTheme(ImGui::GetStyle());
}

void Section(const char* title) {
  ImGui::Spacing();
  ImGui::PushFont(BoldFont(), 0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, CurrentColors().accent_text);
  ImGui::SeparatorText(title);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

}  // namespace f2
