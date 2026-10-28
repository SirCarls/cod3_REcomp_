// cod3 - Sticky Aim at a fixed 60 Hz and the match's rules for it (see
// sticky_aim.h).

#include "game/sticky_aim.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/program.h"
#if !defined(COD3_MP)
#include <optional>

#include <imgui.h>

#include "game/aim_assist.h"
#include "game/button_prompts.h"  // [claude-9b] KeyboardMouseInUse
#include "game/protected_dvars.h"
#endif

// [claude-9b] 2026-10-27 (owner 10:13: "the ADS jutter needs fixing ASAP"; claude-9b-think FINDINGS-ADS.md 3): at 60
// pulls a second the pull came in one usercmd of about every three at ~200 fps, a whole 60 fps frame's turn at once
// and none between, so the assisted part of the view turned in steps (the weapon's sway follows the view's turn and
// shows it). Now every usercmd pulls the part of a pull its own time is worth: as much a second, spread over frames.
// [claude-9b] 2026-10-27 11:35 off by default for now: claude-9b-test's strength check (stick2) found the spread pull
// weaker above 60 fps (144 fps -10%, uncapped -24 to -38% of the turn at 60 fps), so it is reworked first.
// [claude-9b-think] 2026-10-27 UNTESTED reworked: the pull keeps its own lock in the player state (the generated code
// of 0x8248D138: +1300 locked, +1304 the target's entity number, +1308..+1316 the aim point relative to the target,
// +1320 the last aim cosine). Each call moves that aim point a fixed part of the way to the target (not scaled by any
// time) and sets the view angles to look at it (absolute; delta_angles get the change). A call that kept only part of
// its turn still moved the lock a whole step: the view trailed the aim point and lost what it trailed by when the pull
// stopped (the raise done). With cod3_move_hz the timed calls also came only with each 60 Hz movement step, each worth
// the part of 1/60 s since the usercmd before (the usercmds without a step reach it with their time put back to the
// step's start: no pull). Now the game pulls whole, 60 times a second, exactly as with this off (its lock, strength and
// turn as at 60 fps); only the view's share of each pull is spread over the next 1/60 s of usercmds.
// [claude-9b] 13:10 on by default again: claude-9b-test stick4 (the 50 ms payout below): total turn within 0.5% of the
// 60 fps reference at 60, 144 and ~320 fps, a turn in every frame of the pull, largest step 0.16-0.26 deg above 60 fps
// (was 1.5); the pull finishes ~30-50 ms later.
REXCVAR_DEFINE_BOOL(cod3_sticky_aim_smooth, true, "cod3/Game",
                    "Campaign: Sticky Aim's pulls (whole, as on the console) turn the view spread over the next "
                    "50 ms of frames instead of at once; off: the view turns in steps while it pulls above 60 fps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_debug_sticky_aim, false, "cod3",
                    "Log once a second how often Sticky Aim (the game's aim assist) ran, how many 60 Hz "
                    "pulls that was, and how far the pulls turned the view");

namespace cod3::sticky_aim {
namespace {

using guest::ReadF32;
using guest::ReadS32;
using guest::WriteF32;
using guest::WriteU32;

// The player state (playerState_t) fields the pull changes: the view angles
// (pitch, yaw, roll) and delta_angles (whole short angles). Campaign: view
// angles +172, delta_angles +88 (PM_UpdateViewAngles 0x8248DF18 sets one
// from the other; Sticky Aim 0x8248D138 writes both). cod3mp: +204 and +84
// (its Sticky Aim 0x824ABCC8, the same code).
constexpr uint32_t kViewAngles = cod3::kMultiplayer ? 204 : 172;
constexpr uint32_t kDeltaAngles = cod3::kMultiplayer ? 84 : 88;
// usercmd_t: serverTime (milliseconds) first, as in Quake III.
constexpr uint32_t kCmdServerTime = 0;

// The console's rhythm: the game ran at most 60 frames a second on the Xbox
// 360 (its display's vblank), one usercmd and one pull per frame.
constexpr int64_t kPullsPerSecond = 60;
// A long hitch catches up at most this many pulls (a quarter of a second at
// 60 Hz would be 15; more than a few in one frame would jerk the view).
constexpr int64_t kMaxPullsAtOnce = 4;

int64_t PullIndex(int64_t ms) {
  // floor(ms * 60 / 1000), also for negative times.
  const int64_t n = ms * kPullsPerSecond;
  return n >= 0 ? n / 1000 : -((-n + 999) / 1000);
}

float AngleDelta(float to, float from) {
  const float d = std::remainder(to - from, 360.0f);
  return std::isfinite(d) ? d : 0.0f;
}

// The client-only call (CG's sub_82449C88 hands the same usercmd as both, so
// it has no time of its own): paced by this machine's clock instead, per
// player state. Nothing authoritative runs there.
struct ClockPace {
  uint32_t ps = 0;
  int64_t next_ns = 0;
};
std::array<ClockPace, 8> g_clock_pace{};
std::mutex g_clock_mutex;

int ClockPulls(uint32_t ps) {
  using namespace std::chrono;
  constexpr int64_t kStepNs = 1'000'000'000 / kPullsPerSecond;
  const int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  std::lock_guard lock(g_clock_mutex);
  ClockPace* slot = nullptr;
  for (auto& p : g_clock_pace)
    if (p.ps == ps) slot = &p;
  if (!slot) {
    slot = &*std::min_element(g_clock_pace.begin(), g_clock_pace.end(),
                              [](const ClockPace& a, const ClockPace& b) { return a.next_ns < b.next_ns; });
    *slot = {ps, now};
  }
  if (now < slot->next_ns) return 0;
  const int64_t pulls = std::min<int64_t>(1 + (now - slot->next_ns) / kStepNs, kMaxPullsAtOnce);
  slot->next_ns = std::max(slot->next_ns + pulls * kStepNs, now - kStepNs);
  return int(pulls);
}

// What happened over the last second (cod3_debug_sticky_aim), per player
// state (the server's copy and the client's prediction are different ones).
// The client's prediction runs the newest usercmds again every frame, so only
// a usercmd newer than the last one seen for that player state is counted:
// "usercmds" is how often the game alone would have pulled, "pulls" how often
// it pulls now.
struct PsStats {
  uint32_t ps = 0;
  int64_t last_time = INT64_MIN;
  uint32_t usercmds = 0, pulls = 0;
  int64_t ms = 0;
};
std::array<PsStats, 4> g_ps_stats{};
uint32_t g_clock_calls = 0;
float g_turned = 0.0f;  // degrees the pulls turned the view, summed
std::mutex g_stats_mutex;

void Count(uint32_t ps, int64_t before, int64_t now, int pulls, bool clock, float turned) {
  std::lock_guard lock(g_stats_mutex);
  g_turned += turned;
  if (clock) {
    ++g_clock_calls;
    return;
  }
  PsStats* slot = nullptr;
  for (auto& p : g_ps_stats)
    if (p.ps == ps) slot = &p;
  if (!slot) {
    slot = &*std::min_element(g_ps_stats.begin(), g_ps_stats.end(),
                              [](const PsStats& a, const PsStats& b) { return a.usercmds < b.usercmds; });
    *slot = PsStats{};
    slot->ps = ps;
  }
  if (now <= slot->last_time) return;  // prediction running an old usercmd again
  slot->last_time = now;
  slot->usercmds += 1;
  slot->pulls += uint32_t(pulls);
  slot->ms += std::clamp<int64_t>(now - before, 0, 1000);
}

void LogStats() {
  if (!REXCVAR_GET(cod3_debug_sticky_aim)) return;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  const auto now = Clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(1);
  std::string line;
  {
    std::lock_guard lock(g_stats_mutex);
    for (auto& p : g_ps_stats) {
      if (p.ps == 0 || p.ms <= 0) continue;
      line += fmt::format(" [player state {:08X}: {} usercmds, {} pulls in {} ms of usercmds = {:.1f} pulls/s]", p.ps,
                          p.usercmds, p.pulls, p.ms, p.pulls * 1000.0 / double(p.ms));
      p.usercmds = p.pulls = 0;
      p.ms = 0;
    }
    line += fmt::format(" {} calls without a time of their own; the pulls turned the view {:.3f} deg; share "
                        "kept {:.2f}",
                        g_clock_calls, g_turned, PullShare());
    g_clock_calls = 0;
    g_turned = 0.0f;
  }
  REXLOG_INFO("cod3: sticky aim debug:{}", line);
}

struct View {
  std::array<float, 3> angles;
  std::array<int32_t, 3> delta;
};

View ReadView(uint8_t* base, uint32_t ps) {
  View v;
  for (int i = 0; i < 3; ++i) {
    v.angles[i] = ReadF32(base, ps + kViewAngles + 4 * i);
    v.delta[i] = ReadS32(base, ps + kDeltaAngles + 4 * i);
  }
  return v;
}

// Keeps `share` of what one pull did to the view (0 undoes it, 1 keeps it).
float KeepShare(uint8_t* base, uint32_t ps, const View& before, double share) {
  const View after = ReadView(base, ps);
  float turned = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float d = AngleDelta(after.angles[i], before.angles[i]);
    turned += std::abs(d) * float(share);
    if (share >= 1.0) continue;
    const float angle = before.angles[i] + float(d * share);
    // delta_angles are short angles kept in an int; the game adds to them
    // without masking, so the difference is read as a short.
    const int16_t dd = int16_t(uint16_t(after.delta[i] - before.delta[i]));
    const int32_t delta = before.delta[i] + int32_t(std::lround(dd * share));
    if (std::isfinite(angle)) WriteF32(base, ps + kViewAngles + 4 * i, angle);
    WriteU32(base, ps + kDeltaAngles + 4 * i, uint32_t(delta));
  }
  return turned;
}

// [claude-9b-think] 2026-10-27 UNTESTED cod3_sticky_aim_smooth: a pull's view turn spread over the next 1/60 s, per
// player state. The turn is kept in delta_angles' units (PM_UpdateViewAngles makes the view angles from cmd.angles +
// delta_angles on every usercmd, so the units are what lasts); the view angles get the same turn for the usercmd in
// progress. Parts of a unit not applied yet stay pending (no rounding loss).
constexpr double kUnitDegrees = 360.0 / 65536.0;
constexpr int64_t kSpreadNs = 1'000'000'000 / kPullsPerSecond;
// [claude-9b] 2026-10-27 (claude-9b-test stick3: strength within 1.7% of the 60 fps reference at any fps, but the
// game's aim point moves only every 50 ms, every third pull, so a turn spread over 1/60 s and flushed at the next pull
// still left 2 of 3 frames still at 144 fps and 2 of 3 uncapped). The game sets the view to look at its aim point on
// every pull (absolute), so what a pull asks for is the whole way from the view to that point: the pending turn is
// replaced by it (not added to), never flushed, and paid out over the next 50 ms. The view reaches where the game
// wants it within 50 ms of each change and turns in every frame while the aim point moves.
constexpr int64_t kWindowNs = 3 * kSpreadNs;
struct Spread {
  uint32_t ps = 0;
  double units[3] = {};  // the turn not applied yet, in delta_angles' units
  int64_t from_ns = 0;   // the spread's schedule: linear from here...
  int64_t until_ns = 0;  // ...to here
  int64_t call_ns = 0;   // this player state's last call (a usercmd's length)
};
std::array<Spread, 4> g_spread{};  // the game thread runs Pmove and the client game's call

int64_t NowNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

Spread& SpreadFor(uint32_t ps) {
  for (auto& s : g_spread)
    if (s.ps == ps) return s;
  for (auto& s : g_spread)
    if (s.ps == 0) {
      s.ps = ps;
      return s;
    }
  g_spread[0] = Spread{};  // more player states than slots: start again
  g_spread[0].ps = ps;
  return g_spread[0];
}

// Applies the part of the pending turn due by `now` (all of it with `flush`): delta_angles get whole units, the view
// angles the same turn. Returns the degrees it turned. [claude-9b]
float ApplySpread(uint8_t* base, uint32_t ps, Spread& s, int64_t now, bool flush) {
  double part = 1.0;
  if (!flush && s.until_ns > s.from_ns)
    part = std::clamp(double(now - s.from_ns) / double(s.until_ns - s.from_ns), 0.0, 1.0);
  if (part <= 0.0) return 0.0f;
  float turned = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const int32_t units = int32_t(part >= 1.0 ? std::lround(s.units[i]) : std::lround(s.units[i] * part));
    if (units == 0) continue;
    s.units[i] -= double(units);
    turned += std::abs(float(double(units) * kUnitDegrees));
    const int32_t delta = ReadS32(base, ps + kDeltaAngles + 4 * i) + units;
    WriteU32(base, ps + kDeltaAngles + 4 * i, uint32_t(delta));
    const float angle = ReadF32(base, ps + kViewAngles + 4 * i) + float(double(units) * kUnitDegrees);
    if (std::isfinite(angle)) WriteF32(base, ps + kViewAngles + 4 * i, angle);
  }
  // The rest of the schedule starts here.
  s.from_ns = std::max(s.from_ns, std::min(now, s.until_ns));
  return turned;
}

// After a whole pull: its turn (times `share`) undone and made the pending turn, due over the kWindowNs from the start
// of the usercmd in progress (`span_ns` long so far). Returns 0: the turn is counted as ApplySpread pays it out.
float DeferPull(uint8_t* base, uint32_t ps, const View& before, double share, Spread& s, int64_t now, int64_t span_ns) {
  const View after = ReadView(base, ps);
  int16_t dd[3];
  bool moved = false;
  for (int i = 0; i < 3; ++i) {
    dd[i] = int16_t(uint16_t(after.delta[i] - before.delta[i]));
    moved |= dd[i] != 0;
  }
  // [claude-9b] A pull that left the view alone (no lock, nothing to do) keeps what is pending; one that aimed sets
  // the whole remaining way (it aims from the view as it is, the pending part not paid out yet).
  if (!moved) return 0.0f;
  for (int i = 0; i < 3; ++i) {
    s.units[i] = double(dd[i]) * share;
    WriteF32(base, ps + kViewAngles + 4 * i, before.angles[i]);
    WriteU32(base, ps + kDeltaAngles + 4 * i, uint32_t(before.delta[i]));
  }
  s.from_ns = now - std::clamp<int64_t>(span_ns, 0, kWindowNs);
  s.until_ns = s.from_ns + kWindowNs;
  return 0.0f;  // the turn is counted as it is paid out (ApplySpread)
}

}  // namespace

double PullShare() {
#if defined(COD3_MP)
  // cod3mp: the game's own until the protected category is built there.
  return 1.0;
#else
  namespace pd = protected_dvars;
  if (pd::Get(pd::Id::kStickyAim) == 0) return 0.0;
  return std::clamp(pd::Get(pd::Id::kAimSlowdown), 0, 100) / 100.0;
#endif
}

#if !defined(COD3_MP)
namespace {

void HelpLine(const char* text) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
}

}  // namespace

void DrawF2Section() {
  namespace pd = protected_dvars;
  const int rule = pd::Get(pd::Id::kStickyAim);
  const int strength = pd::Get(pd::Id::kAimSlowdown);

  // What applies now: it is what a player looks for here.
  const std::optional<bool> own = AimAssistEnabled();
  std::string now;
  if (rule == 0)
    now = "Now: off for everyone (the match rule, sticky_aim 0).";
  else if (own && !*own)
    now = "Now: off (your own choice above).";
  else if (strength == 0)
    now = "Now: no pull (the match rule, aim_slowdown 0).";
  else
    now = fmt::format("Now: {}% of the game's own pull, 60 times a second at any frame rate.", strength);
  ImGui::TextUnformatted(now.c_str());
  // The match's rule itself is the host's (owner 2026-10-20 20:42): F2 >
  // Game > Server, shown only to the host (ui/server_tab.h).
  HelpLine(pd::IsLocked() ? "This match's host sets the aim assist rule."
                          : "The aim assist rule for a match you host is in the Server tab.");
}
#endif

}  // namespace cod3::sticky_aim

// The game's Sticky Aim (campaign 0x8248D138, cod3mp 0x824ABCC8):
// (playerState_t* ps, usercmd_t* cmd, usercmd_t* oldcmd), from
// PM_UpdateViewAngles once per usercmd, on the server and in the client's
// prediction alike.
REX_EXTERN(__imp__sub_8248D138);
REX_HOOK_RAW(sub_8248D138) {
  using namespace cod3::sticky_aim;
  const uint32_t ps = ctx.r3.u32, cmd = ctx.r4.u32, oldcmd = ctx.r5.u32;
  int pulls;
  int64_t now = 0, before = 0;
  const bool clock = cmd == oldcmd;
  // [claude-9b-think] 2026-10-27 UNTESTED the game's whole pulls, 60 a second, with or without cod3_sticky_aim_smooth
  // (which only spreads their view turn). The campaign only: cod3mp's prediction runs usercmds again.
  const bool smooth = !cod3::kMultiplayer && REXCVAR_GET(cod3_sticky_aim_smooth);
  if (clock) {
    pulls = ClockPulls(ps);
  } else {
    now = cod3::guest::ReadS32(base, cmd + kCmdServerTime);
    before = cod3::guest::ReadS32(base, oldcmd + kCmdServerTime);
    pulls = int(std::clamp<int64_t>(PullIndex(now) - PullIndex(before), 0, kMaxPullsAtOnce));
  }
  // [claude-9b] 2026-10-27 (owner 12:39: "The usage of aim assist on kbm cannot work."): while the keyboard and mouse
  // are in use (mnk_mode on and a key or mouse press since the last controller input, as the prompts and the rumble
  // follow), the game's pull is undone: no aim assist for the mouse, also when it aims through the emulated right
  // stick (mnk_mouse). A controller in use gets it as before.
#if !defined(COD3_MP)
  const bool kbm = cod3::prompts::KeyboardMouseInUse();
#else
  // cod3mp: the host pulls for every player, and its own keyboard says nothing of the others' (to do: a client's
  // device has to reach the host).
  const bool kbm = false;
#endif
  const double share = kbm ? 0.0 : PullShare();
  float turned = 0.0f;
  Spread* spread = smooth ? &SpreadFor(ps) : nullptr;
  const int64_t now_ns = spread ? NowNs() : 0;
  int64_t span_ns = 0;  // this usercmd's length on this machine's clock, so far as this player state's calls tell
  if (spread) {
    span_ns = spread->call_ns ? now_ns - spread->call_ns : kSpreadNs;
    spread->call_ns = now_ns;
    // The part of the pending turn due now. [claude-9b] Not flushed before a pull: the game aims absolutely, and the
    // pull's result replaces what is pending.
    turned += ApplySpread(base, ps, *spread, now_ns, false);
  }
  for (int i = 0; i < pulls; ++i) {
    const View view_before = ReadView(base, ps);
    ctx.r3.u64 = ps;
    ctx.r4.u64 = cmd;
    ctx.r5.u64 = oldcmd;
    __imp__sub_8248D138(ctx, base);
    turned += spread ? DeferPull(base, ps, view_before, share, *spread, now_ns, span_ns)
                     : KeepShare(base, ps, view_before, share);
  }
  // This usercmd's part of the new pull's turn.
  if (spread && pulls > 0) turned += ApplySpread(base, ps, *spread, now_ns, false);
  if (REXCVAR_GET(cod3_debug_sticky_aim)) Count(ps, before, now, pulls, clock, turned);
  LogStats();
}
