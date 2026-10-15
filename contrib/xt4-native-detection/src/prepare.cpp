#include "math.hpp"
#include "xt4_preview.h"
#include <algorithm>
#include <cstring>
uint32_t xt4_prepare_bgr160(const xt4_frame_view *v, uint8_t *dst, uint64_t bytes,
                            xt4_prepare_transform *t) {
  if (!t)
    return 1;
  *t = {};
  if (!v || v->struct_size != sizeof(*v) || v->abi_version != 1 || v->format != XT4_BGR8 ||
      !v->data || !dst || bytes < 76800 || !v->width || !v->height ||
      uint64_t(v->width) * 3 > v->stride || !v->source_width || !v->source_height ||
      v->source_x >= v->width || v->source_y >= v->height ||
      v->source_width > v->width - v->source_x || v->source_height > v->height - v->source_y)
    return 1;
  uint64_t extent = uint64_t(v->height - 1) * v->stride + uint64_t(v->width) * 3;
  uintptr_t src_addr = reinterpret_cast<uintptr_t>(v->data),
            dst_addr = reinterpret_cast<uintptr_t>(dst);
  if (extent > v->data_bytes || extent > SIZE_MAX || extent > UINTPTR_MAX - src_addr ||
      76800 > UINTPTR_MAX - dst_addr)
    return 1;
  if (dst_addr < src_addr + extent && src_addr < dst_addr + 76800)
    return 1;
  double scale = std::min(160.0 / v->source_width, 160.0 / v->source_height);
  uint32_t rw =
      std::clamp<uint32_t>(uint32_t(xt4_math::floor(v->source_width * scale + .5)), 1u, 160u);
  uint32_t rh =
      std::clamp<uint32_t>(uint32_t(xt4_math::floor(v->source_height * scale + .5)), 1u, 160u);
  uint32_t left = (160 - rw) / 2, top = (160 - rh) / 2;
  std::memset(dst, 0, 76800);
  for (uint32_t y = 0; y < rh; y++) {
    double sy =
        std::clamp((y + .5) * v->source_height / rh - .5, 0.0, double(v->source_height - 1));
    uint32_t y0 = uint32_t(sy), y1 = std::min(y0 + 1, v->source_height - 1);
    double wy = sy - y0;
    for (uint32_t x = 0; x < rw; x++) {
      double sx =
          std::clamp((x + .5) * v->source_width / rw - .5, 0.0, double(v->source_width - 1));
      uint32_t x0 = uint32_t(sx), x1 = std::min(x0 + 1, v->source_width - 1);
      double wx = sx - x0;
      const uint8_t *a =
          v->data + uint64_t(y0 + v->source_y) * v->stride + uint64_t(x0 + v->source_x) * 3;
      const uint8_t *b =
          v->data + uint64_t(y0 + v->source_y) * v->stride + uint64_t(x1 + v->source_x) * 3;
      const uint8_t *c =
          v->data + uint64_t(y1 + v->source_y) * v->stride + uint64_t(x0 + v->source_x) * 3;
      const uint8_t *d =
          v->data + uint64_t(y1 + v->source_y) * v->stride + uint64_t(x1 + v->source_x) * 3;
      for (uint32_t ch = 0; ch < 3; ch++) {
        double value =
            (a[ch] * (1 - wx) + b[ch] * wx) * (1 - wy) + (c[ch] * (1 - wx) + d[ch] * wx) * wy;
        dst[(uint64_t(y + top) * 160 + x + left) * 3 + ch] =
            uint8_t(std::clamp(xt4_math::floor(value + .5), 0.0, 255.0));
      }
    }
  }
  t->struct_size = sizeof(*t);
  t->abi_version = 1;
  t->source_x = v->source_x;
  t->source_y = v->source_y;
  t->source_width = v->source_width;
  t->source_height = v->source_height;
  t->resized_width = rw;
  t->resized_height = rh;
  t->pad_left = left;
  t->pad_right = 160 - rw - left;
  t->pad_top = top;
  t->pad_bottom = 160 - rh - top;
  t->scale_x = float(rw) / v->source_width;
  t->scale_y = float(rh) / v->source_height;
  t->display_id = v->display_id;
  t->generation = v->generation;
  return 0;
}
