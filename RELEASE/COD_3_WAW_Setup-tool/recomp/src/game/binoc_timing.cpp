// cod3 - [rexglue-native thread2] 2026-11-24 the player's per-step timers at any frame rate (owner rule 14, "every game
// works the same at 20, 30, 60, 144 and uncapped"; coordinator 19:27Z / 20:05Z).
//
// Found: the binoculars came up later the higher the frame rate. B held 3 s, twice at each cap (thread2/i4/binoc.txt,
// The Mace), key to zoom start: 1.14 s at 20 fps, 1.36 s at 60, 1.90 s at 144; the zoom itself 0.50 s at all three
// (sub_82470C08 steps ps+140 by the weapon's rate x pml.msec, time-correct).
//
// Why: the player's movement runs in fixed steps (movement/hooks_sp.cpp RunHzSteps: one Pmove per 1/cod3_move_hz s,
// 60 a second; the CoD4 style: Pmove's own 8 ms loop), and PmoveSingle (0x824960D0) -> 0x824959A8 runs three things
// that read Com_Frame's time block (0x825F4E08: +4 msec, +12 seconds, +16 a constant / msec) instead of the step's
// own time (pml.msec, 0x82A922B0 + 40):
//  - 0x82488B88, the binoculars' wait: with the request word 0x82A578F4 bit 2 it adds +12 to a timer (0x82A2A2E8) and
//    raises them at 0.40 s (the float at 0x82064208);
//  - 0x82476E78, which scales a per-step value by +12;
//  - 0x824949E0 -> 0x824929D0, which adds +4 to a millisecond counter (+156 of its record).
// The block holds the drawn frame's time, so each 1/60 s step added a whole frame's time: 60 x 0.050 = 3.0 s a second
// at 20 fps, 1.0 at 60, 0.42 at 144. With the 0.40 s timer that is 0.13 / 0.40 / 0.96 s, which with the rest of the
// raise (0.97 s, time-correct) is the 1.10 / 1.36 / 1.90 s measured. The probe below (cod3_debug_binoc_timer) showed it
// directly: at 20 fps three calls a frame, each adding 0.050.
//
// Fix (cod3_pmove_step_time, on): while 0x824959A8 runs (only PmoveSingle calls it; the three above are reached only
// through it), the time block says the step's own time: +4 = pml.msec, +12 = the frame's seconds x pml.msec / the
// frame's msec (so a time scale the game put in the seconds stays in them), +16 = the frame's value x frame msec /
// step msec (it is a constant over msec); put back right after. That is what the console had at 60 fps, where the
// step and the frame were the same. A step as long as the frame (cod3_move_hz 0, one move a frame) changes nothing.
//
// cod3mp (coordinator 20:18Z), the same shape at other addresses: PmoveSingle 0x824B39B8 -> 0x824B3320 (its only
// caller) -> 0x82494688 (seconds, as 0x82476E78) and 0x824B2520 -> 0x824B1910 (msec into a counter, as 0x824929D0);
// no binoculars' timer under Pmove there. The time block is 0x825FEBD8 with +8 seconds and +12 K / msec, pml
// 0x82C74880 (movement/program_mp.h). By the math: with cod3mp_server_hz 0 (the default) the host's SV_Frame runs once a
// frame with the frame's time and each player's usercmd is that frame (one PmoveSingle below 66 ms), so step = block
// and nothing changes; with cod3mp_server_hz > 0 the block holds the update's time (16.7 ms at 60) while a usercmd is a
// frame (6.9 ms at 144: 2.4x too fast; 50 ms at 20: a third), and this makes them the step's own. Every player's Pmove
// on the host (split screen's second player too) goes through it.

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_pmove_step_time, true, "cod3/Game",
                    "The player's per-step timers (the binoculars' wait and two others in each movement step) count "
                    "the step's own time, so they take the same time at any frame rate; off: the frame's time, as "
                    "before (the binoculars came up later the higher the frame rate)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_pmove_step_time, false, "cod3",
                    "Log every 2 s how many movement steps ran and how many had a step time other than the frame "
                    "time block's (game/binoc_timing.cpp; a test of the frame-rate rule)");
REXCVAR_DEFINE_BOOL(cod3_debug_binoc_timer, false, "cod3",
                    "Log each step of the binoculars' wait (game/binoc_timing.cpp; a test of the frame-rate rule)");

namespace {
namespace g = cod3::guest;
#if defined(COD3_MP)
constexpr uint32_t kTimeBlock = 0x825FEBD8;  // Com_Frame's time block: +4 msec, +8 seconds, +12 K / msec
constexpr uint32_t kSeconds = 8, kPerMsec = 12;
constexpr uint32_t kPmlMsec = 0x82C74880 + 40;  // pml.msec (movement/program_mp.h)
#else
constexpr uint32_t kTimeBlock = 0x825F4E08;  // Com_Frame's time block: +0 frame count, +4 msec, +12 seconds, +16 K/msec
constexpr uint32_t kSeconds = 12, kPerMsec = 16;
constexpr uint32_t kPmlMsec = 0x82A922B0 + 40;  // pml.msec: this movement step's milliseconds (movement/program_sp.h)
#endif
std::atomic<bool> g_said{false};
// cod3_debug_pmove_step_time: since the last line
struct StepCounts {
  uint64_t steps = 0, differed = 0, last_ms = 0;
  int32_t step = 0;
  uint32_t block = 0;
};
StepCounts g_counts;
uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
void CountStep(int32_t step_msec, uint32_t frame_msec) {
  if (!REXCVAR_GET(cod3_debug_pmove_step_time)) return;
  StepCounts& c = g_counts;
  ++c.steps;
  if (step_msec > 0 && frame_msec != 0 && static_cast<uint32_t>(step_msec) != frame_msec) {
    ++c.differed;
    c.step = step_msec;
    c.block = frame_msec;
  }
  const uint64_t now = NowMs();
  if (c.last_ms == 0) c.last_ms = now;
  if (now - c.last_ms < 2000) return;
  REXLOG_INFO("cod3: movement steps: {} in {} ms, {} with a step time other than the frame's (e.g. {} ms in a {} ms "
              "block); cod3_pmove_step_time {}",
              c.steps, now - c.last_ms, c.differed, c.step, c.block,
              REXCVAR_GET(cod3_pmove_step_time) ? "on: they count the step's" : "off: they count the block's");
  c = StepCounts{};
  c.last_ms = now;
}
#if !defined(COD3_MP)
constexpr uint32_t kRequest = 0x82A578F4;   // IN_BinocularsDown's request word (bit 2 asked, bit 128 timer done)
constexpr uint32_t kTimer = 0x82A2A2E8;     // the wait's timer (seconds, float)
constexpr uint32_t kThreshold = 0x82064208; // when the timer is done (float)
constexpr uint32_t kClientNum = 0x82A2A1C8; // as 0x82488B88 reads them: the client number,
constexpr uint32_t kEntities = 0x82A2AB40;  // the entity table
constexpr int kMostLines = 400;
std::atomic<int> g_lines{0};
#endif

// PmoveSingle's second half with the step's time in Com_Frame's time block (see the top of the file); `half` is the
// game's own function.
template <typename Half>
void WithStepTime(PPCContext& ctx, uint8_t* base, Half half) {
  const int32_t step_msec = static_cast<int32_t>(g::ReadU32(base, kPmlMsec));
  const uint32_t frame_msec = g::ReadU32(base, kTimeBlock + 4);
  CountStep(step_msec, frame_msec);  // debug
  if (!REXCVAR_GET(cod3_pmove_step_time) || step_msec <= 0 || frame_msec == 0 ||
      static_cast<uint32_t>(step_msec) == frame_msec) {
    half(ctx, base);
    return;
  }
  const float frame_sec = g::ReadF32(base, kTimeBlock + kSeconds);
  const float frame_inv = g::ReadF32(base, kTimeBlock + kPerMsec);
  const float ratio = float(step_msec) / float(frame_msec);
  g::WriteU32(base, kTimeBlock + 4, static_cast<uint32_t>(step_msec));
  g::WriteF32(base, kTimeBlock + kSeconds, frame_sec * ratio);
  g::WriteF32(base, kTimeBlock + kPerMsec, frame_inv / ratio);
  half(ctx, base);
  g::WriteU32(base, kTimeBlock + 4, frame_msec);
  g::WriteF32(base, kTimeBlock + kSeconds, frame_sec);
  g::WriteF32(base, kTimeBlock + kPerMsec, frame_inv);
  if (!g_said.exchange(true, std::memory_order_relaxed))
    REXLOG_INFO("cod3: movement steps: the player's per-step timers count the step's time ({} ms in a {} ms frame; "
                "cod3_pmove_step_time)",
                step_msec, frame_msec);
}
}  // namespace

// PmoveSingle's second half (its only caller): the weapon, the binoculars and the rest of the step.
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_824B3320);
REX_HOOK_RAW(sub_824B3320) { WithStepTime(ctx, base, __imp__sub_824B3320); }  // PmoveSingle 0x824B39B8
#else
REX_EXTERN(__imp__sub_824959A8);
REX_HOOK_RAW(sub_824959A8) { WithStepTime(ctx, base, __imp__sub_824959A8); }  // PmoveSingle 0x824960D0
#endif

#if !defined(COD3_MP)
// The binoculars' wait, logged per call while a request is up (debug; the campaign's).
REX_EXTERN(__imp__sub_82488B88);
REX_HOOK_RAW(sub_82488B88) {
  const bool debug = REXCVAR_GET(cod3_debug_binoc_timer);
  const uint32_t req = debug ? g::ReadU32(base, kRequest) : 0;
  const float timer = debug ? g::ReadF32(base, kTimer) : 0.0f;
  __imp__sub_82488B88(ctx, base);
  if (!debug || (req == 0 && g::ReadU32(base, kRequest) == 0)) return;
  if (g_lines.fetch_add(1, std::memory_order_relaxed) >= kMostLines) return;
  const uint32_t ents = g::ReadU32(base, kEntities);
  const uint32_t ent = ents ? g::ReadU32(base, ents + 4 * (g::ReadU32(base, kClientNum) + 1)) : 0;
  const uint32_t ps = ent ? g::ReadU32(base, ent + 528) : 0;
  REXLOG_INFO("cod3: binoc timer: frame {} msec {} sec {:.4f} step {} | req {:#x} -> {:#x} | timer {:.4f} -> {:.4f} "
              "(done at {:.4f}) | ps+136 {} ps+140 {:.3f}",
              g::ReadU32(base, kTimeBlock), g::ReadU32(base, kTimeBlock + 4), g::ReadF32(base, kTimeBlock + 12),
              int32_t(g::ReadU32(base, kPmlMsec)), req, g::ReadU32(base, kRequest), timer, g::ReadF32(base, kTimer),
              g::ReadF32(base, kThreshold), ps ? int32_t(g::ReadU32(base, ps + 136)) : -1,
              ps ? g::ReadF32(base, ps + 140) : -1.0f);
}

#endif  // !COD3_MP (the probe)
