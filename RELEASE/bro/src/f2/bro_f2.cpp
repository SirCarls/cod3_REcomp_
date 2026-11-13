// [f2-menus] Big Red One's F2 settings window (see bro_f2.h and dev/agents/f2-menus/bro/BRO-F2.md).
//
// The window is the other games' (Call of Duty 3's F2 window): its theme and fonts (f2_theme), its rows (f2_rows),
// its CONSOLE page (f2_console) and Back + Start (f2_pad), all from COD_3_WAW_Setup-tool/recomp/src/f2/, built
// unchanged; their rex::cvar calls are answered by shim/rex/cvar.h (bro_f2_store.cpp), since Big Red One isn't on the
// rexglue SDK. This file does what the SDK does for the other games: Dear ImGui's SDL3 and Vulkan backends on
// ngl_vk's device, the events, and the draw over the game's picture (ngl_vk's overlay hook, at the end of each
// frame, before the picture is copied out and shown).
//
// Pages: VIDEO, GAME, CONTROLS, INTERFACE and PERFORMANCE have the port's bro.toml settings and, under them, the
// game's own cvars that belong there (bro_dvars.cpp); DVARS has every cvar the game has made, in groups; CONSOLE
// sends lines to the game's own command buffer.
//
// The picture the window is drawn into is ngl_vk's target (the game's 640 x 480 times its scale), which ngl_vk then
// fits into the window, centered. So the window lays itself out in the target's pixels, and the mouse's position is
// taken from SDL and mapped into the target the same way.

#include "bro_f2.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>  // built with IMGUI_IMPL_VULKAN_NO_PROTOTYPES (Vulkan's functions loaded, below)
#include <SDL3/SDL_vulkan.h>

#include "bro_dvars.h"
#include "bro_f2_store.h"
#include "f2/f2_accel.h"
#include "f2/f2_console.h"
#include "f2/f2_pad.h"
#include "f2/f2_rows.h"
#include "f2/f2_theme.h"
#include "ngl_vk_backend.h"
#include <rex/cvar.h>  // shim/rex/cvar.h

extern "C" {
#include "bro_game_cvars.h"
#include "bro_settings.h"
// [f2-menus] proposed in ngl_vk_backend.h (BRO-F2.md): something drawn over the finished picture, at the end of the
// frame, before it is copied out and shown. The target is in target_layout_at_end, and stays so.
typedef void (*ngl_vk_overlay_fn)(VkCommandBuffer commands, void* user);
void ngl_vk_set_overlay(ngl_vk_overlay_fn fn, void* user);
}

namespace f2 {
// What a page's rows tell the window: the other games' f2_window.h has the same, but that file is built on the
// rexglue SDK's dialog, so Big Red One has its own copy of this one struct.
struct PageContext {
  bool dirty = false;     // something changed since the last save
  bool restart = false;   // a change takes effect at the next start
  bool save_now = false;  // a row asks to be saved at once
};
}  // namespace f2

namespace {

using bro_f2::DvarRow;
using bro_f2::Kind;
using f2::Applies;
using f2::Help;
using f2::Section;

SDL_Window* g_window = nullptr;
bool g_ready = false;
bool g_open = false;
f2::PadShortcut g_pad;
f2::PageContext g_page;
std::string g_status;
int g_wanted_tab = -1;

// ------------------------------------------------------------------------------------------------ the game's rows
void DrawGameRows(const char* page, f2::PageContext& c) {
  size_t count = 0;
  const DvarRow* rows = bro_f2::DvarRows(&count);
  const char* section = nullptr;
  int missing = 0;
  for (size_t i = 0; i < count; ++i) {
    const DvarRow& r = rows[i];
    if (std::strcmp(r.page, page) != 0) continue;
    if (!f2::Has(r.name)) {
      ++missing;
      continue;
    }
    if (!section || std::strcmp(section, r.section) != 0) {
      Section(r.section);
      section = r.section;
    }
    switch (r.kind) {
      case Kind::kCheck:
        c.dirty |= f2::Check(r.label, r.name, Applies::kAtOnce, &c.restart);
        break;
      case Kind::kInt:
        c.dirty |= f2::SliderInt(r.label, r.name, static_cast<int>(r.min), static_cast<int>(r.max), "%d",
                                 Applies::kAtOnce, &c.restart);
        break;
      case Kind::kFloat:
        c.dirty |= f2::SliderDouble(r.label, r.name, r.min, r.max, "%.2f", Applies::kAtOnce, &c.restart);
        break;
      case Kind::kText:
        c.dirty |= f2::TextRow(r.label, r.name, &c.restart);
        break;
    }
    if (r.help) Help(r.help);
  }
  if (missing) {
    char text[160];
    std::snprintf(text, sizeof(text), "%d of the game's settings for this page appear once a level has started.",
                  missing);
    Help(text);
  }
}

// "Reset" for a page's game rows: each back to the game's default.
void ResetGameRows(const char* page, f2::PageContext& c) {
  size_t count = 0;
  const DvarRow* rows = bro_f2::DvarRows(&count);
  for (size_t i = 0; i < count; ++i) {
    if (std::strcmp(rows[i].page, page) != 0) continue;
    bro_cvar cv;
    if (!bro_game_cvar_find(rows[i].name, &cv) || bro_game_cvar_read_only(cv.flags)) continue;
    if (std::strcmp(f2::Get(rows[i].name).c_str(), cv.reset) != 0) {
      bro_game_cvar_set(rows[i].name, cv.reset);
      c.dirty = true;
      if (cv.flags & BRO_CVAR_LATCH) c.restart = true;
    }
  }
}

// ------------------------------------------------------------------------------------------------ the pages
void DrawVideo(f2::PageContext& c) {
  if (f2::ResetButton("reset_video_game", "game's picture settings")) ResetGameRows("VIDEO", c);
  Section("Display");
  {
    const char* v[] = {"false", "true"};
    const char* n[] = {"Windowed", "Full screen"};
    c.dirty |= f2::Choice("Display mode", "display.fullscreen", v, n, 2, Applies::kAtOnce, &c.restart);
  }
  c.dirty |= f2::Check("V-Sync", "display.vsync", Applies::kAfterRestart, &c.restart);
  Help("Frames in step with the display. Applies after a restart.");
  {
    const char* v[] = {"", "1280x960", "1920x1440", "2560x1920"};
    const char* n[] = {"Twice the game's 640 x 480", "1280 x 960", "1920 x 1440", "2560 x 1920"};
    c.dirty |= f2::Choice("Picture size", "display.size", v, n, 4, Applies::kAfterRestart, &c.restart);
    Help("The size the game is drawn at. Applies after a restart.");
  }
  Section("Frame rate");
  c.dirty |= f2::SliderInt("Frame rate limit", "display.max_fps", 0, 1000, "%d fps", Applies::kAtOnce, &c.restart);
  Help("0 is no limit; empty in bro.toml is the game's own pace (the Xbox's 30). Movies keep their own speed.");
  c.dirty |= f2::Check("Show the frame rate", "display.show_fps", Applies::kAtOnce, &c.restart);
  Section("View");
  c.dirty |= f2::SliderDouble("Field of view", "display.fov", 1.0, 160.0, "%.0f", Applies::kAtOnce, &c.restart);
  Help("The angle across the picture, in degrees, kept whatever the game sets. Empty in bro.toml: the game's own.");
  DrawGameRows("VIDEO", c);
}

void DrawGame(f2::PageContext& c) {
  if (f2::ResetButton("reset_game_game", "game's settings on this page")) ResetGameRows("GAME", c);
  Section("Movies");
  c.dirty |= f2::Check("Movies at their own speed", "display.movie_pacing", Applies::kAtOnce, &c.restart);
  Help("The game's 30 pictures a second, whatever the frame rate limit.");
  c.dirty |= f2::Check("Skip every movie (for tests)", "display.skip_movies", Applies::kAtOnce, &c.restart);
  DrawGameRows("GAME", c);
}

void DrawControls(f2::PageContext& c) {
  if (f2::ResetButton("reset_controls_game", "game's control settings")) ResetGameRows("CONTROLS", c);
  Section("Mouse");
  c.dirty |= f2::Check("Raw mouse (as on the PC)", "input.raw_mouse", Applies::kAtOnce, &c.restart);
  c.dirty |= f2::SliderDouble("Sensitivity", "input.mouse_sensitivity", 0.1, 30.0, "%.2f", Applies::kAtOnce,
                              &c.restart);
  Help("As the PC games' sensitivity: each count turns the view 0.022 degrees times this, at any frame rate.");
  c.dirty |= f2::SliderDouble("Side to side", "input.mouse_scale_x", 0.1, 4.0, "%.2f", Applies::kAtOnce, &c.restart);
  c.dirty |= f2::SliderDouble("Up and down", "input.mouse_scale_y", 0.1, 4.0, "%.2f", Applies::kAtOnce, &c.restart);
  c.dirty |= f2::Check("Invert up and down", "input.invert_y", Applies::kAtOnce, &c.restart);
  c.dirty |= f2::SliderDouble("Zoom sensitivity", "input.zoom_sensitivity", 0.05, 4.0, "%.2f", Applies::kAtOnce,
                              &c.restart);
  Help("While aiming down the sights the mouse turns slower by the zoom, as in CoD3; this multiplies that.");
  {  // [f2-menus] 2026-11-11 the sensitivity scales, the acceleration and the sensitivity randomizer (f2/f2_accel.h)
    static const f2::accel::Widgets kWidgets = {
        [](const char* title) { Section(title); },
        [](const char* label, const char* name, bool* restart) {
          return f2::Check(label, name, Applies::kAtOnce, restart);
        },
        [](const char* label, const char* name, const char* const* values, const char* const* names, int count,
           bool* restart) { return f2::Choice(label, name, values, names, count, Applies::kAtOnce, restart); },
        [](const char* label, const char* name, int min, int max, const char* format, bool* restart) {
          return f2::SliderInt(label, name, min, max, format, Applies::kAtOnce, restart);
        },
        [](const char* label, const char* name, double min, double max, const char* format, bool* restart) {
          return f2::SliderDouble(label, name, min, max, format, Applies::kAtOnce, restart);
        },
        [](const char* text) { Help(text); },
    };
    c.dirty |= f2::accel::DrawRows(kWidgets, &c.restart);
    if (f2::ResetButton("reset_controls_mouse", "acceleration, randomizer and sensitivity scale settings")) {
      const char* const* names = nullptr;
      const int count = f2::accel::Settings(&names);
      for (int i = 0; i < count; ++i) c.restart |= f2::ResetSettings({names[i]});
      c.dirty = true;
    }
  }
  Section("Controller");
  c.dirty |= f2::SliderDouble("Dead zone", "input.dead_zone", 0.0, 0.9, "%.2f", Applies::kAtOnce, &c.restart);
  c.dirty |= f2::Check("Back + Start held 1 second opens these settings", "interface.pad_shortcut", Applies::kAtOnce,
                       &c.restart);
  Section("Keys");
  c.dirty |= f2::Check("Keys do Call of Duty 3's actions", "input.binds", Applies::kAtOnce, &c.restart);
  if (f2::GetBool("input.binds")) {
    static const char* const kActions[][2] = {
        {"binds.fire", "Fire"},          {"binds.aim", "Aim"},
        {"binds.use", "Use"},            {"binds.reload", "Reload"},
        {"binds.melee", "Melee"},        {"binds.frag", "Grenade"},
        {"binds.sprint", "Sprint"},      {"binds.leanleft", "Lean left"},
        {"binds.leanright", "Lean right"}, {"binds.jump", "Jump"},
        {"binds.crouch", "Crouch"},      {"binds.prone", "Prone"},
        {"binds.weapnext", "Next weapon"}, {"binds.weapprev", "Previous weapon"},
        {"binds.scores", "Objectives"},
    };
    Help("Up to two keys an action, comma-separated, Call of Duty 3's names (MOUSE1, SPACE, SHIFT, ...).");
    for (const auto& a : kActions) c.dirty |= f2::TextRow(a[1], a[0], &c.restart);
  }
  DrawGameRows("CONTROLS", c);
}

void DrawInterface(f2::PageContext& c) {
  const f2::LookSettings& look = f2::Look();
  if (f2::ResetButton("reset_interface", "interface settings")) {
    c.restart |= f2::ResetSettings({look.theme_name, look.text_size_name, look.opacity_name});
    ResetGameRows("INTERFACE", c);
    c.dirty = true;
  }
  Section("This window");
  size_t count = 0;
  const f2::Theme* themes = f2::Themes(&count);
  const f2::Theme& chosen = f2::CurrentTheme();
  if (ImGui::BeginCombo("Theme", chosen.label)) {
    for (size_t i = 0; i < count; ++i) {
      const bool selected = &themes[i] == &chosen;
      if (ImGui::Selectable(themes[i].label, selected) && !selected) {
        rex::cvar::SetFlagByName(look.theme_name, themes[i].id);
        c.dirty = true;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", themes[i].description);
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Help(chosen.description);
  {
    const char* v[] = {"14", "16", "18", "20", "24"};
    const char* n[] = {"Small", "Normal", "Large", "Larger", "Largest"};
    c.dirty |= f2::Choice("Text size", look.text_size_name, v, n, 5, Applies::kAtOnce, &c.restart);
  }
  c.dirty |= f2::SliderDouble("Window opacity", look.opacity_name, 0.6, 1.0, "%.2f", Applies::kAtOnce, &c.restart);
  Help("Lower it to see the game behind this window while you change settings.");
  DrawGameRows("INTERFACE", c);
}

void DrawPerformance(f2::PageContext& c) {
  Section("Logging");
  c.dirty |= f2::SliderDouble("Frame-time log every (s)", "display.fps_log", 0.0, 60.0, "%.0f", Applies::kAtOnce,
                              &c.restart);
  Help("A log line with the frame rate and the frame's parts every this many seconds; 0 = never.");
  DrawGameRows("PERFORMANCE", c);
}

// ------------------------------------------------------------------------------------------------ DVARS
struct DvarsPage {
  char filter[64] = {};
  bool changed_only = false;
} g_dvars;

bool NameMatches(const char* name, const char* filter) {
  if (!filter[0]) return true;
  const size_t n = std::strlen(filter);
  for (const char* p = name; *p; ++p) {
    size_t i = 0;
    while (i < n && p[i] && std::tolower(static_cast<unsigned char>(p[i])) ==
                                std::tolower(static_cast<unsigned char>(filter[i])))
      ++i;
    if (i == n) return true;
  }
  return false;
}

std::string FlagWords(uint32_t flags) {
  std::string out;
  auto add = [&](uint32_t bit, const char* word) {
    if (!(flags & bit)) return;
    if (!out.empty()) out += ", ";
    out += word;
  };
  add(BRO_CVAR_ROM, "the game's own");
  add(BRO_CVAR_INIT, "set at start");
  add(BRO_CVAR_CHEAT, "cheat");
  add(BRO_CVAR_LATCH, "next level");
  add(BRO_CVAR_ARCHIVE, "profile");
  add(BRO_CVAR_TEMP, "temporary");
  add(BRO_CVAR_USER_CREATED, "made by a command");
  return out;
}

void DrawDvars(f2::PageContext& c) {
  const int count = bro_game_cvars_count();
  Section("Every setting the game has");
  Help("All of the game's own cvars, as it has them now, in groups. A change goes into the game at once (a \"next "
       "level\" one when the level starts again) and is kept in bro.toml's [cvars], so it comes back at every start "
       "and when the game puts it back to its default. \"The game's own\" ones can't be changed.");
  if (count == 0) {
    Help("The game hasn't made its settings yet.");
    return;
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
  ImGui::InputTextWithHint("##dvar_filter", "Find (part of a name)", g_dvars.filter, sizeof(g_dvars.filter));
  ImGui::SameLine();
  ImGui::Checkbox("Changed only", &g_dvars.changed_only);
  ImGui::SameLine();
  ImGui::TextDisabled("%d settings", count);

  size_t group_count = 0;
  const char* const* groups = bro_f2::DvarGroups(&group_count);
  std::vector<std::vector<std::pair<std::string, int>>> by_group(group_count);
  for (int i = 0; i < count; ++i) {
    bro_cvar cv;
    if (!bro_game_cvar_at(i, &cv) || !NameMatches(cv.name, g_dvars.filter)) continue;
    if (g_dvars.changed_only && std::strcmp(cv.value, cv.reset) == 0 && !cv.latched) continue;
    by_group[bro_f2::GroupOf(cv.name)].emplace_back(cv.name, i);
  }
  for (size_t g = 0; g < group_count; ++g) {
    auto& list = by_group[g];
    if (list.empty()) continue;
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
      return std::lexicographical_compare(a.first.begin(), a.first.end(), b.first.begin(), b.first.end(),
                                          [](char x, char y) {
                                            return std::tolower(static_cast<unsigned char>(x)) <
                                                   std::tolower(static_cast<unsigned char>(y));
                                          });
    });
    char header[96];
    std::snprintf(header, sizeof(header), "%s (%zu)###dvar_group_%zu", groups[g], list.size(), g);
    const ImGuiTreeNodeFlags open = (g_dvars.filter[0] || g_dvars.changed_only) ? ImGuiTreeNodeFlags_DefaultOpen : 0;
    if (!ImGui::CollapsingHeader(header, open)) continue;
    if (!ImGui::BeginTable(header, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) continue;
    ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 0.36f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.40f);
    ImGui::TableSetupColumn("##flags", ImGuiTableColumnFlags_WidthStretch, 0.24f);
    for (const auto& [name, index] : list) {
      bro_cvar cv;
      if (!bro_game_cvar_at(index, &cv)) continue;
      const bool locked = bro_game_cvar_read_only(cv.flags);
      const DvarRow* row = bro_f2::FindDvarRow(name.c_str());
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(name.c_str());
      if (ImGui::IsItemHovered()) {
        std::string tip;
        if (row) tip = std::string(row->label) + " (" + row->page + ")\n";
        tip += "Default: \"" + std::string(cv.reset) + "\"";
        const std::string words = FlagWords(cv.flags);
        if (!words.empty()) tip += "\n" + words;
        if (cv.latched) tip += "\nAt the next level: \"" + std::string(cv.latched) + "\"";
        ImGui::SetTooltip("%s", tip.c_str());
      }
      ImGui::TableSetColumnIndex(1);
      if (locked) {
        ImGui::TextDisabled("%s", cv.value);
      } else {
        ImGui::SetNextItemWidth(-1.0f);
        c.dirty |= f2::TextRow("##value", name.c_str(), &c.restart);
      }
      ImGui::TableSetColumnIndex(2);
      if (!locked && std::strcmp(cv.value, cv.reset) != 0) {
        ImGui::PushID(name.c_str());
        if (ImGui::SmallButton("Default")) {
          bro_game_cvar_set(name.c_str(), cv.reset);
          c.dirty = true;
          if (cv.flags & BRO_CVAR_LATCH) c.restart = true;
        }
        ImGui::PopID();
        ImGui::SameLine();
      }
      const std::string words = FlagWords(cv.flags & (BRO_CVAR_ROM | BRO_CVAR_INIT | BRO_CVAR_CHEAT | BRO_CVAR_LATCH));
      if (!words.empty()) ImGui::TextDisabled("%s", words.c_str());
    }
    ImGui::EndTable();
  }
}

// ------------------------------------------------------------------------------------------------ CONSOLE
f2::ConsolePage g_console;

void DrawConsole(f2::PageContext&) {
  Help("Lines for the game's own command buffer: set <name> <value>, reset <name>, toggle <name>, cvarlist, ... "
       "The game answers in its log.");
  f2::DrawConsolePage(g_console, [](const std::string& line) {
    f2::ConsoleLog::Get().Add(0, "] " + line);
    bro_game_command(line.c_str());
  });
}

struct Page {
  const char* label;
  void (*draw)(f2::PageContext&);
};
const Page kPages[] = {
    {"VIDEO", DrawVideo},           {"GAME", DrawGame},   {"CONTROLS", DrawControls}, {"INTERFACE", DrawInterface},
    {"PERFORMANCE", DrawPerformance}, {"DVARS", DrawDvars}, {"CONSOLE", DrawConsole},
};

// ------------------------------------------------------------------------------------------------ the window
void Save() {
  std::string error;
  g_status = bro_f2::Save(&error) ? "Saved to bro.toml" : error;
  g_page.dirty = false;
}

void SaveAtExit() {
  if (g_page.dirty) Save();
}

// False once its close box was clicked.
bool DrawWindow() {
  bool keep_open = true;
  ImGuiIO& io = ImGui::GetIO();
  const float fs = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(std::min(fs * 48.0f, io.DisplaySize.x * 0.92f),
                                  std::min(fs * 40.0f, io.DisplaySize.y * 0.88f)),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
                          ImVec2(0.5f, 0.5f));
  ImGui::PushFont(f2::BoldFont(), 0.0f);
  const bool shown = ImGui::Begin("CALL OF DUTY 2: BIG RED ONE  \xC2\xB7  SETTINGS###bro_settings", &keep_open,
                                  ImGuiWindowFlags_NoCollapse);
  ImGui::PopFont();
  if (shown && ImGui::BeginTabBar("##pages")) {
    const int step = g_pad.TakePageStep();
    static int current = 0;
    if (step) g_wanted_tab = (current + step + static_cast<int>(std::size(kPages))) % static_cast<int>(std::size(kPages));
    for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i) {
      const ImGuiTabItemFlags flags = g_wanted_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
      if (!ImGui::BeginTabItem(kPages[i].label, nullptr, flags)) continue;
      current = i;
      ImGui::BeginChild("##page", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 6.0f));
      kPages[i].draw(g_page);
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    g_wanted_tab = -1;
    ImGui::EndTabBar();
    ImGui::Separator();
    if (ImGui::Button(g_page.dirty ? "Save *" : "Save")) Save();
    if (g_page.restart) {
      ImGui::SameLine();
      ImGui::TextColored(f2::CurrentColors().warn, "Restart the game to apply some changes.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s  F2 or Back + Start (held) to close", g_status.c_str());
  }
  ImGui::End();
  return keep_open;
}

// ------------------------------------------------------------------------------------------------ open and close
void SetOpen(bool open) {
  if (open == g_open) return;
  g_open = open;
  // The pointer: shown while the window is open; the game's mouse look (relative mode) again after, as the port
  // has it while its window has the focus. The motion made meanwhile is dropped, so the view doesn't jump.
  const char* look = std::getenv("BRO_MOUSE_LOOK");
  const bool focused = g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS);
  if (g_window) SDL_SetWindowRelativeMouseMode(g_window, !open && focused && !(look && look[0] == '0'));
  float dx, dy;
  SDL_GetRelativeMouseState(&dx, &dy);
  if (!open && g_page.dirty) Save();  // closing saves, as in every other game
  std::fprintf(stderr, "[f2] window %s\n", open ? "open" : "closed");
}

// ------------------------------------------------------------------------------------------------ the mouse
// [f2-menus] 2026-11-11 (owner 04:40 "Please help with BRo"; 03:24 "just so we are clear FPS cannot affect mouse
// input."): the in-game acceleration, the sensitivity randomizer and the sensitivity scales (f2/f2_accel.h, the
// other games' file) for Big Red One. bro_pad.c reads the mouse once each time the game asks for its controls
// (SDL_GetRelativeMouseState): a sum, with no timing in it. The same motion is in SDL's queue event by event, each
// with the time it was made, until the port drops the queue at the end of check_quit(). So bro_f2_events puts every
// motion event through f2::accel with the event's own spacing and age, and keeps what the curve and the randomizer
// ADDED to those counts for bro_pad.c's next read (bro_f2_mouse). The frame rate only decides how many events are
// taken at a time, never what each one becomes. Nothing is taken out of the read itself: an event missed here only
// misses its addition.
float g_mouse_add_x = 0.0f, g_mouse_add_y = 0.0f;  // added by the curve and the randomizer, not yet handed over
Uint64 g_mouse_last_ns = 0;                          // the last motion event's time (SDL's clock, nanoseconds)

// BRO_MOUSE_EVENTS=<dx>,<dy>,<a second>: a test mouse for runs without one: that motion that many times a second by
// the clock, whatever the frame rate, each one put through f2::accel as a real event is, and handed to bro_pad.c
// whole. The log gets the counts handed over in every second or so (counts a second); and with BRO_MOUSE_TEST=0,0
// (bro_pad.c's own log of the view angles every 120 reads) it has, for the same reads, the counts handed over so
// far: degrees a count.
bool g_test_mouse = false;
float g_test_dx = 0.0f, g_test_dy = 0.0f;
double g_test_period_ns = 0.0, g_test_next_ns = 0.0;
float g_test_x = 0.0f, g_test_y = 0.0f;  // made, not yet handed over
double g_handed_x = 0.0, g_handed_y = 0.0;
unsigned g_mouse_reads = 0;
Uint64 g_mouse_first_ns = 0;
Uint64 g_window_ns = 0;  // the log's current second or so: its start, its reads and its counts
unsigned g_window_reads = 0;
double g_window_x = 0.0, g_window_y = 0.0;

void ReadTestMouse() {
  const char* text = std::getenv("BRO_MOUSE_EVENTS");
  float hz = 0.0f;
  if (!text || std::sscanf(text, "%f,%f,%f", &g_test_dx, &g_test_dy, &hz) != 3 || !(hz > 0.0f)) return;
  g_test_mouse = true;
  g_test_period_ns = 1.0e9 / hz;
  std::fprintf(stderr, "[f2] test mouse: %g,%g counts %g times a second\n", double(g_test_dx), double(g_test_dy),
               double(hz));
}

double AgeMs(Uint64 now_ns, double at_ns) {
  const double age = (double(now_ns) - at_ns) / 1.0e6;
  return age > 0.0 ? age : 0.0;
}

void TakeMouseMotion() {
  static SDL_Event events[1024];  // a frame's worth of an 8000 Hz mouse down to 8 frames a second
  const int n = SDL_PeepEvents(events, 1024, SDL_PEEKEVENT, SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_MOTION);
  const Uint64 now = SDL_GetTicksNS();
  if (g_open || !g_window || !SDL_GetWindowRelativeMouseMode(g_window)) {
    g_mouse_add_x = g_mouse_add_y = 0.0f;  // the game isn't reading the mouse: nothing to add to
    g_mouse_last_ns = 0;
  } else {
    for (int i = 0; i < n;) {
      // Events with one time (a mouse faster than the clock they are stamped with) are one motion.
      const Uint64 at = events[i].motion.timestamp;
      float dx = 0.0f, dy = 0.0f;
      for (; i < n && events[i].motion.timestamp == at; ++i) {
        dx += events[i].motion.xrel;
        dy += events[i].motion.yrel;
      }
      const double ms = g_mouse_last_ns && at > g_mouse_last_ns ? double(at - g_mouse_last_ns) / 1.0e6 : 0.0;
      g_mouse_last_ns = at;
      float ax = dx, ay = dy;
      f2::accel::ApplyAged(&ax, &ay, ms, AgeMs(now, double(at)));
      g_mouse_add_x += ax - dx;
      g_mouse_add_y += ay - dy;
    }
  }
  if (!g_test_mouse) return;
  if (g_open || g_test_next_ns == 0.0 || double(now) - g_test_next_ns > 1.0e9) g_test_next_ns = double(now);
  if (g_open) return;
  for (; g_test_next_ns <= double(now); g_test_next_ns += g_test_period_ns) {
    float ax = g_test_dx, ay = g_test_dy;
    f2::accel::ApplyAged(&ax, &ay, g_test_period_ns / 1.0e6, AgeMs(now, g_test_next_ns));
    g_test_x += ax;
    g_test_y += ay;
  }
}

// ------------------------------------------------------------------------------------------------ Vulkan
VkDescriptorPool g_pool = VK_NULL_HANDLE;
VkRenderPass g_pass = VK_NULL_HANDLE;
VkFramebuffer g_framebuffer = VK_NULL_HANDLE;
VkImageView g_framebuffer_view = VK_NULL_HANDLE;
struct ngl_vk_backend g_vk{};

// ngl_vk opens the system's Vulkan loader when it starts (ngl_vk_functions.h) rather than linking to it, so the port
// has no vk* symbols to link to: this file's few functions, and ImGui's backend's (IMGUI_IMPL_VULKAN_NO_PROTOTYPES),
// are looked up the same way, through the loader SDL opened for the port's Vulkan window, on ngl_vk's instance.
PFN_vkGetInstanceProcAddr g_get_proc = nullptr;
PFN_vkCreateRenderPass vkCreateRenderPass = nullptr;
PFN_vkCreateFramebuffer vkCreateFramebuffer = nullptr;
PFN_vkDestroyFramebuffer vkDestroyFramebuffer = nullptr;
PFN_vkCreateDescriptorPool vkCreateDescriptorPool = nullptr;
PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = nullptr;
PFN_vkCmdEndRenderPass vkCmdEndRenderPass = nullptr;

PFN_vkVoidFunction VulkanFunction(const char* name, void*) {
  return g_get_proc ? g_get_proc(g_vk.instance, name) : nullptr;
}

template <typename T>
bool Load(T& fn, const char* name) {
  fn = reinterpret_cast<T>(VulkanFunction(name, nullptr));
  if (!fn) std::fprintf(stderr, "[f2] Vulkan has no %s: the F2 window is off\n", name);
  return fn != nullptr;
}

bool LoadVulkan() {
  g_get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  if (!g_get_proc) {
    std::fprintf(stderr, "[f2] SDL has no Vulkan loader open (%s): the F2 window is off\n", SDL_GetError());
    return false;
  }
  if (!Load(vkCreateRenderPass, "vkCreateRenderPass") || !Load(vkCreateFramebuffer, "vkCreateFramebuffer") ||
      !Load(vkDestroyFramebuffer, "vkDestroyFramebuffer") || !Load(vkCreateDescriptorPool, "vkCreateDescriptorPool") ||
      !Load(vkCmdBeginRenderPass, "vkCmdBeginRenderPass") || !Load(vkCmdEndRenderPass, "vkCmdEndRenderPass"))
    return false;
  if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, VulkanFunction, nullptr)) {
    std::fprintf(stderr, "[f2] ImGui's Vulkan backend is missing a Vulkan function: the F2 window is off\n");
    return false;
  }
  return true;
}

bool MakeFramebuffer() {
  if (g_framebuffer) vkDestroyFramebuffer(g_vk.device, g_framebuffer, nullptr);
  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = g_pass;
  fb.attachmentCount = 1;
  fb.pAttachments = &g_vk.target_view;
  fb.width = g_vk.width;
  fb.height = g_vk.height;
  fb.layers = 1;
  g_framebuffer_view = g_vk.target_view;
  return vkCreateFramebuffer(g_vk.device, &fb, nullptr, &g_framebuffer) == VK_SUCCESS;
}

// The pass that draws over the target: it keeps what the game drew (load), in the layout ngl_vk leaves the target in
// when it calls the overlay (TRANSFER_SRC_OPTIMAL), and leaves it so for ngl_vk's copies.
bool MakePass() {
  VkAttachmentDescription color{};
  color.format = g_vk.target_format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = g_vk.target_layout_at_end;
  color.finalLayout = g_vk.target_layout_at_end;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkSubpassDependency deps[2]{};
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;  // after ngl_vk's last pass and copies
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].srcSubpass = 0;  // before ngl_vk's copies out
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = 1;
  info.pAttachments = &color;
  info.subpassCount = 1;
  info.pSubpasses = &sub;
  info.dependencyCount = 2;
  info.pDependencies = deps;
  return vkCreateRenderPass(g_vk.device, &info, nullptr, &g_pass) == VK_SUCCESS && MakeFramebuffer();
}

void DrawOverlay(VkCommandBuffer commands, void*) {
  ImDrawData* data = ImGui::GetDrawData();
  if (!g_ready || !data || data->CmdListsCount == 0) return;  // closed: nothing recorded, nothing spent
  struct ngl_vk_backend now{};
  if (ngl_vk_backend(&now) == 0 && (now.target_view != g_framebuffer_view || now.width != g_vk.width ||
                                    now.height != g_vk.height)) {
    g_vk = now;  // the target was made again (a new size)
    if (!MakeFramebuffer()) return;
  }
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = g_pass;
  begin.framebuffer = g_framebuffer;
  begin.renderArea.extent = {g_vk.width, g_vk.height};
  vkCmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(data, commands);
  vkCmdEndRenderPass(commands);
}

// Where the window's pointer is in the target's pixels (ngl_vk fits the target into the window, centered).
bool PointerInTarget(float* x, float* y) {
  int ww = 0, wh = 0, pw = 0, ph = 0;
  float mx = 0, my = 0;
  if (!g_window || !SDL_GetWindowSize(g_window, &ww, &wh) || !SDL_GetWindowSizeInPixels(g_window, &pw, &ph) ||
      ww <= 0 || wh <= 0 || pw <= 0 || ph <= 0)
    return false;
  SDL_GetMouseState(&mx, &my);
  const float density_x = static_cast<float>(pw) / static_cast<float>(ww);
  const float density_y = static_cast<float>(ph) / static_cast<float>(wh);
  const float scale = std::min(static_cast<float>(pw) / static_cast<float>(g_vk.width),
                               static_cast<float>(ph) / static_cast<float>(g_vk.height));
  const float left = (static_cast<float>(pw) - static_cast<float>(g_vk.width) * scale) * 0.5f;
  const float top = (static_cast<float>(ph) - static_cast<float>(g_vk.height) * scale) * 0.5f;
  *x = (mx * density_x - left) / scale;
  *y = (my * density_y - top) / scale;
  return true;
}

// A test run's steps, from BRO_F2_TEST: "<seconds>:<step>|...", the seconds from the window's first frame. Steps:
// open, close, page <LABEL>, set <name> <value> (as a row would), save, console <line>. Each runs once, in order.
// Also print <name>, count, and watch / snap / diff (below).
struct TestStep {
  double seconds;
  std::string action;
};
std::vector<TestStep> g_test;
size_t g_test_next = 0;
Uint64 g_first_ms = 0;

// Test steps that find where the game keeps what a command changes: "watch <hex address> <hex bytes>" adds a
// stretch of the game's memory to the watched ones, "snap" remembers them all as they are now, and "diff <label>"
// prints the words that differ from what was remembered (the first 16).
struct Watched {
  uint32_t address;
  std::vector<uint32_t> words;
};
std::vector<Watched> g_watched;

void SnapWatched() {
  for (Watched& w : g_watched)
    for (size_t i = 0; i < w.words.size(); ++i) w.words[i] = bro_game_peek32(w.address + static_cast<uint32_t>(i) * 4);
}

void DiffWatched(const char* label) {
  int shown = 0, changed = 0;
  std::string line;
  for (const Watched& w : g_watched) {
    for (size_t i = 0; i < w.words.size(); ++i) {
      const uint32_t at = w.address + static_cast<uint32_t>(i) * 4, now = bro_game_peek32(at);
      if (now == w.words[i]) continue;
      ++changed;
      if (shown++ < 16) {
        char text[64];
        std::snprintf(text, sizeof text, " %08X:%X>%X", at, w.words[i], now);
        line += text;
      }
    }
  }
  std::fprintf(stderr, "[f2] test: diff %s: %d word(s)%s\n", label, changed, line.c_str());
}

void ReadTest() {
  const char* text = std::getenv("BRO_F2_TEST");
  if (!text || !*text) return;
  std::string all(text);
  size_t at = 0;
  while (at <= all.size()) {
    const size_t bar = all.find('|', at);
    const std::string step = all.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
    const size_t colon = step.find(':');
    if (colon != std::string::npos) g_test.push_back({std::atof(step.substr(0, colon).c_str()), step.substr(colon + 1)});
    if (bar == std::string::npos) break;
    at = bar + 1;
  }
  std::fprintf(stderr, "[f2] test: %zu steps\n", g_test.size());
}

void RunTest() {
  if (g_test_next >= g_test.size()) return;
  const double seconds = static_cast<double>(SDL_GetTicks() - g_first_ms) / 1000.0;
  while (g_test_next < g_test.size() && seconds >= g_test[g_test_next].seconds) {
    const std::string& a = g_test[g_test_next++].action;
    std::fprintf(stderr, "[f2] test %.1f s: %s\n", seconds, a.c_str());
    if (a == "open") SetOpen(true);
    else if (a == "close") SetOpen(false);
    else if (a == "save") Save();
    else if (a.rfind("page ", 0) == 0) {
      for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i)
        if (a.substr(5) == kPages[i].label) g_wanted_tab = i;
    } else if (a.rfind("set ", 0) == 0) {
      const size_t space = a.find(' ', 4);
      if (space != std::string::npos) {
        const std::string name = a.substr(4, space - 4), value = a.substr(space + 1);
        const bool ok = rex::cvar::SetFlagByName(name, value);
        g_page.dirty |= ok;
        std::fprintf(stderr, "[f2] test: set %s \"%s\" %s\n", name.c_str(), value.c_str(), ok ? "ok" : "refused");
      }
    } else if (a.rfind("console ", 0) == 0) {
      f2::ConsoleLog::Get().Add(0, "] " + a.substr(8));
      bro_game_command(a.c_str() + 8);
    } else if (a.rfind("print ", 0) == 0) {  // a cvar's value now, for the log
      bro_cvar cv;
      if (bro_game_cvar_find(a.c_str() + 6, &cv))
        std::fprintf(stderr, "[f2] test: %s = \"%s\" (default \"%s\", flags 0x%X)\n", cv.name, cv.value, cv.reset,
                     cv.flags);
      else
        std::fprintf(stderr, "[f2] test: %s isn't made yet\n", a.c_str() + 6);
    } else if (a.rfind("watch ", 0) == 0) {
      unsigned address = 0, bytes = 0;
      if (std::sscanf(a.c_str() + 6, "%x %x", &address, &bytes) == 2 && bytes > 0 && bytes <= 0x4000) {
        g_watched.push_back({address, std::vector<uint32_t>(bytes / 4)});
        SnapWatched();
      }
    } else if (a == "snap") {
      SnapWatched();
    } else if (a.rfind("diff", 0) == 0) {
      DiffWatched(a.size() > 5 ? a.c_str() + 5 : "");
    } else if (a == "count") {
      std::fprintf(stderr, "[f2] test: the game has %d cvars\n", bro_game_cvars_count());
    }
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------ the C side
extern "C" int bro_f2_init(SDL_Window* window) {
  if (g_ready) return 0;
  if (!window || ngl_vk_backend(&g_vk) != 0 || !LoadVulkan()) return -1;
  g_window = window;
  bro_f2::SetWindow(window);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // the window's place isn't kept between runs
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  f2::LookSettings look;
  look.theme_name = "interface.theme";
  look.theme = &bro_f2::ThemeStorage;
  look.text_size_name = "interface.text_size";
  look.text_size = &bro_f2::TextSizeStorage;
  look.opacity_name = "interface.opacity";
  look.opacity = &bro_f2::OpacityStorage;
  f2::SetLook(look);  // Big Red One's own colors are still to be read from its menus: Call of Duty 3's meanwhile
  io.Fonts->AddFontDefault();  // the fixed-width font f2_theme keeps as MonoFont, as the SDK adds it first
  f2::ConfigureFonts(io.Fonts);
  f2::ApplyTheme(ImGui::GetStyle());
  if (!ImGui_ImplSDL3_InitForVulkan(window)) return -1;
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool.maxSets = 16;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  if (vkCreateDescriptorPool(g_vk.device, &pool, nullptr, &g_pool) != VK_SUCCESS || !MakePass()) return -1;
  ImGui_ImplVulkan_InitInfo info{};
  info.ApiVersion = VK_API_VERSION_1_0;
  info.Instance = g_vk.instance;
  info.PhysicalDevice = g_vk.gpu;
  info.Device = g_vk.device;
  info.QueueFamily = g_vk.queue_family;
  info.Queue = g_vk.queue;  // used by the backend only to upload its font picture (BRO-F2.md: ngl_vk's own device)
  info.DescriptorPool = g_pool;
  info.MinImageCount = 2;
  info.ImageCount = 2;
  info.PipelineInfoMain.RenderPass = g_pass;
  info.PipelineInfoMain.Subpass = 0;
  info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  if (!ImGui_ImplVulkan_Init(&info)) return -1;
  ngl_vk_set_overlay(DrawOverlay, nullptr);
  // [f2-menus] 2026-11-11 the mouse settings f2_accel.cpp makes itself, from bro.toml's [mouse]; Big Red One turns
  // 0.022 degrees a count at a sensitivity of 1 (bro_pad.c), with input.raw_mouse on.
  rex::cvar::shim::Load();
  f2::accel::SetSupported(true);
  f2::accel::SetGameSensitivity("input.mouse_sensitivity", "input.raw_mouse", 0.022, 0.01, 100.0);
  ReadTestMouse();
  ReadTest();
  std::atexit(SaveAtExit);
  g_ready = true;
  std::fprintf(stderr, "[f2] ready, drawn at %ux%u (F2, or Back + Start held 1 s on a controller)\n", g_vk.width,
               g_vk.height);
  return 0;
}

extern "C" void bro_f2_events(void) {
  if (!g_ready) return;
  // [f2-menus] 2026-11-11 the keys and the text, then the mouse's buttons and wheel: the mouse's motion is between
  // them in SDL's numbering and isn't wanted here (a fast mouse's motion alone could fill these 128 and push F2 out).
  SDL_Event events[128];
  int n = SDL_PeepEvents(events, 128, SDL_PEEKEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_MOTION - 1);
  if (n < 0) n = 0;
  if (n < 128) {
    const int more =
        SDL_PeepEvents(events + n, 128 - n, SDL_PEEKEVENT, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_WHEEL);
    if (more > 0) n += more;
  }
  for (int i = 0; i < n; ++i) {
    const SDL_Event& e = events[i];
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F2) {
      SetOpen(!g_open);
      continue;
    }
    if (!g_open) continue;
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && !ImGui::GetIO().WantTextInput) {
      SetOpen(false);
      continue;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION) continue;  // the position is mapped into the target below
    ImGui_ImplSDL3_ProcessEvent(&e);
  }
  if (g_pad.Poll(f2::GetBool("interface.pad_shortcut"))) SetOpen(!g_open);
  if (bro_game_cvars_take_adopted()) g_page.dirty = true;
  TakeMouseMotion();  // [f2-menus] 2026-11-11 before the port drops the queue

  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(g_vk.width), static_cast<float>(g_vk.height));
  io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
  float x, y;
  if (g_open && PointerInTarget(&x, &y)) io.AddMousePosEvent(x, y);
  if (!g_first_ms) g_first_ms = SDL_GetTicks();
  RunTest();
  ImGui::NewFrame();
  f2::accel::Tick();  // [f2-menus] 2026-11-11 the sensitivity randomizer (on real time) and its corner line
  g_pad.FeedNavigation(io, g_open);
  f2::ApplyThemeIfChanged();
  if (g_open && !DrawWindow()) SetOpen(false);  // its close box
  ImGui::Render();  // drawn by DrawOverlay when ngl_vk reaches the end of this frame
}

extern "C" int bro_f2_game_gets_input(void) { return !g_open; }

// [f2-menus] 2026-11-11 bro_pad.c, with the motion it just read (raw mouse only): what the curve and the randomizer
// added to it (TakeMouseMotion), and the test mouse's motion.
extern "C" void bro_f2_mouse(float* dx, float* dy) {
  if (!g_ready || !dx || !dy) return;
  if (g_test_mouse) {
    const Uint64 now = SDL_GetTicksNS();
    if (!g_mouse_first_ns) g_mouse_first_ns = now;
    if (g_mouse_reads % 120 == 0)
      std::fprintf(stderr, "[f2] test mouse: read %u, %.3f,%.3f counts handed over before it, at %.1f ms\n",
                   g_mouse_reads, g_handed_x, g_handed_y, double(now - g_mouse_first_ns) / 1.0e6);
    ++g_mouse_reads;
    if (!g_window_ns) g_window_ns = now;
    if (now - g_window_ns >= 1000000000ull) {
      std::fprintf(stderr, "[f2] test mouse: %u reads, %.3f,%.3f counts handed to the game in %.1f ms\n",
                   g_window_reads, g_window_x, g_window_y, double(now - g_window_ns) / 1.0e6);
      g_window_ns = now;
      g_window_reads = 0;
      g_window_x = g_window_y = 0.0;
    }
    ++g_window_reads;
  }
  *dx += g_mouse_add_x + g_test_x;
  *dy += g_mouse_add_y + g_test_y;
  g_mouse_add_x = g_mouse_add_y = g_test_x = g_test_y = 0.0f;
  g_handed_x += double(*dx);
  g_handed_y += double(*dy);
  g_window_x += double(*dx);
  g_window_y += double(*dy);
}

// f2_accel.cpp's log lines (Big Red One has no SDK log): the port's own, with the window's mark.
extern "C" void f2_accel_log(const char* text) { std::fprintf(stderr, "[f2] %s\n", text); }

extern "C" void bro_f2_shutdown(void) {
  if (!g_ready) return;
  SaveAtExit();
}
