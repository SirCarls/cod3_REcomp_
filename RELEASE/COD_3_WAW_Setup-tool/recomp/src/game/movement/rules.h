// cod3 - the movement rules: CoD3 movement, sprint, and the CoD4 style with
// its three parts (docs/MOVEMENT.md explains each, with every value's source).
//
// This file is plain logic: no game address, no port setting, no guest
// memory. The hooks (hooks_sp.cpp, and cod3mp's own) read the game through
// the Program table (program.h) and ask here what to do. So both programs
// decide the same way, and the logic can be checked on its own
// (dev/agents/movement/tests/test_rules.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cod3::movement {

// --- numbers -------------------------------------------------------------------
// Call of Duty 4's public dvar defaults (CoD4 1.0 dvar lists; docs/MOVEMENT.md
// gives the URLs). No CoD4 code was used.
namespace cod4 {
inline constexpr float kSprintTime = 4.0f;               // player_sprintTime
inline constexpr float kSprintMinTime = 1.0f;            // player_sprintMinTime
inline constexpr int kSprintRechargePauseMs = 0;         // player_sprintRechargePause
// Not a public dvar: an estimate (empty to full in 4 s), to be measured in
// CoD4 with a stopwatch (watching the game is fine, its code is not).
inline constexpr float kSprintRechargeTime = 4.0f;
inline constexpr float kSprintSpeedScale = 1.5f;         // player_sprintSpeedScale
inline constexpr float kSprintStrafeSpeedScale = 0.667f; // player_sprintStrafeSpeedScale
inline constexpr int kSprintForwardMinimum = 105;        // player_sprintForwardMinimum (of 127)
inline constexpr float kStrafeSpeedScale = 0.8f;         // player_strafeSpeedScale
inline constexpr float kBackSpeedScale = 0.7f;           // player_backSpeedScale
inline constexpr float kBounceMinNormalZ = 0.3f;         // steeper than this: a wall (no bounce)
// CoD4 PC moves the player once per client frame, in whole milliseconds, and
// rounds the velocity to whole units after every step (Quake III's
// SnapVector; public descriptions, docs/MOVEMENT.md 4.2). That rounding is
// CoD4's air strafing and jump height: what both do depends on the step's
// length. The CoD4 style runs the movement in CoD4's 125 fps steps (8 ms)
// whatever the real frame rate (owner 2026-10-16: "Fixed 125 only").
inline constexpr int kMoveStepMsec = 8;
}  // namespace cod4

// Call of Duty 3 multiplayer's tuned stamina (codmp_xenonf.xex,
// PM_UpdateStamina 0x82495980), used for the campaign's sprint with CoD3
// movement. The campaign's own numbers were never tuned (1.5 s of sprint and
// a breath sound every frame below half stamina).
namespace cod3mp {
inline constexpr float kDrainPerSecond = 0.108525f;   // 9.2 s of sprint
inline constexpr float kRecoverPerSecond = 0.165424f; // 6 s from empty to full
inline constexpr int kRechargePauseMs = 1000;
}  // namespace cod3mp

inline constexpr float kSprintStartMinimum = 0.25f;  // the game's own test (PM_UpdateSprint)
// The gun's offset while sprinting (sprintMove forward, right, up, in game
// units, at full speed) for a weapon that has none: pulled back, right and
// down, a lowered carry. Port values (no game data has them); visual only.
inline constexpr float kSprintPoseFallback[3] = {-1.0f, 1.0f, -1.5f};
inline constexpr float kWalkableNormalZ = 0.7f;      // the game's MIN_WALK_NORMAL
inline constexpr float kJumpHeight = 39.0f;          // the game's jump step stops 39 above the jump's start

// --- the rules for one player ------------------------------------------------
// The three parts of the CoD4 style.
struct Parts {
  bool sprint = true;  // Sprint like CoD4: 4 s, aim/fire ends it, only forward
  bool speeds = true;  // Speeds and feel: strafe 0.8, back 0.7, sprint 1.5, 125 fps steps
  bool jumps = true;   // PC jump tricks: bounces off slopes (the air strafing is in `speeds`)
  bool operator==(const Parts&) const = default;
};

// One player's own choice (the settings cod3_cod4_movement and cod3_cod4_*).
struct Preference {
  bool cod4 = false;
  Parts parts;
  bool operator==(const Preference&) const = default;
};

// alternate_movement (a server_sided_protected dvar, game/protected_dvars.h).
enum class MatchMode : int {
  kCod3 = 0,           // everyone moves as on the console
  kCod4 = 1,           // everyone moves CoD4 style, with the host's parts
  kPlayersChoice = 2,  // each player's own Preference
};

// What one player's movement follows now.
struct MoveRules {
  bool sprint = false;  // the player can sprint (campaign: cod3_sprint)
  bool cod4 = false;    // CoD4 style
  Parts parts;          // used only when cod4
  bool Cod4Sprint() const { return cod4 && parts.sprint; }
  bool Cod4Speeds() const { return cod4 && parts.speeds; }
  // CoD4 PC's movement steps: 8 ms steps, the velocity rounded after each
  // (part of Speeds and feel: it is CoD4's air strafing and jump height).
  bool Cod4Steps() const { return cod4 && parts.speeds; }
  bool Cod4Jumps() const { return cod4 && parts.jumps; }
  // Nothing differs from the console's movement: the hooks call the game's
  // own code unchanged.
  bool AllOriginal() const { return !sprint && !cod4; }
  bool operator==(const MoveRules&) const = default;
};

// The rules for a player. `mode` is alternate_movement's effective value
// (any other number counts as 0); `host_parts` are alternate_movement_*.
// (Matches are PC only, owner 2026-10-16: no Xbox 360 player rule.)
MoveRules EffectiveRules(int mode, const Parts& host_parts, const Preference& player, bool sprint_available);

// A short description ("Call of Duty 4 (sprint, speeds, jumps)") for logs and
// announcements.
std::string Describe(const MoveRules& rules);
const char* DescribeMode(int mode);

// --- stamina -------------------------------------------------------------------
struct StaminaRule {
  float drain_per_second;
  float recover_per_second;
  int recharge_pause_ms;
  bool recover_while_held;  // CoD3: holding sprint stops the recovery
};
StaminaRule StaminaFor(const MoveRules& rules);

struct StaminaState {
  float stamina = 1.0f;       // 0..1
  int last_sprint_time = 0;   // serverTime of the last sprinting frame
  bool sprinting = false;     // in: the sprint flag this frame
  bool out_of_breath = false; // in/out: pm_flags kOutOfBreath
};
struct StaminaInput {
  int server_time = 0;
  int msec = 0;               // this step's length
  bool sprint_held = false;   // the usercmd's sprint button
  bool noclip = false;
  bool no_drain = false;      // the game's never-set "unlimited" switch
};
struct StaminaResult {
  bool breath_event = false;        // add the game's "out of breath" event once
  bool call_breath_events = false;  // call the game's breathing (PM_SprintBreathEvents)
};
// One step of the game's PM_UpdateStamina, with `rule`'s numbers: the same
// structure as the game's, but the breath event comes once, when the stamina
// runs out (the campaign's version adds it every frame below half).
StaminaResult StepStamina(const StaminaRule& rule, const StaminaInput& in, StaminaState& state);

// --- sprint conditions -----------------------------------------------------------
// The port's extra conditions (on top of the game's own, PM_UpdateSprint):
// sprinting stops while aiming or firing and needs the stick (or key) forward:
// any forward with CoD3 rules, at least 105 of 127 with CoD4 sprint.
bool SprintAllowed(const MoveRules& rules, int forwardmove, bool aiming, bool firing);
// The sideways input while sprinting with CoD4 sprint (x0.667).
int SprintRightmove(const MoveRules& rules, int rightmove);

// CoD4 sprint: once the stamina runs out, the sprint button has to be let go
// before the next sprint, so holding it doesn't start a short sprint each
// time a quarter of the stamina has come back (the cloud run of 2026-10-16
// showed that stutter). CoD3's rule needs nothing: its stamina doesn't come
// back while the button is held. One per player, on the server.
class SprintRestart {
 public:
  // Before the sprint test, once per movement step: false while the button
  // must be let go first. Letting go clears it.
  bool Allowed(const MoveRules& rules, bool button_held);
  // After the stamina step, when the stamina ran out in it.
  void RanOut(bool button_held);
  bool up_required() const { return up_required_; }

 private:
  bool up_required_ = false;
};

// --- the mode-2 preference on the wire (userinfo key "cod3mv") --------------------
// "3" = CoD3, "4" + four 0/1 digits (sprint, speeds, unused, jumps) = CoD4. The
// third digit belonged to a part that was taken out (2026-10-20): it is sent
// as 0 and ignored, so ports before and after read each other's preference.
std::string EncodePreference(const Preference& pref);
// Strict: exactly those forms; anything else is refused (false).
bool DecodePreference(std::string_view text, Preference* out);

// The server's table of mode-2 preferences, one per client (cod3mp fills it
// from each client's userinfo; the campaign has one player and reads its
// settings directly). A change is taken at most once per kPreferenceGapMs per
// player; nothing but the strict forms above is taken, and nothing from it
// ever becomes a dvar.
inline constexpr int kPreferenceGapMs = 5000;
class PlayerPreferences {
 public:
  static constexpr int kMaxClients = 64;
  enum class Result { kApplied, kUnchanged, kTooSoon, kInvalid };
  // `text` is the userinfo value ("" = an older port without the key:
  // Call of Duty 3). `now_ms` is the server's time.
  Result Update(int client, std::string_view text, int now_ms);
  Preference Get(int client) const;
  void Clear(int client);  // the client left

 private:
  struct Slot {
    Preference pref;
    int last_change = -1000000;
  };
  Slot slots_[kMaxClients];
};

// --- Q3 info strings ("\key\value\key\value") ---------------------------------------
// Calls `fn(key, value)` for each pair; false for a malformed string (then
// nothing should be taken from it). Bounded: at most 64 pairs.
template <typename Fn>
bool ForEachInfoPair(std::string_view info, Fn&& fn);

// --- CoD4 PC's movement steps (Speeds and feel) ------------------------------------------
// The velocity after a step, as CoD4 PC keeps it: each component rounded to
// the nearest whole unit, halves to even (the x87/SSE default; Quake III's
// SnapVector). Exact for any float, whatever the thread's rounding mode.
float SnapToUnit(float v);

// Which fixed steps a usercmd runs. The player's time not yet moved
// (`pending_ms`, under one step) and the usercmd's own time make whole steps
// of `step_ms`; what is left waits for the next usercmd. At most
// `max_span_ms` is moved at once (the game's own Pmove limit, a multiple of
// the step). The steps start at `start_time`; the playerState's commandTime
// is the usercmd's time again afterwards (the game's other code counts time
// exactly as before).
struct StepPlan {
  int start_time = 0;
  int steps = 0;
  int pending_after = 0;
};
StepPlan PlanSteps(int command_time, int final_time, int pending_ms, int step_ms, int max_span_ms);

// --- bounces (PC jump tricks) --------------------------------------------------------
// A steep slope the step move projects onto instead of clipping (CoD4): from
// 0.3 (steeper is a wall) up to the walkable 0.7.
bool IsBounceSlope(float normal_z);
// `in` projected onto the plane through the origin with `normal`, keeping its
// speed. False when nothing is left along the plane (then clip as the game).
bool ProjectKeepSpeed(const float in[3], const float normal[3], float out[3]);
// When the step move's clip on such a slope becomes a bounce: a jump falling
// onto it in the air move, below the jump's top (the game's jump step, which
// needs the jump flag). Never on the walk move's step down: a steep bit of an
// uneven floor would lift a player walking or strafing up a slight incline
// (owner 2026-10-16: "randomly raise a couple feet in the air").
struct BounceCase {
  bool air_move = false;       // inside the air move (not the walk or ladder move)
  bool jumping = false;        // the playerState's jump flag
  float origin_z = 0.0f;       // where the player is
  float jump_origin_z = 0.0f;  // where the jump started
  float velocity_z = 0.0f;     // the velocity going into the clip
  float normal_z = 0.0f;       // the slope's normal
};
bool BounceApplies(const BounceCase& c);

// ------------------------------------------------------------------------------------
template <typename Fn>
bool ForEachInfoPair(std::string_view info, Fn&& fn) {
  if (info.empty()) return true;
  if (info.front() != '\\' || info.size() > 1024) return false;
  size_t at = 1;
  for (int pairs = 0; at < info.size(); ++pairs) {
    if (pairs >= 64) return false;
    const size_t key_end = info.find('\\', at);
    if (key_end == std::string_view::npos) return false;
    const size_t value_end = info.find('\\', key_end + 1);
    const std::string_view key = info.substr(at, key_end - at);
    const std::string_view value =
        info.substr(key_end + 1, (value_end == std::string_view::npos ? info.size() : value_end) - key_end - 1);
    if (key.empty()) return false;
    fn(key, value);
    if (value_end == std::string_view::npos) break;
    at = value_end + 1;
  }
  return true;
}

}  // namespace cod3::movement
