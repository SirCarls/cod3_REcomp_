// [acct2-fh] 2026-11-14 Finest Hour's F2 settings (see fh_f2_store.h and shim/rex/cvar.h), as Big Red One's
// bro_f2_store.cpp, over fh_settings.c.
//
// Each row's setting is a fh.toml key ("display.render_resolution"): read with fh_setting_text (its FH_* variable
// first, as the port reads it, unless the window changed it), changed with fh_settings_set (which then wins over the
// variable and counts a new generation), written by fh_settings_save. fh_present.c looks at the generation every
// frame, so a change to the display or the resolution takes effect at once (the window's size, mode and monitor, the
// render size, the picture's shape, V-Sync and the window's scaling).

#include "fh_f2_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>

extern "C" {
#include "fh_resolution.h"
#include "fh_settings.h"
}

namespace fh_f2 {
namespace {

enum class When { kAtOnce, kAtOnceWhenLive, kAtStart };

struct Setting {
  const char* key;
  const char* env;       // its FH_* variable, or nullptr
  const char* fallback;  // the default, as fh/config/fh.toml has it
  When when;
};

const Setting kSettings[] = {
    {"display.mode", "FH_DISPLAY_MODE", "window", When::kAtOnce},
    {"display.monitor", "FH_MONITOR", "", When::kAtOnce},
    {"display.refresh_rate", "FH_REFRESH_RATE", "0", When::kAtOnce},
    {"display.fullscreen_size", "FH_FULLSCREEN_SIZE", "", When::kAtOnce},
    {"display.window_size", "FH_WINDOW_SIZE", "1280x960", When::kAtOnce},
    {"display.vsync", "FH_VSYNC", "true", When::kAtOnce},      // ngl_vk_set_present_mode
    {"display.scaling", "FH_SCALING", "smooth", When::kAtOnce},  // ngl_vk_set_window_scaling
    {"display.cas_sharpness", "FH_CAS_SHARPNESS", "0", When::kAtOnce},  // ngl_vk_set_window_sharpness
    {"display.fsr_sharpness_reduction", "FH_FSR_SHARPNESS_REDUCTION", "0.2", When::kAtOnce},
    {"display.fsr_passes", "FH_FSR_PASSES", "4", When::kAtOnce},
    {"display.fit", "FH_FIT", "bars", When::kAtOnceWhenLive},
    {"display.render_resolution", "FH_RENDER_RESOLUTION", "auto", When::kAtOnceWhenLive},
    {"display.render_percent", "FH_RENDER_PERCENT", "100", When::kAtOnceWhenLive},
    {"display.live_resolution", "FH_LIVE_RESOLUTION", "true", When::kAtOnce},
    {"display.aspect_ratio", "FH_ASPECT_RATIO", "auto", When::kAtOnceWhenLive},
    {"display.max_fps", "FH_MAX_FPS", "console", When::kAtOnce},     // [acct2-fh] 11-14 fh_frame_pace.c
    {"display.fps_log", "FH_FPS_LOG", "0", When::kAtOnce},
    // [acct2-fh] 11-14: the console's language, which the game reads once at its start (main.c)
    {"game.language", "FH_LANGUAGE", "english", When::kAtStart},
    {"game.fine_frame_clock", "FH_FINE_FRAME_CLOCK", "true", When::kAtOnce},  // [acct2-fh] 11-14 fh_frame_clock.c
    {"game.frame_clocks_by_time", "FH_FRAME_CLOCKS_BY_TIME", "true", When::kAtOnce},
    // the window's own, in [interface] (no variables): at once, f2_theme reads the storage every frame
    {"interface.theme", nullptr, "cod3", When::kAtOnce},
    {"interface.text_size", nullptr, "16", When::kAtOnce},
    {"interface.opacity", nullptr, "0.94", When::kAtOnce},
    {"interface.pad_shortcut", nullptr, "true", When::kAtOnce},
};

const Setting* Find(std::string_view key) {
  for (const Setting& s : kSettings)
    if (key == s.key) return &s;
  return nullptr;
}

std::string g_theme;
int32_t g_text_size = 16;
double g_opacity = 0.94;
bool g_look_read = false;

void ReadLook() {
  if (g_look_read) return;
  g_look_read = true;
  const char* v = fh_setting_text("interface.theme", nullptr);
  g_theme = v && v[0] ? v : "cod3";
  g_text_size = fh_setting_int("interface.text_size", nullptr, 16);
  g_opacity = fh_setting_number("interface.opacity", nullptr, 0.94);
}

std::unordered_map<std::string, rex::cvar::FlagInfo> g_infos;  // node-based: entries never move

}  // namespace

std::string& ThemeStorage() {
  ReadLook();
  return g_theme;
}
int32_t& TextSizeStorage() {
  ReadLook();
  return g_text_size;
}
double& OpacityStorage() {
  ReadLook();
  return g_opacity;
}

bool IsSetting(const char* key) { return Find(key) != nullptr; }

bool Save(std::string* error) {
  if (fh_settings_save() == 0) return true;
  if (error) *error = "fh.toml couldn't be written";
  return false;
}

}  // namespace fh_f2

// ---------------------------------------------------------------------------------------------------- rex::cvar
namespace rex::cvar {

using fh_f2::Find;

const FlagInfo* GetFlagInfo(std::string_view name) {
  const fh_f2::Setting* s = Find(name);
  if (!s) return nullptr;
  const std::string key(name);
  FlagInfo& info = fh_f2::g_infos[key];
  info.name = key;
  info.default_value = s->fallback;
  // The render size and the shape follow the settings while the game runs only with live_resolution on.
  const bool at_once = s->when == fh_f2::When::kAtOnce ||
                       (s->when == fh_f2::When::kAtOnceWhenLive && fh_resolution_live());
  info.lifecycle = at_once ? Lifecycle::kHotReload : Lifecycle::kRequiresRestart;
  return &info;
}

std::string GetFlagByName(std::string_view name) {
  const fh_f2::Setting* s = Find(name);
  if (!s) return "";
  const std::string key(name);
  if (key == "interface.theme") return fh_f2::ThemeStorage();
  if (key == "interface.text_size") return std::to_string(fh_f2::TextSizeStorage());
  if (key == "interface.opacity") {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", fh_f2::OpacityStorage());
    return text;
  }
  const char* v = fh_setting_text(s->key, s->env);
  return v ? v : s->fallback;
}

bool SetFlagByName(std::string_view name, std::string_view value) {
  const fh_f2::Setting* s = Find(name);
  if (!s) return false;
  const std::string key(name), text(value);
  if (fh_settings_set(s->key, text.c_str()) != 0) return false;
  if (key == "interface.theme") fh_f2::ThemeStorage() = text;
  if (key == "interface.text_size") fh_f2::TextSizeStorage() = std::atoi(text.c_str());
  if (key == "interface.opacity") fh_f2::OpacityStorage() = std::strtod(text.c_str(), nullptr);
  return true;
}

}  // namespace rex::cvar
