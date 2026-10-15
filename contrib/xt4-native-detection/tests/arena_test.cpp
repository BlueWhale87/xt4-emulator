#include "arena.hpp"
#include "tensor.hpp"
#include "test_support.hpp"
#include <cstdint>
#include <utility>
int main() {
  return test_main([] {
    alignas(128) uint8_t memory[16384 + 128];
    memset(memory, 0xa5, sizeof memory);
    Arena a;
    CHECK(a.init(memory + 1, 1024) == Status::Invalid);
    CHECK(a.init(memory, 64) == Status::Invalid);
    CHECK(a.init(memory, 16384) == Status::Ok);
    auto baseline = a.stats().current_bytes;
    CHECK(a.allocate(UINT64_MAX, 128) == nullptr);
    CHECK(a.allocate(32, 3) == nullptr);
    void *p = a.allocate(100, 128);
    CHECK(p && (reinterpret_cast<uintptr_t>(p) % 128) == 0);
    void *q = a.allocate(200, 256);
    CHECK(q && (reinterpret_cast<uintptr_t>(q) % 256) == 0);
    CHECK(a.release(memory + 16000) == Status::Invalid);
    CHECK(a.release(p) == Status::Ok);
    CHECK(a.release(p) == Status::Invalid);
    CHECK(a.release(q) == Status::Ok);
    CHECK(a.stats().current_bytes == baseline);
    for (int i = 0; i < 1000; i++) {
      Tensor x;
      CHECK(x.create(a, 4, 5, 3) == Status::Ok);
      CHECK(x.data[0] == 0);
      x.data[0] = 42;
      Tensor y = std::move(x);
      CHECK(!x.data && y.data[0] == 42);
      Tensor z;
      z = std::move(y);
      CHECK(!y.data && z.data[0] == 42);
    }
    CHECK(a.stats().current_bytes == baseline);
    {
      Tensor bad;
      CHECK(bad.create(a, UINT32_MAX, UINT32_MAX, UINT32_MAX) != Status::Ok);
    }
    CHECK(a.allocate(20000, 128) == nullptr);
    CHECK(a.stats().failed_allocations >= 3 && a.stats().peak_bytes <= 16384);
    for (size_t i = 16384; i < sizeof memory; i++)
      CHECK(memory[i] == 0xa5);
    // Same block must be reusable after every possible partial prefix fails.
    for (int count = 1; count < 16; count++) {
      void *ptrs[16]{};
      int n = 0;
      for (; n < count; n++) {
        ptrs[n] = a.allocate(1024, 128);
        if (!ptrs[n])
          break;
      }
      while (n)
        CHECK(a.release(ptrs[--n]) == Status::Ok);
      CHECK(a.stats().current_bytes == baseline);
    }
  });
}
