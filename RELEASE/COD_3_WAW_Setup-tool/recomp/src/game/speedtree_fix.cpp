// cod3 - [speedtree] the trees' buffers are released once, and only the ones
// that were made.
//
// SpeedTreeWrapper (one per tree model in a level) makes its branch geometry
// in sub_823EFB80 (cod3mp sub_824097A0) as the level loads: a vertex buffer
// (+456) and one index buffer per level of detail (LOD), kept in an array
// (+460) it allocates with the game's Mem_Alloc, which doesn't clear it. A LOD
// without branch strips (its count, in the array at *(+72), is 0) gets no
// index buffer, and its slot keeps whatever that memory held before.
//
// Its release, sub_823F04B0 (cod3mp sub_8240A0C0), calls D3DResource_Release
// (sub_822F1340, cod3mp sub_822C0060) on the vertex buffer and on every slot of
// that array (it checks the other arrays' slots for null, not this one), on
// the frond and leaf buffers, then frees the arrays and +24. It changes none of
// the fields it reads, and it runs twice for every tree when a level ends:
// from the tree manager's shutdown (sub_823F0828) and again from the
// wrapper's destructor (SpeedTreeWrapper__vfunc_0, and sub_823F30F8). The
// second time it releases buffers already destroyed and reads arrays already
// freed, and frees them again.
//
// (The game's small-block pools are in its .bss, from 0x826A2C00: a freed
// block's first word becomes the pool's free-list link, so the second run
// finds a pointer to another free block in the first slot of the array it
// freed.) Both end in D3DResource_Release on a pointer that is no buffer: it
// decrements a word wherever that points, and when the word reaches 0 it
// "destroys" it, freeing memory that isn't a block of the heap. In this port
// that broke the game at level changes: 2026-10-20, `spmap forest` and then
// `spmap falaise` freed 0x82878A00 (in the game's .bss) from the destructor's
// release and crashed in the C heap's free (RtlpCoalesceFreeBlocks,
// sub_823460D8), two runs out of two (found with cod3_heap_check,
// game/heap_check.cpp); earlier long runs read 0x234 at the third load or hung
// inside the heap.
//
// The fix, at the release (both programs; the same code and fields in cod3mp):
//   - before it runs, the slots of LODs without strips (never written) are set
//     to 0, and D3DResource_Release(0) returns 0 without touching memory (the
//     game's code would read address 4);
//   - after it has run, the counts it checks (+84 branch vertices, +172 frond
//     vertices, +244 leaf groups) and the pointers it released or freed (+24,
//     +456 ... +480) are set to 0, so its second run releases and frees
//     nothing (it still calls Mem_Free(0) for the leaf array, and the game's
//     free, sub_820CBF58, returns at once for 0).
// Every buffer and array the game made is still released and freed, once.

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "game/guest.h"

REX_EXTERN(__imp__sub_823F04B0);  // SpeedTreeWrapper's geometry release (cod3mp sub_8240A0C0)
REX_EXTERN(__imp__sub_822F1340);  // D3DResource_Release (cod3mp sub_822C0060)

namespace {

using cod3::guest::ReadU32;
using cod3::guest::WriteU32;

// SpeedTreeWrapper fields (the same in both programs).
constexpr uint32_t kRtObject = 24;          // freed at the end of the release when not 0
constexpr uint32_t kBranchLods = 68;        // s32: LODs (the geometry's first field)
constexpr uint32_t kBranchStrips = 72;      // s32*: strips per LOD
constexpr uint32_t kBranchVertices = 84;    // s32: the release skips the branches when <= 0
constexpr uint32_t kFrondVertices = 172;    // s32: ... the fronds
constexpr uint32_t kLeafGroups = 244;       // s32: leaf groups (its loop count)
constexpr uint32_t kBranchVb = 456;         // IDirect3DVertexBuffer9*
constexpr uint32_t kBranchIbs = 460;        // IDirect3DIndexBuffer9*[LODs]
constexpr uint32_t kBranchIbLengths = 464;  // u16[LODs]
constexpr uint32_t kFrondVb = 468;
constexpr uint32_t kFrondIbs = 472;
constexpr uint32_t kFrondIbLengths = 476;
constexpr uint32_t kLeafVbs = 480;
constexpr int32_t kMaxLods = 64;  // more than any tree has; larger counts are left alone

// Plain guest memory this file may read and write: not the MMIO range, and
// below the high view (which the generated code reaches another way).
bool Plain(uint32_t addr, uint32_t len) {
  const uint64_t end = uint64_t(addr) + len;
  if (addr < 0x10000 || (addr & 3) != 0 || end > 0xE0000000ull) return false;
  return !(addr < 0x80000000u && end > 0x7F000000ull);  // MMIO
}

std::atomic<uint64_t> g_trees{0};    // first releases
std::atomic<uint64_t> g_cleared{0};  // LOD slots never written, set to 0
std::atomic<uint64_t> g_repeats{0};  // later releases that found nothing left
std::atomic<uint64_t> g_null_releases{0};
std::atomic<int> g_logged{0};
std::atomic<int> g_logged_repeats{0};
std::atomic<int64_t> g_next_summary_ns{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The slots of LODs without strips: never written by the setup.
void ClearUnmadeSlots(uint8_t* base, uint32_t self) {
  if (static_cast<int32_t>(ReadU32(base, self + kBranchVertices)) <= 0) return;  // not released
  const int32_t lods = static_cast<int32_t>(ReadU32(base, self + kBranchLods));
  const uint32_t ibs = ReadU32(base, self + kBranchIbs);
  const uint32_t strips = ReadU32(base, self + kBranchStrips);
  if (lods <= 0 || lods > kMaxLods) return;
  if (!Plain(ibs, uint32_t(lods) * 4) || !Plain(strips, uint32_t(lods) * 4)) return;
  for (int32_t i = 0; i < lods; ++i) {
    if (static_cast<int32_t>(ReadU32(base, strips + 4 * i)) > 0) continue;  // made (or 0 if failed)
    const uint32_t stale = ReadU32(base, ibs + 4 * i);
    if (!stale) continue;
    WriteU32(base, ibs + 4 * i, 0);
    g_cleared.fetch_add(1, std::memory_order_relaxed);
    if (g_logged.fetch_add(1, std::memory_order_relaxed) < 8)
      REXLOG_INFO("cod3: SpeedTree: tree {:08X}, LOD {} has no branch strips; its index buffer slot "
                  "held {:08X}, never set (now 0, so nothing is released for it)",
                  self, i, stale);
  }
}

// After the release: nothing left for another run to release or free.
void Disarm(uint8_t* base, uint32_t self) {
  for (uint32_t off : {kBranchVertices, kFrondVertices, kLeafGroups, kRtObject, kBranchVb, kBranchIbs,
                       kBranchIbLengths, kFrondVb, kFrondIbs, kFrondIbLengths, kLeafVbs})
    WriteU32(base, self + off, 0);
}

bool Armed(uint8_t* base, uint32_t self) {
  for (uint32_t off : {kBranchVertices, kFrondVertices, kLeafGroups, kRtObject, kLeafVbs})
    if (ReadU32(base, self + off)) return true;
  return false;
}

void MaybeSummary() {
  const int64_t now = NowNs();
  int64_t next = g_next_summary_ns.load(std::memory_order_relaxed);
  if (now < next) return;
  if (!g_next_summary_ns.compare_exchange_strong(next, now + 5'000'000'000ll)) return;
  REXLOG_INFO("cod3: SpeedTree: {} trees released so far, {} unmade LOD slots cleared, {} repeated "
              "releases left with nothing to do, {} releases of no buffer skipped",
              g_trees.load(), g_cleared.load(), g_repeats.load(), g_null_releases.load());
}

}  // namespace

// SpeedTreeWrapper's geometry release: sub_823F04B0(this) (cod3mp sub_8240A0C0).
REX_HOOK_RAW(sub_823F04B0) {
  const uint32_t self = ctx.r3.u32;
  if (!Plain(self, 976)) {  // not a wrapper we can read: the game's code as it is
    __imp__sub_823F04B0(ctx, base);
    return;
  }
  const bool armed = Armed(base, self);
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  if (armed) ClearUnmadeSlots(base, self);
  __imp__sub_823F04B0(ctx, base);
  Disarm(base, self);
  if (armed) {
    g_trees.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_repeats.fetch_add(1, std::memory_order_relaxed);
    if (g_logged_repeats.fetch_add(1, std::memory_order_relaxed) < 4)
      REXLOG_INFO("cod3: SpeedTree: tree {:08X} released again (from {:08X}); it had nothing left "
                  "to release or free",
                  self, caller);
  }
  MaybeSummary();
}

// D3DResource_Release(resource) -> remaining references (cod3mp sub_822C0060):
// Release(0) does nothing (the game's code would read address 4).
REX_HOOK_RAW(sub_822F1340) {
  if (ctx.r3.u32 == 0) {
    if (g_null_releases.fetch_add(1, std::memory_order_relaxed) < 4)
      REXLOG_INFO("cod3: D3DResource_Release(0) from {:08X} skipped (no buffer)",
                  static_cast<uint32_t>(ctx.lr));
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_822F1340(ctx, base);
}
