// cod3 - World at War dvar names in the console (see waw_dvars.h).

#include "ui/waw_dvars.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <rex/cvar.h>

#include "game/aim_assist.h"
#include "game/mouse_look.h"
#include "game/render_resolution.h"
#include "game/viewmodel.h"  // [claude-9b] CS2 weapon position

namespace cod3::ui::waw {

namespace {

bool EqualsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

bool LessNoCase(std::string_view a, std::string_view b) {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
  });
}

std::string Num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

double ToNum(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
bool Truthy(const std::string& s) {
  return s == "1" || EqualsNoCase(s, "true") || EqualsNoCase(s, "on") || EqualsNoCase(s, "yes");
}

constexpr double kNeverFade = 100000.0;  // seconds: "never" for the HUD fade timers

const Dvar kDvars[] = {
    // --- View: bob, gun position, view kick (cheat-protected in both games) --
    {"bg_bobAmplitudeDucked", Kind::kGame, "cg_bobAmplitudeDucked", Conv::kPairMax,
     "View bob while crouched (World at War: 'horizontal vertical'; CoD3 takes one value)"},
    {"bg_bobAmplitudeProne", Kind::kGame, "cg_bobAmplitudeProne", Conv::kPairMax,
     "View bob while prone (one value in CoD3)"},
    {"bg_bobAmplitudeStanding", Kind::kGame, "cg_bobAmplitudeStanding", Conv::kPairMax,
     "View bob while standing (one value in CoD3)"},
    {"bg_bobMax", Kind::kGame, "cg_bobMax", Conv::kSame, "Largest view bob"},
    {"bg_viewKickMax", Kind::kGame, "cg_viewKickMax", Conv::kSame, "Largest view kick when hit"},
    {"bg_viewKickScale", Kind::kGame, "cg_viewKickScale", Conv::kTimes10,
     "View kick per point of damage (CoD3 counts it x 10)"},
    {"cg_gun_x", Kind::kGame, "cg_gunX", Conv::kSame, "Weapon model offset forward"},
    {"cg_gun_y", Kind::kGame, "cg_gunY", Conv::kSame, "Weapon model offset sideways"},
    {"cg_gun_z", Kind::kGame, "cg_gunZ", Conv::kSame, "Weapon model offset up"},
    {"g_earthquakeEnable", Kind::kGame, "cg_camerashake", Conv::kSame, "Camera shake from explosions"},
    // --- Shell shock -----------------------------------------------------------
    {"bg_shock_lookControl", Kind::kGame, "cg_shock_mouse", Conv::kSame,
     "Slow the view down while shell-shocked"},
    {"bg_shock_lookControl_fadeTime", Kind::kGame, "cg_shock_mouse_fadeTime", Conv::kSame,
     "Seconds for the shell-shock view slowdown to fade"},
    {"bg_shock_lookControl_maxpitchspeed", Kind::kGame, "cg_shock_mouse_maxpitchspeed", Conv::kSame,
     "Fastest up/down turning while shell-shocked (degrees/second)"},
    {"bg_shock_lookControl_maxyawspeed", Kind::kGame, "cg_shock_mouse_maxyawspeed", Conv::kSame,
     "Fastest left/right turning while shell-shocked (degrees/second)"},
    {"bg_shock_lookControl_mousesensitivityscale", Kind::kGame, "cg_shock_mouse_sensitivityscale",
     Conv::kSame, "Look sensitivity while shell-shocked"},
    {"bg_shock_sound", Kind::kGame, "cg_shock_sound", Conv::kSame, "Play the shell-shock sound"},
    {"bg_shock_soundFadeInTime", Kind::kGame, "cg_shock_soundFadeInTime", Conv::kSame,
     "Shell-shock sound fade in (seconds)"},
    {"bg_shock_soundFadeOutTime", Kind::kGame, "cg_shock_soundFadeOutTime", Conv::kSame,
     "Shell-shock sound fade out (seconds)"},
    {"bg_shock_soundLoopEndDelay", Kind::kGame, "cg_shock_soundLoopEndDelay", Conv::kSame,
     "When the shell-shock sound loop ends, from the end of the shock (seconds)"},
    {"bg_shock_soundLoopFadeTime", Kind::kGame, "cg_shock_soundLoopFadeTime", Conv::kSame,
     "Shell-shock sound loop fade (seconds)"},
    {"bg_shock_soundModEndDelay", Kind::kGame, "cg_shock_soundModEndDelay", Conv::kSame,
     "When the muffled sound ends, from the end of the shock (seconds)"},
    {"bg_shock_soundRoomType", Kind::kGame, "cg_shock_soundRoomType", Conv::kSame,
     "Reverb room used while shell-shocked"},
    {"bg_shock_soundWetLevel", Kind::kGame, "cg_shock_soundWetLevel", Conv::kSame,
     "Reverb amount while shell-shocked"},
    {"bg_shock_viewKickFadeTime", Kind::kGame, "cg_shock_viewKickFadeTime", Conv::kSame,
     "Seconds for the shell-shock view kick to fade"},
    {"bg_shock_viewKickPeriod", Kind::kGame, "cg_shock_viewKickPeriod", Conv::kSame,
     "Period of the shell-shock view kick (seconds)"},
    {"bg_shock_viewKickRadius", Kind::kGame, "cg_shock_viewKickRadius", Conv::kSame,
     "Size of the shell-shock view kick"},
    {"bg_shock_volume_music", Kind::kGame, "cg_shock_volume_music", Conv::kSame,
     "Music volume while shell-shocked (0-1)"},
    {"bg_shock_volume_ui", Kind::kGame, "cg_shock_volume_menu", Conv::kSame,
     "Menu sound volume while shell-shocked (0-1)"},
    {"bg_shock_volume_voice", Kind::kGame, "cg_shock_volume_voice", Conv::kSame,
     "Voice volume while shell-shocked (0-1)"},
    {"cg_drawShellshock", Kind::kGame, "cg_shellshockblur", Conv::kSame,
     "Draw the shell-shock blur"},
    // --- HUD and compass -------------------------------------------------------
    {"cg_drawHUD", Kind::kGame, "cg_drawStatus", Conv::kSame, "Draw the HUD"},
    {"hud_drawHUD", Kind::kGame, "cg_drawStatus", Conv::kSame, "Draw the HUD"},
    {"hud_enable", Kind::kGame, "cg_drawStatus", Conv::kSame, "Draw the HUD"},
    {"ui_drawCrosshair", Kind::kGame, "cg_drawCrosshair", Conv::kSame, "Draw the crosshair"},
    {"hud_fade_ammodisplay", Kind::kGame, "g_ammoSolidTime", Conv::kFadeTime,
     "Seconds before the ammo counter fades (0 = never)"},
    {"hud_fade_compass", Kind::kGame, "g_compassSolidTime", Conv::kFadeTime,
     "Seconds before the compass fades (0 = never)"},
    {"hud_fade_offhand", Kind::kGame, "g_grenadeSolidTime", Conv::kFadeTime,
     "Seconds before the grenade counter fades (0 = never)"},
    {"hud_fade_stance", Kind::kGame, "g_stanceSolidTime", Conv::kFadeTime,
     "Seconds before the stance icon fades (0 = never)"},
    {"compassSize", Kind::kGame, "cg_hudCompassSize", Conv::kSame, "Compass size"},
    {"compassMaxRange", Kind::kGame, "cg_hudCompassMaxRange", Conv::kSame,
     "Farthest distance shown on the compass"},
    {"compassMinRange", Kind::kGame, "cg_hudCompassMinRange", Conv::kSame,
     "Nearest distance shown on the compass"},
    {"compassMinRadius", Kind::kGame, "cg_hudCompassMinRadius", Conv::kSame,
     "Smallest distance from the compass center things are drawn at"},
    {"compassShowEnemies", Kind::kGame, "enemy_radar", Conv::kSame, "Show enemies on the compass"},
    {"compassObjectiveMaxHeight", Kind::kGame, "cg_hudObjectiveMaxHeight", Conv::kSame,
     "Height above which an objective counts as on another floor"},
    {"compassObjectiveMinHeight", Kind::kGame, "cg_hudObjectiveMinHeight", Conv::kSame,
     "Height below which an objective counts as on another floor"},
    {"compassObjectiveMaxRange", Kind::kGame, "cg_hudObjectiveMaxRange", Conv::kSame,
     "Farthest distance an objective shows on the compass"},
    {"compassObjectiveMinAlpha", Kind::kGame, "cg_hudObjectiveMinAlpha", Conv::kSame,
     "Fade of an objective at the compass edge"},
    {"compassObjectiveNumRings", Kind::kGame, "cg_hudObjectiveNumRings", Conv::kSame,
     "Rings drawn when a new objective appears"},
    {"compassObjectiveRingTime", Kind::kGame, "cg_hudObjectiveRingTime", Conv::kSame,
     "Milliseconds between those rings"},
    {"r_aspectRatio", Kind::kGame, "cg_widescreen", Conv::kAspect,
     "auto, standard (4:3) or wide 16:9 (CoD3's cg_widescreen 0/1)"},
    // --- Frame rate, time, language, vehicles ------------------------------
    {"cl_wadefps", Kind::kGame, "cg_drawFPS", Conv::kSame, "Frame rate counter"},
    {"com_timescale", Kind::kGame, "timescale", Conv::kSame, "Game speed (cheat-protected)"},
    {"fs_ignoreLocalized", Kind::kGame, "fs_ignoreLozalized", Conv::kSame,
     "Ignore localized files (CoD3 spells it fs_ignoreLozalized)"},
    {"loc_language", Kind::kGame, "cl_language", Conv::kSame, "Language number (applies after a restart)"},
    {"loc_translate", Kind::kGame, "cl_languagetranslate", Conv::kSame, "Translate text"},
    {"loc_warnings", Kind::kGame, "cl_languagewarnings", Conv::kSame, "Warn about untranslated text"},
    {"loc_warningsAsErrors", Kind::kGame, "cl_languagewarningsaserrors", Conv::kSame,
     "Untranslated text is an error"},
    {"vehControlMode", Kind::kGame, "g_vehControlMode", Conv::kSame, "Tank controls"},
    // --- Call of Duty 4 names (from its public dvar lists) -------------------
    {"r_lodScaleRigid", Kind::kGame, "r_lodscale", Conv::kSame,
     "Model detail distance scale (Call of Duty 3 has one scale for every model)"},
    {"r_lodScaleSkinned", Kind::kGame, "r_lodscale", Conv::kSame,
     "Model detail distance scale (Call of Duty 3 has one scale for every model)"},
    // --- Match rules (the host's, in multiplayer) -------------------------------
    {"g_gametype", Kind::kGame, "mp_gametype", Conv::kSame,
     "Game mode, by this game's names: dm, tdm, ctf, hq, scf, dom, war, sd (war is Call of Duty 3's own mode)"},
    {"scr_teambalance", Kind::kGame, "mp_teambalance", Conv::kSame, "Keep the teams even"},
    {"scr_dm_timelimit", Kind::kGame, "mp_timelimit", Conv::kSame, "Minutes in a match (one limit for every mode)"},
    {"scr_dm_scorelimit", Kind::kGame, "mp_scorelimit", Conv::kSame, "Score that ends the match (one limit for every mode)"},
    {"scr_dm_roundlimit", Kind::kGame, "mp_roundlimit", Conv::kSame, "Rounds in a match (one limit for every mode)"},
    {"scr_tdm_timelimit", Kind::kGame, "mp_timelimit", Conv::kSame, "Minutes in a match (one limit for every mode)"},
    {"scr_tdm_scorelimit", Kind::kGame, "mp_scorelimit", Conv::kSame, "Score that ends the match (one limit for every mode)"},
    {"scr_tdm_roundlimit", Kind::kGame, "mp_roundlimit", Conv::kSame, "Rounds in a match (one limit for every mode)"},
    {"scr_ctf_timelimit", Kind::kGame, "mp_timelimit", Conv::kSame, "Minutes in a match (one limit for every mode)"},
    {"scr_ctf_scorelimit", Kind::kGame, "mp_scorelimit", Conv::kSame, "Score that ends the match (one limit for every mode)"},
    {"scr_ctf_roundlimit", Kind::kGame, "mp_roundlimit", Conv::kSame, "Rounds in a match (one limit for every mode)"},
    {"scr_hq_timelimit", Kind::kGame, "mp_timelimit", Conv::kSame, "Minutes in a match (one limit for every mode)"},
    {"scr_hq_scorelimit", Kind::kGame, "mp_scorelimit", Conv::kSame, "Score that ends the match (one limit for every mode)"},
    {"scr_hq_roundlimit", Kind::kGame, "mp_roundlimit", Conv::kSame, "Rounds in a match (one limit for every mode)"},
    {"scr_scf_timelimit", Kind::kGame, "mp_timelimit", Conv::kSame, "Minutes in a match (one limit for every mode)"},
    {"scr_scf_scorelimit", Kind::kGame, "mp_scorelimit", Conv::kSame, "Score that ends the match (one limit for every mode)"},
    {"scr_scf_roundlimit", Kind::kGame, "mp_roundlimit", Conv::kSame, "Rounds in a match (one limit for every mode)"},
    {"scr_dom_timelimit", Kind::kGame, "mp_timelimit", Conv::kSame, "Minutes in a match (one limit for every mode)"},
    {"scr_dom_scorelimit", Kind::kGame, "mp_scorelimit", Conv::kSame, "Score that ends the match (one limit for every mode)"},
    {"scr_dom_roundlimit", Kind::kGame, "mp_roundlimit", Conv::kSame, "Rounds in a match (one limit for every mode)"},
    {"scr_war_timelimit", Kind::kGame, "mp_timelimit", Conv::kSame, "Minutes in a match (one limit for every mode)"},
    {"scr_war_scorelimit", Kind::kGame, "mp_scorelimit", Conv::kSame, "Score that ends the match (one limit for every mode)"},
    {"scr_war_roundlimit", Kind::kGame, "mp_roundlimit", Conv::kSame, "Rounds in a match (one limit for every mode)"},
    {"scr_sd_timelimit", Kind::kGame, "mp_timelimit", Conv::kSame, "Minutes in a match (one limit for every mode)"},
    {"scr_sd_scorelimit", Kind::kGame, "mp_scorelimit", Conv::kSame, "Score that ends the match (one limit for every mode)"},
    {"scr_sd_roundlimit", Kind::kGame, "mp_roundlimit", Conv::kSame, "Rounds in a match (one limit for every mode)"},

    // --- [claude-9b] Counter-Strike 2's weapon position (game/viewmodel.h) -------
    {"viewmodel_offset_x", Kind::kPort, "cod3_viewmodel_offset_x", Conv::kViewmodelOffset,
     "Counter-Strike 2: weapon to the right (-2.5 to 2.5; F2 > Game > Weapon)"},
    {"viewmodel_offset_y", Kind::kPort, "cod3_viewmodel_offset_y", Conv::kViewmodelOffset,
     "Counter-Strike 2: weapon away from you (-2 to 2)"},
    {"viewmodel_offset_z", Kind::kPort, "cod3_viewmodel_offset_z", Conv::kViewmodelOffset,
     "Counter-Strike 2: weapon up (-2 to 2)"},
    {"viewmodel_presetpos", Kind::kPort, "cod3_viewmodel_presetpos", Conv::kSame,
     "Counter-Strike 2: 1 Desktop, 2 Couch, 3 Classic, 0 your own offsets"},
    {"viewmodel_fov", Kind::kPort, "cod3_viewmodel_fov", Conv::kNotInPort,
     "Counter-Strike 2: the weapon's own field of view (none here: Call of Duty 3 draws the weapon "
     "with the view's, cg_fov)"},

    // --- Port settings (F2) ----------------------------------------------------
    {"sensitivity", Kind::kPort, "cod3_sensitivity", Conv::kSame, "Mouse sensitivity (default 5)"},
    {"m_yaw", Kind::kPort, "cod3_m_yaw", Conv::kSame, "Degrees turned per mouse count, sideways"},
    {"m_pitch", Kind::kPort, "cod3_m_pitch", Conv::kSame, "Degrees turned per mouse count, up/down"},
    {"input_invertPitch", Kind::kPort, "cod3_invert_mouse", Conv::kInvertPitch, "Invert mouse up/down"},
    {"in_mouse", Kind::kPort, "mnk_mode", Conv::kBool, "Keyboard and mouse controls"},
    {"aim_autoaim_enabled", Kind::kPort, "cod3_aim_assist", Conv::kAimAssist, "Aim assist"},
    {"aim_lockon_enabled", Kind::kPort, "cod3_aim_assist", Conv::kAimAssist, "Aim assist"},
    {"aim_slowdown_enabled", Kind::kPort, "cod3_aim_assist", Conv::kAimAssist, "Aim assist"},
    {"cg_crosshairEnemyColor", Kind::kPort, "cod3_crosshair_constant_color", Conv::kNotBool,
     "Crosshair turns red over enemies (the game's own colors)"},
    {"g_redCrosshairs", Kind::kPort, "cod3_crosshair_constant_color", Conv::kNotBool,
     "Crosshair turns red over enemies (the game's own colors)"},
    {"r_fullscreen", Kind::kPort, "fullscreen", Conv::kBool, "Fullscreen (borderless)"},
    {"r_mode", Kind::kPort, "cod3_render_resolution", Conv::kResolution,
     "Render resolution, e.g. 1920x1080 or auto (applies at once)"},
    {"r_customMode", Kind::kPort, "cod3_render_resolution", Conv::kResolution,
     "Render resolution, e.g. 2560x1440 (applies at once)"},
    {"r_vsync", Kind::kPort, "vulkan_allow_present_mode_immediate", Conv::kVsync,
     "Wait for the monitor (no tearing; applies after a restart)"},
    {"r_aaSamples", Kind::kPort, "cod3_game_msaa", Conv::kGameMsaa,
     "1 = no MSAA, 2 = the game's own 2x MSAA where it fits (applies at once)"},
    {"r_texFilterAnisoMax", Kind::kPort, "anisotropic_override", Conv::kAniso,
     "Anisotropic filtering: 1, 2, 4, 8 or 16"},
    {"sm_enable", Kind::kPort, "cod3_shadow_quality", Conv::kShadows,
     "Shadows of soldiers and models: 0 = off, 1 = on (F2 > Video > Shadow quality)"},
    {"sm_sunEnable", Kind::kPort, "cod3_shadow_quality", Conv::kShadows,
     "Sun shadows of soldiers and models: 0 = off, 1 = on (the same setting as sm_enable)"},
    {"snd_volume", Kind::kPort, "cod3_volume", Conv::kSame, "Master volume (0-1)"},
    {"snd_menu_master", Kind::kPort, "cod3_volume", Conv::kSame, "Master volume (0-1)"},
    {"r_scaleViewport", Kind::kPort, "cod3_render_percent", Conv::kFraction100,
     "Draw fewer pixels for speed: a fraction from 0.25 to 1 (F2 > Video > Resolution scale)"},
};

std::vector<Dvar> MakeAll() {
  std::vector<Dvar> v(std::begin(kDvars), std::end(kDvars));
  std::sort(v.begin(), v.end(), [](const Dvar& a, const Dvar& b) { return LessNoCase(a.name, b.name); });
  return v;
}

const char* const kWawOnly[] = {
#include "ui/waw_dvar_names.inc"
};

const CvarInfo* FindCvar(const EngineSnapshot& snap, std::string_view name) {
  for (const auto& c : snap.cvars)
    if (EqualsNoCase(c.name, name)) return &c;
  return nullptr;
}

// The setting's value as World at War writes it.
std::string FromTarget(const Dvar& d, const std::string& v) {
  switch (d.conv) {
    case Conv::kTimes10: return Num(ToNum(v) / 10.0);
    case Conv::kPairMax: return v + " " + v;
    case Conv::kFadeTime: return ToNum(v) >= kNeverFade ? "0" : v;
    case Conv::kAspect: return v == "0" ? "standard" : "wide 16:9";
    case Conv::kBool: return Truthy(v) ? "1" : "0";
    case Conv::kNotBool: return Truthy(v) ? "0" : "1";
    case Conv::kAimAssist: {
      auto on = AimAssistEnabled();
      return on ? (*on ? "1" : "0") : (v == "0" ? "0" : "1");
    }
    case Conv::kInvertPitch: return MouseLook::MouseInverted() ? "1" : "0";
    case Conv::kAniso: {
      const int i = std::atoi(v.c_str());
      return i <= 0 ? (i == 0 ? "0" : "default") : std::to_string(1 << (i - 1));
    }
    // -1 follows the render plan (native: no MSAA).
    case Conv::kGameMsaa: return v == "1" || (v != "0" && cod3::GetRenderPlan().msaa) ? "2" : "1";
    case Conv::kVsync: return Truthy(v) ? "0" : "1";  // immediate allowed = no vsync
    case Conv::kShadows: return v == "off" ? "0" : "1";
    case Conv::kFraction100: return Num(ToNum(v) / 100.0);
    default: return v;
  }
}

}  // namespace

const std::vector<Dvar>& All() {
  static const std::vector<Dvar> all = MakeAll();
  return all;
}

const Dvar* Find(std::string_view name) {
  for (const auto& d : All())
    if (EqualsNoCase(d.name, name)) return &d;
  return nullptr;
}

const std::vector<std::string_view>& WawOnlyNames() {
  static const std::vector<std::string_view> names(std::begin(kWawOnly), std::end(kWawOnly));
  return names;
}

bool IsWawOnly(std::string_view name) {
  for (auto n : WawOnlyNames())
    if (EqualsNoCase(n, name)) return true;
  return false;
}

std::string Value(const Dvar& d, const EngineSnapshot& snap) {
  if (d.kind == Kind::kGame) {
    const CvarInfo* c = FindCvar(snap, d.target);
    return c ? FromTarget(d, c->value) : std::string();
  }
  if (!rex::cvar::GetFlagInfo(d.target)) return {};
  return FromTarget(d, rex::cvar::GetFlagByName(d.target));
}

std::string DefaultValue(const Dvar& d, const EngineSnapshot& snap) {
  if (d.kind == Kind::kGame) {
    const CvarInfo* c = FindCvar(snap, d.target);
    return c ? FromTarget(d, c->reset) : std::string();
  }
  const auto* info = rex::cvar::GetFlagInfo(d.target);
  if (!info) return {};
  // Aim assist's value comes from the game's profile, not the setting.
  if (d.conv == Conv::kAimAssist) return "1";
  return FromTarget(d, info->default_value);
}

SetResult Set(const Dvar& d, const std::string& value) {
  SetResult r;
  std::string v = value;
  switch (d.conv) {
    case Conv::kTimes10: v = Num(ToNum(value) * 10.0); break;
    case Conv::kPairMax: {
      double a = 0, b = 0;
      const int n = std::sscanf(value.c_str(), "%lf %lf", &a, &b);
      v = Num(n >= 2 ? std::max(a, b) : a);
      break;
    }
    case Conv::kFadeTime: v = ToNum(value) <= 0.0 ? Num(kNeverFade) : Num(ToNum(value)); break;
    case Conv::kAspect: {
      std::string lower;
      for (char c : value) lower += char(std::tolower(static_cast<unsigned char>(c)));
      v = lower.find("standard") != std::string::npos || lower == "0" ? "0" : "1";
      break;
    }
    case Conv::kBool: v = Truthy(value) ? "true" : "false"; break;
    case Conv::kNotBool: v = Truthy(value) ? "false" : "true"; break;
    case Conv::kInvertPitch:
      MouseLook::SetMouseInverted(Truthy(value));
      r.message = std::string(d.name) + " " + (Truthy(value) ? "1" : "0") + " (F2 > Mouse > Invert vertical)\n";
      r.changed_port_setting = true;
      return r;
    case Conv::kAimAssist:
      SetAimAssist(Truthy(value));
      r.message = std::string(d.name) + " " + (Truthy(value) ? "1" : "0") + " (F2 > Game > Aim)\n";
      r.changed_port_setting = true;  // saved in cod3.toml, like the F2 row
      return r;
    case Conv::kAniso: {
      const int n = std::atoi(value.c_str());
      int idx = 0;
      if (n >= 16) idx = 5;
      else if (n >= 8) idx = 4;
      else if (n >= 4) idx = 3;
      else if (n >= 2) idx = 2;
      else if (n >= 1) idx = 1;
      v = std::to_string(idx);
      break;
    }
    case Conv::kResolution: {
      if (EqualsNoCase(value, "auto") || EqualsNoCase(value, "original")) {
        v = value;
      } else {
        unsigned w = 0, h = 0;
        // The same bounds as ParseResolutionChoice (game/render_resolution.h).
        if (value.size() > 16 || std::sscanf(value.c_str(), "%ux%u", &w, &h) != 2 ||
            w < cod3::kMinCustomWidth || h < cod3::kMinCustomHeight || w > cod3::kMaxCustomWidth ||
            h > cod3::kMaxCustomHeight) {
          r.ok = false;
          r.message = "^1" + std::string(d.name) + ": use WIDTHxHEIGHT from " +
                      std::to_string(cod3::kMinCustomWidth) + "x" + std::to_string(cod3::kMinCustomHeight) +
                      " to " + std::to_string(cod3::kMaxCustomWidth) + "x" +
                      std::to_string(cod3::kMaxCustomHeight) + " (e.g. 1920x1080) or auto\n";
          return r;
        }
        v = std::to_string(w) + "x" + std::to_string(h);
      }
      break;
    }
    case Conv::kGameMsaa: v = ToNum(value) >= 2.0 ? "1" : "0"; break;
    case Conv::kShadows: v = Truthy(value) ? "original" : "off"; break;
    case Conv::kViewmodelOffset: {
      // [claude-9b] Typed by hand, the position is a custom one, as in F2: a
      // chosen preset's offsets become the three settings first, then this one
      // changes (game/viewmodel.h).
      namespace vm = cod3::viewmodel;
      vm::Offsets o;
      if (rex::cvar::GetFlagInfo(vm::kPreset) &&
          vm::PresetOffsets(std::atoi(rex::cvar::GetFlagByName(vm::kPreset).c_str()), &o)) {
        rex::cvar::SetFlagByName(vm::kOffsetX, Num(o.x));
        rex::cvar::SetFlagByName(vm::kOffsetY, Num(o.y));
        rex::cvar::SetFlagByName(vm::kOffsetZ, Num(o.z));
        rex::cvar::SetFlagByName(vm::kPreset, "0");
      }
      break;
    }
    case Conv::kNotInPort:
      r.ok = false;
      r.message = "^3" + std::string(d.name) + "^7: " + d.help + "\n";
      return r;
    case Conv::kFraction100:
      v = std::to_string(static_cast<int>(std::lround(std::clamp(ToNum(value), 0.25, 1.0) * 100.0)));
      break;
    case Conv::kVsync: {
      // V-Sync on = immediate presentation not allowed.
      const std::string allow = Truthy(value) ? "false" : "true";
      bool any = false;
      for (const char* cvar : {"vulkan_allow_present_mode_immediate",
                               "d3d12_allow_variable_refresh_rate_and_tearing"}) {
        if (rex::cvar::GetFlagInfo(cvar)) {
          rex::cvar::SetFlagByName(cvar, allow);
          any = true;
        }
      }
      r.ok = any;
      r.changed_port_setting = any;
      r.needs_restart = any;
      r.message = any ? std::string(d.name) + " " + (Truthy(value) ? "1" : "0") +
                            " (F2 > Video > V-Sync; applies after a restart)\n"
                      : "^1" + std::string(d.name) + ": not available with this GPU backend\n";
      return r;
    }
    default: break;
  }

  if (d.kind == Kind::kGame) {
    r.game_command = std::string(d.target) + " \"" + v + "\"";
    return r;
  }
  auto* info = rex::cvar::GetFlagInfo(d.target);
  if (!info) {
    r.ok = false;
    r.message = "^1" + std::string(d.name) + ": not available in this build\n";
    return r;
  }
  rex::cvar::SetFlagByName(d.target, v);
  r.changed_port_setting = true;
  r.needs_restart = info->lifecycle != rex::cvar::Lifecycle::kHotReload &&
                    std::string_view(d.target) != "fullscreen";
  r.message = std::string(d.name) + " \"" + FromTarget(d, rex::cvar::GetFlagByName(d.target)) +
              "\" (port setting " + d.target + (r.needs_restart ? "; applies after a restart" : "") +
              ")\n";
  return r;
}

bool RewriteGameAlias(const std::vector<std::string>& args, std::vector<std::string>* out) {
  if (args.empty()) return false;
  static const char* const kNameSecond[] = {"set", "seta", "sets", "setu", "reset", "toggle", "togglep"};
  size_t pos = 0;
  for (const char* c : kNameSecond)
    if (EqualsNoCase(args[0], c)) pos = 1;
  if (pos >= args.size()) return false;
  const Dvar* d = Find(args[pos]);
  if (!d || d->kind != Kind::kGame) return false;
  *out = args;
  (*out)[pos] = d->target;
  // Convert the value too for set-style commands with a value.
  if (args.size() > pos + 1 && !(pos == 1 && (EqualsNoCase(args[0], "reset") ||
                                               EqualsNoCase(args[0], "toggle") ||
                                               EqualsNoCase(args[0], "togglep")))) {
    std::string value = args[pos + 1];
    for (size_t i = pos + 2; i < args.size(); ++i) value += " " + args[i];
    const SetResult r = Set(*d, value);
    if (!r.ok) return false;
    // r.game_command is "<target> "<converted>""; keep only the value.
    const size_t q = r.game_command.find('"');
    out->resize(pos + 2);
    (*out)[pos + 1] = q == std::string::npos ? value
                                             : r.game_command.substr(q + 1, r.game_command.size() - q - 2);
  }
  return true;
}

}  // namespace cod3::ui::waw
