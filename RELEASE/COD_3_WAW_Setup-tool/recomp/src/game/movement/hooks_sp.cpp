// cod3 - the campaign's movement hooks: sprint, the CoD4 style's parts, and
// the sprint button (game/movement/rules.h, sprint_input.h).
//
// Server side (the campaign is its own server; every hook runs inside the
// one Pmove call per usercmd, from ClientThink_real):
//   Pmove 0x82496978          the rules of the player being moved; the CoD4
//                             speed scales swapped in for the call and put
//                             back after it (the playerState, savegames and
//                             level scripts keep the game's own values);
//                             CoD4 speeds also runs it in CoD4 PC's 125 fps
//                             steps (8 ms, the game's own pmove_fixed loop)
//   PmoveSingle 0x824960D0    with those steps, the velocity rounded to whole
//                             units after each one, as on CoD4 PC (its air
//                             strafing and jump height)
//   PM_UpdateSprint 0x824655B0  no sprint while aiming, firing or not moving
//                             forward; CoD4's sideways x0.667 while sprinting
//   PM_UpdateStamina 0x82477F28 the stamina with tuned numbers (rules.h)
//   PM_CheckJump 0x82478100   CoD4 sprint keeps its stamina when jumping
//   PM_AirMove 0x82490B70     marks the air move (a bounce happens only there)
//   PM_ClipVelocity 0x8246B138  the step move's two clips become CoD4's
//                             projection on steep slopes (bounces)
//   0x82483C20 from ClientThink_real  [claude-9b] not after a usercmd that ran
//                             no movement step (see g_no_step)
// With alternate rules off (sprint off, CoD3 style) every hook calls the
// game's own code unchanged: the console's movement, number for number.
//
// [acct2-main] 2026-10-26 The movement's rate (owner 2026-10-25 23:30: "also collision needs to
// be that of 60fps", and asked which: "Player vs world, All game collision"). The campaign moves
// the player once per usercmd, one per drawn frame, with that frame's milliseconds, so how the
// player meets walls, stairs, slopes and the ground changed with the frame rate. With
// cod3_move_hz (60) the CoD3 style moves in steps that end at fixed times of the game's clock
// (rules.h MoveStepEndAfter: 16, 17, 17 ms at 60, the game's own milliseconds at 60 fps), one Pmove
// call per step as the game at 60 fps makes one per frame: none, one or several in a usercmd. The
// playerState's commandTime is the usercmd's time again afterwards (as with the CoD4 steps), and a
// usercmd too short for a step still turns the view (PM_UpdateViewAngles). The CoD4 style keeps
// CoD4 PC's 125 fps steps (its own design). Above the rate the player's position changes in steps,
// so the camera is smoothed between the last two (CG_CalcViewValues hook below; one step behind
// at most, the view angles are never delayed). 0: one move a frame, as the game.
//
// Client side (the player's input):
//   IN_SprintDown/Up, IN_SprintbreathDown/Up (empty in the retail campaign):
//     the game's own sprint button, kbutton 15 -> usercmd bit 2
//   IN_BinocularsDown/Up: the pad's L3 sprints while pushing forward
//     (the keyboard's own keys call +sprintbreath, +sprint and +binoculars
//     directly: game/keybinds/binds.h)
//   CL_FinishMove: the toggle's end conditions, once per usercmd
//
// Function names: the codegen names unnamed functions sub_<address>; the
// hooks below call __imp__sub_<address>, so a later rename in
// config/names.toml makes the build fail here instead of unhooking silently.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string_view>

#include <rex/cvar.h>  // [claude-9b]
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/movement/program_current.h"
#include "game/movement/rules.h"
#include "game/movement/settings.h"
#include "game/movement/sprint_input.h"
#include "game/protected_dvars.h"
#include "game/server_step.h"  // [acct2-main] the camera on a vehicle
#include "game/movement/step_view.h"  // [claude-9b] EndDrawnStepValues

// Pmove, the game's own (hooked below); RunCod4Steps calls it too.
REX_EXTERN(__imp__sub_82496978);

// [claude-9b] 2026-10-27 Stairs between the movement steps (dev/agents/claude-9b-think/CAMERA_ORIGIN.md 5 (A)).
REXCVAR_DEFINE_BOOL(cod3_move_stairs_view, true, "cod3",
                    "Walking up or down stairs above 60 fps: the camera eases each stair step once, as the game does; "
                    "off: the step between movement steps is eased a second time (a dip of about a stair's height)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [claude-9b] 2026-10-27 (owner 10:13: "the ADS jutter needs fixing ASAP")
REXCVAR_DEFINE_BOOL(cod3_move_bob_smooth, true, "cod3",
                    "Above 60 fps: the view's and the weapon's bob move in every frame (between the movement steps, as "
                    "the camera does); off: they move only in the movement steps (jitter, most visible aiming down the "
                    "sights while moving)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_move_aim_smooth, true, "cod3",
                    "Above 60 fps: raising the weapon to the sights (its travel and the zoom) and leaning move in every "
                    "frame (between the movement steps, as the camera does); off: only in the movement steps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace mv = cod3::movement;
namespace g = cod3::guest;
constexpr const mv::Program& P = mv::kProgram;

// --- the movement step being run (server, game thread) ------------------------------
struct MoveContext {
  bool active = false;      // inside a Pmove whose rules differ from the console's
  mv::MoveRules rules;
  uint32_t pm = 0;
  uint32_t ps = 0;
  float scales[7] = {};     // the playerState's own speed scales, put back after Pmove
  int8_t rightmove = 0;     // the usercmd's own sideways input, put back after Pmove
  bool snap = false;        // CoD4 PC's steps: round the velocity after each PmoveSingle
  bool air_move = false;    // inside PM_AirMove (a bounce happens only there)
};
thread_local MoveContext g_move;

// [claude-9b] 2026-10-27 The last Pmove call ran no movement step: a usercmd shorter than what is left
// of a step (RunHzSteps, RunCod4Steps) runs Pmove with no time and only turns the view. Owner 10-27
// 03:27: "movement is still fucked. Now it doesn't matter the fps when you move you bounce";
// 03:29: CoD4 movement "doesn't work right either". After Pmove, ClientThink_real (0x8256C260) calls
// 0x82483C20 (the call returns to 0x8256C644) when the player pressed to move, Pmove left the origin
// where it was and the player isn't in pm_type 2: the game's fix for a player held in place (relatives:
// CoD2 PC SP 0x005092F0, World at War PC SP 0x004156C0). On the console every usercmd moved the player
// (one Pmove step a frame), so it ran only for a player really held; with steps of 1/60 s (or CoD4's
// 8 ms) every usercmd without a step looked held to it. Run c9b-vt-144 (saint_lo, 144 fps, walking,
// cod3_debug_view_trace): it lifted the player 30 units at the first such usercmd and put him back up
// after each step, while the falling speed kept growing (the step fell 0.1, 0.3 ... 12 units): the
// origin went down and up by up to 12 units every other frame, at any frame rate that has frames
// without a step (CoD4's random lifts on slopes, 10-18, are the same call). Now it runs only after a
// usercmd that ran a step, as on the console.
thread_local bool g_no_step = false;
int g_no_step_skips = 0;  // calls left out (the debug log)

// CoD4 sprint's "let go of the button after running out", per client (the
// campaign moves client 0 only).
mv::SprintRestart g_restart[mv::PlayerPreferences::kMaxClients];

// CoD4 PC's 125 fps steps, per client: the time not moved yet (under one step)
// and whether the last usercmd used the steps (a new run starts at 0).
struct StepState {
  bool on = false;
  int pending = 0;
};
StepState g_steps[mv::PlayerPreferences::kMaxClients];

// The debug log's jump heights, per client: the highest point above the
// jump's start, and the horizontal speed at takeoff and landing (air strafing
// adds to it), logged when the player lands.
struct JumpWatch {
  bool in_jump = false;
  float apex = 0.0f;
  float takeoff_speed = 0.0f;
  float top_speed = 0.0f;
};
JumpWatch g_jumps[mv::PlayerPreferences::kMaxClients];

// The client being moved. The campaign moves one player, client 0 (Pmove's
// only caller is the player's ClientThink); cod3mp's glue finds the index
// per player. (playerState +128 is not it: see program.h pass_entity.)
int ClientIndex(uint8_t* base, uint32_t ps) {
  (void)base;
  (void)ps;
  return 0;
}

mv::SprintRestart& RestartFor(uint8_t* base, uint32_t ps) { return g_restart[ClientIndex(base, ps)]; }

// --- what the client side knows about the local player ---------------------------
// The campaign's player is client 0 on this machine; the last movement step
// leaves its state here for the sprint button (game thread only).
mv::PlayerView g_view;
mv::SprintInput g_input;
int g_last_log_time = -1000000;

constexpr uint32_t kScaleOffsets[7] = {P.ps.scale_ads,    P.ps.scale_run,    P.ps.scale_sprint, P.ps.scale_prone,
                                       P.ps.scale_crouch, P.ps.scale_strafe, P.ps.scale_back};

bool ValidGuest(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

uint32_t Cmd(uint32_t pm) { return pm + P.pmove.cmd; }
int8_t ReadS8(uint8_t* base, uint32_t a) { return static_cast<int8_t>(g::ReadU8(base, a)); }

// The rules of the player being moved. The campaign has one player, this
// machine's; cod3mp looks the client up (its userinfo preference).
mv::MoveRules RulesFor(int client) {
  (void)client;
  return mv::LocalRules();
}

// (The campaign's only player-movement caller is the player's ClientThink:
// every Pmove here is this machine's player; cod3mp keeps one per client.)
void RecordView(uint8_t* base, uint32_t ps) {
  g_view.valid = true;
  g_view.pm_flags = g::ReadU32(base, ps + P.ps.pm_flags);
  g_view.pm_type = g::ReadS32(base, ps + P.ps.pm_type);
  g_view.flags2 = g::ReadU32(base, ps + P.ps.flags2);
  g_view.in_binoculars = g::ReadF32(base, ps + P.ps.binoculars_frac) >= 1.0f;
}

void DebugLog(uint8_t* base, uint32_t pm, uint32_t ps, const mv::MoveRules& rules) {
  if (!mv::DebugMovement()) return;
  const uint32_t cmd = Cmd(pm);
  const int t = g::ReadS32(base, cmd + P.cmd.server_time);
  if (t < g_last_log_time) g_last_log_time = -1000000;  // a new level
  if (t - g_last_log_time < 250) return;
  g_last_log_time = t;
  const float vx = g::ReadF32(base, ps + P.ps.velocity), vy = g::ReadF32(base, ps + P.ps.velocity + 4);
  REXLOG_INFO(
      "cod3: move t={} pos=({:.1f} {:.1f} {:.1f}) hspeed={:.1f} vz={:.1f} stamina={:.3f} flags={:#x} type={} "
      "fwd={} right={} up={} buttons={:#x} latch={} rules={}",
      t, g::ReadF32(base, ps + P.ps.origin), g::ReadF32(base, ps + P.ps.origin + 4),
      g::ReadF32(base, ps + P.ps.origin + 8), std::sqrt(vx * vx + vy * vy), g::ReadF32(base, ps + P.ps.velocity + 8),
      g::ReadF32(base, ps + P.ps.stamina), g::ReadU32(base, ps + P.ps.pm_flags), g::ReadS32(base, ps + P.ps.pm_type),
      static_cast<int>(ReadS8(base, cmd + P.cmd.forwardmove)), static_cast<int>(ReadS8(base, cmd + P.cmd.rightmove)),
      static_cast<int>(ReadS8(base, cmd + P.cmd.upmove)), g::ReadU32(base, cmd + P.cmd.buttons),
      g_input.latched() ? 1 : 0, mv::Describe(rules));
}

// The gun while sprinting. The client game's gun-move code (CG_WeaponMove
// 0x82437490) offsets the gun by the weapon's sprintMove F/R/U (weaponDef
// +1260) instead of its standMove (+1284) while the sprint flag is set, scaled
// by the speed (full at the walk speed). That is the whole sprint pose in the
// campaign: no other code reads the sprint fields with the sprint flag. A
// weapon whose sprintMove is all zero (filled in multiplayer's weapon files
// only) would hold the gun still while sprinting, so it gets a lowered pose
// once, when it first sprints (visual only; the def is the level's copy).
void SprintPose(uint8_t* base) {
  const uint32_t def = g::ReadU32(base, P.pml + P.pml_fields.weapon_def);
  if (!ValidGuest(def) || P.weapon_sprint_pose == 0) return;
  const uint32_t move = def + P.weapon_sprint_pose;
  float v[3];
  for (int i = 0; i < 3; ++i) v[i] = g::ReadF32(base, move + 4 * i);
  const bool none = v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f;
  if (none) {
    for (int i = 0; i < 3; ++i) g::WriteF32(base, move + 4 * i, mv::kSprintPoseFallback[i]);
  }
  if (!mv::DebugMovement()) return;
  static uint32_t logged[16] = {};
  for (uint32_t& d : logged) {
    if (d == def) return;
    if (d == 0) {
      d = def;
      break;
    }
  }
  float stand[3];
  for (int i = 0; i < 3; ++i) stand[i] = g::ReadF32(base, move + P.weapon_stand_move_from_sprint + 4 * i);
  REXLOG_INFO("cod3: sprint pose of weapon def {:#x}: sprintMove {:.2f} {:.2f} {:.2f}{} (standMove {:.2f} {:.2f} {:.2f})",
              def, v[0], v[1], v[2], none ? " -> the port's lowered pose" : "", stand[0], stand[1], stand[2]);
}

// --- the sprint button (client side, game thread) ---------------------------------
void Carry(PPCContext& ctx, uint8_t* base, const mv::KeyAction& a) {
  if (a.kind == mv::KeyAction::kNone) return;
  const uint32_t kb = P.kbuttons + mv::kKbuttonSize * mv::kSprintKbutton;
  g::Call c(ctx, base);
  switch (a.kind) {
    case mv::KeyAction::kPress:
      c.Invoke(P.in_key_down, {kb, static_cast<uint64_t>(static_cast<uint32_t>(a.key)), static_cast<uint32_t>(a.time)});
      break;
    case mv::KeyAction::kRelease:
      c.Invoke(P.in_key_up, {kb, static_cast<uint64_t>(static_cast<uint32_t>(a.key)), static_cast<uint32_t>(a.time)});
      break;
    case mv::KeyAction::kHoldBreathPress:
      if (P.in_holdbreath_down) c.Invoke(P.in_holdbreath_down, {static_cast<uint32_t>(a.key), static_cast<uint32_t>(a.time)});
      break;
    case mv::KeyAction::kHoldBreathRelease:
      if (P.in_holdbreath_up) c.Invoke(P.in_holdbreath_up, {static_cast<uint32_t>(a.key), static_cast<uint32_t>(a.time)});
      break;
    default:
      break;
  }
  if (mv::DebugMovement() && (a.kind == mv::KeyAction::kPress || a.kind == mv::KeyAction::kRelease))
    REXLOG_INFO("cod3: sprint button {} (key {})", a.kind == mv::KeyAction::kPress ? "down" : "up", a.key);
}

bool Aiming() {
  // The last usercmd's aim bit (CL_KeyMove sets it from CL_IsAiming).
  return g_view.valid && (g_view.pm_flags & mv::pmf::kAds) != 0;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// PM_UpdateViewAngles for a usercmd that runs no CoD4 step (shorter than
// what is left of a step): the playerState's view angles still follow every
// usercmd, as the game's own Pmove does, so the client game's view-angle
// prediction (which replays only the newer usercmds) stays right.
void UpdateViewAngles(PPCContext& ctx, uint8_t* base, uint32_t pm, uint32_t ps, int msec) {
  const uint32_t saved_pm = g::ReadU32(base, P.pm);
  g::WriteU32(base, P.pm, pm);  // the function reads the pm global, as inside Pmove
  {
    g::Call c(ctx, base);
    c.Invoke(P.update_view_angles, {ps, Cmd(pm), pm + P.pmove.view_arg, static_cast<uint64_t>(std::max(msec, 1)),
                                    g::ReadU32(base, pm + P.pmove.view_flag)});
  }
  g::WriteU32(base, P.pm, saved_pm);
}

// [claude-9b] 2026-10-27 What the drawn frame reads raw from the playerState besides the origin, which also changes
// only in movement steps (owner 10:13: "the ADS jutter needs fixing ASAP"; claude-9b-think FINDINGS-ADS.md): the bob
// cycle (+40, its low byte: the view's and the gun's bob phase, 0x824345D8 -> cg +5804), the speed across the ground
// (velocity +16 / +20: the bob's size and the gun's run offset, cg +5808), how far the weapon is up to the sights
// (+140: the gun's travel and the zoom, CG_GetViewFov) and the lean (+80).
constexpr uint32_t kPsBobCycle = 40;
constexpr uint32_t kPsLean = 80;
struct StepVals {
  uint32_t bob = 0;
  float speed = 0.0f;
  float aim = 0.0f;
  float lean = 0.0f;
};
StepVals ReadStepVals(uint8_t* base, uint32_t ps) {
  StepVals v;
  v.bob = g::ReadU32(base, ps + kPsBobCycle) & 0xFFu;
  const float vx = g::ReadF32(base, ps + P.ps.velocity), vy = g::ReadF32(base, ps + P.ps.velocity + 4);
  v.speed = std::sqrt(vx * vx + vy * vy);
  v.aim = g::ReadF32(base, ps + P.ps.binoculars_frac);
  v.lean = g::ReadF32(base, ps + kPsLean);
  return v;
}

void Cod4StepsDone(uint8_t* base, uint32_t ps, int client, const float before[3], const StepVals& pre,
                   int start_time, int steps, int step_ms, int final_time);  // [claude-9b] below

// Runs the game's Pmove in CoD4 PC's 8 ms steps (its own pmove_fixed loop),
// the usercmd's time and the time left from the usercmds before. The
// playerState's commandTime is the usercmd's time again afterwards, so
// ClientThink and everything after it count time as before; only the movement
// steps are fixed. The velocity is rounded after each step (PmoveSingle hook).
void RunCod4Steps(PPCContext& ctx, uint8_t* base, uint32_t pm, uint32_t ps, int client) {
  constexpr int kStep = mv::cod4::kMoveStepMsec;
  const uint32_t cmd = Cmd(pm);
  const int final_time = g::ReadS32(base, cmd + P.cmd.server_time);
  const int command_time = g::ReadS32(base, ps + P.ps.command_time);
  StepState& st = g_steps[client];
  if (!st.on) st.pending = 0;
  st.on = true;
  // Pmove keeps at most 1000 ms, and 50 ms with pm_flags 0x80000: whole steps
  // within that.
  const int max_span = (g::ReadU32(base, ps + P.ps.pm_flags) & 0x80000u) ? 48 : 1000;
  const mv::StepPlan plan = mv::PlanSteps(command_time, final_time, st.pending, kStep, max_span);

  const uint32_t fixed = g::ReadU32(base, pm + P.pmove.pmove_fixed);
  const uint32_t step_msec = g::ReadU32(base, pm + P.pmove.pmove_msec);
  g::WriteU32(base, ps + P.ps.command_time, static_cast<uint32_t>(plan.start_time));
  g::WriteU32(base, cmd + P.cmd.server_time, static_cast<uint32_t>(plan.start_time + plan.steps * kStep));
  g::WriteU32(base, pm + P.pmove.pmove_fixed, 1);
  g::WriteU32(base, pm + P.pmove.pmove_msec, kStep);
  float before[3];  // [claude-9b] the camera between the steps (StepViewOffset)
  for (int i = 0; i < 3; ++i) before[i] = g::ReadF32(base, ps + P.ps.origin + 4 * i);
  const StepVals pre = ReadStepVals(base, ps);  // [claude-9b] the bob, speed, aim and lean too
  g_move.snap = true;
  __imp__sub_82496978(ctx, base);  // no steps: its loop runs none
  g_move.snap = false;
  if (plan.steps == 0) g_no_step = true;  // [claude-9b]
  g::WriteU32(base, pm + P.pmove.pmove_fixed, fixed);
  g::WriteU32(base, pm + P.pmove.pmove_msec, step_msec);
  if (plan.steps == 0) UpdateViewAngles(ctx, base, pm, ps, final_time - command_time);
  g::WriteU32(base, ps + P.ps.command_time, static_cast<uint32_t>(final_time));
  g::WriteU32(base, cmd + P.cmd.server_time, static_cast<uint32_t>(final_time));
  st.pending = plan.pending_after;
  Cod4StepsDone(base, ps, client, before, pre, plan.start_time, plan.steps, kStep, final_time);  // [claude-9b]
}

// [acct2-main] The fixed movement rate (cod3_move_hz), per client: the last step's positions, to
// smooth the camera between steps, and the newest usercmd's time.
struct HzSteps {
  bool valid = false;  // a step to smooth from
  float from[3] = {};  // the origin before the last step
  float to[3] = {};    // and after it
  int step_end = 0;    // the last step's end (game ms)
  int step_len = 1;    // its length
  int now = 0;         // the newest usercmd's time
  uint32_t ps = 0;     // the playerState moved
  int32_t ease_time = 0;  // [claude-9b] the game's stair easing time when the last step ran (StepEaseTime)
  // [claude-9b] 2026-10-27 The last step's other values (StepVals): how far the bob cycle went forward in it (its
  // byte, so mod 256), and the speed, aim and lean before and after it.
  float bob_delta = 0.0f;
  StepVals vals_from;
  StepVals vals_to;
  // The debug log: steps and usercmds since the last line.
  int log_steps = 0;
  int log_cmds = 0;
  int log_time = -1000000;
};
HzSteps g_hz[mv::PlayerPreferences::kMaxClients];

void ReadOrigin(uint8_t* base, uint32_t ps, float o[3]) {
  for (int i = 0; i < 3; ++i) o[i] = g::ReadF32(base, ps + P.ps.origin + 4 * i);
}

// [claude-9b] 2026-10-27 The game's stair easing (cg.stepTime, cg + 0x10BC; cg_t 0x82A98D90 + client x 5840): set
// to cg.time by the stair-step event (0xA8) that ClientThink_real's ClientEvents passes to CG_EntityEvent after
// Pmove; the camera then rises (or sinks) through the stair's height over 100 ms (CAMERA_ORIGIN.md 3). Read when a
// movement step runs, it tells StepViewOffset whether that step went up or down a stair.
int32_t StepEaseTime(uint8_t* base, int client) {
  return g::ReadS32(base, 0x82A98D90u + static_cast<uint32_t>(client) * 5840u + 0x10BCu);
}

// Moves the player through every step end up to the usercmd's time, one Pmove call each (the
// game's own loop runs one step: the call's time is that step). A usercmd with no step end in it
// runs Pmove with no time, then turns the view, as RunCod4Steps does.
void RunHzSteps(PPCContext& ctx, uint8_t* base, uint32_t pm, uint32_t ps, int client, int hz) {
  const uint32_t cmd = Cmd(pm);
  const int final_time = g::ReadS32(base, cmd + P.cmd.server_time);
  const int command_time = g::ReadS32(base, ps + P.ps.command_time);
  HzSteps& h = g_hz[client];
  if (final_time <= command_time || command_time < 0) {
    // Nothing to move (the game refuses an older usercmd): its own call, as before.
    __imp__sub_82496978(ctx, base);
    return;
  }
  // Pmove keeps at most 1000 ms, and 50 with pm_flags 0x80000 (the time before it is dropped,
  // as the game drops it).
  const int max_span = (g::ReadU32(base, ps + P.ps.pm_flags) & 0x80000u) ? 50 : 1000;
  int start = mv::MoveStepEndAtOrBefore(command_time, hz);
  if (final_time - start > max_span) start = final_time - max_span;

  float before[3] = {};
  StepVals pre;  // [claude-9b] the other values before the last step
  int steps = 0, t = start, last_len = 1;
  // Pmove(pm, r4) is called with two arguments (ClientThink_real passes 0 in r4); a call leaves
  // the argument registers changed, so each step gets the caller's again (2026-10-26: without
  // this the second step's r4 was left over and the trace called through a null pointer).
  const PPCRegister args[8] = {ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r9, ctx.r10};
  auto restore_args = [&] {
    ctx.r3 = args[0]; ctx.r4 = args[1]; ctx.r5 = args[2]; ctx.r6 = args[3];
    ctx.r7 = args[4]; ctx.r8 = args[5]; ctx.r9 = args[6]; ctx.r10 = args[7];
  };
  for (int end = mv::MoveStepEndAfter(t, hz); end <= final_time; end = mv::MoveStepEndAfter(end, hz)) {
    ReadOrigin(base, ps, before);
    pre = ReadStepVals(base, ps);  // [claude-9b]
    g::WriteU32(base, ps + P.ps.command_time, static_cast<uint32_t>(t));
    g::WriteU32(base, cmd + P.cmd.server_time, static_cast<uint32_t>(end));
    restore_args();
    __imp__sub_82496978(ctx, base);
    last_len = end - t;
    t = end;
    ++steps;
  }
  if (steps == 0) {
    g::WriteU32(base, ps + P.ps.command_time, static_cast<uint32_t>(start));
    g::WriteU32(base, cmd + P.cmd.server_time, static_cast<uint32_t>(start));
    restore_args();
    __imp__sub_82496978(ctx, base);  // no time: its loop runs none
    UpdateViewAngles(ctx, base, pm, ps, final_time - command_time);
    g_no_step = true;  // [claude-9b]
  }
  g::WriteU32(base, ps + P.ps.command_time, static_cast<uint32_t>(final_time));
  g::WriteU32(base, cmd + P.cmd.server_time, static_cast<uint32_t>(final_time));

  if (steps > 0) {
    for (int i = 0; i < 3; ++i) h.from[i] = before[i];
    ReadOrigin(base, ps, h.to);
    const float dx = h.to[0] - h.from[0], dy = h.to[1] - h.from[1], dz = h.to[2] - h.from[2];
    // More than a step can move (a teleport, a spawn): nothing to smooth.
    h.valid = dx * dx + dy * dy + dz * dz < 64.0f * 64.0f;
    h.step_end = t;
    h.step_len = std::max(last_len, 1);
    h.ease_time = StepEaseTime(base, client);  // [claude-9b] before this usercmd's events
    // [claude-9b] 2026-10-27 the bob, speed, aim and lean before and after the last step
    h.vals_from = pre;
    h.vals_to = ReadStepVals(base, ps);
    h.bob_delta = float((h.vals_to.bob - pre.bob) & 0xFFu);
  }
  h.now = final_time;
  h.ps = ps;
  h.log_steps += steps;
  h.log_cmds += 1;
  if (mv::DebugMovement() && (final_time - h.log_time >= 1000 || final_time < h.log_time)) {
    if (h.log_time > -1000000 && final_time > h.log_time)
      REXLOG_INFO("cod3: movement steps: {} steps of 1/{} s in {} usercmds over {} ms (cod3_move_hz); "
                  "held-player fix left out after {} usercmds without a step",  // [claude-9b]
                  h.log_steps, hz, h.log_cmds, final_time - h.log_time, g_no_step_skips);
    h.log_time = final_time;
    h.log_steps = 0;
    h.log_cmds = 0;
  }
}

// [claude-9b] 2026-10-27 The CoD4 style's camera between its 8 ms steps (owner 10-27 03:29: CoD4 movement
// "doesn't work right either"): above 125 fps some frames run no step and the next one runs one, so the
// camera moved in steps of 1/125 s. The steps of one usercmd run in one Pmove call (the game's own
// loop), so the last step's start is taken as the call's movement split evenly over its steps; the
// camera is drawn between it and the end by how far the usercmd's time is past the last step's end
// (StepViewOffset, as for cod3_move_hz's steps: one step behind at most). Run c9b-vt2-cod4-144 with
// the whole call as one span: the camera lagged and caught up (0.6 then 3.2 units) after each usercmd
// that ran two steps.
void Cod4StepsDone(uint8_t* base, uint32_t ps, int client, const float before[3], const StepVals& pre,
                   int start_time, int steps, int step_ms, int final_time) {
  HzSteps& h = g_hz[client];
  if (steps > 0) {
    ReadOrigin(base, ps, h.to);
    const float dx = h.to[0] - before[0], dy = h.to[1] - before[1], dz = h.to[2] - before[2];
    h.valid = dx * dx + dy * dy + dz * dz < 64.0f * 64.0f * steps * steps;  // more: a teleport, a spawn
    for (int i = 0; i < 3; ++i) h.from[i] = h.to[i] - (h.to[i] - before[i]) / static_cast<float>(steps);
    h.step_end = start_time + steps * step_ms;
    h.step_len = std::max(step_ms, 1);
    h.ease_time = StepEaseTime(base, client);  // [claude-9b] before this usercmd's events
    // [claude-9b] 2026-10-27 the other values, the last step's part of the call's change (as the origin)
    const float n = static_cast<float>(steps);
    h.vals_to = ReadStepVals(base, ps);
    h.vals_from = h.vals_to;
    h.vals_from.speed = h.vals_to.speed - (h.vals_to.speed - pre.speed) / n;
    h.vals_from.aim = h.vals_to.aim - (h.vals_to.aim - pre.aim) / n;
    h.vals_from.lean = h.vals_to.lean - (h.vals_to.lean - pre.lean) / n;
    h.bob_delta = float((h.vals_to.bob - pre.bob) & 0xFFu) / n;
  }
  h.now = final_time;
  h.ps = ps;
}

// The debug log's jump height: the highest point of each jump above where it
// started (CoD4 PC at 125 fps: about 41.3; CoD3: 39).
void WatchJump(uint8_t* base, uint32_t ps, int client, bool cod4_steps) {
  if (!mv::DebugMovement()) return;
  JumpWatch& j = g_jumps[client];
  const bool jumping = (g::ReadU32(base, ps + P.ps.pm_flags) & mv::pmf::kJumping) != 0;
  const float vx = g::ReadF32(base, ps + P.ps.velocity), vy = g::ReadF32(base, ps + P.ps.velocity + 4);
  const float speed = std::sqrt(vx * vx + vy * vy);
  if (jumping) {
    const float h = g::ReadF32(base, ps + P.ps.origin + 8) - g::ReadF32(base, ps + P.ps.jump_origin_z);
    if (!j.in_jump) {
      j.apex = h;
      j.takeoff_speed = speed;
      j.top_speed = speed;
    }
    j.apex = std::max(j.apex, h);
    j.top_speed = std::max(j.top_speed, speed);
    j.in_jump = true;
  } else if (j.in_jump) {
    j.in_jump = false;
    REXLOG_INFO("cod3: jump apex {:.3f} units above its start; speed {:.1f} at takeoff, {:.1f} at most in the air ({})",
                j.apex, j.takeoff_speed, j.top_speed,
                cod4_steps ? "CoD4 PC 125 fps steps" : "the game's own steps");
  }
}

}  // namespace

// ===========================================================================
// Server side
// ===========================================================================

// Pmove(pmove_t* pm): one usercmd of one player.
REX_EXTERN(__imp__sub_82496978);
REX_HOOK_RAW(sub_82496978) {
  g_no_step = false;  // [claude-9b] set again below for a usercmd that runs no step
  cod3::protected_dvars::Tick();  // the test's pretend match (game/protected_dvars.h)
  const uint32_t pm = ctx.r3.u32;
  const uint32_t ps = ValidGuest(pm) ? g::ReadU32(base, pm + P.pmove.ps) : 0;
  if (!ValidGuest(ps)) {
    __imp__sub_82496978(ctx, base);
    return;
  }
  const mv::MoveRules rules = RulesFor(ClientIndex(base, ps));
  if (rules.AllOriginal()) {
    // The console's movement: nothing of ours runs (but the fixed rate,
    // cod3_move_hz). A sprint key still held from before sprint was switched
    // off doesn't count (the console game never sets the sprint bit).
    g_move.active = false;
    const uint32_t buttons = g::ReadU32(base, Cmd(pm) + P.cmd.buttons);
    if (buttons & mv::button::kSprint) g::WriteU32(base, Cmd(pm) + P.cmd.buttons, buttons & ~mv::button::kSprint);
    // [acct2-main] At the fixed rate (cod3_move_hz), as the console at 60 fps; 0: the game's call.
    const int hz = mv::MoveStepHz();
    if (hz > 0 && P.update_view_angles != 0) {
      RunHzSteps(ctx, base, pm, ps, ClientIndex(base, ps), hz);
    } else {
      g_hz[ClientIndex(base, ps)].valid = false;
      __imp__sub_82496978(ctx, base);
    }
    if (buttons & mv::button::kSprint) g::WriteU32(base, Cmd(pm) + P.cmd.buttons, buttons);
    g_steps[ClientIndex(base, ps)].on = false;
    RecordView(base, ps);
    DebugLog(base, pm, ps, rules);
    return;
  }
  MoveContext& m = g_move;
  m.active = true;
  m.rules = rules;
  m.pm = pm;
  m.ps = ps;
  for (int i = 0; i < 7; ++i) m.scales[i] = g::ReadF32(base, ps + kScaleOffsets[i]);
  m.rightmove = ReadS8(base, Cmd(pm) + P.cmd.rightmove);
  // CoD4's speeds, relative to whatever the game set (ClientSpawn, scripts).
  if (rules.Cod4Speeds()) {
    g::WriteF32(base, ps + P.ps.scale_strafe, m.scales[5] * mv::cod4::kStrafeSpeedScale);
    g::WriteF32(base, ps + P.ps.scale_back, m.scales[6] * mv::cod4::kBackSpeedScale);
  }
  if (rules.Cod4Sprint())
    g::WriteF32(base, ps + P.ps.scale_sprint, m.scales[2] * (mv::cod4::kSprintSpeedScale / P.spawn_sprint_scale));

  const int client = ClientIndex(base, ps);
  const int hz = mv::MoveStepHz();  // [acct2-main]
  if (rules.Cod4Steps() && P.pmove.pmove_fixed != 0 && P.update_view_angles != 0) {
    // [acct2-main] CoD4 PC's own steps; [claude-9b] RunCod4Steps keeps the camera's steps (g_hz)
    RunCod4Steps(ctx, base, pm, ps, client);
  } else if (hz > 0 && P.update_view_angles != 0) {
    g_steps[client].on = false;
    RunHzSteps(ctx, base, pm, ps, client, hz);  // [acct2-main] the fixed rate (cod3_move_hz)
  } else {
    g_steps[client].on = false;
    g_hz[client].valid = false;  // [acct2-main]
    __imp__sub_82496978(ctx, base);
  }

  for (int i = 0; i < 7; ++i) g::WriteF32(base, ps + kScaleOffsets[i], m.scales[i]);
  g::WriteU8(base, Cmd(pm) + P.cmd.rightmove, static_cast<uint8_t>(m.rightmove));
  m.active = false;
  RecordView(base, ps);
  DebugLog(base, pm, ps, rules);
}

// PmoveSingle(pmove_t* pm, ...): one movement step. With CoD4 PC's steps the
// velocity is rounded to whole units after it, as CoD4 PC does after every
// step (Quake III's SnapVector).
REX_EXTERN(__imp__sub_824960D0);
REX_HOOK_RAW(sub_824960D0) {
  const uint32_t pm = ctx.r3.u32;
  __imp__sub_824960D0(ctx, base);
  const bool snap = g_move.active && g_move.snap && ValidGuest(g_move.ps);
  if (snap) {
    for (int i = 0; i < 3; ++i) {
      const uint32_t a = g_move.ps + P.ps.velocity + 4 * i;
      g::WriteF32(base, a, mv::SnapToUnit(g::ReadF32(base, a)));
    }
  }
  // The debug log's jump heights, every step (either style).
  const uint32_t ps = ValidGuest(pm) ? g::ReadU32(base, pm + P.pmove.ps) : 0;
  if (ValidGuest(ps)) WatchJump(base, ps, ClientIndex(base, ps), snap);
}

// PM_UpdateSprint(): sets or clears the sprinting flag from the sprint button.
REX_EXTERN(__imp__sub_824655B0);
REX_HOOK_RAW(sub_824655B0) {
  if (!g_move.active) {
    __imp__sub_824655B0(ctx, base);
    return;
  }
  const uint32_t pm = g::ReadU32(base, P.pm);
  const uint32_t ps = g::ReadU32(base, pm + P.pmove.ps);
  const uint32_t cmd = Cmd(pm);
  const uint32_t flags = g::ReadU32(base, ps + P.ps.pm_flags);
  const uint32_t buttons = g::ReadU32(base, cmd + P.cmd.buttons);
  const bool aiming = (flags & mv::pmf::kAds) != 0;
  const bool firing = (buttons & mv::button::kAttack) != 0;
  const bool restart_ok = RestartFor(base, ps).Allowed(g_move.rules, (buttons & mv::button::kSprint) != 0);
  if (!restart_ok || !mv::SprintAllowed(g_move.rules, ReadS8(base, cmd + P.cmd.forwardmove), aiming, firing)) {
    // Not the game's own test: it would cancel a toggled aim when sprint starts.
    g::WriteU32(base, ps + P.ps.pm_flags, flags & ~mv::pmf::kSprinting);
    g::WriteU8(base, cmd + P.cmd.rightmove, static_cast<uint8_t>(g_move.rightmove));
    return;
  }
  __imp__sub_824655B0(ctx, base);
  const bool sprinting = (g::ReadU32(base, ps + P.ps.pm_flags) & mv::pmf::kSprinting) != 0;
  if (sprinting && !(flags & mv::pmf::kSprinting)) SprintPose(base);
  const int right = sprinting ? mv::SprintRightmove(g_move.rules, g_move.rightmove) : g_move.rightmove;
  g::WriteU8(base, cmd + P.cmd.rightmove, static_cast<uint8_t>(static_cast<int8_t>(right)));
}

// PM_UpdateStamina(): drains while sprinting, recovers after it.
REX_EXTERN(__imp__sub_82477F28);
REX_HOOK_RAW(sub_82477F28) {
  if (!g_move.active || !g_move.rules.sprint) {
    __imp__sub_82477F28(ctx, base);
    return;
  }
  const uint32_t pm = g::ReadU32(base, P.pm);
  const uint32_t ps = g::ReadU32(base, pm + P.pmove.ps);
  const uint32_t cmd = Cmd(pm);
  uint32_t flags = g::ReadU32(base, ps + P.ps.pm_flags);
  mv::StaminaState s;
  s.stamina = g::ReadF32(base, ps + P.ps.stamina);
  if (!std::isfinite(s.stamina)) s.stamina = 1.0f;
  s.last_sprint_time = g::ReadS32(base, ps + P.ps.last_sprint_time);
  s.sprinting = (flags & mv::pmf::kSprinting) != 0;
  s.out_of_breath = (flags & mv::pmf::kOutOfBreath) != 0;
  mv::StaminaInput in;
  in.server_time = g::ReadS32(base, cmd + P.cmd.server_time);
  in.msec = g::ReadS32(base, P.pml + P.pml_fields.msec);
  in.sprint_held = (g::ReadU32(base, cmd + P.cmd.buttons) & mv::button::kSprint) != 0;
  in.noclip = g::ReadS32(base, ps + P.ps.pm_type) == mv::kPmNoclip;
  in.no_drain = g::ReadS32(base, P.no_stamina_drain) != 0;
  const mv::StaminaResult r = mv::StepStamina(mv::StaminaFor(g_move.rules), in, s);

  g::WriteF32(base, ps + P.ps.stamina, s.stamina);
  g::WriteU32(base, ps + P.ps.last_sprint_time, static_cast<uint32_t>(s.last_sprint_time));
  flags = s.out_of_breath ? (flags | mv::pmf::kOutOfBreath) : (flags & ~mv::pmf::kOutOfBreath);
  g::WriteU32(base, ps + P.ps.pm_flags, flags);
  if (r.breath_event) {
    RestartFor(base, ps).RanOut(in.sprint_held);
    // The game's own event, added the way PM_UpdateStamina adds it.
    const uint32_t seq = g::ReadU32(base, ps + P.ps.event_sequence);
    g::WriteU32(base, ps + P.ps.events + 4 * (seq & 3), mv::kEventSprintBreath);
    g::WriteU32(base, ps + P.ps.event_parms + 4 * (seq & 3), 0);
    g::WriteU32(base, ps + P.ps.event_sequence, seq + 1);
  }
  if (r.call_breath_events) {
    g::Call c(ctx, base);
    c.Invoke(P.sprint_breath_events);
  }
}

// PM_CheckJump(): true when a jump started.
REX_EXTERN(__imp__sub_82478100);
REX_HOOK_RAW(sub_82478100) {
  if (!g_move.active) {
    __imp__sub_82478100(ctx, base);
    return;
  }
  const uint32_t pm = g::ReadU32(base, P.pm);
  const uint32_t ps = g::ReadU32(base, pm + P.pmove.ps);
  const float stamina = g::ReadF32(base, ps + P.ps.stamina);
  __imp__sub_82478100(ctx, base);
  // CoD4 sprint: jumping doesn't cost stamina (the campaign takes 0.667).
  if (ctx.r3.u32 != 0 && g_move.rules.Cod4Sprint()) g::WriteF32(base, ps + P.ps.stamina, stamina);
}

// PM_AirMove(...): the move in the air (a bounce happens only there).
REX_EXTERN(__imp__sub_82490B70);
REX_HOOK_RAW(sub_82490B70) {
  const bool outer = g_move.air_move;
  g_move.air_move = true;
  __imp__sub_82490B70(ctx, base);
  g_move.air_move = outer;
}

// PM_ClipVelocity(in, normal, out, overbounce).
REX_EXTERN(__imp__sub_8246B138);
REX_HOOK_RAW(sub_8246B138) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (g_move.active && g_move.rules.Cod4Jumps() && (lr == P.step_clip_return[0] || lr == P.step_clip_return[1])) {
    const uint32_t vin = ctx.r3.u32, vn = ctx.r4.u32, vout = ctx.r5.u32;
    float in[3], n[3], out[3];
    for (int i = 0; i < 3; ++i) {
      in[i] = g::ReadF32(base, vin + 4 * i);
      n[i] = g::ReadF32(base, vn + 4 * i);
    }
    // CoD4's step move projects onto a steep slope (bounces) where CoD3
    // clips: a jump falling onto it (mv::BounceApplies). Walls, walkable
    // ground and every step down while walking stay the game's clip.
    if (mv::IsBounceSlope(n[2])) {
      const uint32_t pm = g::ReadU32(base, P.pm);
      const uint32_t ps = g::ReadU32(base, pm + P.pmove.ps);
      mv::BounceCase bounce;
      bounce.air_move = g_move.air_move;
      bounce.jumping = (g::ReadU32(base, ps + P.ps.pm_flags) & mv::pmf::kJumping) != 0;
      bounce.origin_z = g::ReadF32(base, ps + P.ps.origin + 8);
      bounce.jump_origin_z = g::ReadF32(base, ps + P.ps.jump_origin_z);
      bounce.velocity_z = in[2];
      bounce.normal_z = n[2];
      if (mv::BounceApplies(bounce) && mv::ProjectKeepSpeed(in, n, out)) {
        for (int i = 0; i < 3; ++i) g::WriteF32(base, vout + 4 * i, out[i]);
        if (mv::DebugMovement())
          REXLOG_INFO("cod3: bounce: ({:.0f} {:.0f} {:.0f}) -> ({:.0f} {:.0f} {:.0f}) on a slope of {:.2f}", in[0],
                      in[1], in[2], out[0], out[1], out[2], n[2]);
        return;
      }
      // A clip on a steep slope left to the game (at most 4 lines a second).
      static int last_log_time = -1000000;
      const int t = g::ReadS32(base, Cmd(pm) + P.cmd.server_time);
      if (mv::DebugMovement() && (t - last_log_time >= 250 || t < last_log_time)) {
        last_log_time = t;
        const char* why = !bounce.air_move              ? "the walk move's step down"
                          : !bounce.jumping              ? "no jump"
                          : in[2] >= 0.0f                ? "rising"
                          : mv::BounceApplies(bounce)    ? "straight into it"
                                                         : "above the jump's top";
        REXLOG_INFO("cod3: no bounce ({}): ({:.0f} {:.0f} {:.0f}) clipped on a slope of {:.2f}", why, in[0], in[1],
                    in[2], n[2]);
      }
    }
  }
  __imp__sub_8246B138(ctx, base);
}

// [claude-9b] 2026-10-27 0x82483C20: ClientThink_real calls it after Pmove for a player that pressed to
// move and stayed in place (see g_no_step); Pmove's own code calls it too (0x82486CA8, 0x8248EFF8),
// left as it is. Left out after a usercmd that ran no movement step.
namespace {
constexpr uint32_t kHeldFixReturn = 0x8256C644;  // ClientThink_real, after its call
}  // namespace
REX_EXTERN(__imp__sub_82483C20);
REX_HOOK_RAW(sub_82483C20) {
  if (g_no_step && static_cast<uint32_t>(ctx.lr) == kHeldFixReturn) {
    g_no_step = false;
    ++g_no_step_skips;
    return;
  }
  __imp__sub_82483C20(ctx, base);
}

// ===========================================================================
// [acct2-main] 2026-10-26 Client side: the camera between the fixed movement steps
// ===========================================================================
// CG_CalcViewValues: the view setup it calls (0x824505E8) places the camera from the player's
// origin (refdef origin at cg + 4480, cg = 0x82A98D90 + client x 5840; the client index at
// 0x82A2A1C8), so above cod3_move_hz it would move in steps. After it, the camera is moved back by
// the part of the last step not due yet: the origin between that step's start and end, by how far
// the newest usercmd's time is into the next step. Only while the playerState is where that step
// left it (nothing else moved the player: a vehicle, a script) and the camera is at the player's
// eye (not a cutscene or a turret's camera). The view angles are not touched.
namespace {
constexpr uint32_t kCgBase = 0x82A98D90;    // cg_t of local client 0
constexpr uint32_t kCgSize = 5840;
constexpr uint32_t kCgRefdefOrigin = 4480;  // vec3
constexpr uint32_t kActiveLocalClient = 0x82A2A1C8;

// The camera's offset from the movement steps: true while the playerState is where the last step
// left it and the camera is at the player's eye.
bool StepViewOffset(uint8_t* base, uint32_t client, uint32_t cam, float out[3]) {
  if (mv::MoveStepHz() <= 0) return false;
  const HzSteps& h = g_hz[client];
  if (!h.valid || !ValidGuest(h.ps)) return false;
  float now[3];
  ReadOrigin(base, h.ps, now);
  if (now[0] != h.to[0] || now[1] != h.to[1] || now[2] != h.to[2]) return false;  // moved since the step
  float c[3];
  for (int i = 0; i < 3; ++i) c[i] = g::ReadF32(base, cam + 4 * i);
  // The eye: above the origin by the view height (standing, crouched, prone) with the lean, bob
  // and landing offsets.
  const float ex = c[0] - h.to[0], ey = c[1] - h.to[1], ez = c[2] - h.to[2];
  if (ex * ex + ey * ey > 40.0f * 40.0f || ez < -24.0f || ez > 96.0f) return false;
  const float into = static_cast<float>(h.now - h.step_end) / static_cast<float>(h.step_len);
  const float back = 1.0f - std::clamp(into, 0.0f, 1.0f);  // the part of the last step not due yet
  if (back <= 0.0f) return false;
  for (int i = 0; i < 3; ++i) out[i] = (h.from[i] - h.to[i]) * back;
  // [claude-9b] 2026-10-27 A stair step in the last movement step: the game eases the camera through it (from the
  // event the step raised), so its height is not eased here too (CAMERA_ORIGIN.md 5 (A): a dip of about the stair's
  // height, then a 100 ms return). The step's sideways part is still drawn between the steps.
  if (REXCVAR_GET(cod3_move_stairs_view) && StepEaseTime(base, static_cast<int>(client)) != h.ease_time) {
    out[2] = 0.0f;
  }
  return true;
}

void SmoothView(uint8_t* base) {
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (client >= static_cast<uint32_t>(mv::PlayerPreferences::kMaxClients)) return;
  const uint32_t cam = kCgBase + client * kCgSize + kCgRefdefOrigin;
  float off[3];
  // [claude-9b-think] 2026-10-27 UNTESTED On a ridden vehicle in first person: on the vehicle as it is drawn
  // (cod3_server_smooth_ride). The movement steps' offset is left out there: Pmove doesn't move a rider, so it was a
  // zero offset that stopped the update offset below (the jeep's camera jumped once an update).
  float c[3];
  for (int i = 0; i < 3; ++i) c[i] = g::ReadF32(base, cam + 4 * i);
  const int ride = cod3::server_step::RideViewOffset(base, c, off);
  if (ride < 0) {
    if (!cod3::server_step::PlayerViewOffset(base, off)) return;
  } else if (ride == 0) {
    // Walking: the movement steps. Carried by a vehicle (moved in the game's updates, not by the
    // movement): the same smoothing as the vehicle is drawn with (game/server_step.h).
    if (!StepViewOffset(base, client, cam, off) && !cod3::server_step::PlayerViewOffset(base, off)) return;
  }
  for (int i = 0; i < 3; ++i) g::WriteF32(base, cam + 4 * i, c[i] + off[i]);
}

// [claude-9b] 2026-10-27 The rest of what the drawn frame reads from the playerState in steps (owner 10:13: "the ADS
// jutter needs fixing ASAP"; claude-9b-think FINDINGS-ADS.md 2): drawn between the last step's values by the same part
// as the camera (StepViewOffset's `back`), so the bob, the gun's travel to the sights, the zoom and the lean move in
// every frame instead of with the movement steps (60 a second) while the camera moves in every frame.
constexpr uint32_t kEntityList = 0x82A2AB40;      // *(it) + 4 * (client + 1): the player's entity
constexpr uint32_t kEntityClient = 528;           // its gclient (the playerState first)
constexpr uint32_t kCgBobPhase = 5804;            // 0x824345D8: bob cycle byte x kBobPhaseScale + constant
constexpr uint32_t kCgBobSpeed = 5808;            // 0x824345D8: the speed across the ground
constexpr uint32_t kBobPhaseScale = 0x8207C944;   // the float 0x824345D8 multiplies the bob cycle by
constexpr uint32_t kPmFlagsBobSpeedOwn = 0x10;    // pm_flags: 0x824345D8 sets the speed another way

// The local client's playerState as the client game reads it, or 0.
uint32_t DrawnPlayerState(uint8_t* base, uint32_t client) {
  const uint32_t list = g::ReadU32(base, kEntityList);
  if (!ValidGuest(list)) return 0;
  const uint32_t ent = g::ReadU32(base, list + 4 * (client + 1));
  if (!ValidGuest(ent)) return 0;
  const uint32_t ps = g::ReadU32(base, ent + kEntityClient);
  return ValidGuest(ps) ? ps : 0;
}

// The part of the last movement step not drawn yet (StepViewOffset's `back`; 0: nothing to draw between), while the
// playerState is the one moved and where the step left it.
float StepBack(uint8_t* base, uint32_t client, const HzSteps** out) {
  if (mv::MoveStepHz() <= 0 || client >= static_cast<uint32_t>(mv::PlayerPreferences::kMaxClients)) return 0.0f;
  // [claude-9b-think] 2026-10-27 UNTESTED nothing of the steps to draw between on a ridden vehicle: the vehicle's
  // update sets the speed (and the seat), not the steps (cod3_server_smooth_ride).
  if (cod3::server_step::PlayerRides(base)) return 0.0f;
  const HzSteps& h = g_hz[client];
  if (!h.valid || !ValidGuest(h.ps) || DrawnPlayerState(base, client) != h.ps) return 0.0f;
  float now[3];
  ReadOrigin(base, h.ps, now);
  if (now[0] != h.to[0] || now[1] != h.to[1] || now[2] != h.to[2]) return 0.0f;  // moved since the step
  const float into = static_cast<float>(h.now - h.step_end) / static_cast<float>(h.step_len);
  *out = &h;
  return 1.0f - std::clamp(into, 0.0f, 1.0f);
}

// The aim and the lean written for the drawn frame (BeginDrawnStepValues), to put back after it.
struct DrawnWrite {
  uint32_t at = 0;
  float game = 0.0f;
  float drawn = 0.0f;
};
DrawnWrite g_drawn_aim, g_drawn_lean;

void PutBackOneDrawn(uint8_t* base, DrawnWrite& w) {
  if (!w.at) return;
  if (g::ReadF32(base, w.at) == w.drawn) g::WriteF32(base, w.at, w.game);  // unless the game set it since
  w.at = 0;
}

void WriteOneDrawn(uint8_t* base, DrawnWrite& w, uint32_t at, float game_now, float from, float to, float back) {
  if (game_now != to || from == to) return;  // changed since the step, or nothing to draw between
  const float drawn = to - (to - from) * back;
  if (!std::isfinite(drawn) || drawn == to) return;
  w.at = at;
  w.game = game_now;
  w.drawn = drawn;
  g::WriteF32(base, at, drawn);
}

void PutBackDrawnStepValues(uint8_t* base) {
  PutBackOneDrawn(base, g_drawn_aim);
  PutBackOneDrawn(base, g_drawn_lean);
}

// Before the client game's view (CG_CalcViewValues): the aim and the lean between the last step's values. The gun,
// the zoom (CG_GetViewFov) and the crosshair read them during the client game's frame; the game's values go back
// after it (EndDrawnStepValues, from server_step.cpp) and before any game code runs (its PutBackDrawn).
void BeginDrawnStepValues(uint8_t* base) {
  PutBackDrawnStepValues(base);
  if (!REXCVAR_GET(cod3_move_aim_smooth)) return;
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  const HzSteps* h = nullptr;
  const float back = StepBack(base, client, &h);
  if (back <= 0.0f || !h) return;
  const uint32_t aim_at = h->ps + P.ps.binoculars_frac, lean_at = h->ps + kPsLean;
  WriteOneDrawn(base, g_drawn_aim, aim_at, g::ReadF32(base, aim_at), h->vals_from.aim, h->vals_to.aim, back);
  WriteOneDrawn(base, g_drawn_lean, lean_at, g::ReadF32(base, lean_at), h->vals_from.lean, h->vals_to.lean, back);
}
}  // namespace

namespace cod3::movement {
void EndDrawnStepValues(uint8_t* base) { PutBackDrawnStepValues(base); }
}  // namespace cod3::movement

REX_EXTERN(__imp__CG_CalcViewValues);
REX_HOOK_RAW(CG_CalcViewValues) {
  BeginDrawnStepValues(base);  // [claude-9b]
  __imp__CG_CalcViewValues(ctx, base);
  SmoothView(base);
}

// [claude-9b] 2026-10-27 0x824345D8: the bob's phase (cg +5804, from the bob cycle byte) and speed (cg +5808, from the
// velocity), called by the view setup 0x824505E8 before the view and the gun use them. Both changed only in movement
// steps; drawn between the last step's values as the camera is (StepBack), the phase held back by the part of the
// step's advance not drawn yet.
REX_EXTERN(__imp__sub_824345D8);
REX_HOOK_RAW(sub_824345D8) {
  __imp__sub_824345D8(ctx, base);
  if (!REXCVAR_GET(cod3_move_bob_smooth)) return;
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  const HzSteps* h = nullptr;
  const float back = StepBack(base, client, &h);
  if (back <= 0.0f || !h) return;
  const uint32_t cg = kCgBase + client * kCgSize;
  if ((g::ReadU32(base, h->ps + kPsBobCycle) & 0xFFu) == h->vals_to.bob && h->bob_delta > 0.0f) {
    const float scale = g::ReadF32(base, kBobPhaseScale);
    const float phase = g::ReadF32(base, cg + kCgBobPhase) - scale * h->bob_delta * back;
    if (std::isfinite(phase)) g::WriteF32(base, cg + kCgBobPhase, phase);
  }
  // The speed only while the playerState's is still the step's (a vehicle, a script or a hit can change the velocity
  // without moving the origin: claude-9b-think's check).
  if (!(g::ReadU32(base, h->ps + P.ps.pm_flags) & kPmFlagsBobSpeedOwn) &&
      std::fabs(ReadStepVals(base, h->ps).speed - h->vals_to.speed) < 1e-3f) {
    const float speed = h->vals_to.speed - (h->vals_to.speed - h->vals_from.speed) * back;
    if (std::isfinite(speed) && speed >= 0.0f) g::WriteF32(base, cg + kCgBobSpeed, speed);
  }
}

// ===========================================================================
// Client side: the sprint button
// ===========================================================================

// +sprint (key, time) / -sprint: empty in the retail campaign.
REX_EXTERN(__imp__IN_SprintDown);
REX_HOOK_RAW(IN_SprintDown) {
  const int key = static_cast<int32_t>(ctx.r3.u32), time = static_cast<int32_t>(ctx.r4.u32);
  Carry(ctx, base, g_input.OnSprintDown(key, time, mv::SprintButtonMode(), mv::SprintSetting()));
}

REX_EXTERN(__imp__IN_SprintUp);
REX_HOOK_RAW(IN_SprintUp) {
  const int key = static_cast<int32_t>(ctx.r3.u32), time = static_cast<int32_t>(ctx.r4.u32);
  Carry(ctx, base, g_input.OnSprintUp(key, time, mv::SprintButtonMode(), mv::SprintSetting()));
}

// +sprintbreath / -sprintbreath (World at War's Shift): hold breath while
// aiming, sprint otherwise; empty in the retail campaign. [keybinds] Looking
// through the binoculars counts as aiming: hold breath steadies them (as the
// pad's L3 does once they are up).
REX_EXTERN(__imp__IN_SprintbreathDown);
REX_HOOK_RAW(IN_SprintbreathDown) {
  const int key = static_cast<int32_t>(ctx.r3.u32), time = static_cast<int32_t>(ctx.r4.u32);
  const bool steady = Aiming() || (g_view.valid && g_view.in_binoculars);
  Carry(ctx, base, g_input.OnSprintBreathDown(key, time, mv::SprintButtonMode(), mv::SprintSetting(), steady));
}

REX_EXTERN(__imp__IN_SprintbreathUp);
REX_HOOK_RAW(IN_SprintbreathUp) {
  const int key = static_cast<int32_t>(ctx.r3.u32), time = static_cast<int32_t>(ctx.r4.u32);
  Carry(ctx, base, g_input.OnSprintBreathUp(key, time, mv::SprintButtonMode(), mv::SprintSetting()));
}

// +binoculars / -binoculars: the pad's L3 in layouts A, B and D.
REX_EXTERN(__imp__IN_BinocularsDown);
REX_HOOK_RAW(IN_BinocularsDown) {
  const int key = static_cast<int32_t>(ctx.r3.u32), time = static_cast<int32_t>(ctx.r4.u32);
  bool handled = false;
  const mv::KeyAction a =
      g_input.OnBinocularsDown(key, time, mv::SprintButtonMode(), mv::SprintSetting(), g_view, &handled);
  if (!handled) {
    __imp__IN_BinocularsDown(ctx, base);
    return;
  }
  Carry(ctx, base, a);
}

REX_EXTERN(__imp__IN_BinocularsUp);
REX_HOOK_RAW(IN_BinocularsUp) {
  const int key = static_cast<int32_t>(ctx.r3.u32), time = static_cast<int32_t>(ctx.r4.u32);
  bool handled = false;
  const mv::KeyAction a = g_input.OnBinocularsUp(key, time, mv::SprintButtonMode(), mv::SprintSetting(), &handled);
  if (!handled) {
    __imp__IN_BinocularsUp(ctx, base);
    return;
  }
  Carry(ctx, base, a);
}

// CL_FinishMove(usercmd_t* cmd): the last step of making a usercmd.
REX_EXTERN(__imp__CL_FinishMove);
REX_HOOK_RAW(CL_FinishMove) {
  const uint32_t cmd = ctx.r3.u32;
  __imp__CL_FinishMove(ctx, base);
  if (!ValidGuest(cmd)) return;
  mv::CommandView view;
  view.forwardmove = ReadS8(base, cmd + P.cmd.forwardmove);
  view.buttons = g::ReadU32(base, cmd + P.cmd.buttons);
  view.server_time = g::ReadS32(base, cmd + P.cmd.server_time);
  // A new level restarts the clock: let go of the latch.
  static int last_time = 0;
  if (view.server_time < last_time) {
    Carry(ctx, base, g_input.Reset(0));
    g_view = mv::PlayerView{};
  }
  last_time = view.server_time;
  bool clear = false;
  Carry(ctx, base, g_input.OnCommand(view, g_view, mv::SprintButtonMode(), mv::SprintSetting(), NowMs(), &clear));
  if (clear) g::WriteU32(base, cmd + P.cmd.buttons, view.buttons & ~mv::button::kSprint);
}
