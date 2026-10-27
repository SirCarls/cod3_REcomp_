// cod3 - cg_fov / cg_fovScale: the player's values, their limits, and the
// game's own FOV changes kept apart from them (see fov.h).
//
// CG_GetViewFov (0x8243A198) is the single place cgame turns cg_fov into the
// horizontal view angle (it also folds in ADS, turret and vehicle zoom). Its
// result feeds the refdef, the zoom sensitivity scale and the viewmodel, so
// scaling the return value here behaves exactly like World at War's
// cg_fovScale: hip-fire and ADS both scale, sensitivity stays consistent.
//
// cg_fov itself already exists in CoD3 (default 65, clamped to [1,160] by the
// game, not cheat protected, CVAR_TEMP so the game never saves it);
// cg_fovScale is registered by the port (Engine::FirstFrame). Both are saved
// in the console config (cod3_config.cfg) from here, and only when the player
// changed them.
//
// Threads: the player's pair (g_player_fov, g_player_scale) is read anywhere
// and changed under g_player_mutex. Everything else here runs on the game's main thread: the
// engine frame (Cbuf_Execute hook), cgame (CG_GetViewFov), the level scripts
// (fibers on that thread) and the interaction code.

#include "game/fov.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/engine.h"
#include "game/game_api.h"
#include "game/guest.h"
#include "game/mouse_look.h"
#include "ui/game_console.h"

// [claude-9b] 2026-10-27 The view from a vehicle (owner 10-27 03:15: "vehicle fov needs to be a slider").
// Every vehicle and turret view gets its FOV from CG_GetViewFov (the hook below), and the game picks
// it there: a tank's crew (type 2, riding or on its gun, with cg_altTankCam) and a jeep's driver get
// cg_fov itself, other seats the seat weapon's turretFov (weapon def +0x89C; multiplayer +0x8A0) or,
// fully aimed, its turretAdsFov (+0x640). No vehicle data has a FOV of its own and no vehicle code
// changes cg_fov (claude-9b-think, FINDINGS-2026-10-27). So the scale goes on that result while the
// playerState says the player rides (flags +0xD0, 0x100000) or works a tank's gun (0x6000 with
// vehicle type 2 at +0x4F8, the game's own test), on top of cg_fovScale; the mouse follows it the
// same way (MouseLook::SetViewFov).
REXCVAR_DEFINE_DOUBLE(cod3_vehicle_fov_scale, 1.0, "cod3/View",
                      "Multiplies the field of view while you ride a vehicle or work a tank's gun (tanks, "
                      "jeeps, bikes, their guns), on top of the FOV scale: 1 = the game's own view")
    .range(0.5, 2.0);

REXCVAR_DEFINE_BOOL(cod3_debug_fov, false, "cod3",
                    "Log the FOV state (the player's values, the dvars, cgame's copy, the view) every 5 s");
// For automated tests (0 = off): the game-made changes the port must keep
// apart from the player's FOV, without playing to them. Each runs once per
// new value (the command line, or the console / cod3_autoexec in a level).
REXCVAR_DEFINE_DOUBLE(cod3_test_fov_script, 0.0, "cod3",
                      "Test: once cg_fov exists, a level-script write of this cg_fov (as Black Baron's "
                      "scripted view does)");
REXCVAR_DEFINE_DOUBLE(cod3_test_fov_cgame, 0.0, "cod3",
                      "Test: once a level is drawn, cgame's copy of cg_fov set to this (as an "
                      "interaction's end does)");

namespace cod3::fov {
namespace {

namespace a = cod3::addr;
using guest::ReadU32;

constexpr const char* kFovDvar = "cg_fov";
constexpr const char* kScaleDvar = "cg_fovScale";
constexpr uint32_t kMaxLocalClients = 4;  // the Interaction* array (addresses.h interact::kPerClient)

// --- limits -----------------------------------------------------------------
// Rounded inward to steps of 1 / per_unit (1: whole degrees, 100: two
// decimals); the slack keeps an exact bound (65 / 0.5 = 130) on its own step.
// Dividing by per_unit gives the float nearest the decimal (1.98, not
// 198 x 0.01 = 1.9799999), so a typed 1.98 is not moved.
float CeilTo(float v, float per_unit) { return std::ceil(v * per_unit - 1e-4f) / per_unit; }
float FloorTo(float v, float per_unit) { return std::floor(v * per_unit + 1e-4f) / per_unit; }

bool Usable(float v) { return std::isfinite(v); }

// The text a value is saved and set with ("%g", like the console shows it),
// and the float the game reads back from that text, so the player's value and
// the dvar's compare equal. No heap allocation.
struct ValueText {
  char text[32];
};
ValueText Format(float v) {
  ValueText t;
  std::snprintf(t.text, sizeof(t.text), "%g", v);
  return t;
}
float Quantize(float v) { return std::strtof(Format(v).text, nullptr); }

float BitsToFloat(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

// --- the player's values -------------------------------------------------------
std::mutex g_player_mutex;
std::atomic<float> g_player_fov{kDefaultFov};
std::atomic<float> g_player_scale{kDefaultScale};
std::atomic<bool> g_scripted_view{false};

Settings Player() { return {g_player_fov.load(), g_player_scale.load()}; }
float PlayerFov() { return g_player_fov.load(); }
float PlayerScale() { return g_player_scale.load(); }

enum class Changed { kFov, kScale };

// The player changed one value (`what`; `asked` holds it, the other is taken
// as it is now) or, with `both`, the pair: fitted, remembered and, with
// `save`, what changed saved in the console config. Returns the new pair.
Settings ChangePlayer(Changed what, Settings asked, bool both, bool save) {
  Settings before;
  Settings now;
  {
    std::lock_guard lock(g_player_mutex);
    before = Player();
    Settings want = asked;
    if (!both) {
      if (what == Changed::kFov) want.scale = before.scale;
      else want.fov = before.fov;
    }
    now = what == Changed::kFov ? FitFov(want.fov, want.scale) : FitScale(want.fov, want.scale);
    now.fov = Quantize(now.fov);
    now.scale = Quantize(now.scale);
    g_player_fov = now.fov;
    g_player_scale = now.scale;
  }
  if (save) {
    auto& engine = Engine::Get();
    if (now.fov != before.fov) engine.SetArchived(kFovDvar, Format(now.fov).text);
    if (now.scale != before.scale) engine.SetArchived(kScaleDvar, Format(now.scale).text);
  }
  return now;
}

// A console note when a value asked for was limited (not for per-frame use).
void NoteLimited(const char* dvar, float asked, float now, const std::string& rule) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s %g is outside the allowed range; set to %g. ", dvar, asked, now);
  REXLOG_INFO("cod3: {}{}", buf, rule);
  ui::GameConsole::Get().Print(ui::ConsoleChannel::kError, std::string("^3") + buf + rule + "\n");
}

// --- the dvars as the game has them (game main thread) ----------------------------
struct TrackedDvar {
  uint32_t cvar = 0;        // cvar_t*, 0 until the game has the dvar
  uint32_t seen_bits = 0;   // its value when last looked at (bits, so NaN compares too)
  bool seen = false;
  bool scripted = false;    // a level script set it and hasn't put the player's back yet
};
TrackedDvar g_fov;
TrackedDvar g_scale;
uint64_t g_frames = 0;
float g_last_view_fov = 0.0f;  // CG_GetViewFov's last result, for cod3_debug_fov

// The player's last cg_fov values. The game reads cg_fov once and writes that
// value back later (a level script's saved view, an interaction's start), so
// a game write of one of them puts the player's view back.
std::array<float, 8> g_past_fovs{};
size_t g_past_count = 0;
size_t g_past_next = 0;

void RememberPlayerFov(float fov) {
  for (size_t i = 0; i < g_past_count; ++i)
    if (g_past_fovs[i] == fov) return;
  g_past_fovs[g_past_next] = fov;
  g_past_next = (g_past_next + 1) % g_past_fovs.size();
  g_past_count = std::min(g_past_count + 1, g_past_fovs.size());
}

bool IsPlayerFov(float value) {
  if (value == g_player_fov.load()) return true;
  for (size_t i = 0; i < g_past_count; ++i)
    if (g_past_fovs[i] == value) return true;
  return false;
}

void UpdateScriptedFlag() { g_scripted_view = g_fov.scripted || g_scale.scripted; }

void Look(uint8_t* base, TrackedDvar& t) {
  t.seen_bits = ReadU32(base, t.cvar + a::cvar::kValue);
  t.seen = true;
}

// Sets one dvar to `value` if it isn't already, the way the console would.
void SetDvar(PPCContext& ctx, uint8_t* base, TrackedDvar& t, const char* name, float value) {
  if (t.cvar && game::CvarValue(base, t.cvar) == value) {
    Look(base, t);
    return;
  }
  Engine::CvarSet(ctx, base, name, Format(value).text, /*force=*/false);
  if (!t.cvar) t.cvar = Engine::CvarFind(ctx, base, name);
  if (t.cvar) Look(base, t);
}

// The player's values into the game's dvars. A player's choice ends a
// scripted view (the script's later restore is still recognized).
void ApplyPlayer(PPCContext& ctx, uint8_t* base) {
  const Settings p = Player();
  SetDvar(ctx, base, g_fov, kFovDvar, p.fov);
  if (!g_scale.cvar) g_scale.cvar = Engine::Get().fov_scale_cvar();
  SetDvar(ctx, base, g_scale, kScaleDvar, p.scale);
  RememberPlayerFov(p.fov);
  g_fov.scripted = g_scale.scripted = false;
  UpdateScriptedFlag();
}

// Whether the local player's interaction (vehicle, turret, melee ...) is
// running. Unknown counts as running, so cgame's copy is then left alone.
bool InteractionRunning(uint8_t* base) {
  const uint32_t client = ReadU32(base, a::kActiveLocalClient);
  if (client >= kMaxLocalClients) return true;
  const uint32_t interaction = ReadU32(base, a::interact::kPerClient + client * 4);
  if (interaction == 0) return false;
  if (interaction < 0x10000 || interaction >= 0xC0000000u || (interaction & 3)) return true;
  return ReadU32(base, interaction + a::interact::kState) != 0;
}

// cgame's copy of cg_fov (the vmCvar CG_GetViewFov reads) back to the dvar's
// value once no interaction is running: an interaction writes the copy
// directly and, at its end, puts back what it saved at its start, which was
// an old value when the player changed the FOV meanwhile. `when` is for the
// log line (none when null).
void FollowDvarInCgame(uint8_t* base, const char* when) {
  if (!g_fov.cvar || InteractionRunning(base)) return;
  const uint32_t dvar_bits = ReadU32(base, g_fov.cvar + a::cvar::kValue);
  const uint32_t copy = a::kCgFovVmCvar + a::kVmCvarValue;
  const uint32_t copy_bits = ReadU32(base, copy);
  if (copy_bits == dvar_bits) return;
  guest::WriteU32(base, copy, dvar_bits);
  guest::WriteU32(base, a::kCgFovVmCvar + a::kVmCvarInteger,
                  static_cast<uint32_t>(game::CvarInteger(base, g_fov.cvar)));
  // Before cgame first registers cg_fov its copy is still 0: nothing to say.
  if (when && copy_bits != 0)
    REXLOG_INFO("cod3: FOV: cgame's cg_fov was {:g} {}; back to {:g}", BitsToFloat(copy_bits), when,
                BitsToFloat(dvar_bits));
}

// A dvar changed and none of the port's own writes or a level script did it:
// the console, an exec'd config, set / seta / reset. That is the player's
// choice: limited to the allowed range, saved, and written back if limited.
void PlayerChangedDvar(PPCContext& ctx, uint8_t* base, bool is_fov, float value) {
  const Changed what = is_fov ? Changed::kFov : Changed::kScale;
  const Settings now = ChangePlayer(what, {value, value}, /*both=*/false, /*save=*/true);
  const float got = is_fov ? now.fov : now.scale;
  if (!(std::fabs(value - got) <= 1e-4f)) {
    NoteLimited(is_fov ? kFovDvar : kScaleDvar, value, got,
                is_fov ? DescribeFovRange(now.scale) : DescribeScaleRange(now.fov));
  } else {
    REXLOG_INFO("cod3: FOV: {} {:g} (effective {:g}), saved", is_fov ? kFovDvar : kScaleDvar, got,
                now.fov * now.scale);
  }
  ApplyPlayer(ctx, base);
  FollowDvarInCgame(base, nullptr);  // cgame may have copied the asked-for value already
}

void CheckDvar(PPCContext& ctx, uint8_t* base, TrackedDvar& t, bool is_fov) {
  const uint32_t bits = ReadU32(base, t.cvar + a::cvar::kValue);
  if (t.seen && bits == t.seen_bits) return;  // the usual case: nothing to do
  t.seen = true;
  t.seen_bits = bits;
  const float value = BitsToFloat(bits);
  const Settings p = Player();
  if (value == (is_fov ? p.fov : p.scale)) {
    // Already the player's (the first look, or the console typed it again).
    t.scripted = false;
    UpdateScriptedFlag();
    return;
  }
  PlayerChangedDvar(ctx, base, is_fov, value);
}

// cod3_test_fov_script / cod3_test_fov_cgame (see their definitions): each
// runs once for every new nonzero value.
void RunTests(PPCContext& ctx, uint8_t* base) {
  static double script_done = 0.0;
  static double cgame_done = 0.0;
  const double script = REXCVAR_GET(cod3_test_fov_script);
  if (script != 0.0 && script != script_done && g_fov.cvar) {
    script_done = script;
    REXLOG_INFO("cod3: FOV test: a level-script write of cg_fov {:g}", script);
    guest::Call call(ctx, base);
    const uint32_t name = call.PushString(kFovDvar);
    call.Invoke(a::level_cvar::kSetFloat, {name}, {script});  // through the hook below, as a level DLL calls it
  }
  const double cgame = REXCVAR_GET(cod3_test_fov_cgame);
  if (cgame != 0.0 && cgame != cgame_done && g_last_view_fov > 0.0f) {
    cgame_done = cgame;
    REXLOG_INFO("cod3: FOV test: cgame's copy of cg_fov set to {:g}", cgame);
    guest::WriteF32(base, a::kCgFovVmCvar + a::kVmCvarValue, static_cast<float>(cgame));
  }
}

// Every engine frame, and in cgame right before the view is computed (so a
// console line of this frame is limited before anything is drawn with it).
void SyncNow(PPCContext& ctx, uint8_t* base, bool from_view) {
  if (!from_view) {
    ++g_frames;
    // The client game registers cg_fov when it first starts; a guest call,
    // so only every 30th frame until then.
    if (!g_fov.cvar && (g_frames % 30) == 1) g_fov.cvar = Engine::CvarFind(ctx, base, kFovDvar);
    if (!g_scale.cvar) g_scale.cvar = Engine::Get().fov_scale_cvar();
  }
  if (g_fov.cvar) CheckDvar(ctx, base, g_fov, /*is_fov=*/true);
  if (g_scale.cvar) CheckDvar(ctx, base, g_scale, /*is_fov=*/false);
  if (g_fov.cvar) FollowDvarInCgame(base, "with no interaction running");
  if (!from_view) RunTests(ctx, base);
  if (!from_view && REXCVAR_GET(cod3_debug_fov)) {
    static auto next = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
      next = now + std::chrono::seconds(5);
      const Settings p = Player();
      REXLOG_INFO("cod3: FOV state: player {:g} x {:g}; cg_fov {:g}, cg_fovScale {:g}, cgame's copy {:g}; "
                  "interaction {}; scripted view {}; last view {:.1f}",
                  p.fov, p.scale, g_fov.cvar ? game::CvarValue(base, g_fov.cvar) : -1.0f,
                  g_scale.cvar ? game::CvarValue(base, g_scale.cvar) : -1.0f,
                  guest::ReadF32(base, a::kCgFovVmCvar + a::kVmCvarValue),
                  InteractionRunning(base) ? "running" : "none", g_scripted_view.load() ? "yes" : "no",
                  g_last_view_fov);
    }
  }
}

// --- level scripts ---------------------------------------------------------------
// Case-insensitive compare of a guest string with `name`, in place.
bool GuestNameIs(uint8_t* base, uint32_t addr, const char* name) {
  if (addr < 0x10000 || addr >= 0xC0000000u) return false;
  const char* guest = reinterpret_cast<const char*>(base + addr);
  for (size_t i = 0;; ++i) {
    const char want = name[i];
    const char got = guest[i];
    if (std::tolower(static_cast<unsigned char>(got)) != std::tolower(static_cast<unsigned char>(want)))
      return false;
    if (want == '\0') return true;
  }
}

enum class Watched { kNone, kFov, kScale };
Watched WatchedDvar(uint8_t* base, uint32_t name) {
  if (GuestNameIs(base, name, kFovDvar)) return Watched::kFov;
  if (GuestNameIs(base, name, kScaleDvar)) return Watched::kScale;
  return Watched::kNone;
}

// A level script just set cg_fov or cg_fovScale (its setcvar returned the
// cvar_t* in r3). Kept as the game's, never saved as the player's.
void ScriptSetDvar(PPCContext& ctx, uint8_t* base, Watched which, uint32_t cvar) {
  const bool is_fov = which == Watched::kFov;
  TrackedDvar& t = is_fov ? g_fov : g_scale;
  if (!t.cvar) t.cvar = cvar;
  if (!t.cvar) return;
  Look(base, t);
  const float value = BitsToFloat(t.seen_bits);
  const Settings p = Player();
  const bool players = is_fov ? IsPlayerFov(value) : value == p.scale;
  if (players) {
    // The script puts its saved view back: that may be an older choice of
    // the player's (changed during the scripted view), so the current one
    // goes in.
    const float current = is_fov ? p.fov : p.scale;
    if (t.scripted || value != current)
      REXLOG_INFO("cod3: FOV: a level script put the view back ({} {:g}); the player's {:g} is used",
                  is_fov ? kFovDvar : kScaleDvar, value, current);
    t.scripted = false;
    if (value != current) SetDvar(ctx, base, t, is_fov ? kFovDvar : kScaleDvar, current);
  } else if (!t.scripted) {
    t.scripted = true;
    REXLOG_INFO("cod3: FOV: a level script set {} to {:g} for a scripted view (not saved; the player's "
                "{:g} comes back when the script ends it, or at the next level start)",
                is_fov ? kFovDvar : kScaleDvar, value, is_fov ? p.fov : p.scale);
  }
  UpdateScriptedFlag();
}

}  // namespace

// --- limits (fov.h) ---------------------------------------------------------------
Range FovRange(float scale) {
  if (!Usable(scale) || scale <= 0.0f) scale = kDefaultScale;
  Range r{std::max(kMinFov, CeilTo(kMinEffective / scale, 1.0f)),
          std::min(kMaxFov, FloorTo(kMaxEffective / scale, 1.0f))};
  // Only a scale outside every allowed pair gets here (FitFov moves it first).
  if (r.min > r.max) r.min = r.max = scale < 1.0f ? kMaxFov : kMinFov;
  return r;
}

Range ScaleRange(float fov) {
  if (!Usable(fov)) fov = kDefaultFov;
  fov = std::clamp(fov, kMinFov, kMaxFov);
  return {std::max(kMinScale, CeilTo(kMinEffective / fov, 100.0f)),
          std::min(kMaxScale, FloorTo(kMaxEffective / fov, 100.0f))};
}

Settings FitFov(float fov, float scale) {
  if (!Usable(scale)) scale = kDefaultScale;
  // A scale some cg_fov fits: from what the widest cg_fov allows up to 2.
  scale = std::clamp(scale, ScaleRange(kMaxFov).min, kMaxScale);
  if (!Usable(fov)) fov = kDefaultFov;
  const Range r = FovRange(scale);
  return {std::clamp(fov, r.min, r.max), scale};
}

Settings FitScale(float fov, float scale) {
  if (!Usable(fov)) fov = kDefaultFov;
  fov = std::clamp(fov, kMinFov, kMaxFov);
  if (!Usable(scale)) scale = kDefaultScale;
  const Range r = ScaleRange(fov);
  return {fov, std::clamp(scale, r.min, r.max)};
}

std::string DescribeFovRange(float scale) {
  const Range r = FovRange(scale);
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "cg_fov x cg_fovScale stays within %g to %g: with cg_fovScale %.2f, cg_fov goes from %g "
                "to %g.",
                kMinEffective, kMaxEffective, scale, r.min, r.max);
  return buf;
}

std::string DescribeScaleRange(float fov) {
  const Range r = ScaleRange(fov);
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "cg_fov x cg_fovScale stays within %g to %g: with cg_fov %g, cg_fovScale goes from %.2f "
                "to %.2f.",
                kMinEffective, kMaxEffective, fov, r.min, r.max);
  return buf;
}

bool ScriptedViewActive() { return g_scripted_view.load(); }

// --- the player's values (fov.h) ---------------------------------------------------
void LoadPlayerSettings(std::map<std::string, std::string>& archived) {
  float fov = kDefaultFov;
  float scale = kDefaultScale;
  const auto fov_it = archived.find(kFovDvar);
  const auto scale_it = archived.find(kScaleDvar);
  if (fov_it != archived.end()) fov = std::strtof(fov_it->second.c_str(), nullptr);
  if (scale_it != archived.end()) scale = std::strtof(scale_it->second.c_str(), nullptr);
  // From a file the FOV is kept and the scale fitted to it.
  const Settings s = ChangePlayer(Changed::kScale, {fov, scale}, /*both=*/true, /*save=*/false);
  if (s.fov != fov || s.scale != scale) {
    REXLOG_WARN("cod3: console config: cg_fov {:g} x cg_fovScale {:g} is outside the allowed range "
                "(effective {:g} to {:g}); using {:g} x {:g}",
                fov, scale, kMinEffective, kMaxEffective, s.fov, s.scale);
    // What Engine::FirstFrame applies, and what is saved next time.
    if (fov_it != archived.end()) fov_it->second = Format(s.fov).text;
    if (scale_it != archived.end()) scale_it->second = Format(s.scale).text;
  }
}

namespace {
// The player's choice from F2, the Options rows or the console's `fov`.
void SetPlayerValue(Changed what, Settings asked, bool both) {
  const Settings now = ChangePlayer(what, asked, both, /*save=*/true);
  // The F2 sliders and the Options rows stay within the range; a typed
  // value (the console's `fov 30`) may not.
  const bool is_fov = what == Changed::kFov;
  const float asked_value = is_fov ? asked.fov : asked.scale;
  const float got = is_fov ? now.fov : now.scale;
  if (Usable(asked_value) && std::fabs(asked_value - got) > 0.01f)
    NoteLimited(is_fov ? kFovDvar : kScaleDvar, asked_value, got,
                is_fov ? DescribeFovRange(now.scale) : DescribeScaleRange(now.fov));
}
}  // namespace

void OnLevelStart(PPCContext& ctx, uint8_t* base) {
  const bool was_scripted = g_fov.scripted || g_scale.scripted;
  if (!g_fov.cvar) g_fov.cvar = Engine::CvarFind(ctx, base, kFovDvar);
  if (!g_fov.cvar) return;  // nothing to put back before cgame ever ran
  const Settings p = Player();
  const float fov_before = game::CvarValue(base, g_fov.cvar);
  ApplyPlayer(ctx, base);
  // cgame copies the dvar when it starts the level; an interaction cut off
  // by the level change may have left its own value in the copy until then.
  guest::WriteF32(base, a::kCgFovVmCvar + a::kVmCvarValue, p.fov);
  guest::WriteU32(base, a::kCgFovVmCvar + a::kVmCvarInteger, static_cast<uint32_t>(static_cast<int32_t>(p.fov)));
  if (was_scripted || fov_before != p.fov)
    REXLOG_INFO("cod3: FOV: level start: cg_fov {:g} -> the player's {:g} (cg_fovScale {:g})", fov_before,
                p.fov, p.scale);
}

void AdoptFromGuest(PPCContext& ctx, uint8_t* base) {
  if (!g_fov.cvar) g_fov.cvar = Engine::CvarFind(ctx, base, kFovDvar);
  if (!g_scale.cvar) g_scale.cvar = Engine::Get().fov_scale_cvar();
  const float fov = g_fov.cvar ? game::CvarValue(base, g_fov.cvar) : kDefaultFov;
  const float scale = g_scale.cvar ? game::CvarValue(base, g_scale.cvar) : kDefaultScale;
  ChangePlayer(Changed::kScale, {fov, scale}, /*both=*/true, /*save=*/false);
  ApplyPlayer(ctx, base);
}

void Sync(PPCContext& ctx, uint8_t* base) { SyncNow(ctx, base, /*from_view=*/false); }

}  // namespace cod3::fov

// ---------------------------------------------------------------------------
// Engine's FOV calls (engine.h): the player's values, limited and saved.
// ---------------------------------------------------------------------------
namespace cod3 {

float Engine::fov() const { return fov::PlayerFov(); }
float Engine::fov_scale() const { return fov::PlayerScale(); }

void Engine::SetFov(float value) {
  fov::SetPlayerValue(fov::Changed::kFov, {value, value}, /*both=*/false);
  QueueGuestTask([](PPCContext& ctx, uint8_t* base) { fov::ApplyPlayer(ctx, base); });
}

void Engine::SetFovScale(float scale) {
  fov::SetPlayerValue(fov::Changed::kScale, {scale, scale}, /*both=*/false);
  QueueGuestTask([](PPCContext& ctx, uint8_t* base) { fov::ApplyPlayer(ctx, base); });
}

void Engine::SetFovSettings(float value, float scale) {
  // Both at once (F2's Reset): the pair is fitted together, the FOV kept.
  fov::SetPlayerValue(fov::Changed::kScale, {value, scale}, /*both=*/true);
  QueueGuestTask([](PPCContext& ctx, uint8_t* base) { fov::ApplyPlayer(ctx, base); });
}

void Engine::ApplyFovFromGuest(PPCContext& ctx, uint8_t* base, float value) {
  fov::SetPlayerValue(fov::Changed::kFov, {value, value}, /*both=*/false);
  fov::ApplyPlayer(ctx, base);
}

}  // namespace cod3

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
namespace {
// [claude-9b] 2026-10-27 The player's playerState says a vehicle view (cod3_vehicle_fov_scale above).
namespace vehicle_view {
#if defined(COD3_MP)  // [mp] codmp_xenonf.xex (claude-9b-think, FINDINGS-2026-10-27 section 4)
constexpr uint32_t kEntityTable = 0x82BCC7FC;  // *(it) + 4 * (client + 1): the player's entity
constexpr uint32_t kEntPs = 0x200;             // its playerState
constexpr uint32_t kPsFlags = 0xF0;
constexpr uint32_t kPsVehicleType = 0x518;
#else
constexpr uint32_t kEntityTable = 0x82A2AB40;
constexpr uint32_t kEntPs = 0x210;
constexpr uint32_t kPsFlags = 0xD0;
constexpr uint32_t kPsVehicleType = 0x4F8;
#endif
constexpr uint32_t kRiding = 0x100000;   // in a vehicle, any seat, any camera
constexpr uint32_t kOnAGun = 0x6000;     // working a turret
constexpr uint32_t kTankType = 2;
bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }
bool Active(uint8_t* base) {
  const uint32_t client = cod3::guest::ReadU32(base, cod3::addr::kActiveLocalClient);
  if (client >= 4) return false;
  const uint32_t table = cod3::guest::ReadU32(base, kEntityTable);
  if (!Valid(table)) return false;
  const uint32_t ent = cod3::guest::ReadU32(base, table + 4 * (client + 1));
  if (!Valid(ent)) return false;
  const uint32_t ps = cod3::guest::ReadU32(base, ent + kEntPs);
  if (!Valid(ps)) return false;
  const uint32_t flags = cod3::guest::ReadU32(base, ps + kPsFlags);
  if (flags & kRiding) return true;
  return (flags & kOnAGun) && cod3::guest::ReadU32(base, ps + kPsVehicleType) == kTankType;
}
}  // namespace vehicle_view
}  // namespace

REX_EXTERN(__imp__CG_GetViewFov);
REX_HOOK_RAW(CG_GetViewFov) {
  cod3::fov::SyncNow(ctx, base, /*from_view=*/true);
  __imp__CG_GetViewFov(ctx, base);

  float scale = 1.0f;
  if (uint32_t scale_cvar = cod3::Engine::Get().fov_scale_cvar()) {
    float v = cod3::guest::ReadF32(base, scale_cvar + cod3::addr::cvar::kValue);
    if (v > 0.0f) scale = std::clamp(v, cod3::fov::kMinScale, cod3::fov::kMaxScale);  // also rejects NaN
  }
  // [claude-9b] cod3_vehicle_fov_scale, in a vehicle's view only
  const double vehicle = REXCVAR_GET(cod3_vehicle_fov_scale);
  if (vehicle != 1.0 && std::isfinite(vehicle) && vehicle_view::Active(base))
    scale *= static_cast<float>(std::clamp(vehicle, 0.5, 2.0));
  if (scale != 1.0f) ctx.f1.f64 = std::clamp(ctx.f1.f64 * scale, 1.0, 170.0);

  // Zoom ratio for mouse sensitivity while aiming down sights (WaW style):
  // current FOV over the un-zoomed FOV.
  cod3::fov::g_last_view_fov = static_cast<float>(ctx.f1.f64);
  float cg_fov = cod3::guest::ReadF32(base, cod3::addr::kCgFovVmCvar + cod3::addr::kVmCvarValue);
  cod3::MouseLook::Get().SetViewFov(static_cast<float>(ctx.f1.f64), cg_fov * scale);
}

// Interaction end (interaction, restore_view): the game writes the FOV it
// saved at the start back into cgame's copy; the copy follows the dvar
// (the player's current value, or a level script's) instead.
REX_EXTERN(__imp__sub_824FDE90);
REX_HOOK_RAW(sub_824FDE90) {
  __imp__sub_824FDE90(ctx, base);
  cod3::fov::FollowDvarInCgame(base, "after an interaction ended");
}

// The level interface's setcvar (slots 0x258 string, 0x25C int, 0x260
// float; addresses.h level_cvar): a level script's write of cg_fov or
// cg_fovScale is the game's, not the player's.
namespace {
void LevelSetCvar(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*)) {
  const auto which = cod3::fov::WatchedDvar(base, ctx.r3.u32);
  original(ctx, base);
  if (which != cod3::fov::Watched::kNone) cod3::fov::ScriptSetDvar(ctx, base, which, ctx.r3.u32);
}
}  // namespace

REX_EXTERN(__imp__sub_824AB398);
REX_HOOK_RAW(sub_824AB398) { LevelSetCvar(ctx, base, __imp__sub_824AB398); }
REX_EXTERN(__imp__sub_824AB300);
REX_HOOK_RAW(sub_824AB300) { LevelSetCvar(ctx, base, __imp__sub_824AB300); }
REX_EXTERN(__imp__sub_824AB260);
REX_HOOK_RAW(sub_824AB260) { LevelSetCvar(ctx, base, __imp__sub_824AB260); }
