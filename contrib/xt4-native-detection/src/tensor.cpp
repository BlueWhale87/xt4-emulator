#include "tensor.hpp"
#include <cstring>
#include <utility>
Tensor::~Tensor() { reset(); }
Tensor::Tensor(Tensor &&o) noexcept { *this = std::move(o); }
Tensor &Tensor::operator=(Tensor &&o) noexcept {
  if (this != &o) {
    reset();
    rows = o.rows;
    cols = o.cols;
    channels = o.channels;
    channel_step = o.channel_step;
    data = o.data;
    owner_ = o.owner_;
    o.data = nullptr;
    o.owner_ = nullptr;
    o.rows = o.cols = o.channels = o.channel_step = 0;
  }
  return *this;
}
Status Tensor::create(Arena &a, uint32_t r, uint32_t c, uint32_t ch) {
  reset();
  uint64_t step = (uint64_t(ch) + 3) & ~uint64_t(3);
  if (!r || !c || !ch || step > UINT32_MAX)
    return Status::Invalid;
  uint64_t cells = uint64_t(r) * c;
  if (cells > UINT64_MAX / step / sizeof(float))
    return Status::Invalid;
  uint64_t bytes = cells * step * sizeof(float);
  if (bytes > SIZE_MAX)
    return Status::Invalid;
  auto *p = static_cast<float *>(a.allocate(bytes, 128));
  if (!p)
    return Status::NoMemory;
  rows = r;
  cols = c;
  channels = ch;
  channel_step = static_cast<uint32_t>(step);
  data = p;
  owner_ = &a;
  std::memset(data, 0, static_cast<size_t>(bytes));
  return Status::Ok;
}
void Tensor::reset() {
  if (data && owner_)
    owner_->release(data);
  data = nullptr;
  owner_ = nullptr;
  rows = cols = channels = channel_step = 0;
}
