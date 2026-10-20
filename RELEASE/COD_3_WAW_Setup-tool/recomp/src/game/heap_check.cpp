// cod3 - [heap-check] a check of the game's C heap, to find what damages it.
//
// The campaign's C runtime heap is the Xbox 360 system library's version of
// the NT heap: RtlAllocateHeap (sub_823478F0), RtlFreeHeap (sub_823481D8) and
// RtlReAllocateHeap (sub_823484C0), which malloc, free, operator new and
// delete reach (the process heap's handle is kept at 0x82BEA9C0). A block
// starts with a 16-byte header:
//
//   +0 u16 its size in 16-byte units (header included)
//   +2 u16 the size of the block before it in memory, the same way
//   +4 u8 segment index, +5 u8 flags: 0x01 in use, 0x02 extra present,
//      0x04 fill pattern, 0x08 a big block of its own (virtual memory),
//      0x10 the last block before the end of its segment's memory
//   +6 u8 the block's size minus the bytes asked for, +7 u8 tag index
//   a free block: +8 Flink, +12 Blink of its free list (the heads are in the
//   heap at +0x180 + 8 * size in units, sizes 0..127)
//
// and the heap's lock (a critical-section pointer) is at heap +0x580.
//
// Level changes sometimes crash inside the heap's free (2026-10-20, run
// claude-setup-chain1: RtlpCoalesceFreeBlocks, sub_823460D8, read the null
// Blink of a "free" neighbor while Forest's SpeedTree data was freed at the
// next level's start): something damaged a block header earlier, and the
// crash comes long after. cod3_heap_check (command line only, off) finds the
// damage nearer to where it happens:
//   - every allocation is recorded with the guest call stack that asked for
//     it, every free in a ring of the last 8192 with both stacks;
//   - at every allocation, free and reallocation, the block and its two
//     neighbors in memory are checked: the sizes they give each other agree,
//     a free neighbor's list links point back at it, a block freed is in use
//     and was allocated;
//   - once a second every recorded block is checked (its header is in use,
//     big enough for what was asked, and agrees with the next one);
// all under the heap's own lock, so no other thread is half-way through a
// change. A problem is logged with the damaged header, the block in use just
// before it in memory (who asked for it and how many bytes: a write past its
// end is the usual cause), and who had that memory when it was freed
// recently (a write after free). The game's heap is only read, never changed.
// Costs a few microseconds per allocation and a few milliseconds a second;
// campaign only (cod3mp's heap functions are elsewhere).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "game/guest.h"
#include "game/watchdog.h"

REXCVAR_DEFINE_BOOL(cod3_heap_check, false, "cod3",
                    "Testing, from the command line only: the game's C heap is checked at every "
                    "allocation and free and once a second, and what damaged it is written to the "
                    "log (slower)");

REX_EXTERN(__imp__sub_823478F0);  // RtlAllocateHeap(heap, flags, size)
REX_EXTERN(__imp__sub_823481D8);  // RtlFreeHeap(heap, flags, ptr)
REX_EXTERN(__imp__sub_823484C0);  // RtlReAllocateHeap(heap, flags, ptr, size)

namespace {

using cod3::guest::ReadU16;
using cod3::guest::ReadU32;
using cod3::guest::ReadU8;

constexpr uint32_t kEnterCriticalSection = 0x8257835C;  // __imp__RtlEnterCriticalSection
constexpr uint32_t kLeaveCriticalSection = 0x8257836C;  // __imp__RtlLeaveCriticalSection

constexpr uint32_t kHeapSignatureOff = 0x10;
constexpr uint32_t kHeapForceFlagsOff = 0x18;
constexpr uint32_t kHeapFreeListsOff = 0x180;
constexpr uint32_t kHeapFreeListsEnd = 0x580;  // 128 list heads of 8 bytes
constexpr uint32_t kHeapLockOff = 0x580;
constexpr uint32_t kNoSerialize = 0x1;
constexpr uint32_t kHeader = 16;
constexpr uint8_t kBusy = 0x01;
constexpr uint8_t kVirtual = 0x08;
constexpr uint8_t kLast = 0x10;

constexpr int kDepth = 8;            // return addresses kept per stack
constexpr size_t kFreedRing = 8192;  // frees remembered
constexpr uint64_t kMaxReports = 12;
constexpr int64_t kSweepEveryNs = 1'000'000'000;

struct Stack {
  uint32_t pc[kDepth] = {};
};
struct Live {
  uint32_t heap = 0;
  uint32_t size = 0;  // bytes asked for
  uint32_t seq = 0;   // allocation number
  Stack stack;
};
struct Freed {
  uint32_t ptr = 0, size = 0, alloc_seq = 0, free_seq = 0;
  Stack alloc, free;
};
struct HeapInfo {
  uint32_t signature = 0;
  bool gone = false;
};

std::mutex g_mu;  // guards everything below; taken after a heap's lock, never before it
std::map<uint32_t, Live> g_live;  // by user address
std::vector<Freed> g_freed;       // ring
size_t g_freed_next = 0;
std::unordered_map<uint32_t, HeapInfo> g_heaps;
uint32_t g_seq = 0;
uint32_t g_free_seq = 0;
uint64_t g_allocs = 0, g_frees = 0, g_reallocs = 0, g_sweeps = 0, g_problems = 0;
std::atomic<int64_t> g_next_sweep_ns{0};
std::atomic_flag g_sweeping;  // starts clear (C++20)
thread_local int t_in_realloc = 0;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 1 on, 0 off; decided once, at the first heap call (before the game's code
// has allocated anything, so every block is seen).
std::atomic<int> g_mode{-1};
bool On() {
  int m = g_mode.load(std::memory_order_relaxed);
  if (m >= 0) return m == 1;
  const bool asked = REXCVAR_GET(cod3_heap_check);
  const bool ok =
      asked && rex::cvar::GetFlagSource("cod3_heap_check") == rex::cvar::Source::kCommandLine;
  if (ok) {
    std::lock_guard lock(g_mu);
    if (g_freed.empty()) g_freed.resize(kFreedRing);
  }
  int expected = -1;
  if (g_mode.compare_exchange_strong(expected, ok ? 1 : 0)) {
    if (ok) {
      REXLOG_WARN("cod3: heap check on: the game's C heap is checked at every allocation and free "
                  "(slower; a test)");
    } else if (asked) {
      REXLOG_WARN("cod3: cod3_heap_check is a command-line test switch; not used");
    }
  }
  return g_mode.load(std::memory_order_relaxed) == 1;
}

// --- Guest call stacks ------------------------------------------------------

Stack Capture(const PPCContext& ctx, uint8_t* base) {
  using rex::system::XThread;
  Stack s;
  s.pc[0] = static_cast<uint32_t>(ctx.lr);
  if (!XThread::IsInThread()) return s;
  XThread* t = XThread::GetCurrentThread();
  const uint32_t pcr = t ? t->pcr_ptr() : 0;
  if (!pcr) return s;
  const uint32_t top = ReadU32(base, pcr + 0x70);
  const uint32_t end = ReadU32(base, pcr + 0x74);
  if (!(end && end < top && top - end <= (64u << 20))) return s;
  // Each frame's back chain at its r1; the return address its function saved
  // 8 bytes below the caller's r1 (mflr r12; stw r12,-8(r1) or __savegprlr).
  uint32_t sp = ctx.r1.u32;
  for (int i = 1; i < kDepth; ++i) {
    if ((sp & 3) || sp < end || sp + 4 > top) break;
    const uint32_t caller = ReadU32(base, sp);
    if (caller <= sp || caller > top || (caller & 3) || caller - 8 < end) break;
    s.pc[i] = ReadU32(base, caller - 8);
    sp = caller;
  }
  return s;
}

std::string Format(const Stack& s) {
  std::string out;
  for (uint32_t pc : s.pc) {
    if (!pc) break;
    if (!out.empty()) out += " <- ";
    out += cod3::watchdog::GuestCodeName(pc);
  }
  return out.empty() ? std::string("?") : out;
}

// --- Reading the heap safely --------------------------------------------------

// Guest pages known readable during one check (the heap's lock is held, and
// the heap only gives memory back under it).
class Pages {
 public:
  bool Readable(uint32_t addr, uint32_t len) {
    const uint64_t last = uint64_t(addr) + len - 1;
    if (len == 0 || last > 0xDFFFFFFFull) return false;
    if (addr < 0x10000) return false;
    if (last >= 0x7F000000ull && addr < 0x80000000u) return false;  // MMIO
    for (uint64_t page = addr & ~0xFFFull; page <= last; page += 0x1000) {
      const uint32_t p = static_cast<uint32_t>(page);
      if (ok_.count(p)) continue;
      auto* mem = rex::system::kernel_state()->memory();
      auto* heap = mem ? mem->LookupHeap(p) : nullptr;
      uint32_t protect = 0;
      if (!heap || !heap->QueryProtect(p, &protect) ||
          !(protect & rex::memory::kMemoryProtectRead))
        return false;
      ok_.insert(p);
    }
    return true;
  }

 private:
  std::unordered_set<uint32_t> ok_;
};

struct Header {
  bool ok = false;
  uint32_t size = 0;  // bytes
  uint32_t prev = 0;  // bytes
  uint8_t flags = 0;
};

struct Problem {
  uint32_t at = 0;  // the header that is wrong
  std::string what;
};

class Checker {
 public:
  Checker(uint8_t* base, uint32_t heap) : base_(base), heap_(heap) {}

  Header Read(uint32_t entry) {
    Header h;
    if (!pages_.Readable(entry, kHeader)) return h;
    h.ok = true;
    h.size = uint32_t(ReadU16(base_, entry)) * 16;
    h.prev = uint32_t(ReadU16(base_, entry + 2)) * 16;
    h.flags = ReadU8(base_, entry + 5);
    return h;
  }

  // A free-list link: a list head in the heap, or a free block's +8.
  bool LinkOk(uint32_t v) {
    if (v >= heap_ + kHeapFreeListsOff && v < heap_ + kHeapFreeListsEnd)
      return ((v - heap_ - kHeapFreeListsOff) & 7) == 0;
    if (((v - 8) & 15) != 0) return false;
    const Header h = Read(v - 8);
    return h.ok && !(h.flags & kBusy) && h.size != 0;
  }

  // A free block's list links point at list entries that point back at it.
  bool FreeLinks(uint32_t f, Problem* p) {
    const uint32_t me = f + 8;
    const uint32_t flink = ReadU32(base_, f + 8), blink = ReadU32(base_, f + 12);
    if (!LinkOk(flink) || !LinkOk(blink)) {
      *p = {f, fmt::format("the free block at {:08X} has free-list links that point at no list "
                           "(Flink {:08X}, Blink {:08X})",
                           f, flink, blink)};
      return false;
    }
    const uint32_t back = ReadU32(base_, flink + 4), fwd = ReadU32(base_, blink);
    if (back != me || fwd != me) {
      *p = {f, fmt::format("the free block at {:08X}: its free-list neighbors don't point back at "
                           "it (Flink {:08X} whose Blink is {:08X}, Blink {:08X} whose Flink is "
                           "{:08X})",
                           f, flink, back, blink, fwd)};
      return false;
    }
    return true;
  }

  // A block and its two neighbors in memory.
  bool Around(uint32_t entry, Problem* p) {
    const Header h = Read(entry);
    if (!h.ok) {
      *p = {entry, fmt::format("the block header at {:08X} can't be read", entry)};
      return false;
    }
    if (h.flags & kVirtual) return true;  // a big block of its own; no neighbors
    if (h.size == 0) {
      *p = {entry, fmt::format("the block at {:08X} has size 0", entry)};
      return false;
    }
    if (!(h.flags & kLast)) {
      const uint32_t n = entry + h.size;
      const Header hn = Read(n);
      if (!hn.ok) {
        *p = {n, fmt::format("the block after {:08X} ({} bytes) has a header at {:08X} that "
                             "can't be read",
                             entry, h.size, n)};
        return false;
      }
      if (hn.prev != h.size || hn.size == 0) {
        *p = {n, fmt::format("the block at {:08X} says the block before it is {} bytes, but that "
                             "block (at {:08X}) is {} bytes",
                             n, hn.prev, entry, h.size)};
        return false;
      }
      if (!(hn.flags & kBusy) && !FreeLinks(n, p)) return false;
    }
    if (h.prev) {
      const uint32_t q = entry - h.prev;
      const Header hq = Read(q);
      if (!hq.ok) {
        *p = {entry, fmt::format("the block at {:08X} says the block before it is {} bytes, but "
                                 "there ({:08X}) is no readable header",
                                 entry, h.prev, q)};
        return false;
      }
      if (hq.size != h.prev) {
        *p = {entry, fmt::format("the block at {:08X} says the block before it is {} bytes, but "
                                 "the block there ({:08X}) says it is {} bytes",
                                 entry, h.prev, q, hq.size)};
        return false;
      }
      if (!(hq.flags & kBusy) && !(hq.flags & kVirtual) && !FreeLinks(q, p)) return false;
    }
    return true;
  }

  std::string Hex(uint32_t addr, uint32_t len) {
    if (!pages_.Readable(addr, len)) return "(not readable)";
    std::string out;
    for (uint32_t i = 0; i < len; ++i) {
      if (i && (i % 4) == 0) out += ' ';
      out += fmt::format("{:02X}", ReadU8(base_, addr + i));
    }
    return out;
  }

 private:
  uint8_t* base_;
  uint32_t heap_;
  Pages pages_;
};

// The heap's own lock, held while a check reads it (unless the heap or the
// call says the heap isn't serialized, as its own functions decide).
class HeapLock {
 public:
  HeapLock(PPCContext& ctx, uint8_t* base, uint32_t heap, uint32_t flags) : ctx_(ctx), base_(base) {
    if ((ReadU32(base, heap + kHeapForceFlagsOff) | flags) & kNoSerialize) return;
    cs_ = ReadU32(base, heap + kHeapLockOff);
    if (!cs_) return;
    cod3::guest::Call c(ctx_, base_);
    c.Invoke(kEnterCriticalSection, {cs_});
  }
  ~HeapLock() {
    if (!cs_) return;
    cod3::guest::Call c(ctx_, base_);
    c.Invoke(kLeaveCriticalSection, {cs_});
  }
  HeapLock(const HeapLock&) = delete;
  HeapLock& operator=(const HeapLock&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t cs_ = 0;
};

// --- Reports (g_mu held) ----------------------------------------------------

void NoteHeap(uint8_t* base, uint32_t heap) {
  auto [it, added] = g_heaps.try_emplace(heap);
  if (!added) return;
  it->second.signature = ReadU32(base, heap + kHeapSignatureOff);
  REXLOG_INFO("cod3: heap check: heap {:08X} (signature {:08X})", heap, it->second.signature);
}

void Report(Checker& ck, const char* during, const Problem& p, const Stack& stack) {
  ++g_problems;
  if (g_problems > kMaxReports) {
    if (g_problems == kMaxReports + 1)
      REXLOG_ERROR("cod3: heap check: more problems are only counted from now on");
    return;
  }
  REXLOG_ERROR("cod3: heap check: {} (found at a {}, called from {})", p.what, during, Format(stack));
  REXLOG_ERROR("cod3: heap check:   the header at {:08X}: {}; the 32 bytes before it: {}", p.at,
               ck.Hex(p.at, 16), ck.Hex(p.at - 32, 32));
  // The block in use whose header is the damaged one, and the one just
  // before it in memory (a write past that one's end is the usual cause).
  if (auto it = g_live.find(p.at + kHeader); it != g_live.end())
    REXLOG_ERROR("cod3: heap check:   that block is in use: {:08X}, {} bytes asked, allocation #{}, "
                 "asked for by {}",
                 it->first, it->second.size, it->second.seq, Format(it->second.stack));
  auto it = g_live.lower_bound(p.at + kHeader);
  if (it != g_live.begin()) {
    --it;
    const int64_t gap = int64_t(p.at) - (int64_t(it->first) + it->second.size);
    REXLOG_ERROR("cod3: heap check:   the block in use before it: {:08X}, {} bytes asked ({} {} "
                 "bytes {} the header), allocation #{}, asked for by {}",
                 it->first, it->second.size, gap >= 0 ? "ending" : "reaching", gap >= 0 ? gap : -gap,
                 gap >= 0 ? "before" : "over", it->second.seq, Format(it->second.stack));
  }
  // Who had that memory, when it was freed recently.
  const Freed* last = nullptr;
  for (const Freed& f : g_freed) {
    if (!f.ptr) continue;
    const uint32_t lo = f.ptr - kHeader, hi = f.ptr + f.size;
    if (p.at + kHeader > lo && p.at < hi && (!last || f.free_seq > last->free_seq)) last = &f;
  }
  if (last)
    REXLOG_ERROR("cod3: heap check:   that memory was freed {} frees ago: {:08X}, {} bytes, "
                 "allocation #{} by {}; freed by {}",
                 g_free_seq - last->free_seq, last->ptr, last->size, last->alloc_seq,
                 Format(last->alloc), Format(last->free));
  rex::FlushLogging();
}

void Remember(uint32_t ptr, const Live& l, const Stack& freed_by) {
  if (g_freed.empty()) return;
  Freed& f = g_freed[g_freed_next];
  g_freed_next = (g_freed_next + 1) % g_freed.size();
  f.ptr = ptr;
  f.size = l.size;
  f.alloc_seq = l.seq;
  f.free_seq = ++g_free_seq;
  f.alloc = l.stack;
  f.free = freed_by;
}

// --- The once-a-second sweep ------------------------------------------------

void Sweep(PPCContext& ctx, uint8_t* base) {
  std::vector<uint32_t> heaps;
  {
    std::lock_guard lock(g_mu);
    for (auto& [h, info] : g_heaps)
      if (!info.gone) heaps.push_back(h);
  }
  const Stack none;
  for (const uint32_t heap : heaps) {
    HeapLock heap_lock(ctx, base, heap, 0);
    std::lock_guard lock(g_mu);
    HeapInfo& info = g_heaps[heap];
    if (ReadU32(base, heap + kHeapSignatureOff) != info.signature) {
      info.gone = true;  // the heap was destroyed; its blocks with it
      for (auto it = g_live.begin(); it != g_live.end();)
        it = it->second.heap == heap ? g_live.erase(it) : std::next(it);
      REXLOG_INFO("cod3: heap check: heap {:08X} is gone; its blocks are forgotten", heap);
      continue;
    }
    Checker ck(base, heap);
    int found = 0;
    for (const auto& [ptr, l] : g_live) {
      if (l.heap != heap) continue;
      const uint32_t entry = ptr - kHeader;
      const Header h = ck.Read(entry);
      Problem p;
      if (!h.ok) {
        p = {entry, fmt::format("the header of the block in use at {:08X} can't be read", ptr)};
      } else if (!(h.flags & kBusy)) {
        p = {entry, fmt::format("the block in use at {:08X} ({} bytes asked) is marked free", ptr,
                                l.size)};
      } else if (!(h.flags & kVirtual) && h.size < l.size + kHeader) {
        p = {entry, fmt::format("the block in use at {:08X} says it is {} bytes, but {} bytes were "
                                "asked for it",
                                ptr, h.size, l.size)};
      } else if (!ck.Around(entry, &p)) {
        // p filled in
      } else {
        continue;
      }
      Report(ck, "check of every block", p, none);
      if (++found >= 3) break;
    }
  }
  std::lock_guard lock(g_mu);
  if (++g_sweeps % 60 == 1) {
    uint64_t bytes = 0;
    for (const auto& [ptr, l] : g_live) bytes += l.size;
    REXLOG_INFO("cod3: heap check: {} blocks in use ({} KB), {} allocations, {} frees, {} "
                "reallocations, {} problems so far",
                g_live.size(), bytes / 1024, g_allocs, g_frees, g_reallocs, g_problems);
  }
}

void MaybeSweep(PPCContext& ctx, uint8_t* base) {
  // Not inside a reallocation: it holds its heap's lock, and the sweep takes
  // every heap's in turn.
  if (t_in_realloc) return;
  const int64_t now = NowNs();
  if (now < g_next_sweep_ns.load(std::memory_order_relaxed)) return;
  if (g_sweeping.test_and_set()) return;
  g_next_sweep_ns.store(now + kSweepEveryNs, std::memory_order_relaxed);
  Sweep(ctx, base);
  g_sweeping.clear();
}

// A new block (or one a reallocation returned): checked, then recorded.
void Added(PPCContext& ctx, uint8_t* base, uint32_t heap, uint32_t flags, uint32_t ptr,
           uint32_t size, const Stack& stack, const char* during) {
  Checker ck(base, heap);
  Problem p;
  HeapLock heap_lock(ctx, base, heap, flags);
  const bool bad = !ck.Around(ptr - kHeader, &p);
  std::lock_guard lock(g_mu);
  NoteHeap(base, heap);
  g_live[ptr] = Live{heap, size, ++g_seq, stack};
  if (bad) Report(ck, during, p, stack);
}

// A block about to be freed (or reallocated): checked, then forgotten.
// Returns its record (when there was one) in *was.
bool Removing(PPCContext& ctx, uint8_t* base, uint32_t heap, uint32_t flags, uint32_t ptr,
              const Stack& stack, const char* during, Live* was) {
  Checker ck(base, heap);
  Problem p;
  HeapLock heap_lock(ctx, base, heap, flags);
  const uint32_t entry = ptr - kHeader;
  const Header h = ck.Read(entry);
  bool bad = false;
  if (!h.ok) {
    bad = true;
    p = {entry, fmt::format("{:08X} can't be freed: its header can't be read", ptr)};
  } else if (!(h.flags & kBusy)) {
    bad = true;
    p = {entry, fmt::format("{:08X} is freed, but its block is already free (freed twice?)", ptr)};
  } else {
    bad = !ck.Around(entry, &p);
  }
  std::lock_guard lock(g_mu);
  NoteHeap(base, heap);
  bool had = false;
  if (auto it = g_live.find(ptr); it != g_live.end()) {
    *was = it->second;
    had = true;
    g_live.erase(it);
  } else if (!bad && !t_in_realloc) {
    bad = true;
    p = {entry, fmt::format("{:08X} is freed, but this check never saw it allocated (not from this "
                            "heap, or freed twice)",
                            ptr)};
  }
  if (bad) Report(ck, during, p, stack);
  return had;
}

}  // namespace

// RtlAllocateHeap(heap r3, flags r4, size r5) -> r3 the block (or 0).
REX_HOOK_RAW(sub_823478F0) {
  if (!On()) {
    __imp__sub_823478F0(ctx, base);
    return;
  }
  const uint32_t heap = ctx.r3.u32, flags = ctx.r4.u32, size = ctx.r5.u32;
  const Stack stack = Capture(ctx, base);
  __imp__sub_823478F0(ctx, base);
  const uint32_t ptr = ctx.r3.u32;
  {
    std::lock_guard lock(g_mu);
    ++g_allocs;
  }
  if (ptr) Added(ctx, base, heap, flags, ptr, size, stack, "allocation");
  MaybeSweep(ctx, base);
}

// RtlFreeHeap(heap r3, flags r4, ptr r5) -> r3 TRUE / FALSE.
REX_HOOK_RAW(sub_823481D8) {
  const uint32_t ptr = ctx.r5.u32;
  if (!On() || ptr == 0) {
    __imp__sub_823481D8(ctx, base);
    return;
  }
  const uint32_t heap = ctx.r3.u32, flags = ctx.r4.u32;
  const Stack stack = Capture(ctx, base);
  Live was;
  if (Removing(ctx, base, heap, flags, ptr, stack, "free", &was)) {
    std::lock_guard lock(g_mu);
    Remember(ptr, was, stack);
  }
  {
    std::lock_guard lock(g_mu);
    ++g_frees;
  }
  __imp__sub_823481D8(ctx, base);
  MaybeSweep(ctx, base);
}

// RtlReAllocateHeap(heap r3, flags r4, ptr r5, size r6) -> r3 the block (or 0).
// It may call RtlAllocateHeap and RtlFreeHeap itself (both hooked above).
REX_HOOK_RAW(sub_823484C0) {
  const uint32_t ptr = ctx.r5.u32;
  if (!On() || ptr == 0) {
    __imp__sub_823484C0(ctx, base);
    return;
  }
  const uint32_t heap = ctx.r3.u32, flags = ctx.r4.u32, size = ctx.r6.u32;
  const Stack stack = Capture(ctx, base);
  Live was;
  const bool had = Removing(ctx, base, heap, flags, ptr, stack, "reallocation", &was);
  ++t_in_realloc;
  __imp__sub_823484C0(ctx, base);
  --t_in_realloc;
  const uint32_t result = ctx.r3.u32;
  {
    std::lock_guard lock(g_mu);
    ++g_reallocs;
    if (!result && had) g_live[ptr] = was;               // failed: the old block stays
    if (result && result != ptr && had) Remember(ptr, was, stack);  // moved
  }
  if (result) Added(ctx, base, heap, flags, result, size, stack, "reallocation");
  MaybeSweep(ctx, base);
}
