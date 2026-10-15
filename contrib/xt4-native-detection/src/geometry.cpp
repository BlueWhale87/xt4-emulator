#include "math.hpp"
#include "xt4_preview.h"
#include <algorithm>
static bool finite_box(const xt4_box_edges &b) {
  return xt4_math::isfinite(b.x0) && xt4_math::isfinite(b.y0) && xt4_math::isfinite(b.x1) &&
         xt4_math::isfinite(b.y1) && b.x1 > b.x0 && b.y1 > b.y0;
}
uint32_t xt4_map_box(const xt4_box_edges *b, const xt4_prepare_transform *p,
                     const xt4_display_transform *d, xt4_box_edges *out) {
  if (!out)
    return 1;
  *out = {};
  if (!b || !p || !d || p->struct_size != sizeof(*p) || p->abi_version != 1 ||
      d->struct_size != sizeof(*d) || d->abi_version != 1 || p->generation != d->generation ||
      p->display_id != d->display_id || !finite_box(*b) || !finite_box(d->viewport) ||
      !xt4_math::isfinite(p->scale_x) || !xt4_math::isfinite(p->scale_y) || p->scale_x <= 0 ||
      p->scale_y <= 0 || !p->source_width || !p->source_height || !p->resized_width ||
      !p->resized_height || uint64_t(p->pad_left) + p->pad_right + p->resized_width != 160 ||
      uint64_t(p->pad_top) + p->pad_bottom + p->resized_height != 160)
    return 1;
  for (float x : d->matrix)
    if (!xt4_math::isfinite(x))
      return 1;
  const float *m = d->matrix;
  if (m[6] != 0 || m[7] != 0 || m[8] != 1 || double(m[0]) * m[4] - double(m[1]) * m[3] == 0)
    return 1;
  double x0 = std::max(double(b->x0), double(p->pad_left)),
         x1 = std::min(double(b->x1), double(p->pad_left + p->resized_width));
  double y0 = std::max(double(b->y0), double(p->pad_top)),
         y1 = std::min(double(b->y1), double(p->pad_top + p->resized_height));
  if (x1 <= x0 || y1 <= y0)
    return 1;
  x0 = (x0 - p->pad_left) / p->scale_x + p->source_x;
  x1 = (x1 - p->pad_left) / p->scale_x + p->source_x;
  y0 = (y0 - p->pad_top) / p->scale_y + p->source_y;
  y1 = (y1 - p->pad_top) / p->scale_y + p->source_y;
  double lo_x = __builtin_inf(), lo_y = __builtin_inf(), hi_x = -__builtin_inf(),
         hi_y = -__builtin_inf();
  for (double x : {x0, x1})
    for (double y : {y0, y1}) {
      double xx = m[0] * x + m[1] * y + m[2], yy = m[3] * x + m[4] * y + m[5];
      lo_x = std::min(lo_x, xx);
      hi_x = std::max(hi_x, xx);
      lo_y = std::min(lo_y, yy);
      hi_y = std::max(hi_y, yy);
    }
  lo_x = std::max(lo_x, double(d->viewport.x0));
  lo_y = std::max(lo_y, double(d->viewport.y0));
  hi_x = std::min(hi_x, double(d->viewport.x1));
  hi_y = std::min(hi_y, double(d->viewport.y1));
  if (hi_x <= lo_x || hi_y <= lo_y)
    return 1;
  *out = {float(lo_x), float(lo_y), float(hi_x), float(hi_y)};
  if (!finite_box(*out)) {
    *out = {};
    return 1;
  }
  return 0;
}
