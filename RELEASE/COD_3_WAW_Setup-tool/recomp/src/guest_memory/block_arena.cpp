// Shared by every game's port - [acct2-decomp] 2026-11-12: see block_arena.h.

#include "guest_memory/block_arena.h"

#include <algorithm>
#include <iterator>

namespace guest_memory {

void BlockArena::Reset(uint32_t start, uint32_t bytes, uint32_t page) {
  std::lock_guard lock(mutex_);
  used_.clear();
  free_.clear();
  in_use_ = most_in_use_ = 0;
  allocations_ = refused_ = 0;
  if (page == 0 || (page & (page - 1)) != 0) page = 0x10000;
  page_ = page;
  const uint64_t mask = uint64_t(page) - 1;
  const uint64_t first = (uint64_t(start) + mask) & ~mask;  // up to a page
  const uint64_t last = (uint64_t(start) + bytes) & ~mask;  // down to a page
  if (start == 0 || last <= first || last > 0x100000000ull) {
    start_ = end_ = 0;
    return;
  }
  start_ = first;
  end_ = last;
  free_[uint32_t(first)] = uint32_t(last - first);
}

uint32_t BlockArena::Allocate(uint32_t bytes, uint32_t owner, uint32_t kind) {
  std::lock_guard lock(mutex_);
  const uint64_t mask = uint64_t(page_) - 1;
  const uint64_t need = (uint64_t(bytes) + mask) & ~mask;
  if (bytes == 0 || need > end_ - start_) {
    ++refused_;
    return 0;
  }
  // First fit, lowest address: free_ is ordered by address.
  for (auto it = free_.begin(); it != free_.end(); ++it) {
    if (it->second < need) continue;
    const uint32_t address = it->first;
    const uint32_t left = it->second - uint32_t(need);
    free_.erase(it);
    if (left) free_[address + uint32_t(need)] = left;
    used_[address] = Block{address, uint32_t(need), bytes, owner, kind};
    in_use_ += uint32_t(need);
    most_in_use_ = std::max(most_in_use_, in_use_);
    ++allocations_;
    return address;
  }
  ++refused_;
  return 0;
}

bool BlockArena::Free(uint32_t address) {
  std::lock_guard lock(mutex_);
  return FreeLocked(address);
}

bool BlockArena::FreeLocked(uint32_t address) {
  const auto it = used_.find(address);
  if (it == used_.end()) return false;
  uint32_t start = it->second.address, bytes = it->second.bytes;
  used_.erase(it);
  in_use_ -= bytes;
  // Merge with the free span after it, then with the one before it.
  if (const auto after = free_.find(start + bytes); after != free_.end()) {
    bytes += after->second;
    free_.erase(after);
  }
  if (auto next = free_.lower_bound(start); next != free_.begin()) {
    const auto before = std::prev(next);
    if (before->first + before->second == start) {
      start = before->first;
      bytes += before->second;
      free_.erase(before);
    }
  }
  free_[start] = bytes;
  return true;
}

uint32_t BlockArena::FreeOwner(uint32_t owner) {
  std::lock_guard lock(mutex_);
  std::vector<uint32_t> addresses;
  for (const auto& [address, block] : used_)
    if (block.owner == owner) addresses.push_back(address);
  uint32_t bytes = 0;
  for (uint32_t address : addresses) {
    bytes += used_[address].bytes;
    FreeLocked(address);
  }
  return bytes;
}

bool BlockArena::Contains(uint32_t address) const {
  std::lock_guard lock(mutex_);
  return address >= start_ && address < end_;
}

bool BlockArena::Find(uint32_t address, Block* block) const {
  std::lock_guard lock(mutex_);
  auto it = used_.upper_bound(address);
  if (it == used_.begin()) return false;
  --it;
  if (address - it->second.address >= it->second.bytes) return false;
  if (block) *block = it->second;
  return true;
}

uint32_t BlockArena::LargestFreeLocked() const {
  uint32_t largest = 0;
  for (const auto& [start, bytes] : free_) largest = std::max(largest, bytes);
  return largest;
}

BlockArena::Stats BlockArena::stats() const {
  std::lock_guard lock(mutex_);
  Stats s;
  s.bytes = uint32_t(end_ - start_);
  s.in_use = in_use_;
  s.most_in_use = most_in_use_;
  s.blocks = uint32_t(used_.size());
  s.largest_free = LargestFreeLocked();
  s.allocations = allocations_;
  s.refused = refused_;
  return s;
}

std::vector<BlockArena::Block> BlockArena::Blocks() const {
  std::lock_guard lock(mutex_);
  std::vector<Block> out;
  out.reserve(used_.size());
  for (const auto& [address, block] : used_) out.push_back(block);
  return out;
}

}  // namespace guest_memory
