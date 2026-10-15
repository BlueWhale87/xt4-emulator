#include "pipeline.hpp"
#include <new>
std::atomic<xt4_worker *> xt4_live_worker{nullptr};
std::atomic<xt4_result_exchange *> xt4_live_exchange{nullptr};
std::atomic<uint32_t> xt4_pipeline_mode{0}, xt4_pipeline_epoch{1};
std::atomic<uint32_t> xt4_pipeline_exhausted{0};
static std::atomic<uint32_t> gate{0};
bool xt4_pipeline_enter() {
  uint32_t x = 0;
  return gate.compare_exchange_strong(x, 1, std::memory_order_acquire);
}
void xt4_pipeline_leave() { gate.store(0, std::memory_order_release); }
bool xt4_exchange_live(xt4_result_exchange *e) {
  return e && xt4_live_exchange.load(std::memory_order_acquire) == e;
}
uint32_t xt4_pipeline_bump_epoch() {
  for (uint32_t i = 0; i < 4; i++) {
    uint32_t old = xt4_pipeline_epoch.load(std::memory_order_acquire);
    if (old == UINT32_MAX)
      break;
    if (xt4_pipeline_epoch.compare_exchange_strong(old, old + 1, std::memory_order_acq_rel))
      return 0;
  }
  xt4_pipeline_exhausted.store(1, std::memory_order_release);
  xt4_pipeline_mode.store(0, std::memory_order_release);
  return XT4_FACE_UNAVAILABLE;
}
uint32_t xt4_pipeline_init(xt4_face_context *c, const xt4_pipeline_config *config,
                           xt4_worker **worker, xt4_result_exchange **exchange) {
  if (worker)
    *worker = nullptr;
  if (exchange)
    *exchange = nullptr;
  if (!config || !worker || !exchange || config->struct_size != sizeof(*config) ||
      config->abi_version != 1 || config->mode != XT4_MODE_DISABLED || !config->sync.try_enter ||
      !config->sync.leave || !config->provider.try_acquire || !config->provider.release)
    return XT4_FACE_INVALID;
  if (!xt4_pipeline_enter())
    return XT4_FACE_BUSY;
  PipelineRelease release;
  uint32_t locked = xt4_context_enter(c);
  if (locked)
    return locked;
  if (c->pipeline || xt4_live_worker.load()) {
    xt4_context_leave();
    return XT4_FACE_BUSY;
  }
  auto *storage = c->arena.allocate(sizeof(xt4_worker), 128);
  if (!storage) {
    xt4_context_leave();
    return XT4_FACE_NO_MEMORY;
  }
  auto *w = new (storage) xt4_worker;
  w->ctx = c;
  w->exchange.config = *config;
  if (config->synthetic) {
    if (config->synthetic->struct_size != sizeof(xt4_result_snapshot) ||
        config->synthetic->abi_version != 1 || config->synthetic->count > 16) {
      w->~xt4_worker();
      c->arena.release(storage);
      xt4_context_leave();
      return XT4_FACE_INVALID;
    }
    w->synthetic = *config->synthetic;
    w->has_synthetic = true;
  }
  w->exchange.config.synthetic = nullptr;
  c->pipeline = w;
  xt4_pipeline_epoch.store(1);
  xt4_pipeline_mode.store(0);
  xt4_pipeline_exhausted.store(0);
  xt4_live_worker.store(w, std::memory_order_release);
  xt4_live_exchange.store(&w->exchange, std::memory_order_release);
  *worker = w;
  *exchange = &w->exchange;
  xt4_context_leave();
  return 0;
}
uint32_t xt4_pipeline_destroy(xt4_worker *w) {
  if (!xt4_pipeline_enter())
    return XT4_FACE_BUSY;
  PipelineRelease release;
  if (!w || xt4_live_worker.load() != w)
    return XT4_FACE_INVALID;
  if (w->running.load())
    return XT4_FACE_BUSY;
  auto *c = w->ctx;
  uint32_t locked = xt4_context_enter(c);
  if (locked)
    return locked;
  if (c->overlay) {
    xt4_context_leave();
    return XT4_FACE_BUSY;
  }
  xt4_pipeline_mode.store(0);
  xt4_pipeline_bump_epoch();
  xt4_live_exchange.store(nullptr, std::memory_order_release);
  xt4_live_worker.store(nullptr, std::memory_order_release);
  c->pipeline = nullptr;
  w->~xt4_worker();
  auto status = c->arena.release(w);
  xt4_context_leave();
  return uint32_t(status);
}
uint32_t xt4_worker_enable(xt4_worker *w, uint32_t mode) {
  if (!xt4_pipeline_enter())
    return XT4_FACE_BUSY;
  PipelineRelease release;
  if (!w || xt4_live_worker.load() != w)
    return XT4_FACE_INVALID;
  if (mode == XT4_MODE_VERIFIED_PREVIEW)
    return XT4_FACE_UNAVAILABLE;
  if (mode != XT4_MODE_RECORDED && mode != XT4_MODE_SYNTHETIC)
    return XT4_FACE_INVALID;
  if (w->faulted.load() || xt4_pipeline_exhausted.load() ||
      (mode == XT4_MODE_SYNTHETIC && !w->has_synthetic))
    return XT4_FACE_UNAVAILABLE;
  uint32_t changed = xt4_pipeline_bump_epoch();
  if (changed)
    return changed;
  xt4_pipeline_mode.store(mode, std::memory_order_release);
  return 0;
}
uint32_t xt4_worker_disable(xt4_worker *w) {
  // No worker memory access: disabling must also work while a reader holds the snapshot lock.
  if (!w || xt4_live_worker.load(std::memory_order_acquire) != w)
    return XT4_FACE_INVALID;
  xt4_pipeline_mode.store(0, std::memory_order_release);
  return xt4_pipeline_bump_epoch();
}
uint32_t xt4_worker_get_state(xt4_worker *w, xt4_worker_state *out) {
  if (!out || out->struct_size != sizeof(*out) || out->abi_version != 1)
    return XT4_FACE_INVALID;
  if (!xt4_pipeline_enter())
    return XT4_FACE_BUSY;
  PipelineRelease release;
  if (!w || xt4_live_worker.load() != w)
    return XT4_FACE_INVALID;
  out->mode = xt4_pipeline_mode.load();
  out->epoch = xt4_pipeline_epoch.load();
  out->running = w->running.load();
  out->dropped_samples = w->dropped.load();
  return 0;
}
uint32_t xt4_worker_get_timings(xt4_worker *w, xt4_worker_timings *out) {
  if (!out || out->struct_size != sizeof(*out) || out->abi_version != 1)
    return XT4_FACE_INVALID;
  out->clock_available = 0;
  out->prepare_ns = out->publication_ns = 0;
  if (!xt4_pipeline_enter())
    return XT4_FACE_BUSY;
  PipelineRelease release;
  if (!w || xt4_live_worker.load() != w)
    return XT4_FACE_INVALID;
  if (w->running.load(std::memory_order_acquire))
    return XT4_FACE_BUSY;
  out->prepare_ns = w->prepare_ns;
  out->publication_ns = w->publication_ns;
  out->clock_available = w->timing_available;
  return 0;
}
uint32_t xt4_worker_step(xt4_worker *w) {
  if (!xt4_pipeline_enter())
    return XT4_FACE_BUSY;
  if (!w || xt4_live_worker.load() != w) {
    xt4_pipeline_leave();
    return XT4_FACE_INVALID;
  }
  if (w->running.load()) {
    ++w->dropped;
    xt4_pipeline_leave();
    return XT4_FACE_BUSY;
  }
  uint32_t mode = xt4_pipeline_mode.load(), epoch = xt4_pipeline_epoch.load();
  if (!mode || w->faulted.load() || xt4_pipeline_exhausted.load()) {
    xt4_pipeline_leave();
    return XT4_FACE_UNAVAILABLE;
  }
  w->running.store(1, std::memory_order_release);
  xt4_pipeline_leave();
  struct RunningRelease {
    xt4_worker *w;
    ~RunningRelease() { w->running.store(0, std::memory_order_release); }
  } running{w};
  w->prepare_ns = w->publication_ns = 0;
  w->timing_available = false;
  auto &cfg = w->exchange.config;
  uint64_t now = 0;
  if (!cfg.now_ns || cfg.now_ns(cfg.clock_user, &now) != 0) {
    xt4_result_invalidate(&w->exchange);
    return XT4_FACE_UNAVAILABLE;
  }
  uint64_t last_stage_ns = now;
  auto publish = [&](const xt4_result_snapshot &s) {
    uint64_t start = 0, end = 0;
    if (cfg.now_ns(cfg.clock_user, &start) != 0 || start < last_stage_ns) {
      xt4_result_invalidate(&w->exchange);
      return uint32_t(XT4_FACE_UNAVAILABLE);
    }
    uint32_t status = xt4_result_publish(&w->exchange, &s);
    if (cfg.now_ns(cfg.clock_user, &end) != 0 || end < start) {
      xt4_result_invalidate(&w->exchange);
      return uint32_t(XT4_FACE_UNAVAILABLE);
    }
    w->publication_ns = end - start;
    w->timing_available = true;
    return status;
  };
  xt4_result_snapshot snapshot{};
  snapshot.struct_size = sizeof snapshot;
  snapshot.abi_version = 1;
  snapshot.epoch = epoch;
  if (mode == XT4_MODE_SYNTHETIC) {
    snapshot = w->synthetic;
    snapshot.epoch = epoch;
    snapshot.capture_ns = now;
    return publish(snapshot);
  }
  xt4_frame_view frame{};
  uint64_t lease = 0;
  uint32_t acquired = cfg.provider.try_acquire(cfg.provider.user, &frame, &lease);
  if (acquired == XT4_FACE_BUSY) {
    ++w->dropped;
    return acquired;
  }
  if (acquired) {
    xt4_result_invalidate(&w->exchange);
    if (acquired == XT4_FACE_INTERNAL || acquired == XT4_FACE_NO_MEMORY) {
      w->faulted.store(1);
      xt4_pipeline_mode.store(0);
    }
    return acquired;
  }
  uint32_t prepared = xt4_prepare_bgr160(&frame, w->ctx->prepared, 76800, &snapshot.prepare);
  cfg.provider.release(cfg.provider.user, lease);
  if (prepared) {
    xt4_result_invalidate(&w->exchange);
    return XT4_FACE_INVALID;
  }
  uint64_t prepared_at = 0;
  if (cfg.now_ns(cfg.clock_user, &prepared_at) != 0 || prepared_at < now) {
    xt4_result_invalidate(&w->exchange);
    return XT4_FACE_UNAVAILABLE;
  }
  w->prepare_ns = prepared_at - now;
  last_stage_ns = prepared_at;
  if (xt4_pipeline_epoch.load() != epoch || xt4_pipeline_mode.load() == 0)
    return XT4_FACE_UNAVAILABLE;
  xt4_face_image image{sizeof image, 1, XT4_BGR8, w->ctx->prepared, 76800, 160, 160, 480};
  xt4_face_result result{sizeof result, 1, 0, 0};
  uint32_t detected = xt4_face_detect(w->ctx, &image, snapshot.boxes, 16, &result);
  if (detected) {
    if (detected == XT4_FACE_BUSY) {
      ++w->dropped;
      return detected;
    }
    xt4_result_invalidate(&w->exchange);
    if (detected == XT4_FACE_INTERNAL || detected == XT4_FACE_NO_MEMORY) {
      w->faulted.store(1);
      xt4_pipeline_mode.store(0);
    }
    return detected;
  }
  snapshot.count = result.count;
  snapshot.capture_ns = frame.capture_ns;
  snapshot.generation = frame.generation;
  snapshot.display_id = frame.display_id;
  return publish(snapshot);
}
