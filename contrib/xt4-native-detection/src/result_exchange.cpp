#include "math.hpp"
#include "pipeline.hpp"
uint32_t xt4_result_publish(xt4_result_exchange *e, const xt4_result_snapshot *s) {
  if (!s || s->struct_size != sizeof(*s) || s->abi_version != 1 || s->count > 16)
    return XT4_FACE_INVALID;
  for (uint32_t i = 0; i < s->count; i++) {
    auto &b = s->boxes[i];
    auto &r = b.edges;
    if (!xt4_math::isfinite(b.confidence) || !xt4_math::isfinite(r.x0) ||
        !xt4_math::isfinite(r.y0) || !xt4_math::isfinite(r.x1) || !xt4_math::isfinite(r.y1) ||
        r.x0 < 0 || r.y0 < 0 || r.x1 > 160 || r.y1 > 160 || r.x1 <= r.x0 || r.y1 <= r.y0)
      return XT4_FACE_INVALID;
  }
  if (!xt4_pipeline_enter())
    return XT4_FACE_BUSY;
  PipelineRelease release;
  if (!xt4_exchange_live(e))
    return XT4_FACE_INVALID;
  if (!xt4_pipeline_mode.load() || xt4_pipeline_exhausted.load() ||
      s->epoch != xt4_pipeline_epoch.load())
    return XT4_FACE_UNAVAILABLE;
  auto &sync = e->config.sync;
  if (sync.try_enter(sync.user) != 0)
    return XT4_FACE_BUSY;
  e->snapshot = *s;
  e->has_snapshot = true;
  sync.leave(sync.user);
  return s->epoch == xt4_pipeline_epoch.load() && xt4_pipeline_mode.load() != 0
             ? 0
             : XT4_FACE_UNAVAILABLE;
}
uint32_t xt4_result_invalidate(xt4_result_exchange *e) {
  if (!xt4_exchange_live(e))
    return XT4_FACE_INVALID;
  return xt4_pipeline_bump_epoch();
}
uint32_t xt4_result_try_read(xt4_result_exchange *e, uint64_t epoch, uint64_t generation,
                             uint64_t now, uint64_t max_age, xt4_result_snapshot *out) {
  if (!out)
    return XT4_FACE_INVALID;
  *out = {};
  if (!xt4_pipeline_enter())
    return XT4_FACE_BUSY;
  PipelineRelease release;
  if (!xt4_exchange_live(e))
    return XT4_FACE_INVALID;
  if (!xt4_pipeline_mode.load() || xt4_pipeline_exhausted.load() ||
      epoch != xt4_pipeline_epoch.load())
    return XT4_FACE_UNAVAILABLE;
  auto &sync = e->config.sync;
  if (sync.try_enter(sync.user) != 0)
    return XT4_FACE_BUSY;
  xt4_result_snapshot copy = e->snapshot;
  bool has = e->has_snapshot;
  sync.leave(sync.user);
  uint64_t probe = 0;
  if (!e->config.now_ns || e->config.now_ns(e->config.clock_user, &probe) != 0 ||
      (e->has_read_clock && now < e->last_read_ns)) {
    xt4_pipeline_bump_epoch();
    return XT4_FACE_UNAVAILABLE;
  }
  e->last_read_ns = now;
  e->has_read_clock = true;
  if (!has || copy.epoch != epoch || copy.generation != generation || copy.capture_ns > now ||
      now - copy.capture_ns > max_age || epoch != xt4_pipeline_epoch.load() ||
      !xt4_pipeline_mode.load())
    return XT4_FACE_UNAVAILABLE;
  *out = copy;
  return 0;
}
