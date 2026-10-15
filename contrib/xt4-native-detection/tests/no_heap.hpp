#pragma once
#include <atomic>
#include <cstdlib>
#include <new>
static std::atomic<bool> deny_heap{false};
void *operator new(std::size_t n) {
  if (deny_heap.load())
    throw std::bad_alloc();
  if (void *p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void *p) noexcept { ::operator delete(p); }
