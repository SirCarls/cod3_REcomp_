// cod3 - [acct2-main] 2026-11-21 The text-copy check: where the game's own code copies our text (the translations in
// assets/text/, which the game was never sized for) into a buffer, and whether the copy fits.
//
// The game's text buffers were sized for the five languages on its disc. Our packs add languages whose letters take
// 2-3 bytes (Russian, Chinese, Japanese, Korean) and rows the disc never had, so a translation can be longer than any
// text a buffer ever held. The owner (2026-11-21 11:20, 11:21): every such buffer is to be fixed in full, with no
// length caps and no shortened translations. This check finds them and keeps watching once they are fixed.
//
// How: the guest memory that holds our text is registered here (NoteOurText: text_language's arena, color_text's
// ring). The game's copy routines (memcpy, XMemCpy, memmove, strcpy, strncpy, strncat in crt_native.cpp; _output_l, the engine
// under sprintf and _vsnprintf, in crt_printf_native.cpp) call Before*() when the check is on, before they copy
// anything, so a copy that crashes the game still leaves its line. A copy whose source is our text marks what it
// writes as ours too, so the next copy of that buffer is followed as well (in 64-byte steps, never cleared by the
// game's own writes except by a copy of something else; a dev check, so it errs on the side of reporting).
//
// For each copy it records the calling site (the return address, and the one above it), the length, and where the
// destination is: in a stack frame (with the room from it to the frame's end, the most a stack buffer there can
// hold), a static buffer in the program's image, or elsewhere (heap). It reports, once per site, as an error:
//   - cut: strncpy/strncat/_vsnprintf left part of our text out, or strncpy left the buffer without its zero (a block
//     copy, memcpy and the like, is never cut: its caller gives the length, often of a part on purpose);
//   - over: the copy runs past the end of the stack frame its destination is in (a stack overflow in the making).
// cod3_text_copy_report writes every site seen so far to the log (a table), also every minute while the check is on.
// Off (the default): each routine pays one relaxed load. Both programs, Linux and Windows, either renderer.
#pragma once

#include <atomic>
#include <cstdint>

struct PPCContext;

namespace cod3::textcopy {

enum Kind : uint8_t { kMemcpy, kXMemCpy, kMemmove, kStrcpy, kStrncpy, kStrncat, kPrintf, kKindCount };

// 0: not read yet, 1: on, 2: off.
extern std::atomic<int> g_state;
bool ReadSetting();
inline bool On() {
  const int s = g_state.load(std::memory_order_relaxed);
  return s == 1 || (s == 0 && ReadSetting());
}

// Guest memory [at, at + size) holds our text (registered whether or not the check is on).
void NoteOurText(uint32_t at, uint32_t size);
// Whether [at, at + size) touches our text, or something copied from it.
bool IsOurs(uint32_t at, uint64_t size);

// memcpy, XMemCpy, memmove, strcpy, strncpy, strncat: at the routine's entry (r3 r4 r5 as the caller passed them).
void Before(Kind kind, const PPCContext& ctx, uint8_t* base);

// _output_l at its entry, with what Format worked out (nothing written yet). out: the stream's _ptr; n: the
// characters it will write (sized false: not known, the game's own code works it out); cnt: the stream's _cnt
// (INT32_MAX for sprintf, the size left for _vsnprintf); text: Format's copy of the output (n bytes) or null.
void BeforePrintf(const PPCContext& ctx, uint8_t* base, uint32_t out, uint32_t n, bool sized, uint32_t cnt,
                  bool ours, const uint8_t* text);
// After it: what it wrote ([out, out + n)), so a copy of that buffer is followed.
void AfterPrintf(uint32_t out, uint32_t n, bool ours);

}  // namespace cod3::textcopy
