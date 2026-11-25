// Shared by every game's port - [acct2-decomp] 2026-11-12 (continuing memory-360's work for more games): one range of
// guest addresses handed out in blocks, with a record of what each block is for.
//
// Why it exists. A port can give a game memory outside the Xbox 360's 512 MB (guest virtual memory, which the SDK
// maps and the console never had room for). Call of Duty 3's own allocator could be pointed at such room, because
// it is a list of arenas (game/native_memory_shadow.cpp borrows a spare arena header). The Infinity Ward games'
// allocator can't: their pool is one two-ended stack whose size is fixed at start. For those the port needs an
// allocator of its own for the room outside. This is it.
//
// It only keeps books. It never reads or writes the memory it hands out and knows nothing of any game or of the
// SDK, so it is tested by itself (dev/agents/acct2-decomp/tests/test_block_arena.cpp). Every call takes one lock, so
// any thread may call it.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace guest_memory {

class BlockArena {
 public:
  struct Block {
    uint32_t address = 0;  // where it starts (a multiple of the page size)
    uint32_t bytes = 0;    // its size, rounded up to whole pages
    uint32_t asked = 0;    // the size that was asked for
    uint32_t owner = 0;    // the caller's own number for who holds it (a zone's serial number, say)
    uint32_t kind = 0;     // the caller's own number for what it is (a zone block's index, say)
  };

  struct Stats {
    uint32_t bytes = 0;         // the arena's size
    uint32_t in_use = 0;        // bytes in blocks now
    uint32_t most_in_use = 0;   // the highest in_use has been
    uint32_t blocks = 0;        // blocks now
    uint32_t largest_free = 0;  // the biggest block that would fit now
    uint64_t allocations = 0;   // blocks ever handed out
    uint64_t refused = 0;       // requests that didn't fit
  };

  // The arena becomes [start, start + bytes) with nothing in use. `page` is a power of two; `start` and `bytes`
  // are cut to whole pages. An arena of no bytes refuses everything, which is how it starts.
  void Reset(uint32_t start, uint32_t bytes, uint32_t page);

  // A block of at least `bytes` at the lowest address that has room. 0 when nothing fits, or bytes is 0.
  uint32_t Allocate(uint32_t bytes, uint32_t owner, uint32_t kind);

  // Gives back the block that starts at `address`. False when no block starts there (nothing changes).
  bool Free(uint32_t address);

  // Gives back every block of one owner. Returns the bytes given back.
  uint32_t FreeOwner(uint32_t owner);

  // Whether `address` is inside the arena at all (in a block or not).
  bool Contains(uint32_t address) const;

  // The block that holds `address`. False when it is in none.
  bool Find(uint32_t address, Block* block) const;

  Stats stats() const;
  std::vector<Block> Blocks() const;  // every block, lowest address first

 private:
  bool FreeLocked(uint32_t address);
  uint32_t LargestFreeLocked() const;

  mutable std::mutex mutex_;
  uint64_t start_ = 0, end_ = 0;       // 64 bits, so start + bytes can't wrap
  uint32_t page_ = 0x10000;
  std::map<uint32_t, Block> used_;     // by address
  std::map<uint32_t, uint32_t> free_;  // start -> bytes; neighbors are always merged
  uint32_t in_use_ = 0, most_in_use_ = 0;
  uint64_t allocations_ = 0, refused_ = 0;
};

}  // namespace guest_memory
