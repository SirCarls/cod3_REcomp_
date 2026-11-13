// cod2 - sprint in the campaign, by United Offensive's rules. [f2-menus]
//
// Owner 2026-11-10 15:44: sprint in "Call of Duty 2, Big Red One and the classic all of these"; 15:51: "Please also
// look at the PC files including UO". Call of Duty 2 (PC and Xbox 360) has no sprint: no button, no stamina, no gun
// pose. United Offensive has one, and this follows it, read from its multiplayer game module
// (dev/coduo-decomp/game_mp, uo_game_mp_x86.dll 1.51):
//   - the sprint flag (0x2000D5B0): the sprint button held, fatigue above 0 (above 0.25 to start), not prone,
//     crouched or aiming, moving (forward, back or sideways), not firing;
//   - fatigue (0x2000C1E0; 1 = fresh): minus 1/3 a second while sprinting (3 s from full); from 1 s after the last
//     sprint, and only with the button let go, plus 1/7 a second (7 s to full); bg_nofatigue turns it off;
//   - speed (PM_CmdScale): times the playerState's sprint scale, which ClientSpawn sets to 1.6.
// United Offensive also needs the ground under the player to start, lowers the gun (its weapons' sprintMoveF / R / U
// -14, -10, -14) and breathes hard below half: not here yet.
//
// Where: PM_CmdScale_Walk 0x82465338 (r3 pmove_t, r4 the usercmd; answers in f1 the scale PM_WalkMove 0x82468E48
// multiplies the wished direction by). The hook multiplies that answer. Pmove has two callers, the server's client
// think and the client's prediction, and both come through here, so they agree.
//
// Read from the game's code (dev/waw-decomp/cod2_360/sp/decomp/82460000.c): pmove_t +0 the playerState, +4 the
// usercmd (+0 serverTime, +4 buttons, +0x18 forwardmove, +0x19 rightmove, signed bytes); playerState +4 pm_type,
// +0xC pm_flags (0x100: the slow walk PM_UpdatePlayerWalkingFlag 0x824637B8 sets while aiming), +0x4C the lean
// (a float), +0xD0 the view height the stance wants (0x28 crouched, 0xB prone). Button 1 is fire (CL_CmdButtons
// 0x8242AA20).
//
// Fatigue is kept here, in the game's own clock (the usercmds' serverTime, so a pause doesn't refill it).
//
// Not there in this first version: the gun stays up, no breathing sound, no HUD bar, nothing in cod2mp, and the keys
// are fixed (Left Shift, or the left stick pressed in), which the game also still sees as whatever they are bound to.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#if __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#define COD2_SPRINT_HAVE_SDL 1
#else
#define COD2_SPRINT_HAVE_SDL 0
#endif

REXCVAR_DEFINE_BOOL(cod2_sprint, true, "cod2/Game",
                    "Sprint in the campaign: hold Left Shift (or press the left stick in) while running forward");
REXCVAR_DEFINE_DOUBLE(cod2_sprint_speed, 1.6, "cod2/Game",
                      "Sprint: how many times the running speed (1.6: United Offensive's)")
    .range(1.0, 3.0);
REXCVAR_DEFINE_DOUBLE(cod2_sprint_time, 3.0, "cod2/Game",
                      "Sprint: seconds of sprint from fresh (3: United Offensive's; 0: no limit)")
    .range(0.0, 60.0);
REXCVAR_DEFINE_DOUBLE(cod2_sprint_recover, 7.0, "cod2/Game",
                      "Sprint: seconds to be fresh again, from a second after the sprint (7: United Offensive's)")
    .range(0.1, 60.0);
REXCVAR_DEFINE_BOOL(cod2_debug_sprint_hold, false, "cod2/Debug", "Tests: as if the sprint key were held");
REXCVAR_DEFINE_INT32(cod2_debug_sprint, 0, "cod2/Debug",
                     "Tests: every this many seconds, log the sprint's state and the speed scale (0: off)")
    .range(0, 600);

#if !defined(COD2_MP)

namespace {

constexpr uint32_t kPsPmType = 0x4;
constexpr uint32_t kPsPmFlags = 0xC;
constexpr uint32_t kPsVelocity = 0x20;
constexpr uint32_t kPsLean = 0x4C;
constexpr uint32_t kPsViewHeightTarget = 0xD0;
constexpr uint32_t kPmfWalking = 0x100;
constexpr uint32_t kCmdServerTime = 0x0;
constexpr uint32_t kCmdButtons = 0x4;
constexpr uint32_t kCmdForward = 0x18;
constexpr uint32_t kButtonFire = 0x1;
constexpr float kStartFatigue = 0.25f;  // United Offensive: a new sprint needs more than this
constexpr int kRecoverWaitMs = 1000;    // United Offensive: recovery starts this long after the last sprint
constexpr int kMaxAdvanceMs = 200;      // a longer gap between commands (a load, a pause) doesn't count

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

float LoadF32(const uint8_t* base, uint32_t address) {
  const uint32_t v = Load32(base, address);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// The key, read at most every 4 ms (the hook runs several times a frame).
bool KeyHeld() {
  if (REXCVAR_GET(cod2_debug_sprint_hold)) return true;
#if COD2_SPRINT_HAVE_SDL
  using Clock = std::chrono::steady_clock;
  static std::atomic<int64_t> next_ns{0};
  static std::atomic<bool> held{false};
  const int64_t now = Clock::now().time_since_epoch().count();
  if (now < next_ns.load(std::memory_order_relaxed)) return held.load(std::memory_order_relaxed);
  next_ns.store(now + 4'000'000, std::memory_order_relaxed);
  bool down = false;
  int key_count = 0;
  if (const bool* keys = SDL_GetKeyboardState(&key_count); keys && SDL_SCANCODE_LSHIFT < key_count)
    down = keys[SDL_SCANCODE_LSHIFT];
  if (!down) {
    int pad_count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&pad_count)) {
      for (int i = 0; i < pad_count && !down; ++i)
        if (SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]))
          down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK);
      SDL_free(ids);
    }
  }
  held.store(down, std::memory_order_relaxed);
  return down;
#else
  return false;
#endif
}

struct State {
  int last_time = 0;         // the newest serverTime seen
  int last_sprint_time = 0;  // the last command that sprinted
  float fatigue = 1.0f;      // 1 fresh, 0 spent (United Offensive's playerState field)
  bool sprinting = false;
  int64_t next_log_ns = 0;
};
State g;

// Whether this movement step sprints. The state only moves on for a command newer than any before (the prediction
// runs older commands again; they get the newest answer).
bool Decide(const uint8_t* base, uint32_t pm, uint32_t cmd) {
  const uint32_t ps = Load32(base, pm);
  if (ps == 0) return false;
  const int time = static_cast<int>(Load32(base, cmd + kCmdServerTime));
  const int forward = static_cast<int8_t>(base[cmd + kCmdForward]);
  const int right = static_cast<int8_t>(base[cmd + kCmdForward + 1]);
  const uint32_t buttons = Load32(base, cmd + kCmdButtons);
  const int height = static_cast<int>(Load32(base, ps + kPsViewHeightTarget));
  const bool able = Load32(base, ps + kPsPmType) == 0 && (Load32(base, ps + kPsPmFlags) & kPmfWalking) == 0 &&
                    LoadF32(base, ps + kPsLean) == 0.0f && height != 0x28 && height != 0xB &&
                    (forward != 0 || right != 0) && (buttons & kButtonFire) == 0;
  const bool key = KeyHeld();
  const double tank = REXCVAR_GET(cod2_sprint_time);

  int dt = time - g.last_time;
  if (dt < -2000) {  // the game's clock went back: a new level
    g = State{};
    g.last_time = time;
    dt = 0;
  }
  if (dt > 0) {
    g.last_time = time;
    if (dt > kMaxAdvanceMs) dt = 0;
    if (g.sprinting) {
      g.last_sprint_time = time;
      if (tank > 0.0) g.fatigue = std::max(0.0f, g.fatigue - static_cast<float>(dt * 0.001 / tank));
    } else if (!key && time >= g.last_sprint_time + kRecoverWaitMs) {
      const double recover = std::max(0.1, REXCVAR_GET(cod2_sprint_recover));
      g.fatigue = std::min(1.0f, g.fatigue + static_cast<float>(dt * 0.001 / recover));
    }
    if (tank <= 0.0) g.fatigue = 1.0f;
    g.sprinting = able && key && g.fatigue > 0.0f && (g.sprinting || g.fatigue > kStartFatigue);
  }
  return g.sprinting && able;
}

}  // namespace

REX_EXTERN(__imp__sub_82465338);  // PM_CmdScale_Walk
REX_HOOK_RAW(sub_82465338) {
  const uint32_t pm = ctx.r3.u32;
  const uint32_t cmd = ctx.r4.u32;
  __imp__sub_82465338(ctx, base);
  if (!REXCVAR_GET(cod2_sprint) || pm == 0 || cmd == 0) return;
  const bool sprint = Decide(base, pm, cmd);
  const double before = ctx.f1.f64;
  if (sprint) ctx.f1.f64 = static_cast<double>(static_cast<float>(before * REXCVAR_GET(cod2_sprint_speed)));
  if (const int every = REXCVAR_GET(cod2_debug_sprint); every > 0) {
    const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (now >= g.next_log_ns) {
      g.next_log_ns = now + static_cast<int64_t>(every) * 1'000'000'000;
      const uint32_t ps = Load32(base, pm);
      const float vx = LoadF32(base, ps + kPsVelocity), vy = LoadF32(base, ps + kPsVelocity + 4);
      REXLOG_INFO("[sprint] time {} sprinting {} fatigue {:.3f} scale {:.2f} -> {:.2f} speed {:.1f} forward {} "
                  "flags {:#x} height {}",
                  g.last_time, sprint ? 1 : 0, g.fatigue, before, ctx.f1.f64, std::sqrt(vx * vx + vy * vy),
                  static_cast<int>(static_cast<int8_t>(base[cmd + kCmdForward])), Load32(base, ps + kPsPmFlags),
                  static_cast<int>(Load32(base, ps + kPsViewHeightTarget)));
    }
  }
}

#endif  // !COD2_MP
