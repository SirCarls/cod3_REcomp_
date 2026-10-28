// cod3 - [native-crt] the game's busiest C runtime functions as native code:
// memcpy, memset, memmove and XMemCpy; _stricmp, strncmp, strcpy, strncpy,
// strchr and strstr; since setup tool 2.12.7 the rest of the small string
// functions too: _strnicmp, strrchr, strncat, memchr, strcspn, _strlwr and
// _strupr.
//
// These are the Xbox 360 C runtime's own PowerPC code, translated by the
// recompiler (memcpy alone has 682 call sites; with XMemCpy and memset they
// were 1.5-3% of the game thread in the function profiles; _stricmp has 484).
// They only read and write guest memory, so the host's memcpy / memset /
// memmove can do the block functions' work with the CPU's widest moves, and
// the string functions are plain byte loops over host pointers.
//
// A native version runs only where it provably does what the translated code
// does: the same bytes written and the same r3 returned.
//   - Each range is plain guest memory reached through one host pointer, the
//     way the generated code reaches it (REX_RAW_ADDR in the generated
//     cod3_pch.h): no wrap past 4 GB, nothing in the GPU register window
//     0x7F000000-0x7FFFFFFF, and no range across 0xE0000000 (on Windows the
//     host view of 0xE0000000 and up sits 4 KB further on).
//   - memcpy and XMemCpy: the ranges don't overlap (the guest code copies
//     forward in blocks and XMemCpy clears destination cache lines first, so
//     an overlapping copy is left to it).
//   - memmove: an overlapping move whose ranges sit on both sides of
//     0x80000000 is left to the guest code (it picks its direction with a
//     signed compare).
//   - The length is r5's low 32 bits, as the guest code reads it; r3 comes
//     back as the guest code leaves it (memmove's backward path adds r5's high
//     half, so that is done too).
//   - The string functions read only what the guest code reads; a string that
//     runs past plain guest memory goes to the guest code before anything is
//     written, and so do overlapping strcpy/strncpy. Their quirks are kept:
//     _stricmp returns s1's byte unfolded when s2 ends first, strncmp treats
//     n as signed, strchr compares c as a 32-bit value.
//   - 2.12.7's functions: what the error checks of _strnicmp, _strlwr and
//     _strupr catch (a null pointer, a count above INT_MAX) goes to the guest
//     code (errno and the invalid parameter handler). Those three write a
//     stack frame below r1 and strcspn a 32-byte bitmap there; the native
//     versions write the same bytes, and a string that overlaps that area
//     goes to the guest code (it would read what was just written). strrchr
//     and memchr compare c as a 32-bit value, memchr's count is r5's low 32
//     bits unsigned, strncat's too; an strncat whose source overlaps what it
//     writes goes to the guest code.
// Everything else runs the original (__imp__) code.
//
// Before the first native call, a self-check runs every original and every
// native version on two identical blocks of scratch guest memory (thousands of
// random lengths, alignments, overlaps, fill values and junk in the registers'
// high halves) and compares every byte of the blocks and r3. A function that
// differs once, or that the check couldn't run natively, stays on the original
// code for the session (logged). cod3_native_crt (default on) switches them
// all off at any time.
//
// Measured on the owner's PC (2026-10-20, bench claude-setup-crt-ab/ab2, Saint-Lo
// and Forest): the same frame rate (the GPU emulation thread sets it), about
// 2.4 GB a minute of copies and fills done natively, nothing differed.
//
// Both programs: cod3mp (the multiplayer program) builds this file too, and
// tools/mp_aliases.py points these names at its own copies of the functions
// (mp/config/campaign_map.tsv); its self-check runs against those copies.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(cod3_native_crt, true, "cod3/Performance",
                    "memcpy, memset, memmove, XMemCpy and the string functions (_stricmp, "
                    "strncmp, strcpy, strncpy, strchr, strstr, _strnicmp, strrchr, strncat, "
                    "memchr, strcspn, _strlwr, _strupr) run as native code where that does "
                    "exactly what the game's own code does (checked at start; off: the "
                    "translated Xbox 360 code)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [claude-9b-think] 2026-10-27 UNTESTED (FINDINGS-FRAME-CALLS): the per-call counters and their log line
// only on request. claude-9b-test's s138 profile (mp_merv, 4 players moving): Use() and Mine() were 0.6% of
// the game's main thread (~25 us a frame), mostly the setting read on every call and the thread-local
// counters; the counts are only ever read by the log line.
REXCVAR_DEFINE_BOOL(cod3_native_crt_counts, false, "cod3/Performance",
                    "Count the native C runtime calls (cod3_native_crt) and log the counts a minute after "
                    "the start check and then every 10 minutes (off: nothing is counted)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__memcpy_guest);   // 0x8234F460
REX_EXTERN(__imp__memset_guest);   // 0x8234EBA0
REX_EXTERN(__imp__memmove_guest);  // 0x8234E220
REX_EXTERN(__imp__sub_82343548);   // XMemCpy
REX_EXTERN(__imp___stricmp_guest);  // 0x823536D0
REX_EXTERN(__imp__strncmp_guest);   // 0x8234B6F0
REX_EXTERN(__imp__strcpy_guest);    // 0x8234EB30
REX_EXTERN(__imp__strncpy_guest);   // 0x8234C7F0
REX_EXTERN(__imp__strchr_guest);    // 0x8234C2A0
REX_EXTERN(__imp__strstr_guest);    // 0x8234BB48
REX_EXTERN(__imp__sub_82353618);    // _strnicmp (cod3mp sub_82323220)
REX_EXTERN(__imp__sub_8234E890);    // strrchr (cod3mp sub_8231DDD0)
REX_EXTERN(__imp__sub_8234E600);    // strncat (cod3mp sub_8231DC60)
REX_EXTERN(__imp__sub_8234E650);    // memchr (cod3mp sub_8231DCB0)
REX_EXTERN(__imp__sub_8234E8E8);    // strcspn (cod3mp sub_8231DE28)
REX_EXTERN(__imp__sub_82353528);    // _strlwr (cod3mp sub_82323130)
REX_EXTERN(__imp__sub_82353728);    // _strupr (cod3mp sub_82323328)

namespace {

// ---------------------------------------------------------------------------
// How the generated code reaches guest memory
// ---------------------------------------------------------------------------

// REX_PHYS_HOST_OFFSET in the generated cod3_pch.h, same condition: there the
// host view of guest 0xE0000000-0xFFFFFFFF sits 4 KB further on.
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && defined(REX_ARCH_ARM64))
constexpr uint32_t kHighViewOffset = 0x1000;
#else
constexpr uint32_t kHighViewOffset = 0;
#endif
constexpr uint32_t kHighView = 0xE0000000u;
constexpr uint32_t kMmioBegin = 0x7F000000u;  // REX_IS_MMIO_ADDR
constexpr uint32_t kMmioEnd = 0x80000000u;

// Set by the self-check when the memory's own address math (TranslateVirtual)
// agrees with kHighViewOffset; until then ranges at 0xE0000000 and up go to
// the guest code.
std::atomic<bool> g_high_view_ok{false};

inline uint8_t* Host(uint8_t* base, uint32_t addr) {
  return base + addr + (addr >= kHighView ? kHighViewOffset : 0u);
}

// [addr, addr + n) is reached through Host(base, addr) the way the generated
// code reaches each of its bytes. n > 0.
inline bool Plain(uint32_t addr, uint32_t n) {
  const uint64_t end = uint64_t(addr) + n;
  if (end > 0x100000000ull) return false;
  if (addr < kMmioEnd && end > kMmioBegin) return false;
  if (end > kHighView) {
    if (addr < kHighView) return false;  // across the view boundary
    if (!g_high_view_ok.load(std::memory_order_relaxed)) return false;
  }
  return true;
}

inline bool Overlap(uint32_t a, uint32_t b, uint32_t n) {
  return uint64_t(a) < uint64_t(b) + n && uint64_t(b) < uint64_t(a) + n;
}

// ---------------------------------------------------------------------------
// The native versions. Each returns true when it did the call's work (r3 left
// as the guest code leaves it), false when the call goes to the guest code.
// ---------------------------------------------------------------------------

// memcpy(r3 = dst, r4 = src, r5 = n): returns r3 as passed (the guest code
// saves it and loads it back).
bool NativeMemcpy(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n == 0) return true;
  if (!Plain(dst, n) || !Plain(src, n) || Overlap(dst, src, n)) return false;
  std::memcpy(Host(base, dst), Host(base, src), n);
  return true;
}

// XMemCpy(r3 = dst, r4 = src, r5 = n): returns r3 as passed (kept in r27).
bool NativeXMemCpy(PPCContext& ctx, uint8_t* base) { return NativeMemcpy(ctx, base); }

// memset(r3 = dst, r4 = value, r5 = n): stores r4's low byte; returns r3 as
// passed (never written).
bool NativeMemset(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, n = ctx.r5.u32;
  if (n == 0) return true;
  if (!Plain(dst, n)) return false;
  std::memset(Host(base, dst), ctx.r4.u8, n);
  return true;
}

// memmove(r3 = dst, r4 = src, r5 = n). The guest code: dst == src returns at
// once; dst < src as signed 32-bit values goes to memcpy (forward, r3 as
// passed); otherwise it copies backward, ending with r3 = dst + r5's high half.
bool NativeMemmove(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (dst == src) return true;
  const bool backward = int32_t(dst) > int32_t(src);
  if (n != 0) {
    if (!Plain(dst, n) || !Plain(src, n)) return false;
    // Overlapping on both sides of 0x80000000: the signed compare picks the
    // other direction than an unsigned one would.
    if (Overlap(dst, src, n) && ((dst ^ src) & 0x80000000u)) return false;
    std::memmove(Host(base, dst), Host(base, src), n);
  }
  if (backward) ctx.r3.u64 += ctx.r5.u64 & 0xFFFFFFFF00000000ull;
  return true;
}


// --- The string functions ---------------------------------------------------
// Byte loops over host pointers, reading what the guest code reads. A string
// that runs past plain guest memory (see PlainRun) goes to the guest code
// before anything is written.

// How many bytes from addr on are reached through Host(base, addr) the way the
// generated code reaches each of them (0: the guest code takes it).
inline uint64_t PlainRun(uint32_t addr) {
  if (addr < kMmioBegin) return uint64_t(kMmioBegin) - addr;
  if (addr < kMmioEnd) return 0;
  if (addr < kHighView) return uint64_t(kHighView) - addr;
  return g_high_view_ok.load(std::memory_order_relaxed) ? 0x100000000ull - addr : 0;
}

// The first NUL at or after addr within `limit` bytes (and the plain run).
// Returns false when the run ends first; *found is false when `limit` bytes
// had no NUL (then *len == limit).
inline bool ScanNul(uint8_t* base, uint32_t addr, uint64_t limit, uint64_t* len, bool* found) {
  const uint64_t run = PlainRun(addr);
  const uint64_t span = run < limit ? run : limit;
  const uint8_t* p = Host(base, addr);
  const void* z = span ? std::memchr(p, 0, size_t(span)) : nullptr;
  if (z) {
    *len = uint64_t(static_cast<const uint8_t*>(z) - p);
    *found = true;
    return true;
  }
  if (span < limit) return false;  // the plain run ended before `limit`
  *len = limit;
  *found = false;
  return true;
}

inline bool Overlap2(uint32_t a, uint64_t na, uint32_t b, uint64_t nb) {
  return uint64_t(a) < uint64_t(b) + nb && uint64_t(b) < uint64_t(a) + na;
}

inline uint32_t FoldUpper(uint32_t c) { return (c >= 'A' && c <= 'Z') ? (c | 32u) : c; }

// _stricmp(r3 = s1, r4 = s2): byte by byte; when s2's byte is 0 it returns
// s1's byte minus 0 (not folded); bytes that differ are compared folded (A-Z
// only). r3 is the 64-bit difference.
bool NativeStricmp(PPCContext& ctx, uint8_t* base) {
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
  const uint64_t ra = PlainRun(a), rb = PlainRun(b);
  const uint8_t* p = Host(base, a);
  const uint8_t* q = Host(base, b);
  for (uint64_t i = 0;; ++i) {
    if (i >= ra || i >= rb) return false;
    const uint32_t c2 = q[i], c1 = p[i];
    int64_t d = int64_t(c1) - int64_t(c2);
    if (c2 == 0) {
      ctx.r3.s64 = d;
      return true;
    }
    if (d == 0) continue;
    d = int64_t(FoldUpper(c1)) - int64_t(FoldUpper(c2));
    if (d != 0) {
      ctx.r3.s64 = d;
      return true;
    }
  }
}

// strncmp(r3 = s1, r4 = s2, r5 = n): n <= 0 (signed) returns 0; else the
// difference of the last pair compared, stopping after n pairs, at a 0 in s1
// or at the first difference.
bool NativeStrncmp(PPCContext& ctx, uint8_t* base) {
  if (ctx.r5.s32 <= 0) {
    ctx.r3.s64 = 0;
    return true;
  }
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32, n = ctx.r5.u32;
  const uint64_t ra = PlainRun(a), rb = PlainRun(b);
  const uint8_t* p = Host(base, a);
  const uint8_t* q = Host(base, b);
  for (uint64_t i = 0;; ++i) {
    if (i >= ra || i >= rb) return false;
    const uint32_t c1 = p[i], c2 = q[i];
    const int64_t d = int64_t(c1) - int64_t(c2);
    if (i + 1 >= n || c1 == 0 || d != 0) {
      ctx.r3.s64 = d;
      return true;
    }
  }
}

// strcpy(r3 = dst, r4 = src): copies through the NUL; r3 as passed.
// Overlapping strings go to the guest code (its byte order is its own).
bool NativeStrcpy(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
  uint64_t len = 0;
  bool found = false;
  if (!ScanNul(base, src, 0x100000000ull, &len, &found) || !found) return false;
  const uint64_t total = len + 1;
  if (total > 0xFFFFFFFFull || !Plain(dst, uint32_t(total)) || Overlap2(dst, total, src, total))
    return false;
  std::memcpy(Host(base, dst), Host(base, src), size_t(total));
  return true;
}

// strncpy(r3 = dst, r4 = src, r5 = n, low 32 bits): n bytes written: the
// string through its NUL and zeros after it, or n bytes of a longer one;
// r3 as passed.
bool NativeStrncpy(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (n == 0) return true;
  uint64_t len = 0;
  bool found = false;
  if (!ScanNul(base, src, n, &len, &found)) return false;
  const uint64_t copy = found ? len + 1 : uint64_t(n);
  if (!Plain(dst, n) || Overlap2(dst, n, src, copy)) return false;
  uint8_t* d = Host(base, dst);
  std::memcpy(d, Host(base, src), size_t(copy));
  if (copy < n) std::memset(d + copy, 0, size_t(n - copy));
  return true;
}

// strchr(r3 = s, r4 = c): c is compared as a signed 32-bit value with each
// zero-extended byte (so c outside 1..255 matches only the NUL when c == 0).
// Found: r3's low half is the address (its high half kept); not found: 0.
bool NativeStrchr(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32;
  const int32_t c = ctx.r4.s32;
  const uint64_t run = PlainRun(s);
  const uint8_t* p = Host(base, s);
  for (uint64_t i = 0;; ++i) {
    if (i >= run) return false;
    const int32_t b = p[i];
    if (c == 0 ? b == 0 : b == c) {
      ctx.r3.u32 = uint32_t(s + i);
      return true;
    }
    if (b == 0) {
      ctx.r3.s64 = 0;
      return true;
    }
  }
}

// strstr(r3 = hay, r4 = needle): an empty needle returns r3 as passed; found:
// r3 + the position (64-bit add); not found: 0.
bool NativeStrstr(PPCContext& ctx, uint8_t* base) {
  // Reads only what the guest code reads, in the same order of need: the
  // needle lazily (a byte past its run goes to the guest code).
  const uint32_t hay = ctx.r3.u32, nee = ctx.r4.u32;
  const uint64_t rn = PlainRun(nee);
  if (rn == 0) return false;
  const uint8_t* pn = Host(base, nee);
  if (pn[0] == 0) return true;
  const uint64_t run = PlainRun(hay);
  const uint8_t* ph = Host(base, hay);
  for (uint64_t pos = 0;; ++pos) {
    if (pos >= run) return false;
    if (ph[pos] == 0) {
      ctx.r3.s64 = 0;
      return true;
    }
    uint64_t k = 0;
    for (;;) {
      if (k >= rn) return false;
      if (pn[k] == 0) {
        ctx.r3.u64 += pos;
        return true;
      }
      if (pos + k >= run) return false;
      if (ph[pos + k] != pn[k]) break;
      ++k;
      if (pos + k >= run) return false;
      if (ph[pos + k] == 0) {
        if (k >= rn) return false;
        if (pn[k] == 0) {
          ctx.r3.u64 += pos;
          return true;
        }
        break;
      }
    }
  }
}

// --- 2.12.7: the rest of the small string functions --------------------------

// Guest memory is big-endian.
inline void StBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
inline uint32_t LdBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void StBE64(uint8_t* p, uint64_t v) {
  StBE32(p, uint32_t(v >> 32));
  StBE32(p + 4, uint32_t(v));
}

// The 96-byte frame _strnicmp, _strlwr and _strupr make below r1 (mflr r12;
// stw r12,-8(r1); [std r31,-16(r1)]; stwu r1,-96(r1)), written before they
// read anything. False when it isn't plain guest memory.
constexpr uint32_t kFrame = 96;
inline bool FramePlain(uint32_t sp) { return sp >= kFrame && Plain(sp - kFrame, kFrame); }
inline void WriteFrame(PPCContext& ctx, uint8_t* base, bool with_r31) {
  const uint32_t sp = ctx.r1.u32;
  StBE32(Host(base, sp - 8), uint32_t(ctx.lr));
  if (with_r31) StBE64(Host(base, sp - 16), ctx.r31.u64);
  StBE32(Host(base, sp - kFrame), sp);
  ctx.lr = uint32_t(ctx.lr);  // lwz r12,-8(r1); mtlr r12
}

// _strnicmp (sub_82353618; r3 = s1, r4 = s2, r5 = n): s1 or s2 null, or n
// (low 32 bits, unsigned) above 0x7FFFFFFF, set errno and call the invalid
// parameter handler: the guest code. Else its core (sub_823535B8) compares
// pairs with A-Z folded (c - 65 <= 25 unsigned, + 32), stopping after n pairs
// (addic. r5,r5,-1 and a 32-bit compare), at a 0 in s1 or at the first
// difference; r3 is the 64-bit difference of the last pair, 0 for n == 0.
bool NativeStrnicmp(PPCContext& ctx, uint8_t* base) {
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32, n = ctx.r5.u32;
  if (a == 0 || b == 0 || n > 0x7FFFFFFFu) return false;
  const uint32_t sp = ctx.r1.u32;
  if (!FramePlain(sp)) return false;
  int64_t d = 0;
  if (n != 0) {
    const uint64_t ra = PlainRun(a), rb = PlainRun(b);
    const uint8_t* p = Host(base, a);
    const uint8_t* q = Host(base, b);
    uint64_t k = 0;  // pairs read
    for (;;) {
      if (k >= ra || k >= rb) return false;
      const uint32_t c1 = FoldUpper(p[k]), c2 = FoldUpper(q[k]);
      ++k;
      if (k >= n || c1 == 0 || c1 != c2) {
        d = int64_t(c1) - int64_t(c2);
        break;
      }
    }
    if (Overlap2(sp - kFrame, kFrame, a, k) || Overlap2(sp - kFrame, kFrame, b, k)) return false;
  }
  WriteFrame(ctx, base, true);
  ctx.r3.s64 = d;
  return true;
}

// strrchr (sub_8234E890; r3 = s, r4 = c): c == 0 (as a 32-bit value) returns
// the terminator's address; else the last byte equal to c as a signed 32-bit
// value (so c outside 1..255 is never found), 0 if none. A found address is
// r3's low half (lbzu updates only that; the high half stays).
bool NativeStrrchr(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32;
  const int32_t c = ctx.r4.s32;
  uint64_t len = 0;
  bool found = false;
  if (!ScanNul(base, s, 0x100000000ull, &len, &found) || !found) return false;
  if (c == 0) {
    ctx.r3.u32 = uint32_t(s + len);
    return true;
  }
  if (c >= 1 && c <= 255) {
    const uint8_t* p = Host(base, s);
    for (uint64_t i = len; i-- > 0;) {
      if (p[i] == uint8_t(c)) {
        ctx.r3.u32 = uint32_t(s + i);
        return true;
      }
    }
  }
  ctx.r3.s64 = 0;
  return true;
}

// strncat (sub_8234E600; r3 = dst, r4 = src, r5 = n, low 32 bits unsigned):
// finds dst's terminator, copies src's bytes through its NUL but at most n,
// and a 0 after n bytes; r3 as passed. A source that overlaps what is written
// goes to the guest code (its byte order is its own).
bool NativeStrncat(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  uint64_t dlen = 0;
  bool found = false;
  if (!ScanNul(base, dst, 0x100000000ull, &dlen, &found) || !found) return false;
  const uint32_t at = uint32_t(dst + dlen);  // the terminator (inside dst's plain run)
  uint64_t copy = 0, read = 0;
  if (n != 0) {
    uint64_t slen = 0;
    bool sfound = false;
    if (!ScanNul(base, src, n, &slen, &sfound)) return false;
    copy = sfound ? slen : uint64_t(n);      // bytes of src before the 0
    read = sfound ? slen + 1 : uint64_t(n);  // bytes of src the code reads
  }
  const uint64_t written = copy + 1;
  if (written > 0xFFFFFFFFull || !Plain(at, uint32_t(written))) return false;
  if (read != 0 && Overlap2(at, written, src, read)) return false;
  uint8_t* d = Host(base, at);
  if (copy) std::memcpy(d, Host(base, src), size_t(copy));
  d[copy] = 0;
  return true;
}

// memchr (sub_8234E650; r3 = s, r4 = c, r5 = n, low 32 bits unsigned): the
// first of n bytes equal to c as a signed 32-bit value; r3's low half becomes
// its address (r3 as passed for the first byte), 0 if none or n == 0.
bool NativeMemchr(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32, n = ctx.r5.u32;
  const int32_t c = ctx.r4.s32;
  if (n == 0) {
    ctx.r3.s64 = 0;
    return true;
  }
  const uint64_t run = PlainRun(s);
  const uint64_t span = run < n ? run : uint64_t(n);
  if (c >= 0 && c <= 255 && span != 0) {
    const uint8_t* p = Host(base, s);
    const void* z = std::memchr(p, c, size_t(span));
    if (z) {
      ctx.r3.u32 = uint32_t(s + uint64_t(static_cast<const uint8_t*>(z) - p));
      return true;
    }
  }
  if (span < n) return false;  // the guest code would read past plain memory
  ctx.r3.s64 = 0;
  return true;
}

// strcspn (sub_8234E8E8; r3 = s, r4 = reject): a 32-byte bitmap at r1-32
// (cleared, a bit per reject byte, then bit 0 for the NUL); r3 is the index
// of s's first byte in it. The bitmap is left below r1 as the code leaves it.
bool NativeStrcspn(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32, rej = ctx.r4.u32, sp = ctx.r1.u32;
  if (sp < 32 || !Plain(sp - 32, 32)) return false;
  uint64_t rlen = 0;
  bool found = false;
  if (!ScanNul(base, rej, 0x100000000ull, &rlen, &found) || !found) return false;
  if (Overlap2(sp - 32, 32, rej, rlen + 1)) return false;
  uint8_t map[32] = {};
  const uint8_t* r = Host(base, rej);
  for (uint64_t i = 0; i < rlen; ++i) map[r[i] >> 3] = uint8_t(map[r[i] >> 3] | (1u << (r[i] & 7)));
  map[0] = uint8_t(map[0] | 1);
  const uint64_t run = PlainRun(s);
  const uint8_t* p = Host(base, s);
  uint64_t i = 0;
  for (;; ++i) {
    if (i >= run) return false;
    if (map[p[i] >> 3] & (1u << (p[i] & 7))) break;
  }
  if (Overlap2(sp - 32, 32, s, i + 1)) return false;
  std::memcpy(Host(base, sp - 32), map, 32);
  ctx.r3.u64 = i;
  return true;
}

// _strlwr (sub_82353528) / _strupr (sub_82353728; r3 = s): a null s sets
// errno and calls the invalid parameter handler: the guest code. Else every
// byte up to the NUL in 'A'..'Z' ('a'..'z') as a signed byte gets 32 added
// (taken away), in place; r3 as passed.
template <bool kLower>
bool NativeStrcase(PPCContext& ctx, uint8_t* base) {
  const uint32_t s = ctx.r3.u32, sp = ctx.r1.u32;
  if (s == 0 || !FramePlain(sp)) return false;
  uint64_t len = 0;
  bool found = false;
  if (!ScanNul(base, s, 0x100000000ull, &len, &found) || !found) return false;
  if (Overlap2(sp - kFrame, kFrame, s, len + 1)) return false;
  WriteFrame(ctx, base, false);
  uint8_t* p = Host(base, s);
  for (uint64_t i = 0; i < len; ++i) {
    if (kLower) {
      if (p[i] >= 'A' && p[i] <= 'Z') p[i] = uint8_t(p[i] + 32);
    } else {
      if (p[i] >= 'a' && p[i] <= 'z') p[i] = uint8_t(p[i] - 32);
    }
  }
  return true;
}
bool NativeStrlwr(PPCContext& ctx, uint8_t* base) { return NativeStrcase<true>(ctx, base); }
bool NativeStrupr(PPCContext& ctx, uint8_t* base) { return NativeStrcase<false>(ctx, base); }

enum Fn : int {
  kMemcpy, kMemset, kMemmove, kXMemCpy,
  kStricmp, kStrncmp, kStrcpy, kStrncpy, kStrchr, kStrstr,
  // 2.12.7 (after the others: their checks' random numbers stay the same)
  kStrnicmp, kStrrchr, kStrncat, kMemchr, kStrcspn, kStrlwr, kStrupr,
  kFnCount
};
constexpr const char* kFnName[kFnCount] = {"memcpy",  "memset",  "memmove", "XMemCpy",  "_stricmp",
                                           "strncmp", "strcpy",  "strncpy", "strchr",   "strstr",
                                           "_strnicmp", "strrchr", "strncat", "memchr", "strcspn",
                                           "_strlwr", "_strupr"};
constexpr bool IsString(int f) { return f >= kStricmp; }

// ---------------------------------------------------------------------------
// Counters (per thread, so the hot path has no shared writes)
// ---------------------------------------------------------------------------

struct Counters {
  std::atomic<uint64_t> native[kFnCount]{};
  std::atomic<uint64_t> bytes[kFnCount]{};
  std::atomic<uint64_t> guest[kFnCount]{};  // left to the guest code by the range rules
  uint32_t tick = 0;                         // this thread only
};

std::mutex g_counters_mutex;
std::vector<Counters*> g_counters;  // one per thread that ever called; never freed

Counters& Mine() {
  thread_local Counters* t = nullptr;
  if (!t) {
    t = new Counters();
    std::lock_guard lock(g_counters_mutex);
    g_counters.push_back(t);
  }
  return *t;
}

// Only the owning thread writes its counters.
inline void Bump(std::atomic<uint64_t>& c, uint64_t by) {
  c.store(c.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kFirstReportNs = int64_t(60) * 1000000000;      // a minute after the check
constexpr int64_t kReportEveryNs = int64_t(600) * 1000000000;     // then every 10 minutes
std::atomic<int64_t> g_next_report_ns{0};                         // 0: none planned

void Report() {
  uint64_t native[kFnCount] = {}, bytes[kFnCount] = {}, guest[kFnCount] = {};
  {
    std::lock_guard lock(g_counters_mutex);
    for (const Counters* c : g_counters) {
      for (int f = 0; f < kFnCount; ++f) {
        native[f] += c->native[f].load(std::memory_order_relaxed);
        bytes[f] += c->bytes[f].load(std::memory_order_relaxed);
        guest[f] += c->guest[f].load(std::memory_order_relaxed);
      }
    }
  }
  std::string line;
  for (int f = 0; f < kFnCount; ++f) {
    char part[160];
    if (IsString(f)) {
      std::snprintf(part, sizeof(part), "; %s %llu native, %llu by the game's code", kFnName[f],
                    static_cast<unsigned long long>(native[f]),
                    static_cast<unsigned long long>(guest[f]));
    } else {
      std::snprintf(part, sizeof(part), "%s%s %llu native (%.1f MB), %llu by the game's code",
                    f ? "; " : "", kFnName[f], static_cast<unsigned long long>(native[f]),
                    double(bytes[f]) / 1e6, static_cast<unsigned long long>(guest[f]));
    }
    line += part;
  }
  REXLOG_INFO("cod3: native C runtime calls so far: {}", line);
}

void MaybeReport() {
  int64_t next = g_next_report_ns.load(std::memory_order_relaxed);
  if (next == 0) return;
  const int64_t now = NowNs();
  if (now < next) return;
  if (!g_next_report_ns.compare_exchange_strong(next, now + kReportEveryNs,
                                                std::memory_order_relaxed))
    return;  // another thread reports
  Report();
}

// [claude-9b-think] UNTESTED: cod3_native_crt and cod3_native_crt_counts as last set (WatchSettings, from
// the start check on), so the hooks read an atomic instead of a setting on every call.
std::atomic<bool> g_native_on{true};
std::atomic<bool> g_counting{false};
std::once_flag g_watch_once;

void WatchSettings() {
  g_native_on.store(REXCVAR_GET(cod3_native_crt), std::memory_order_relaxed);
  g_counting.store(REXCVAR_GET(cod3_native_crt_counts), std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("cod3_native_crt", [](std::string_view, std::string_view) {
    g_native_on.store(REXCVAR_GET(cod3_native_crt), std::memory_order_relaxed);
  });
  rex::cvar::RegisterChangeCallback("cod3_native_crt_counts", [](std::string_view, std::string_view) {
    g_counting.store(REXCVAR_GET(cod3_native_crt_counts), std::memory_order_relaxed);
  });
}

inline void CountNative(Fn f, uint32_t n) {
  if (!g_counting.load(std::memory_order_relaxed)) return;  // [claude-9b-think]
  Counters& c = Mine();
  Bump(c.native[f], 1);
  Bump(c.bytes[f], n);
  if ((++c.tick & 0x3FFF) == 0) MaybeReport();
}

inline void CountGuest(Fn f) {
  if (g_counting.load(std::memory_order_relaxed)) Bump(Mine().guest[f], 1);  // [claude-9b-think]
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------

// 0: not checked yet, 1: a thread is checking now, 2: checked.
std::atomic<int> g_check_state{0};
std::atomic<bool> g_fn_ok[kFnCount]{};

constexpr uint32_t kBlock = 64 * 1024;  // each scratch block
constexpr uint32_t kEdge = 512;         // no call starts in the first or last 512 bytes
constexpr uint32_t kMaxLength = 20000;

struct Block {
  uint32_t alloc = 0;  // what SystemHeapAlloc returned
  uint32_t guest = 0;  // 4 KB aligned start
  uint8_t* host = nullptr;
};

struct Rng {
  uint64_t s;
  uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint32_t Below(uint32_t n) { return n ? uint32_t(Next() % n) : 0; }  // 0 .. n-1
};

uint32_t Length(Rng& rng) {
  const uint32_t r = rng.Below(100);
  if (r < 45) return rng.Below(65);
  if (r < 75) return rng.Below(600);
  if (r < 93) return rng.Below(5000);
  return rng.Below(kMaxLength + 1);
}

// One call in eight gets junk in a register's high half.
uint64_t Junk(Rng& rng) {
  return rng.Below(8) == 0 ? (rng.Next() & 0xFFFFFFFF00000000ull) : 0;
}

void Fill(const Block& a, const Block& b, Rng& rng) {
  for (uint32_t i = 0; i < kBlock; i += 8) {
    const uint64_t v = rng.Next();
    std::memcpy(a.host + i, &v, 8);
  }
  std::memcpy(b.host, a.host, kBlock);
}

// Runs one call on a copy of the caller's registers (r1 moved below the
// caller's red zone, as guest::Call does, or to `r1` when it isn't 0) with
// r3..r5 set; returns r3.
// native: the hook's path (native where the rules allow, else the original).
uint64_t RunOne(Fn f, bool native, PPCContext& parent, uint8_t* base, uint64_t r3, uint64_t r4,
                uint64_t r5, bool* ran_native, uint32_t r1 = 0) {
  rex::CallFrame frame(parent);
  PPCContext& c = frame.ctx;
  c.r1.u32 = r1 ? r1 : ((parent.r1.u32 - 0x200) & ~0xFu);
  c.r3.u64 = r3;
  c.r4.u64 = r4;
  c.r5.u64 = r5;
  bool done = false;
  switch (f) {
    case kMemcpy:
      done = native && NativeMemcpy(c, base);
      if (!done) __imp__memcpy_guest(c, base);
      break;
    case kMemset:
      done = native && NativeMemset(c, base);
      if (!done) __imp__memset_guest(c, base);
      break;
    case kMemmove:
      done = native && NativeMemmove(c, base);
      if (!done) __imp__memmove_guest(c, base);
      break;
    case kXMemCpy:
      done = native && NativeXMemCpy(c, base);
      if (!done) __imp__sub_82343548(c, base);
      break;
    case kStricmp:
      done = native && NativeStricmp(c, base);
      if (!done) __imp___stricmp_guest(c, base);
      break;
    case kStrncmp:
      done = native && NativeStrncmp(c, base);
      if (!done) __imp__strncmp_guest(c, base);
      break;
    case kStrcpy:
      done = native && NativeStrcpy(c, base);
      if (!done) __imp__strcpy_guest(c, base);
      break;
    case kStrncpy:
      done = native && NativeStrncpy(c, base);
      if (!done) __imp__strncpy_guest(c, base);
      break;
    case kStrchr:
      done = native && NativeStrchr(c, base);
      if (!done) __imp__strchr_guest(c, base);
      break;
    case kStrstr:
      done = native && NativeStrstr(c, base);
      if (!done) __imp__strstr_guest(c, base);
      break;
    case kStrnicmp:
      done = native && NativeStrnicmp(c, base);
      if (!done) __imp__sub_82353618(c, base);
      break;
    case kStrrchr:
      done = native && NativeStrrchr(c, base);
      if (!done) __imp__sub_8234E890(c, base);
      break;
    case kStrncat:
      done = native && NativeStrncat(c, base);
      if (!done) __imp__sub_8234E600(c, base);
      break;
    case kMemchr:
      done = native && NativeMemchr(c, base);
      if (!done) __imp__sub_8234E650(c, base);
      break;
    case kStrcspn:
      done = native && NativeStrcspn(c, base);
      if (!done) __imp__sub_8234E8E8(c, base);
      break;
    case kStrlwr:
      done = native && NativeStrlwr(c, base);
      if (!done) __imp__sub_82353528(c, base);
      break;
    case kStrupr:
      done = native && NativeStrupr(c, base);
      if (!done) __imp__sub_82353728(c, base);
      break;
    default:
      break;
  }
  if (ran_native) *ran_native = done;
  return c.r3.u64;
}

// Runs `cases` random calls of f: the original on block a, the hook's path on
// block b (same offsets, same bytes), and compares r3 and both whole blocks.
bool CheckFn(Fn f, int cases, PPCContext& ctx, uint8_t* base, const Block& a, const Block& b,
             Rng& rng, int* native_cases, std::string* why) {
  for (int i = 0; i < cases; ++i) {
    if (i % 200 == 0) Fill(a, b, rng);
    const uint32_t n = Length(rng);
    const uint32_t span = kBlock - 2 * kEdge - n;  // start offsets kEdge .. kEdge + span
    const uint32_t d = kEdge + rng.Below(span + 1);
    uint32_t s = kEdge + rng.Below(span + 1);
    if ((f == kMemmove || f == kMemcpy || f == kXMemCpy) && rng.Below(4) == 0) {
      // Overlapping ranges (memmove's own job; memcpy and XMemCpy must go to
      // the guest code for these).
      int64_t s2 = int64_t(d) + int64_t(rng.Below(2 * n + 1)) - int64_t(n);
      if (s2 < int64_t(kEdge)) s2 = kEdge;
      if (s2 > int64_t(kEdge + span)) s2 = kEdge + span;
      s = uint32_t(s2);
    }
    const uint64_t hi3 = Junk(rng), hi4 = Junk(rng), hi5 = Junk(rng);
    uint64_t r4a, r4b;
    if (f == kMemset) {
      r4a = r4b = rng.Next();  // the value: only its low byte counts
    } else {
      r4a = hi4 | (a.guest + s);
      r4b = hi4 | (b.guest + s);
    }
    const uint64_t r5 = hi5 | n;
    const uint64_t ra = RunOne(f, false, ctx, base, hi3 | (a.guest + d), r4a, r5, nullptr);
    bool ran_native = false;
    const uint64_t rb = RunOne(f, true, ctx, base, hi3 | (b.guest + d), r4b, r5, &ran_native);
    if (ran_native) ++*native_cases;
    char buf[200];
    if (ra - a.guest != rb - b.guest) {
      std::snprintf(buf, sizeof(buf),
                    "case %d: n=%u dst=+%u src=+%u: r3 0x%016llx, native 0x%016llx (relative)", i,
                    n, d, s, static_cast<unsigned long long>(ra - a.guest),
                    static_cast<unsigned long long>(rb - b.guest));
      *why = buf;
      return false;
    }
    if (std::memcmp(a.host, b.host, kBlock) != 0) {
      uint32_t at = 0;
      while (at < kBlock && a.host[at] == b.host[at]) ++at;
      std::snprintf(buf, sizeof(buf),
                    "case %d: n=%u dst=+%u src=+%u: byte +%u is 0x%02x, native 0x%02x", i, n, d, s,
                    at, a.host[at], b.host[at]);
      *why = buf;
      return false;
    }
  }
  return true;
}

// Text for the string functions: letters of both cases, the neighbors of
// A-Z and a-z ('@' '[' '`' '{'), digits, punctuation, control and high bytes,
// with a NUL every `nul_every` bytes on average; the last kEdge bytes are 0, so
// every string ends inside the block.
void FillText(const Block& a, const Block& b, Rng& rng, uint32_t nul_every) {
  static constexpr char kAlpha[] = "abcxyzABCXYZ@[`{_-./09 ";
  for (uint32_t i = 0; i < kBlock; ++i) {
    const uint64_t r = rng.Next();
    uint8_t c;
    if (r % nul_every == 0) {
      c = 0;
    } else {
      switch ((r >> 20) % 8) {
        case 5: c = uint8_t('A' + (r >> 32) % 26); break;
        case 6: c = uint8_t('a' + (r >> 32) % 26); break;
        case 7: c = uint8_t(((r >> 32) & 1) ? 0x80 + ((r >> 40) & 0x7F) : 1 + ((r >> 40) % 31)); break;
        default: c = uint8_t(kAlpha[(r >> 32) % (sizeof(kAlpha) - 1)]); break;
      }
    }
    a.host[i] = c;
  }
  std::memset(a.host + kBlock - kEdge, 0, kEdge);
  std::memcpy(b.host, a.host, kBlock);
}

uint32_t TextLength(const Block& a, uint32_t off) {
  const void* z = std::memchr(a.host + off, 0, kBlock - off);
  return z ? uint32_t(static_cast<const uint8_t*>(z) - (a.host + off)) : kBlock - off;
}

// Writes a copy of the string at `from` to `to` in block a (letters' case
// flipped now and then, sometimes cut short or one byte changed).
void WriteVariant(const Block& a, Rng& rng, uint32_t from, uint32_t to, uint32_t len) {
  std::string t(reinterpret_cast<const char*>(a.host + from), len);
  for (char& ch : t) {
    if (rng.Below(3) == 0 && std::isalpha(static_cast<unsigned char>(ch))) ch ^= 0x20;
  }
  switch (rng.Below(6)) {
    case 0: if (!t.empty()) t.resize(rng.Below(uint32_t(t.size()))); break;
    case 1: if (!t.empty()) t[rng.Below(uint32_t(t.size()))] = char(1 + rng.Below(255)); break;
    case 2: t.push_back(char('a' + rng.Below(26))); break;
    default: break;
  }
  std::memcpy(a.host + to, t.data(), t.size());
  a.host[to + t.size()] = 0;
}

// The string functions' check: `cases` random calls, the original on block a
// and the hook's path on block b (same text, same offsets); compares r3 (as
// an address relative to the block where it is one) and both whole blocks.
bool CheckStrFn(Fn f, int cases, PPCContext& ctx, uint8_t* base, const Block& a,
                const Block& b, Rng& rng, int* native_cases, std::string* why) {
  constexpr uint32_t kNulEvery[] = {6, 40, 400};
  const uint32_t lo = kEdge, hi = kBlock - 2 * kEdge;  // start offsets lo .. hi
  for (int i = 0; i < cases; ++i) {
    if (i % 100 == 0) FillText(a, b, rng, kNulEvery[(i / 100) % 3]);
    uint32_t s1 = lo + rng.Below(hi - lo);
    uint32_t s2 = lo + rng.Below(hi - lo);
    uint32_t len1 = TextLength(a, s1);
    if (len1 > 2000) {  // keep copies of it inside the block
      a.host[s1 + 2000] = 0;
      len1 = 2000;
    }
    uint64_t r5 = 0;
    uint64_t r4_value = 0;  // r4 when it is not an address
    bool r4_is_value = false;
    switch (f) {
      case kStricmp:
      case kStrncmp:
        if (rng.Below(2)) {
          s2 = lo + rng.Below(hi - lo - len1 - 2);
          if (Overlap2(s2, len1 + 2, s1, len1 + 1)) s2 = s1;  // the same string twice
          if (s2 != s1) WriteVariant(a, rng, s1, s2, len1);
        }
        if (f == kStrncmp) {
          switch (rng.Below(6)) {
            case 0: r5 = rng.Below(10); break;
            case 1: r5 = rng.Below(300); break;
            case 2: r5 = len1 + rng.Below(3); break;
            case 3: r5 = uint32_t(rng.Next()); break;  // large or negative
            case 4: r5 = 0; break;
            default: r5 = len1 + 1; break;
          }
          r5 |= Junk(rng);
        }
        break;
      case kStrchr: {
        r4_is_value = true;
        switch (rng.Below(6)) {
          case 0:
          case 1: r4_value = len1 ? a.host[s1 + rng.Below(len1)] : 'a'; break;
          case 2: r4_value = 0; break;
          case 3: r4_value = uint64_t(int64_t(int8_t(a.host[s1 + (len1 ? rng.Below(len1) : 0)]))); break;
          case 4: r4_value = (len1 ? a.host[s1 + rng.Below(len1)] : 'a') + 256u; break;
          default: r4_value = rng.Next(); break;
        }
        r4_value = (r4_value & 0xFFFFFFFFull) | Junk(rng);
        break;
      }
      case kStrstr: {
        const uint32_t pick = rng.Below(4);
        s2 = lo + rng.Below(hi - lo - 40);
        if (pick <= 1 && len1 > 0) {  // a piece of the haystack
          const uint32_t at = rng.Below(len1);
          uint32_t n = 1 + rng.Below(std::min<uint32_t>(len1 - at, 24));
          if (Overlap2(s2, n + 1, s1, len1 + 1)) break;
          std::memmove(a.host + s2, a.host + s1 + at, n);
          if (pick == 1) a.host[s2 + n - 1] ^= 0x20;  // nearly the same
          a.host[s2 + n] = 0;
        } else if (pick == 2) {  // empty or one byte
          if (Overlap2(s2, 2, s1, len1 + 1)) break;
          a.host[s2] = rng.Below(2) ? 0 : (len1 ? a.host[s1] : 'a');
          a.host[s2 + 1] = 0;
        }
        break;
      }
      case kStrcpy: {
        s2 = lo + rng.Below(hi - lo - len1 - 1);
        if (Overlap2(s2, len1 + 1, s1, len1 + 1)) continue;  // overlapping strcpy: undefined, may never end
        break;
      }
      case kStrncpy: {
        uint32_t n;
        switch (rng.Below(4)) {
          case 0: n = rng.Below(len1 + 3); break;
          case 1: n = len1 + rng.Below(40); break;
          case 2: n = rng.Below(4); break;
          default: n = len1 + 1; break;
        }
        s2 = lo + rng.Below(hi - lo - n);
        r5 = n | Junk(rng);
        break;
      }
      default:
        break;
    }
    std::memcpy(b.host, a.host, kBlock);
    // r3/r4: dst/src or s1/s2 as each function names them.
    const bool copy = (f == kStrcpy || f == kStrncpy);
    const uint32_t o3 = copy ? s2 : s1, o4 = copy ? s1 : s2;
    const uint64_t hi3 = Junk(rng), hi4 = Junk(rng);
    const uint64_t r4a = r4_is_value ? r4_value : (hi4 | (a.guest + o4));
    const uint64_t r4b = r4_is_value ? r4_value : (hi4 | (b.guest + o4));
    const uint64_t ra = RunOne(f, false, ctx, base, hi3 | (a.guest + o3), r4a, r5, nullptr);
    bool ran_native = false;
    const uint64_t rb = RunOne(f, true, ctx, base, hi3 | (b.guest + o3), r4b, r5, &ran_native);
    if (ran_native) ++*native_cases;
    bool same_r3;
    if (f == kStricmp || f == kStrncmp) {
      same_r3 = ra == rb;
    } else if (f == kStrchr || f == kStrstr) {
      same_r3 = (ra == 0 && rb == 0) || (ra != 0 && rb != 0 && ra - a.guest == rb - b.guest);
    } else {
      same_r3 = ra - a.guest == rb - b.guest;
    }
    char buf[220];
    if (!same_r3) {
      std::snprintf(buf, sizeof(buf),
                    "case %d: r3=+%u r4=%s%llx r5=0x%llx: r3 0x%016llx, native 0x%016llx", i, o3,
                    r4_is_value ? "" : "+", static_cast<unsigned long long>(r4_is_value ? r4_value : o4),
                    static_cast<unsigned long long>(r5), static_cast<unsigned long long>(ra),
                    static_cast<unsigned long long>(rb));
      *why = buf;
      return false;
    }
    if (std::memcmp(a.host, b.host, kBlock) != 0) {
      uint32_t at = 0;
      while (at < kBlock && a.host[at] == b.host[at]) ++at;
      std::snprintf(buf, sizeof(buf), "case %d: r3=+%u r4=+%u r5=0x%llx: byte +%u is 0x%02x, native 0x%02x",
                    i, o3, o4, static_cast<unsigned long long>(r5), at, a.host[at], b.host[at]);
      *why = buf;
      return false;
    }
  }
  return true;
}

// 2.12.7's functions' check, as CheckStrFn's. r1 is put inside the blocks
// (kStackOff, in their zero tail) so the frames and strcspn's bitmap are
// compared with everything else; now and then a string is put where those go,
// which the native versions must leave to the guest code. Never a null
// pointer or a count above INT_MAX for _strnicmp, _strlwr and _strupr: that
// is the invalid parameter handler's business.
constexpr uint32_t kStackOff = kBlock - 64;

// A few letters and a NUL at off (inside the frame or bitmap area).
void PutWord(const Block& a, Rng& rng, uint32_t off) {
  const uint32_t n = 1 + rng.Below(6);
  for (uint32_t k = 0; k < n; ++k) a.host[off + k] = uint8_t((rng.Below(2) ? 'A' : 'a') + rng.Below(26));
  a.host[off + n] = 0;
}

bool CheckStrFn2(Fn f, int cases, PPCContext& ctx, uint8_t* base, const Block& a,
                 const Block& b, Rng& rng, int* native_cases, std::string* why) {
  constexpr uint32_t kNulEvery[] = {6, 40, 400};
  const uint32_t lo = kEdge, hi = kBlock - 2 * kEdge;  // start offsets lo .. hi
  for (int i = 0; i < cases; ++i) {
    if (i % 100 == 0) FillText(a, b, rng, kNulEvery[(i / 100) % 3]);
    std::memset(a.host + kBlock - kEdge, 0, kEdge);  // the tail: the stack area again
    uint32_t s1 = lo + rng.Below(hi - lo);
    uint32_t s2 = lo + rng.Below(hi - lo);
    uint32_t len1 = TextLength(a, s1);
    if (len1 > 2000) {
      a.host[s1 + 2000] = 0;
      len1 = 2000;
    }
    const bool in_stack = rng.Below(25) == 0;  // a string where the frame or bitmap goes
    uint64_t r5 = 0;
    uint64_t r4_value = 0;
    bool r4_is_value = false;
    uint32_t o3 = s1, o4 = s2;
    switch (f) {
      case kStrnicmp: {
        if (rng.Below(2)) {
          s2 = lo + rng.Below(hi - lo - len1 - 2);
          if (Overlap2(s2, len1 + 2, s1, len1 + 1)) s2 = s1;
          if (s2 != s1) WriteVariant(a, rng, s1, s2, len1);
        }
        switch (rng.Below(6)) {
          case 0: r5 = rng.Below(10); break;
          case 1: r5 = rng.Below(300); break;
          case 2: r5 = len1 + rng.Below(3); break;
          case 3: r5 = rng.Below(0x80000000u); break;  // up to INT_MAX
          case 4: r5 = 0; break;
          default: r5 = len1 + 1; break;
        }
        r5 |= Junk(rng);
        o3 = s1;
        o4 = s2;
        if (in_stack) {
          o3 = kStackOff - 90 + rng.Below(76);  // its 7 bytes inside the frame, past the back chain
          PutWord(a, rng, o3);
        }
        break;
      }
      case kStrrchr:
      case kMemchr: {
        r4_is_value = true;
        switch (rng.Below(6)) {
          case 0:
          case 1: r4_value = len1 ? a.host[s1 + rng.Below(len1)] : 'a'; break;
          case 2: r4_value = 0; break;
          case 3: r4_value = uint64_t(int64_t(int8_t(a.host[s1 + (len1 ? rng.Below(len1) : 0)]))); break;
          case 4: r4_value = (len1 ? a.host[s1 + rng.Below(len1)] : 'a') + 256u; break;
          default: r4_value = rng.Next(); break;
        }
        r4_value = (r4_value & 0xFFFFFFFFull) | Junk(rng);
        if (f == kMemchr) {
          const uint32_t room = kStackOff - s1;  // never past the block
          switch (rng.Below(6)) {
            case 0: r5 = rng.Below(3); break;
            case 1: r5 = rng.Below(64); break;
            case 2: r5 = len1 + rng.Below(3); break;
            case 3: r5 = rng.Below(room + 1); break;
            default: r5 = rng.Below(len1 + 1); break;
          }
          if (r5 > room) r5 = room;
          r5 |= Junk(rng);
        }
        break;
      }
      case kStrncat: {
        // dst at s2 (its own text, cut to 300), src at s1; what is written stays
        // well inside the block.
        s2 = lo + rng.Below(hi - lo - 6000);
        uint32_t dlen = TextLength(a, s2);
        if (dlen > 300) {
          a.host[s2 + 300] = 0;
          dlen = 300;
        }
        const bool overlap = rng.Below(8) == 0;
        if (overlap) s1 = s2 + rng.Below(dlen + 2);  // src inside or just after dst
        len1 = TextLength(a, s1);
        if (len1 > 2000) {
          a.host[s1 + 2000] = 0;
          len1 = 2000;
        }
        switch (rng.Below(6)) {
          case 0: r5 = rng.Below(10); break;
          case 1: r5 = len1 + rng.Below(3); break;
          case 2: r5 = rng.Below(300); break;
          case 3: r5 = len1 + 1; break;
          case 4: r5 = 0; break;
          default: r5 = rng.Below(3001); break;
        }
        // A source that runs into what is written never meets its NUL: keep n
        // small there so the guest code stops inside the block.
        if (overlap && r5 > 64) r5 = rng.Below(65);
        if (!overlap && Overlap2(s2, dlen + 3002, s1, len1 + 1)) r5 = std::min<uint64_t>(r5, 64);
        r5 |= Junk(rng);
        o3 = s2;
        o4 = s1;
        break;
      }
      case kStrcspn: {
        // reject at s2: up to 8 bytes, some from s's text.
        s2 = lo + rng.Below(hi - lo - 16);
        const uint32_t n = rng.Below(9);
        for (uint32_t k = 0; k < n; ++k) {
          a.host[s2 + k] = (rng.Below(2) && len1) ? a.host[s1 + rng.Below(len1)]
                                                  : uint8_t(1 + rng.Below(255));
        }
        a.host[s2 + n] = 0;
        o3 = s1;
        o4 = s2;
        if (in_stack) {
          const uint32_t at = kStackOff - 8 - rng.Below(32 - 7);  // inside the bitmap
          PutWord(a, rng, at);
          if (rng.Below(2)) o4 = at; else o3 = at;
        }
        break;
      }
      case kStrlwr:
      case kStrupr:
        o3 = s1;
        if (in_stack) {
          o3 = kStackOff - 90 + rng.Below(76);
          PutWord(a, rng, o3);
        }
        break;
      default:
        break;
    }
    std::memcpy(b.host, a.host, kBlock);
    const uint64_t hi3 = Junk(rng), hi4 = Junk(rng);
    const uint64_t r4a = r4_is_value ? r4_value : (hi4 | (a.guest + o4));
    const uint64_t r4b = r4_is_value ? r4_value : (hi4 | (b.guest + o4));
    const uint64_t ra = RunOne(f, false, ctx, base, hi3 | (a.guest + o3), r4a, r5, nullptr,
                               a.guest + kStackOff);
    bool ran_native = false;
    const uint64_t rb = RunOne(f, true, ctx, base, hi3 | (b.guest + o3), r4b, r5, &ran_native,
                               b.guest + kStackOff);
    if (ran_native) ++*native_cases;
    char buf[220];
    if (f == kStrnicmp || f == kStrlwr || f == kStrupr) {
      // The frame's back chain is r1 itself, which differs between the blocks:
      // compared relative to each block, then left out of the byte compare.
      const uint32_t at = kStackOff - kFrame;
      const uint32_t wa = LdBE32(a.host + at), wb = LdBE32(b.host + at);
      if (wa - a.guest != wb - b.guest) {
        std::snprintf(buf, sizeof(buf), "case %d: r3=+%u: back chain +0x%x, native +0x%x", i, o3,
                      wa - a.guest, wb - b.guest);
        *why = buf;
        return false;
      }
      std::memset(a.host + at, 0, 4);
      std::memset(b.host + at, 0, 4);
    }
    bool same_r3;
    if (f == kStrnicmp || f == kStrcspn) {
      same_r3 = ra == rb;
    } else if (f == kStrrchr || f == kMemchr) {
      same_r3 = (ra == 0 && rb == 0) || (ra != 0 && rb != 0 && ra - a.guest == rb - b.guest);
    } else {
      same_r3 = ra - a.guest == rb - b.guest;
    }
    if (!same_r3) {
      std::snprintf(buf, sizeof(buf),
                    "case %d: r3=+%u r4=%s%llx r5=0x%llx: r3 0x%016llx, native 0x%016llx", i, o3,
                    r4_is_value ? "" : "+", static_cast<unsigned long long>(r4_is_value ? r4_value : o4),
                    static_cast<unsigned long long>(r5), static_cast<unsigned long long>(ra),
                    static_cast<unsigned long long>(rb));
      *why = buf;
      return false;
    }
    if (std::memcmp(a.host, b.host, kBlock) != 0) {
      uint32_t at = 0;
      while (at < kBlock && a.host[at] == b.host[at]) ++at;
      std::snprintf(buf, sizeof(buf), "case %d: r3=+%u r4=+%u r5=0x%llx: byte +%u is 0x%02x, native 0x%02x",
                    i, o3, o4, static_cast<unsigned long long>(r5), at, a.host[at], b.host[at]);
      *why = buf;
      return false;
    }
  }
  return true;
}

bool AllocBlock(rex::memory::Memory* mem, uint8_t* base, Block* b) {
  b->alloc = mem->SystemHeapAlloc(kBlock + 0x1000);
  if (!b->alloc) return false;
  b->guest = (b->alloc + 0xFFFu) & ~0xFFFu;
  b->host = Host(base, b->guest);
  return true;
}

// Runs the whole check. Returns false when the memory isn't up yet (try again
// on a later call), true when it ran (or can't ever run; then all stay off).
bool RunSelfCheck(PPCContext& ctx, uint8_t* base) {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) return false;
  auto* mem = ks->memory();
  if (mem->virtual_membase() != base) {
    REXLOG_WARN("cod3: native C runtime off: guest memory isn't where the game's code expects it");
    return true;
  }
  const uint32_t probe = kHighView + 0x10000;
  g_high_view_ok.store(mem->TranslateVirtual<uint8_t*>(probe) == base + probe + kHighViewOffset,
                       std::memory_order_relaxed);

  Block a, b;
  if (!AllocBlock(mem, base, &a) || !AllocBlock(mem, base, &b)) {
    if (a.alloc) mem->SystemHeapFree(a.alloc);
    if (b.alloc) mem->SystemHeapFree(b.alloc);
    REXLOG_WARN("cod3: native C runtime off: no scratch memory for its self-check");
    return true;
  }

  const int64_t start = NowNs();
  // 2.12.7's seven run fewer cases (each copies and compares the whole blocks),
  // so the check stays near its old 55 ms on the game's first call.
  constexpr int kCases[kFnCount] = {2500, 1500, 2500, 2500, 1500, 1500, 1000, 1000, 1500, 1500,
                                    500,  400,  500,  400,  400,  300,  300};
  Rng rng{0x9E3779B97F4A7C15ull};
  std::string summary;
  int on = 0;
  for (int f = 0; f < kFnCount; ++f) {
    int native_cases = 0;
    std::string why;
    const bool same =
        f >= kStrnicmp ? CheckStrFn2(Fn(f), kCases[f], ctx, base, a, b, rng, &native_cases, &why)
        : IsString(f)  ? CheckStrFn(Fn(f), kCases[f], ctx, base, a, b, rng, &native_cases, &why)
                       : CheckFn(Fn(f), kCases[f], ctx, base, a, b, rng, &native_cases, &why);
    const bool ok = same && native_cases > 0;
    g_fn_ok[f].store(ok, std::memory_order_relaxed);
    if (!same) {
      REXLOG_ERROR("cod3: native C runtime: {} differs from the game's own code ({}); it stays "
                   "on the game's code",
                   kFnName[f], why);
    } else if (!ok) {
      REXLOG_WARN("cod3: native C runtime: {} couldn't be checked natively (scratch memory at "
                  "0x{:08X}); it stays on the game's code",
                  kFnName[f], b.guest);
    }
    char part[96];
    std::snprintf(part, sizeof(part), "%s%s %s (%d/%d native)", f ? ", " : "", kFnName[f],
                  ok ? "same" : "OFF", native_cases, kCases[f]);
    summary += part;
    on += ok ? 1 : 0;
  }
  mem->SystemHeapFree(a.alloc);
  mem->SystemHeapFree(b.alloc);

  const double ms = double(NowNs() - start) / 1e6;
  REXLOG_INFO("cod3: native C runtime self-check ({:.0f} ms, scratch at 0x{:08X}, 0xE0000000 view "
              "{}): {}; {} of {} now run as native code (cod3_native_crt)",
              ms, b.guest, g_high_view_ok.load(std::memory_order_relaxed) ? "native" : "game's code",
              summary, on, int(kFnCount));
  g_next_report_ns.store(NowNs() + kFirstReportNs, std::memory_order_relaxed);
  return true;
}

// Called by a hook until the check has run. True once it ran (on this or
// another thread); while another thread runs it, false (that call takes the
// game's code).
bool EnsureChecked(PPCContext& ctx, uint8_t* base) {
  int expected = 0;
  if (!g_check_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
    return expected == 2;
  const bool ran = RunSelfCheck(ctx, base);
  g_check_state.store(ran ? 2 : 0, std::memory_order_release);
  return ran;
}

inline bool Use(Fn f, PPCContext& ctx, uint8_t* base) {
  // [claude-9b-think] UNTESTED: until the start check has run, the setting itself (as before); from then on
  // the copy WatchSettings keeps.
  if (g_check_state.load(std::memory_order_acquire) != 2) {
    if (!REXCVAR_GET(cod3_native_crt)) return false;
    std::call_once(g_watch_once, WatchSettings);
    if (!EnsureChecked(ctx, base)) return false;
  }
  if (!g_native_on.load(std::memory_order_relaxed)) return false;
  return g_fn_ok[f].load(std::memory_order_relaxed);
}

}  // namespace

// ---------------------------------------------------------------------------
// The hooks: native where Use() and the function's own rules allow, else the
// original code.
// ---------------------------------------------------------------------------

REX_HOOK_RAW(memcpy_guest) {
  if (Use(kMemcpy, ctx, base)) {
    const uint32_t n = ctx.r5.u32;
    if (NativeMemcpy(ctx, base)) {
      CountNative(kMemcpy, n);
      return;
    }
    CountGuest(kMemcpy);
  }
  __imp__memcpy_guest(ctx, base);
}

REX_HOOK_RAW(memset_guest) {
  if (Use(kMemset, ctx, base)) {
    const uint32_t n = ctx.r5.u32;
    if (NativeMemset(ctx, base)) {
      CountNative(kMemset, n);
      return;
    }
    CountGuest(kMemset);
  }
  __imp__memset_guest(ctx, base);
}

REX_HOOK_RAW(memmove_guest) {
  if (Use(kMemmove, ctx, base)) {
    const uint32_t n = ctx.r5.u32;
    if (NativeMemmove(ctx, base)) {
      CountNative(kMemmove, n);
      return;
    }
    CountGuest(kMemmove);
  }
  __imp__memmove_guest(ctx, base);
}

// XMemCpy: the Xbox 360's own fast copy (VMX, cache line clears).
REX_HOOK_RAW(sub_82343548) {
  if (Use(kXMemCpy, ctx, base)) {
    const uint32_t n = ctx.r5.u32;
    if (NativeXMemCpy(ctx, base)) {
      CountNative(kXMemCpy, n);
      return;
    }
    CountGuest(kXMemCpy);
  }
  __imp__sub_82343548(ctx, base);
}

// The string functions.
// Each written out (not made by a macro) so tools/mp_aliases.py sees every
// hook's name for the multiplayer program.
namespace {
template <bool (*Native)(PPCContext&, uint8_t*)>
inline bool StringNative(Fn fn, PPCContext& ctx, uint8_t* base) {
  if (!Use(fn, ctx, base)) return false;
  if (Native(ctx, base)) {
    CountNative(fn, 0);
    return true;
  }
  CountGuest(fn);
  return false;
}
}  // namespace

REX_HOOK_RAW(_stricmp_guest) {
  if (!StringNative<NativeStricmp>(kStricmp, ctx, base)) __imp___stricmp_guest(ctx, base);
}

REX_HOOK_RAW(strncmp_guest) {
  if (!StringNative<NativeStrncmp>(kStrncmp, ctx, base)) __imp__strncmp_guest(ctx, base);
}

REX_HOOK_RAW(strcpy_guest) {
  if (!StringNative<NativeStrcpy>(kStrcpy, ctx, base)) __imp__strcpy_guest(ctx, base);
}

REX_HOOK_RAW(strncpy_guest) {
  if (!StringNative<NativeStrncpy>(kStrncpy, ctx, base)) __imp__strncpy_guest(ctx, base);
}

REX_HOOK_RAW(strchr_guest) {
  if (!StringNative<NativeStrchr>(kStrchr, ctx, base)) __imp__strchr_guest(ctx, base);
}

REX_HOOK_RAW(strstr_guest) {
  if (!StringNative<NativeStrstr>(kStrstr, ctx, base)) __imp__strstr_guest(ctx, base);
}

// 2.12.7's functions.
REX_HOOK_RAW(sub_82353618) {  // _strnicmp
  if (!StringNative<NativeStrnicmp>(kStrnicmp, ctx, base)) __imp__sub_82353618(ctx, base);
}

REX_HOOK_RAW(sub_8234E890) {  // strrchr
  if (!StringNative<NativeStrrchr>(kStrrchr, ctx, base)) __imp__sub_8234E890(ctx, base);
}

REX_HOOK_RAW(sub_8234E600) {  // strncat
  if (!StringNative<NativeStrncat>(kStrncat, ctx, base)) __imp__sub_8234E600(ctx, base);
}

REX_HOOK_RAW(sub_8234E650) {  // memchr
  if (!StringNative<NativeMemchr>(kMemchr, ctx, base)) __imp__sub_8234E650(ctx, base);
}

REX_HOOK_RAW(sub_8234E8E8) {  // strcspn
  if (!StringNative<NativeStrcspn>(kStrcspn, ctx, base)) __imp__sub_8234E8E8(ctx, base);
}

REX_HOOK_RAW(sub_82353528) {  // _strlwr
  if (!StringNative<NativeStrlwr>(kStrlwr, ctx, base)) __imp__sub_82353528(ctx, base);
}

REX_HOOK_RAW(sub_82353728) {  // _strupr
  if (!StringNative<NativeStrupr>(kStrupr, ctx, base)) __imp__sub_82353728(ctx, base);
}
