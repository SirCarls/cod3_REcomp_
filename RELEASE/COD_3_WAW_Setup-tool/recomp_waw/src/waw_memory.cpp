// waw - Call of Duty: World at War (Xbox 360) static recompilation: the game's memory past the Xbox 360's 512 MB.
// [acct2-decomp] 2026-11-12, continuing memory-360's work on Call of Duty 3 for more games (the owner, 2026-11-12
// 16:02: "please continue, try to make it work with more games").
//
// The work is in ../../recomp/src/guest_memory/iw_zone.{h,cpp}, shared with Call of Duty 4: what a fastfile zone
// takes of the game's memory pool, reported, and as a test taken from outside the console's memory. This file names
// World at War's functions and tables and holds its settings. All settings are off by default; with them off each
// hook only calls the game's own function.
//
// The functions (dev/waw-decomp/x360_sp):
//   0x82155418  DB_AllocXZoneMemory(r3 the seven block sizes, r4 the file's name, r5 the zone's seven {address,
//               size} pairs, r6 the pool's side): the only caller is DB_LoadXFileInternal.
//   0x82156F60  unloads one zone's memory (r3 the zone: +0 its number, +8 the pool's side, +12 the seven pairs):
//               clears the pairs (0x82155560) and gives the zone's name back to the pool (0x8227B6D0). Its only
//               caller is DB_UnloadXAssetsMemory.
//   0x82158E00  DB_ShutdownXAssets: unloads every zone and gives each one's memory back in a loop of its own, without
//               0x82156F60. So after it, whatever the move still holds is given back too.
//   0x82493AE8  the table of block kinds; 0x82493B04 the block names.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>

#include "guest_memory/iw_zone.h"

REXCVAR_DEFINE_BOOL(waw_memory_report, false, "waw",
                    "Debug: log what each fastfile (zone) takes of the game's memory pool when it loads, block by "
                    "block, and what the loaded zones hold in all (\"waw: zone memory\"). It changes nothing in the "
                    "game");
REXCVAR_DEFINE_BOOL(waw_memory_move_zones, false, "waw",
                    "TEST, leave off: not usable yet. A zone's chosen blocks (waw_memory_move_blocks) are taken "
                    "from guest virtual memory, outside the Xbox 360's 512 MB, instead of the game's pool. Only "
                    "with the native renderer (waw_native_vulkan), and its picture of a level is black with "
                    "this on until it reads moved vertex data where the game keeps it. Needs a restart");
REXCVAR_DEFINE_INT32(waw_memory_move_blocks, 0x12, "waw",
                     "TEST: which zone blocks waw_memory_move_zones moves, one bit a block: 1 temp, 2 runtime, "
                     "4 large_runtime, 16 virtual, 32 large. The default 18 is runtime and virtual. large is not "
                     "checked for sound. The two write-combined blocks (physical_runtime, physical) never move");
REXCVAR_DECLARE(bool, waw_native_vulkan);  // waw_native_window.cpp: the native renderer is the display

namespace {

constexpr guest_memory::iw::Game kGame{"waw", 0x82493AE8, 0x82493B04};
constexpr uint32_t kZonePairs = 12;  // the seven {address, size} pairs in the game's zone record

guest_memory::iw::Options Options() {
  guest_memory::iw::Options options;
  options.report = REXCVAR_GET(waw_memory_report);
  options.move = REXCVAR_GET(waw_memory_move_zones);
  options.native_renderer = REXCVAR_GET(waw_native_vulkan);
  options.move_blocks = static_cast<uint32_t>(REXCVAR_GET(waw_memory_move_blocks)) & 0x7Fu;
  return options;
}

}  // namespace

REX_EXTERN(__imp__sub_82155418);
REX_HOOK_RAW(sub_82155418) {  // DB_AllocXZoneMemory
  guest_memory::iw::AllocZoneMemory(ctx, base, kGame, Options(), [&] { __imp__sub_82155418(ctx, base); });
}

REX_EXTERN(__imp__sub_82156F60);
REX_HOOK_RAW(sub_82156F60) {  // one zone's memory goes back
  guest_memory::iw::ReleaseZoneBlocks(base, kGame, Options(), ctx.r3.u32 + kZonePairs);
  __imp__sub_82156F60(ctx, base);
}

REX_EXTERN(__imp__sub_82158E00);
REX_HOOK_RAW(sub_82158E00) {  // DB_ShutdownXAssets
  __imp__sub_82158E00(ctx, base);
  guest_memory::iw::ReleaseAllZones(kGame, Options());
}
