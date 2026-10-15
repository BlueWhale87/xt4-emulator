#pragma once
#include <cstddef>
#include <cstdint>
enum class Status : uint32_t { Ok, Invalid, Unsupported, NoMemory, Busy, Unavailable, Internal };
struct AllocationStats {
  uint64_t current_bytes = 0, peak_bytes = 0, allocation_count = 0, failed_allocations = 0;
};
class Arena {
public:
  Status init(void *, uint64_t);
  void *allocate(uint64_t, uint32_t alignment = 128);
  Status release(void *);
  AllocationStats stats() const { return stats_; }

private:
  struct alignas(128) Block {
    uint64_t bytes;
    uint64_t user_offset;
    uint64_t next;
    bool used;
  };
  uint8_t *base_ = nullptr;
  uint64_t bytes_ = 0;
  AllocationStats stats_{};
  void account();
};
