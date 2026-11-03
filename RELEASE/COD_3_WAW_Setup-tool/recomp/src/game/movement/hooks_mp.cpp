// cod3mp - [claude-9b-think] 2026-11-02 UNTESTED The movement rules in multiplayer, stage 1: settings, the match
// decision and logs only (c9b-think/owner-1102/mp-movement-design.md). Nothing the game does changes here: the one
// hook calls the game's own Pmove every time, and every match moves as Call of Duty 3.
//
// What it is for: the design's open questions before any movement changes in a match.
//  - Who moves whom: the log names every playerState the game's Pmove (0x824B4270, program_mp.h) moves on this
//    console, with how many calls and how much time. Two consoles (or two cod3mp processes over system link) show
//    whether each moves only its own players (docs/MP_PROTOCOL.md section 1) or the host moves everyone
//    (game/mp/server_step_mp.cpp's note).
//  - The addresses: what Pmove's pm + 312 / + 316 (pmove_fixed / pmove_msec) hold, and how often pm_flags 0x100000
//    sends a call to 0x824AA438.
//  - The settings and the decision: the host's rule (cod3mp_movement_rule) and each split-screen player's own choice
//    (cod3mp_movement, _2 to _4), parsed strictly (rules.h); a malformed value is logged and counts as Call of Duty 3.
//
// Nothing here reads anything another console sent, and nothing is sent.

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/movement/program_mp.h"
#include "game/movement/rules.h"

REXCVAR_DEFINE_STRING(cod3mp_movement_rule, "3", "cod3/Movement",
                      "Multiplayer host: the match's movement rule. 3 = Call of Duty 3 for everyone; 4abcd = Call of "
                      "Duty 4 for everyone with parts a (sprint), b (speeds), c (0, unused), d (jump tricks), as "
                      "0 or 1; own = each player's own (cod3mp_movement). Stage 1: decided and logged only, every "
                      "match moves as Call of Duty 3")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3mp_movement, "3", "cod3/Movement",
                      "Multiplayer: your movement style where the host's rule is own (the first player): 3 = Call of "
                      "Duty 3, 4abcd = Call of Duty 4 with parts a (sprint), b (speeds), c (0), d (jump tricks). "
                      "Stage 1: logged only")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3mp_movement_2, "3", "cod3/Movement", "As cod3mp_movement, for split-screen player 2")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3mp_movement_3, "3", "cod3/Movement", "As cod3mp_movement, for split-screen player 3")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3mp_movement_4, "3", "cod3/Movement", "As cod3mp_movement, for split-screen player 4")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3mp_debug_movement, false, "cod3",
                    "Multiplayer: log every 10 s what the game's Pmove moved on this console (each playerState: "
                    "calls, time, pmove_fixed / pmove_msec, pm_flags 0x100000)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [claude-9b-think] 2026-11-02 UNTESTED stage 2's first part, off until the owner has tried it.
REXCVAR_DEFINE_BOOL(cod3mp_movement_stage2, false, "cod3/Movement",
                    "Multiplayer, tests: stage 2's hooks: which local player each Pmove call moves (from the game's "
                    "ClientThink) and the four rule points (sprint, stamina, jump, air move), counted per player in "
                    "cod3mp_debug_movement's log. Moves nobody differently yet")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace mv = cod3::movement;
namespace g = cod3::guest;
constexpr const mv::Program& P = mv::kMultiplayer;
using Clock = std::chrono::steady_clock;

// Stage 1 never moves anyone differently: the decision is made with the hooks "not ready", so it always says Call of
// Duty 3 and why. Stage 2 sets this once the movement hooks are in and their addresses checked.
constexpr bool kHooksReady = false;
constexpr uint32_t kSpecialMoveFlag = 0x100000;  // pm_flags: Pmove hands the call to 0x824AA438

bool ValidGuest(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

// --- the settings and the decision (logged when they change) ----------------------------
std::string g_last_settings;

void LogSettings() {
  static Clock::time_point next{};  // looked at once a second, not on every Pmove
  const auto now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(1);
  const std::string rule_text = REXCVAR_GET(cod3mp_movement_rule);
  const std::string own[4] = {REXCVAR_GET(cod3mp_movement), REXCVAR_GET(cod3mp_movement_2),
                              REXCVAR_GET(cod3mp_movement_3), REXCVAR_GET(cod3mp_movement_4)};
  std::string key = rule_text;
  for (const std::string& s : own) key += '|' + s;
  if (key == g_last_settings) return;
  g_last_settings = key;

  mv::MatchFacts facts;
  facts.hooks_ready = kHooksReady;
  int mode = 0;
  mv::Parts parts;
  const bool rule_ok = mv::DecodeMatchRule(rule_text, &mode, &parts);
  facts.host_mode = rule_ok ? mode : 0;
  facts.host_parts = rule_ok ? parts : mv::Parts{};
  const mv::MatchDecision d = mv::DecideMatch(facts);
  // Values are logged only as the parse saw them (at most 8 characters; the cvar is the player's own text).
  REXLOG_INFO("cod3mp: movement: host rule {} ({}); decision: {}", rule_ok ? mv::DescribeMode(mode) : "malformed",
              rule_ok ? "as set" : "Call of Duty 3", d.reason);
  for (int i = 0; i < 4; ++i) {
    const mv::LocalChoice c = mv::ParseLocalChoice(own[i]);
    mv::MoveRules r = mv::EffectiveRules(d.mode, d.host_parts, c.pref, /*sprint_available=*/true);
    REXLOG_INFO("cod3mp: movement: local player {}: own choice {}{}; moves as {}", i + 1,
                c.pref.cod4 ? "Call of Duty 4" : "Call of Duty 3", c.valid ? "" : " (malformed setting, ignored)",
                mv::Describe(r));
  }
}

// --- stage 2: which local player a Pmove call moves (cod3mp_movement_stage2) --------------
// [claude-9b-think] 2026-11-02 UNTESTED Every Pmove call of a player runs inside the game's ClientThink_real
// (0x82592CD0, r3 = the entity), directly or through 0x82592B98 (program_mp.h mp_fn). The local players' entities
// are the table cod3_test_autojoin reads (0x82BCC7FC: + 4 * (controller + 1)), so the controller is the slot that
// holds this entity; any other entity (a remote player, if this console ever moved one) is -1 and keeps Call of
// Duty 3. Set around the game's own call, on the game's thread; nothing else reads it.
thread_local int t_controller = -1;
thread_local bool t_in_think = false;

int ControllerOf(uint8_t* base, uint32_t ent) {
  const uint32_t table = g::ReadU32(base, mv::mp_fn::kLocalEntities);
  if (!ValidGuest(table) || !ValidGuest(ent)) return -1;
  for (int c = 0; c < 4; ++c) {
    if (g::ReadU32(base, table + 4 * static_cast<uint32_t>(c + 1)) == ent) return c;
  }
  return -1;
}

// Rule points reached per controller (index controller + 1: 0 = not a local player / outside ClientThink).
enum RulePoint { kSprint, kStamina, kJump, kAirMove, kRulePoints };
uint64_t g_rule_calls[5][kRulePoints] = {};
uint64_t g_think_calls[5] = {};

void CountRule(RulePoint point) {
  if (!REXCVAR_GET(cod3mp_movement_stage2)) return;
  ++g_rule_calls[t_controller + 1][point];
}

// --- what the game's Pmove moves (cod3mp_debug_movement) --------------------------------
struct Seen {
  uint32_t ps = 0;
  uint64_t calls = 0, special = 0, refused = 0;
  int64_t msec = 0;
  uint32_t pmove_fixed = 0, pmove_msec = 0;
  int controller = -2;   // [claude-9b-think] stage 2: the local player (-1 none, -2 not known: stage 2 off)
  bool mixed = false;    // more than one controller seen for this playerState (the mapping is wrong)
  float ads = 0.0f;      // ps + 172 (binoculars_frac, program_mp.h): 0 to 1 while aiming, if the offset is right
};
std::array<Seen, 8> g_seen;
int g_seen_count = 0;
uint64_t g_seen_overflow = 0;
Clock::time_point g_since = Clock::now();

void Note(uint8_t* base, uint32_t pm) {
  const uint32_t ps = g::ReadU32(base, pm + P.pmove.ps);
  if (!ValidGuest(ps)) return;
  Seen* s = nullptr;
  for (int i = 0; i < g_seen_count; ++i) {
    if (g_seen[i].ps == ps) s = &g_seen[i];
  }
  if (!s) {
    if (g_seen_count >= static_cast<int>(g_seen.size())) {
      ++g_seen_overflow;
      return;
    }
    s = &g_seen[g_seen_count++];
    s->ps = ps;
  }
  ++s->calls;
  const int32_t to = g::ReadS32(base, pm + P.pmove.cmd + P.cmd.server_time);
  const int32_t from = g::ReadS32(base, ps + P.ps.command_time);
  const int32_t d = to - from;
  if (d < 0) ++s->refused;
  else if (d <= 1000) s->msec += d;
  if (g::ReadU32(base, ps + P.ps.pm_flags) & kSpecialMoveFlag) ++s->special;
  s->pmove_fixed = g::ReadU32(base, pm + P.pmove.pmove_fixed);
  s->pmove_msec = g::ReadU32(base, pm + P.pmove.pmove_msec);
  if (REXCVAR_GET(cod3mp_movement_stage2)) {
    const int c = t_in_think ? t_controller : -1;
    if (s->controller != -2 && s->controller != c) s->mixed = true;
    s->controller = c;
    s->ads = g::ReadF32(base, ps + P.ps.binoculars_frac);
  }
}

void MaybeLogSeen() {
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - g_since).count();
  if (seconds < 10.0) return;
  g_since = now;
  for (int i = 0; i < g_seen_count; ++i) {
    const Seen& s = g_seen[i];
    REXLOG_INFO("cod3mp: movement: Pmove moved playerState {:#x}: {} calls ({:.0f} a second), {} ms ({:.2f} of real "
                "time), {} older usercmds refused, pm_flags 0x100000 in {}; pm + 312 / + 316 (pmove_fixed / "
                "pmove_msec) {} / {}",
                s.ps, s.calls, s.calls / seconds, s.msec, s.msec / 1000.0 / seconds, s.refused, s.special,
                s.pmove_fixed, s.pmove_msec);
    if (s.controller != -2) {
      REXLOG_INFO("cod3mp: movement: stage 2: playerState {:#x} is local player {}{}; ps + 172 (aim fraction?) {:.3f}",
                  s.ps, s.controller >= 0 ? std::to_string(s.controller + 1) : std::string("none (not local)"),
                  s.mixed ? " (MIXED: more than one player seen; the mapping is wrong)" : "", s.ads);
    }
  }
  if (g_seen_count == 0) REXLOG_INFO("cod3mp: movement: Pmove moved nobody on this console in the last 10 s");
  if (g_seen_overflow) REXLOG_INFO("cod3mp: movement: and {} calls for more than 8 playerStates", g_seen_overflow);
  if (REXCVAR_GET(cod3mp_movement_stage2)) {
    for (int i = 0; i < 5; ++i) {
      const uint64_t* r = g_rule_calls[i];
      if (!g_think_calls[i] && !r[kSprint] && !r[kStamina] && !r[kJump] && !r[kAirMove]) continue;
      REXLOG_INFO("cod3mp: movement: stage 2: {}: ClientThink {}, sprint {}, stamina {}, jump checks {}, air moves {}",
                  i == 0 ? std::string("no local player") : "local player " + std::to_string(i), g_think_calls[i],
                  r[kSprint], r[kStamina], r[kJump], r[kAirMove]);
    }
  }
  g_seen = {};
  g_seen_count = 0;
  g_seen_overflow = 0;
  for (auto& row : g_rule_calls) {
    for (auto& n : row) n = 0;
  }
  for (auto& n : g_think_calls) n = 0;
}

}  // namespace

// Pmove(pm) 0x824B4270 (program_mp.h): the game's own call, always; before it, the settings' log and (with
// cod3mp_debug_movement) what it moves.
REX_EXTERN(__imp__sub_824B4270);
REX_HOOK_RAW(sub_824B4270) {
  LogSettings();
  const uint32_t pm = ctx.r3.u32;
  if (REXCVAR_GET(cod3mp_debug_movement) && ValidGuest(pm)) {
    Note(base, pm);
    MaybeLogSeen();
  }
  __imp__sub_824B4270(ctx, base);
}

// [claude-9b-think] 2026-11-02 UNTESTED stage 2 (cod3mp_movement_stage2, off by default). Each hook calls the game's
// own function every time; with stage 2 on it only notes the local player (ClientThink_real) or counts the rule
// point for that player. The rules themselves (the campaign's hooks_sp.cpp bodies on the program_mp.h fields) come
// once T1 / T3 logs confirm this mapping (no "MIXED", players 1-4 each on their own playerState) and ps + 172.
// They then apply only to a controller >= 0, only when the match decision allows (rules.h DecideMatch with
// hooks_ready), and never in a match with remote players until stage 3 (mp-movement-design.md).
REX_EXTERN(__imp__sub_82592CD0);
REX_HOOK_RAW(sub_82592CD0) {  // ClientThink_real(ent)
  if (!REXCVAR_GET(cod3mp_movement_stage2) || t_in_think) {
    __imp__sub_82592CD0(ctx, base);
    return;
  }
  const uint32_t ent = ctx.r3.u32;
  const int controller = ControllerOf(base, ent);
  ++g_think_calls[controller + 1];
  static int s_described = 0;  // the first 8 calls: what the entity and the table hold, to check the mapping
  if (s_described < 8 && ValidGuest(ent)) {
    ++s_described;
    const uint32_t table = g::ReadU32(base, mv::mp_fn::kLocalEntities);
    REXLOG_INFO("cod3mp: movement: stage 2: ClientThink entity {:#x} (+512 {:#x}, +0x208 {:#x}) -> local player {}; "
                "table {:#x}: {:#x} {:#x} {:#x} {:#x}",
                ent, g::ReadU32(base, ent + 512), g::ReadU32(base, ent + 0x208), controller + 1, table,
                ValidGuest(table) ? g::ReadU32(base, table + 4) : 0, ValidGuest(table) ? g::ReadU32(base, table + 8) : 0,
                ValidGuest(table) ? g::ReadU32(base, table + 12) : 0,
                ValidGuest(table) ? g::ReadU32(base, table + 16) : 0);
  }
  t_controller = controller;
  t_in_think = true;
  __imp__sub_82592CD0(ctx, base);
  t_in_think = false;
  t_controller = -1;
}

REX_EXTERN(__imp__sub_8248E970);
REX_HOOK_RAW(sub_8248E970) {  // PM_UpdateSprint
  CountRule(kSprint);
  __imp__sub_8248E970(ctx, base);
}

REX_EXTERN(__imp__sub_82495980);
REX_HOOK_RAW(sub_82495980) {  // PM_UpdateStamina
  CountRule(kStamina);
  __imp__sub_82495980(ctx, base);
}

REX_EXTERN(__imp__sub_82495B58);
REX_HOOK_RAW(sub_82495B58) {  // PM_CheckJump
  CountRule(kJump);
  __imp__sub_82495B58(ctx, base);
}

REX_EXTERN(__imp__sub_824A9798);
REX_HOOK_RAW(sub_824A9798) {  // PM_AirMove
  CountRule(kAirMove);
  __imp__sub_824A9798(ctx, base);
}
