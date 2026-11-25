// codclassic - the port's defaults for the SDK's own settings. [acct2-uo]
//
// Each default applies only while the player hasn't set the setting (command line, environment or
// codclassic.toml), so a player's choice always wins:
//
//   mnk_mode = true                the SDK's keyboard pad, as Call of Duty 3's and Call of Duty 2's
//                                  ports set it: the keyboard works the menus like a controller
//   keybind_start = Escape,Return  START on Return or Escape (the 360's front end waits for START)
//   guest_vblank_rate = 1000       the guest's vblank at 1000 Hz instead of the 360's 60: the
//                                  console's Direct3D waits for a vblank before each frame, which at
//                                  60 Hz is a hidden 60 fps cap (owner 2026-11-07: every game free
//                                  of the 360's limits). The port's own frame limit
//                                  (codclassic_max_fps, frame_rate.cpp) paces the frames instead.

#include "game/port_defaults.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace codclassic::port_defaults {

namespace {

void SetIfUnset(const char* name, const char* value) {
  if (rex::cvar::GetFlagInfo(name) && rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagByName(name, value);
}

std::string Flag(const char* name) {
  return rex::cvar::GetFlagInfo(name) ? std::string(rex::cvar::GetFlagByName(name)) : std::string("?");
}

// "1000.000000" -> "1000".
std::string ShortNumber(std::string text) {
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
  }
  return text;
}

}  // namespace

void ApplyInputDefaults() {
  SetIfUnset("mnk_mode", "true");
  SetIfUnset("keybind_start", "Escape,Return");
  REXLOG_INFO("codclassic: keyboard pad {} (START {}, A {})", Flag("mnk_mode"), Flag("keybind_start"),
              Flag("keybind_a"));
}

void ApplyGpuDefaults() {
  static bool logged = false;
  SetIfUnset("guest_vblank_rate", "1000");
  if (logged) return;
  logged = true;
  REXLOG_INFO("codclassic: guest vblank {} Hz (the 360's is 60)", ShortNumber(Flag("guest_vblank_rate")));
}

}  // namespace codclassic::port_defaults
