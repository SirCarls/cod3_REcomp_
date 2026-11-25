// cod4 / cod4mp - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation: the game's memory past the Xbox
// 360's 512 MB. [acct2-decomp] 2026-11-12, continuing memory-360's work on Call of Duty 3 for more games (the owner,
// 2026-11-12 16:02: "please continue, try to make it work with more games").
//
// The work is in ../../recomp/src/guest_memory/iw_zone.{h,cpp}, shared with World at War: what a fastfile zone takes
// of the game's memory pool, reported, and as a test taken from outside the console's memory. This file names Call
// of Duty 4's functions and tables and holds its settings. All settings are off by default; with them off each hook
// only calls the game's own function.
//
// The functions (campaign / multiplayer; dev/x360-family/cod4_360/<sp|mp>, checked in both):
//   0x822B1F30 / 0x822A0470  DB_AllocXZoneMemory(r3 the seven block sizes, r4 the file's name, r5 the zone's seven
//                            {address, size} pairs, r6 the pool's side): the only caller is DB_LoadXFileInternal.
//   0x822B1040 / 0x8229F590  DB_UnloadXZoneMemory(r3 the zone: +0 its number, +8 the pool's side, +12 the seven
//                            pairs): zeroes the pairs and gives the zone's name back to the pool. It is the only
//                            caller of the pool's free-by-name, so every zone the game drops passes here.
//   0x82370B5C / 0x823A4180  the table of block kinds; 0x82370FB8 / 0x823A419C the block names.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>

#include "guest_memory/iw_zone.h"

REXCVAR_DEFINE_BOOL(cod4_memory_report, false, "cod4",
                    "Debug: log what each fastfile (zone) takes of the game's memory pool when it loads, block by "
                    "block, and what the loaded zones hold in all (\"cod4: zone memory\"). It changes nothing in the "
                    "game");
REXCVAR_DEFINE_BOOL(cod4_memory_move_zones, false, "cod4",
                    "TEST, leave off: not usable yet. A zone's chosen blocks (cod4_memory_move_blocks) are taken "
                    "from guest virtual memory, outside the Xbox 360's 512 MB, instead of the game's pool. Only "
                    "with the native renderer (cod4_native_vulkan), and its picture of a level is black with "
                    "this on until it reads moved vertex data where the game keeps it. Needs a restart");
REXCVAR_DEFINE_INT32(cod4_memory_move_blocks, 0x12, "cod4",
                     "TEST: which zone blocks cod4_memory_move_zones moves, one bit a block: 1 temp, 2 runtime, "
                     "4 large_runtime, 16 virtual, 32 large. The default 18 is runtime and virtual. large is not "
                     "checked for sound. The two write-combined blocks (physical_runtime, physical) never move");
REXCVAR_DECLARE(bool, cod4_native_vulkan);  // cod4_native_window.cpp: the native renderer is the display

namespace {

#ifdef COD4_MP
constexpr guest_memory::iw::Game kGame{"cod4mp", 0x823A4180, 0x823A419C};
#else
constexpr guest_memory::iw::Game kGame{"cod4", 0x82370B5C, 0x82370FB8};
#endif

constexpr uint32_t kZonePairs = 12;  // the seven {address, size} pairs in the game's zone record

guest_memory::iw::Options Options() {
  guest_memory::iw::Options options;
  options.report = REXCVAR_GET(cod4_memory_report);
  options.move = REXCVAR_GET(cod4_memory_move_zones);
  options.native_renderer = REXCVAR_GET(cod4_native_vulkan);
  options.move_blocks = static_cast<uint32_t>(REXCVAR_GET(cod4_memory_move_blocks)) & 0x7Fu;
  return options;
}

}  // namespace

#ifdef COD4_MP

REX_EXTERN(__imp__sub_822A0470);
REX_HOOK_RAW(sub_822A0470) {  // DB_AllocXZoneMemory
  guest_memory::iw::AllocZoneMemory(ctx, base, kGame, Options(), [&] { __imp__sub_822A0470(ctx, base); });
}

REX_EXTERN(__imp__sub_8229F590);
REX_HOOK_RAW(sub_8229F590) {  // DB_UnloadXZoneMemory
  guest_memory::iw::ReleaseZoneBlocks(base, kGame, Options(), ctx.r3.u32 + kZonePairs);
  __imp__sub_8229F590(ctx, base);
}

#else

REX_EXTERN(__imp__sub_822B1F30);
REX_HOOK_RAW(sub_822B1F30) {  // DB_AllocXZoneMemory
  guest_memory::iw::AllocZoneMemory(ctx, base, kGame, Options(), [&] { __imp__sub_822B1F30(ctx, base); });
}

REX_EXTERN(__imp__sub_822B1040);
REX_HOOK_RAW(sub_822B1040) {  // DB_UnloadXZoneMemory
  guest_memory::iw::ReleaseZoneBlocks(base, kGame, Options(), ctx.r3.u32 + kZonePairs);
  __imp__sub_822B1040(ctx, base);
}

#endif
