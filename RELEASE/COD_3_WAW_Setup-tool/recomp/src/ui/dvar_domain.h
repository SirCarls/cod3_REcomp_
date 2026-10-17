// cod3 - what values a dvar or port setting takes, shown World at War style.
//
// World at War answers a dvar name typed alone with
//   "cg_fov" is:"65^7" default:"65^7"
//     Domain is any number from 65 to 80
// and its completion box shows the same domain. Call of Duty 3 prints the
// first line itself (Cvar_Command's format is the same, Quake III's; retail
// shows it only with developer 1, the port always), but its
// dvars are Quake III cvars: a cvar_t is 44 bytes (name, string, reset
// string, latched string, flags, modified, modification count, value,
// integer, next, hash next; Cvar_Get 0x8252BBE0 lays them out 44 bytes
// apart) with no type and no domain. The game takes any text and reads it as
// a number where it wants one. So the domains here come from, in order:
//   1. the port's own limits, for the dvars the port governs: cg_fov and
//      cg_fovScale (game/fov.h, the effective 65-165), com_maxfps (the frame
//      limiter's 0-1000), cg_drawFPS (World at War's four modes);
//   2. what the game does with the value where that is plain: on/off
//      switches, 0-1 fractions, the difficulty (kKnownDomains in the .cpp);
//   3. otherwise the default value's form: a number gives "any number"
//      (Quake III reads every cvar as both a float and an integer), anything
//      else "any text".
// Port settings (rex cvars) carry their own type, range and allowed values.
// World at War names (waw_dvars.h) take their target's domain, converted.
//
// Nothing here runs per frame: the console asks when a line is printed or
// the completion list changes.

#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "game/engine.h"

namespace rex::cvar {
struct FlagEntry;
}

namespace cod3::ui::waw {
struct Dvar;
}

namespace cod3::ui::domain {

struct Domain {
  enum class Kind : uint8_t {
    kText,    // "Domain is any text"
    kBool,    // "Domain is 0 or 1"
    kInt,     // "Domain is any integer from 0 to 1000" (or open-ended)
    kFloat,   // "Domain is any number from 65 to 165" (or open-ended)
    kEnum,    // "Domain is one of the following:" and the numbered names
    kCustom,  // a sentence of its own, for formats World at War has no words for
  };
  Kind kind = Kind::kText;
  double min = -std::numeric_limits<double>::infinity();
  double max = std::numeric_limits<double>::infinity();
  std::vector<std::string> choices;  // kEnum: the names; a name's number is its index
  std::string custom;                // kCustom: the words after "Domain is "
  std::string note;                  // shown on the line after the domain (the port's limits)
};

// World at War's lines for a domain, without indentation: "Domain is 0 or 1",
// or "Domain is one of the following:" followed by "   0: Off" ...
std::vector<std::string> Describe(const Domain& d);
// A few words for completion lists: "0 or 1", "65 to 165", "any number",
// "Off/Simple/SimpleRanges/Verbose", "text".
std::string Brief(const Domain& d);

// A Call of Duty 3 dvar, by name and default (reset) value.
Domain ForGameDvar(std::string_view name, std::string_view reset_value);
// A port setting (rex cvar).
Domain ForPortSetting(const rex::cvar::FlagEntry& info);
// A World at War name (ui/waw_dvars.h).
Domain ForWaw(const waw::Dvar& d, const EngineSnapshot& snap);

// A port setting's value as the console shows it: booleans as 1 / 0 like
// World at War's dvars, everything else as stored.
std::string PortValueText(const rex::cvar::FlagEntry& info, const std::string& value);

// The whole answer to a name typed alone, World at War style, one string
// per line (no '\n'):
//   "cg_fov" is:"65^7" default:"65^7"
//     Domain is any number from 65 to 165
std::vector<std::string> InfoLines(std::string_view name, std::string_view value, std::string_view reset,
                                   const Domain& d);

// For the game's own answer to a dvar name typed alone (Cvar_Command prints
// "\"%s\" is:\"%s^7\" default:\"%s^7\"\n", through Com_DPrintf in this build;
// game/print_hooks.cpp always shows it): the domain lines that World at War
// prints after it, ready to print ("  Domain is ...\n"), or empty when
// `printed` is any other line. Any thread.
std::string LinesAfterGameAnswer(std::string_view printed);

}  // namespace cod3::ui::domain
