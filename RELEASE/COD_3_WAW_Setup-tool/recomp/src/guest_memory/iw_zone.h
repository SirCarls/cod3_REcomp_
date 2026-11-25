// Shared by the Infinity Ward games' ports (Call of Duty 4, World at War) - [acct2-decomp] 2026-11-12, continuing
// memory-360's work (Call of Duty 3: game/native_memory*.cpp) for more games: a fastfile zone's memory, reported and,
// as a test, taken from outside the Xbox 360's 512 MB.
//
// How these games take a zone's memory (read in the decompiles, dev/x360-family/cod4_360 and dev/waw-decomp/x360_sp;
// the map is dev/agents/waw-decomp/MEMORY-IW.md):
//  - At start the game takes one pool of the console's memory, 407 to 431 MB of the 512 (PMem, a two-ended stack),
//    and everything big comes out of it.
//  - A fastfile (a "zone": the common files, the level) is loaded into seven blocks. DB_AllocXZoneMemory(r3 the
//    seven sizes, r4 the file's name, r5 the zone's seven {address, size} pairs, r6 the pool's side) takes each block
//    that has a size from the pool. The game's own table says what kind each is: "temp" 0, "runtime" 1,
//    "large_runtime" 1, "physical_runtime" 2, "virtual" 1, "large" 1, "physical" 2. A kind below 2 is taken as
//    ordinary cached memory, 2 as write-combined memory (the console's way to mark memory the GPU reads). The
//    measurements below show that the kind says little about who reads a block. The table is the same in cod4,
//    cod4mp and waw.
//  - When a zone is unloaded the game zeroes the seven pairs and gives the zone's name back to the pool.
//
// What this does, all of it off unless the port's settings ask:
//  - the report: one log line a zone, with each block's size and what the loaded zones hold in all;
//  - the move (a TEST): the chosen cached blocks come from a room of guest virtual memory (BlockArena, in the
//    0x40000000 range, which the SDK maps and the console never had) instead of the pool. The game's own function
//    still takes every other block: it is called with the moved blocks' sizes set to 0 for the length of the call,
//    which is exactly how it treats a block a zone doesn't have. The pool then has that much more room.
//
// What was measured, 2026-11-12 (dev/agents/acct2-decomp/MEMORY.md has the runs):
//  - Call of Duty 4, Killhouse: the level's zone is 251 MB: large_runtime 180 MB, virtual 65 MB, large 5 MB, runtime
//    0.5 MB, and the write-combined "physical" only 0.45 MB. With the common zones the level holds 300 MB of the
//    game's 431 MB pool. World at War, pel1a: the level's zone is 156 MB (large_runtime 81, virtual 55, large 19),
//    214 MB with the common zones, of a 412 MB pool.
//  - So cached does not mean CPU only. The game's kinds only say how the CPU caches a block; the graphics data is
//    spread over the cached blocks. With "virtual" and "runtime" moved, Call of Duty 4 loaded and played Killhouse
//    with no error line and the same draws, and the picture was black, the emulated GPU's and the native side's
//    alike: the level's vertex data is in "virtual", and both read it by its console address (the address cut to
//    29 bits), where nothing was. With "large_runtime" moved the emulated GPU read an unmapped address and the
//    program stopped at the level's first frames.
//
// So the move is not usable yet, in either game. The emulated GPU can never follow it (it reads the console's
// 512 MB and nothing else), which is why the move is refused unless the port's native renderer is on. The native
// renderer can follow, once it reads a moved block's graphics data at the address the game's structures hold instead
// of the cut one (Call of Duty 3's stage 5 is the same step: game/native_memory.h, GpuName). The move stays here for
// that work: the room, the bookkeeping and the hand-back when a zone unloads are tested and work.
//
// The rule from Call of Duty 3 (memory-360's notes, 2026-11-11 14:22), which this file's first test confirmed:
// numbers prove nothing here. The black-picture run logged 86 MB moved and 0 errors. A move counts only when
// pictures with it on match pictures with it off, and when what can't be seen (sound) was checked too.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include <rex/ppc.h>

namespace guest_memory::iw {

inline constexpr uint32_t kZoneBlocks = 7;

// What a port tells this code about its game.
struct Game {
  const char* name;      // for the log lines: "cod4"
  uint32_t block_kinds;  // guest address of the game's seven-word table (below 2: cached; else write-combined)
  uint32_t block_names;  // guest address of the game's seven name pointers ("temp", "runtime", ...)
};

struct Options {
  bool report = false;           // log every zone
  bool move = false;             // the test: chosen cached blocks come from guest virtual memory
  bool native_renderer = false;  // the port's native renderer is on (the move is refused without it)
  uint32_t move_blocks = 0;      // bit N set: block N may move (write-combined blocks never do)
};

// The port's hook on DB_AllocXZoneMemory calls this in place of the game's function; `original` runs the game's.
// With report and move both off it only calls `original`. Without the native renderer nothing moves.
void AllocZoneMemory(PPCContext& ctx, uint8_t* base, const Game& game, const Options& options,
                     const std::function<void()>& original);

// The port's hook calls this BEFORE the game zeroes a zone's block pairs and gives the zone back to the pool:
// `pairs` is the guest address of the zone's seven {address, size} pairs. Moved blocks go back to the room.
void ReleaseZoneBlocks(uint8_t* base, const Game& game, const Options& options, uint32_t pairs);

// The port's hook calls this AFTER the game unloaded every zone at once (its shutdown of the asset database):
// whatever is still in the room goes back.
void ReleaseAllZones(const Game& game, const Options& options);

// Whether a guest address is in the room moved blocks come from (false while nothing was ever moved). For a native
// renderer's read checks: such memory is mapped and read as it is, like the game's other virtual memory.
bool InMovedZoneMemory(uint32_t address);

// The bytes from a guest address to the end of that room, 0 when the address is not in it. For a native renderer's
// bounds: the whole room is mapped, so a read of a moved block's data that stops there can't leave mapped memory.
size_t MovedZoneRoom(uint32_t address);

}  // namespace guest_memory::iw
