#include "pipeline.hpp"
#include "xt4_overlay.h"
#include <atomic>
#include <new>
struct OverlayState {
  xt4_face_context *ctx;
  xt4_overlay_backend callbacks;
  void *pool;
  bool disabled;
};
static std::atomic<xt4_overlay_backend *> owner{nullptr};
static OverlayState *state = nullptr;
static std::atomic<uint32_t> gate{0};
struct ReleaseOverlay {
  ~ReleaseOverlay() { gate.store(0, std::memory_order_release); }
};
static bool enter() {
  uint32_t x = 0;
  return gate.compare_exchange_strong(x, 1, std::memory_order_acquire);
}
static bool current(xt4_result_exchange *exchange, const xt4_overlay_backend &b, uint64_t epoch,
                    uint64_t gen, uint32_t display) {
  return xt4_exchange_live(exchange) && xt4_pipeline_mode.load() != 0 &&
         !xt4_pipeline_exhausted.load() && epoch == xt4_pipeline_epoch.load() &&
         b.validate_identity(b.user, epoch, gen, display) == 0;
}
uint32_t xt4_overlay_init(xt4_face_context *c, xt4_overlay_backend *b) {
  if (!b || b->struct_size != sizeof(*b) || b->abi_version != 1 || !b->required_pool_bytes ||
      !b->init || !b->destroy || !b->validate_context || !b->validate_identity || !b->save_state ||
      !b->set_style || !b->draw_rect || !b->restore_state)
    return XT4_FACE_INVALID;
  if (!enter())
    return XT4_FACE_BUSY;
  ReleaseOverlay release;
  if (owner.load() || b->internal)
    return XT4_FACE_BUSY;
  if (b->validate_context(b->user) != 0)
    return XT4_FACE_UNAVAILABLE;
  uint32_t locked = xt4_context_enter(c);
  if (locked)
    return locked;
  if (c->overlay) {
    xt4_context_leave();
    return XT4_FACE_BUSY;
  }
  uint64_t bytes = b->required_pool_bytes(b->user, 16);
  if (!bytes) {
    xt4_context_leave();
    return XT4_FACE_UNAVAILABLE;
  }
  auto *storage = c->arena.allocate(sizeof(OverlayState), 128);
  auto *pool = storage ? c->arena.allocate(bytes, 128) : nullptr;
  if (!storage || !pool) {
    if (storage)
      c->arena.release(storage);
    xt4_context_leave();
    return XT4_FACE_NO_MEMORY;
  }
  auto *s = new (storage) OverlayState{c, *b, pool, false};
  uint32_t initialized = b->init(b->user, pool, bytes, 16);
  if (initialized) {
    s->~OverlayState();
    c->arena.release(pool);
    c->arena.release(storage);
    xt4_context_leave();
    return initialized;
  }
  state = s;
  b->internal = s;
  c->overlay = s;
  owner.store(b, std::memory_order_release);
  xt4_context_leave();
  return 0;
}
uint32_t xt4_overlay_draw(xt4_result_exchange *exchange, xt4_overlay_backend *b,
                          const xt4_display_transform *display, uint64_t epoch, uint64_t gen) {
  if (!b || !display)
    return XT4_FACE_INVALID;
  if (!enter())
    return XT4_FACE_BUSY;
  ReleaseOverlay release;
  if (owner.load(std::memory_order_acquire) != b || !state)
    return XT4_FACE_UNAVAILABLE;
  auto &s = *state;
  auto &cb = s.callbacks;
  if (s.disabled || cb.validate_context(cb.user) != 0 ||
      !current(exchange, cb, epoch, gen, display->display_id))
    return XT4_FACE_UNAVAILABLE;
  uint64_t now = 0;
  if (!cb.now_ns || cb.now_ns(cb.clock_user, &now) != 0) {
    xt4_result_invalidate(exchange);
    return XT4_FACE_UNAVAILABLE;
  }
  xt4_result_snapshot snapshot{};
  uint64_t max_age = cb.max_result_age_ns ? cb.max_result_age_ns : 250000000;
  uint32_t read = xt4_result_try_read(exchange, epoch, gen, now, max_age, &snapshot);
  if (read)
    return read;
  if (snapshot.display_id != display->display_id || display->generation != gen)
    return XT4_FACE_UNAVAILABLE;
  xt4_box_edges full{0, 0, 160, 160}, mapped{};
  if (xt4_map_box(&full, &snapshot.prepare, display, &mapped) != 0)
    return XT4_FACE_UNAVAILABLE;
  if (!snapshot.count)
    return 0;
  if (cb.save_state(cb.user) != 0) {
    s.disabled = true;
    xt4_result_invalidate(exchange);
    return XT4_FACE_INTERNAL;
  }
  uint32_t status = 0;
  xt4_overlay_style style{0, 1, 0, 1, 2, 0, 1, display->viewport};
  if (cb.set_style(cb.user, &style) != 0)
    status = XT4_FACE_INTERNAL;
  for (uint32_t i = 0; status == 0 && i < snapshot.count; i++) {
    if (!current(exchange, cb, epoch, gen, display->display_id)) {
      status = XT4_FACE_UNAVAILABLE;
      break;
    }
    if (xt4_map_box(&snapshot.boxes[i].edges, &snapshot.prepare, display, &mapped) != 0)
      continue;
    if (cb.draw_rect(cb.user, &mapped) != 0)
      status = XT4_FACE_INTERNAL;
  }
  if (cb.restore_state(cb.user) != 0)
    status = XT4_FACE_INTERNAL;
  if (status == XT4_FACE_INTERNAL) {
    s.disabled = true;
    xt4_result_invalidate(exchange);
  }
  return status;
}
uint32_t xt4_overlay_destroy(xt4_overlay_backend *b) {
  if (!b)
    return XT4_FACE_INVALID;
  if (!enter())
    return XT4_FACE_BUSY;
  ReleaseOverlay release;
  if (owner.load() != b || !state)
    return XT4_FACE_INVALID;
  auto *s = state;
  auto &cb = s->callbacks;
  if (cb.validate_context(cb.user) != 0)
    return XT4_FACE_UNAVAILABLE;
  auto *c = s->ctx;
  uint32_t locked = xt4_context_enter(c);
  if (locked)
    return locked;
  if (cb.destroy(cb.user) != 0) {
    s->disabled = true;
    xt4_context_leave();
    return XT4_FACE_INTERNAL;
  }
  void *pool = s->pool;
  c->overlay = nullptr;
  b->internal = nullptr;
  owner.store(nullptr, std::memory_order_release);
  state = nullptr;
  s->~OverlayState();
  c->arena.release(pool);
  c->arena.release(s);
  xt4_context_leave();
  return 0;
}
