// [acct2-fh] 2026-11-14 Finest Hour's stand-in for the rexglue SDK's rex/cvar.h, as Big Red One's
// (bro/src/f2/shim/rex/cvar.h): the few calls the shared F2 rows make (COD_3_WAW_Setup-tool/recomp/src/f2/f2_rows.cpp
// and f2_theme.cpp), answered from Finest Hour's own settings (fh_f2_store.cpp, over fh_settings.c). With it those
// shared files build for Finest Hour unchanged; Finest Hour isn't on the SDK. Only the F2 window's build puts this
// folder on its include path.
//
// Every name is a fh.toml setting with a dot ("display.render_resolution", "interface.theme"). Finest Hour has no
// cvars of its own (its engine keeps its options in its save game), so a name without a dot is no setting.
#pragma once

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

// nullptr when there is no such setting.
const FlagInfo* GetFlagInfo(std::string_view name);
std::string GetFlagByName(std::string_view name);
bool SetFlagByName(std::string_view name, std::string_view value);

}  // namespace rex::cvar
