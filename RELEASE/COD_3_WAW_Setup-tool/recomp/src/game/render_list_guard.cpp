// cod3 - [claude-9b] the render back end's draw lists checked before they are drawn (campaign).
// Written 2026-10-21; built 2026-10-23 (the checkpoint soak, dev/tools/rea/c9b_ckpt_soak.py, ran 7 times
// without the crash, so this guards and logs it in the owner's own play).
//
// The crash (owner 2026-10-21, mace2: Continue from the checkpoint crashes every time): "[FATAL]
// Call to invalid or unregistered function at guest address 0x00000000" on the game's main thread
// (earlier the same spot read guest 0x8). The render back end 0x8214F770 draws a node's four lists
// (node +820, +836, +844, +824) the same way: 0x8214DF88(first) puts the first item into the
// cursor 0x8290E9B0 (after skipping to the debug draw range), then for each item it calls
// item->vtable[2](item) and goes to item +4 until the end marker *0x825A1B34. An object freed while
// still linked into a list has the heap's free-list link where its vtable was (operator delete
// 0x820CBF58), so its draw function is read as 0 (or the vtable itself is 0: the read of 0x8).
//
// This hook runs right after 0x8214DF88: it walks the list once from the cursor (at most
// kMaxItems) and checks each item's vtable and draw function to be the game's own (the executable
// or a level DLL). At the first item that isn't, the list is cut there for this frame: the item
// before it ends the list (its +4 becomes the end marker; the cursor itself when it is the first),
// so the rest of that list isn't drawn for one frame instead of the game ending. The list is made
// again every frame (each object links itself in while the scene is built), so nothing stays cut.
// Every cut is logged with what is needed to find the object freed too early. Not caught: an
// object freed while the list is being drawn (on another thread); the log's absence then says so.
//
// cod3_render_list_guard false: off (the game's own behavior, the crash).

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_render_list_guard, true, "cod3/Debug",
                    "Check the render lists before they are drawn and skip an object that was "
                    "already freed (logged) instead of crashing")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using cod3::guest::ReadU32;
using cod3::guest::WriteU32;

constexpr uint32_t kCursor = 0x8290E9B0;     // the item being drawn (0x8214DF88 sets it)
constexpr uint32_t kEndMarkerPtr = 0x825A1B34;  // u32: the lists' end marker
constexpr uint32_t kCurrentNode = 0x8290E9AC;   // the render node being drawn
constexpr uint32_t kNextOffset = 4;            // item +4: the next item
constexpr uint32_t kDrawSlot = 8;              // vtable +8: Render(item)
constexpr int kMaxItems = 200000;              // a list longer than this is taken as broken

// The game's code and constant data: the executable's image, and the level DLLs (loaded at
// 0x89xxxxxx, see config/<level>.toml). A heap block's free-list link is never in these.
bool IsGameAddress(uint32_t a) {
  return (a & 3) == 0 && ((a >= 0x82000000u && a < 0x82800000u) || (a >= 0x88000000u && a < 0x8C000000u));
}

std::atomic<uint64_t> g_cuts{0};

}  // namespace

// 0x8214DF88(first item): the cursor to the first item to draw.
REX_EXTERN(__imp__sub_8214DF88);
REX_HOOK_RAW(sub_8214DF88) {
  const uint32_t first = ctx.r3.u32;
  __imp__sub_8214DF88(ctx, base);
  if (!REXCVAR_GET(cod3_render_list_guard)) return;

  const uint32_t end = ReadU32(base, kEndMarkerPtr);
  uint32_t prev = 0;
  uint32_t item = ReadU32(base, kCursor);
  for (int n = 0; item != end && n < kMaxItems; ++n) {
    const uint32_t vtable = item ? ReadU32(base, item) : 0;
    const uint32_t draw = IsGameAddress(vtable) ? ReadU32(base, vtable + kDrawSlot) : 0;
    if (item && (item & 3) == 0 && IsGameAddress(vtable) && IsGameAddress(draw)) {
      prev = item;
      item = ReadU32(base, item + kNextOffset);
      continue;
    }
    // A freed (or never valid) item: the list ends before it for this frame.
    if (prev)
      WriteU32(base, prev + kNextOffset, end);
    else
      WriteU32(base, kCursor, end);
    const uint64_t cuts = g_cuts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cuts <= 20 || cuts % 1000 == 0) {
      const uint32_t prev_vtable = prev ? ReadU32(base, prev) : 0;
      REXLOG_WARN("cod3: render list guard: item {:08X} (#{} of list {:08X}, node {:08X}) has vtable {:08X} / "
                  "draw {:08X}, not the game's: freed while still in the list; the list ends before it this "
                  "frame (item before it {:08X}, vtable {:08X}; {} cut so far)",
                  item, n, first, ReadU32(base, kCurrentNode), vtable, draw, prev, prev_vtable, cuts);
    }
    return;
  }
}
