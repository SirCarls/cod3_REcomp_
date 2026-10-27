// cod3 - the movement rules (see rules.h).

#include "game/movement/rules.h"

#include <algorithm>
#include <cmath>

// The multiplayer table compiles here so it stays in step with program.h,
// although this program never uses it (program_mp.h).
#include "game/movement/program_mp.h"

namespace cod3::movement {

static_assert(kMultiplayer.ps.stamina == kMultiplayer.ps.scale_ads + 44, "MP stamina follows the scales");

MoveRules EffectiveRules(int mode, const Parts& host_parts, const Preference& player, bool sprint_available) {
  MoveRules r;
  r.sprint = sprint_available;
  switch (mode) {
    case static_cast<int>(MatchMode::kCod4):
      r.cod4 = true;
      r.parts = host_parts;
      break;
    case static_cast<int>(MatchMode::kPlayersChoice):
      r.cod4 = player.cod4;
      r.parts = player.parts;
      break;
    default:  // 0, or anything else
      break;
  }
  if (!r.cod4) r.parts = Parts{};  // unused; kept equal so comparisons stay simple
  return r;
}

const char* DescribeMode(int mode) {
  switch (mode) {
    case 0: return "Call of Duty 3 movement for everyone";
    case 1: return "Call of Duty 4 movement for everyone";
    case 2: return "each player's own movement style";
  }
  return "Call of Duty 3 movement for everyone";
}

std::string Describe(const MoveRules& rules) {
  std::string out = rules.cod4 ? "Call of Duty 4" : "Call of Duty 3";
  if (rules.cod4) {
    std::string parts;
    auto add = [&](bool on, const char* name) {
      if (!on) return;
      if (!parts.empty()) parts += ", ";
      parts += name;
    };
    add(rules.parts.sprint, "sprint");
    add(rules.parts.speeds, "speeds");
    add(rules.parts.jumps, "jump tricks");
    out += " (" + (parts.empty() ? std::string("no parts") : parts) + ")";
  }
  out += rules.sprint ? ", sprint on" : ", no sprint";
  return out;
}

StaminaRule StaminaFor(const MoveRules& rules) {
  if (rules.Cod4Sprint()) {
    return {1.0f / cod4::kSprintTime, 1.0f / cod4::kSprintRechargeTime, cod4::kSprintRechargePauseMs,
            /*recover_while_held=*/true};
  }
  return {cod3mp::kDrainPerSecond, cod3mp::kRecoverPerSecond, cod3mp::kRechargePauseMs,
          /*recover_while_held=*/false};
}

StaminaResult StepStamina(const StaminaRule& rule, const StaminaInput& in, StaminaState& s) {
  StaminaResult r;
  const float seconds = static_cast<float>(std::max(in.msec, 0)) * 0.001f;
  if (s.sprinting) {
    // As the game: no drain in noclip or with its "unlimited" switch, and no
    // breathing either.
    if (in.noclip || in.no_drain) return r;
    s.last_sprint_time = in.server_time;
    s.stamina -= seconds * rule.drain_per_second;
    if (s.stamina <= 0.0f) {
      s.stamina = 0.0f;
      if (!s.out_of_breath) r.breath_event = true;
      s.out_of_breath = true;
    }
    return r;
  }
  if (in.sprint_held && !rule.recover_while_held) {
    r.call_breath_events = true;  // the game keeps breathing, without recovering
    return r;
  }
  if (in.server_time < s.last_sprint_time + rule.recharge_pause_ms) return r;
  s.stamina += seconds * rule.recover_per_second;
  if (s.stamina >= 1.0f) {
    s.stamina = 1.0f;
    s.out_of_breath = false;
  }
  r.call_breath_events = true;
  return r;
}

bool SprintAllowed(const MoveRules& rules, int forwardmove, bool aiming, bool firing) {
  if (!rules.sprint || aiming || firing) return false;
  return rules.Cod4Sprint() ? forwardmove >= cod4::kSprintForwardMinimum : forwardmove > 0;
}

int SprintRightmove(const MoveRules& rules, int rightmove) {
  if (!rules.Cod4Sprint()) return rightmove;
  return static_cast<int>(std::lround(static_cast<float>(rightmove) * cod4::kSprintStrafeSpeedScale));
}

bool SprintRestart::Allowed(const MoveRules& rules, bool button_held) {
  if (!button_held) up_required_ = false;
  return !(up_required_ && rules.Cod4Sprint());
}

void SprintRestart::RanOut(bool button_held) {
  if (button_held) up_required_ = true;
}

std::string EncodePreference(const Preference& pref) {
  if (!pref.cod4) return "3";
  std::string s = "4";
  for (bool on : {pref.parts.sprint, pref.parts.speeds, false, pref.parts.jumps}) s += on ? '1' : '0';  // third: unused
  return s;
}

bool DecodePreference(std::string_view text, Preference* out) {
  Preference p;
  if (text == "3") {
    *out = p;
    return true;
  }
  if (text.size() != 5 || text[0] != '4') return false;
  bool bits[4];
  for (int i = 0; i < 4; ++i) {
    if (text[i + 1] != '0' && text[i + 1] != '1') return false;
    bits[i] = text[i + 1] == '1';
  }
  p.cod4 = true;
  p.parts = {bits[0], bits[1], bits[3]};  // bits[2]: unused (rules.h)
  *out = p;
  return true;
}

PlayerPreferences::Result PlayerPreferences::Update(int client, std::string_view text, int now_ms) {
  if (client < 0 || client >= kMaxClients) return Result::kInvalid;
  Preference p;
  if (!text.empty() && !DecodePreference(text, &p)) return Result::kInvalid;  // stays as it was
  Slot& slot = slots_[client];
  if (p == slot.pref) return Result::kUnchanged;
  if (now_ms - slot.last_change < kPreferenceGapMs && now_ms >= slot.last_change) return Result::kTooSoon;
  slot.pref = p;
  slot.last_change = now_ms;
  return Result::kApplied;
}

Preference PlayerPreferences::Get(int client) const {
  if (client < 0 || client >= kMaxClients) return Preference{};
  return slots_[client].pref;
}

void PlayerPreferences::Clear(int client) {
  if (client >= 0 && client < kMaxClients) slots_[client] = Slot{};
}

bool IsBounceSlope(float normal_z) {
  return normal_z >= cod4::kBounceMinNormalZ && normal_z < kWalkableNormalZ;
}

bool BounceApplies(const BounceCase& c) {
  return c.air_move && c.jumping && c.velocity_z < 0.0f && c.origin_z < c.jump_origin_z + kJumpHeight &&
         IsBounceSlope(c.normal_z);
}

bool ProjectKeepSpeed(const float in[3], const float n[3], float out[3]) {
  const float speed = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
  const float into = in[0] * n[0] + in[1] * n[1] + in[2] * n[2];
  float along[3] = {in[0] - n[0] * into, in[1] - n[1] * into, in[2] - n[2] * into};
  const float len = std::sqrt(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
  // Nothing (or next to nothing) along the plane: moving straight into it.
  if (!std::isfinite(speed) || !(len > 1e-3f) || len < speed * 0.01f) return false;
  for (int i = 0; i < 3; ++i) out[i] = along[i] / len * speed;
  return true;
}

// --- CoD4 PC's movement steps ------------------------------------------------------------
float SnapToUnit(float v) {
  if (!std::isfinite(v)) return v;
  const float low = std::floor(v);
  const float rest = v - low;  // exact: v and floor(v) share the exponent range
  if (rest > 0.5f) return low + 1.0f;
  if (rest < 0.5f) return low;
  return std::fmod(low, 2.0f) == 0.0f ? low : low + 1.0f;  // a half: to even
}

StepPlan PlanSteps(int command_time, int final_time, int pending_ms, int step_ms, int max_span_ms) {
  StepPlan plan;
  if (step_ms < 1) step_ms = 1;
  if (pending_ms < 0 || pending_ms >= step_ms) pending_ms = 0;
  plan.start_time = command_time - pending_ms;
  if (final_time < command_time) {
    // Not a usercmd the game moves (it refuses an older one): nothing to do.
    plan.start_time = command_time;
    plan.pending_after = pending_ms;
    return plan;
  }
  const int max_span = std::max(step_ms, max_span_ms - max_span_ms % step_ms);
  if (final_time - plan.start_time > max_span) plan.start_time = final_time - max_span;
  const int span = final_time - plan.start_time;
  plan.steps = span / step_ms;
  plan.pending_after = span - plan.steps * step_ms;
  return plan;
}

// [acct2-main] Step ends: e(k) = floor(k * 1000 / hz). The last one at or before t is the largest k
// with floor(k * 1000 / hz) <= t, that is k * 1000 / hz < t + 1: k = floor(((t + 1) * hz - 1) / 1000).
int MoveStepEndAtOrBefore(int t, int hz) {
  if (hz < 1 || t < 0) return t;
  const int64_t k = ((static_cast<int64_t>(t) + 1) * hz - 1) / 1000;
  return static_cast<int>(k * 1000 / hz);
}

int MoveStepEndAfter(int t, int hz) {
  if (hz < 1 || t < 0) return t + 1;
  const int64_t k = ((static_cast<int64_t>(t) + 1) * hz - 1) / 1000 + 1;
  return static_cast<int>(k * 1000 / hz);
}

}  // namespace cod3::movement
