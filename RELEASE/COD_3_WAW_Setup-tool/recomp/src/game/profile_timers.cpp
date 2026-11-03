// cod3 - [claude-9b-think] 2026-10-27 UNTESTED (FINDINGS-FRAME-CALLS, second pass): the console build's
// own profiling timers, read hundreds of times a drawn frame for totals nothing uses (campaign).
// Owner 2026-10-27 14:24: "start looking for more un-necessary calls that can be lifted from
// frame-based calling. Timebased should be where it's off loaded."
//
// 0x82127800 reads the clock (QueryPerformanceCounter through the runtime: the guest wrapper, the
// import, Clock::QueryGuestTickCount, clock_gettime) for 131 call sites. Many of them are timing
// brackets left in the console build: the read before a piece of work goes to a {start, total}
// pair of 64-bit words, the read after it adds the difference to the total. For the brackets
// listed below, the function that brackets with a pair is the only code that loads or stores it
// (no other reference to the pair, to its total or to the call counter beside it in the generated
// code). They sit in the scene build and in traces, so they run per model, per visited cell and
// per trace, every drawn frame:
//   0x8242E450  each model or entity added to the scene (2 reads)        pair 0x829EB6F0
//   0x82421890  each visited cell in the world pass (up to 8 reads)      pair 0x829F0990
//   0x8241C208  inside 0x82421890's last bracket (2 reads)               pair 0x829F1988
//   0x8247EA20  a test against a list of volumes, 16 callers (2 reads)   pair 0x829FBEC0
//   0x8248B998  traces, SV_Trace among its callers (2 reads)             pair 0x829FBF70
//   0x82475258  traces, 5 callers (2 reads)                              pair 0x829F1BC8
// claude-9b-test's profiles put the clock reads at 1.5-2.4% of the main thread in saint_lo (s138:
// vDSO 0.7%, QueryGuestTickCount 0.4%, the wrapper 0.4%; fc-stlo-new: vDSO 1.2%), so they grow
// with the frame rate for no game effect.
//
// cod3_skip_profile_timers:
//   0  every site reads the clock (the game as it is).
//   1  the listed brackets (both reads of each pair) get the thread's last real reading back
//      instead of a new one, so their totals stop growing. Every other site still reads the
//      clock: Sys_Milliseconds, the grass wind and its random seeds (0x823EE7C0, 0x823F3150,
//      0x823EEC78), Com_Frame's frame timing, the render job's timing and the rest.
//   2  count (verify): every site reads the clock as in 0; every 10 s the log gets the reads a
//      frame by site (return address) and how many of them 1 would skip.
// Modes 1 and 2 also log, every 10 s, the reads a frame they skipped or counted. A frame here is a
// pass of Com_Frame's first bracket (0x82537008), once per Com_Frame.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_INT32(cod3_skip_profile_timers, 0, "cod3/Performance",
                     "The console build's own profiling timers in the scene build and in traces: 1 skips "
                     "their clock reads (hundreds a drawn frame, for totals nothing uses); 2 counts the "
                     "clock reads a frame by call site in the log instead (every read kept); 0 keeps "
                     "every read")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// The return addresses (lr) of the listed brackets' calls to 0x82127800: start and end of each pair.
constexpr std::array<uint32_t, 18> kListed = {
    0x8242E534, 0x8242E5D0,                          // 0x8242E450, a model or entity
    0x824218C0, 0x824218E8, 0x824219A0, 0x824219C8,  // 0x82421890, a visited cell
    0x82421A04, 0x82421A2C, 0x82421A60, 0x82421A80,  //
    0x8241C224, 0x8241C24C,                          // 0x8241C208
    0x8247EA40, 0x8247EB6C,                          // 0x8247EA20
    0x8248B9C0, 0x8248BB24,                          // 0x8248B998
    0x8247527C, 0x824754F4,                          // 0x82475258
};

// Com_Frame's first bracket, right after the frame's systems (0x823FFC10): once a frame.
constexpr uint32_t kFrameSite = 0x82537008;

bool Listed(uint32_t lr) {
  if (lr < 0x8241C224 || lr > 0x8248BB24) return false;
  for (uint32_t site : kListed)
    if (site == lr) return true;
  return false;
}

// The calling thread's last real reading (0 before its first).
thread_local uint64_t t_last = 0;

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_skipped{0};

// Mode 2: reads by return address, an open table (131 sites known; 512 slots).
constexpr uint32_t kSlots = 512;
struct Slot {
  std::atomic<uint32_t> lr{0};
  std::atomic<uint64_t> reads{0};
};
std::array<Slot, kSlots> g_slots;

void CountRead(uint32_t lr) {
  uint32_t i = ((lr >> 2) * 2654435761u) % kSlots;
  for (uint32_t n = 0; n < kSlots; ++n, i = (i + 1) % kSlots) {
    uint32_t seen = g_slots[i].lr.load(std::memory_order_relaxed);
    if (seen == 0) {
      if (g_slots[i].lr.compare_exchange_strong(seen, lr, std::memory_order_relaxed)) seen = lr;
    }
    if (seen == lr) {
      g_slots[i].reads.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

// Once a frame (the frame site, either mode): the log line every 10 s.
void OnFrame(int32_t mode) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  static uint64_t frames_at_last = 0, skipped_at_last = 0;
  static std::array<uint64_t, kSlots> reads_at_last{};
  const uint64_t frames = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  const Clock::time_point now = Clock::now();
  if (next == Clock::time_point{}) next = now + std::chrono::seconds(10);
  if (now < next) return;
  next = now + std::chrono::seconds(10);
  const uint64_t span = frames - frames_at_last;
  frames_at_last = frames;
  if (span == 0) return;
  if (mode == 1) {
    const uint64_t skipped = g_skipped.load(std::memory_order_relaxed);
    REXLOG_INFO("cod3: profile timers: {:.1f} clock reads a frame skipped at the listed brackets "
                "(cod3_skip_profile_timers 1)",
                double(skipped - skipped_at_last) / double(span));
    skipped_at_last = skipped;
    return;
  }
  // Mode 2: the sites by reads in the last 10 s.
  struct Row {
    uint32_t lr;
    uint64_t reads;
  };
  std::array<Row, kSlots> rows{};
  uint32_t count = 0;
  uint64_t all = 0, listed = 0;
  for (uint32_t i = 0; i < kSlots; ++i) {
    const uint32_t lr = g_slots[i].lr.load(std::memory_order_relaxed);
    if (lr == 0) continue;
    const uint64_t reads = g_slots[i].reads.load(std::memory_order_relaxed);
    const uint64_t delta = reads - reads_at_last[i];
    reads_at_last[i] = reads;
    if (delta == 0) continue;
    rows[count++] = {lr, delta};
    all += delta;
    if (Listed(lr)) listed += delta;
  }
  std::sort(rows.begin(), rows.begin() + count, [](const Row& a, const Row& b) { return a.reads > b.reads; });
  std::string top;
  for (uint32_t i = 0; i < count && i < 12; ++i) {
    char item[64];
    std::snprintf(item, sizeof(item), "%s0x%08X %.1f%s", i ? ", " : "", rows[i].lr,
                  double(rows[i].reads) / double(span), Listed(rows[i].lr) ? " (listed)" : "");
    top += item;
  }
  REXLOG_INFO("cod3: profile timers: {:.1f} clock reads a frame from 0x82127800 at {} sites, {:.1f} of them at "
              "the listed brackets (what cod3_skip_profile_timers 1 skips); busiest: {}",
              double(all) / double(span), count, double(listed) / double(span), top);
}

}  // namespace

// QueryPerformanceCounter's wrapper (r3 = the count): the listed brackets get the thread's last
// real reading back with cod3_skip_profile_timers 1; every other call reads the clock.
REX_EXTERN(__imp__sub_82127800);
REX_HOOK_RAW(sub_82127800) {
  const uint32_t lr = uint32_t(ctx.lr);
  const int32_t mode = REXCVAR_GET(cod3_skip_profile_timers);
  if (mode == 0) {
    __imp__sub_82127800(ctx, base);
    return;
  }
  if (lr == kFrameSite) OnFrame(mode);
  if (mode == 1 && t_last != 0 && Listed(lr)) {
    ctx.r3.u64 = t_last;
    g_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  __imp__sub_82127800(ctx, base);
  t_last = ctx.r3.u64;
  if (mode == 2) CountRead(lr);
}
