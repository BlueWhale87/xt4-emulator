#pragma once
#include "tensor.hpp"
struct ConvInfoStruct {
  int channels, num_filters;
  bool is_depthwise, is_pointwise, with_relu;
  const float *pWeights;
  const float *pBiases;
};
extern const ConvInfoStruct param_pConvInfo[53];
struct RawHeads {
  Tensor cls, reg, kps, obj;
};
using TensorTrace = void (*)(void *, const char *, const Tensor &);
class Engine {
public:
  Status init(Arena &);
  Status run(const uint8_t *, uint32_t, RawHeads &, TensorTrace trace = nullptr,
             void *user = nullptr);
  Status shutdown();
  ~Engine() { shutdown(); }

private:
  struct Filter {
    Tensor weights, biases;
    uint32_t channels = 0, count = 0;
    bool depthwise = false;
  };
  Filter filters_[53];
  Arena *arena_ = nullptr;
  Status status_ = Status::Unavailable;
  Tensor make(uint32_t, uint32_t, uint32_t);
  Tensor convert(const uint8_t *, uint32_t);
  Tensor conv(const Tensor &, uint32_t, bool = true);
  Tensor dp(const Tensor &, uint32_t, uint32_t, bool = true);
  Tensor unit(const Tensor &, uint32_t, uint32_t, uint32_t, uint32_t);
  Tensor pool(const Tensor &);
  Tensor upsample(const Tensor &);
  Tensor add(const Tensor &, const Tensor &);
  Tensor flatten3(Tensor (&)[3]);
  void decode(Tensor &, uint32_t, bool);
  void sigmoid(Tensor &);
};
