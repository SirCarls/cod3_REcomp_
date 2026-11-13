// cod3 - [memory-360] 2026-11-11 stage 3: the shadow (step 1, a test that moves nothing) and the move (step 2,
// cod3_native_memory_move, off by default; tested on Linux 11:37-13:17, dev/agents/memory-360/NOTES.md), and stage
// 5's test (cod3_native_memory_move_pool, off; UNTESTED).
//
// Stage 3 moves the game's CPU-only data out of arena 0 (the 370.9 MB the game takes from the Xbox 360's 512 MB of
// physical memory, native_memory.cpp) into guest virtual memory, which this SDK keeps outside the 512 MB. A block
// may move only if the graphics side never reads it: the GPU (emulated or native) addresses memory physically, 29
// bits, and a block in virtual memory has no physical address.
//
// The shadow finds out which blocks those are, before anything moves:
//  - every allocation the game makes from arena 0 is recorded with the game code that asked for it (the guest call
//    stack, kStackDepth return addresses), and forgotten when it is freed;
//  - every address the game hands Direct3D for drawing is checked against those blocks: the vertex buffers
//    (SetStreamSource), the index buffers (SetIndices) and the textures (SetTexture: the base and mip addresses of
//    its fetch constant). The native renderer's own reads can be checked too (CheckGpuRead, for ngl_side.cpp);
//  - and the buffers FMOD hands the XMA audio decoder (it reads them by physical address too);
//  - once a minute it logs the call sites whose blocks reached the graphics side, the largest that never did, and
//    each site not listed before ("memory shadow list" lines, merged across runs into the move's list).
//
// The move puts the allocations of the listed call sites (kMoveList: sites that never reached the graphics side in
// any shadow run) in an arena of the game's own kind, in guest virtual memory:
//  - the game keeps eight arena headers (0x82907490 + 0x494 each; 0 is arena 0, 2 arena 2). The move takes a free
//    one (the last) and points it at kMoveBytesWanted of guest virtual memory (the 0x40000000 range, 22 MB of which the
//    game and the SDK use), the way the game's own 0x820CB5E0 does: the dlmalloc state zeroed (dlmalloc sets
//    itself up on first use), start, end, the break (0x820CB958 hands dlmalloc memory from start to end), size;
//  - a listed allocation runs the game's own allocate with that header as the current arena, as the game's
//    0x820CC2A0 does for arena 2: the blocks are the game's dlmalloc chunks, accounted in that header (in use,
//    most), and nothing else changes. If the arena is full, the allocation goes to arena 0 as before;
//  - the game's free (0x820CBF58) finds a block's arena by its address among the eight headers, so moved blocks are
//    freed by the game itself. Its resize (0x820CB778) uses the current arena: a moved block is resized with the
//    move's header current (the hook below);
//  - with the shadow on too, a moved block handed to Direct3D or the XMA decoder by its own address is an error.
//  - NOT covered by the shadow (14:20, the owner's picture): what the native renderer reads straight from the game's
//    memory (bone matrices, materials, lists). ngl_side.cpp reads those only where Readable() / Physical() say so (the
//    0xA0000000 views and the image), so a moved block it reads is skipped there, silently. So a site stays on the list
//    only once pictures with the move on match pictures with it off; InMovedMemory() is there for ngl_side to accept
//    the move's memory too (asked of rexglue-native).
// The game's arena code was read in the decomp (820C0000.c 0x820CB580-0x820CDB90; cod3mp's twins in mp/decomp/
// 820D0000.c at 0x820DDAC8-0x820DE688, the same layout: headers 0x82A84120, current arena 0x82A865D0, set-up flag
// 0x82A865C0).
//
// [memory-360] 2026-11-11 stage 5, a TEST (cod3_native_memory_move_pool, off): the streaming pool (one allocation,
// num_banks x 3 MB of textures and meshes; kPoolSite) goes to the move's arena too. The game's resource headers keep
// CPU addresses and Direct3D makes GPU addresses of them only when it binds (D3DDevice_SetTexture 0x822EBE40,
// SetStreamSource 0x822E9208), so the game itself is fine with that; the native renderer must read those header
// addresses as they are (rexglue-native's side, asked on the board 13:37); without that its pictures are wrong.
// While the shadow or the move is on, the game's locks of pool resources are logged (D3D's locks hand back the
// header's own address as the CPU pointer), and the native renderer's reads of the moved pool are counted, not
// errors.
#include "game/native_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "game/guest.h"
#include "game/program.h"  // COD3_ADDR

REXCVAR_DEFINE_BOOL(cod3_native_memory_shadow, false, "cod3/Debug",
                    "A test for the memory work: record every allocation from the game's main memory arena with "
                    "the game code that asked for it, and log which of them reach the graphics side (textures, "
                    "vertex and index buffers). Moves nothing; slower. Applies after a restart")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(cod3_native_memory_move, false, "cod3/Debug",
                    "Stage 3 of the memory work: the game's allocations that never reach the graphics side (a list "
                    "made from the shadow test's runs) go to guest virtual memory, outside the console's 512 MB, "
                    "instead of the game's main arena. Native renderer only. A TEST: leave it off (pictures with it "
                    "on are still being checked). Applies after a restart")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(cod3_native_memory_move_pool, false, "cod3/Debug",
                    "Stage 5 of the memory work, a TEST: with cod3_native_memory_move on, the game's streaming pool "
                    "(its textures and meshes, 240 MB) goes to guest virtual memory too. Needs the native "
                    "renderer's matching change (it must read those addresses as they are); without it the pictures "
                    "are wrong. Native renderer only. Applies after a restart")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(cod3_native_memory_move_animheap, false, "cod3/Debug",
                    "A TEST of the memory work, for the native renderer's bone reads: with cod3_native_memory_move "
                    "on, the game's animation heap (4 MB, the soldiers' bone matrices) goes to guest virtual memory "
                    "too. The soldiers are drawn unposed until the native renderer reads that memory. Applies after a "
                    "restart")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DECLARE(bool, cod3_native_vulkan);

namespace {

using cod3::guest::ReadU32;

constexpr uint32_t kArenaHeaders = COD3_ADDR(0x82907490, 0x82A84120);  // arena 0's header (native_memory.cpp)
constexpr uint32_t kCurrentArena = COD3_ADDR(0x82909940, 0x82A865D0);   // the arena the allocate call uses
constexpr uint32_t kArenaReady = COD3_ADDR(0x82909930, 0x82A865C0);    // nonzero once the game set up its arenas
constexpr uint32_t kArenaFieldStart = 0x474, kArenaFieldEnd = 0x478;
constexpr uint32_t kArenaFieldBreak = 0x47C, kArenaFieldBreakEnd = 0x480, kArenaFieldSize = 0x484;
constexpr uint32_t kArenaFieldUsed = 0x488, kArenaFieldPeak = 0x48C, kArenaFieldNext = 0x490;
constexpr uint32_t kArenaHeaderBytes = 0x494, kArenaSlots = 8;
constexpr uint32_t kPhysicalMask = 0x1FFFFFFF;  // the GPU's 29-bit physical address
constexpr int kStackDepth = 6;                  // return addresses kept per call site

// --- The switch, read once (it needs a restart) ------------------------------------------------------------------

std::atomic<int> g_mode{-1};  // -1 not read yet, 0 off, 1 on
bool On() {
  int m = g_mode.load(std::memory_order_relaxed);
  if (m >= 0) return m == 1;
  m = REXCVAR_GET(cod3_native_memory_shadow) ? 1 : 0;
  int expected = -1;
  if (g_mode.compare_exchange_strong(expected, m) && m == 1)
    REXLOG_WARN("cod3: memory shadow on: every arena-0 allocation is recorded and checked against what the game "
                "hands Direct3D (slower; a test)");
  return g_mode.load(std::memory_order_relaxed) == 1;
}

// --- Call sites ----------------------------------------------------------------------------------------------------

struct Key {
  uint32_t pc[kStackDepth] = {};
  bool operator==(const Key& o) const { return std::equal(pc, pc + kStackDepth, o.pc); }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v : k.pc) h = (h ^ v) * 1099511628211ull;
    return size_t(h);
  }
};

// The guest call stack: the hook's return address, then each frame's saved return address along the back chain
// (as heap_check.cpp does; the PCR's +0x70 / +0x74 bound the thread's stack).
Key Capture(const PPCContext& ctx, uint8_t* base) {
  using rex::system::XThread;
  Key k;
  k.pc[0] = uint32_t(ctx.lr);
  if (!XThread::IsInThread()) return k;
  XThread* t = XThread::GetCurrentThread();
  const uint32_t pcr = t ? t->pcr_ptr() : 0;
  if (!pcr) return k;
  const uint32_t top = ReadU32(base, pcr + 0x70), end = ReadU32(base, pcr + 0x74);
  if (!(end && end < top && top - end <= (64u << 20))) return k;
  uint32_t sp = ctx.r1.u32;
  for (int i = 1; i < kStackDepth; ++i) {
    if ((sp & 3) || sp < end || sp + 4 > top) break;
    const uint32_t caller = ReadU32(base, sp);
    if (caller <= sp || caller > top || (caller & 3) || caller - 8 < end) break;
    k.pc[i] = ReadU32(base, caller - 8);
    sp = caller;
  }
  return k;
}

struct Site {
  Key key;
  uint64_t allocations = 0, live_bytes = 0, peak_bytes = 0, gpu_hits = 0;
  uint32_t live_blocks = 0, largest = 0;
  const char* first_hit = nullptr;  // what first reached the graphics side
};

struct Block {
  uint32_t end;   // physical, one past the last byte
  uint32_t site;  // index into g_sites
  uint32_t size;
};

std::mutex g_mu;
std::map<uint32_t, Block> g_blocks;  // by physical start (address & kPhysicalMask)
std::vector<Site> g_sites;
std::unordered_map<Key, uint32_t, KeyHash> g_site_index;
uint64_t g_checks = 0, g_hits = 0;
thread_local int t_depth = 0;  // the allocate / free functions call themselves for other arenas

void Record(uint32_t block, uint32_t size, const Key& key) {
  const uint32_t start = block & kPhysicalMask;
  std::lock_guard lock(g_mu);
  auto [it, added] = g_site_index.try_emplace(key, uint32_t(g_sites.size()));
  if (added) g_sites.push_back(Site{key});
  Site& s = g_sites[it->second];
  ++s.allocations;
  ++s.live_blocks;
  s.live_bytes += size;
  s.peak_bytes = std::max(s.peak_bytes, s.live_bytes);
  s.largest = std::max(s.largest, size);
  g_blocks[start] = Block{start + size, it->second, size};
}

// The site of a recorded block (UINT32_MAX when none), and the block forgotten.
uint32_t Forget(uint32_t block) {
  const uint32_t start = block & kPhysicalMask;
  std::lock_guard lock(g_mu);
  auto it = g_blocks.find(start);
  if (it == g_blocks.end()) return UINT32_MAX;
  const uint32_t site = it->second.site;
  Site& s = g_sites[site];
  --s.live_blocks;
  s.live_bytes -= it->second.size;
  g_blocks.erase(it);
  return site;
}

bool InArena0(uint8_t* base, uint32_t block) {
  const uint32_t lo = ReadU32(base, kArenaHeaders + kArenaFieldStart);
  const uint32_t hi = ReadU32(base, kArenaHeaders + kArenaFieldEnd);
  return lo && block >= lo && block < hi;
}

// --- The move ------------------------------------------------------------------------------------------------------

// The call sites whose allocations move (kStackDepth return addresses each, as the shadow logs them). Only sites
// that never reached the graphics side in any shadow run (dev/agents/memory-360/NOTES.md lists the runs: saint_lo,
// forest, mace2, chambois so far, the same sites in each); a site not listed stays in arena 0. One table per
// program (a site's addresses differ in cod3mp).
struct Listed {
  uint32_t pc[kStackDepth];
  const char* what;
};
#if !defined(COD3_MP)
constexpr Listed kMoveList[] = {
    // Sys_Main's three calls of 0x821540E8 take two 12 MB blocks each (the first two pairs are freed once the game
    // is up). Shadow: saint_lo, forest (c3shadow, c4shadow).
    {{0x820CC30C, 0x82522DF4, 0x82127AB8, 0x821542C4, 0x82400178, 0x82344E98}, "Sys_Main 12 MB buffer (1a)"},
    {{0x820CC30C, 0x82522DF4, 0x82127AB8, 0x821542D8, 0x82400178, 0x82344E98}, "Sys_Main 12 MB buffer (1b)"},
    {{0x820CC30C, 0x82522DF4, 0x82127AB8, 0x821542C4, 0x82400188, 0x82344E98}, "Sys_Main 12 MB buffer (2a)"},
    {{0x820CC30C, 0x82522DF4, 0x82127AB8, 0x821542D8, 0x82400188, 0x82344E98}, "Sys_Main 12 MB buffer (2b)"},
    {{0x820CC30C, 0x82522DF4, 0x82127AB8, 0x821542C4, 0x824001A0, 0x82344E98}, "Sys_Main 12 MB buffer (3a)"},
    {{0x820CC30C, 0x82522DF4, 0x82127AB8, 0x821542D8, 0x824001A0, 0x82344E98}, "Sys_Main 12 MB buffer (3b)"},
    // [memory-360] 2026-11-11 14:20: Sys_Main's AnimHeap (operator new(0x400000), an arena of its own inside,
    // 0x820CB580) is OFF the list: it holds the soldiers' bone matrices (object +0xA8), which the native renderer
    // reads only from physical memory (ngl_side.cpp SkinPosition: `if (!Physical(bones) ...) return false`), so with
    // it moved the soldiers were drawn unposed (the owner's picture, forest under Wine, 14:14). The shadow never saw
    // it: it watches what the game hands Direct3D and XMA, not what the native renderer reads from game memory.
    // The particle system's pools (0x820F8BC0 through Sys_Main's 0x82418C80(0x200000)): 4 MB and 2 MB; and its
    // 2 MB heap (operator new in 0x820CC540, a heap object with an arena of its own inside).
    {{0x820CC30C, 0x82522DF4, 0x82418C10, 0x820F8C1C, 0x82418D28, 0x82400398}, "particle pool 4 MB"},
    {{0x820CC30C, 0x82522DF4, 0x82418C10, 0x820F8C1C, 0x82418D1C, 0x82400398}, "particle pool 2 MB"},
    {{0x820CC21C, 0x820CC570, 0x82418D40, 0x82400398, 0x82344E98, 0}, "particle heap 2 MB"},
    // The sound library's bank memory (NSL init 0x8211AD98: 0x23D7840 bytes, 35.8 MB). FMOD decodes with the XMA
    // hardware contexts, which read their buffers physically, but those buffers are in FMOD's own 20 MB (the next
    // allocation, 0x8211AE2C: the XMA check found them there in mace2 and chambois, so it stays); the sound data is
    // copied from the bank memory into them by the CPU (0x823B80A8), and the mixed sound goes to the SDK by its
    // virtual address (sdl_audio_driver.cpp TranslateVirtual).
    {{0x820CC30C, 0x82522DF4, 0x82127AB8, 0x8211AE04, 0x82117CE0, 0x82492F98}, "sound bank memory 35.8 MB"},
};
#else
// cod3mp (shadow: mp_merv, rouen, c4shadowmp; the same roles as the campaign's, at cod3mp's addresses).
constexpr Listed kMoveList[] = {
    // Its start-up's three calls of 0x8216C0xx take two 16 MB blocks each.
    {{0x820DE820, 0x82548CE8, 0x8213F818, 0x8216C1BC, 0x8241804C, 0x82314260}, "start-up 16 MB buffer (1a)"},
    {{0x820DE820, 0x82548CE8, 0x8213F818, 0x8216C1D0, 0x8241804C, 0x82314260}, "start-up 16 MB buffer (1b)"},
    {{0x820DE820, 0x82548CE8, 0x8213F818, 0x8216C1BC, 0x8241805C, 0x82314260}, "start-up 16 MB buffer (2a)"},
    {{0x820DE820, 0x82548CE8, 0x8213F818, 0x8216C1D0, 0x8241805C, 0x82314260}, "start-up 16 MB buffer (2b)"},
    {{0x820DE820, 0x82548CE8, 0x8213F818, 0x8216C1BC, 0x82418074, 0x82314260}, "start-up 16 MB buffer (3a)"},
    {{0x820DE820, 0x82548CE8, 0x8213F818, 0x8216C1D0, 0x82418074, 0x82314260}, "start-up 16 MB buffer (3b)"},
    // AnimHeap 4 MB (0x820DE728, 0x824182F4, 0x82314260): off the list, as the campaign's (bone matrices, 14:20).
    {{0x820DE820, 0x82548CE8, 0x82437370, 0x8210AB8C, 0x82430B00, 0x824182C0}, "particle pool 4 MB"},
    {{0x820DE820, 0x82548CE8, 0x82437370, 0x8210AB8C, 0x82430AF4, 0x824182C0}, "particle pool 2 MB"},
    {{0x820DE728, 0x820DEAA0, 0x82430B18, 0x824182C0, 0x82314260, 0}, "particle heap 2 MB"},
    // The sound library's bank memory, 8.5 MB in cod3mp (its FMOD 16 MB, the next allocation 8212D224, holds the
    // XMA buffers and stays).
    {{0x820DE820, 0x82548CE8, 0x8213F818, 0x8212D1FC, 0x82129848, 0x824AB8FC}, "sound bank memory 8.5 MB"},
};
#endif

// Stage 5's test (cod3_native_memory_move_pool): the streaming pool, Com_Init's streaming setup taking num_banks x
// 3 MB in one piece through 0x820CC2A0 and its cod3mp twin (shadow site "graphics most 245760 KB" / "270336 KB", one
// allocation, c4shadow / c5moveshadow / c4shadowmp; never the XMA decoder's).
#if !defined(COD3_MP)
constexpr Listed kPoolSite = {{0x820CC30C, 0x8245DA54, 0x825375A8, 0x82400458, 0x82344E98, 0}, "streaming pool"};
#else
constexpr Listed kPoolSite = {{0x820DE820, 0x824796B0, 0x82558684, 0x82418330, 0x82314260, 0}, "streaming pool"};
#endif
constexpr uint32_t kPoolRoomNeeded = 512u << 20;

// [memory-360] 2026-11-11 14:30 (rexglue-native 14:21 asked for it, to test ngl_side's bone reads): the AnimHeap,
// off the move's list since 14:20, moves only with cod3_native_memory_move_animheap (off).
#if !defined(COD3_MP)
constexpr Listed kAnimHeapSite = {{0x820CC21C, 0x8240041C, 0x82344E98, 0, 0, 0}, "AnimHeap 4 MB (test)"};
#else
constexpr Listed kAnimHeapSite = {{0x820DE728, 0x824182F4, 0x82314260, 0, 0, 0}, "AnimHeap 4 MB (test)"};
#endif  // the move's room the pool needs (with up to 96 banks and the rest)

constexpr uint32_t kMoveBytesWanted[] = {512u << 20, 256u << 20, 128u << 20};  // the first that fits
constexpr uint32_t kMoveMinBytes = 1024;  // smaller requests stay (the game's small-block pools serve them)

std::atomic<int> g_move{-1};             // -1 not read yet, 0 off, 1 on
std::atomic<uint32_t> g_move_header{0};  // the header in use (0: not made yet)
std::atomic<bool> g_move_failed{false}, g_move_reserve_tried{false};
std::atomic<uint32_t> g_move_lo{0}, g_move_hi{0};
std::atomic<uint64_t> g_moved{0}, g_move_fallbacks{0}, g_moved_seen_by_gpu{0};
std::mutex g_move_mu;

bool MoveOn() {
  int m = g_move.load(std::memory_order_relaxed);
  if (m >= 0) return m == 1;
  m = 0;
  if (REXCVAR_GET(cod3_native_memory_move)) {
    if (!REXCVAR_GET(cod3_native_vulkan)) {
      REXLOG_WARN("cod3: memory move: cod3_native_memory_move is on, but it is for the native renderer "
                  "(cod3_native_vulkan); nothing moves");
    } else if (kMoveList[0].what == nullptr) {
      REXLOG_WARN("cod3: memory move: cod3_native_memory_move is on, but this program has no list of call sites "
                  "yet; nothing moves");
    } else {
      m = 1;
    }
  }
  int expected = -1;
  if (g_move.compare_exchange_strong(expected, m) && m == 1)
    REXLOG_INFO("cod3: memory move on: {} call sites of the game's go to guest virtual memory",
                std::size(kMoveList));
  return g_move.load(std::memory_order_relaxed) == 1;
}

bool InMoveArena(uint32_t address) {
  const uint32_t lo = g_move_lo.load(std::memory_order_relaxed);
  return lo && address >= lo && address < g_move_hi.load(std::memory_order_relaxed);
}

uint32_t Reserve();

// Stage 5's test: the streaming pool's block (wherever it is; noted when the game makes it), and whether it moves.
std::atomic<int> g_pool{-1};  // -1 not read yet, 0 off, 1 on
std::atomic<uint32_t> g_pool_lo{0}, g_pool_hi{0};
std::atomic<uint64_t> g_pool_gpu_reads{0}, g_pool_locks{0};
// The pool's bounds once it was moved (GpuName / GpuRoom / InMovedPool): 0 while it isn't. hi is stored before lo,
// and lo read first (acquire), so a reader that sees lo sees its hi.
std::atomic<uint32_t> g_moved_pool_lo{0}, g_moved_pool_hi{0};

bool IsPoolSite(const Key& key) { return std::equal(kPoolSite.pc, kPoolSite.pc + kStackDepth, key.pc); }

bool InPool(uint32_t address) {
  const uint32_t lo = g_pool_lo.load(std::memory_order_relaxed);
  return lo && address >= lo && address < g_pool_hi.load(std::memory_order_relaxed);
}

bool PoolOn() {
  int p = g_pool.load(std::memory_order_relaxed);
  if (p >= 0) return p == 1;
  p = 0;
  if (REXCVAR_GET(cod3_native_memory_move_pool)) {
    if (!MoveOn()) {
      REXLOG_WARN("cod3: memory move: cod3_native_memory_move_pool is on, but it goes with cod3_native_memory_move "
                  "(off, or not usable here); the streaming pool stays");
    } else if (const uint32_t room = Reserve(); room < kPoolRoomNeeded) {
      REXLOG_WARN("cod3: memory move: the move's room is {} MB, the streaming pool needs {} MB of it; the pool stays",
                  room >> 20, kPoolRoomNeeded >> 20);
    } else {
      p = 1;
    }
  }
  int expected = -1;
  if (g_pool.compare_exchange_strong(expected, p) && p == 1)
    REXLOG_INFO("cod3: memory move: the streaming pool goes to guest virtual memory too (stage 5, a test: the "
                "native renderer must read its addresses as they are)");
  return g_pool.load(std::memory_order_relaxed) == 1;
}

const Listed* OnMoveList(const Key& key) {
  for (const Listed& l : kMoveList) {
    if (l.what && std::equal(l.pc, l.pc + kStackDepth, key.pc)) return &l;
  }
  if (IsPoolSite(key) && PoolOn()) return &kPoolSite;
  if (std::equal(kAnimHeapSite.pc, kAnimHeapSite.pc + kStackDepth, key.pc) &&
      REXCVAR_GET(cod3_native_memory_move_animheap))
    return &kAnimHeapSite;
  return nullptr;
}

void NotePool(const Key& key, uint32_t block, uint32_t size, bool moved) {
  if (!block || !IsPoolSite(key)) return;
  // The game makes the pool once (Com_Init's streaming set-up); a second one would mean bounds that change under the
  // native renderer's reads, so it is told.
  if (const uint32_t before = g_pool_lo.load(std::memory_order_relaxed); before && before != block)
    REXLOG_WARN("cod3: memory: the streaming pool was made again ({:08X}, before {:08X}); stage 5's bounds follow it",
                block, before);
  g_pool_lo.store(block, std::memory_order_relaxed);
  g_pool_hi.store(block + size, std::memory_order_relaxed);
  if (moved) {
    g_moved_pool_hi.store(block + size, std::memory_order_relaxed);
    g_moved_pool_lo.store(block, std::memory_order_release);
  }
}

// The move's arena: guest virtual memory and a free header of the game's eight, made the first time a listed
// allocation comes after the game set up its arenas (its set-up, 0x820CB890, zeroes all eight headers). Returns the
// header, 0 when there is none (then nothing moves).
// The move's guest virtual memory, taken once: at the arena maker (native_memory.cpp, so stage 2 knows the move will
// make room in arena 0) or else at the first listed allocation. Returns its size (0: none; then nothing moves).
uint32_t Reserve() {
  const auto reserved = [] { return g_move_hi.load(std::memory_order_relaxed) - g_move_lo.load(std::memory_order_relaxed); };
  if (g_move_reserve_tried.load(std::memory_order_acquire)) return reserved();
  std::lock_guard lock(g_move_mu);
  if (g_move_reserve_tried.load(std::memory_order_acquire)) return reserved();
  auto* ks = rex::system::kernel_state();
  auto* heap = ks && ks->memory() ? ks->memory()->LookupHeap(0x40000000u) : nullptr;
  uint32_t start = 0, bytes = 0;
  for (uint32_t want : kMoveBytesWanted) {
    if (heap && heap->AllocRange(0x40000000u, 0x7EFFFFFFu, want, 0x10000u,
                                 rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                                 rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                                 /*top_down=*/true, &start)) {
      bytes = want;
      break;
    }
  }
  if (bytes) {
    g_move_lo.store(start, std::memory_order_relaxed);
    g_move_hi.store(start + bytes, std::memory_order_relaxed);
  } else {
    REXLOG_WARN("cod3: memory move: no room in guest virtual memory (0x40000000 range) for the move's arena; "
                "nothing moves");
  }
  g_move_reserve_tried.store(true, std::memory_order_release);
  return bytes;
}

uint32_t MoveArena(uint8_t* base) {
  if (uint32_t h = g_move_header.load(std::memory_order_acquire)) return h;
  if (g_move_failed.load(std::memory_order_relaxed) || ReadU32(base, kArenaReady) == 0) return 0;
  const uint32_t bytes = Reserve();
  std::lock_guard lock(g_move_mu);
  if (uint32_t h = g_move_header.load(std::memory_order_acquire)) return h;
  if (g_move_failed.load(std::memory_order_relaxed)) return 0;
  g_move_failed.store(true, std::memory_order_relaxed);  // until made
  if (!bytes) return 0;

  // A free header, from the last: the game makes arenas of its own at run time with 0x820CB5E0, which takes the
  // first free one, and uses 0 and 2 by number.
  uint32_t header = 0;
  for (uint32_t i = kArenaSlots - 1; i > 2; --i) {
    const uint32_t h = kArenaHeaders + i * kArenaHeaderBytes;
    if (ReadU32(base, h + kArenaFieldSize) == 0) {
      header = h;
      break;
    }
  }
  if (!header) {
    REXLOG_WARN("cod3: memory move: the game's arena headers 3 to 7 are all in use; nothing moves");
    return 0;
  }
  // As 0x820CB5E0 does: the dlmalloc state zeroed, then the arena's fields.
  std::memset(cod3::guest::Ptr(base, header), 0, kArenaHeaderBytes);
  const uint32_t start = g_move_lo.load(std::memory_order_relaxed), end = start + bytes;
  cod3::guest::WriteU32(base, header + kArenaFieldStart, start);
  cod3::guest::WriteU32(base, header + kArenaFieldEnd, end);
  cod3::guest::WriteU32(base, header + kArenaFieldBreak, start);
  cod3::guest::WriteU32(base, header + kArenaFieldBreakEnd, end);
  cod3::guest::WriteU32(base, header + kArenaFieldSize, bytes);
  cod3::guest::WriteU32(base, header + kArenaFieldUsed, 0);
  cod3::guest::WriteU32(base, header + kArenaFieldPeak, 0);
  cod3::guest::WriteU32(base, header + kArenaFieldNext, 0);
  g_move_failed.store(false, std::memory_order_relaxed);
  g_move_header.store(header, std::memory_order_release);
  REXLOG_INFO("cod3: memory move: arena header {} ({:08X}) now holds {} MB of guest virtual memory at {:08X}-{:08X}",
              (header - kArenaHeaders) / kArenaHeaderBytes, header, bytes >> 20, start, end);
  return header;
}

// A moved block handed to the graphics side or the XMA decoder by its own (virtual) address: an error (the call
// site must come off the move's list). True when it was one. Stage 5's test: the moved streaming pool is read by the
// graphics side on purpose (counted, not an error); by the XMA decoder (graphics false) it is still an error.
bool ReportIfMoved(uint32_t address, const char* what, bool graphics = true) {
  if (!InMoveArena(address)) return false;
  if (graphics && InPool(address)) {
    g_pool_gpu_reads.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  if (g_moved_seen_by_gpu.fetch_add(1, std::memory_order_relaxed) < 8)
    REXLOG_ERROR("cod3: memory move: {} at {:08X} is in a block moved to guest virtual memory; the graphics side "
                 "cannot read it there (a call site on the move's list must come off it)",
                 what, address);
  return true;
}

void Check(uint32_t address, const char* what, bool graphics = true) {
  if (address == 0 || ReportIfMoved(address, what, graphics)) return;
  // The physical address, as Direct3D makes it (SetTexture, SetStreamSource): the low 29 bits, plus 4 KB for the
  // 0xE0000000 view (the console's own offset there).
  const uint32_t a = (address & kPhysicalMask) + (address >= 0xE0000000u ? 0x1000u : 0u);
  std::lock_guard lock(g_mu);
  ++g_checks;
  auto it = g_blocks.upper_bound(a);
  if (it == g_blocks.begin()) return;
  --it;
  if (a >= it->second.end) return;
  ++g_hits;
  Site& s = g_sites[it->second.site];
  if (s.gpu_hits++ == 0) {
    s.first_hit = what;
    REXLOG_INFO("cod3: memory shadow: {} at {:08X} is in a block of {} bytes from {:08X} <- {:08X} <- {:08X} <- "
                "{:08X} (site {})",
                what, address, it->second.size, s.key.pc[0], s.key.pc[1], s.key.pc[2], s.key.pc[3],
                it->second.site);
  }
}

// What the hooks below call for each address handed over: the shadow's check when it is on, else (the move on) only
// whether the address is a moved block's.
bool Watching() { return On() || g_move_lo.load(std::memory_order_relaxed) != 0; }

void Watch(uint32_t address, const char* what, bool graphics = true) {
  if (g_mode.load(std::memory_order_relaxed) == 1) Check(address, what, graphics);
  else if (address) ReportIfMoved(address, what, graphics);
}

// Stage 5's check: the game locking a resource whose data is in the streaming pool (Direct3D hands back the header's
// own address as the CPU pointer: D3DVertexBuffer_Lock +0x18 & ~3, D3DIndexBuffer_Lock +0x18, a texture level from
// its base +0x20 through D3D_LockTextureLevel; docs/COD3_D3D.md, d3d_names.tsv). Counted, the first 8 logged.
void NotePoolLock(uint32_t address, const char* what, uint32_t caller) {
  if (!InPool(address)) return;
  if (g_pool_locks.fetch_add(1, std::memory_order_relaxed) < 8)
    REXLOG_INFO("cod3: memory pool: the game locks {} in the streaming pool at {:08X} (from {:08X}){}", what, address,
                caller, InMoveArena(address) ? ", moved" : "");
}

// Once a minute: the totals, every site that reached the graphics side, and the 12 largest that never did.
void SummaryThread() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
    std::vector<Site> sites;
    uint64_t checks, hits;
    size_t blocks;
    {
      std::lock_guard lock(g_mu);
      sites = g_sites;
      checks = g_checks;
      hits = g_hits;
      blocks = g_blocks.size();
    }
    uint64_t gpu_bytes = 0, cpu_bytes = 0, gpu_sites = 0;
    for (const Site& s : sites) {
      if (s.gpu_hits) gpu_bytes += s.peak_bytes, ++gpu_sites;
      else cpu_bytes += s.peak_bytes;
    }
    REXLOG_INFO("cod3: memory shadow: {} call sites, {} live blocks; {} graphics addresses checked, {} in a block; "
                "sites reaching the graphics side {} (their most {} MB), never {} (their most {} MB)",
                sites.size(), blocks, checks, hits, gpu_sites, gpu_bytes >> 20, sites.size() - gpu_sites,
                cpu_bytes >> 20);
    std::vector<uint32_t> order(sites.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](uint32_t a, uint32_t b) { return sites[a].peak_bytes > sites[b].peak_bytes; });
    // Each site not listed before, or listed before it reached the graphics side: one line, for merging runs.
    static std::vector<uint8_t> listed;  // per site: 0 not yet, 1 as cpu only, 2 as graphics
    listed.resize(sites.size(), 0);
    for (uint32_t i = 0; i < sites.size(); ++i) {
      const Site& s = sites[i];
      const uint8_t state = s.gpu_hits ? 2 : 1;
      if (listed[i] == state) continue;
      listed[i] = state;
      REXLOG_INFO("cod3: memory shadow list: site {} {} most {} KB, {} allocations; {:08X} {:08X} {:08X} {:08X} {:08X} "
                  "{:08X}",
                  i, state == 2 ? "graphics" : "cpu", s.peak_bytes >> 10, s.allocations, s.key.pc[0], s.key.pc[1],
                  s.key.pc[2], s.key.pc[3], s.key.pc[4], s.key.pc[5]);
    }
    int cpu_logged = 0;
    for (uint32_t i : order) {
      const Site& s = sites[i];
      const bool gpu = s.gpu_hits != 0;
      if (!gpu && cpu_logged >= 12) continue;
      if (!gpu) ++cpu_logged;
      REXLOG_INFO("cod3: memory shadow site {}: {} ({}{}) most {} KB, now {} KB in {} blocks, {} allocations, largest "
                  "{} KB; {:08X} <- {:08X} <- {:08X} <- {:08X} <- {:08X} <- {:08X}",
                  i, gpu ? "graphics" : "cpu only", gpu ? s.first_hit : "", gpu ? "" : "never seen by the graphics side",
                  s.peak_bytes >> 10, s.live_bytes >> 10, s.live_blocks, s.allocations, s.largest >> 10, s.key.pc[0],
                  s.key.pc[1], s.key.pc[2], s.key.pc[3], s.key.pc[4], s.key.pc[5]);
    }
  }
}

void StartSummary() {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(&SummaryThread).detach(); });
}

}  // namespace

namespace cod3::native_memory {

void CheckGpuRead(uint32_t address) {
  if (g_mode.load(std::memory_order_relaxed) == 1) Check(address, "a native renderer read");
  else if (address && g_move_lo.load(std::memory_order_relaxed)) ReportIfMoved(address, "a native renderer read");
}

uint32_t MoveArenaHeader() { return g_move_header.load(std::memory_order_acquire); }

uint32_t MoveReserve() { return MoveOn() ? Reserve() : 0; }

uint64_t MoveFallbacks() { return g_move_fallbacks.load(std::memory_order_relaxed); }

bool MovePoolOn() { return PoolOn(); }

bool InMovedMemory(uint32_t address) { return InMoveArena(address); }

bool InMovedPool(uint32_t address) {
  const uint32_t lo = g_moved_pool_lo.load(std::memory_order_acquire);
  return lo && address >= lo && address < g_moved_pool_hi.load(std::memory_order_relaxed);
}

uint32_t GpuName(uint32_t word, uint32_t mask) {
  if (InMovedPool(word)) return word & (mask | 0xE0000000u);
  return 0xA0000000u | (word & mask);
}

size_t GpuRoom(uint32_t name) {
  if (const uint32_t lo = g_moved_pool_lo.load(std::memory_order_acquire); lo && name >= lo) {
    const uint32_t hi = g_moved_pool_hi.load(std::memory_order_relaxed);
    if (name < hi) return hi - name;
  }
  if (name >= 0xA0000000u && name < 0xC0000000u) return 0x20000000u - (name & 0x1FFFFFFFu);
  return 0;
}

PoolStats Pool() {
  PoolStats s;
  s.start = g_pool_lo.load(std::memory_order_relaxed);
  s.end = g_pool_hi.load(std::memory_order_relaxed);
  s.moved = s.start && InMoveArena(s.start);
  s.graphics_reads = g_pool_gpu_reads.load(std::memory_order_relaxed);
  s.locks = g_pool_locks.load(std::memory_order_relaxed);
  return s;
}

}  // namespace cod3::native_memory

// --- The arena's allocate, free and resize -------------------------------------------------------------------------

REX_EXTERN(__imp__sub_820CBC40);
REX_HOOK_RAW(sub_820CBC40) {
  const bool shadow = On(), move = MoveOn();
  if ((!shadow && !move) || t_depth > 0) {
    __imp__sub_820CBC40(ctx, base);
    return;
  }
  if (shadow) StartSummary();
  // [memory-360] 2026-11-11 c2shadow: r4 is the size (r3 the alignment); the first build read r3, so every block
  // was recorded as 16 bytes.
  const uint32_t alignment = ctx.r3.u32, size = ctx.r4.u32, arena = ReadU32(base, kCurrentArena);
  const bool may_move = move && arena == kArenaHeaders && size >= kMoveMinBytes;
  if (!shadow && !may_move) {
    __imp__sub_820CBC40(ctx, base);
    return;
  }
  const Key key = Capture(ctx, base);
  // The move: a listed site's allocation from arena 0, with the move's arena current (as 0x820CC2A0 does for a
  // given arena). Its blocks are the game's own chunks there; when that arena is full, arena 0 as before.
  const Listed* listed = may_move ? OnMoveList(key) : nullptr;
  if (listed) {
    if (const uint32_t header = MoveArena(base)) {
      cod3::guest::WriteU32(base, kCurrentArena, header);
      ++t_depth;
      __imp__sub_820CBC40(ctx, base);
      --t_depth;
      cod3::guest::WriteU32(base, kCurrentArena, arena);
      if (ctx.r3.u32) {
        NotePool(key, ctx.r3.u32, size, true);
        if (g_moved.fetch_add(1, std::memory_order_relaxed) < 32)
          REXLOG_INFO("cod3: memory move: {} ({} bytes) at {:08X}", listed->what, size, ctx.r3.u32);
        return;
      }
      if (g_move_fallbacks.fetch_add(1, std::memory_order_relaxed) < 8)
        REXLOG_WARN("cod3: memory move: the move's arena had no room for {} bytes; arena 0 instead", size);
      ctx.r3.u64 = alignment;
      ctx.r4.u64 = size;
    }
  }
  ++t_depth;
  __imp__sub_820CBC40(ctx, base);
  --t_depth;
  const uint32_t block = ctx.r3.u32;
  if (block && arena == kArenaHeaders) NotePool(key, block, size, false);
  if (shadow && block && arena == kArenaHeaders && InArena0(base, block))
    Record(block, std::max<uint32_t>(size, 16), key);
}

REX_EXTERN(__imp__sub_820CBF58);
REX_HOOK_RAW(sub_820CBF58) {
  if (On() && t_depth == 0 && ctx.r3.u32) Forget(ctx.r3.u32);
  ++t_depth;
  __imp__sub_820CBF58(ctx, base);
  --t_depth;
}

REX_EXTERN(__imp__sub_820CB778);
REX_HOOK_RAW(sub_820CB778) {
  const uint32_t old_block = ctx.r3.u32, size = ctx.r4.u32;
  // The move: the game resizes in the current arena (its dlmalloc state); a moved block is resized in the move's.
  if (InMoveArena(old_block)) {
    const uint32_t arena = ReadU32(base, kCurrentArena), header = g_move_header.load(std::memory_order_acquire);
    cod3::guest::WriteU32(base, kCurrentArena, header);
    __imp__sub_820CB778(ctx, base);
    cod3::guest::WriteU32(base, kCurrentArena, arena);
    return;
  }
  if (!On()) {
    __imp__sub_820CB778(ctx, base);
    return;
  }
  const Key key = Capture(ctx, base);
  __imp__sub_820CB778(ctx, base);
  const uint32_t block = ctx.r3.u32;
  if (!block) return;  // not resized: the old block stays as it was
  const uint32_t site = old_block ? Forget(old_block) : UINT32_MAX;
  if (!InArena0(base, block)) return;
  if (site != UINT32_MAX) {
    Key old_key;
    {
      std::lock_guard lock(g_mu);
      old_key = g_sites[site].key;
    }
    Record(block, size, old_key);  // the block keeps the call site that made it
  } else {
    Record(block, size, key);
  }
}

// --- What the game hands the XMA audio decoder ----------------------------------------------------------------------

// [memory-360] 2026-11-11 FMOD's XMA codec start (0x823B7ED8 / 0x82387560, 464 bytes in both; r3 the codec). The
// codec decodes with the SDK's hardware XMA contexts (XMAInitializeContext, XMASetInputBuffer0/1Valid ...), which
// read and write their buffers by physical address: the codec's XMA block at [codec + 0xD8] holds the context
// handle at +0, the two 2 KB input buffers at +4 and +8, the output buffer at +0xC and the work buffer at +0x10
// (the init call in the decomp, 823B0000.c). The sound data itself is copied into the input buffers by the CPU
// (0x823B80A8 reads 0x800 bytes at a time), so only these buffers are the decoder's.
REX_EXTERN(__imp__sub_823B7ED8);
REX_HOOK_RAW(sub_823B7ED8) {
  const uint32_t codec = ctx.r3.u32;
  __imp__sub_823B7ED8(ctx, base);
  if ((!On() && !g_move_lo.load(std::memory_order_relaxed)) || !codec) return;
  const uint32_t xma = ReadU32(base, codec + 0xD8);
  if (!xma) return;
  for (uint32_t offset : {0x4u, 0x8u, 0xCu, 0x10u})
    Watch(ReadU32(base, xma + offset), "an XMA audio buffer", /*graphics=*/false);
}

// --- What the game hands Direct3D ----------------------------------------------------------------------------------

REX_EXTERN(__imp__sub_822E9208);
REX_HOOK_RAW(sub_822E9208) {  // SetStreamSource
  if (ctx.r5.u32 && Watching()) Watch(ReadU32(base, ctx.r5.u32 + 0x18) + ctx.r6.u32, "a vertex buffer");
  __imp__sub_822E9208(ctx, base);
}

REX_EXTERN(__imp__sub_822E93B8);
REX_HOOK_RAW(sub_822E93B8) {  // SetIndices
  if (ctx.r4.u32 && Watching()) Watch(ReadU32(base, ctx.r4.u32 + 0x18), "an index buffer");
  __imp__sub_822E93B8(ctx, base);
}

REX_EXTERN(__imp__sub_822EBE40);
REX_HOOK_RAW(sub_822EBE40) {  // SetTexture
  if (ctx.r5.u32 && Watching()) {
    const uint32_t texture = ctx.r5.u32;
    Watch(ReadU32(base, texture + 0x20) & 0xFFFFF000u, "a texture");
    Watch(ReadU32(base, texture + 0x30) & 0xFFFFF000u, "a texture's mip levels");
  }
  __imp__sub_822EBE40(ctx, base);
}

// --- Stage 5's check: the game's locks of pool resources ------------------------------------------------------------

// [memory-360] 2026-11-11 D3DBaseTexture_LockRect_Internal (0x822EB1D0 / 0x822B9F18, 140 bytes in both; r3 the
// texture; every texture, cube and surface lock goes through it), D3DVertexBuffer_Lock (0x822F14D8 / 0x822C01F8,
// 96 bytes; r3 the buffer) and D3DIndexBuffer_Lock (0x822F15F8 / 0x822C0318, 92 bytes; r3 the buffer). Names and
// twins: dev/x360-family/d3d/d3d_names.tsv, d3d_mp_twins.tsv. Nothing changes; the lock is noted when its data is
// in the streaming pool.
REX_EXTERN(__imp__sub_822EB1D0);
REX_HOOK_RAW(sub_822EB1D0) {
  if (ctx.r3.u32 && Watching())
    NotePoolLock(ReadU32(base, ctx.r3.u32 + 0x20) & 0xFFFFF000u, "a texture", static_cast<uint32_t>(ctx.lr));
  __imp__sub_822EB1D0(ctx, base);
}

REX_EXTERN(__imp__sub_822F14D8);
REX_HOOK_RAW(sub_822F14D8) {
  if (ctx.r3.u32 && Watching())
    NotePoolLock(ReadU32(base, ctx.r3.u32 + 0x18) & ~3u, "a vertex buffer", static_cast<uint32_t>(ctx.lr));
  __imp__sub_822F14D8(ctx, base);
}

REX_EXTERN(__imp__sub_822F15F8);
REX_HOOK_RAW(sub_822F15F8) {
  if (ctx.r3.u32 && Watching())
    NotePoolLock(ReadU32(base, ctx.r3.u32 + 0x18), "an index buffer", static_cast<uint32_t>(ctx.lr));
  __imp__sub_822F15F8(ctx, base);
}
