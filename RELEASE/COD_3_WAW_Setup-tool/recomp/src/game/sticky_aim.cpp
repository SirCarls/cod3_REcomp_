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
#include "game/protected_dvars.h"
#endif

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
  if (clock) {
    pulls = ClockPulls(ps);
  } else {
    now = cod3::guest::ReadS32(base, cmd + kCmdServerTime);
    before = cod3::guest::ReadS32(base, oldcmd + kCmdServerTime);
    pulls = int(std::clamp<int64_t>(PullIndex(now) - PullIndex(before), 0, kMaxPullsAtOnce));
  }
  const double share = PullShare();
  float turned = 0.0f;
  for (int i = 0; i < pulls; ++i) {
    const View view_before = ReadView(base, ps);
    ctx.r3.u64 = ps;
    ctx.r4.u64 = cmd;
    ctx.r5.u64 = oldcmd;
    __imp__sub_8248D138(ctx, base);
    turned += KeepShare(base, ps, view_before, share);
  }
  if (REXCVAR_GET(cod3_debug_sticky_aim)) Count(ps, before, now, pulls, clock, turned);
  LogStats();
}
