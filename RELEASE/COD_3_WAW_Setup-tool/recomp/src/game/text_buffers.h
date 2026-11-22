// cod3 - [acct2-main] 2026-11-21 The game's multi-line text without its fixed-size buffers.
//
// The owner (2026-11-21 11:20, 11:21): our translations are longer than the buffers the game sized for its five disc
// languages, and the fix has to be complete: no length caps, no shortened translations. Menus, hints, message boxes and
// prompts are FEMultiLineText elements; the text set on one passes through the game's functions below, which have fixed
// buffers (both programs):
//
//  1. The "[ACTION]" pass (campaign 0x824DE290; cod3mp FEMultiLineText slot 93, 0x824D6C48): it copies its input
//     with strncpy(512) and writes its result into a static 512-byte buffer, at most 509 characters (its length is a
//     halfword at +510). A longer text is cut there. ExpandWhole() runs the game's own pass on pieces of the text that
//     fit, split where the pass splits anyway (at spaces: it handles each space-separated word on its own and keeps the
//     spaces between them), and joins the results in a buffer of ours that grows to fit. A text that fits is left to
//     the game's pass alone, so nothing changes for it.
//
//  2. FEMultiLineText's set-text (slot 36, campaign 0x824E8180, cod3mp 0x824F76A0): after the pass it copies each
//     line (up to a '\n') into a 256-byte stack buffer with strncpy(line length), with no size check, so a line of
//     256 bytes or more runs over the function's saved registers. SetLines() does what the game's function does, in
//     the same order with the same calls (the pass, each line's layout 0x824D8108 / 0x824EC790, the shown String, the
//     finishing call), with each line in guest memory of its own length.
//
//  3. The in-game hint (IGOHintWidget slot 2, both programs) builds its text in a 256-byte buffer on its stack with
//     sprintf or a byte loop, with no size check. WithRoom() runs it with room above its frame.
//
//  1b. Both programs' pass takes each space-separated word on its own, and a word with a "[NAME]" in it gives only that
//     name's result: the rest of the word is dropped (campaign 0x824DE290 appends the name's sentence alone; cod3mp's
//     slot 94 is asked about the whole word). Our Russian and Japanese rows join names to other text ("«[USE]»",
//     "[VERB_ATTACK]で..."; Japanese has no spaces at all), so their words around a name were lost. ExpandWhole() and
//     cod3mp's slot 93 here give such a word's name to the game's pass (or slot 94) alone and keep the rest of the
//     word as it is.
//
//  4. The level's loading-screen text (campaign sub_824DEC18, cod3mp sub_82502230): copied with strncpy(163) into a
//     static 163-byte field (campaign 0x829EACB8, cod3mp 0x82B792E8) and taken apart there with strtok (two numbers,
//     then the text); the loading screen measures and draws the pieces through the pointers strtok gave, kept in
//     fields of their own. No other instruction names the field. A text of 163 bytes or more was cut there (and one of
//     exactly 163 left with no zero, so strtok ran on into the next fields). LevelInfoCopied() gives the function the
//     whole text in guest memory of ours instead, right after the copy: the register that holds the field (r30,
//     cod3mp r29) is what it hands strtok next.
//
// Each reports what the game's own code would have done wrong (a text cut by the pass, a line or hint past its
// buffer) once per text as an error, so a longer text from any pack is seen in the log even though it is now shown
// whole. The word wrap's word buffer (campaign 0x824DD290, cod3mp 0x824F6F98: about 256 bytes on its stack) is run
// with WithRoom too: no pack has a word that long (the longest is 45 bytes), but a Japanese or Chinese line has no
// spaces, so its "word" is the whole line (dev/agents/acct2-main/TEXT-BUFFERS.md has the measures).
// Linux and Windows, both renderers: this is game logic, the same everywhere.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct PPCContext;

namespace cod3::textbuf {

// cod3_text_whole (default on): the fixes below; off, the game's own code.
bool On();

// The longest text followed (a pack's longest row is well under 1 KB).
inline constexpr size_t kMaxText = 64 * 1024;

// Guest memory of at least `size` bytes for one use (slot) on this thread; it grows when asked for more and what it
// held is then gone (as the game's own static buffers are overwritten by the next call). 0 when the heap is full.
enum Slot : int { kExpandIn, kExpandOut, kLine, kPromptsIn, kPromptsOut, kTest, kGlued, kSlotCount };  // kTest: SelfTest
// (kGlued: ExpandWhole's names joined to other text)
uint32_t Scratch(uint8_t* base, Slot slot, size_t size);

// `s` with its zero in Scratch(slot); 0 when the heap is full.
uint32_t PutText(uint8_t* base, Slot slot, const std::string& s);

// The game's "[ACTION]" pass on the whole of the guest text `text`. `pass(guest text) -> guest result` runs the
// game's own pass once (the original function, not a hook). Returns the guest address of the result: the game's own
// buffer when the text fits it, else ours.
uint32_t ExpandWhole(uint8_t* base, uint32_t text, const std::function<uint32_t(uint32_t)>& pass);

// FEMultiLineText slot 36 (r3 this, r4 text), as the game's own function does it, without its 256-byte line buffer.
void SetLines(PPCContext& ctx, uint8_t* base);

// Room above a frame: runs `original`, a game function that copies text into a fixed buffer in its own stack frame with
// no size check, with kRoom bytes of stack between its frame and its caller's, and gives the caller back the
// registers it had (r14-r31, f14-f31, cr2-cr4; the function's epilogue reloads them from the saved-register area a
// long text ran over). The text then runs on into the room instead of over the saved registers and the caller's
// frame, and the function goes on with the whole of it. Only for a function whose frame holds nothing above the
// buffer but its saved registers, and which reads no arguments from its caller's frame (each use says so).
// Reports a text of `buffer_size` bytes or more (at `buffer_at` in its `frame`-byte frame) once, as the other fixes do.
inline constexpr uint32_t kRoom = 4096;  // a hint is a few hundred bytes; the longest pack row is 660
void WithRoom(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), uint32_t frame,
              uint32_t buffer_at, uint32_t buffer_size, const char* what);

// 4. The return address of the level info's strncpy(field, text, 163) (crt_native.cpp's strncpy hook asks), and what
// that hook calls after the copy with the text (r4) it was given. A text that fit is left as the game copied it.
inline constexpr uint32_t kLevelInfoReturn =
#if defined(COD3_MP)
    0x82502404;  // sub_82502230
#else
    0x824DEE58;  // sub_824DEC18
#endif
void LevelInfoCopied(PPCContext& ctx, uint8_t* base, uint32_t src);

}  // namespace cod3::textbuf
