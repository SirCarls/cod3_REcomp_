// [f2-menus] Big Red One's cvars on the F2 window's pages (see bro_dvars.h and dev/agents/f2-menus/bro/DVARS.md).
//
// The names, defaults and flags were read from the campaign program (default.xbe) with
// dev/agents/f2-menus/tools/bro_cvars.py: CG_RegisterCvars' table (0x008D9D60, 137 records), G_RegisterCvars'
// (0x008DA8E8, 57) and the calls to Cvar_Get. Labels say only what the name and the game's own use make plain; where a
// cvar's effect isn't known yet, the label is its name.

#include "bro_dvars.h"

#include <cctype>
#include <cstring>
#include <initializer_list>
#include <iterator>

namespace bro_f2 {
namespace {

const DvarRow kRows[] = {
    // ---------------------------------------------------------------------------------------------------- VIDEO
    {"VIDEO", "The game's picture", "cg_widescreen", "Widescreen (the game's own)", Kind::kCheck, 0, 1,
     "The game's own 16:9 setting (cg_widescreen), as the Xbox's widescreen option set it."},
    {"VIDEO", "The game's picture", "cg_viewsize", "Picture size on the screen", Kind::kInt, 30, 100,
     "How much of the screen the game's 3D picture fills, in percent (cg_viewsize)."},
    {"VIDEO", "The game's picture", "cg_fov", "The game's field of view", Kind::kFloat, 1, 160,
     "cg_fov, in degrees. \"Use my own field of view\" above wins over it while that is on."},
    {"VIDEO", "The game's picture", "cg_drawGun", "Show the gun", Kind::kCheck, 0, 1,
     "cg_drawGun. A cheat setting: the game puts it back each level, and F2 sets it again."},
    {"VIDEO", "The game's picture", "cg_skybox", "Draw the sky", Kind::kCheck, 0, 1, "cg_skybox (a cheat setting)."},
    {"VIDEO", "The game's picture", "cg_letterbox", "Letterbox", Kind::kCheck, 0, 1,
     "cg_letterbox: black bars above and below, as in the game's scenes."},
    {"VIDEO", "Gun position (cheat settings)", "cg_gunX", "Gun forward / back", Kind::kFloat, -20, 20,
     "cg_gunX: moves the gun on the screen. 0 is the game's own place."},
    {"VIDEO", "Gun position (cheat settings)", "cg_gunY", "Gun left / right", Kind::kFloat, -20, 20, "cg_gunY."},
    {"VIDEO", "Gun position (cheat settings)", "cg_gunZ", "Gun up / down", Kind::kFloat, -20, 20, "cg_gunZ."},
    {"VIDEO", "View bob and kick", "cg_bobAmplitudeStanding", "View bob, standing", Kind::kFloat, 0, 0.05,
     "cg_bobAmplitudeStanding: how much the view sways while you walk. The game's own is 0.008; 0 = none."},
    {"VIDEO", "View bob and kick", "cg_bobAmplitudeDucked", "View bob, crouched", Kind::kFloat, 0, 0.05,
     "cg_bobAmplitudeDucked. The game's own is 0.015."},
    {"VIDEO", "View bob and kick", "cg_bobAmplitudeProne", "View bob, prone", Kind::kFloat, 0, 0.1,
     "cg_bobAmplitudeProne. The game's own is 0.03."},
    {"VIDEO", "View bob and kick", "cg_bobWeaponAmplitude", "Gun sway", Kind::kFloat, 0, 0.5,
     "cg_bobWeaponAmplitude: how much the gun sways while you walk. The game's own is 0.16."},
    {"VIDEO", "View bob and kick", "cg_bobWeaponRollAmplitude", "Gun roll while walking", Kind::kFloat, 0, 5,
     "cg_bobWeaponRollAmplitude. The game's own is 1.5; 0 = none."},
    {"VIDEO", "View bob and kick", "cg_bobWeaponLag", "Gun lags behind the view", Kind::kFloat, 0, 1,
     "cg_bobWeaponLag. The game's own is 0.25."},
    {"VIDEO", "View bob and kick", "cg_bobMax", "View bob, most", Kind::kFloat, 0, 20,
     "cg_bobMax: the most the view may sway. The game's own is 8."},
    {"VIDEO", "View bob and kick", "cg_bobWeaponMax", "Gun sway, most", Kind::kFloat, 0, 20,
     "cg_bobWeaponMax. The game's own is 10."},
    {"VIDEO", "View bob and kick", "cg_viewKickScale", "View kick when hit", Kind::kFloat, 0, 1,
     "cg_viewKickScale (a cheat setting). The game's own is 0.2; 0 = none."},
    {"VIDEO", "View bob and kick", "cg_viewKickMax", "View kick when hit, most", Kind::kFloat, 0, 180,
     "cg_viewKickMax (a cheat setting), in degrees. The game's own is 90."},
    // ----------------------------------------------------------------------------------------------------- GAME
    {"GAME", "The game", "cg_subtitles", "Subtitles", Kind::kCheck, 0, 1, "cg_subtitles."},
    {"GAME", "The game", "cg_blood", "Blood", Kind::kCheck, 0, 1, "cg_blood."},
    {"GAME", "The game", "cg_camerashake", "Camera shake", Kind::kCheck, 0, 1,
     "cg_camerashake: explosions shake the view (a cheat setting)."},
    {"GAME", "The game", "cg_shellshockblur", "Blur when shell-shocked", Kind::kCheck, 0, 1, "cg_shellshockblur."},
    {"GAME", "The game", "cg_altTankCam", "The other tank camera", Kind::kCheck, 0, 1, "cg_altTankCam."},
    {"GAME", "The game", "ai_corpseCount", "Bodies left on the ground", Kind::kInt, 0, 64,
     "ai_corpseCount: how many fallen soldiers stay before the oldest go."},
    {"GAME", "Third person (cheat settings)", "cg_thirdPerson", "Third-person view", Kind::kCheck, 0, 1,
     "cg_thirdPerson."},
    {"GAME", "Third person (cheat settings)", "cg_thirdPersonRange", "Distance", Kind::kFloat, 0, 400,
     "cg_thirdPersonRange, in game units."},
    {"GAME", "Third person (cheat settings)", "cg_thirdPersonAngle", "Angle", Kind::kFloat, -180, 180,
     "cg_thirdPersonAngle, in degrees around the player."},
    {"GAME", "Movement (the game's own numbers)", "g_speed", "Walking speed", Kind::kInt, 50, 400,
     "g_speed, in game units a second. The game's own is 190; it sets it again each level."},
    {"GAME", "Movement (the game's own numbers)", "g_gravity", "Gravity", Kind::kInt, 100, 1600,
     "g_gravity. The game's own is 800."},
    {"GAME", "Movement (the game's own numbers)", "g_knockback", "Knockback", Kind::kInt, 0, 4000,
     "g_knockback: how far a hit pushes. The game's own is 1000."},
    {"GAME", "Movement (the game's own numbers)", "bg_fallDamageMinHeight", "Falls hurt from this height",
     Kind::kInt, 0, 2000, "bg_fallDamageMinHeight, in game units (a cheat setting). The game's own is 256."},
    {"GAME", "Movement (the game's own numbers)", "bg_fallDamageMaxHeight", "Falls kill from this height",
     Kind::kInt, 0, 4000, "bg_fallDamageMaxHeight, in game units (a cheat setting). The game's own is 480."},
    {"GAME", "Shell shock (cheat settings)", "cg_shock_mouse", "Shell shock slows your aim", Kind::kCheck, 0, 1,
     "cg_shock_mouse."},
    {"GAME", "Shell shock (cheat settings)", "cg_shock_sound", "Shell shock muffles the sound", Kind::kCheck, 0, 1,
     "cg_shock_sound."},
    {"GAME", "Enemy aim (the game's own numbers)", "ai_playerNearAccuracy", "ai_playerNearAccuracy", Kind::kFloat, 0,
     1, "ai_playerNearAccuracy. The game's own is 0.5 (near is up to ai_playerNearRange, 800). What it changes in "
        "play hasn't been measured yet."},
    {"GAME", "Enemy aim (the game's own numbers)", "ai_playerFarAccuracy", "ai_playerFarAccuracy", Kind::kFloat, 0, 1,
     "ai_playerFarAccuracy. The game's own is 0.1 (far is from ai_playerFarRange, 2000)."},
    // ------------------------------------------------------------------------------------------------- CONTROLS
    {"CONTROLS", "The game's own controls", "sensitivityX", "Look speed, side to side", Kind::kFloat, 0.1, 5,
     "The game's own (sensitivityX): the controller's turn speed."},
    {"CONTROLS", "The game's own controls", "sensitivityY", "Look speed, up and down", Kind::kFloat, 0.1, 5,
     "The game's own (sensitivityY)."},
    {"CONTROLS", "The game's own controls", "cl_yawspeed", "Stick turn speed, side to side", Kind::kFloat, 0, 400,
     "cl_yawspeed, degrees a second at full stick."},
    {"CONTROLS", "The game's own controls", "cl_pitchspeed", "Stick turn speed, up and down", Kind::kFloat, 0, 400,
     "cl_pitchspeed."},
    {"CONTROLS", "The game's own controls", "joy_threshold", "The game's stick dead zone", Kind::kInt, 0, 100,
     "joy_threshold. The port's own dead zone is above."},
    {"CONTROLS", "The game's own controls", "cl_analogStickLean", "Lean with the stick", Kind::kCheck, 0, 1,
     "cl_analogStickLean."},
    {"CONTROLS", "The game's own controls", "cg_weaponCycleDelay", "Weapon switch delay", Kind::kInt, 0, 1000,
     "cg_weaponCycleDelay, in milliseconds."},
    {"CONTROLS", "The game's own controls", "cl_freelook", "Free look", Kind::kCheck, 0, 1, "cl_freelook."},
    {"CONTROLS", "The game's own controls", "m_pitch", "m_pitch", Kind::kFloat, -1, 1,
     "m_pitch: the up-and-down look factor. The game's own is 0.7; a negative number turns it over."},
    {"CONTROLS", "The game's own controls", "m_yaw", "m_yaw", Kind::kFloat, -1, 1,
     "m_yaw: the side-to-side look factor. The game's own is 0.7."},
    {"CONTROLS", "The game's own controls", "g_useholdtime", "Hold time to use", Kind::kInt, 0, 100,
     "g_useholdtime: how long the use button is held before it counts. The game's own is 10 (its unit isn't known "
     "yet)."},
    // ------------------------------------------------------------------------------------------------ INTERFACE
    {"INTERFACE", "The game's HUD", "cg_draw2D", "Show the HUD", Kind::kCheck, 0, 1, "cg_draw2D."},
    {"INTERFACE", "The game's HUD", "cg_drawStatus", "Health and ammunition", Kind::kCheck, 0, 1, "cg_drawStatus."},
    {"INTERFACE", "The game's HUD", "cg_drawCrosshair", "Crosshair", Kind::kCheck, 0, 1, "cg_drawCrosshair."},
    {"INTERFACE", "The game's HUD", "cg_crosshairDynamic", "Crosshair spreads", Kind::kCheck, 0, 1,
     "cg_crosshairDynamic."},
    {"INTERFACE", "The game's HUD", "cg_crosshairAlpha", "Crosshair opacity", Kind::kFloat, 0, 1,
     "cg_crosshairAlpha."},
    {"INTERFACE", "The game's HUD", "cg_crosshairAlphaMin", "Crosshair opacity, least", Kind::kFloat, 0, 1,
     "cg_crosshairAlphaMin. The game's own is 0.7."},
    {"INTERFACE", "The game's HUD", "cg_hudAlpha", "HUD opacity", Kind::kFloat, 0, 1, "cg_hudAlpha."},
    {"INTERFACE", "The game's HUD", "cg_hudCompassSize", "Compass size", Kind::kFloat, 0.25, 2, "cg_hudCompassSize."},
    {"INTERFACE", "The game's HUD", "cg_cursorHints", "Use hints", Kind::kInt, 0, 4,
     "cg_cursorHints: how the \"press to use\" hints are shown (0 = none)."},
    {"INTERFACE", "The game's HUD", "cg_hintFadeTime", "Hint fade time", Kind::kInt, 0, 2000,
     "cg_hintFadeTime, in milliseconds."},
    {"INTERFACE", "The game's HUD", "cg_hudDamageIconTime", "Damage arrows stay", Kind::kInt, 0, 10000,
     "cg_hudDamageIconTime, in milliseconds."},
    {"INTERFACE", "The game's HUD", "cg_hudDamageIconInScope", "Damage arrows while in a scope", Kind::kCheck, 0, 1,
     "cg_hudDamageIconInScope."},
    {"INTERFACE", "The game's HUD", "cg_centertime", "Messages stay", Kind::kFloat, 0, 10,
     "cg_centertime, in seconds."},
    {"INTERFACE", "The game's HUD", "cg_hudCompassMaxRange", "Compass range", Kind::kInt, 256, 4096,
     "cg_hudCompassMaxRange, in game units. The game's own is 1024."},
    {"INTERFACE", "The game's HUD", "cg_hudCompassSpringyPointers", "Springy compass pointers", Kind::kCheck, 0, 1,
     "cg_hudCompassSpringyPointers."},
    {"INTERFACE", "The game's HUD", "cg_hudObjectiveRingTime", "Objective rings stay", Kind::kInt, 0, 30000,
     "cg_hudObjectiveRingTime, in milliseconds. The game's own is 10000."},
    {"INTERFACE", "The game's HUD", "cg_hudObjectiveMaxRange", "Objective marker range", Kind::kInt, 256, 8192,
     "cg_hudObjectiveMaxRange, in game units. The game's own is 2048."},
    {"INTERFACE", "The game's HUD", "cg_hudObjectiveMinAlpha", "Objective markers, least opacity", Kind::kFloat, 0, 1,
     "cg_hudObjectiveMinAlpha. The game's own is 1."},
    {"INTERFACE", "The game's HUD", "cg_drawpaused", "Show \"paused\"", Kind::kCheck, 0, 1, "cg_drawpaused."},
    // ---------------------------------------------------------------------------------------------- PERFORMANCE
    {"PERFORMANCE", "The game's own", "com_maxfps", "The game's frame rate limit", Kind::kInt, 0, 1000,
     "com_maxfps; 0 = none. The port's own limit is on VIDEO."},
    {"PERFORMANCE", "The game's own", "fx_enable", "Effects", Kind::kCheck, 0, 1,
     "fx_enable: smoke, fire, sparks (a cheat setting)."},
    {"PERFORMANCE", "The game's own", "cg_tracerchance", "Tracers", Kind::kFloat, 0, 1,
     "cg_tracerchance: how often a bullet draws a tracer."},
    {"PERFORMANCE", "The game's own", "cg_tracerlength", "Tracer length", Kind::kFloat, 0, 1000,
     "cg_tracerlength, in game units. The game's own is 250."},
    {"PERFORMANCE", "The game's own", "cg_tracerwidth", "Tracer width", Kind::kFloat, 0, 8,
     "cg_tracerwidth. The game's own is 0.8."},
};

// The DVARS page's groups. A cvar goes into the first whose rule takes it (see GroupOf).
const char* const kGroups[] = {
    "View and gun", "HUD", "Shell shock", "Effects", "Movement", "Controls", "AI and difficulty",
    "Game", "Language", "System", "Debug and network", "Other",
};
enum Group { kView, kHud, kShock, kEffects, kMovement, kControls, kAi, kGame, kLanguage, kSystem, kDebug, kOther };

bool StartsWith(const char* name, const char* prefix) {
  for (; *prefix; ++prefix, ++name)
    if (std::tolower(static_cast<unsigned char>(*name)) != std::tolower(static_cast<unsigned char>(*prefix)))
      return false;
  return true;
}

bool Contains(const char* name, const char* part) {
  for (; *name; ++name)
    if (StartsWith(name, part)) return true;
  return false;
}

bool AnyPrefix(const char* name, std::initializer_list<const char*> prefixes) {
  for (const char* p : prefixes)
    if (StartsWith(name, p)) return true;
  return false;
}

bool Is(const char* name, std::initializer_list<const char*> names) {
  for (const char* n : names)
    if (StartsWith(name, n) && std::strlen(name) == std::strlen(n)) return true;
  return false;
}

}  // namespace

const DvarRow* DvarRows(size_t* count) {
  *count = std::size(kRows);
  return kRows;
}

const DvarRow* FindDvarRow(const char* name) {
  for (const DvarRow& r : kRows)
    if (StartsWith(name, r.name) && std::strlen(name) == std::strlen(r.name)) return &r;
  return nullptr;
}

const char* const* DvarGroups(size_t* count) {
  *count = std::size(kGroups);
  return kGroups;
}

size_t GroupOf(const char* name) {
  if (Contains(name, "debug") || AnyPrefix(name, {"show", "cl_show", "net"}) ||
      Is(name, {"cl_avidemo", "cl_forceavidemo", "cl_freeze", "cl_frameadvance", "cl_testAnimWeight", "cl_nodelta",
                "com_speeds", "com_statmon", "com_animCheck", "viewlog", "renderheaps", "renderpaks", "progressbar",
                "cg_norender", "cg_nopredict", "cg_drawPosition", "cl_xmodelcheck", "exception",
                "ai_NoCriticalSections"}))
    return kDebug;
  if (AnyPrefix(name, {"cl_language", "loc_"})) return kLanguage;
  if (AnyPrefix(name, {"cg_gun", "cg_bob", "cg_viewKick", "cg_thirdPerson"}) ||
      Is(name, {"cg_fov", "cg_drawGun", "cg_viewsize", "cg_letterbox", "cg_widescreen", "cg_skybox", "cg_errordecay",
                "letterbox_enabled", "bg_stickyAimRender"}))
    return kView;
  if (AnyPrefix(name, {"cg_shock", "cg_shellshock"}) || Is(name, {"cg_camerashake"})) return kShock;
  if (AnyPrefix(name, {"cg_draw", "cg_hud", "cg_crosshair", "cg_deadscreen", "cg_victoryscreen"}) ||
      Is(name, {"cg_cursorHints", "cg_hintFadeTime", "cg_centertime", "cg_subtitles", "statusbar"}))
    return kHud;
  if (AnyPrefix(name, {"fx_", "cg_tracer"}) || Is(name, {"cg_blood"})) return kEffects;
  if (AnyPrefix(name, {"bg_", "pmove_", "g_bounds", "cm_"}) || Is(name, {"g_speed", "g_gravity", "g_knockback"}))
    return kMovement;
  if (AnyPrefix(name, {"ai_", "g_player", "g_accuracy", "g_friendlyfire"}) ||
      Is(name, {"g_gameskill", "conical_spread", "disable_ai", "enemy_radar", "g_spawnai"}))
    return kAi;
  if (Is(name, {"cl_running", "cl_paused", "sv_running", "cl_conXOffset", "cl_noprint"})) return kSystem;
  if (AnyPrefix(name, {"cl_", "m_", "sensitivity", "in_", "joy_"}) ||
      Is(name, {"g_vehControlMode", "cg_controllerconfig", "cg_weaponCycleDelay", "g_useholdtime"}))
    return kControls;
  if (AnyPrefix(name, {"fs_", "com_", "developer", "sv_", "journal", "sys_", "r_"}) ||
      Is(name, {"timescale", "fixedtime", "logfile", "arch", "num_banks", "gamename", "gamedate", "name",
                "mapname", "nextmap", "g_reloading", "cg_configversion"}))
    return kSystem;
  if (AnyPrefix(name, {"g_", "ui_", "cg_"}) || Is(name, {"checkpoint", "introScreen", "nextdemo"})) return kGame;
  return kOther;
}

}  // namespace bro_f2
