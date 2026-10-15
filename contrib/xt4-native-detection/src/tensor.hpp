#pragma once
#include "arena.hpp"
class Tensor {
public:
  uint32_t rows = 0, cols = 0, channels = 0, channel_step = 0;
  float *data = nullptr;
  Tensor() = default;
  ~Tensor();
  Tensor(const Tensor &) = delete;
  Tensor &operator=(const Tensor &) = delete;
  Tensor(Tensor &&) noexcept;
  Tensor &operator=(Tensor &&) noexcept;
  Status create(Arena &, uint32_t, uint32_t, uint32_t);
  float *ptr(uint32_t r, uint32_t c) {
    return data && r < rows && c < cols ? data + (uint64_t(r) * cols + c) * channel_step : nullptr;
  }
  const float *ptr(uint32_t r, uint32_t c) const {
    return data && r < rows && c < cols ? data + (uint64_t(r) * cols + c) * channel_step : nullptr;
  }
  void reset();

private:
  Arena *owner_ = nullptr;
};
