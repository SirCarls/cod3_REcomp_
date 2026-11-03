// cod3 - [claude-9b-think] 2026-11-02 the render lists' frame memory: measured, given room past its end, and the
// render back end's scene nodes and the effects pool checked (both programs since [mp]; the physical pages, the
// effects pool and the level-load lines campaign only). UNTESTED.
//
// Each drawn frame's render lists (the scene nodes of 0x8212DB18, quads, strings, the grass, the objects' draw
// items) are recorded in a double-buffered frame memory: two buffers of 4 MB (Sys_Main -> 0x821540E8(0,
// 0x400000); 0x82127A20 allocates them; their addresses at 0x82912D00 and 0x82912D04), taken in turn by
// 0x82130808 when the next frame opens. Every allocation only moves the cursor 0x8290E9BC on (about 95 places,
// inlined) and none checks the end: the grass's "unable to get %d bytes from nglListAlloc" only tests for 0. A
// frame that needs more than its buffer writes on past the end: into the other buffer (the frame the render job
// is drawing on a worker: its scene nodes and draw items) or into whatever was allocated after it.
//
// ob19 (owner 2026-11-02, mace2, the same place in both runs):
//  - 60 fps, main thread: the effects pool's manager (0x82A2ACCC -> A92C6AA0, made by Sys_Main after the list
//    buffers; vtable 0x82001378) held {86.7, -2765.3, 311.4, 7.39} where its vtable and first fields were, and
//    FX_PlayFX -> 0x8241B2B0 called its slot +16: "call to 0";
//  - ~200 fps, render job: scene node A8EC6D70 had 3955FEBF as its first callback (+792): "call to 3955FEBF".
// A8EC6D70 + 4 MB - 720 bytes = A92C6AA0: the node is near the start of a 4 MB buffer and the pool's manager
// just past that buffer's end, as if frames that ran past 4 MB damaged both. Not proven (two runs; the boot's
// allocations are the same in every run).
//
// This file:
//  - cod3_list_memory_extra_mb (default 8 since r2): each list buffer is allocated that much bigger, so a frame that runs
//    past the game's 4 MB writes into room of its own, not into the other frame or the heap. The game still
//    counts with its 4 MB (only its statistics read that). If the bigger block isn't given, the game's size.
//  - at every frame's end the memory it used is measured: a new high above half a buffer is logged, and a frame
//    past the game's size is logged as a warning (in the added room: nothing damaged) or as an error (past the
//    room too: damaged). The first frame logs where the buffers and the effects pool's manager are, and the
//    manager's vtable is checked every frame (its damage is logged in the frame it is seen).
//  - cod3_render_node_guard (default on): before the back end draws a node, its four callbacks must be 0 or game
//    code and its child and next links 0 or inside a list buffer; one that isn't is set to 0 for this frame
//    (that callback isn't called, those nodes aren't drawn) and logged. The nodes are made again every frame.
//  - cod3_fx_pool_guard (default on): FX_PlayFX's "make room" (0x8241B2B0) with a damaged pool manager returns 0,
//    so that effect isn't played instead of calling through the damaged vtable. The pool's next allocation or
//    free still goes through it (0x82418AE0, 0x82418B88): this gets the log written, it isn't a fix.
//
// [mp] 2026-11-02 UNTESTED: cod3mp (codmp_xenonf.xex) too. Split screen draws 2 to 4 views into one frame's lists,
// so multiplayer is where a frame most likely needs more than 4 MB. There: the room (cod3_list_memory_extra_mb),
// the 10 s summary, the high and past-the-end lines and the node guard. Not there: the physical pages, the effects
// pool's checks and the level-load memory line (their multiplayer addresses not looked for). cod3mp's own:
//  - the allocator 0x8213F780 (campaign 0x82127A20; mp_aliases.h), called for the list buffers by the list init
//    0x8216C1F0 (campaign 0x821542F8; returns at +0xC4, +0xD8, +0x204, +0x218) and the size setter 0x8216BFE0
//    (campaign 0x821540E8; +0x1DC, +0x1F0), the same offsets in both;
//  - the buffers' addresses at 0x82A8FC10 and 0x82A8FC14 (stored by 0x8216C1F0 as -1008 and -1004 off 0x82A90000);
//    the open frame's buffer, cursor and size at 0x82A8C140, 0x82A8C144 (addresses_mp.h's kListCursor), 0x82A8C148;
//  - the buffer switch 0x82143838 (campaign 0x82130808, mp_aliases.h; render_resolution.cpp's hook calls
//    BeforeBeginNextFrame in both programs now);
//  - the back end 0x82167688 (campaign 0x8214F770), hooked in viewmodel_flip.cpp, which now calls CheckNode; it
//    reads the node it draws from 0x82A8C134 (campaign 0x8290E9AC), the same node layout.

#include "game/list_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#if !defined(COD3_MP)
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#endif

#include "game/guest.h"
#include "game/program.h"  // [mp] COD3_ADDR

// [r2] 2026-11-02 the default room is 8 MB (owner: 12 MB a buffer); ob20's uncapped trench peaked at 4736 KB.
REXCVAR_DEFINE_INT32(cod3_list_memory_extra_mb, 8, "cod3/Debug",
                     "Each of the two render list buffers (the game's 4 MB, one frame each) is allocated this "
                     "many MB bigger, so a frame that needs more doesn't write into the other frame or the heap "
                     "(0: the game's own size). Applies after a restart")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

#if !defined(COD3_MP)
// [r2] 2026-11-02 UNTESTED: the list buffers as physical pages of their own (64 KB aligned, PAGE_READWRITE, no
// write-combine), through the game's own physical allocation 0x82344EE8 (the path 0x82127A20 takes for flags
// 0x30000), so they no longer come out of the heap the levels allocate from (or the render module's allocator
// override, when one is set). They stay inside the console's 512 MB, which the GPU sees. If the pages aren't given,
// the game's own path is used. Freed by XPhysicalFree (0x82344F78) whether or not the override is set.
REXCVAR_DEFINE_BOOL(cod3_list_memory_physical, false, "cod3/Debug",
                    "Allocate the two render list buffers as physical pages of their own instead of from the "
                    "game's heap (the same guest memory the GPU sees). Applies after a restart")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#endif

// [r1] 2026-11-02 UNTESTED: one line every 10 s (the most each buffer's frames used, frames over 3/4 of the game's
// size and over it); the layout printed again after a level load or when the effects pool's manager appears.
// [claude-9b] 2026-11-02 sync: the 10 s line is off by default (the new-high, past-the-game's-size and
// past-the-room lines and the guards are unchanged).
REXCVAR_DEFINE_BOOL(cod3_debug_list_memory, false, "cod3/Debug",
                    "Log every 10 s the most render list memory a frame used in each buffer and how many frames "
                    "came near or past the game's 4 MB")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_render_node_guard, true, "cod3/Debug",
                    "Before a render scene node is drawn, its callbacks and links are checked; a broken one is "
                    "skipped for that frame (logged) instead of crashing")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#if !defined(COD3_MP)
REXCVAR_DEFINE_BOOL(cod3_fx_pool_guard, true, "cod3/Debug",
                    "An effect isn't played when the effects pool's manager is damaged (logged), instead of "
                    "crashing in that call")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif

namespace {

using cod3::guest::ReadU32;
using cod3::guest::WriteU32;

constexpr uint32_t kListStart = COD3_ADDR(0x8290E9B8, 0x82A8C140);   // the open frame's buffer
constexpr uint32_t kListCursor = COD3_ADDR(0x8290E9BC, 0x82A8C144);  // its next free byte (addr::live_video::kListCursor)
constexpr uint32_t kListSize = COD3_ADDR(0x8290E9C0, 0x82A8C148);    // the game's size of a buffer (4 MB)
constexpr uint32_t kBuffers[2] = {COD3_ADDR(0x82912D00, 0x82A8FC10), COD3_ADDR(0x82912D04, 0x82A8FC14)};
constexpr uint32_t kCurrentNode = COD3_ADDR(0x8290E9AC, 0x82A8C134);  // the node the back end draws (render_list_guard.cpp)
#if !defined(COD3_MP)
constexpr uint32_t kFxPool = 0x82A2ACCC;  // the effects pool's manager (0x82418C80; 0x820CC540 makes it)
constexpr uint32_t kFxPoolVtable = 0x82001378;
#endif

// 0x82127A20's calls that allocate a list buffer (their return addresses): in 0x821540E8 (the size Sys_Main
// sets) and in 0x821542F8 (nglInit's), two buffers each. [mp] cod3mp's 0x8216BFE0 and 0x8216C1F0, the same offsets.
constexpr uint32_t kListAllocSites[] = {
    COD3_ADDR(0x821542C4, 0x8216C1BC), COD3_ADDR(0x821542D8, 0x8216C1D0), COD3_ADDR(0x821543BC, 0x8216C2B4),
    COD3_ADDR(0x821543D0, 0x8216C2C8), COD3_ADDR(0x821544FC, 0x8216C3F4), COD3_ADDR(0x82154510, 0x8216C408)};

// Scene node (1072 bytes, 0x8212DB18): +756 next, +760 first child; callbacks (function, argument) at +768,
// +776, +784, +792 (0x8214F770 calls each that isn't 0; cod3mp's 0x82167688 the same).
constexpr uint32_t kNodeNext = 756;
constexpr uint32_t kNodeChild = 760;
constexpr uint32_t kNodeCallbacks[] = {768, 776, 784, 792};

struct Buffer {
  std::atomic<uint32_t> start{0}, game{0}, room{0};
};
Buffer g_buffers[4];  // the latest list buffer allocations
std::atomic<uint32_t> g_next_slot{0};

std::atomic<uint64_t> g_frames{0}, g_over{0}, g_damaged{0}, g_node_fixes{0};
std::atomic<uint32_t> g_last_used{0}, g_peak{0};
uint32_t g_logged_peak = 0;  // main thread
#if !defined(COD3_MP)
std::atomic<uint64_t> g_fx_skips{0};
std::atomic<bool> g_pool_logged{false};
#endif

// Both programs' code lies below 0x82800000 (cod3mp's 0x820B0000..0x825A21D4); 0x88000000..0x8C000000 is the
// campaign's second module.
bool IsGameCode(uint32_t a) {
  return (a & 3) == 0 && ((a >= 0x82000000u && a < 0x82800000u) || (a >= 0x88000000u && a < 0x8C000000u));
}

bool IsListAllocSite(uint32_t lr) {
  for (uint32_t a : kListAllocSites)
    if (a == lr) return true;
  return false;
}

void Record(uint32_t start, uint32_t game, uint32_t room) {
  // [r1] Boot allocates the pair three times at the same addresses: one record per address, the latest sizes.
  for (Buffer& b : g_buffers) {
    if (b.start.load(std::memory_order_acquire) != start) continue;
    const bool same = b.game.load(std::memory_order_relaxed) == game && b.room.load(std::memory_order_relaxed) == room;
    b.game.store(game, std::memory_order_relaxed);
    b.room.store(room, std::memory_order_relaxed);
    if (!same)
      REXLOG_INFO("cod3: list memory: buffer {:08X}..{:08X} ({} KB; the game's {} KB)", start, start + room,
                  room / 1024, game / 1024);
    return;
  }
  Buffer& b = g_buffers[g_next_slot.fetch_add(1, std::memory_order_relaxed) % 4];
  b.start.store(0, std::memory_order_relaxed);
  b.game.store(game, std::memory_order_relaxed);
  b.room.store(room, std::memory_order_relaxed);
  b.start.store(start, std::memory_order_release);
  REXLOG_INFO("cod3: list memory: buffer {:08X}..{:08X} ({} KB; the game's {} KB)", start, start + room,
              room / 1024, game / 1024);
}

// The game's size and the size given for the buffer at `start`.
bool Sizes(uint8_t* base, uint32_t start, uint32_t* game, uint32_t* room) {
  for (Buffer& b : g_buffers) {
    if (b.start.load(std::memory_order_acquire) != start) continue;
    *game = b.game.load(std::memory_order_relaxed);
    *room = b.room.load(std::memory_order_relaxed);
    return *game != 0;
  }
  *game = *room = ReadU32(base, kListSize);
  return *game != 0;
}

// Inside one of the two list buffers (true when neither is known).
bool InListBuffer(uint8_t* base, uint32_t addr) {
  bool known = false;
  for (uint32_t slot : kBuffers) {
    const uint32_t start = ReadU32(base, slot);
    uint32_t game = 0, room = 0;
    if (!start || !Sizes(base, start, &game, &room)) continue;
    known = true;
    if (addr >= start && addr - start < room) return true;
  }
  return !known;
}

void LogLayout(uint8_t* base, const char* why) {
#if defined(COD3_MP)
  // [mp] cod3mp: the buffers only (the effects pool's manager not looked for there).
  for (int i = 0; i < 2; ++i) {
    const uint32_t start = ReadU32(base, kBuffers[i]);
    uint32_t game = 0, room = 0;
    if (!start || !Sizes(base, start, &game, &room)) continue;
    REXLOG_INFO("cod3: list memory ({}): buffer {} {:08X}..{:08X} (the game's end {:08X})", why, i + 1, start,
                start + room, start + game);
  }
#else
  const uint32_t pool = ReadU32(base, kFxPool);
  for (int i = 0; i < 2; ++i) {
    const uint32_t start = ReadU32(base, kBuffers[i]);
    uint32_t game = 0, room = 0;
    if (!start || !Sizes(base, start, &game, &room)) continue;
    // [r1] The manager's distance from the game's end and from the end with the room (0: not made yet).
    if (pool)
      REXLOG_INFO("cod3: list memory ({}): buffer {} {:08X}..{:08X} (the game's end {:08X}); the effects pool's "
                  "manager {:08X} is {} bytes past the game's end, {} past the room's end",
                  why, i + 1, start, start + room, start + game, pool,
                  int64_t(pool) - int64_t(start) - int64_t(game), int64_t(pool) - int64_t(start) - int64_t(room));
    else
      REXLOG_INFO("cod3: list memory ({}): buffer {} {:08X}..{:08X} (the game's end {:08X}); the effects pool's "
                  "manager not made yet",
                  why, i + 1, start, start + room, start + game);
  }
#endif
}

#if !defined(COD3_MP)
// [r2] Guest physical memory not in use, in MB (as render_resolution.cpp's FreeGuestMb: the physical base heap
// holds every physical allocation, whatever its page size).
uint32_t FreeGuestMb() {
  if (auto* ks = rex::system::kernel_state(); ks && ks->memory()) {
    if (auto* heap = ks->memory()->GetPhysicalHeap())
      return uint32_t(uint64_t(heap->GetUnreservedPageCount()) * heap->page_size() >> 20);
  }
  return 0;
}

// [r2] The list buffers allocated as physical pages here (freed by 0x821276D0's hook).
constexpr uint32_t kPhysicalAlign = 0x10000;
std::atomic<uint32_t> g_physical[4] = {};

void NotePhysical(uint32_t address) {
  for (auto& slot : g_physical) {
    uint32_t empty = 0;
    if (slot.compare_exchange_strong(empty, address)) return;
  }
}

// True (and forgotten) when `address` is one of them.
bool TakePhysical(uint32_t address) {
  if (!address) return false;
  for (auto& slot : g_physical) {
    uint32_t expected = address;
    if (slot.compare_exchange_strong(expected, 0)) return true;
  }
  return false;
}

// [r2] The render module's allocator override (0x82127A20 / 0x821276D0 call these when set).
constexpr uint32_t kAllocOverride = 0x8290E6C4, kFreeOverride = 0x8290E6CC;
uint32_t g_lowest_free_mb = 0;
bool g_override_logged = false;

void LogLevelMemory(uint8_t* base) {
  const uint32_t free_mb = FreeGuestMb();
  if (free_mb && (!g_lowest_free_mb || free_mb < g_lowest_free_mb)) g_lowest_free_mb = free_mb;
  REXLOG_INFO("cod3: list memory (level loaded): {} MB of guest physical memory free (the lowest at a level load "
              "so far {} MB)",
              free_mb, g_lowest_free_mb);
  if (g_override_logged) return;
  g_override_logged = true;
  REXLOG_INFO("cod3: list memory: the render module's allocator override: allocate {:08X}, free {:08X} (0: none)",
              ReadU32(base, kAllocOverride), ReadU32(base, kFreeOverride));
}
#endif

// [r1] The 10 s summary and when the layout is printed again (main thread only).
struct Window {
  uint32_t peak[2] = {0, 0};
  uint64_t frames = 0, over_three_quarters = 0, over_game = 0;
};
Window g_window;
std::chrono::steady_clock::time_point g_window_since = std::chrono::steady_clock::now();
uint32_t g_layout_buffers[2] = {0, 0};
#if !defined(COD3_MP)
uint32_t g_layout_pool = 0;
int32_t g_layout_level_time = 0;
constexpr uint32_t kLevelTime = 0x82A4E790 + 156;  // level.time (ms): goes back at a level load or restart
#endif

void MaybeRelogLayout(uint8_t* base) {
  const uint32_t b0 = ReadU32(base, kBuffers[0]), b1 = ReadU32(base, kBuffers[1]);
#if defined(COD3_MP)
  // [mp] cod3mp: when the buffers change (the first frame).
  if (b0 == g_layout_buffers[0] && b1 == g_layout_buffers[1]) return;
  g_layout_buffers[0] = b0;
  g_layout_buffers[1] = b1;
  LogLayout(base, "buffers");
#else
  const uint32_t pool = ReadU32(base, kFxPool);
  const int32_t level_time = int32_t(ReadU32(base, kLevelTime));
  const char* why = nullptr;
  const bool level_loaded =
      level_time < g_layout_level_time || (g_layout_level_time == 0 && level_time > 0);  // [r2] the first one too
  if (b0 != g_layout_buffers[0] || b1 != g_layout_buffers[1]) why = "buffers";
  else if (pool != g_layout_pool) why = "effects pool";
  else if (level_loaded) why = "level loaded";
  g_layout_level_time = level_time;
  if (!why) return;
  g_layout_pool = pool;
  g_layout_buffers[0] = b0;
  g_layout_buffers[1] = b1;
  LogLayout(base, why);
  if (level_loaded) LogLevelMemory(base);  // [r2]
#endif
}

void CountWindow(uint8_t* base, uint32_t start, uint32_t used, uint32_t game) {
  Window& w = g_window;
  const int i = start == ReadU32(base, kBuffers[0]) ? 0 : start == ReadU32(base, kBuffers[1]) ? 1 : -1;
  if (i >= 0) w.peak[i] = std::max(w.peak[i], used);
  ++w.frames;
  if (used > game / 4 * 3) ++w.over_three_quarters;
  if (used > game) ++w.over_game;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_window_since < std::chrono::seconds(10)) return;
  REXLOG_INFO("cod3: list memory, last 10 s: {} frames; the most a frame used: buffer 1 {} KB, buffer 2 {} KB (the "
              "game's {} KB); {} frames over 3/4 of it, {} over it",
              w.frames, w.peak[0] / 1024, w.peak[1] / 1024, game / 1024, w.over_three_quarters, w.over_game);
  g_window = Window{};
  g_window_since = now;
}

#if !defined(COD3_MP)
bool PoolDamaged(uint8_t* base, uint32_t* pool) {
  *pool = ReadU32(base, kFxPool);
  return *pool != 0 && ReadU32(base, *pool) != kFxPoolVtable;
}

void LogPool(uint8_t* base, uint32_t pool, const char* where) {
  if (g_pool_logged.exchange(true)) return;
  REXLOG_ERROR("cod3: the effects pool's manager {:08X} is damaged ({}): {:08X} {:08X} {:08X} {:08X} (vtable "
               "{:08X} expected); list memory: the last frame used {} KB, the most {} KB, {} frames past the "
               "game's size ({} past the room)",
               pool, where, ReadU32(base, pool), ReadU32(base, pool + 4), ReadU32(base, pool + 8),
               ReadU32(base, pool + 12), kFxPoolVtable, g_last_used.load() / 1024, g_peak.load() / 1024,
               g_over.load(), g_damaged.load());
}
#endif

void LogNodeFix(uint8_t* base, uint32_t node, uint32_t offset, uint32_t value) {
  const uint64_t n = g_node_fixes.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 20 && n % 1000 != 0) return;
  REXLOG_WARN("cod3: render node guard: node {:08X} +{} was {:08X} (not {}), set to 0 for this frame; list "
              "memory: the last frame used {} KB, the most {} KB, {} frames past the game's size; {} fixed so far",
              node, offset, value, offset == kNodeNext || offset == kNodeChild ? "in a list buffer" : "game code",
              g_last_used.load() / 1024, g_peak.load() / 1024, g_over.load(), n);
}

}  // namespace

namespace cod3::list_memory {

void BeforeBeginNextFrame(uint8_t* base) {
  const uint32_t start = ReadU32(base, kListStart), cursor = ReadU32(base, kListCursor);
  if (!start || cursor < start) return;
  g_frames.fetch_add(1, std::memory_order_relaxed);
  MaybeRelogLayout(base);  // [r1] at the first frame, and again when the buffers, the pool or the level change
  uint32_t game = 0, room = 0;
  if (!Sizes(base, start, &game, &room)) return;
  const uint32_t used = cursor - start;
  g_last_used.store(used, std::memory_order_relaxed);
  if (REXCVAR_GET(cod3_debug_list_memory)) CountWindow(base, start, used, game);  // [r1]
  if (used > g_peak.load(std::memory_order_relaxed)) {
    g_peak.store(used, std::memory_order_relaxed);
    if (used >= game / 2 && used >= g_logged_peak + game / 16) {
      g_logged_peak = used;
      REXLOG_INFO("cod3: list memory: a frame used {} KB of the game's {} KB (a new high)", used / 1024,
                  game / 1024);
    }
  }
  if (used > game) {
    const uint64_t n = g_over.fetch_add(1, std::memory_order_relaxed) + 1;
    if (used > room) {
      const uint64_t d = g_damaged.fetch_add(1, std::memory_order_relaxed) + 1;
      if (d <= 20 || d % 1000 == 0)
#if defined(COD3_MP)
        REXLOG_ERROR("cod3: list memory: a frame used {} bytes, {} past the end of its buffer {:08X}..{:08X}: "
                     "{:08X}..{:08X} was written over; {} frames past the game's size so far",
                     used, used - room, start, start + room, start + room, cursor, n);
#else
        REXLOG_ERROR("cod3: list memory: a frame used {} bytes, {} past the end of its buffer {:08X}..{:08X}: "
                     "{:08X}..{:08X} was written over (the effects pool's manager is at {:08X}); {} frames past "
                     "the game's size so far",
                     used, used - room, start, start + room, start + room, cursor, ReadU32(base, kFxPool), n);
#endif
    } else if (n <= 20 || n % 1000 == 0) {
      REXLOG_WARN("cod3: list memory: a frame used {} bytes, {} past the game's {} (in the room of "
                  "cod3_list_memory_extra_mb: nothing damaged); {} such frames so far",
                  used, used - game, game, n);
    }
  }
#if !defined(COD3_MP)
  uint32_t pool = 0;
  if (PoolDamaged(base, &pool)) LogPool(base, pool, "seen at a frame's end");
#endif
}

void CheckNode(uint8_t* base) {
  if (!REXCVAR_GET(cod3_render_node_guard)) return;
  const uint32_t node = ReadU32(base, kCurrentNode);
  if (!node || (node & 3) != 0) return;
  for (uint32_t offset : kNodeCallbacks) {
    const uint32_t fn = ReadU32(base, node + offset);
    if (fn == 0 || IsGameCode(fn)) continue;
    WriteU32(base, node + offset, 0);
    LogNodeFix(base, node, offset, fn);
  }
  for (uint32_t offset : {kNodeNext, kNodeChild}) {
    const uint32_t link = ReadU32(base, node + offset);
    if (link == 0 || ((link & 3) == 0 && InListBuffer(base, link))) continue;
    WriteU32(base, node + offset, 0);
    LogNodeFix(base, node, offset, link);
  }
}

}  // namespace cod3::list_memory

// 0x82127A20(size, alignment, flags): the render module's allocator. For the list buffers, the size asked for
// plus cod3_list_memory_extra_mb. [mp] cod3mp's 0x8213F780 (mp_aliases.h).
#if !defined(COD3_MP)
REX_EXTERN(sub_82344EE8);  // [r2] physical pages (size, highest address, alignment, protection)
REX_EXTERN(sub_82344F78);  // [r2] XPhysicalFree(address)
#endif
REX_EXTERN(__imp__sub_82127A20);
REX_HOOK_RAW(sub_82127A20) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (!IsListAllocSite(lr)) {
    __imp__sub_82127A20(ctx, base);
    return;
  }
  const uint32_t game = ctx.r3.u32;
  const uint32_t extra = uint32_t(std::clamp(int(REXCVAR_GET(cod3_list_memory_extra_mb)), 0, 16)) << 20;
#if !defined(COD3_MP)
  // [r2] Physical pages of their own: 0x82344EE8(size, highest address, alignment, protection), as 0x82127A20
  // calls it for flags 0x30000 (protection 4 = PAGE_READWRITE; 0x404 would add write-combine).
  if (REXCVAR_GET(cod3_list_memory_physical) && game != 0) {
    const uint64_t r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64;
    const uint32_t size = game + extra;
    ctx.r3.u64 = size;
    ctx.r4.s64 = -1;
    ctx.r5.u64 = kPhysicalAlign;
    ctx.r6.u64 = 4;
    sub_82344EE8(ctx, base);
    const uint32_t pages = ctx.r3.u32;
    ctx.lr = lr;
    if (pages != 0) {
      NotePhysical(pages);
      Record(pages, game, size);
      REXLOG_INFO("cod3: list memory: buffer {:08X} as physical pages ({} KB), {} MB of guest physical memory free",
                  pages, size / 1024, FreeGuestMb());
      return;
    }
    REXLOG_WARN("cod3: list memory: {} KB of physical pages for a list buffer not given; the game's own allocation",
                size / 1024);
    ctx.r3.u64 = game;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
  }
#endif
  if (extra != 0 && game != 0) {
    const uint64_t r4 = ctx.r4.u64, r5 = ctx.r5.u64;
    ctx.r3.u64 = uint64_t(game) + extra;
    __imp__sub_82127A20(ctx, base);
    if (ctx.r3.u32 != 0) {
      Record(ctx.r3.u32, game, game + extra);
      return;
    }
    REXLOG_WARN("cod3: list memory: {} KB more for a list buffer not given; the game's {} KB", extra / 1024,
                game / 1024);
    ctx.r3.u64 = game;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.lr = lr;
  }
  __imp__sub_82127A20(ctx, base);
  if (ctx.r3.u32 != 0) Record(ctx.r3.u32, game, game);
}

#if !defined(COD3_MP)
// [r2] 0x821276D0(address): the render module's free (the override's free when one is set, else XPhysicalFree for
// an address with bit 31, else free). A list buffer allocated above as physical pages always goes to XPhysicalFree.
REX_EXTERN(__imp__sub_821276D0);
REX_HOOK_RAW(sub_821276D0) {
  if (TakePhysical(ctx.r3.u32)) {
    sub_82344F78(ctx, base);
    return;
  }
  __imp__sub_821276D0(ctx, base);
}

// 0x8241B2B0(effect, mode): FX_PlayFX's "make room" (string "killing %08X %s to make way for %s"); its first
// act calls the pool manager's slot +16. Nonzero: play the effect.
REX_EXTERN(__imp__sub_8241B2B0);
REX_HOOK_RAW(sub_8241B2B0) {
  uint32_t pool = 0;
  if (REXCVAR_GET(cod3_fx_pool_guard) && PoolDamaged(base, &pool)) {
    LogPool(base, pool, "seen by FX_PlayFX");
    const uint64_t n = g_fx_skips.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % 1000 == 0) REXLOG_WARN("cod3: effects pool guard: {} effects not played so far", n);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8241B2B0(ctx, base);
}
#endif
