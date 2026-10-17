// cod3 - the campaign's movement hooks: sprint, the CoD4 style's parts, and
// the sprint button (game/movement/rules.h, sprint_input.h, mantle.h).
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
//   PM_CheckJump 0x82478100   a jump at a ledge becomes a mantle; CoD4 sprint
//                             keeps its stamina when jumping
//   PM_WalkMove / PM_AirMove  the mantle's climb instead of the move
//   PM_ClipVelocity 0x8246B138  the step move's two clips become CoD4's
//                             projection on steep slopes (bounces)
// With alternate rules off (sprint off, CoD3 style) every hook calls the
// game's own code unchanged: the console's movement, number for number.
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

#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/movement/mantle.h"
#include "game/movement/program_current.h"
#include "game/movement/rules.h"
#include "game/movement/settings.h"
#include "game/movement/sprint_input.h"
#include "game/protected_dvars.h"

// Pmove, the game's own (hooked below); RunCod4Steps calls it too.
REX_EXTERN(__imp__sub_82496978);

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
};
thread_local MoveContext g_move;

mv::Mantles g_mantles;

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

// --- the game's player trace, for the mantle --------------------------------------
class GameTracer final : public mv::Tracer {
 public:
  GameTracer(PPCContext& ctx, uint8_t* base, uint32_t pm, uint32_t ps) : ctx_(ctx), base_(base), pm_(pm), ps_(ps) {}

  bool Trace(const float start[3], const float end[3], mv::TraceHit* out) override {
    g::Call c(ctx_, base_);
    // Our data above the call's stack pointer: the trace, two vec4s and the
    // collision context {vtable, entity to skip, 0, 0, 0, clip mask}.
    const uint32_t data = (c.ctx().r1.u32 - 256) & ~0xFu;
    c.ctx().r1.u32 = data;
    const uint32_t trace = data, vstart = data + 160, vend = data + 176, context = data + 192;
    for (uint32_t i = 0; i < 256; i += 4) g::WriteU32(base_, data + i, 0);
    for (int i = 0; i < 3; ++i) {
      g::WriteF32(base_, vstart + 4 * i, start[i]);
      g::WriteF32(base_, vend + 4 * i, end[i]);
    }
    g::WriteU32(base_, context, P.collision_context_vtable);
    g::WriteU32(base_, context + 4, g::ReadU32(base_, ps_ + P.ps.pass_entity));
    g::WriteU32(base_, context + 20, g::ReadU32(base_, pm_ + P.pmove.tracemask));
    c.Invoke(P.player_trace, {trace, vstart, pm_ + P.pmove.mins, pm_ + P.pmove.maxs, vend, context});
    out->fraction = g::ReadF32(base_, trace + P.trace.fraction);
    for (int i = 0; i < 3; ++i) {
      out->endpos[i] = g::ReadF32(base_, trace + P.trace.endpos + 4 * i);
      out->normal[i] = g::ReadF32(base_, trace + P.trace.normal + 4 * i);
    }
    out->allsolid = g::ReadU8(base_, trace + P.trace.allsolid) != 0;
    out->startsolid = g::ReadU8(base_, trace + P.trace.startsolid) != 0;
    return std::isfinite(out->fraction);
  }

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t pm_, ps_;
};

mv::MantleInput MantleInputFor(uint8_t* base, uint32_t pm, uint32_t ps) {
  mv::MantleInput in;
  in.client = ClientIndex(base, ps);
  in.server_time = g::ReadS32(base, Cmd(pm) + P.cmd.server_time);
  for (int i = 0; i < 3; ++i) in.origin[i] = g::ReadF32(base, ps + P.ps.origin + 4 * i);
  // The view's direction from the movement code's own forward vector (pml).
  const float fx = g::ReadF32(base, P.pml + P.pml_fields.forward), fy = g::ReadF32(base, P.pml + P.pml_fields.forward + 4);
  const float len = std::sqrt(fx * fx + fy * fy);
  in.forward[0] = len > 1e-3f ? fx / len : 1.0f;
  in.forward[1] = len > 1e-3f ? fy / len : 0.0f;
  in.half_width = std::fabs(g::ReadF32(base, pm + P.pmove.maxs));
  if (!(in.half_width > 1.0f && in.half_width < 64.0f)) in.half_width = 15.0f;
  in.pm_type = g::ReadS32(base, ps + P.ps.pm_type);
  in.pm_flags = g::ReadU32(base, ps + P.ps.pm_flags);
  in.flags2 = g::ReadU32(base, ps + P.ps.flags2);
  return in;
}

// Starts a climb when the player is at a ledge; the jump isn't made then.
// `from_ground`: the jump from the ground (PM_CheckJump), whose every miss the
// debug log tells; a rising jump looks again every 100 ms and tells only
// misses at a wall (the forward trace hit something).
bool TryMantle(PPCContext& ctx, uint8_t* base, uint32_t pm, uint32_t ps, bool from_ground) {
  GameTracer tracer(ctx, base, pm, ps);
  const mv::MantleInput in = MantleInputFor(base, pm, ps);
  if (!g_mantles.TryStart(in, tracer)) {
    const std::string_view why = g_mantles.why_not();
    const mv::TraceHit& w = g_mantles.last_wall();
    if (mv::DebugMovement() && !why.empty() && (from_ground || w.fraction < 1.0f)) {
      REXLOG_INFO("cod3: mantle: none at t={} ({}; wall trace {:.2f} normal ({:.2f} {:.2f} {:.2f}), facing ({:.2f} "
                  "{:.2f}), top {:.1f} above the feet)",
                  in.server_time, why, w.fraction, w.normal[0], w.normal[1], w.normal[2], in.forward[0],
                  in.forward[1], g_mantles.last_height());
    }
    return false;
  }
  // No landing slowdown after it, jump held so it doesn't jump at the top.
  uint32_t flags = g::ReadU32(base, ps + P.ps.pm_flags);
  flags = (flags & ~(mv::pmf::kJumping | mv::pmf::kSprinting)) | mv::pmf::kJumpHeld;
  g::WriteU32(base, ps + P.ps.pm_flags, flags);
  g::WriteU32(base, ps + P.ps.jump_time, static_cast<uint32_t>(in.server_time));
  if (mv::DebugMovement())
    REXLOG_INFO("cod3: mantle starts at t={} from ({:.1f} {:.1f} {:.1f})", in.server_time, in.origin[0],
                in.origin[1], in.origin[2]);
  return true;
}

// While climbing: the climb's position instead of the move. True when it ran.
bool StepMantle(uint8_t* base, uint32_t pm, uint32_t ps) {
  const mv::MantleInput in = MantleInputFor(base, pm, ps);
  if (!g_mantles.Active(in.client)) return false;
  float at[3];
  if (!g_mantles.Step(in, at)) return false;
  for (int i = 0; i < 3; ++i) {
    g::WriteF32(base, ps + P.ps.origin + 4 * i, at[i]);
    g::WriteF32(base, ps + P.ps.velocity + 4 * i, 0.0f);
  }
  if (!g_mantles.Active(in.client) && mv::DebugMovement())
    REXLOG_INFO("cod3: mantle done at t={} on ({:.1f} {:.1f} {:.1f})", in.server_time, at[0], at[1], at[2]);
  return true;
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
  g_move.snap = true;
  __imp__sub_82496978(ctx, base);  // no steps: its loop runs none
  g_move.snap = false;
  g::WriteU32(base, pm + P.pmove.pmove_fixed, fixed);
  g::WriteU32(base, pm + P.pmove.pmove_msec, step_msec);
  if (plan.steps == 0) UpdateViewAngles(ctx, base, pm, ps, final_time - command_time);
  g::WriteU32(base, ps + P.ps.command_time, static_cast<uint32_t>(final_time));
  g::WriteU32(base, cmd + P.cmd.server_time, static_cast<uint32_t>(final_time));
  st.pending = plan.pending_after;
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
  cod3::protected_dvars::Tick();  // the test's pretend match (game/protected_dvars.h)
  const uint32_t pm = ctx.r3.u32;
  const uint32_t ps = ValidGuest(pm) ? g::ReadU32(base, pm + P.pmove.ps) : 0;
  if (!ValidGuest(ps)) {
    __imp__sub_82496978(ctx, base);
    return;
  }
  const mv::MoveRules rules = RulesFor(ClientIndex(base, ps));
  if (rules.AllOriginal()) {
    // The console's movement: nothing of ours runs. A sprint key still held
    // from before sprint was switched off doesn't count (the console game
    // never sets the sprint bit).
    g_move.active = false;
    g_mantles.Cancel(ClientIndex(base, ps));
    const uint32_t buttons = g::ReadU32(base, Cmd(pm) + P.cmd.buttons);
    if (buttons & mv::button::kSprint) g::WriteU32(base, Cmd(pm) + P.cmd.buttons, buttons & ~mv::button::kSprint);
    __imp__sub_82496978(ctx, base);
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
  if (!rules.Mantle()) g_mantles.Cancel(ClientIndex(base, ps));
  // CoD4's speeds, relative to whatever the game set (ClientSpawn, scripts).
  if (rules.Cod4Speeds()) {
    g::WriteF32(base, ps + P.ps.scale_strafe, m.scales[5] * mv::cod4::kStrafeSpeedScale);
    g::WriteF32(base, ps + P.ps.scale_back, m.scales[6] * mv::cod4::kBackSpeedScale);
  }
  if (rules.Cod4Sprint())
    g::WriteF32(base, ps + P.ps.scale_sprint, m.scales[2] * (mv::cod4::kSprintSpeedScale / P.spawn_sprint_scale));

  const int client = ClientIndex(base, ps);
  if (rules.Cod4Steps() && P.pmove.pmove_fixed != 0 && P.update_view_angles != 0) {
    RunCod4Steps(ctx, base, pm, ps, client);
  } else {
    g_steps[client].on = false;
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
  const bool jump_pressed = ReadS8(base, Cmd(pm) + P.cmd.upmove) >= 10 &&
                            !(g::ReadU32(base, ps + P.ps.pm_flags) & mv::pmf::kJumpHeld);
  if (jump_pressed && g_move.rules.Mantle() && mv::DebugMovement())
    REXLOG_INFO("cod3: mantle: jump pressed at t={}", g::ReadS32(base, Cmd(pm) + P.cmd.server_time));
  if (g_move.rules.Mantle() && jump_pressed && TryMantle(ctx, base, pm, ps, /*from_ground=*/true)) {
    ctx.r3.u64 = 0;
    return;
  }
  const float stamina = g::ReadF32(base, ps + P.ps.stamina);
  __imp__sub_82478100(ctx, base);
  // CoD4 sprint: jumping doesn't cost stamina (the campaign takes 0.667).
  if (ctx.r3.u32 != 0 && g_move.rules.Cod4Sprint()) g::WriteF32(base, ps + P.ps.stamina, stamina);
}

// PM_WalkMove(...): the move on the ground.
REX_EXTERN(__imp__sub_82492038);
REX_HOOK_RAW(sub_82492038) {
  if (g_move.active && g_move.rules.Mantle()) {
    const uint32_t pm = g::ReadU32(base, P.pm);
    if (StepMantle(base, pm, g::ReadU32(base, pm + P.pmove.ps))) return;
  }
  __imp__sub_82492038(ctx, base);
}

// PM_AirMove(...): the move in the air. Holding jump while rising at a ledge
// mantles too (as in CoD4: jump, then keep jump held against the ledge).
REX_EXTERN(__imp__sub_82490B70);
REX_HOOK_RAW(sub_82490B70) {
  if (g_move.active && g_move.rules.Mantle()) {
    const uint32_t pm = g::ReadU32(base, P.pm);
    const uint32_t ps = g::ReadU32(base, pm + P.pmove.ps);
    if (StepMantle(base, pm, ps)) return;
    const bool rising_jump = (g::ReadU32(base, ps + P.ps.pm_flags) & mv::pmf::kJumping) &&
                             g::ReadF32(base, ps + P.ps.velocity + 8) > 0.0f;
    if (rising_jump && ReadS8(base, Cmd(pm) + P.cmd.upmove) >= 10 && TryMantle(ctx, base, pm, ps, false) &&
        StepMantle(base, pm, ps))
      return;
  }
  __imp__sub_82490B70(ctx, base);
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
    // clips; walls and walkable ground stay the game's clip.
    if (mv::IsBounceSlope(n[2]) && mv::ProjectKeepSpeed(in, n, out)) {
      for (int i = 0; i < 3; ++i) g::WriteF32(base, vout + 4 * i, out[i]);
      if (mv::DebugMovement())
        REXLOG_INFO("cod3: bounce: ({:.0f} {:.0f} {:.0f}) -> ({:.0f} {:.0f} {:.0f}) on a slope of {:.2f}", in[0],
                    in[1], in[2], out[0], out[1], out[2], n[2]);
      return;
    }
  }
  __imp__sub_8246B138(ctx, base);
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
    g_mantles.CancelAll();
  }
  last_time = view.server_time;
  bool clear = false;
  Carry(ctx, base, g_input.OnCommand(view, g_view, mv::SprintButtonMode(), mv::SprintSetting(), NowMs(), &clear));
  if (clear) g::WriteU32(base, cmd + P.cmd.buttons, view.buttons & ~mv::button::kSprint);
}
