// [acct2-fh] 2026-11-14 Finest Hour's F2 settings window (see fh_f2.h; dev/agents/acct2-fh/RESOLUTION.md stage 4).
//
// Owner 2026-11-13 20:38: "start working on the resolution options. Copy everything cod3 has. Please don't limit
// options based on cod3 tho." So this is Call of Duty 3's F2 window, as Big Red One has it (bro/src/f2/bro_f2.cpp,
// the model for this file): its theme and fonts (f2_theme), its rows (f2_rows) and Back + Start (f2_pad), all from
// COD_3_WAW_Setup-tool/recomp/src/f2/, built unchanged; their rex::cvar calls are answered by shim/rex/cvar.h
// (fh_f2_store.cpp). This file does what the rexglue SDK does for the other games: Dear ImGui's SDL3 and Vulkan
// backends on ngl_vk's device, the events, and the draw over the game's picture (ngl_vk's overlay hook, at the end of
// each frame, before the picture is copied out and shown).
//
// Pages: VIDEO (Display, Resolution, Picture shape: every fh.toml [display] setting, each with what Call of Duty 3's
// VIDEO page has and more where Finest Hour can go further), GAME (the console's language, as Call of Duty 3's
// "Console language"), INTERFACE (the window's look) and, since 11-14, CONSOLE: the port's own console
// (fh_console.c), since Finest Hour has no cvars and no console of its own: the port's log, and a line for commands,
// where fh.toml's settings are the variables. ~ opens the window on that page.
//
// Unlike Big Red One, Finest Hour remakes ngl_vk while it runs (a new render resolution: ngl_vk closed and opened on
// a new device), so what this window made on the device goes first (fh_f2_vulkan_stop) and is made again after
// (fh_f2_vulkan_start); Dear ImGui makes its font picture again by itself.
//
// The picture the window is drawn into is ngl_vk's target (the render resolution, anything from 64x48 to 16384 a
// side), which ngl_vk then fits into the window. So the window is laid out in the window's own points (its text the
// same size on the screen whatever the render resolution) and drawn into the target at the target's pixels per
// point, never fewer than one (a target smaller than the window shows the window larger).
//
// Finest Hour links the Vulkan loader (for ngl_vk_low.c), so ImGui's Vulkan backend calls Vulkan directly.

#include "fh_f2.h"

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
#include <imgui_impl_vulkan.h>

#include "f2/f2_pad.h"
#include "f2/f2_rows.h"
#include "f2/f2_theme.h"
#include "fh_f2_store.h"
#include "ngl_vk_backend.h"
#include <rex/cvar.h>  // shim/rex/cvar.h

extern "C" {
#include "fh_console.h"
#include "fh_present.h"
#include "fh_resolution.h"
#include "fh_settings.h"
}

namespace f2 {
// What a page's rows tell the window: the other games' f2_window.h has the same, but that file is built on the
// rexglue SDK's dialog, so Finest Hour has its own copy of this one struct (as Big Red One does).
struct PageContext {
  bool dirty = false;    // something changed since the last save
  bool restart = false;  // a change takes effect at the next start
};
}  // namespace f2

namespace {

using f2::Applies;
using f2::Help;
using f2::Section;

SDL_Window* g_window = nullptr;
bool g_ready = false;        // fh_f2_init done
bool g_vk_ready = false;     // its Vulkan part made on ngl_vk's device now
bool g_skip_draw = false;    // this frame's picture was made for the device before a remake: not drawn
bool g_open = false;
bool g_wait_release = false; // closed: the game gets the keyboard again once every key is up
f2::PadShortcut g_pad;
f2::PageContext g_page;
std::string g_status;
int g_wanted_tab = -1;

// ------------------------------------------------------------------------------------------------ rows made here
// A combo whose choices are made while the game runs (the monitors, their modes): values and names as strings.
struct Choices {
  std::vector<std::string> values, names;
  void Add(std::string value, std::string name) {
    values.push_back(std::move(value));
    names.push_back(std::move(name));
  }
  bool Draw(const char* label, const char* setting, Applies applies, bool* restart) const {
    std::vector<const char*> v, n;
    for (size_t i = 0; i < values.size(); ++i) {
      v.push_back(values[i].c_str());
      n.push_back(names[i].c_str());
    }
    return f2::Choice(label, setting, v.data(), n.data(), static_cast<int>(v.size()), applies, restart);
  }
};

// [acct2-fh] stage 5b: a number as the settings file keeps it (no trailing zeros: 0.25, not 0.250000).
std::string Number(float value) {
  char text[32];
  std::snprintf(text, sizeof text, "%.4g", static_cast<double>(value));
  return text;
}

// The monitor the window's modes are listed for: display.monitor's, else the one the window is on.
SDL_DisplayID ChosenDisplay() {
  const std::string want = f2::Get("display.monitor");
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  SDL_DisplayID id = 0;
  for (int i = 0; ids && i < count && !id; ++i) {
    const char* name = SDL_GetDisplayName(ids[i]);
    if ((name && want == name) || want == std::to_string(i + 1)) id = ids[i];
  }
  SDL_free(ids);
  if (!id && g_window) id = SDL_GetDisplayForWindow(g_window);
  return id;
}

void MonitorRow(f2::PageContext& c) {
  Choices m;
  m.Add("", "The one the window opens on");
  int count = 0;
  SDL_DisplayID* ids = SDL_GetDisplays(&count);
  for (int i = 0; ids && i < count; ++i) {
    const char* name = SDL_GetDisplayName(ids[i]);
    std::string label = std::to_string(i + 1) + ": " + (name ? name : "(no name)");
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(ids[i]);
    if (desktop) label += " (" + std::to_string(desktop->w) + " x " + std::to_string(desktop->h) + ")";
    // By name, as Call of Duty 3 keeps it, so it stays put when other screens come and go; two of the same name
    // by number.
    bool same_name = false;
    for (int j = 0; j < count; ++j) {
      const char* other = SDL_GetDisplayName(ids[j]);
      if (j != i && name && other && !std::strcmp(name, other)) same_name = true;
    }
    m.Add(name && !same_name ? name : std::to_string(i + 1), label);
  }
  SDL_free(ids);
  c.dirty |= m.Draw("Monitor", "display.monitor", Applies::kAtOnce, &c.restart);
}

// "WxH" of the monitor's full-screen modes, the largest first.
void FullscreenSizeRow(f2::PageContext& c) {
  Choices s;
  s.Add("", "The desktop's size");
  int count = 0;
  SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(ChosenDisplay(), &count);
  for (int i = 0; modes && i < count; ++i) {
    const std::string value = std::to_string(modes[i]->w) + "x" + std::to_string(modes[i]->h);
    if (std::find(s.values.begin(), s.values.end(), value) != s.values.end()) continue;
    s.Add(value, std::to_string(modes[i]->w) + " x " + std::to_string(modes[i]->h));
  }
  SDL_free(modes);
  c.dirty |= s.Draw("Full-screen size", "display.fullscreen_size", Applies::kAtOnce, &c.restart);
}

// The refresh rates the monitor has at the full-screen size (the desktop's when none is chosen).
void RefreshRateRow(f2::PageContext& c) {
  Choices r;
  r.Add("0", "The monitor's own");
  const SDL_DisplayID id = ChosenDisplay();
  const SDL_DisplayMode* desktop = id ? SDL_GetDesktopDisplayMode(id) : nullptr;
  int want_w = desktop ? desktop->w : 0, want_h = desktop ? desktop->h : 0;
  const std::string size = f2::Get("display.fullscreen_size");
  unsigned w = 0, h = 0;
  if (std::sscanf(size.c_str(), "%ux%u", &w, &h) == 2) {
    want_w = static_cast<int>(w);
    want_h = static_cast<int>(h);
  }
  int count = 0;
  SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(id, &count);
  for (int i = 0; modes && i < count; ++i) {
    if (modes[i]->w != want_w || modes[i]->h != want_h || modes[i]->refresh_rate <= 0.0f) continue;
    char value[32], name[48];
    std::snprintf(value, sizeof value, "%.2f", static_cast<double>(modes[i]->refresh_rate));
    std::snprintf(name, sizeof name, "%.2f Hz", static_cast<double>(modes[i]->refresh_rate));
    if (std::find(r.values.begin(), r.values.end(), value) != r.values.end()) continue;
    r.Add(value, name);
  }
  SDL_free(modes);
  c.dirty |= r.Draw("Refresh rate", "display.refresh_rate", Applies::kAtOnce, &c.restart);
}

// ------------------------------------------------------------------------------------------------ the pages
const char* const kVideoSettings[] = {
    "display.mode",       "display.monitor",           "display.refresh_rate",   "display.fullscreen_size",
    "display.window_size", "display.vsync",            "display.fit",            "display.render_resolution",
    "display.render_percent", "display.live_resolution", "display.aspect_ratio", "display.scaling",
    "display.max_fps",
};

void DrawVideo(f2::PageContext& c) {
  if (f2::ResetButton("reset_video", "display and resolution settings")) {
    c.restart |= f2::ResetSettings({kVideoSettings[0], kVideoSettings[1], kVideoSettings[2], kVideoSettings[3],
                                    kVideoSettings[4], kVideoSettings[5], kVideoSettings[6], kVideoSettings[7],
                                    kVideoSettings[8], kVideoSettings[9], kVideoSettings[10], kVideoSettings[11],
                                    kVideoSettings[12]});
    c.dirty = true;
  }
  {
    char note[400];
    fh_present_resolution_note(note, sizeof note);
    if (note[0]) {
      const std::string now = std::string("Now: ") + note;
      Help(now.c_str());
    }
  }

  Section("Display");
  {
    const char* v[] = {"window", "borderless", "exclusive"};
    const char* n[] = {"Windowed", "Borderless full screen", "Exclusive full screen"};
    c.dirty |= f2::Choice("Display mode", "display.mode", v, n, 3, Applies::kAtOnce, &c.restart);
    Help("Borderless fills the screen at the desktop's own size and refresh rate. Exclusive switches the monitor to "
         "the size and refresh rate below.");
  }
  MonitorRow(c);
  const bool exclusive = f2::Get("display.mode") == "exclusive";
  ImGui::BeginDisabled(!exclusive);
  FullscreenSizeRow(c);
  RefreshRateRow(c);
  ImGui::EndDisabled();
  if (!exclusive) Help("The full-screen size and refresh rate are for exclusive full screen.");
  {
    const char* v[] = {"640x480",   "960x720",   "1280x960",  "1600x1200", "1920x1440", "2560x1920",
                       "1280x720",  "1600x900",  "1920x1080", "2560x1440", "3840x2160", "1280x800",
                       "1680x1050", "1920x1200", "2560x1080", "3440x1440", "5120x1440"};
    const char* n[] = {"640 x 480 (the Xbox's)", "960 x 720", "1280 x 960", "1600 x 1200", "1920 x 1440",
                       "2560 x 1920", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160",
                       "1280 x 800", "1680 x 1050", "1920 x 1200", "2560 x 1080", "3440 x 1440", "5120 x 1440"};
    c.dirty |= f2::Choice("Window size", "display.window_size", v, n, static_cast<int>(std::size(v)),
                          Applies::kAtOnce, &c.restart);
    c.dirty |= f2::TextRow("Window size (any)", "display.window_size", &c.restart);
    Help("<width>x<height>; the window can also be dragged to any size.");
  }
  {
    const char* v[] = {"true", "false", "immediate"};
    const char* n[] = {"On (in step with the display)", "Off (the newest frame; no tearing where offered)",
                       "Off, immediate (the least wait, may tear)"};
    c.dirty |= f2::Choice("V-Sync", "display.vsync", v, n, 3, Applies::kAtOnce, &c.restart);
  }
  {
    // [acct2-fh] 11-14: the frame limit (fh_frame_pace.c, FRAMERATE-AUDIT.md); any number from 1 to 1000 below
    const char* v[] = {"console", "30", "60", "120", "144", "165", "240", "360", "500", "1000", "0"};
    const char* n[] = {"The console's (60, held to the game's own pace)", "30", "60", "120", "144", "165", "240", "360",
                       "500", "1000", "None"};
    c.dirty |= f2::Choice("Frame limit", "display.max_fps", v, n, static_cast<int>(std::size(v)), Applies::kAtOnce,
                          &c.restart);
    c.dirty |= f2::TextRow("Frame limit (any)", "display.max_fps", &c.restart);
    Help("Frames a second at most. The game moves by the time between frames, so its speed stays the same; with "
         "V-Sync on the display's refresh rate is the most. What can still differ above 60 is in the frame-rate "
         "audit.");
  }
  {
    const char* v[] = {"smooth", "sharp", "integer", "cas", "fsr"};
    const char* n[] = {"Smooth (filtered)", "Sharp (each pixel kept)", "Whole multiples (sharp, black around)",
                       "AMD FidelityFX CAS (filtered, sharpened)", "AMD FidelityFX FSR 1 (upscaled, sharpened)"};
    c.dirty |= f2::Choice("Scaling to the window", "display.scaling", v, n, static_cast<int>(std::size(v)),
                          Applies::kAtOnce, &c.restart);
    Help("How the picture is fitted into the window when the two sizes differ. FSR 1 is for a picture drawn smaller "
         "than the window (render scale below 100%); it is CAS when the picture isn't scaled up.");
  }
  // [acct2-fh] stage 5b: Call of Duty 3's "CAS sharpness" and "FSR sharpness" as percents, and FSR's passes
  if (f2::Get("display.scaling") == "cas") {
    double sharpness = f2::GetDouble("display.cas_sharpness", 0.0) * 100.0;
    sharpness = sharpness < 0.0 ? 0.0 : sharpness > 100.0 ? 100.0 : sharpness;
    float shown = static_cast<float>(sharpness);
    if (ImGui::SliderFloat("CAS sharpness", &shown, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
      rex::cvar::SetFlagByName("display.cas_sharpness", Number(shown / 100.0f));
      c.dirty = true;
    }
  }
  if (f2::Get("display.scaling") == "fsr") {
    // shown as Call of Duty 3 shows it: 100% the sharpest (0 stops less sharp), 0% two stops less sharp
    double sharpness = (1.0 - f2::GetDouble("display.fsr_sharpness_reduction", 0.2) / 2.0) * 100.0;
    sharpness = sharpness < 0.0 ? 0.0 : sharpness > 100.0 ? 100.0 : sharpness;
    float shown = static_cast<float>(sharpness);
    if (ImGui::SliderFloat("FSR sharpness", &shown, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
      rex::cvar::SetFlagByName("display.fsr_sharpness_reduction", Number((1.0f - shown / 100.0f) * 2.0f));
      c.dirty = true;
    }
    c.dirty |= f2::SliderInt("FSR passes at most", "display.fsr_passes", 1, 8, "%d", Applies::kAtOnce, &c.restart);
    Help("Each EASU pass doubles the size, the last one goes the rest of the way.");
  }

  Section("Resolution");
  {
    const char* v[] = {"auto", "original", "480p", "720p", "1080p", "1440p", "2160p", "2880p", "4320p"};
    const char* n[] = {"The window's size (automatic)",  "640 x 480 (the Xbox's)", "480p (854 x 480)",
                       "720p",  "1080p", "1440p", "2160p (4K)", "2880p (5K)", "4320p (8K)"};
    c.dirty |= f2::Choice("Render resolution", "display.render_resolution", v, n, static_cast<int>(std::size(v)),
                          Applies::kAtOnce, &c.restart);
    c.dirty |= f2::TextRow("Render resolution (any)", "display.render_resolution", &c.restart);
    Help("What the game is drawn at, apart from the window: any <width>x<height> from 64x48 to the graphics card's "
         "largest (16384 a side on most). Bigger than the window is shrunk to fit it (supersampling).");
  }
  c.dirty |= f2::SliderInt("Render scale", "display.render_percent", static_cast<int>(FH_MIN_PERCENT),
                           static_cast<int>(FH_MAX_PERCENT), "%d%%", Applies::kAtOnce, &c.restart);
  Help("Each side times this: 50% draws a quarter of the pixels, 200% four times as many. Not for 640 x 480.");
  {
    const char* v[] = {"bars", "stretch"};
    const char* n[] = {"Keep the shape (black bars)", "Stretch to the window"};
    c.dirty |= f2::Choice("Fit", "display.fit", v, n, 2, Applies::kAtOnce, &c.restart);
  }
  c.dirty |= f2::Check("Change it while playing", "display.live_resolution", Applies::kAtOnce, &c.restart);
  Help("A new size (a setting here, the window resized) is made at once; off keeps the size the game started with.");

  Section("Picture shape");
  {
    const char* v[] = {"auto", "game", "resolution", "5:4", "16:10", "16:9", "21:9", "32:9", "3:4", "9:16"};
    const char* n[] = {"The window's shape (automatic)", "4:3 (the game's own)", "The render resolution's",
                       "5:4", "16:10", "16:9", "21:9", "32:9", "3:4 (taller than wide)", "9:16"};
    c.dirty |= f2::Choice("Aspect ratio", "display.aspect_ratio", v, n, static_cast<int>(std::size(v)),
                          Applies::kAtOnce, &c.restart);
    c.dirty |= f2::TextRow("Aspect ratio (any)", "display.aspect_ratio", &c.restart);
    Help("Wider shows more of the world at the sides, narrower less; the view up and down stays the game's. Any "
         "\"W:H\", \"WxH\" or number from 1:4 to 8:1. The HUD, the menus and the films stay 4:3 in the middle.");
  }
}

// [acct2-fh] 11-14: the game's own settings. The console's language is read by the game once, at its start
// (XGetLanguage, 0x001320BE; main.c), so a change here is for the next start.
void DrawGame(f2::PageContext& c) {
  if (f2::ResetButton("reset_game", "game settings")) {
    c.restart |= f2::ResetSettings({"game.language", "game.fine_frame_clock", "game.frame_clocks_by_time"});
    c.dirty = true;
  }
  Section("Language");
  {
    const char* v[] = {"english", "french", "italian", "spanish"};
    const char* n[] = {"English", "French", "Italian", "Spanish"};
    c.dirty |= f2::Choice("Console language", "game.language", v, n, static_cast<int>(std::size(v)),
                          Applies::kAfterRestart, &c.restart);
    Help("What the console tells the game at its start: its menus, voices and the films that have their own language "
         "follow it. These are the four this disc has.");
  }
  // [acct2-fh] 11-14: the game's frame timer (fh_frame_clock.c), read again whenever a setting changes
  Section("Timing");
  c.dirty |= f2::Check("Time between frames to the microsecond", "game.fine_frame_clock", Applies::kAtOnce, &c.restart);
  Help("The game moves everything by the time since the last frame. The console measured it in whole milliseconds, "
       "which above 60 frames a second makes the steps uneven (20% at 240); on, it is measured exactly.");
  c.dirty |= f2::Check("Camera shake by time, not frames", "game.frame_clocks_by_time", Applies::kAtOnce,
                       &c.restart);
  Help("Two of the game's clocks count frames: the frame count / 60, which moves the camera shake's smooth random "
       "wobble, and the shake's own frame time, which the game works out once, at the first shake. Above 60 frames a "
       "second the shakes wobble too fast and end too soon (4 times at 240); on, both follow the time the frames "
       "took.");
}

// [input-port] 2026-11-24 (owner 15:55: port what we can of Call of Duty 3's F2 menu to the other games; the menus
// thread's matrix, menus-native/f2-port-matrix.md): CONTROLS, which Finest Hour's window didn't have. Its rows are the
// settings fh_pad.c reads (fh.toml [input]), again whenever a setting changes, so they apply at once. Finest Hour has
// no mouse look and no rumble, so CoD3's mouse, acceleration, per-weapon and vibration rows don't apply here.
void DrawControls(f2::PageContext& c) {
  if (f2::ResetButton("reset_controls", "control settings")) {
    c.restart |= f2::ResetSettings({"input.binds_kbm", "input.dead_zone"});
    c.dirty = true;
  }
  Section("Controller");
  c.dirty |= f2::SliderDouble("Stick dead zone", "input.dead_zone", 0.0, 0.9, "%.2f", Applies::kAtOnce, &c.restart);
  Help("A push smaller than this part of a full push counts as none, and the rest of the stick's travel is stretched "
       "over the whole range, so a worn stick doesn't drift. Both sticks; 0 (the default) leaves them as they are.");
  Section("Keyboard");
  c.dirty |= f2::TextRow("Keys", "input.binds_kbm", &c.restart);
  Help("Empty: the defaults (W A S D the left stick, I J K L the right stick, the arrow keys the d-pad, Enter start, "
       "Space a, Ctrl b, R x, F y, E black, Q white, Z lt, X rt, Shift and V the stick clicks). To change some, list "
       "them as action=KEY,KEY separated by ; (for example a=SPACE;rt=X,MOUSE1), with Call of Duty 3's key names. "
       "The actions are the pad's: up down left right start back lthumb rthumb a b x y black white lt rt "
       "move_forward move_back move_left move_right look_up look_down look_left look_right. Escape is always back.");
}

void DrawInterface(f2::PageContext& c) {
  const f2::LookSettings& look = f2::Look();
  if (f2::ResetButton("reset_interface", "interface settings")) {
    c.restart |= f2::ResetSettings({look.theme_name, look.text_size_name, look.opacity_name, "interface.pad_shortcut"});
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
  c.dirty |= f2::Check("Back + Start opens this window", "interface.pad_shortcut", Applies::kAtOnce, &c.restart);
  Help("Held for a second on a controller, so a player without a keyboard can reach the settings.");
}

// ------------------------------------------------------------------------------------------------ CONSOLE
// [acct2-fh] 11-14 (owner 17:13, "a console within finest hour similar to WAW"): the port's console, fh_console.c.
// Drawn here rather than with the shared f2_console page, whose help names Call of Duty's own commands and which has
// no Tab completion; the look is the same (its lines in the fixed-width font, its input line under them, Up and Down
// for what was typed before).
struct ConsoleState {
  char input[256] = {};
  std::vector<std::string> history;
  int history_pos = -1;
  std::vector<std::string> lines;  // the console's kept lines, copied when a new one comes
  uint64_t copied = ~0ull;         // fh_console_count() when they were copied
  bool focus = false;              // the input line takes the keyboard at the next draw (~ opened the page)
  bool follow = true;              // go to the bottom at the next draw (the page opened, a line was typed)
};
ConsoleState g_console;

void CollectName(const char* name, void* user) { static_cast<std::vector<std::string>*>(user)->push_back(name); }

void CollectLine(const char* line, void* user) {
  // the console's own lines without their "[console] " (the log keeps it), the rest as the log has them
  static const char kOwn[] = "[console] ";
  auto* lines = static_cast<std::vector<std::string>*>(user);
  lines->push_back(std::strncmp(line, kOwn, sizeof kOwn - 1) == 0 ? line + sizeof kOwn - 1 : line);
}

// Up and Down: what was typed before. Tab: the name being typed, completed as far as the names that start with it
// agree (commands and settings); when there are several, they are listed.
int ConsoleCallback(ImGuiInputTextCallbackData* data) {
  auto* c = static_cast<ConsoleState*>(data->UserData);
  if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    if (c->history.empty()) return 0;
    const int last = static_cast<int>(c->history.size()) - 1;
    if (data->EventKey == ImGuiKey_UpArrow)
      c->history_pos = c->history_pos < 0 ? last : std::max(0, c->history_pos - 1);
    else if (data->EventKey == ImGuiKey_DownArrow && c->history_pos >= 0)
      c->history_pos = c->history_pos >= last ? -1 : c->history_pos + 1;
    data->DeleteChars(0, data->BufTextLen);
    if (c->history_pos >= 0) data->InsertChars(0, c->history[c->history_pos].c_str());
  } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
    int start = data->CursorPos;
    while (start > 0 && data->Buf[start - 1] != ' ' && data->Buf[start - 1] != ';') --start;
    const std::string word(data->Buf + start, data->Buf + data->CursorPos);
    std::vector<std::string> names;
    fh_console_complete(word.c_str(), CollectName, &names);
    if (names.empty()) return 0;
    std::string common = names[0];
    for (const std::string& name : names) {
      size_t n = 0;
      while (n < common.size() && n < name.size() &&
             std::tolower(static_cast<unsigned char>(common[n])) == std::tolower(static_cast<unsigned char>(name[n])))
        ++n;
      common.resize(n);
    }
    if (names.size() == 1) common += ' ';
    data->DeleteChars(start, data->CursorPos - start);
    data->InsertChars(start, common.c_str());
    if (names.size() > 1) {
      std::string all;
      for (const std::string& name : names) all += (all.empty() ? "" : "  ") + name;
      fh_console_printf("%s", all.c_str());
    }
  }
  return 0;
}

void DrawConsole(f2::PageContext& c) {
  Help("The port's console: its log, and a line for commands (Finest Hour has no console of its own). help lists "
       "the commands and list the settings; a setting's name alone shows it, its name and a value sets it "
       "(display.max_fps 144), as soon as it is typed. Tab completes a name. A setting changed here is saved as the "
       "other pages' are: Save, or closing this window.");
  ConsoleState& con = g_console;
  const bool new_lines = fh_console_count() != con.copied;
  if (new_lines) {
    con.copied = fh_console_count();
    con.lines.clear();
    fh_console_lines(CollectLine, &con.lines);
  }
  const float input_height = ImGui::GetFrameHeightWithSpacing();
  ImGui::BeginChild("##console_lines", ImVec2(0, -input_height), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);
  // new lines follow while the view is at the bottom; scrolled up, it stays where the player put it
  const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
  ImGui::PushFont(f2::MonoFont(), 0.0f);
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(con.lines.size()));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const std::string& text = con.lines[static_cast<size_t>(i)];
      const bool typed = text.rfind("] ", 0) == 0;
      const bool bad = text.find("ERROR") != std::string::npos || text.find("Error") != std::string::npos ||
                       text.find("[crash]") != std::string::npos || text.rfind("no command", 0) == 0 ||
                       text.find(": no setting") != std::string::npos || text.find(" can't ") != std::string::npos;
      if (typed || bad) ImGui::PushStyleColor(ImGuiCol_Text, typed ? f2::CurrentColors().accent_text
                                                                   : f2::CurrentColors().bad);
      ImGui::TextUnformatted(text.c_str());
      if (typed || bad) ImGui::PopStyleColor();
    }
  }
  if (con.follow || (new_lines && at_bottom)) ImGui::SetScrollHereY(1.0f);
  con.follow = false;
  ImGui::PopFont();
  ImGui::EndChild();

  ImGui::SetNextItemWidth(-1.0f);
  if (con.focus) {
    ImGui::SetKeyboardFocusHere();
    con.focus = false;
  }
  const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
                                    ImGuiInputTextFlags_CallbackCompletion;
  if (ImGui::InputTextWithHint("##console_input", "A command or a setting's name, then Enter (help, list, Tab)",
                               con.input, sizeof con.input, flags, ConsoleCallback, &con)) {
    std::string line(con.input);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
    if (!line.empty()) {
      const unsigned before = fh_settings_generation();
      fh_console_run(line.c_str());
      if (fh_settings_generation() != before) c.dirty = true;  // saved with the other pages' changes
      con.history.erase(std::remove(con.history.begin(), con.history.end(), line), con.history.end());
      con.history.push_back(line);
      if (con.history.size() > 64) con.history.erase(con.history.begin());
      con.follow = true;
    }
    con.history_pos = -1;
    con.input[0] = '\0';
    ImGui::SetKeyboardFocusHere(-1);  // keep typing
  }
}

struct Page {
  const char* label;
  void (*draw)(f2::PageContext&);
};
// [input-port] 2026-11-24 CONTROLS after GAME, as in Call of Duty 3's window.
const Page kPages[] = {{"VIDEO", DrawVideo},         {"GAME", DrawGame},       {"CONTROLS", DrawControls},
                       {"INTERFACE", DrawInterface}, {"CONSOLE", DrawConsole}};
constexpr int kConsolePage = 4;

// ------------------------------------------------------------------------------------------------ the window
void Save() {
  std::string error;
  g_status = fh_f2::Save(&error) ? "Saved to fh.toml" : error;
  g_page.dirty = false;
  std::fprintf(stderr, "[f2] %s\n", g_status.c_str());
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
  const bool shown = ImGui::Begin("CALL OF DUTY: FINEST HOUR  \xC2\xB7  SETTINGS###fh_settings", &keep_open,
                                  ImGuiWindowFlags_NoCollapse);
  ImGui::PopFont();
  if (shown && ImGui::BeginTabBar("##pages")) {
    const int step = g_pad.TakePageStep();
    static int current = 0;
    const int pages = static_cast<int>(std::size(kPages));
    if (step) g_wanted_tab = (current + step + pages) % pages;
    for (int i = 0; i < pages; ++i) {
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

void SetOpen(bool open) {
  if (open == g_open) return;
  g_open = open;
  if (!open) {
    g_wait_release = true;               // the Escape or Back that closed it doesn't reach the game
    if (g_page.dirty) Save();            // closing saves, as in every other game
  }
  std::fprintf(stderr, "[f2] window %s\n", open ? "open" : "closed");
}

// ------------------------------------------------------------------------------------------------ Vulkan
VkDescriptorPool g_pool = VK_NULL_HANDLE;
VkRenderPass g_pass = VK_NULL_HANDLE;
VkFramebuffer g_framebuffer = VK_NULL_HANDLE;
VkImageView g_framebuffer_view = VK_NULL_HANDLE;
struct ngl_vk_backend g_vk{};

bool MakeFramebuffer() {
  if (g_framebuffer) vkDestroyFramebuffer(g_vk.device, g_framebuffer, nullptr);
  g_framebuffer = VK_NULL_HANDLE;
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
// when it calls the overlay (TRANSFER_SRC_OPTIMAL), and leaves it so for ngl_vk's copies (Big Red One's pass).
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

// What this window has on ngl_vk's device: its descriptor pool, its pass and framebuffer, and ImGui's backend.
bool StartVulkan() {
  if (g_vk_ready) return true;
  if (ngl_vk_backend(&g_vk) != 0) return false;
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool.maxSets = 16;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  if (vkCreateDescriptorPool(g_vk.device, &pool, nullptr, &g_pool) != VK_SUCCESS || !MakePass()) return false;
  ImGui_ImplVulkan_InitInfo info{};
  info.ApiVersion = VK_API_VERSION_1_0;
  info.Instance = g_vk.instance;
  info.PhysicalDevice = g_vk.gpu;
  info.Device = g_vk.device;
  info.QueueFamily = g_vk.queue_family;
  info.Queue = g_vk.queue;  // used by the backend only to upload its font picture (ngl_vk's own device)
  info.DescriptorPool = g_pool;
  info.MinImageCount = 2;
  info.ImageCount = 2;
  info.PipelineInfoMain.RenderPass = g_pass;
  info.PipelineInfoMain.Subpass = 0;
  info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  if (!ImGui_ImplVulkan_Init(&info)) return false;
  g_vk_ready = true;
  return true;
}

void StopVulkan() {
  if (!g_vk_ready) return;
  g_vk_ready = false;
  g_skip_draw = true;  // the picture already made this frame names textures of the old device
  vkDeviceWaitIdle(g_vk.device);
  ImGui_ImplVulkan_Shutdown();
  if (g_framebuffer) vkDestroyFramebuffer(g_vk.device, g_framebuffer, nullptr);
  if (g_pass) vkDestroyRenderPass(g_vk.device, g_pass, nullptr);
  if (g_pool) vkDestroyDescriptorPool(g_vk.device, g_pool, nullptr);
  g_framebuffer = VK_NULL_HANDLE;
  g_framebuffer_view = VK_NULL_HANDLE;
  g_pass = VK_NULL_HANDLE;
  g_pool = VK_NULL_HANDLE;
}

void DrawOverlay(VkCommandBuffer commands, void*) {
  ImDrawData* data = ImGui::GetDrawData();
  if (!g_ready || !g_vk_ready || g_skip_draw || !data || data->CmdListsCount == 0) return;  // closed: nothing spent
  struct ngl_vk_backend now{};
  if (ngl_vk_backend(&now) == 0 && (now.target_view != g_framebuffer_view || now.width != g_vk.width ||
                                    now.height != g_vk.height)) {
    g_vk = now;  // the target was made again on the same device
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

// ------------------------------------------------------------------------------------------------ the layout
// How the target sits in the window (ngl_vk fits it in, centered): the window's pixels per point, the window's pixels
// per target pixel, and the picture's top left corner in the window's pixels.
struct Shown {
  float density = 1.0f, scale = 1.0f, left = 0.0f, top = 0.0f;
  bool known = false;
};

Shown WhereShown() {
  Shown s;
  int ww = 0, wh = 0, pw = 0, ph = 0;
  if (!g_window || !SDL_GetWindowSize(g_window, &ww, &wh) || !SDL_GetWindowSizeInPixels(g_window, &pw, &ph) ||
      ww <= 0 || wh <= 0 || pw <= 0 || ph <= 0 || !g_vk.width || !g_vk.height)
    return s;
  s.density = static_cast<float>(pw) / static_cast<float>(ww);
  s.scale = std::min(static_cast<float>(pw) / static_cast<float>(g_vk.width),
                     static_cast<float>(ph) / static_cast<float>(g_vk.height));
  s.left = (static_cast<float>(pw) - static_cast<float>(g_vk.width) * s.scale) * 0.5f;
  s.top = (static_cast<float>(ph) - static_cast<float>(g_vk.height) * s.scale) * 0.5f;
  s.known = s.scale > 0.0f;
  return s;
}

// ------------------------------------------------------------------------------------------------ test runs
// FH_F2_TEST="<seconds>:<step>|...", the seconds from the window's first frame, as Big Red One's BRO_F2_TEST. Steps:
// open, close, page <LABEL>, set <name> <value> (as a row would), print <name>, save, console <line> (as typed on the
// CONSOLE page, [acct2-fh] 11-14). Each runs once, in order. A
// comma stands for a space (the test scripts split their variables at spaces): "70:set,display.fit,stretch". Test
// runs point FH_SETTINGS at a scratch copy of fh.toml, since closing and save write it.
struct TestStep {
  double seconds;
  std::string action;
};
std::vector<TestStep> g_test;
size_t g_test_next = 0;
Uint64 g_first_ms = 0;

void ReadTest() {
  const char* text = std::getenv("FH_F2_TEST");
  if (!text || !*text) return;
  std::string all(text);
  size_t at = 0;
  while (at <= all.size()) {
    const size_t bar = all.find('|', at);
    const std::string step = all.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
    const size_t colon = step.find(':');
    if (colon != std::string::npos) {
      std::string action = step.substr(colon + 1);
      std::replace(action.begin(), action.end(), ',', ' ');
      g_test.push_back({std::atof(step.substr(0, colon).c_str()), action});
    }
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
    if (a == "open") {
      SetOpen(true);
    } else if (a == "close") {
      SetOpen(false);
    } else if (a == "save") {
      Save();
    } else if (a.rfind("page ", 0) == 0) {
      for (int i = 0; i < static_cast<int>(std::size(kPages)); ++i)
        if (a.substr(5) == kPages[i].label) g_wanted_tab = i;
    } else if (a.rfind("set ", 0) == 0) {
      const size_t space = a.find(' ', 4);
      const std::string name = a.substr(4, space == std::string::npos ? std::string::npos : space - 4);
      const std::string value = space == std::string::npos ? "" : a.substr(space + 1);
      const bool ok = rex::cvar::SetFlagByName(name, value);
      g_page.dirty |= ok;
      std::fprintf(stderr, "[f2] test: set %s \"%s\" %s\n", name.c_str(), value.c_str(), ok ? "ok" : "refused");
    } else if (a.rfind("console ", 0) == 0) {
      const unsigned before = fh_settings_generation();
      fh_console_run(a.c_str() + 8);
      if (fh_settings_generation() != before) g_page.dirty = true;
    } else if (a.rfind("print ", 0) == 0) {
      const std::string name = a.substr(6);
      const rex::cvar::FlagInfo* info = rex::cvar::GetFlagInfo(name);
      std::fprintf(stderr, "[f2] test: %s = \"%s\"%s\n", name.c_str(), rex::cvar::GetFlagByName(name).c_str(),
                   !info ? " (no such setting)"
                   : info->lifecycle == rex::cvar::Lifecycle::kHotReload ? " (applies at once)"
                                                                         : " (applies after a restart)");
    }
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------ the C side
extern "C" int fh_f2_init(SDL_Window* window) {
  const char* off = std::getenv("FH_F2");
  if (g_ready) return 0;
  if (off && off[0] == '0') {
    std::fprintf(stderr, "[f2] off (FH_F2=0)\n");
    return -1;
  }
  if (!window) return -1;
  g_window = window;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // the window's place isn't kept between runs
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  f2::LookSettings look;
  look.theme_name = "interface.theme";
  look.theme = &fh_f2::ThemeStorage;
  look.text_size_name = "interface.text_size";
  look.text_size = &fh_f2::TextSizeStorage;
  look.opacity_name = "interface.opacity";
  look.opacity = &fh_f2::OpacityStorage;
  f2::SetLook(look);  // Finest Hour's own colors are still to be read from its menus: Call of Duty 3's meanwhile
  io.Fonts->AddFontDefault();  // the fixed-width font f2_theme keeps as MonoFont, as the SDK adds it first
  f2::ConfigureFonts(io.Fonts);
  f2::ApplyTheme(ImGui::GetStyle());
  if (!ImGui_ImplSDL3_InitForVulkan(window) || !StartVulkan()) {
    std::fprintf(stderr, "[f2] Dear ImGui didn't start on ngl_vk's device: the F2 window is off\n");
    return -1;
  }
  ngl_vk_set_overlay(DrawOverlay, nullptr);
  ReadTest();
  std::atexit(SaveAtExit);
  g_ready = true;
  std::fprintf(stderr, "[f2] ready, drawn at %ux%u (F2, or Back + Start held 1 s on a controller)\n", g_vk.width,
               g_vk.height);
  return 0;
}

extern "C" void fh_f2_events(void) {
  if (!g_ready || !g_vk_ready) return;
  // The keys and the text, then the mouse's buttons and wheel: the mouse's motion is between them in SDL's numbering
  // and isn't wanted here (a fast mouse's motion alone could fill these 128 and push F2 out). Peeked, not taken:
  // fh_present.c drops the queue after this.
  SDL_Event events[128];
  int n = SDL_PeepEvents(events, 128, SDL_PEEKEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_MOTION - 1);
  if (n < 0) n = 0;
  if (n < 128) {
    const int more =
        SDL_PeepEvents(events + n, 128 - n, SDL_PEEKEVENT, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_WHEEL);
    if (more > 0) n += more;
  }
  bool grave_typed = false;  // [acct2-fh] 11-14: the ` that ~ types isn't text for the console's line
  for (int i = 0; i < n; ++i) {
    const SDL_Event& e = events[i];
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.key == SDLK_F2) {
      SetOpen(!g_open);
      continue;
    }
    // ~ (the key left of 1, as in World at War) opens the window on the CONSOLE page, and closes it
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_GRAVE) {
      if (!e.key.repeat) {
        SetOpen(!g_open);
        if (g_open) {
          g_wanted_tab = kConsolePage;
          g_console.focus = true;
          g_console.follow = true;
        }
      }
      grave_typed = true;
      continue;
    }
    if (grave_typed && e.type == SDL_EVENT_TEXT_INPUT) {
      grave_typed = false;
      continue;
    }
    if (!g_open) continue;
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && !ImGui::GetIO().WantTextInput) {
      SetOpen(false);
      continue;
    }
    ImGui_ImplSDL3_ProcessEvent(&e);
  }
  if (g_pad.Poll(f2::GetBool("interface.pad_shortcut"))) SetOpen(!g_open);

  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGuiIO& io = ImGui::GetIO();
  // Laid out in the window's points, drawn into the target at its pixels per point (at least one).
  const Shown shown = WhereShown();
  const float per_point = shown.known ? std::max(1.0f, shown.density / shown.scale) : 1.0f;
  io.DisplaySize = ImVec2(static_cast<float>(g_vk.width) / per_point, static_cast<float>(g_vk.height) / per_point);
  io.DisplayFramebufferScale = ImVec2(per_point, per_point);
  if (g_open && shown.known) {
    float mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    const float tx = (mx * shown.density - shown.left) / shown.scale, ty = (my * shown.density - shown.top) / shown.scale;
    io.AddMousePosEvent(tx / per_point, ty / per_point);
  }
  if (!g_first_ms) g_first_ms = SDL_GetTicks();
  RunTest();
  ImGui::NewFrame();
  g_pad.FeedNavigation(io, g_open);
  f2::ApplyThemeIfChanged();
  if (g_open && !DrawWindow()) SetOpen(false);  // its close box
  ImGui::Render();  // drawn by DrawOverlay when ngl_vk reaches the end of this frame
  g_skip_draw = false;
}

extern "C" int fh_f2_game_gets_input(void) {
  if (!g_ready) return 1;
  if (g_open) return 0;
  if (g_wait_release) {
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    for (int i = 0; keys && i < count; ++i)
      if (keys[i]) return 0;
    g_wait_release = false;
  }
  return 1;
}

extern "C" void fh_f2_vulkan_stop(void) {
  if (g_ready) StopVulkan();
}

extern "C" void fh_f2_vulkan_start(void) {
  if (!g_ready || g_vk_ready) return;
  if (StartVulkan())
    std::fprintf(stderr, "[f2] made again on the new picture, %ux%u\n", g_vk.width, g_vk.height);
  else
    std::fprintf(stderr, "[f2] couldn't be made again on the new picture: the F2 window is off\n");
}

extern "C" void fh_f2_shutdown(void) {
  if (!g_ready) return;
  SaveAtExit();
}
