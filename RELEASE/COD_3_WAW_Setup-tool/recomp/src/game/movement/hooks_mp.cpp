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

#include <algorithm>  // [acct2-main] the camera check
#include <array>
#include <atomic>  // [rexglue-native thread2] the fixed steps
#include <chrono>
#include <cmath>  // [acct2-main] the camera check
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"  // [acct2-main] addr::keybinds::kLocalClients (the local clients in use)
#include "game/guest.h"
#include "game/movement/program_mp.h"
#include "game/movement/rules.h"
#include "game/program.h"  // [rexglue-native-mp] 2026-11-11 COD3_ADDR (tools/mp_aliases.py --check)

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
// [rexglue-native thread2] 2026-11-24 rule 14 (FPS-RULE14.md M4): the players' moves in fixed steps.
REXCVAR_DEFINE_INT32(cod3mp_move_hz, 60, "cod3/Movement",
                     "Multiplayer: every player's movement in steps of 1/N s that end at fixed times of the usercmds' "
                     "clock, as the campaign's cod3_move_hz, so a move is the same at any frame rate (the console's "
                     "steps at 60 fps); 0: one step per usercmd, as the game (a usercmd is a drawn frame: 50 ms steps "
                     "at 20 fps, 7 ms at 144)")
    .range(0, 250)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3mp_debug_move_steps, false, "cod3",
                    "Log every 2 s each player's movement steps (cod3mp_move_hz; with 0 the game's one a usercmd): "
                    "usercmds, steps, usercmds without a step, boxes kept, unsticks left out, the speed across the "
                    "ground and the lowest and highest height");
REXCVAR_DEFINE_BOOL(cod3mp_movement_stage2, false, "cod3/Movement",
                    "Multiplayer, tests: stage 2's hooks: which local player each Pmove call moves (from the game's "
                    "ClientThink) and the four rule points (sprint, stamina, jump, air move), counted per player in "
                    "cod3mp_debug_movement's log. Moves nobody differently yet")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#if defined(COD3_MP)  // cod3mp's own name (tools/mp_aliases.py), as the hooks' block below
REX_EXTERN(__imp__sub_824B4270);  // Pmove (hooked below; the fixed steps call the game's own)
#endif

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

int SlotOf(uint8_t* base, uint32_t ent) {  // [acct2-main] was ControllerOf: the table's slot (0-3) or -1
  const uint32_t table = g::ReadU32(base, mv::mp_fn::kLocalEntities);
  if (!ValidGuest(table) || !ValidGuest(ent)) return -1;
  for (int c = 0; c < 4; ++c) {
    if (g::ReadU32(base, table + 4 * static_cast<uint32_t>(c + 1)) == ent) return c;
  }
  return -1;
}

// [acct2-main] 2026-11-05 The table holds this console's client slots, not only its local players. In a system
// link match (T2: runs acct2-main-t2-A / -B) the other console's player has a slot here too: its one ClientThink
// when it spawned was labeled "local player 2" on both consoles, each with one player. So a slot is a local
// player only while that local client is in use, the test Com_Frame (0x82557D60) makes before each CL_Frame:
// *(kLocalClients + 184 + 3208 * client) != 0. Split screen fills the clients in order (0 .. n-1); if the ones
// in use aren't in order, no slot counts as local (logged once), because then slot and client may not match.
uint32_t LocalClientsInUse(uint8_t* base) {  // bit c: local client c is in use
  namespace kb = cod3::addr::keybinds;
  uint32_t mask = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    if (g::ReadU32(base, kb::kLocalClients + kb::kLocalClientState + c * kb::kLocalClientStride) != 0) {
      mask |= 1u << c;
    }
  }
  return mask;
}

bool InOrder(uint32_t mask) { return (mask & (mask + 1)) == 0; }  // 0, 1, 3, 7 or 15

int ControllerOf(int slot, uint32_t mask) {
  if (slot < 0 || !InOrder(mask)) return -1;
  return ((mask >> slot) & 1u) ? slot : -1;
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

// --- stage 2 step 2: the camera beside the playerState (cod3mp_movement_stage2) ----------
// [acct2-main] 2026-11-05 Observe only (dev/agents/acct2-main/MP_MOVEMENT_PLAN.md, step 2). Before the campaign's step
// view (hooks_sp.cpp SmoothView) can be ported, cod3mp's camera has to be found and checked. CG_CalcViewValues is
// 0x82467C18: the campaign's 0x82450C10 instruction for instruction (the view setup 0x824674E0 with this client's
// 496-byte record at 0x82C77E30, then 0x8245D338 and 0x8245CDC0 as CG_CalcVrect and CG_CalcFov); CG_DrawActiveFrame
// calls it and then reads the camera's axes at cg + 4976, as the campaign's at cg + 4496. After it, for the active
// local client: the camera (cg + 4960, coronas.cpp; cg = 0x82C7D490 + client x 6368) against the drawn playerState's
// eye (the entity list 0x82BCC7FC, the entity's gclient at + 512, the view height at + 220: viewmodel.cpp). Logged per
// local player every 10 s: how often the camera is at the eye, how far off, and how often the camera and the
// playerState moved, which tells step 3 whether the camera moves only with the movement steps. Nothing is written.
namespace view {
constexpr uint32_t kCg = COD3_ADDR(0x82A98D90, 0x82C7D490);  // cg_t of local client 0 [rexglue-native-mp] 2026-11-11 COD3_ADDR
constexpr uint32_t kCgSize = 6368;          // (campaign 5840)
constexpr uint32_t kCgViewOrigin = 4960;    // the camera's origin, vec3 (campaign 4480)
constexpr uint32_t kActiveClient = COD3_ADDR(0x82A2A1C8, 0x82BCBE9C);  // program_mp.h client_active_index (client side) [rexglue-native-mp] 2026-11-11 COD3_ADDR
constexpr uint32_t kEntityClient = 512;     // the entity's gclient, the playerState first (campaign 528)
constexpr uint32_t kViewHeight = 220;       // the view height the client game reads (campaign 188)

struct Stats {
  uint64_t frames = 0, at_eye = 0, off_eye = 0, no_ps = 0;
  uint64_t cam_moved = 0, origin_moved = 0, cam_only = 0;  // cam_only: the camera moved, the origin didn't
  float max_across = 0.0f, min_up = 0.0f, max_up = 0.0f;   // the camera's offset from the eye (frames at the eye)
  float last_cam[3] = {}, last_origin[3] = {};
  float cam[3] = {}, eye[3] = {};  // the last frame's, for the log line
  bool have_last = false;
  uint32_t ps = 0;
};
std::array<Stats, 4> g_stats;
Clock::time_point g_view_since = Clock::now();

uint32_t DrawnPlayerState(uint8_t* base, uint32_t client) {
  const uint32_t list = g::ReadU32(base, mv::mp_fn::kLocalEntities);
  if (!ValidGuest(list)) return 0;
  const uint32_t ent = g::ReadU32(base, list + 4 * (client + 1));
  if (!ValidGuest(ent)) return 0;
  const uint32_t ps = g::ReadU32(base, ent + kEntityClient);
  return ValidGuest(ps) ? ps : 0;
}

void MaybeLog() {
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - g_view_since).count();
  if (seconds < 10.0) return;
  g_view_since = now;
  for (uint32_t c = 0; c < g_stats.size(); ++c) {
    const Stats& s = g_stats[c];
    if (!s.frames) continue;
    REXLOG_INFO("cod3mp: movement: stage 2: camera, local player {}: {} frames ({:.0f} a second); at the eye "
                "(playerState {:#x} origin + view height) in {}, away from it in {}, no playerState in {}; at the eye "
                "it was up to {:.2f} across and {:.2f} to {:.2f} up from it; the camera moved in {} frames, the "
                "playerState in {}, the camera alone in {}; last frame camera ({:.1f} {:.1f} {:.1f}), eye "
                "({:.1f} {:.1f} {:.1f})",
                c + 1, s.frames, s.frames / seconds, s.ps, s.at_eye, s.off_eye, s.no_ps, s.max_across, s.min_up,
                s.max_up, s.cam_moved, s.origin_moved, s.cam_only, s.cam[0], s.cam[1], s.cam[2], s.eye[0], s.eye[1],
                s.eye[2]);
  }
  for (Stats& s : g_stats) {
    const bool have = s.have_last;
    float cam[3], origin[3];
    for (int i = 0; i < 3; ++i) cam[i] = s.last_cam[i], origin[i] = s.last_origin[i];
    s = Stats{};
    s.have_last = have;  // keep the last frame, so the next 10 s count their first frame's movement too
    for (int i = 0; i < 3; ++i) s.last_cam[i] = cam[i], s.last_origin[i] = origin[i];
  }
}

void NoteFrame(uint8_t* base) {
  const uint32_t client = g::ReadU32(base, kActiveClient);
  if (client >= g_stats.size()) return;
  Stats& s = g_stats[client];
  ++s.frames;
  const uint32_t at = kCg + client * kCgSize + kCgViewOrigin;
  float cam[3];
  for (int i = 0; i < 3; ++i) cam[i] = g::ReadF32(base, at + 4 * i);
  const uint32_t ps = DrawnPlayerState(base, client);
  if (!ps) {
    ++s.no_ps;
    s.have_last = false;
    return;
  }
  s.ps = ps;
  float origin[3];
  for (int i = 0; i < 3; ++i) origin[i] = g::ReadF32(base, ps + P.ps.origin + 4 * i);
  const float height = g::ReadF32(base, ps + kViewHeight);
  const float eye[3] = {origin[0], origin[1], origin[2] + height};
  const float dx = cam[0] - eye[0], dy = cam[1] - eye[1], dz = cam[2] - eye[2];
  const float across = std::sqrt(dx * dx + dy * dy);
  // The campaign's step view counts the camera as at the eye within 40 across (the lean, the bob) and a little up or
  // down (the landing dip, the stair ease); a cutscene, a turret or a dead player's camera is elsewhere.
  if (std::isfinite(across) && std::isfinite(dz) && across <= 40.0f && dz >= -24.0f && dz <= 24.0f) {
    if (!s.at_eye) s.min_up = s.max_up = dz;
    ++s.at_eye;
    s.max_across = std::max(s.max_across, across);
    s.min_up = std::min(s.min_up, dz);
    s.max_up = std::max(s.max_up, dz);
  } else {
    ++s.off_eye;
  }
  if (s.have_last) {
    const bool cam_moved = cam[0] != s.last_cam[0] || cam[1] != s.last_cam[1] || cam[2] != s.last_cam[2];
    const bool origin_moved =
        origin[0] != s.last_origin[0] || origin[1] != s.last_origin[1] || origin[2] != s.last_origin[2];
    s.cam_moved += cam_moved;
    s.origin_moved += origin_moved;
    s.cam_only += cam_moved && !origin_moved;
  }
  for (int i = 0; i < 3; ++i) {
    s.last_cam[i] = s.cam[i] = cam[i];
    s.last_origin[i] = origin[i];
    s.eye[i] = eye[i];
  }
  s.have_last = true;
}
}  // namespace view

// --- [rexglue-native thread2] 2026-11-24 the fixed movement steps (cod3mp_move_hz; rule 14, FPS-RULE14.md M4) ----
// The game's Pmove (0x824B4270, the campaign's 0x82496978 instruction for instruction) moves a player through one
// usercmd in one PmoveSingle (0x824B39B8) of the whole usercmd's time (at most 666 ms a step without pmove_fixed), and
// a usercmd is one drawn frame of the console that sent it: 50 ms steps at 20 fps, 7 ms at 144. The step's length
// changes the move: gravity and friction are applied once a step, the jump, the ground and the step-up tests once, and
// pml.msec scales the rest. The campaign moves in steps of 1/cod3_move_hz s that end at fixed times of the usercmds'
// clock (hooks_sp.cpp RunHzSteps; the ends from rules.h MoveStepEndAfter: 16, 17, 17 ms at 60, the console's own
// milliseconds at 60 fps). This is that, for every player whose ClientThink runs on this console:
//  - each step end within the usercmd is one Pmove call with that step's time (ps command_time = the step's start,
//    the usercmd's server time = its end); the call's arguments are given again to each (a call changes them);
//  - a usercmd with no step end in it (above 60 fps) calls Pmove with no time (its loop runs no PmoveSingle), then
//    turns the view by the usercmd (PM_UpdateViewAngles 0x824AC818, as PmoveSingle calls it), so the view follows
//    every usercmd;
//  - such a usercmd leaves the pmove_t's box (pm + 272 mins, + 288 maxs; ClientThink_real 0x82592CD0 zeroes the pm,
//    336 bytes, and copies the box into the entity at 0x82593334: + 240 / + 256, read from r1 + 448 / + 464 with the
//    pm at r1 + 176, the campaign's 0x8256C7EC at r1 + 416 / + 432 with its pm at r1 + 144) empty: it gets this
//    player's box from the last step back (the campaign's KeepBox), or nobody could hit him in that frame;
//  - and ClientThink_real's unstick (0x8249F070 from 0x825930E0, for a player who pressed to move and whose origin
//    didn't change, the campaign's 0x82483C20) is left out after it (stuck_fix_mp.cpp asks MoveStepUnstickLeftOut).
// Left to the game: a call with pm_flags 0x100000 (Pmove gives it to 0x824AA438) and an older usercmd. The touches
// after the move already follow the 1/60 s marks of the usercmd clock (touch_pace_mp.cpp). cod3mp's client game has no
// movement prediction (program_mp.h), so nothing on the wire changes: a console moves its own players' usercmds as before,
// only in these steps.
namespace steps {
constexpr uint32_t kPmMins = 272, kPmMaxs = 288;  // the pmove_t's box (see above)
#if defined(COD3_MP)  // cod3mp's own address (tools/mp_aliases.py), as the hooks' block below
constexpr uint32_t kUnstickReturn = 0x825930E4;    // ClientThink_real, after its call of 0x8249F070
#endif
constexpr int kMostSlots = 64;

struct Player {
  uint32_t ps = 0;
  bool box_valid = false;
  uint32_t box[8] = {};
  int box_time = 0;
  // the debug log, since its last line
  int cmds = 0, steps = 0, no_step = 0, boxes_kept = 0, most_steps = 0;
  float path = 0.0f, z_low = 0.0f, z_high = 0.0f, last[3] = {};  // across the ground since the last line; heights
  bool have_last = false;
  int first_time = 0, last_time = 0;
};
std::array<Player, kMostSlots> g_players;
thread_local bool t_no_step = false;  // the last Pmove ran no step: ClientThink's unstick is left out after it
std::atomic<uint64_t> g_unsticks_left_out{0};

Player* Find(uint32_t ps) {
  for (Player& p : g_players)
    if (p.ps == ps) return &p;
  for (Player& p : g_players)
    if (p.ps == 0) {
      p.ps = ps;
      return &p;
    }
  return nullptr;
}

uint32_t Cmd(uint32_t pm) { return pm + P.pmove.cmd; }

void TurnView(PPCContext& ctx, uint8_t* base, uint32_t pm, uint32_t ps, int msec) {
  const uint32_t saved_pm = g::ReadU32(base, P.pm);
  g::WriteU32(base, P.pm, pm);  // PM_UpdateViewAngles reads the pm global, as inside Pmove
  {
    g::Call c(ctx, base);
    c.Invoke(P.update_view_angles, {ps, Cmd(pm), pm + P.pmove.view_arg, static_cast<uint64_t>(std::max(msec, 1)),
                                    g::ReadU32(base, pm + P.pmove.view_flag)});
  }
  g::WriteU32(base, P.pm, saved_pm);
}

void KeepBox(uint8_t* base, uint32_t pm, Player& p, bool stepped, int final_time) {
  if (stepped) {
    for (uint32_t i = 0; i < 4; ++i) {
      p.box[i] = g::ReadU32(base, pm + kPmMins + 4 * i);
      p.box[4 + i] = g::ReadU32(base, pm + kPmMaxs + 4 * i);
    }
    p.box_valid = g::ReadF32(base, pm + kPmMaxs + 8) > g::ReadF32(base, pm + kPmMins + 8);  // no height: none
    p.box_time = final_time;
    return;
  }
  if (!p.box_valid || final_time < p.box_time || final_time - p.box_time > 1000) {  // a new level, a restart
    p.box_valid = false;
    return;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    g::WriteU32(base, pm + kPmMins + 4 * i, p.box[i]);
    g::WriteU32(base, pm + kPmMaxs + 4 * i, p.box[4 + i]);
  }
  ++p.boxes_kept;
}

// The debug log's measures (cod3mp_debug_move_steps), after each usercmd: the same at every frame rate if the moves are.
void Measure(uint8_t* base, Player& p, int final_time) {
  if (!REXCVAR_GET(cod3mp_debug_move_steps)) return;
  float o[3];
  for (uint32_t i = 0; i < 3; ++i) o[i] = g::ReadF32(base, p.ps + P.ps.origin + 4 * i);
  if (!p.have_last || final_time < p.last_time || final_time - p.last_time > 1000) {  // the start, a level, a spawn
    p.have_last = true;
    p.path = 0.0f;
    p.z_low = p.z_high = o[2];
    p.first_time = final_time;
  } else {
    const float dx = o[0] - p.last[0], dy = o[1] - p.last[1];
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d < 64.0f) p.path += d;  // more: a spawn
    p.z_low = std::min(p.z_low, o[2]);
    p.z_high = std::max(p.z_high, o[2]);
  }
  for (int i = 0; i < 3; ++i) p.last[i] = o[i];
  p.last_time = final_time;
}

void MaybeLog(uint8_t* base) {
  if (!REXCVAR_GET(cod3mp_debug_move_steps)) return;
  static Clock::time_point next{};
  const auto now = Clock::now();
  if (next == Clock::time_point{}) next = now + std::chrono::seconds(2);
  if (now < next) return;
  next = now + std::chrono::seconds(2);
  for (Player& p : g_players) {
    if (p.ps == 0 || p.cmds == 0) continue;
    REXLOG_INFO("cod3mp: move steps: playerState {:#x}: {} usercmds, {} steps of 1/{} s (at most {} in one), {} "
                "without a step ({} got the last step's box), at {:.1f} {:.1f} {:.1f}; unsticks left out {} "
                "(cod3mp_move_hz)",
                p.ps, p.cmds, p.steps, REXCVAR_GET(cod3mp_move_hz), p.most_steps, p.no_step, p.boxes_kept,
                g::ReadF32(base, p.ps + P.ps.origin), g::ReadF32(base, p.ps + P.ps.origin + 4),
                g::ReadF32(base, p.ps + P.ps.origin + 8), g_unsticks_left_out.load());
    if (p.last_time > p.first_time)
      REXLOG_INFO("cod3mp: move steps: playerState {:#x}: {:.1f} units across the ground in {} ms of usercmds "
                  "({:.1f} a second), height {:.1f} to {:.1f} ({:.1f})",
                  p.ps, p.path, p.last_time - p.first_time, p.path * 1000.0f / float(p.last_time - p.first_time),
                  p.z_low, p.z_high, p.z_high - p.z_low);
    p.cmds = p.steps = p.no_step = p.boxes_kept = p.most_steps = 0;
    p.path = 0.0f;
    p.z_low = p.z_high = p.last[2];
    p.first_time = p.last_time;
  }
}

// Returns false when the call is left to the game.
bool Run(PPCContext& ctx, uint8_t* base, int hz) {
  const uint32_t pm = ctx.r3.u32;
  if (!ValidGuest(pm)) return false;
  const uint32_t ps = g::ReadU32(base, pm + P.pmove.ps);
  if (!ValidGuest(ps) || (g::ReadU32(base, ps + P.ps.pm_flags) & kSpecialMoveFlag)) return false;
  const uint32_t cmd = Cmd(pm);
  const int final_time = g::ReadS32(base, cmd + P.cmd.server_time);
  const int command_time = g::ReadS32(base, ps + P.ps.command_time);
  if (final_time <= command_time || command_time < 0) return false;  // nothing to move: the game's own call
  Player* p = Find(ps);
  if (!p) return false;
  // Pmove keeps at most 1000 ms of a usercmd (cod3mp has no 50 ms limit for pm_flags 0x80000, program_mp.h)
  int start = mv::MoveStepEndAtOrBefore(command_time, hz);
  if (final_time - start > 1000) start = final_time - 1000;
  // Pmove(pm, r4): each step gets the caller's argument registers again
  const PPCRegister args[8] = {ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r9, ctx.r10};
  auto restore_args = [&] {
    ctx.r3 = args[0]; ctx.r4 = args[1]; ctx.r5 = args[2]; ctx.r6 = args[3];
    ctx.r7 = args[4]; ctx.r8 = args[5]; ctx.r9 = args[6]; ctx.r10 = args[7];
  };
  int steps = 0, t = start;
  for (int end = mv::MoveStepEndAfter(t, hz); end <= final_time; end = mv::MoveStepEndAfter(end, hz)) {
    g::WriteU32(base, ps + P.ps.command_time, static_cast<uint32_t>(t));
    g::WriteU32(base, cmd + P.cmd.server_time, static_cast<uint32_t>(end));
    restore_args();
    __imp__sub_824B4270(ctx, base);
    t = end;
    ++steps;
  }
  if (steps == 0) {
    g::WriteU32(base, ps + P.ps.command_time, static_cast<uint32_t>(start));
    g::WriteU32(base, cmd + P.cmd.server_time, static_cast<uint32_t>(start));
    restore_args();
    __imp__sub_824B4270(ctx, base);  // no time: its loop runs no step
    TurnView(ctx, base, pm, ps, final_time - command_time);
    t_no_step = true;
    ++p->no_step;
  }
  KeepBox(base, pm, *p, steps > 0, final_time);
  g::WriteU32(base, ps + P.ps.command_time, static_cast<uint32_t>(final_time));
  g::WriteU32(base, cmd + P.cmd.server_time, static_cast<uint32_t>(final_time));
  ++p->cmds;
  p->steps += steps;
  p->most_steps = std::max(p->most_steps, steps);
  return true;
}

// After every Pmove call (stepped or the game's own): the debug log's measures.
void AfterMove(uint8_t* base, uint32_t pm, bool stepped) {
  if (!REXCVAR_GET(cod3mp_debug_move_steps) || !ValidGuest(pm)) return;
  const uint32_t ps = g::ReadU32(base, pm + P.pmove.ps);
  if (!ValidGuest(ps)) return;
  Player* p = Find(ps);
  if (!p) return;
  if (!stepped) {  // the game's own call: one step of the usercmd's time
    ++p->cmds;
    ++p->steps;
    p->most_steps = std::max(p->most_steps, 1);
  }
  Measure(base, *p, g::ReadS32(base, ps + P.ps.command_time));
  MaybeLog(base);
}
}  // namespace steps

}  // namespace

namespace cod3::movement::mp {
// stuck_fix_mp.cpp: whether this call of 0x8249F070 is ClientThink_real's unstick right after a usercmd that ran no
// movement step (its origin didn't change because no step ran, not because the player is stuck): then it is left out.
bool MoveStepUnstickLeftOut(uint32_t lr) {
  if (!steps::t_no_step || lr != steps::kUnstickReturn) return false;
  steps::t_no_step = false;
  steps::g_unsticks_left_out.fetch_add(1, std::memory_order_relaxed);
  return true;
}
}  // namespace cod3::movement::mp

// [acct2-main] 2026-11-05 This file is built into cod3mp only, so COD3_MP is always set and nothing changes here.
// The block tells tools/mp_aliases.py and test_multiplayer_build that these names are cod3mp's own functions
// (no campaign name to map), as in pc_menu.cpp and keybinds/pad_binds.cpp.
#if defined(COD3_MP)

// Pmove(pm) 0x824B4270 (program_mp.h): the game's own call, always; before it, the settings' log and (with
// cod3mp_debug_movement) what it moves.
// (declared at the top: the fixed steps call it)
REX_HOOK_RAW(sub_824B4270) {
  LogSettings();
  const uint32_t pm = ctx.r3.u32;
  if (REXCVAR_GET(cod3mp_debug_movement) && ValidGuest(pm)) {
    Note(base, pm);
    MaybeLogSeen();
  }
  steps::t_no_step = false;  // [rexglue-native thread2] set again by a usercmd that runs no step
  const int hz = REXCVAR_GET(cod3mp_move_hz);
  const uint32_t pm_arg = ctx.r3.u32;
  const bool stepped = hz > 0 && P.update_view_angles != 0 && steps::Run(ctx, base, hz);
  if (!stepped) __imp__sub_824B4270(ctx, base);
  steps::AfterMove(base, pm_arg, stepped);
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
  // [acct2-main] the slot, then whether a local client plays it (LocalClientsInUse)
  const int slot = SlotOf(base, ent);
  const uint32_t in_use = LocalClientsInUse(base);
  const int controller = ControllerOf(slot, in_use);
  ++g_think_calls[controller + 1];
  static int s_described = 0;  // the first 8 calls: what the entity and the table hold, to check the mapping
  if (s_described < 8 && ValidGuest(ent)) {
    ++s_described;
    const uint32_t table = g::ReadU32(base, mv::mp_fn::kLocalEntities);
    REXLOG_INFO("cod3mp: movement: stage 2: ClientThink entity {:#x} (+512 {:#x}, +0x208 {:#x}) -> local player {}; "
                "table {:#x}: {:#x} {:#x} {:#x} {:#x}; slot {}, local clients in use {:#x}",
                ent, g::ReadU32(base, ent + 512), g::ReadU32(base, ent + 0x208), controller + 1, table,
                ValidGuest(table) ? g::ReadU32(base, table + 4) : 0, ValidGuest(table) ? g::ReadU32(base, table + 8) : 0,
                ValidGuest(table) ? g::ReadU32(base, table + 12) : 0,
                ValidGuest(table) ? g::ReadU32(base, table + 16) : 0, slot + 1, in_use);
  }
  // [acct2-main] once per slot: a slot in the table that no local client plays (another console's player)
  static uint32_t s_not_local_logged = 0;
  if (slot >= 0 && controller < 0 && !(s_not_local_logged & (1u << slot))) {
    s_not_local_logged |= 1u << slot;
    REXLOG_INFO("cod3mp: movement: stage 2: ClientThink for slot {} (entity {:#x}): not a local player here "
                "(local clients in use {:#x}{}); it keeps Call of Duty 3",
                slot + 1, ent, in_use, InOrder(in_use) ? "" : ", not in order: no slot counts as local");
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

// [acct2-main] 2026-11-05 CG_CalcViewValues (stage 2 step 2, observe only: the view namespace above). The game's own
// function always runs first and nothing it set is changed.
REX_EXTERN(__imp__sub_82467C18);
REX_HOOK_RAW(sub_82467C18) {  // CG_CalcViewValues
  __imp__sub_82467C18(ctx, base);
  if (!REXCVAR_GET(cod3mp_movement_stage2)) return;
  static bool s_said = false;
  if (!s_said) {
    s_said = true;
    REXLOG_INFO("cod3mp: movement: stage 2: watching the camera after CG_CalcViewValues (0x82467C18); nothing is "
                "changed");
  }
  view::NoteFrame(base);
  view::MaybeLog();
}

#endif  // defined(COD3_MP) [acct2-main]
