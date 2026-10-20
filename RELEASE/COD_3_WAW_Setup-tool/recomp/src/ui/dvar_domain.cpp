// cod3 - dvar and port setting domains, World at War style. See dvar_domain.h.

#include "ui/dvar_domain.h"
#include "ui/dvar_help.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <rex/cvar.h>

#include "game/fov.h"
#include "game/render_resolution.h"
#include "ui/waw_dvars.h"

namespace cod3::ui::domain {

namespace {

using Kind = Domain::Kind;
constexpr double kInf = std::numeric_limits<double>::infinity();

bool EqualsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && EqualsNoCase(s.substr(0, prefix.size()), prefix);
}

// Numbers the way World at War prints them: %g for numbers, %i for integers.
std::string Number(double v, bool integer) {
  char buf[48];
  if (integer)
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  else
    std::snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

// A plain decimal number ("1", "-70", "0.25", "80.000000").
bool LooksNumeric(std::string_view s) {
  size_t i = 0;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
  bool digits = false, dot = false;
  for (; i < s.size(); ++i) {
    if (std::isdigit(static_cast<unsigned char>(s[i]))) {
      digits = true;
    } else if (s[i] == '.' && !dot) {
      dot = true;
    } else {
      return false;
    }
  }
  return digits;
}

Domain Make(Kind kind, double min = -kInf, double max = kInf) {
  Domain d;
  d.kind = kind;
  d.min = min;
  d.max = max;
  return d;
}

// Call of Duty 3 dvars whose values are plain from what the game does with
// them (and, for most, from World at War's dvar of the same name). The
// game itself checks none of them: these say what makes sense.
struct KnownDomain {
  const char* name;
  Kind kind;
  double min;
  double max;
};
constexpr KnownDomain kKnownDomains[] = {
    // On/off switches.
    {"ai_disableSpawn", Kind::kBool, 0, 1},
    {"ai_noDodge", Kind::kBool, 0, 1},
    {"cg_altTankCam", Kind::kBool, 0, 1},
    {"cg_blood", Kind::kBool, 0, 1},
    {"cg_camerashake", Kind::kBool, 0, 1},
    {"cg_crosshairDynamic", Kind::kBool, 0, 1},
    {"cg_draw2D", Kind::kBool, 0, 1},
    {"cg_drawCrosshair", Kind::kBool, 0, 1},
    {"cg_drawGun", Kind::kBool, 0, 1},
    {"cg_drawpaused", Kind::kBool, 0, 1},
    {"cg_drawPosition", Kind::kBool, 0, 1},
    {"cg_drawStatus", Kind::kBool, 0, 1},
    {"cg_hudCompassSpringyPointers", Kind::kBool, 0, 1},
    {"cg_hudCompassVisible", Kind::kBool, 0, 1},
    {"cg_hudDamageIconInScope", Kind::kBool, 0, 1},
    {"cg_letterbox", Kind::kBool, 0, 1},
    {"cg_nopredict", Kind::kBool, 0, 1},
    {"cg_norender", Kind::kBool, 0, 1},
    {"cg_shellshockblur", Kind::kBool, 0, 1},
    {"cg_shock_mouse", Kind::kBool, 0, 1},
    {"cg_shock_sound", Kind::kBool, 0, 1},
    {"cg_skybox", Kind::kBool, 0, 1},
    {"cg_subtitles", Kind::kBool, 0, 1},
    {"cg_thirdPerson", Kind::kBool, 0, 1},
    {"cg_thirdPersonLock", Kind::kBool, 0, 1},
    {"cg_widescreen", Kind::kBool, 0, 1},
    {"cl_freelook", Kind::kBool, 0, 1},
    {"cl_freeze", Kind::kBool, 0, 1},
    {"cl_languagetranslate", Kind::kBool, 0, 1},
    {"cl_languagewarnings", Kind::kBool, 0, 1},
    {"cl_languagewarningsaserrors", Kind::kBool, 0, 1},
    {"cl_noprint", Kind::kBool, 0, 1},
    {"cl_running", Kind::kBool, 0, 1},
    {"cm_noCurves", Kind::kBool, 0, 1},
    {"cm_playerCurveClip", Kind::kBool, 0, 1},
    {"disable_ai", Kind::kBool, 0, 1},
    {"enemy_radar", Kind::kBool, 0, 1},
    {"fx_cull", Kind::kBool, 0, 1},
    {"fx_draw", Kind::kBool, 0, 1},
    {"fx_enable", Kind::kBool, 0, 1},
    {"fx_freeze", Kind::kBool, 0, 1},
    {"g_restoreFullCheckpoint", Kind::kBool, 0, 1},
    {"g_spawnai", Kind::kBool, 0, 1},
    {"in_joystick", Kind::kBool, 0, 1},
    {"in_mouse", Kind::kBool, 0, 1},
    {"in_stickLegacy", Kind::kBool, 0, 1},
    {"in_stickSouthPaw", Kind::kBool, 0, 1},
    {"introScreen", Kind::kBool, 0, 1},
    {"letterbox_enabled", Kind::kBool, 0, 1},
    {"m_filter", Kind::kBool, 0, 1},
    {"player_deathInvulnerableToMelee", Kind::kBool, 0, 1},
    {"pmove_fixed", Kind::kBool, 0, 1},
    {"r_cullBModels", Kind::kBool, 0, 1},
    {"r_cullXModels", Kind::kBool, 0, 1},
    {"r_drawentities", Kind::kBool, 0, 1},
    {"r_dynamiclight", Kind::kBool, 0, 1},
    {"r_flares", Kind::kBool, 0, 1},
    {"r_fog", Kind::kBool, 0, 1},
    {"r_fullbright", Kind::kBool, 0, 1},
    {"r_lockpvs", Kind::kBool, 0, 1},
    {"r_nocull", Kind::kBool, 0, 1},
    {"r_noportals", Kind::kBool, 0, 1},
    {"r_norefresh", Kind::kBool, 0, 1},
    {"r_showLocationalDamage", Kind::kBool, 0, 1},
    {"r_showSkeletons", Kind::kBool, 0, 1},
    {"sv_cheats", Kind::kBool, 0, 1},  // the port's cheats / sv_cheats command (game/cheat_guard.h)
    {"sv_running", Kind::kBool, 0, 1},
    // Fractions: opacity and volume.
    {"cg_crosshairAlpha", Kind::kFloat, 0, 1},
    {"cg_crosshairAlphaMin", Kind::kFloat, 0, 1},
    {"cg_hudAlpha", Kind::kFloat, 0, 1},
    {"cg_hudObjectiveMinAlpha", Kind::kFloat, 0, 1},
    {"cg_tracerchance", Kind::kFloat, 0, 1},
    // Whole numbers with a fixed range.
    {"g_gameskill", Kind::kInt, 0, 3},  // Easy, Normal, Hardened, Veteran
};

const KnownDomain* FindKnown(std::string_view name) {
  for (const KnownDomain& k : kKnownDomains)
    if (EqualsNoCase(k.name, name)) return &k;
  return nullptr;
}

// World at War's four cg_drawFPS modes; the port draws the counter and
// takes these names as well as 0-3 (game/engine.cpp, ParseDrawFps).
const char* const kDrawFpsModes[] = {"Off", "Simple", "SimpleRanges", "Verbose"};
// World at War's r_aspectRatio choices (ui/waw_dvars.cpp converts them).
const char* const kAspectNames[] = {"auto", "standard", "wide 16:10", "wide 16:9"};

std::string Range(const Domain& d, bool integer, const char* any) {
  const bool has_min = std::isfinite(d.min), has_max = std::isfinite(d.max);
  if (has_min && has_max) {
    if (d.min == d.max) return Number(d.min, integer);
    return Number(d.min, integer) + " to " + Number(d.max, integer);
  }
  if (has_min) return Number(d.min, integer) + " or bigger";
  if (has_max) return Number(d.max, integer) + " or smaller";
  return any;
}

}  // namespace

std::vector<std::string> Describe(const Domain& d) {
  std::vector<std::string> lines;
  const bool has_min = std::isfinite(d.min), has_max = std::isfinite(d.max);
  switch (d.kind) {
    case Kind::kText: lines.emplace_back("Domain is any text"); break;
    case Kind::kBool: lines.emplace_back("Domain is 0 or 1"); break;
    case Kind::kInt:
    case Kind::kFloat: {
      const bool integer = d.kind == Kind::kInt;
      const char* what = integer ? "integer" : "number";
      if (has_min && has_max && d.min == d.max)
        lines.push_back("Domain is " + Number(d.min, integer));
      else if (has_min && has_max)
        lines.push_back(std::string("Domain is any ") + what + " from " + Number(d.min, integer) + " to " +
                        Number(d.max, integer));
      else if (has_min)
        lines.push_back(std::string("Domain is any ") + what + " " + Number(d.min, integer) + " or bigger");
      else if (has_max)
        lines.push_back(std::string("Domain is any ") + what + " " + Number(d.max, integer) + " or smaller");
      else
        lines.push_back(std::string("Domain is any ") + what);
      break;
    }
    case Kind::kEnum: {
      lines.emplace_back("Domain is one of the following:");
      char buf[16];
      for (size_t i = 0; i < d.choices.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%2d: ", static_cast<int>(i));
        lines.push_back(buf + d.choices[i]);
      }
      break;
    }
    case Kind::kCustom: lines.push_back("Domain is " + d.custom); break;
  }
  // The port's own words about the limit, after World at War's line.
  if (!d.note.empty()) lines.push_back("(" + d.note + ")");
  return lines;
}

std::string Brief(const Domain& d) {
  switch (d.kind) {
    case Kind::kText: return "any text";
    case Kind::kBool: return "0 or 1";
    case Kind::kInt: return Range(d, true, "any integer");
    case Kind::kFloat: return Range(d, false, "any number");
    case Kind::kEnum: {
      std::string names;
      for (const auto& c : d.choices) names += (names.empty() ? "" : "/") + c;
      if (names.size() <= 48) return names;
      return std::to_string(d.choices.size()) + " choices";
    }
    case Kind::kCustom:
      return d.custom.size() <= 44 ? d.custom : d.custom.substr(0, 41) + "...";
  }
  return {};
}

Domain ForGameDvar(std::string_view name, std::string_view reset_value) {
  // 1. The dvars the port governs: the port's limits.
  if (EqualsNoCase(name, "cg_fov")) {
    const float scale = Engine::Get().fov_scale();
    const fov::Range r = fov::FovRange(scale);
    Domain d = Make(Kind::kFloat, r.min, r.max);
    d.note = fov::DescribeFovRange(scale);
    return d;
  }
  if (EqualsNoCase(name, "cg_fovScale")) {
    const float player_fov = Engine::Get().fov();
    const fov::Range r = fov::ScaleRange(player_fov);
    Domain d = Make(Kind::kFloat, r.min, r.max);
    d.note = fov::DescribeScaleRange(player_fov);
    return d;
  }
  if (EqualsNoCase(name, "com_maxfps")) {
    // A limit, not a target; the port's frame limiter paces at most 1000.
    Domain d = Make(Kind::kInt, 0, 1000);
    d.note = "0 = no limit";
    return d;
  }
  if (EqualsNoCase(name, "cg_drawFPS")) {
    Domain d = Make(Kind::kEnum);
    d.choices.assign(std::begin(kDrawFpsModes), std::end(kDrawFpsModes));
    return d;
  }
  // The shell-shock volumes (cg_shock_volume_music ...) are fractions too.
  if (StartsWithNoCase(name, "cg_shock_volume_")) return Make(Kind::kFloat, 0, 1);
  // 2. What the game does with the value.
  if (const KnownDomain* k = FindKnown(name)) return Make(k->kind, k->min, k->max);
  // 3. The default value's form.
  if (LooksNumeric(reset_value)) return Make(Kind::kFloat);
  return Make(Kind::kText);
}

Domain ForPortSetting(const rex::cvar::FlagEntry& info) {
  using rex::cvar::FlagType;
  const auto& c = info.constraints;
  Domain d;
  switch (info.type) {
    case FlagType::Boolean: d.kind = Kind::kBool; break;
    case FlagType::Int32:
    case FlagType::Int64:
    case FlagType::Uint32:
    case FlagType::Uint64:
      d.kind = Kind::kInt;
      if (info.type == FlagType::Uint32 || info.type == FlagType::Uint64) d.min = 0;
      break;
    case FlagType::Double: d.kind = Kind::kFloat; break;
    case FlagType::String:
      if (c.HasAllowedValues()) {
        d.kind = Kind::kEnum;
        d.choices = c.allowed_values;
      }
      break;
    case FlagType::Command:
      d.kind = Kind::kCustom;
      d.custom = "a command (it takes no value)";
      break;
  }
  if (d.kind == Kind::kInt || d.kind == Kind::kFloat) {
    if (c.min) d.min = *c.min;
    if (c.max) d.max = *c.max;
  }
  return d;
}

Domain ForWaw(const waw::Dvar& w, const EngineSnapshot& snap) {
  using waw::Conv;
  if (w.kind == waw::Kind::kGame) {
    std::string_view reset;
    bool registered = false;
    for (const CvarInfo& c : snap.cvars) {
      if (EqualsNoCase(c.name, w.target)) {
        reset = c.reset;
        registered = true;
      }
    }
    Domain d = ForGameDvar(w.target, reset);
    // Not registered yet (cgame's dvars come with the first level): its
    // default isn't known, so neither is its form, unless the port knows it.
    if (!registered && d.kind == Kind::kText) {
      d.kind = Kind::kCustom;
      d.custom = "known once the game has registered " + std::string(w.target);
      return d;
    }
    switch (w.conv) {
      case Conv::kTimes10:
        // Call of Duty 3 counts it ten times larger.
        if (std::isfinite(d.min)) d.min /= 10.0;
        if (std::isfinite(d.max)) d.max /= 10.0;
        break;
      case Conv::kPairMax:
        d.note = "World at War takes two numbers; Call of Duty 3 keeps the larger";
        break;
      case Conv::kFadeTime:
        d = Make(Kind::kFloat, 0);
        d.note = "0 = never fade";
        break;
      default: break;
    }
    return d;
  }
  switch (w.conv) {
    case Conv::kBool:
    case Conv::kNotBool:
    case Conv::kAimAssist:
    case Conv::kVsync:
    case Conv::kShadows: return Make(Kind::kBool, 0, 1);
    case Conv::kFraction100: {
      Domain d = Make(Kind::kFloat, 0.25, 1.0);
      d.note = "the port's resolution scale: 0.5 draws half the pixels each way";
      return d;
    }
    case Conv::kAniso: {
      Domain d = Make(Kind::kInt, 1, 16);
      d.note = "1, 2, 4, 8 or 16 (0 = the game's own filtering)";
      return d;
    }
    case Conv::kGameMsaa: {
      Domain d = Make(Kind::kInt, 1, 16);
      d.note = "1 = no MSAA; 2 or more = the game's own 2x MSAA where it fits";
      return d;
    }
    case Conv::kResolution: {
      Domain d = Make(Kind::kCustom);
      d.custom = "auto, original, or any WIDTHxHEIGHT from " + std::to_string(kMinCustomWidth) + "x" +
                 std::to_string(kMinCustomHeight) + " to " + std::to_string(kMaxCustomWidth) + "x" +
                 std::to_string(kMaxCustomHeight);
      return d;
    }
    case Conv::kAspect: {
      Domain d = Make(Kind::kEnum);
      d.choices.assign(std::begin(kAspectNames), std::end(kAspectNames));
      d.note = "or a shape of your own, W:H from 4:3 to 48:9, e.g. 21:9";
      return d;
    }
    default: break;
  }
  if (const auto* info = rex::cvar::GetFlagInfo(w.target)) return ForPortSetting(*info);
  return Make(Kind::kText);
}

std::string PortValueText(const rex::cvar::FlagEntry& info, const std::string& value) {
  if (info.type == rex::cvar::FlagType::Boolean) return value == "true" ? "1" : "0";
  return value;
}

std::vector<std::string> InfoLines(std::string_view name, std::string_view value, std::string_view reset,
                                   const Domain& d) {
  std::vector<std::string> lines;
  lines.push_back("\"" + std::string(name) + "\" is:\"" + std::string(value) + "^7\" default:\"" +
                  std::string(reset) + "^7\"");
  for (auto& l : Describe(d)) lines.push_back("  " + l);
  return lines;
}

std::string LinesAfterGameAnswer(std::string_view printed) {
  // "\"%s\" is:\"%s^7\" default:\"%s^7\"\n"
  if (printed.empty() || printed.front() != '"') return {};
  const size_t name_end = printed.find('"', 1);
  if (name_end == std::string_view::npos || printed.substr(name_end, 6) != "\" is:\"") return {};
  constexpr std::string_view kDefault = "^7\" default:\"";
  const size_t def = printed.rfind(kDefault);
  if (def == std::string_view::npos || def < name_end) return {};
  std::string_view reset = printed.substr(def + kDefault.size());
  while (!reset.empty() && (reset.back() == '\n' || reset.back() == '\r')) reset.remove_suffix(1);
  if (reset.size() < 3 || reset.substr(reset.size() - 3) != "^7\"") return {};
  reset.remove_suffix(3);
  const std::string_view name = printed.substr(1, name_end - 1);
  std::string out;
  for (auto& l : Describe(ForGameDvar(name, reset))) out += "  " + l + "\n";
  // What it does, in the port's words (ui/dvar_help.h).
  if (const dvar_help::Entry* e = dvar_help::Find(name)) out += "  " + std::string(e->text) + "\n";
  return out;
}

}  // namespace cod3::ui::domain
