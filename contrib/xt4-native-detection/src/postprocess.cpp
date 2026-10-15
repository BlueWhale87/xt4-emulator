#include "context.hpp"
#include "math.hpp"
#include <algorithm>
static bool valid(const Candidate &c, float threshold) {
  const auto &b = c.box.edges;
  return xt4_math::isfinite(c.box.confidence) && c.box.confidence >= threshold &&
         xt4_math::isfinite(b.x0) && xt4_math::isfinite(b.y0) && xt4_math::isfinite(b.x1) &&
         xt4_math::isfinite(b.y1) && b.x1 > b.x0 && b.y1 > b.y0;
}
static bool better(const Candidate &a, const Candidate &b) {
  return a.box.confidence > b.box.confidence ||
         (a.box.confidence == b.box.confidence && a.index < b.index);
}
static float overlap(const xt4_box_edges &a, const xt4_box_edges &b) {
  float w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0),
        h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
  if (w <= 0 || h <= 0)
    return 0;
  float area = w * h, den = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - area;
  return den > 0 ? area / den : 0;
}
uint32_t xt4_select_boxes(const Candidate *input, uint32_t count, const xt4_face_config &cfg,
                          Candidate *work, xt4_face_box *out, uint32_t capacity,
                          xt4_face_result &result) {
  result.count = result.truncated = 0;
  if ((count && !input) || !work || capacity > 16 || (capacity && !out) || !cfg.top_k ||
      cfg.top_k > 1000 || !cfg.keep_top_k || cfg.keep_top_k > 16)
    return XT4_FACE_INVALID;
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; i++) {
    Candidate next = input[i];
    if (!valid(next, cfg.confidence_threshold))
      continue;
    uint32_t at = 0;
    while (at < n && !better(next, work[at]))
      ++at;
    if (at >= cfg.top_k)
      continue;
    uint32_t end = std::min(n, cfg.top_k - 1);
    while (end > at) {
      work[end] = work[end - 1];
      --end;
    }
    work[at] = next;
    if (n < cfg.top_k)
      ++n;
  }
  Candidate retained[16];
  uint32_t kept = 0;
  for (uint32_t i = 0; i < n && kept < cfg.keep_top_k; i++) {
    auto b = work[i].box;
    b.edges.x0 = std::clamp(b.edges.x0, 0.f, 160.f);
    b.edges.x1 = std::clamp(b.edges.x1, 0.f, 160.f);
    b.edges.y0 = std::clamp(b.edges.y0, 0.f, 160.f);
    b.edges.y1 = std::clamp(b.edges.y1, 0.f, 160.f);
    if (b.edges.x1 <= b.edges.x0 || b.edges.y1 <= b.edges.y0)
      continue;
    bool keep = true;
    for (uint32_t k = 0; k < kept; k++)
      if (overlap(work[i].box.edges, retained[k].box.edges) > cfg.nms_threshold) {
        keep = false;
        break;
      }
    if (!keep)
      continue;
    retained[kept++] = work[i];
    if (result.count < capacity)
      out[result.count++] = b;
    else
      result.truncated = 1;
  }
  return 0;
}
