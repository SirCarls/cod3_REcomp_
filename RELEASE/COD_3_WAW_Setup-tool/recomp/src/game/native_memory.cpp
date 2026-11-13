// cod3 - [memory-360] 2026-11-11 the game's memory past the Xbox 360's limits, in both programs. Stages 1-2 tested
// (dev/agents/memory-360/NOTES.md); the move's room for banks (12:10) untested.
//
// Owner 2026-11-11 08:22: "We need to be able to access memory directly and not be limited by the 360." 09:25-09:28:
// this chat removes the game's dependency on the 360's memory, in three stages, each proven before the next
// (dev/agents/rexglue-native/MEMORY-NOTES.md has the measurements and the plan; dev/agents/memory-360/NOTES.md the
// work since).
//
// How the game uses its memory (decomp dev/waw-decomp/cod3/decomp/820C0000.c, cod3mp mp/decomp/820E0000.c):
//  - At start the arena maker 0x820CDC30 (cod3mp 0x820E0158) takes two blocks of the console's 512 MB of physical
//    memory with XPhysicalAlloc 0x82344EE8 (cod3mp 0x823142B0): arena 0, 370.9 MB (cod3mp 369.9 MB), and arena 2,
//    11 MB (cod3mp 9 MB, write-combined). It keeps their start, end and size in six words (0x829099CC..0x829099E0,
//    cod3mp 0x82A8665C..0x82A86670). The arena setup 0x820CB890 reads them through four accessors (0x820CDD90,
//    0x820CDDD0, 0x820CDD18, 0x820CDCD8), which call the maker the first time.
//  - Everything the game allocates comes from arena 0 (operator new, Z_Malloc, the anim heap, cvar strings, level
//    data) or arena 2 (the GPU command buffers). The header of arena N is at 0x82907490 + N * 0x494 (cod3mp
//    0x82A84120): +0x474 start, +0x478 end, +0x484 size, +0x488 in use, +0x48C the most ever in use.
//  - Com_Init's streaming setup 0x8245D7F0 (cod3mp 0x82479448) takes the streaming pool from arena 0 in one piece:
//    num_banks (a cvar, default 80; cod3mp 88) banks of 3 MB, 240 MB (264 MB). Its object (kept at 0x82A2AB64,
//    cod3mp 0x82BCC820) has a free-bank mask at +0..+8 (a set bit is a free bank), a second mask at +0xC..+0x14,
//    the bank count (float) at +0x18, the bank size at +0x1C and the pool at +0x20. The masks are three words, so
//    the game can never use more than 96 banks.
//
// This file:
//  - stage 1, cod3_native_memory_report (off; started at the arena maker): a host thread reads about 1 KB of the game's memory once a second
//    and logs how full each budget is, every 30 s, at each level load, and whenever a budget reaches a new high.
//    It writes nothing.
//  - stage 2, cod3_native_stream_banks_extra (0, native renderer only): arena 0 is made that many 3 MB banks
//    bigger and num_banks is raised by as many (at most 96 in all), so more level data stays loaded. A start-up
//    check lowers the number when the console memory left would be too small (see PlanExtraBanks). With stage 3's
//    move on, the first 10 banks need no console memory: they fit in the room the move makes in arena 0.
//  - stage 3, moving the game's CPU-only data out of the 512 MB, is in native_memory_shadow.cpp (the shadow test
//    and cod3_native_memory_move); the report shows the moved data ("moved to guest virtual memory").
//  - stage 5's test, cod3_native_memory_move_pool (off; with the move), is there too: the streaming pool goes to the
//    move's room, and then stage 2's banks need no console memory (PlanExtraBanks).
//
// Both switches off: the arena maker's hook calls the game's own function and the streaming setup's hook calls the
// game's own function; nothing else changes.

#include "game/native_memory.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "game/addresses.h"  // cvar_flag::kUserCreated
#include "game/game_api.h"   // CvarFind, CvarGet
#include "game/guest.h"
#include "game/program.h"  // COD3_ADDR
#include "game/render_resolution.h"  // ScreenBufferBytes

REXCVAR_DEFINE_BOOL(cod3_native_memory_report, false, "cod3/Debug",
                    "Log how full the game's memory is (its two arenas, the streaming banks, the effects pools, the "
                    "particle buffer and the console memory left), every 30 s, at each level load and at each new "
                    "high. Reads only. Applies after a restart")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(cod3_native_stream_banks_extra, 0, "cod3/Debug",
                     "Streaming memory (native renderer only): this many more 3 MB streaming banks than the game's "
                     "own (80 in the campaign, 88 in multiplayer; 96 at most), so more level data stays loaded. "
                     "Taken from the Xbox 360's 512 MB; a check at start lowers it when too little would be left. "
                     "0: the game's own. Applies after a restart")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// The native renderer's switch (game/ngl_side.cpp). Stage 2 acts only when it is on.
REXCVAR_DECLARE(bool, cod3_native_vulkan);

namespace {

using cod3::guest::ReadU32;
using cod3::guest::WriteU32;
using cod3::native_memory::MoveArenaHeader;
using cod3::native_memory::MoveFallbacks;
using cod3::native_memory::MoveReserve;

// --- The game's addresses --------------------------------------------------------------------------------------

// The arena maker's six words (0x820CDC30 writes them in this order: size 0, start 0, end 0, size 2, start 2, end 2).
constexpr uint32_t kArena0Start = COD3_ADDR(0x829099CC, 0x82A8665C);
constexpr uint32_t kArena0End = COD3_ADDR(0x829099D0, 0x82A86660);
constexpr uint32_t kArena0Size = COD3_ADDR(0x829099D4, 0x82A86664);
constexpr uint32_t kArena2Start = COD3_ADDR(0x829099D8, 0x82A86668);
constexpr uint32_t kArena2End = COD3_ADDR(0x829099DC, 0x82A8666C);
constexpr uint32_t kArena2Size = COD3_ADDR(0x829099E0, 0x82A86670);
// XPhysicalAlloc(size, highest address, alignment, protection).
constexpr uint32_t kXPhysicalAlloc = COD3_ADDR(0x82344EE8, 0x823142B0);
// The sizes and protections the maker asks for (protection 0x20000004: read/write, 0x20000404: and write-combined).
constexpr uint32_t kGameArena0Bytes = cod3::kMultiplayer ? 0x171F0000u : 0x172F0000u;
constexpr uint32_t kGameArena2Bytes = cod3::kMultiplayer ? 0x900000u : 0xB00000u;
constexpr uint32_t kArena0Protect = 0x20000004u;
constexpr uint32_t kArena2Protect = 0x20000404u;
constexpr uint32_t kArenaAlign = 0x10000u;

// Arena headers (0x494 bytes each, eight of them) and their fields.
constexpr uint32_t kArenaHeaders = COD3_ADDR(0x82907490, 0x82A84120);
constexpr uint32_t kArenaHeaderBytes = 0x494;
constexpr uint32_t kArenaUsed = 0x488;
constexpr uint32_t kArenaPeak = 0x48C;
constexpr uint32_t kArenaFieldStart = 0x474;
constexpr uint32_t kArenaFieldEnd = 0x478;
constexpr uint32_t kArenaFieldSize = 0x484;

// The streaming banks' object and its fields.
constexpr uint32_t kBanksObject = COD3_ADDR(0x82A2AB64, 0x82BCC820);
constexpr uint32_t kBanksFreeMask = 0x00;   // three words
constexpr uint32_t kBanksCount = 0x18;      // float
constexpr uint32_t kBanksBankBytes = 0x1C;  // 3 MB
constexpr uint32_t kBanksPool = 0x20;
constexpr uint32_t kGameBanks = cod3::kMultiplayer ? 88u : 80u;  // num_banks' default in 0x8245D7F0 / 0x82479448
constexpr uint32_t kMaxBanks = 96;                                // the masks' 3 x 32 bits
constexpr uint32_t kBankBytes = 0x300000;

// The effects' apsMemory pools: the list (made by 0x820FF580) has the array of pools at +0 and their number (16 bits)
// at +6; each pool: +0 blocks, +4 in use, +8 the most in use (kept by the game), +0x14 block size. The apsEffect
// pool's header is at [kApsEffect] + 8, the same layout.
constexpr uint32_t kApsList = COD3_ADDR(0x8290B560, 0x82A883F0);
constexpr uint32_t kApsEffect = COD3_ADDR(0x8290A9DC, 0x82A8786C);
// The particle buffer (0x820FAD68 sets it up, 0x820FACA8 hands out room): +0 how many particles fit (12000), +0x10
// a flag set when a request is refused (never cleared), +0x14 how many this frame.
constexpr uint32_t kParticles = COD3_ADDR(0x8290B5B0, 0x82A88440);
// level.time (ms); it goes back when a level loads or restarts.
constexpr uint32_t kLevelTime = COD3_ADDR(0x82A4E82C, 0x82C2826C);
// The game's image (code and data): pointers read here are followed only into it or into the two arenas. Its end
// here is just past the highest global either program reads (the C heap's handle, 0x82BEA9C0 / 0x82DD55CC).
constexpr uint32_t kImageLow = COD3_ADDR(0x82000000, 0x82000000);
constexpr uint32_t kImageHigh = COD3_ADDR(0x82BEB000, 0x82DD6000);

constexpr uint32_t kMB = 1u << 20;

// --- Stage 2's plan, decided once by the arena maker ---------------------------------------------------------------

std::atomic<uint32_t> g_extra_banks{0};  // banks added to the game's own (0: none)

rex::memory::Memory* GuestMemory() {
  auto* ks = rex::system::kernel_state();
  return ks ? ks->memory() : nullptr;
}

// The console's physical memory not in use, in bytes (the physical heap holds every physical allocation).
uint64_t FreePhysicalBytes() {
  if (auto* mem = GuestMemory()) {
    if (auto* heap = mem->GetPhysicalHeap())
      return uint64_t(heap->GetUnreservedPageCount()) * heap->page_size();
  }
  return 0;
}

// How many banks to add. Room after the two arenas must still hold, during play:
//  - the screen buffers at the largest size the port draws the game at in guest memory (1920x1080, 37 MB; at
//    1280x720 they take 17 MB);
//  - what the game takes from outside its arenas when a level loads: the campaign 21-29 MB (32 MB counted); cod3mp
//    more, 43 MB in mp_merv (48 MB counted; [memory-360] 2026-11-11 c3mp: at +8 banks only 29 MB stayed free at
//    1280x720, which 1080p's bigger screen buffers would have taken below the floor);
//  - a floor of 16 MB (the lowest seen during play, 44 MB at native 1080p, minus all of the above, stays above it).
// Measured on the owner's PC: 492 MB free before the arenas in both programs; this allows 8 banks (24 MB) in the
// campaign and 4 (12 MB) in cod3mp. More needs stage 3, which frees room inside arena 0.
// [memory-360] 2026-11-11 12:10: with stage 3's move on (native_memory_shadow.cpp), the first banks come out of the
// room the move makes in arena 0 before Com_Init takes the bank pool: the campaign's list moves 36 MB by then
// (Sys_Main's two live 12 MB buffers, the particle pools and heap 8 MB, AnimHeap 4 MB; c5movebank's log order),
// cod3mp's at least as much (16 MB buffers); 30 MB is counted (10 banks). Arena 0 grows only for the banks past
// those. At the end of a level's load arena 0 then holds the game's ~365 MB, less the 72 MB moved, plus the banks.
constexpr uint32_t kMoveRoomForBanks = 30u * kMB;

struct BankPlan {
  uint32_t banks = 0;  // banks added to the game's own
  uint32_t grow = 0;   // of those, the ones arena 0 grows for (the rest fit in the move's room)
};

BankPlan PlanExtraBanks() {
  BankPlan plan;
  const int asked = std::clamp(int(REXCVAR_GET(cod3_native_stream_banks_extra)), 0, int(kMaxBanks - kGameBanks));
  if (asked == 0) return plan;
  if (!REXCVAR_GET(cod3_native_vulkan)) {
    REXLOG_INFO("cod3: memory: cod3_native_stream_banks_extra {} left unused: it works with the native renderer "
                "only (cod3_native_vulkan)",
                asked);
    return plan;
  }
  // Stage 5's test (cod3_native_memory_move_pool): the whole pool goes to the move's room, so its banks need no
  // console memory at all.
  if (cod3::native_memory::MovePoolOn()) {
    plan.banks = uint32_t(asked);
    REXLOG_INFO("cod3: memory: {} more streaming banks asked for, {} given ({} of the game's {}), all in the move's "
                "room with the streaming pool (stage 5's test); no console memory",
                asked, plan.banks, kGameBanks + plan.banks, kGameBanks);
    return plan;
  }
  const uint32_t in_room = MoveReserve() ? std::min<uint32_t>(uint32_t(asked), kMoveRoomForBanks / kBankBytes) : 0;
  const uint64_t free_now = FreePhysicalBytes();
  const uint64_t screens = cod3::ScreenBufferBytes(1920, 1080);
  const uint64_t level = (cod3::kMultiplayer ? 48ull : 32ull) * kMB, floor = 16ull * kMB;
  const uint64_t needed = uint64_t(kGameArena0Bytes) + kGameArena2Bytes + screens + level + floor;
  const uint64_t room = free_now > needed ? free_now - needed : 0;
  const uint32_t fits = uint32_t(std::min<uint64_t>(room / kBankBytes, kMaxBanks - kGameBanks));
  plan.grow = std::min<uint32_t>(uint32_t(asked) - in_room, fits);
  plan.banks = in_room + plan.grow;
  REXLOG_INFO("cod3: memory: {} more streaming banks asked for, {} given ({} of the game's {}; {} in the room stage 3's "
              "move makes, {} from console memory); console memory free before the arenas {} MB, kept for play {} MB "
              "(screen buffers {} MB, a level's own {} MB, floor {} MB)",
              asked, plan.banks, kGameBanks + plan.banks, kGameBanks, in_room, plan.grow, free_now / kMB,
              (needed - kGameArena0Bytes - kGameArena2Bytes) / kMB, screens / kMB, level / kMB, floor / kMB);
  return plan;
}

// XPhysicalAlloc through the game's own function, on a frame of its own. 0 when not given.
uint32_t PhysicalAlloc(PPCContext& ctx, uint8_t* base, uint32_t size, uint32_t protect) {
  cod3::guest::Call call(ctx, base);
  return uint32_t(call.Invoke(kXPhysicalAlloc, {size, ~uint64_t(0), kArenaAlign, protect}));
}

// --- Stage 1: the report ---------------------------------------------------------------------------------------------

struct Sample {
  bool ok = false;
  uint32_t arena0_used = 0, arena0_peak = 0, arena0_size = 0;
  uint32_t arena2_used = 0, arena2_size = 0;
  uint32_t moved_used = 0, moved_peak = 0, moved_size = 0;  // stage 3's move arena (0: none)
  uint64_t move_fallbacks = 0;
  uint32_t banks = 0, banks_free = 0, bank_bytes = 0;
  uint32_t aps_pools = 0, aps_full = 0;       // pools, and how many of them reached their block count
  uint32_t aps_worst_peak = 0, aps_worst_blocks = 0, aps_worst_size = 0;  // the fullest pool (by its own peak)
  uint32_t effect_peak = 0, effect_blocks = 0;
  uint32_t particles_cap = 0, particles_frame = 0;
  bool particles_refused = false;
  int32_t level_time = 0;
  uint64_t physical_free = 0;
  uint64_t virtual_used = 0;  // guest virtual memory taken in the 0x40000000 range (the C heap's and others')
};

class Reader {
 public:
  explicit Reader(rex::memory::Memory* mem) : mem_(mem) {}

  uint32_t U32(uint32_t addr) const {
    uint32_t v;
    std::memcpy(&v, mem_->TranslateVirtual(addr), 4);
    return __builtin_bswap32(v);
  }
  float F32(uint32_t addr) const {
    const uint32_t bits = U32(addr);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }
  // Pointers the game keeps are followed only into its image or into its two arenas (committed memory).
  bool Safe(uint32_t addr, uint32_t len) const {
    const uint64_t end = uint64_t(addr) + len;
    if ((addr & 3) != 0) return false;
    if (addr >= kImageLow && end <= kImageHigh) return true;
    // Arena 0, arena 2 and stage 3's move arena (native_memory_shadow.cpp: the particle pools can live there).
    for (uint32_t h : {kArenaHeaders, kArenaHeaders + 2 * kArenaHeaderBytes, MoveArenaHeader()}) {
      if (h == 0) continue;
      const uint32_t lo = U32(h + kArenaFieldStart), hi = U32(h + kArenaFieldEnd);
      if (lo && addr >= lo && end <= hi) return true;
    }
    return false;
  }

 private:
  rex::memory::Memory* mem_;
};

Sample Read(rex::memory::Memory* mem) {
  Sample s;
  const Reader r(mem);
  const uint32_t a0 = kArenaHeaders, a2 = kArenaHeaders + 2 * kArenaHeaderBytes;
  s.arena0_size = r.U32(a0 + kArenaFieldSize);
  if (s.arena0_size == 0) return s;  // not made yet
  s.arena0_used = r.U32(a0 + kArenaUsed);
  s.arena0_peak = r.U32(a0 + kArenaPeak);
  s.arena2_size = r.U32(a2 + kArenaFieldSize);
  s.arena2_used = r.U32(a2 + kArenaUsed);
  if (const uint32_t moved = MoveArenaHeader()) {
    s.moved_size = r.U32(moved + kArenaFieldSize);
    s.moved_used = r.U32(moved + kArenaUsed);
    s.moved_peak = r.U32(moved + kArenaPeak);
  }
  s.move_fallbacks = MoveFallbacks();

  const uint32_t banks = r.U32(kBanksObject);
  if (banks && r.Safe(banks, 0x24)) {
    const float count = r.F32(banks + kBanksCount);
    if (count > 0.0f && count <= float(kMaxBanks)) {
      s.banks = uint32_t(count + 0.5f);
      s.bank_bytes = r.U32(banks + kBanksBankBytes);
      for (uint32_t w = 0; w < 3; ++w) {
        uint32_t mask = r.U32(banks + kBanksFreeMask + 4 * w);
        const uint32_t first = 32 * w;
        if (first >= s.banks) break;
        if (s.banks - first < 32) mask &= (1u << (s.banks - first)) - 1;
        s.banks_free += uint32_t(std::popcount(mask));
      }
    }
  }

  const uint32_t list = r.U32(kApsList);
  if (list && r.Safe(list, 8)) {
    const uint32_t array = r.U32(list);
    const uint32_t n = r.U32(list + 4) & 0xFFFF;  // the 16 bits at +6
    if (array && n > 0 && n <= 32 && r.Safe(array, 4 * n)) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t pool = r.U32(array + 4 * i);
        if (!pool || !r.Safe(pool, 0x18)) continue;
        const uint32_t blocks = r.U32(pool), peak = r.U32(pool + 8), size = r.U32(pool + 0x14);
        if (blocks == 0) continue;
        ++s.aps_pools;
        if (peak >= blocks) ++s.aps_full;
        // The fullest by its own share (peak / blocks), compared without division.
        if (s.aps_worst_blocks == 0 || uint64_t(peak) * s.aps_worst_blocks > uint64_t(s.aps_worst_peak) * blocks) {
          s.aps_worst_peak = peak;
          s.aps_worst_blocks = blocks;
          s.aps_worst_size = size;
        }
      }
    }
  }
  const uint32_t effect = r.U32(kApsEffect);
  if (effect && r.Safe(effect + 8, 12)) {
    s.effect_blocks = r.U32(effect + 8);
    s.effect_peak = r.U32(effect + 16);
  }

  s.particles_cap = r.U32(kParticles);
  s.particles_refused = r.U32(kParticles + 0x10) != 0;
  s.particles_frame = r.U32(kParticles + 0x14);
  s.level_time = int32_t(r.U32(kLevelTime));

  s.physical_free = FreePhysicalBytes();
  if (auto* heap = mem->LookupHeap(0x40000000u))
    s.virtual_used = uint64_t(heap->heap_size()) - uint64_t(heap->GetUnreservedPageCount()) * heap->page_size();
  s.ok = true;
  return s;
}

double Mb(uint64_t bytes) { return double(bytes) / double(kMB); }

void LogSample(const Sample& s, const char* why, uint32_t banks_peak, uint64_t physical_lowest) {
  // Stage 3's move (native_memory_shadow.cpp), when it has an arena.
  char moved[320] = "";
  if (s.moved_size) {
    // Stage 5's test (cod3_native_memory_move_pool): the streaming pool among the moved data.
    char pool[160] = "";
    const cod3::native_memory::PoolStats p = cod3::native_memory::Pool();
    if (p.moved)
      std::snprintf(pool, sizeof(pool), ", the streaming pool's %.0f MB among it (%llu graphics reads, %llu locks)",
                    Mb(p.end - p.start), static_cast<unsigned long long>(p.graphics_reads),
                    static_cast<unsigned long long>(p.locks));
    std::snprintf(moved, sizeof(moved), "; moved to guest virtual memory %.1f of %.0f MB (most %.1f)%s%s",
                  Mb(s.moved_used), Mb(s.moved_size), Mb(s.moved_peak), pool,
                  s.move_fallbacks ? ", some went to arena 0 for want of room" : "");
  }
  REXLOG_INFO("cod3: memory ({}, {}): arena 0 {:.1f} of {:.1f} MB (most {:.1f}); stream banks {} of {} in use (most "
              "{}); arena 2 {:.1f} of {:.1f} MB; console memory free {:.0f} MB (lowest {:.0f}); effects pools {} "
              "(fullest {} of {} blocks of {} bytes; {} reached their size); effect pool most {} of {}; particles "
              "this frame {} of {}{}; guest virtual memory at 0x40000000 {:.0f} MB{}",
              why, REXCVAR_GET(cod3_native_vulkan) ? "native renderer" : "emulated GPU", Mb(s.arena0_used),
              Mb(s.arena0_size), Mb(s.arena0_peak), s.banks - s.banks_free, s.banks, banks_peak, Mb(s.arena2_used),
              Mb(s.arena2_size), Mb(s.physical_free), Mb(physical_lowest), s.aps_pools, s.aps_worst_peak,
              s.aps_worst_blocks, s.aps_worst_size, s.aps_full, s.effect_peak, s.effect_blocks, s.particles_frame,
              s.particles_cap, s.particles_refused ? ", a request was refused" : "", Mb(s.virtual_used), moved);
}

// Once a second (started only when cod3_native_memory_report is on). Never joined: the process ends with it running.
void ReportThread() {
  using clock = std::chrono::steady_clock;
  uint32_t banks_peak = 0, aps_full = 0, effect_peak = 0, arena0_peak_logged = 0;
  uint64_t physical_lowest = ~uint64_t(0);
  bool refused = false;
  int32_t level_time = 0;
  clock::time_point last = clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto* mem = GuestMemory();
    if (!mem) continue;
    const Sample s = Read(mem);
    if (!s.ok) continue;

    const uint32_t in_use = s.banks - s.banks_free;
    if (s.physical_free < physical_lowest) physical_lowest = s.physical_free;
    const char* why = nullptr;
    // A level loaded or restarted: level.time went back (or started).
    if (s.level_time < level_time || (level_time == 0 && s.level_time > 0)) why = "level loaded";
    level_time = s.level_time;

    if (in_use > banks_peak) {
      banks_peak = in_use;
      if (!why && in_use + 2 >= s.banks) why = "stream banks near their count";
    }
    if (s.aps_full > aps_full) {
      aps_full = s.aps_full;
      if (!why) why = "an effects pool reached its size";
    }
    if (s.effect_blocks && s.effect_peak > effect_peak) {
      effect_peak = s.effect_peak;
      if (!why && effect_peak >= s.effect_blocks) why = "the effect pool reached its size";
    }
    if (s.particles_refused && !refused) {
      refused = true;
      if (!why) why = "the particle buffer refused a request";
    }
    if (s.arena0_peak >= arena0_peak_logged + kMB) {  // a new most for arena 0, by 1 MB or more
      const bool first = arena0_peak_logged == 0;
      arena0_peak_logged = s.arena0_peak;
      if (!why && !first) why = "arena 0 at a new high";
    }
    const auto now = clock::now();
    if (!why && now - last >= std::chrono::seconds(30)) why = "every 30 s";
    if (!why) continue;
    last = now;
    LogSample(s, why, banks_peak, physical_lowest);
  }
}

void StartReport() {
  if (!REXCVAR_GET(cod3_native_memory_report)) return;
  static std::once_flag once;
  std::call_once(once, [] {
    REXLOG_INFO("cod3: memory: report on (cod3_native_memory_report): a line every 30 s, at each level load and at "
                "each new high");
    std::thread(&ReportThread).detach();
  });
}

}  // namespace

// 0x820CDC30 (cod3mp 0x820E0158): the arena maker, called once at start through the arena accessors. With stage 2's
// banks it does what the game's does with arena 0 that much bigger; otherwise it is the game's own.
REX_EXTERN(__imp__sub_820CDC30);
REX_HOOK_RAW(sub_820CDC30) {
  StartReport();
  const BankPlan plan = PlanExtraBanks();
  if (plan.grow == 0) {
    __imp__sub_820CDC30(ctx, base);
    g_extra_banks.store(plan.banks, std::memory_order_relaxed);  // all in the move's room (or none)
    return;
  }
  const uint32_t banks = plan.grow;
  // The game's steps (820C0000.c, FUN_820cdc30), arena 0 given `banks` banks more plus 1 MB for alignment.
  const uint32_t arena0 = kGameArena0Bytes + banks * kBankBytes + kMB;
  WriteU32(base, kArena0Size, arena0);
  const uint32_t start0 = PhysicalAlloc(ctx, base, arena0, kArena0Protect);
  if (start0 == 0) {
    // Not given: the game's own sizes, from the start (nothing has been written but the size); the banks in the
    // move's room stay.
    REXLOG_WARN("cod3: memory: {:.1f} MB for a bigger arena 0 not given; the game's own sizes", Mb(arena0));
    __imp__sub_820CDC30(ctx, base);
    g_extra_banks.store(plan.banks - plan.grow, std::memory_order_relaxed);
    return;
  }
  WriteU32(base, kArena0Start, start0);
  WriteU32(base, kArena0End, start0 + arena0);
  WriteU32(base, kArena2Size, kGameArena2Bytes);
  const uint32_t start2 = PhysicalAlloc(ctx, base, kGameArena2Bytes, kArena2Protect);
  WriteU32(base, kArena2Start, start2);
  WriteU32(base, kArena2End, start2 + kGameArena2Bytes);
  g_extra_banks.store(plan.banks, std::memory_order_relaxed);
  REXLOG_INFO("cod3: memory: arena 0 {:08X}..{:08X} ({:.1f} MB, the game's {:.1f} MB) for {} more streaming banks; "
              "arena 2 {:08X}; console memory free now {:.0f} MB",
              start0, start0 + arena0, Mb(arena0), Mb(kGameArena0Bytes), banks, start2, Mb(FreePhysicalBytes()));
}

// 0x8245D7F0 (cod3mp 0x82479448), Com_Init's streaming setup (r3: the banks' object): reads num_banks (Cvar_Get
// "num_banks" with the default 80 / 88 and flag 0x10, write protected) and takes the pool from arena 0. With
// stage 2's banks, num_banks is made first with the bigger count, as a user-made dvar the way a command line's
// "+set num_banks" makes it: the game's Cvar_Get then keeps that value and takes its own default as the reset value
// (Cvar_Get 0x8252BBE0, the existing-dvar branch). A num_banks the player set is left as it is.
REX_EXTERN(__imp__sub_8245D7F0);
REX_HOOK_RAW(sub_8245D7F0) {
  const uint32_t banks = g_extra_banks.load(std::memory_order_relaxed);
  if (banks == 0) {
    __imp__sub_8245D7F0(ctx, base);
    return;
  }
  const uint32_t object = ctx.r3.u32;
  const std::string count = std::to_string(kGameBanks + banks);
  if (cod3::game::CvarFind(ctx, base, "num_banks") == 0)
    cod3::game::CvarGet(ctx, base, "num_banks", count, cod3::addr::cvar_flag::kUserCreated);
  else
    REXLOG_INFO("cod3: memory: num_banks was set already; the arena has room for {} banks", kGameBanks + banks);
  ctx.r3.u64 = object;
  __imp__sub_8245D7F0(ctx, base);
  const uint32_t pool = ReadU32(base, object + kBanksPool);
  float got = 0.0f;
  {
    const uint32_t bits = ReadU32(base, object + kBanksCount);
    std::memcpy(&got, &bits, 4);
  }
  if (pool == 0)
    REXLOG_ERROR("cod3: memory: the streaming pool for {:.0f} banks was not given", got);
  else
    REXLOG_INFO("cod3: memory: streaming pool {:08X}, {:.0f} banks of {} KB (the game's own {})", pool, got,
                ReadU32(base, object + kBanksBankBytes) / 1024, kGameBanks);
}
