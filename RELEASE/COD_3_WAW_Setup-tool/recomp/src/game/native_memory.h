// cod3 - [memory-360] 2026-11-11 the game's memory past the Xbox 360's limits (native_memory.cpp: the report and
// more streaming banks; native_memory_shadow.cpp: stage 3, the test of which allocations reach the graphics side and
// the move of those that never do).

#pragma once

#include <cstddef>
#include <cstdint>

namespace cod3::native_memory {

// For the native renderer (game/ngl_side.cpp): a guest address it reads graphics data from (0xA0000000 view or
// physical). Checked against the game's arena-0 allocations while cod3_native_memory_shadow is on, and against the
// moved blocks while cod3_native_memory_move has moved some; otherwise two relaxed atomic loads.
void CheckGpuRead(uint32_t address);

// Stage 3's move (cod3_native_memory_move): the arena header (one of the game's eight) that holds the moved blocks in
// guest virtual memory, 0 while there is none; and how many listed allocations went to arena 0 for want of room.
uint32_t MoveArenaHeader();
uint64_t MoveFallbacks();

// The move's guest virtual memory, taken now if the move is on (once; the arena maker asks, so stage 2 can count on
// the room the move makes in arena 0). Its size in bytes, 0 when the move is off or there was no room.
uint32_t MoveReserve();

// Stage 5's test (cod3_native_memory_move_pool, off; with the move): whether the game's streaming pool goes to the
// move's guest virtual memory (then stage 2's extra banks need no console memory). Decided once (it takes the move's
// room first).
bool MovePoolOn();

// For the native renderer (game/ngl_side.cpp): whether a guest address is in the move's guest virtual memory (mapped
// and readable as it is, like the 0xA0000000 views; false while nothing was moved). Its Readable() / Physical()
// checks skip such addresses today, so data moved there (e.g. bone matrices) is not drawn. Two relaxed atomic loads.
bool InMovedMemory(uint32_t address);

// Stage 5 (cod3_native_memory_move_pool), for the native renderer's reads of graphics data (semantics asked by
// rexglue-native-mem, board 2026-11-11 14:45). While the streaming pool isn't moved, each returns exactly what
// ngl_side computes today. The game makes the pool once, in Com_Init's streaming set-up (0x8245D7F0), before the
// first frame is drawn; it is never freed or resized (a level change starts a new program), so its bounds are fixed
// once a draw can ask.
// - GpuName: a word the game keeps for graphics data (a CPU address in a resource header), with `mask` kept. In the
//   moved pool: word & (mask | 0xE0000000) (the address as it is, in guest virtual memory); otherwise
//   0xA0000000 | (word & mask) (the 0xA0000000 view, as today).
// - GpuRoom: the bytes readable from a name GpuName gave: in the moved pool, up to its end; in
//   0xA0000000-0xBFFFFFFF, up to the end of that 512 MB view; otherwise 0.
// - InMovedPool: whether an address is in the moved streaming pool (false while the pool isn't moved).
// Each is a few relaxed atomic loads; any thread.
uint32_t GpuName(uint32_t word, uint32_t mask);
size_t GpuRoom(uint32_t name);
bool InMovedPool(uint32_t address);

// The streaming pool's block as the game made it (0 before that, or while neither the shadow nor the move is on),
// whether it was moved, the graphics reads of it the moved pool had (the native renderer's, Direct3D's binds) and the
// game's locks of resources in it.
struct PoolStats {
  uint32_t start = 0, end = 0;
  bool moved = false;
  uint64_t graphics_reads = 0, locks = 0;
};
PoolStats Pool();

}  // namespace cod3::native_memory
