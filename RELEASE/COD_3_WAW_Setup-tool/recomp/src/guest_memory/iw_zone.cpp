// Shared by the Infinity Ward games' ports - [acct2-decomp] 2026-11-12: see iw_zone.h.

#include "guest_memory/iw_zone.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "guest_memory/block_arena.h"

namespace guest_memory::iw {
namespace {

constexpr double kMB = 1024.0 * 1024.0;
constexpr uint32_t kPage = 0x10000;  // the 0x40000000 range's page size in the SDK
// The room's size: the first of these that the guest's virtual memory has free. Call of Duty 4's Killhouse takes
// 246 MB in its three biggest cached blocks alone (runtime, large_runtime, virtual) and the common zones stay loaded
// beside it, so 256 MB was too little once large_runtime may move. The room is reserved and committed at once; a
// host gives it real memory only as blocks are written.
constexpr uint32_t kRoomBytes[] = {512u << 20, 256u << 20, 128u << 20, 64u << 20};
// The game asks the pool for 15 bytes more than a cached block's size (DB_AllocXZoneMemory); so does the move.
constexpr uint32_t kCachedSlack = 15;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, 4);
  return __builtin_bswap32(value);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

// A short text from the game's memory (a zone's file name, a block's name): printable characters only.
std::string GuestText(const uint8_t* base, uint32_t address, size_t most = 48) {
  std::string text;
  if (address < 0x10000) return text;
  for (size_t i = 0; i < most; ++i) {
    const char c = static_cast<char>(base[address + i]);
    if (c == 0) break;
    text.push_back(c >= 0x20 && c < 0x7F ? c : '?');
  }
  return text;
}

// The game's table of block kinds as this code expects it: seven words, each 0, 1 or 2, and the last ("physical")
// write-combined. Anything else means the address is wrong for this program (another build of the game): then this
// code changes nothing and says so once.
bool KindsLookRight(const uint8_t* base, const Game& game) {
  static std::atomic<int> answer{0};  // 0: not checked; 1: right; 2: wrong
  int known = answer.load(std::memory_order_relaxed);
  if (known == 0) {
    bool right = true;
    for (uint32_t i = 0; i < kZoneBlocks; ++i) right = right && Load32(base, game.block_kinds + 4 * i) <= 2;
    right = right && Load32(base, game.block_kinds + 4 * (kZoneBlocks - 1)) == 2;
    known = right ? 1 : 2;
    answer.store(known, std::memory_order_relaxed);
    if (!right)
      REXLOG_WARN("{}: zone memory: the table of block kinds at {:08X} doesn't look like one; the report and the "
                  "move stay off",
                  game.name, game.block_kinds);
  }
  return known == 1;
}

// The room outside the 512 MB, taken once, the first time a block is to move.
std::once_flag g_room_once;
BlockArena g_room;
std::atomic<uint32_t> g_room_lo{0}, g_room_hi{0};

bool Room(const Game& game) {
  std::call_once(g_room_once, [&game] {
    auto* kernel = rex::system::kernel_state();
    auto* heap = kernel && kernel->memory() ? kernel->memory()->LookupHeap(0x40000000u) : nullptr;
    uint32_t start = 0, bytes = 0;
    for (uint32_t want : kRoomBytes) {
      if (heap && heap->AllocRange(0x40000000u, 0x7EFFFFFFu, want, kPage,
                                   rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                                   rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                                   /*top_down=*/true, &start)) {
        bytes = want;
        break;
      }
    }
    if (bytes == 0) {
      REXLOG_WARN("{}: zone memory move: no room in guest virtual memory (the 0x40000000 range); nothing moves",
                  game.name);
      return;
    }
    g_room.Reset(start, bytes, kPage);
    g_room_hi.store(start + bytes, std::memory_order_relaxed);
    g_room_lo.store(start, std::memory_order_release);
    REXLOG_INFO("{}: zone memory move: a room of {:.0f} MB at {:08X}..{:08X}, outside the console's 512 MB",
                game.name, bytes / kMB, start, start + bytes);
  });
  return g_room_lo.load(std::memory_order_acquire) != 0;
}

// What the loaded zones hold, for the report (bytes; the game loads and unloads zones on one thread, the lock is for
// a reader on another).
std::mutex g_totals_mutex;
struct Totals {
  uint64_t cached = 0, write_combined = 0, moved = 0;  // now
  uint64_t most_cached = 0, most_in_pool = 0;          // the highest so far
  uint32_t zones = 0;
  uint64_t refused = 0;  // blocks that were to move and didn't fit the room
} g_totals;
std::atomic<uint32_t> g_zone_serial{0};

// Puts the game's own sizes back when the game's function is left, however it is left.
class SizesBack {
 public:
  SizesBack(uint8_t* base, uint32_t sizes) : base_(base), sizes_(sizes) {}
  SizesBack(const SizesBack&) = delete;
  SizesBack& operator=(const SizesBack&) = delete;
  ~SizesBack() {
    for (uint32_t i = 0; i < kZoneBlocks; ++i)
      if (hidden_[i]) Store32(base_, sizes_ + 4 * i, size_[i]);
  }
  void Hide(uint32_t block, uint32_t size) {
    hidden_[block] = true;
    size_[block] = size;
    Store32(base_, sizes_ + 4 * block, 0);
  }

 private:
  uint8_t* base_;
  uint32_t sizes_;
  bool hidden_[kZoneBlocks] = {};
  uint32_t size_[kZoneBlocks] = {};
};

}  // namespace

void AllocZoneMemory(PPCContext& ctx, uint8_t* base, const Game& game, const Options& options,
                     const std::function<void()>& original) {
  const uint32_t sizes = ctx.r3.u32, file = ctx.r4.u32, pairs = ctx.r5.u32;
  if ((!options.report && !options.move) || sizes == 0 || pairs == 0 || !KindsLookRight(base, game)) {
    original();
    return;
  }
  uint32_t size[kZoneBlocks], kind[kZoneBlocks], moved[kZoneBlocks] = {};
  for (uint32_t i = 0; i < kZoneBlocks; ++i) {
    size[i] = Load32(base, sizes + 4 * i);
    kind[i] = Load32(base, game.block_kinds + 4 * i);
  }

  // The emulated GPU reads the console's 512 MB and nothing else, so it can never follow a moved block (iw_zone.h).
  const bool move = options.move && options.native_renderer;
  if (options.move && !move) {
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
      REXLOG_WARN("{}: zone memory move: asked for without the native renderer; the emulated GPU can't read moved "
                  "blocks, so nothing moves",
                  game.name);
  }

  const uint32_t serial = g_zone_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t refused = 0;
  {
    SizesBack sizes_back(base, sizes);
    if (move) {
      for (uint32_t i = 0; i < kZoneBlocks; ++i) {
        const bool wanted = size[i] != 0 && kind[i] < 2 && ((options.move_blocks >> i) & 1) != 0;
        if (!wanted || !Room(game)) continue;
        moved[i] = g_room.Allocate(size[i] + kCachedSlack, serial, i);
        if (moved[i] == 0) {
          ++refused;  // no room: this block comes from the pool, as without the move
          continue;
        }
        sizes_back.Hide(i, size[i]);  // the game's function skips a block of size 0
      }
    }
    original();
  }
  // The moved blocks' pairs, as the game's function writes them for its own: the address, and the size as asked.
  // A block the pool hands out more than once isn't cleared by the game either, but this one is: the same contents
  // every run.
  uint64_t cached = 0, write_combined = 0, outside = 0;
  for (uint32_t i = 0; i < kZoneBlocks; ++i) {
    if (moved[i]) {
      const uint32_t whole = (size[i] + kCachedSlack + (kPage - 1)) & ~(kPage - 1);
      std::memset(base + moved[i], 0, whole);
      Store32(base, pairs + 8 * i, moved[i]);
      Store32(base, pairs + 8 * i + 4, size[i]);
      outside += size[i];
    }
    (kind[i] < 2 ? cached : write_combined) += size[i];
  }

  Totals now;
  {
    std::lock_guard lock(g_totals_mutex);
    g_totals.cached += cached;
    g_totals.write_combined += write_combined;
    g_totals.moved += outside;
    g_totals.zones += 1;
    g_totals.refused += refused;
    g_totals.most_cached = std::max(g_totals.most_cached, g_totals.cached);
    g_totals.most_in_pool =
        std::max(g_totals.most_in_pool, g_totals.cached + g_totals.write_combined - g_totals.moved);
    now = g_totals;
  }
  if (refused)
    REXLOG_WARN("{}: zone memory move: the room had no space for {} block(s) of zone {}; they came from the pool",
                game.name, refused, GuestText(base, file));
  if (!options.report) return;
  std::string blocks;
  for (uint32_t i = 0; i < kZoneBlocks; ++i) {
    if (size[i] == 0) continue;
    std::string name = GuestText(base, Load32(base, game.block_names + 4 * i), 20);
    if (name.empty()) name = "block " + std::to_string(i);
    char text[64];
    std::snprintf(text, sizeof text, "%s%s %.2f%s", blocks.empty() ? "" : ", ", name.c_str(), size[i] / kMB,
                  moved[i] ? " (moved)" : "");
    blocks += text;
  }
  REXLOG_INFO("{}: zone memory: {} takes {:.2f} MB cached and {:.2f} MB write-combined (MB by block: {}); "
              "{:.2f} MB of it outside the console's 512 MB",
              game.name, GuestText(base, file), cached / kMB, write_combined / kMB, blocks, outside / kMB);
  REXLOG_INFO("{}: zone memory: {} zones loaded hold {:.2f} MB cached, {:.2f} MB write-combined; {:.2f} MB is "
              "outside the console's 512 MB, {:.2f} MB in the game's pool (the most so far {:.2f} MB)",
              game.name, now.zones, now.cached / kMB, now.write_combined / kMB, now.moved / kMB,
              (now.cached + now.write_combined - now.moved) / kMB, now.most_in_pool / kMB);
}

void ReleaseZoneBlocks(uint8_t* base, const Game& game, const Options& options, uint32_t pairs) {
  if (!options.report && !options.move && g_room_lo.load(std::memory_order_acquire) == 0) return;
  uint64_t cached = 0, write_combined = 0, outside = 0;
  for (uint32_t i = 0; i < kZoneBlocks; ++i) {
    const uint32_t address = Load32(base, pairs + 8 * i), size = Load32(base, pairs + 8 * i + 4);
    if (address == 0) continue;
    (Load32(base, game.block_kinds + 4 * i) < 2 ? cached : write_combined) += size;
    if (InMovedZoneMemory(address)) {
      if (g_room.Free(address))
        outside += size;
      else
        REXLOG_WARN("{}: zone memory move: block {} at {:08X} is in the room but was not handed out there", game.name,
                    i, address);
    }
  }
  if (cached + write_combined == 0) return;
  Totals now;
  {
    std::lock_guard lock(g_totals_mutex);
    // A zone loaded before the report was on was never added: never below zero.
    g_totals.cached -= std::min(g_totals.cached, cached);
    g_totals.write_combined -= std::min(g_totals.write_combined, write_combined);
    g_totals.moved -= std::min(g_totals.moved, outside);
    g_totals.zones -= std::min<uint32_t>(g_totals.zones, 1);
    now = g_totals;
  }
  if (options.report)
    REXLOG_INFO("{}: zone memory: a zone gave back {:.2f} MB cached and {:.2f} MB write-combined ({:.2f} MB of it "
                "outside the console's 512 MB); {} zones loaded now hold {:.2f} MB, {:.2f} MB of it outside",
                game.name, cached / kMB, write_combined / kMB, outside / kMB, now.zones,
                (now.cached + now.write_combined) / kMB, now.moved / kMB);
}

void ReleaseAllZones(const Game& game, const Options& options) {
  if (g_room_lo.load(std::memory_order_acquire) == 0) return;
  uint32_t blocks = 0;
  uint64_t bytes = 0;
  for (const BlockArena::Block& block : g_room.Blocks()) {
    if (g_room.Free(block.address)) {
      ++blocks;
      bytes += block.bytes;
    }
  }
  {
    std::lock_guard lock(g_totals_mutex);
    g_totals.cached = g_totals.write_combined = g_totals.moved = 0;
    g_totals.zones = 0;
  }
  if (blocks && (options.report || options.move))
    REXLOG_INFO("{}: zone memory move: every zone was unloaded; {} block(s), {:.2f} MB, went back to the room",
                game.name, blocks, bytes / kMB);
}

bool InMovedZoneMemory(uint32_t address) {
  const uint32_t lo = g_room_lo.load(std::memory_order_acquire);
  return lo != 0 && address >= lo && address < g_room_hi.load(std::memory_order_relaxed);
}

size_t MovedZoneRoom(uint32_t address) {
  const uint32_t lo = g_room_lo.load(std::memory_order_acquire);
  const uint32_t hi = g_room_hi.load(std::memory_order_relaxed);
  return lo != 0 && address >= lo && address < hi ? hi - address : 0;
}

}  // namespace guest_memory::iw
