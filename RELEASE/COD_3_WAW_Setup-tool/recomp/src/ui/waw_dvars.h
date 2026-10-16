// cod3 - World at War dvar names in the console.
//
// Call of Duty 3 already has many of World at War's dvars under the same
// name (cg_fov, cg_drawCrosshair, timescale ...). For the rest:
//
// - Game aliases: the same setting under another name in CoD3, e.g. WaW's
//   cg_gun_x is CoD3's cg_gunX and bg_viewKickScale is cg_viewKickScale x 10.
//   Typing the WaW name reads or sets the CoD3 dvar (converted where the
//   scales differ), through the game, so cheat protection still applies.
// - Port settings: things the PC port does itself, e.g. sensitivity, m_yaw,
//   r_fullscreen, r_mode, r_vsync, snd_volume. They change the matching F2
//   setting and are saved with it.
// - Everything else World at War has: the console says it's a World at War
//   dvar that Call of Duty 3 has nothing for, instead of "unknown command".

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "game/engine.h"

namespace cod3::ui::waw {

enum class Kind {
  kGame,  // a CoD3 dvar under another name
  kPort,  // a setting of the PC port (rex cvar)
};

// How a World at War value becomes the target's value (and back).
enum class Conv {
  kSame,
  kTimes10,     // CoD3 = WaW x 10
  kPairMax,     // WaW "h v" pair -> CoD3's single value (the larger)
  kFadeTime,    // WaW 0 = never fade -> a very long time
  kAspect,      // auto / standard / wide 16:10 / wide 16:9 -> cg_widescreen 0/1
  kBool,        // 0/1 -> false/true
  kNotBool,     // 0/1 -> true/false
  kAimAssist,   // 0/1 -> F2 aim assist off/on
  kAniso,       // 1/2/4/8/16 -> anisotropic_override 1..5 (0 = off)
  kResolution,  // "1920x1080" -> cod3_render_resolution
  kGameMsaa,    // samples 1 -> off, 2+ -> on when it fits
  kVsync,       // 0/1 -> the two presentation cvars
  kShadows,     // 0/1 -> cod3_shadow_quality off / original
};

struct Dvar {
  const char* name;    // World at War's name
  Kind kind;
  const char* target;  // CoD3 dvar or port setting
  Conv conv;
  const char* help;    // one line
};

const std::vector<Dvar>& All();
// Case-insensitive.
const Dvar* Find(std::string_view name);
// A World at War dvar Call of Duty 3 has nothing for.
bool IsWawOnly(std::string_view name);
const std::vector<std::string_view>& WawOnlyNames();

// The value to show for `d`, on World at War's scale. Game dvars come from
// the snapshot (empty while the game hasn't registered them yet).
std::string Value(const Dvar& d, const EngineSnapshot& snap);

struct SetResult {
  bool ok = true;
  std::string game_command;  // to run in the game (game aliases)
  std::string message;       // to print (port settings, errors)
  bool needs_restart = false;
  bool changed_port_setting = false;
};
SetResult Set(const Dvar& d, const std::string& value);

// Rewrites a command whose dvar name is a World at War alias:
// "cg_gun_x 2", "set cg_gun_x 2", "seta ...", "reset ...", "toggle ...".
// Returns false when nothing in it is an alias.
bool RewriteGameAlias(const std::vector<std::string>& args, std::vector<std::string>* out);

}  // namespace cod3::ui::waw
