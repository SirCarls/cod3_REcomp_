// [f2-menus] Big Red One's F2 settings (see bro_f2_store.h and shim/rex/cvar.h).
//
// Two kinds of setting, told apart by a dot in the name:
//   - the port's own, in bro.toml ("display.fov"): read with bro_setting_text (its BRO_* variable first, as the port
//     reads it), changed with bro_settings_set (which then wins over the variable), written by bro_settings_save. A
//     change takes effect at once when the setting's reader has a reload function (proposed to each reader's owner in
//     dev/agents/f2-menus/bro/BRO-F2.md); until then the row says "restart".
//   - the game's own cvars ("cg_drawGun"): bro_game_cvars.c.
// And a third since 2026-11-11: settings a shared file makes itself with REXCVAR_DEFINE_* (f2_accel.cpp's f2_accel_*
// and f2_sens_*; shim/rex/cvar.h). Each lives in that file's own variable and is kept in bro.toml's [mouse].

#include "bro_f2_store.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>

#if defined(BRO_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

extern "C" {
#include "bro_game_cvars.h"
#include "bro_settings.h"
}

// The readers' reload functions: each puts its setting's "read once" static back to "not read". They are proposed
// (BRO-F2.md); weak here, so a build without one still links and its row says "restart".
#if defined(__GNUC__) && !defined(_WIN32)
#define BRO_F2_WEAK __attribute__((weak))
extern "C" {
void bro_pad_reload(void) BRO_F2_WEAK;          // bro_pad.c / bro_binds.c: read_binds() again
void bro_view_reload(void) BRO_F2_WEAK;         // bro_view.c: setting = -1
void ngl_fps_counter_reload(void) BRO_F2_WEAK;  // ngl_fps_counter.c: enabled = -1
void ngl_frame_clock_reload(void) BRO_F2_WEAK;  // ngl_frame_clock.c: read_frame_limit() again
void ngl_movie_reload(void) BRO_F2_WEAK;        // ngl_movie.c: setting = skip = -1
}
#define RELOAD(fn) (fn)
#else
#define RELOAD(fn) nullptr
#endif

namespace bro_f2 {
namespace {

SDL_Window* g_window = nullptr;
void (*const kPadReload)() = RELOAD(bro_pad_reload);

void ApplyFullscreen() {
#if defined(BRO_HAVE_SDL3)
  if (g_window) SDL_SetWindowFullscreen(g_window, bro_setting_bool("display.fullscreen", "BRO_FULLSCREEN", 0) != 0);
#endif
}

struct PortSetting {
  const char* key;
  const char* env;       // its BRO_* variable, or nullptr
  const char* fallback;  // the default, as bro/config/bro.toml has it
  bool restart;          // read once when the window or the game starts
  void (*reload)();      // makes a change take effect at once; nullptr: at the next start (unless restart is false
                         // and there is nothing to reload, as for the window's own [interface] settings)
};

const PortSetting kSettings[] = {
    {"display.fullscreen", "BRO_FULLSCREEN", "false", false, ApplyFullscreen},
    {"display.vsync", "BRO_VSYNC", "true", true, nullptr},
    {"display.size", "BRO_VK_SIZE", "", true, nullptr},
    {"display.max_fps", "BRO_MAX_FPS", "", false, RELOAD(ngl_frame_clock_reload)},
    {"display.show_fps", "BRO_SHOW_FPS", "false", false, RELOAD(ngl_fps_counter_reload)},
    {"display.fps_log", "BRO_FPS_LOG", "0", false, RELOAD(ngl_frame_clock_reload)},
    {"display.fov", "BRO_FOV", "", false, RELOAD(bro_view_reload)},
    {"display.movie_pacing", "BRO_MOVIE_PACING", "true", false, RELOAD(ngl_movie_reload)},
    {"display.skip_movies", "BRO_SKIP_MOVIES", "false", false, RELOAD(ngl_movie_reload)},
    {"input.raw_mouse", "BRO_RAW_MOUSE", "true", false, RELOAD(bro_pad_reload)},
    {"input.mouse_sensitivity", "BRO_MOUSE_SENSITIVITY", "5", false, RELOAD(bro_pad_reload)},
    {"input.mouse_scale_x", "BRO_MOUSE_SCALE_X", "1.0", false, RELOAD(bro_pad_reload)},
    {"input.mouse_scale_y", "BRO_MOUSE_SCALE_Y", "1.0", false, RELOAD(bro_pad_reload)},
    {"input.invert_y", "BRO_INVERT_Y", "false", false, RELOAD(bro_pad_reload)},
    {"input.zoom_sensitivity", "BRO_ZOOM_SENSITIVITY", "1.0", false, RELOAD(bro_pad_reload)},
    {"input.mouse_speed", "BRO_MOUSE_SPEED", "1600", false, RELOAD(bro_pad_reload)},
    {"input.dead_zone", "BRO_DEAD_ZONE", "0", false, RELOAD(bro_pad_reload)},
    {"input.binds", "BRO_BINDS", "true", false, RELOAD(bro_pad_reload)},
};

// The window's own, in bro.toml's [interface]: they take effect at once (f2_theme reads the storage every frame).
struct LookSetting {
  const char* key;
  const char* fallback;
};
const LookSetting kLook[] = {
    {"interface.theme", "cod3"},
    {"interface.text_size", "16"},
    {"interface.opacity", "0.94"},
    {"interface.pad_shortcut", "true"},
};

const PortSetting* FindPort(std::string_view key) {
  for (const PortSetting& s : kSettings)
    if (key == s.key) return &s;
  return nullptr;
}

const LookSetting* FindLook(std::string_view key) {
  for (const LookSetting& s : kLook)
    if (key == s.key) return &s;
  return nullptr;
}

// [binds], [pad] and [keys]: lists the port reads with bro_pad (no variables).
bool IsListKey(std::string_view key) {
  return key.rfind("binds.", 0) == 0 || key.rfind("pad.", 0) == 0 || key.rfind("keys.", 0) == 0;
}

std::string g_theme;
int32_t g_text_size = 16;
double g_opacity = 0.94;
bool g_look_read = false;

void ReadLook() {
  if (g_look_read) return;
  g_look_read = true;
  const char* v = bro_setting_text("interface.theme", nullptr);
  g_theme = v ? v : "cod3";
  g_text_size = bro_setting_int("interface.text_size", nullptr, 16);
  g_opacity = bro_setting_number("interface.opacity", nullptr, 0.94);
}

std::unordered_map<std::string, rex::cvar::FlagInfo> g_infos;  // node-based: entries never move

// [f2-menus] 2026-11-11 The settings made with REXCVAR_DEFINE_* (shim/rex/cvar.h). They are made while the program
// starts, before main, so the list is made at its first use.
struct DefinedSetting {
  enum Type { kBool, kInt, kDouble };
  std::string name;      // as its file calls it: "f2_accel_mode"
  std::string key;       // in bro.toml: "mouse.accel_mode"
  std::string env;       // its variable: "BRO_MOUSE_ACCEL_MODE"
  std::string fallback;  // the default, as text
  Type type;
  void* storage;         // the file's own variable
};

std::deque<DefinedSetting>& DefinedList() {
  static std::deque<DefinedSetting> list;
  return list;
}

std::string DefinedText(const DefinedSetting& d) {
  char text[40];
  switch (d.type) {
    case DefinedSetting::kBool:
      return *static_cast<bool*>(d.storage) ? "true" : "false";
    case DefinedSetting::kInt:
      std::snprintf(text, sizeof(text), "%d", static_cast<int>(*static_cast<int32_t*>(d.storage)));
      return text;
    default:
      std::snprintf(text, sizeof(text), "%.10g", *static_cast<double*>(d.storage));
      return text;
  }
}

bool IsOn(const char* text) {
  return !std::strcmp(text, "1") || !std::strcmp(text, "true") || !std::strcmp(text, "yes") || !std::strcmp(text, "on");
}

void DefinedTake(const DefinedSetting& d, const char* text) {
  switch (d.type) {
    case DefinedSetting::kBool:
      *static_cast<bool*>(d.storage) = IsOn(text);
      break;
    case DefinedSetting::kInt:
      *static_cast<int32_t*>(d.storage) = static_cast<int32_t>(std::strtol(text, nullptr, 10));
      break;
    default:
      *static_cast<double*>(d.storage) = std::strtod(text, nullptr);
      break;
  }
}

void AddDefined(const char* name, DefinedSetting::Type type, void* storage) {
  DefinedSetting d;
  d.name = name;
  const std::string bare = d.name.rfind("f2_", 0) == 0 ? d.name.substr(3) : d.name;
  d.key = "mouse." + bare;
  d.env = "BRO_MOUSE_";
  for (const char ch : bare) d.env += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  d.type = type;
  d.storage = storage;
  d.fallback = DefinedText(d);  // the variable still has the value its file gave it
  DefinedList().push_back(std::move(d));
}

// By the file's name for it or by its bro.toml key.
const DefinedSetting* FindDefined(std::string_view name) {
  for (const DefinedSetting& d : DefinedList())
    if (name == d.name || name == d.key) return &d;
  return nullptr;
}

}  // namespace

void SetWindow(SDL_Window* window) { g_window = window; }

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

bool IsPortSetting(const char* key) { return FindPort(key) || FindLook(key) || IsListKey(key) || FindDefined(key); }

bool AppliesAtOnce(const char* key) {
  if (FindLook(key) || FindDefined(key)) return true;
  const PortSetting* s = FindPort(key);
  if (s) return !s->restart && s->reload;
  return IsListKey(key) && kPadReload;
}

bool Save(std::string* error) {
  if (bro_settings_save() == 0) return true;
  if (error) *error = "bro.toml couldn't be written";
  return false;
}

}  // namespace bro_f2

// ---------------------------------------------------------------------------------------------------- rex::cvar
namespace rex::cvar {

using namespace bro_f2;

namespace shim {
Defined Define(const char* name, bool* storage) {
  AddDefined(name, DefinedSetting::kBool, storage);
  return {};
}
Defined Define(const char* name, int32_t* storage) {
  AddDefined(name, DefinedSetting::kInt, storage);
  return {};
}
Defined Define(const char* name, double* storage) {
  AddDefined(name, DefinedSetting::kDouble, storage);
  return {};
}
void Load() {
  for (const DefinedSetting& d : DefinedList())
    if (const char* v = bro_setting_text(d.key.c_str(), d.env.c_str())) DefinedTake(d, v);
}
}  // namespace shim

const FlagInfo* GetFlagInfo(std::string_view name) {
  const std::string key(name);
  FlagInfo info;
  info.name = key;
  if (const DefinedSetting* d = FindDefined(key)) {
    info.default_value = d->fallback;  // takes effect at once: its file reads the variable every time
  } else if (key.find('.') != std::string::npos) {
    if (const LookSetting* l = FindLook(key)) {
      info.default_value = l->fallback;
    } else if (const PortSetting* p = FindPort(key)) {
      info.default_value = p->fallback;
      info.lifecycle = AppliesAtOnce(key.c_str()) ? Lifecycle::kHotReload : Lifecycle::kRequiresRestart;
    } else if (IsListKey(key)) {
      info.lifecycle = AppliesAtOnce(key.c_str()) ? Lifecycle::kHotReload : Lifecycle::kRequiresRestart;
    } else {
      return nullptr;
    }
  } else {
    bro_cvar c;
    if (!bro_game_cvar_find(key.c_str(), &c)) return nullptr;
    info.default_value = c.reset ? c.reset : "";
    // A LATCH cvar takes a change at the next level; the window says "restart" for it.
    info.lifecycle = (c.flags & BRO_CVAR_LATCH) ? Lifecycle::kRequiresRestart : Lifecycle::kHotReload;
  }
  FlagInfo& kept = g_infos[key];
  kept = std::move(info);
  return &kept;
}

std::string GetFlagByName(std::string_view name) {
  const std::string key(name);
  if (const DefinedSetting* d = FindDefined(key)) return DefinedText(*d);
  if (key.find('.') != std::string::npos) {
    if (key == "interface.theme") return ThemeStorage();
    if (key == "interface.text_size") return std::to_string(TextSizeStorage());
    if (key == "interface.opacity") {
      char text[32];
      std::snprintf(text, sizeof(text), "%.2f", OpacityStorage());
      return text;
    }
    const PortSetting* p = FindPort(key);
    const LookSetting* l = FindLook(key);
    const char* v = bro_setting_text(key.c_str(), p ? p->env : nullptr);
    if (v) return v;
    return p ? p->fallback : (l ? l->fallback : "");
  }
  // What the game has (a LATCH cvar: the value it waits with). A change goes in at this frame's end, so the next
  // picture of the window shows it.
  bro_cvar c;
  if (!bro_game_cvar_find(key.c_str(), &c)) return "";
  return c.latched ? c.latched : c.value;
}

bool SetFlagByName(std::string_view name, std::string_view value) {
  const std::string key(name);
  std::string text(value);
  if (const DefinedSetting* d = FindDefined(key)) {
    // Kept as the variable reads it back ("true", "3", "0.1"), so bro.toml gets a clean value.
    const std::string before = DefinedText(*d);
    DefinedTake(*d, text.c_str());
    if (bro_settings_set(d->key.c_str(), DefinedText(*d).c_str()) == 0) return true;
    DefinedTake(*d, before.c_str());
    return false;
  }
  if (key.find('.') != std::string::npos) {
    if (!IsPortSetting(key.c_str()) || bro_settings_set(key.c_str(), text.c_str()) != 0) return false;
    if (key == "interface.theme") ThemeStorage() = text;
    if (key == "interface.text_size") TextSizeStorage() = std::atoi(text.c_str());
    if (key == "interface.opacity") OpacityStorage() = std::strtod(text.c_str(), nullptr);
    const PortSetting* p = FindPort(key);
    if (p && p->reload) p->reload();
    if (IsListKey(key) && kPadReload) kPadReload();
    return true;
  }
  bro_cvar c;
  if (bro_game_cvar_find(key.c_str(), &c) && bro_game_cvar_read_only(c.flags)) return false;
  // The rows write switches as true/false; the game's switches are 1/0.
  if (text == "true") text = "1";
  if (text == "false") text = "0";
  bro_game_cvar_set(key.c_str(), text.c_str());
  return true;
}

}  // namespace rex::cvar
