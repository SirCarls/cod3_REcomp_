// cod3 - [acct2-main] 2026-11-21 The text-copy check (see text_copy_check.h).

#include "game/text_copy_check.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "game/text_buffers.h"  // [acct2-main] kLevelInfoReturn
#include "game/watchdog.h"

REXCVAR_DEFINE_BOOL(cod3_text_copy_check, false, "cod3/Debug",
                    "Follow every copy the game makes of our translated text (memcpy, strncpy, strncat, sprintf ...) "
                    "and log, once per place in the game's code, a copy that cuts the text short or runs past its "
                    "stack frame; cod3_text_copy_report lists every place seen (dev check, off by default)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::textcopy {

std::atomic<int> g_state{0};

namespace {

// ---------------------------------------------------------------------------
// Guest memory
// ---------------------------------------------------------------------------

// Read only below 0xE0000000 (on Windows the host view above it sits 4 KB further on) and outside the GPU register
// window: what lies elsewhere is left unchecked.
constexpr uint32_t kMmioBegin = 0x7F000000u, kMmioEnd = 0x80000000u, kHighView = 0xE0000000u;
constexpr uint32_t kImageBegin = 0x82000000u, kImageEnd = 0x84000000u;  // the program's image (both programs)
constexpr uint32_t kMaxScan = 64 * 1024;  // the longest string followed

inline bool Readable(uint32_t at, uint64_t n) {
  const uint64_t end = uint64_t(at) + n;
  if (at < 0x10000u || end > kHighView) return false;
  return !(at < kMmioEnd && end > kMmioBegin);
}

// [addr, addr + n) is committed, readable guest memory (asked of the guest heaps, as heap_check.cpp does; the answer for
// a page is kept a while). Every read here of memory the copy itself doesn't read goes through this: the stack frames'
// links and saved return addresses. (2026-11-21 first runs: reading the word at the top of a thread's stack faulted.)
bool Mapped(uint32_t addr, uint32_t n) {
  if (!Readable(addr, n)) return false;
  struct Entry {
    uint32_t page = 1;  // never a page address
    bool ok = false;
  };
  thread_local std::array<Entry, 64> cache;
  thread_local uint32_t uses = 0;
  if (++uses == 1u << 16) {
    uses = 0;
    cache.fill(Entry{});
  }
  const uint64_t last = uint64_t(addr) + n - 1;
  for (uint64_t page = addr & ~0xFFFull; page <= last; page += 0x1000) {
    const uint32_t p = uint32_t(page);
    Entry& e = cache[(p >> 12) & 63];
    if (e.page != p) {
      auto* mem = rex::system::kernel_state()->memory();
      auto* heap = mem ? mem->LookupHeap(p) : nullptr;
      uint32_t protect = 0;
      e.page = p;
      e.ok = heap && heap->QueryProtect(p, &protect) && (protect & rex::memory::kMemoryProtectRead);
    }
    if (!e.ok) return false;
  }
  return true;
}

inline uint32_t Rd32(uint8_t* base, uint32_t at) {
  uint32_t v;
  std::memcpy(&v, base + at, 4);
  return __builtin_bswap32(v);
}

// strnlen within what may be read, up to `limit` bytes; *ended: a zero was found.
uint32_t StrLen(uint8_t* base, uint32_t at, uint32_t limit, bool* ended) {
  *ended = false;
  if (!Readable(at, 1)) return 0;
  const uint64_t room = std::min<uint64_t>(limit, uint64_t(kHighView) - at);
  const uint64_t run = at < kMmioBegin ? std::min<uint64_t>(room, uint64_t(kMmioBegin) - at) : room;
  const void* z = run ? std::memchr(base + at, 0, size_t(run)) : nullptr;
  if (z) {
    *ended = true;
    return uint32_t(static_cast<const uint8_t*>(z) - (base + at));
  }
  return uint32_t(run);
}

// ---------------------------------------------------------------------------
// Our text: registered ranges, and a bit for each 64 bytes of guest memory holding our text or a copy of it.
// ---------------------------------------------------------------------------

constexpr uint32_t kGrainShift = 6;
constexpr size_t kWords = (size_t(1) << (32 - kGrainShift)) / 64;  // 8 MB of bits

std::mutex g_ranges_mutex;
std::vector<std::pair<uint32_t, uint32_t>> g_ranges;
std::unique_ptr<std::atomic<uint64_t>[]> g_bits_owner;
std::atomic<std::atomic<uint64_t>*> g_bits{nullptr};

void SetBits(uint32_t at, uint64_t n, bool on, bool whole_only) {
  std::atomic<uint64_t>* bits = g_bits.load(std::memory_order_acquire);
  if (!bits || n == 0) return;
  uint64_t first = uint64_t(at) >> kGrainShift, last = (uint64_t(at) + n - 1) >> kGrainShift;
  if (whole_only) {  // only the 64-byte steps the range covers in full
    if (at & 63u) ++first;
    if (((uint64_t(at) + n) & 63u) != 0) {
      if (last == 0) return;
      --last;
    }
    if (first > last) return;
  }
  last = std::min<uint64_t>(last, (uint64_t(1) << (32 - kGrainShift)) - 1);
  for (uint64_t g = first; g <= last; ++g) {
    const uint64_t mask = uint64_t(1) << (g & 63);
    if (on)
      bits[g >> 6].fetch_or(mask, std::memory_order_relaxed);
    else
      bits[g >> 6].fetch_and(~mask, std::memory_order_relaxed);
  }
}

bool AnyBit(uint32_t at, uint64_t n) {
  std::atomic<uint64_t>* bits = g_bits.load(std::memory_order_acquire);
  if (!bits || n == 0) return false;
  const uint64_t first = uint64_t(at) >> kGrainShift;
  const uint64_t last = std::min<uint64_t>((uint64_t(at) + n - 1) >> kGrainShift, first + 1024);
  for (uint64_t g = first; g <= last && g < (uint64_t(1) << (32 - kGrainShift)); ++g)
    if (bits[g >> 6].load(std::memory_order_relaxed) & (uint64_t(1) << (g & 63))) return true;
  return false;
}

// How long our text at `at` runs (its zero not counted), following it only while it stays in our memory.
uint32_t OurLength(uint8_t* base, uint32_t at) {
  uint32_t len = 0;
  while (len < kMaxScan) {
    const uint32_t p = at + len;
    if (!AnyBit(p, 1)) return len;
    const uint32_t step = 64 - (p & 63u);
    bool ended = false;
    const uint32_t k = StrLen(base, p, step, &ended);
    len += k;
    if (ended || k < step) return len;
  }
  return len;
}

// ---------------------------------------------------------------------------
// The places in the game's code that copy
// ---------------------------------------------------------------------------

constexpr const char* kKindName[kKindCount] = {"memcpy", "XMemCpy", "memmove", "strcpy", "strncpy", "strncat", "sprintf"};

enum Where : uint8_t { kUnknown, kStack, kImage, kHeap };

struct Key {
  uint32_t lr, outer;
  Kind kind;
  bool operator==(const Key& o) const { return lr == o.lr && outer == o.outer && kind == o.kind; }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    return std::hash<uint64_t>()((uint64_t(k.lr) << 32 | k.outer) ^ (uint64_t(k.kind) << 61));
  }
};

struct Site {
  uint64_t calls = 0, ours = 0, cut = 0, over = 0;
  uint32_t longest = 0, longest_ours = 0;   // bytes written (strings: without the zero; sprintf: characters)
  uint32_t min_room = UINT32_MAX;            // stack: the least room seen to the frame's end (every copy located)
  uint32_t dst = 0;                          // the last destination of our text
  Where where = kUnknown;
  uint8_t depth = 0;                         // stack: frames above the caller's (0: the caller's own)
  bool said_ours = false, said_cut = false, said_over = false, changed = true;
};

std::mutex g_sites_mutex;
std::unordered_map<Key, Site, KeyHash> g_sites;
std::atomic<int64_t> g_next_report{0};

Where Locate(uint8_t* base, uint32_t r1, uint32_t dst, uint32_t* room, uint8_t* depth) {
  *room = UINT32_MAX;
  *depth = 0;
  if (dst >= kImageBegin && dst < kImageEnd) return kImage;
  uint32_t f = r1;
  for (uint8_t d = 0; d < 16; ++d) {
    // Only frames at or below the destination are read: a destination above them all (heap, another stack) stops
    // the walk before the last link, which may be the top of the stack.
    if (dst < f || !Mapped(f, 4)) break;
    const uint32_t next = Rd32(base, f);
    if (next <= f || next - f > 0x100000u) break;
    if (dst >= f && dst < next) {
      *room = next - dst;
      *depth = d;
      return kStack;
    }
    f = next;
  }
  return kHeap;
}

// The return address the frame `r1`'s caller saved (8 bytes below its frame's top), 0 when not readable.
uint32_t OuterReturn(uint8_t* base, uint32_t r1) {
  if (!Mapped(r1, 4)) return 0;
  const uint32_t top = Rd32(base, r1);
  if (top <= r1 || top - r1 > 0x100000u || !Mapped(top - 8, 4)) return 0;
  return Rd32(base, top - 8);
}

std::string Sample(const uint8_t* p, uint32_t n) {
  std::string s;
  for (uint32_t i = 0; i < n && i < 72; ++i) {
    const uint8_t c = p[i];
    if (c == '\n')
      s += "\\n";
    else if (c < 0x20)
      s += '.';
    else
      s += char(c);
  }
  if (n > 72) s += "...";
  return s;
}

std::string Name(uint32_t addr) { return addr ? cod3::watchdog::GuestCodeName(addr) : std::string("?"); }

std::string Describe(const Key& k, const Site& s) {
  std::string where;
  switch (s.where) {
    case kStack:
      where = "stack (frame +" + std::to_string(s.depth) + ", room " +
              (s.min_room == UINT32_MAX ? std::string("?") : std::to_string(s.min_room)) + ")";
      break;
    case kImage: {
      char b[32];
      std::snprintf(b, sizeof(b), "static %08X", s.dst);
      where = b;
      break;
    }
    case kHeap: where = "heap"; break;
    default: where = "?"; break;
  }
  return std::string(kKindName[k.kind]) + " at " + Name(k.lr) + " (from " + Name(k.outer) + "): calls " +
         std::to_string(s.calls) + ", our text " + std::to_string(s.ours) + ", longest " +
         std::to_string(s.longest) + " (ours " + std::to_string(s.longest_ours) + "), into " + where + ", cut " +
         std::to_string(s.cut) + ", over " + std::to_string(s.over);
}

void Report(bool all) {
  std::vector<std::pair<Key, Site>> rows;
  {
    std::lock_guard lock(g_sites_mutex);
    for (auto& [k, s] : g_sites) {
      if (!all && !s.changed) continue;
      rows.emplace_back(k, s);
      s.changed = false;
    }
  }
  if (rows.empty()) return;
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    if ((a.second.cut + a.second.over != 0) != (b.second.cut + b.second.over != 0))
      return a.second.cut + a.second.over != 0;
    if (a.second.longest_ours != b.second.longest_ours) return a.second.longest_ours > b.second.longest_ours;
    return a.second.longest > b.second.longest;
  });
  REXLOG_INFO("cod3: text copy report ({}): {} places", all ? "all" : "changed", rows.size());
  for (const auto& [k, s] : rows) REXLOG_INFO("cod3: text copy: {}", Describe(k, s));
}

void MaybeReport() {
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t next = g_next_report.load(std::memory_order_relaxed);
  if (now < next) return;
  if (!g_next_report.compare_exchange_strong(next, now + 60)) return;
  if (next != 0) Report(false);
}

void ReportCommand(std::string_view) { Report(true); }

// One copy: `w`/`wn` what it writes; `len` the bytes of text it copies; `full` how long the text was (ours only).
void Note(Kind kind, const PPCContext& ctx, uint8_t* base, uint32_t lr, uint32_t outer, uint32_t r1, uint32_t w,
          uint32_t wn, uint32_t len, bool ours, bool cut, const uint8_t* text, uint32_t text_n) {
  uint32_t room = UINT32_MAX;
  uint8_t depth = 0;
  const Where where = ours || (kind != kMemcpy && kind != kXMemCpy && kind != kMemmove) ? Locate(base, r1, w, &room, &depth) : kUnknown;
  const bool over = where == kStack && uint64_t(wn) > room;
  bool say_ours = false, say_cut = false, say_over = false;
  Site copy;
  const Key key{lr, outer, kind};
  {
    std::lock_guard lock(g_sites_mutex);
    Site& s = g_sites[key];
    ++s.calls;
    s.longest = std::max(s.longest, len);
    s.changed = true;
    if (s.where == kUnknown || ours) s.where = where, s.depth = depth;
    // [acct2-main] 2026-11-21 (tcw2): where and how much room for every copy, so the table says it for the game's own
    // text too (it said "static 00000000" and "room ?"); our text's destination wins once there is one.
    if (where == kStack) s.min_room = std::min(s.min_room, room);
    if (!s.ours) s.dst = w;
    if (ours) {
      ++s.ours;
      s.longest_ours = std::max(s.longest_ours, len);
      s.dst = w;
      if (!s.said_ours) s.said_ours = say_ours = true;
      if (cut) {
        ++s.cut;
        if (!s.said_cut) s.said_cut = say_cut = true;
      }
    }
    if (over) {
      ++s.over;
      if (!s.said_over) s.said_over = say_over = true;
    }
    copy = s;
  }
  if (say_cut || say_over) {
    REXLOG_ERROR("cod3: text copy {}{}: {} \"{}\" ({} bytes into {:08X}, {} bytes of room)", say_cut ? "CUT" : "",
                 say_over ? (say_cut ? " and OVER" : "OVER") : "", Describe(key, copy),
                 text ? Sample(text, text_n) : std::string(), wn, w,
                 room == UINT32_MAX ? std::string("?") : std::to_string(room));
    REXLOG_ERROR("cod3: text copy stack: {}", cod3::watchdog::GuestStack(ctx, base));
  } else if (say_ours) {
    REXLOG_INFO("cod3: text copy first: {} \"{}\"", Describe(key, copy), text ? Sample(text, text_n) : std::string());
  }
  // What it writes is ours from now on, or no longer.
  if (ours)
    SetBits(w, wn, true, false);
  else
    SetBits(w, wn, false, true);
  MaybeReport();
}

void Apply() {
  const bool on = REXCVAR_GET(cod3_text_copy_check);
  if (on && !g_bits.load(std::memory_order_acquire)) {
    std::lock_guard lock(g_ranges_mutex);
    if (!g_bits_owner) {
      g_bits_owner.reset(new std::atomic<uint64_t>[kWords]());
      g_bits.store(g_bits_owner.get(), std::memory_order_release);
      for (const auto& [at, size] : g_ranges) SetBits(at, size, true, false);
    }
  }
  size_t ranges = 0;
  {
    std::lock_guard lock(g_ranges_mutex);
    ranges = g_ranges.size();
  }
  const int was = g_state.exchange(on ? 1 : 2, std::memory_order_relaxed);
  if (was != (on ? 1 : 2) && (on || was == 1))
    REXLOG_INFO("cod3: text copy check {} ({} ranges of our text known)", on ? "on" : "off", ranges);
}

std::once_flag g_once;

}  // namespace

REXCVAR_DEFINE_COMMAND_ARGS(cod3_text_copy_report, ReportCommand, "cod3/Debug",
                            "cod3_text_copy_report: every place in the game's code seen copying text since "
                            "cod3_text_copy_check went on, written to the log");

bool ReadSetting() {
  std::call_once(g_once, [] {
    Apply();
    rex::cvar::RegisterChangeCallback("cod3_text_copy_check", [](std::string_view, std::string_view) { Apply(); });
  });
  return g_state.load(std::memory_order_relaxed) == 1;
}

void NoteOurText(uint32_t at, uint32_t size) {
  std::lock_guard lock(g_ranges_mutex);
  g_ranges.emplace_back(at, size);
  SetBits(at, size, true, false);
}

bool IsOurs(uint32_t at, uint64_t size) { return AnyBit(at, size); }

void Before(Kind kind, const PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  const uint32_t lr = uint32_t(ctx.lr), r1 = ctx.r1.u32;
  const bool ours = AnyBit(src, 1);
  if (kind == kMemcpy || kind == kXMemCpy || kind == kMemmove) {
    if (!ours || n == 0) {
      if (n) SetBits(dst, n, false, true);
      return;  // the game's other block copies are not followed
    }
    // [acct2-main] 2026-11-21 (tcw2): never "cut". A block copy is given its length by its caller, and the game copies a
    // part of a text on purpose (its config and command parsers take a line or a word out of a whole file: "exec
    // xenon_a.cfg" out of the default binds was reported cut). Only where it lands is checked (over).
    const uint32_t full = OurLength(base, src);
    Note(kind, ctx, base, lr, OuterReturn(base, r1), r1, dst, n, std::min(n, full), true, false,
         Readable(src, std::min(n, full)) ? base + src : nullptr, std::min(n, full));
    return;
  }
  bool ended = false;
  uint32_t w = dst, wn = 0, len = 0;
  bool cut = false;
  if (kind == kStrcpy) {
    len = StrLen(base, src, kMaxScan, &ended);
    wn = len + 1;
  } else if (kind == kStrncpy) {
    if (n == 0) return;
    len = StrLen(base, src, n, &ended);
    wn = n;  // strncpy fills the rest with zeros
    cut = len == n && (!ours || OurLength(base, src) >= n);  // no zero within n: cut, the buffer unterminated
    // The level's loading-screen text: the game goes on with the whole of it (text_buffers.h, LevelInfoCopied).
    if (cut && lr == cod3::textbuf::kLevelInfoReturn && cod3::textbuf::On()) cut = false;
  } else if (kind == kStrncat) {
    bool dst_ended = false;
    const uint32_t have = StrLen(base, dst, kMaxScan, &dst_ended);
    len = StrLen(base, src, n, &ended);
    w = dst + have;
    wn = len + 1;
    cut = ours && len == n && OurLength(base, src) > n;
  }
  Note(kind, ctx, base, lr, OuterReturn(base, r1), r1, w, wn, len, ours, cut && ours,
       Readable(src, len) ? base + src : nullptr, len);
}

void BeforePrintf(const PPCContext& ctx, uint8_t* base, uint32_t out, uint32_t n, bool sized, uint32_t cnt,
                  bool ours, const uint8_t* text) {
  const uint32_t r1 = ctx.r1.u32;
  // _output_l is called by sprintf, _vsnprintf and the like: the place that matters is their caller.
  const uint32_t lr = OuterReturn(base, r1), wrapper = uint32_t(ctx.lr);
  if (!sized) {
    // Format left it to the game's code (a float, a wide string ...): still a place seen, its length unknown.
    Note(kPrintf, ctx, base, lr, wrapper, r1, out, 1, 0, ours, false, nullptr, 0);
    return;
  }
  // A bounded call (_vsnprintf: _cnt is what is left) that our text doesn't fit.
  const bool cut = int32_t(cnt) != INT32_MAX && int64_t(n) > int64_t(int32_t(cnt));
  Note(kPrintf, ctx, base, lr, wrapper, r1, out, n + 1, n, ours, cut, text, n);
}

void AfterPrintf(uint32_t out, uint32_t n, bool ours) {
  if (!n) return;
  if (ours)
    SetBits(out, n, true, false);
  else
    SetBits(out, n, false, true);
}

}  // namespace cod3::textcopy
