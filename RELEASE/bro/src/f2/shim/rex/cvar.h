// [f2-menus] Big Red One's stand-in for the rexglue SDK's rex/cvar.h: the few calls the shared F2 rows make
// (COD_3_WAW_Setup-tool/recomp/src/f2/f2_rows.cpp, f2_theme.cpp, f2_console.cpp), answered from Big Red One's own
// settings (bro_f2_store.cpp). With it those shared files build for Big Red One unchanged; Big Red One isn't on the
// SDK. Only this port's build puts this folder on its include path.
// And, since 2026-11-11, the SDK's way of making a setting (REXCVAR_DEFINE_*, REXCVAR_GET), for f2_accel.cpp.
//
// A name with a dot is a bro.toml setting ("display.fov", "interface.theme"); a name without one is one of the game's
// own cvars ("cg_drawGun"), read and changed through bro_game_cvars.c.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rex::cvar {

enum class Lifecycle { kHotReload, kRequiresRestart };

struct Constraints {
  std::vector<std::string> allowed_values;
  bool HasAllowedValues() const { return !allowed_values.empty(); }
};

struct FlagInfo {
  std::string name;
  std::string default_value;
  Lifecycle lifecycle = Lifecycle::kHotReload;
  Constraints constraints;
};

// nullptr when there is no such setting (or the game hasn't made that cvar yet).
const FlagInfo* GetFlagInfo(std::string_view name);
std::string GetFlagByName(std::string_view name);
bool SetFlagByName(std::string_view name, std::string_view value);

// [f2-menus] 2026-11-11 A shared file that makes its own settings with the SDK's REXCVAR_DEFINE_* (f2_accel.cpp:
// the in-game mouse acceleration, the sensitivity randomizer and the sensitivity scales). As with the SDK each lives
// in a variable of its own, read with REXCVAR_GET; here bro_f2_store.cpp keeps it in bro.toml's [mouse] under its
// name without "f2_" (f2_accel_mode: mouse.accel_mode, variable BRO_MOUSE_ACCEL_MODE), and the three calls above
// know it by either name.
namespace shim {
struct Defined {
  // The SDK's ".range(low, high)" after a definition. The rows have their own limits: nothing is kept.
  template <typename A, typename B>
  const Defined& range(A, B) const {
    return *this;
  }
};
Defined Define(const char* name, bool* storage);
Defined Define(const char* name, int32_t* storage);
Defined Define(const char* name, double* storage);
// Every defined setting read from bro.toml (its variable first) into its variable. Once, from bro_f2_init.
void Load();
}  // namespace shim

}  // namespace rex::cvar

#define REXCVAR_SHIM_DEFINE(type, name, value)                                              \
  type FLAGS_##name = (value);                                                              \
  [[maybe_unused]] static const ::rex::cvar::shim::Defined rexcvar_shim_defined_##name =    \
      ::rex::cvar::shim::Define(#name, &FLAGS_##name)
#define REXCVAR_DEFINE_BOOL(name, value, category, description) REXCVAR_SHIM_DEFINE(bool, name, value)
#define REXCVAR_DEFINE_INT32(name, value, category, description) REXCVAR_SHIM_DEFINE(int32_t, name, value)
#define REXCVAR_DEFINE_DOUBLE(name, value, category, description) REXCVAR_SHIM_DEFINE(double, name, value)
#define REXCVAR_GET(name) (FLAGS_##name)
