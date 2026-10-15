#include "arena.hpp"
#include <limits>
#include <new>
static constexpr uint64_t end_block = UINT64_MAX;
Status Arena::init(void *p, uint64_t n) {
  if (base_)
    return Status::Busy;
  if (!p || (reinterpret_cast<uintptr_t>(p) & 127) || n < 256 || n > SIZE_MAX ||
      n > UINTPTR_MAX - reinterpret_cast<uintptr_t>(p))
    return Status::Invalid;
  base_ = static_cast<uint8_t *>(p);
  bytes_ = n & ~uint64_t(127);
  stats_ = {};
  new (base_) Block{bytes_, 0, end_block, false};
  account();
  return Status::Ok;
}
void Arena::account() {
  uint64_t current = 0;
  for (uint64_t off = 0; off != end_block;) {
    auto *b = reinterpret_cast<Block *>(base_ + off);
    current += b->used ? b->bytes : sizeof(Block);
    off = b->next;
  }
  stats_.current_bytes = current;
  if (current > stats_.peak_bytes)
    stats_.peak_bytes = current;
}
void *Arena::allocate(uint64_t n, uint32_t alignment) {
  auto fail = [&]() -> void * {
    ++stats_.failed_allocations;
    return nullptr;
  };
  if (!base_ || !n || !alignment || (alignment & (alignment - 1)) || alignment > 4096 || n > bytes_)
    return fail();
  for (uint64_t off = 0; off != end_block;) {
    auto *b = reinterpret_cast<Block *>(base_ + off);
    uint64_t address = reinterpret_cast<uintptr_t>(base_) + off + sizeof(Block);
    if (address > UINT64_MAX - (alignment - 1))
      return fail();
    uint64_t start =
        ((address + alignment - 1) & ~uint64_t(alignment - 1)) - reinterpret_cast<uintptr_t>(base_);
    uint64_t padding = start - off;
    if (!b->used && padding <= b->bytes && n <= b->bytes - padding) {
      uint64_t used = padding + n;
      if (used > UINT64_MAX - 127)
        return fail();
      used = (used + 127) & ~uint64_t(127);
      if (used > b->bytes) {
        off = b->next;
        continue;
      }
      uint64_t remaining = b->bytes - used;
      if (remaining >= sizeof(Block)) {
        auto *next = new (base_ + off + used) Block{remaining, 0, b->next, false};
        (void)next;
        b->next = off + used;
        b->bytes = used;
      }
      b->used = true;
      b->user_offset = start;
      ++stats_.allocation_count;
      account();
      return base_ + start;
    }
    off = b->next;
  }
  return fail();
}
Status Arena::release(void *p) {
  if (!base_ || !p)
    return Status::Invalid;
  Block *found = nullptr;
  for (uint64_t off = 0; off != end_block;) {
    auto *b = reinterpret_cast<Block *>(base_ + off);
    if (b->used &&
        reinterpret_cast<uintptr_t>(p) == reinterpret_cast<uintptr_t>(base_) + b->user_offset) {
      found = b;
      break;
    }
    off = b->next;
  }
  if (!found)
    return Status::Invalid;
  found->used = false;
  found->user_offset = 0;
  for (uint64_t off = 0; off != end_block;) {
    auto *b = reinterpret_cast<Block *>(base_ + off);
    if (!b->used && b->next != end_block) {
      auto *next = reinterpret_cast<Block *>(base_ + b->next);
      if (!next->used) {
        b->bytes += next->bytes;
        b->next = next->next;
        continue;
      }
    }
    off = b->next;
  }
  account();
  return Status::Ok;
}
