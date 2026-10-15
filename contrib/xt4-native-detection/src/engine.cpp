#include "engine.hpp"
#include "math.hpp"
#include <algorithm>
#include <cstring>
#include <utility>
Status Engine::init(Arena &a) {
  if (arena_)
    return Status::Busy;
  arena_ = &a;
  status_ = Status::Ok;
  for (uint32_t i = 0; i < 53; i++) {
    const auto &p = param_pConvInfo[i];
    auto &f = filters_[i];
    if (p.channels <= 0 || p.num_filters <= 0 || p.is_depthwise == p.is_pointwise ||
        (p.is_depthwise && p.num_filters != p.channels)) {
      shutdown();
      return Status::Internal;
    }
    f.channels = uint32_t(p.channels);
    f.count = uint32_t(p.num_filters);
    f.depthwise = p.is_depthwise;
    Status s = f.weights.create(a, 1, f.depthwise ? 9 : f.count, f.channels);
    if (s == Status::Ok)
      s = f.biases.create(a, 1, 1, f.count);
    if (s != Status::Ok) {
      shutdown();
      return s;
    }
    for (uint32_t j = 0; j < f.weights.cols; j++)
      std::memcpy(f.weights.ptr(0, j), p.pWeights + uint64_t(j) * f.channels,
                  f.channels * sizeof(float));
    std::memcpy(f.biases.data, p.pBiases, f.count * sizeof(float));
  }
  return Status::Ok;
}
Status Engine::shutdown() {
  for (auto &f : filters_) {
    f.weights.reset();
    f.biases.reset();
    f.channels = f.count = 0;
  }
  arena_ = nullptr;
  status_ = Status::Unavailable;
  return Status::Ok;
}
Tensor Engine::make(uint32_t r, uint32_t c, uint32_t ch) {
  Tensor t;
  if (status_ != Status::Ok || !arena_)
    return t;
  status_ = t.create(*arena_, r, c, ch);
  return t;
}
Tensor Engine::convert(const uint8_t *bgr, uint32_t stride) {
  auto out = make(80, 80, 32);
  if (status_ != Status::Ok)
    return out;
  for (int r = 0; r < 80; r++)
    for (int c = 0; c < 80; c++) {
      float *p = out.ptr(r, c);
      for (int fy = -1; fy <= 1; fy++)
        for (int fx = -1; fx <= 1; fx++) {
          int y = r * 2 + fy, x = c * 2 + fx;
          if (y < 0 || y >= 160 || x < 0 || x >= 160)
            continue;
          const uint8_t *src = bgr + uint64_t(y) * stride + x * 3;
          uint32_t ch = uint32_t((fy + 1) * 3 + fx + 1) * 3;
          for (int k = 0; k < 3; k++)
            p[ch + k] = src[k];
        }
    }
  return out;
}
Tensor Engine::conv(const Tensor &in, uint32_t index, bool relu) {
  if (status_ != Status::Ok)
    return {};
  if (index >= 53 || !in.data || in.channels != filters_[index].channels) {
    status_ = Status::Internal;
    return {};
  }
  auto &f = filters_[index];
  auto out = make(in.rows, in.cols, f.count);
  if (status_ != Status::Ok)
    return out;
  for (uint32_t r = 0; r < in.rows; r++)
    for (uint32_t c = 0; c < in.cols; c++) {
      float *p = out.ptr(r, c);
      if (!f.depthwise) {
        auto *input = in.ptr(r, c);
        for (uint32_t ch = 0; ch < f.count; ch++) {
          const float *w = f.weights.ptr(0, ch);
          float sum = 0;
          for (uint32_t k = 0; k < in.channels; k++)
            sum += input[k] * w[k];
          p[ch] = sum + f.biases.data[ch];
        }
      } else {
        for (int y = std::max(0, int(r) - 1); y < std::min(int(in.rows), int(r) + 2); y++)
          for (int x = std::max(0, int(c) - 1); x < std::min(int(in.cols), int(c) + 2); x++) {
            const float *input = in.ptr(y, x),
                        *w = f.weights.ptr(0, uint32_t((y - int(r) + 1) * 3 + x - int(c) + 1));
            for (uint32_t ch = 0; ch < f.count; ch++)
              p[ch] += input[ch] * w[ch];
          }
        for (uint32_t ch = 0; ch < f.count; ch++)
          p[ch] += f.biases.data[ch];
      }
    }
  if (relu)
    for (uint64_t i = 0; i < uint64_t(out.rows) * out.cols * out.channel_step; i++)
      out.data[i] *= (out.data[i] > 0);
  return out;
}
Tensor Engine::dp(const Tensor &in, uint32_t p, uint32_t d, bool relu) {
  auto t = conv(in, p, false);
  return conv(t, d, relu);
}
Tensor Engine::unit(const Tensor &in, uint32_t p1, uint32_t d1, uint32_t p2, uint32_t d2) {
  auto t = dp(in, p1, d1);
  return dp(t, p2, d2);
}
Tensor Engine::pool(const Tensor &in) {
  if (status_ != Status::Ok)
    return {};
  auto out = make(in.rows / 2, in.cols / 2, in.channels);
  if (status_ != Status::Ok)
    return out;
  for (uint32_t r = 0; r < out.rows; r++)
    for (uint32_t c = 0; c < out.cols; c++)
      for (uint32_t ch = 0; ch < in.channels; ch++) {
        float v = in.ptr(r * 2, c * 2)[ch];
        for (uint32_t dy = 0; dy < 2; dy++)
          for (uint32_t dx = 0; dx < 2; dx++) {
            float x = in.ptr(r * 2 + dy, c * 2 + dx)[ch];
            v = v > x ? v : x;
          }
        out.ptr(r, c)[ch] = v;
      }
  return out;
}
Tensor Engine::upsample(const Tensor &in) {
  if (status_ != Status::Ok)
    return {};
  auto out = make(in.rows * 2, in.cols * 2, in.channels);
  if (status_ != Status::Ok)
    return out;
  for (uint32_t r = 0; r < out.rows; r++)
    for (uint32_t c = 0; c < out.cols; c++)
      std::memcpy(out.ptr(r, c), in.ptr(r / 2, c / 2), in.channels * sizeof(float));
  return out;
}
Tensor Engine::add(const Tensor &a, const Tensor &b) {
  if (status_ != Status::Ok)
    return {};
  if (!a.data || !b.data || a.rows != b.rows || a.cols != b.cols || a.channels != b.channels) {
    status_ = Status::Internal;
    return {};
  }
  auto out = make(a.rows, a.cols, a.channels);
  if (status_ != Status::Ok)
    return out;
  for (uint32_t r = 0; r < a.rows; r++)
    for (uint32_t c = 0; c < a.cols; c++)
      for (uint32_t ch = 0; ch < a.channels; ch++)
        out.ptr(r, c)[ch] = a.ptr(r, c)[ch] + b.ptr(r, c)[ch];
  return out;
}
void Engine::decode(Tensor &t, uint32_t stride, bool landmarks) {
  if (status_ != Status::Ok)
    return;
  for (uint32_t r = 0; r < t.rows; r++)
    for (uint32_t c = 0; c < t.cols; c++) {
      float *p = t.ptr(r, c), px = float(c * stride), py = float(r * stride), s = float(stride);
      if (landmarks) {
        for (uint32_t k = 0; k < 5; k++) {
          p[2 * k] = p[2 * k] * s + px;
          p[2 * k + 1] = p[2 * k + 1] * s + py;
        }
      } else {
        float cx = p[0] * s + px, cy = p[1] * s + py, w = xt4_math::exp(p[2]) * s,
              h = xt4_math::exp(p[3]) * s;
        p[0] = cx - w / 2.f;
        p[1] = cy - h / 2.f;
        p[2] = cx + w / 2.f;
        p[3] = cy + h / 2.f;
      }
    }
}
Tensor Engine::flatten3(Tensor (&in)[3]) {
  if (status_ != Status::Ok)
    return {};
  uint32_t channels = 0;
  for (auto &t : in)
    channels += t.rows * t.cols * t.channels;
  auto out = make(1, 1, channels);
  if (status_ != Status::Ok)
    return out;
  float *p = out.data;
  for (auto &t : in)
    for (uint32_t r = 0; r < t.rows; r++)
      for (uint32_t c = 0; c < t.cols; c++) {
        std::memcpy(p, t.ptr(r, c), t.channels * sizeof(float));
        p += t.channels;
      }
  return out;
}
void Engine::sigmoid(Tensor &t) {
  if (status_ != Status::Ok)
    return;
  for (uint32_t i = 0; i < t.channels; i++) {
    float v = std::min(t.data[i], 88.3762626647949f);
    v = std::max(v, -88.3762626647949f);
    t.data[i] = 1.f / (1.f + xt4_math::exp(-v));
  }
}
Status Engine::run(const uint8_t *bgr, uint32_t stride, RawHeads &out, TensorTrace trace,
                   void *user) {
  out = {};
  if (!arena_ || status_ == Status::Unavailable)
    return Status::Unavailable;
  if (!bgr || stride < 480)
    return Status::Invalid;
  status_ = Status::Ok;
  auto emit = [&](const char *name, const Tensor &t) {
    if (trace && status_ == Status::Ok)
      trace(user, name, t);
  };
  auto fx = convert(bgr, stride);
  fx = conv(fx, 0);
  emit("early", fx);
  fx = dp(fx, 1, 2);
  fx = pool(fx);
  fx = unit(fx, 3, 4, 5, 6);
  fx = unit(fx, 7, 8, 9, 10);
  fx = pool(fx);
  auto fb1 = unit(fx, 11, 12, 13, 14);
  emit("middle", fb1);
  fx = pool(fb1);
  auto fb2 = unit(fx, 15, 16, 17, 18);
  fx = pool(fb2);
  auto fb3 = unit(fx, 19, 20, 21, 22);
  Tensor cls[3], reg[3], kps[3], obj[3];
  fb3 = dp(fb3, 27, 28);
  cls[2] = dp(fb3, 33, 34, false);
  reg[2] = dp(fb3, 39, 40, false);
  kps[2] = dp(fb3, 51, 52, false);
  obj[2] = dp(fb3, 45, 46, false);
  fb2 = add(upsample(fb3), fb2);
  fb2 = dp(fb2, 25, 26);
  cls[1] = dp(fb2, 31, 32, false);
  reg[1] = dp(fb2, 37, 38, false);
  kps[1] = dp(fb2, 49, 50, false);
  obj[1] = dp(fb2, 43, 44, false);
  fb1 = add(upsample(fb2), fb1);
  fb1 = dp(fb1, 23, 24);
  cls[0] = dp(fb1, 29, 30, false);
  reg[0] = dp(fb1, 35, 36, false);
  kps[0] = dp(fb1, 47, 48, false);
  obj[0] = dp(fb1, 41, 42, false);
  for (uint32_t i = 0; i < 3; i++) {
    decode(reg[i], 8u << i, false);
    decode(kps[i], 8u << i, true);
  }
  out.cls = flatten3(cls);
  out.reg = flatten3(reg);
  out.kps = flatten3(kps);
  out.obj = flatten3(obj);
  sigmoid(out.cls);
  sigmoid(out.obj);
  emit("cls", out.cls);
  emit("reg", out.reg);
  emit("kps", out.kps);
  emit("obj", out.obj);
  if (status_ != Status::Ok)
    out = {};
  return status_;
}
