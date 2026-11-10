// cod2 - from launch to the main menu without test settings. [acct2-cod2]
//
// On the Xbox 360 the game starts with four films (the legal screen,
// Activision, Infinity Ward and the intro: about 150 s, A skips each), then its
// Press START screen (the main_lockout menu): START picks the controller, the
// SDK's device selector answers the storage question with the hard drive, and
// the main menu opens (main_text). On the PC that takes the keyboard, so:
//
//   keyboard   the SDK's keyboard pad is on (mnk_mode, as Call of Duty 3 sets
//              it): Return or Escape is START, Space is A, the rest the SDK's
//              own binds. Each only while the player hasn't set it (command
//              line, environment or cod2.toml). Called from Cod2App's
//              OnPreSetup, before the input drivers start.
//   cod2_skip_intro_movies  default false (as CoD3's cod3_skip_movies): the
//              four films aren't started at all, so the Press START screen
//              opens a few seconds after launch.
//
// The films come from COM_PlayIntroMovies 0x8233B3F0 (the decompile in
// dev/waw-decomp/cod2_360/sp): when neither of two launch flags (the bytes
// 0x8414E4D8 and 0x8414E154) is set, com_introPlayed is false and a second
// switch is on, it queues "cinematic legal", "atvi", "IW_logo" and
// "cod2_intro" in the command buffer; when the first flag is set it clears it
// and calls XamLoaderSetLaunchData instead. The hook leaves out only the films.

#include "game/front_end.h"

#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(cod2_skip_intro_movies, false, "cod2/Game",
                    "Skip the four start-up films (legal screen, Activision, Infinity Ward, the "
                    "intro): the Press START screen comes at once");

namespace cod2::front_end {

namespace {

// The SDK's value until the player chose one.
void SetIfUnset(const char* name, const char* value) {
  if (rex::cvar::GetFlagInfo(name) && rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagByName(name, value);
}

std::string Flag(const char* name) {
  return rex::cvar::GetFlagInfo(name) ? std::string(rex::cvar::GetFlagByName(name)) : std::string("?");
}

}  // namespace

void ApplyInputDefaults() {
  SetIfUnset("mnk_mode", "true");
  SetIfUnset("keybind_start", "Escape,Return");
#if defined(COD2_MP)
  // cod2mp has no start-up films.
  REXLOG_INFO("cod2: keyboard pad {} (START {}, A {})", Flag("mnk_mode"), Flag("keybind_start"),
              Flag("keybind_a"));
#else
  REXLOG_INFO("cod2: keyboard pad {} (START {}, A {}); start-up films {}", Flag("mnk_mode"),
              Flag("keybind_start"), Flag("keybind_a"),
              REXCVAR_GET(cod2_skip_intro_movies) ? "skipped" : "played (A skips each)");
#endif
}

}  // namespace cod2::front_end

#if !defined(COD2_MP)

// COM_PlayIntroMovies: without its films when cod2_skip_intro_movies is on.
REX_EXTERN(__imp__sub_8233B3F0);
REX_HOOK_RAW(sub_8233B3F0) {
  constexpr uint32_t kLaunchFlag = 0x8414E4D8u;   // set: launch data to hand back instead
  constexpr uint32_t kLaunchFlag2 = 0x8414E154u;  // set: no films either
  if (REXCVAR_GET(cod2_skip_intro_movies) && base[kLaunchFlag] == 0 && base[kLaunchFlag2] == 0) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("cod2: start-up films skipped (cod2_skip_intro_movies)");
    }
    return;
  }
  __imp__sub_8233B3F0(ctx, base);
}

#endif  // !COD2_MP
