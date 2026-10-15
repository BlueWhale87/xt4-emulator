#include "context.hpp"
#include "math.hpp"
#include <cstring>
#include <new>
static_assert(std::atomic<uint32_t>::is_always_lock_free, "Nonblocking 32-bit atomic required");
static_assert(std::atomic<xt4_face_context *>::is_always_lock_free,
              "Nonblocking pointer atomic required");
static std::atomic<uint32_t> api_gate{0};
static std::atomic<xt4_face_context *> live_context{nullptr};
static bool enter_gate() {
  uint32_t expected = 0;
  return api_gate.compare_exchange_strong(expected, 1, std::memory_order_acquire);
}
void xt4_context_leave() { api_gate.store(0, std::memory_order_release); }
struct GateRelease {
  ~GateRelease() { xt4_context_leave(); }
};
bool xt4_context_live(xt4_face_context *c) {
  return c && live_context.load(std::memory_order_acquire) == c;
}
uint32_t xt4_context_enter(xt4_face_context *c) {
  if (!enter_gate())
    return XT4_FACE_BUSY;
  if (!xt4_context_live(c)) {
    xt4_context_leave();
    return XT4_FACE_INVALID;
  }
  return 0;
}
uint32_t xt4_face_create(const xt4_face_config *c, void *region, uint64_t bytes,
                         xt4_face_context **out) {
  if (!out)
    return XT4_FACE_INVALID;
  *out = nullptr;
  if (!c || c->struct_size != sizeof(*c) || !region || (reinterpret_cast<uintptr_t>(region) & 127))
    return XT4_FACE_INVALID;
  if (c->abi_version != 1 || std::memcmp(c->model_sha256, XT4_FACE_MODEL_SHA256, 65))
    return XT4_FACE_UNSUPPORTED;
  if (!xt4_math::isfinite(c->confidence_threshold) || c->confidence_threshold < 0 ||
      c->confidence_threshold > 1 || !xt4_math::isfinite(c->nms_threshold) ||
      c->nms_threshold < 0 || c->nms_threshold > 1 || !c->top_k || c->top_k > 1000 ||
      !c->keep_top_k || c->keep_top_k > 16 || c->memory_limit > bytes ||
      c->memory_limit > SIZE_MAX ||
      c->memory_limit > UINTPTR_MAX - reinterpret_cast<uintptr_t>(region))
    return XT4_FACE_INVALID;
  uint64_t prefix = (sizeof(xt4_face_context) + 127) & ~uint64_t(127);
  if (c->memory_limit < prefix + 256)
    return XT4_FACE_NO_MEMORY;
  if (!enter_gate())
    return XT4_FACE_BUSY;
  GateRelease release;
  if (live_context.load(std::memory_order_acquire))
    return XT4_FACE_BUSY;
  auto *ctx = new (region) xt4_face_context;
  ctx->control_bytes = prefix;
  ctx->config = *c;
  Status status =
      ctx->arena.init(static_cast<uint8_t *>(region) + prefix, c->memory_limit - prefix);
  if (status == Status::Ok) {
    ctx->prepared = static_cast<uint8_t *>(ctx->arena.allocate(76800));
    ctx->reserved_results =
        static_cast<xt4_face_box *>(ctx->arena.allocate(16 * sizeof(xt4_face_box)));
    ctx->candidates = static_cast<Candidate *>(ctx->arena.allocate(1000 * sizeof(Candidate)));
    ctx->raw_candidates = static_cast<Candidate *>(ctx->arena.allocate(1000 * sizeof(Candidate)));
    if (!ctx->prepared || !ctx->reserved_results || !ctx->candidates || !ctx->raw_candidates)
      status = Status::NoMemory;
  }
  if (status == Status::Ok)
    status = ctx->engine.init(ctx->arena);
  if (status != Status::Ok) {
    ctx->~xt4_face_context();
    return uint32_t(status);
  }
  live_context.store(ctx, std::memory_order_release);
  *out = ctx;
  return 0;
}
static bool read_clock(xt4_face_context *c, uint64_t &value) {
  return c->config.now_ns && c->config.now_ns(c->config.clock_user, &value) == 0;
}
uint32_t xt4_face_detect(xt4_face_context *c, const xt4_face_image *image, xt4_face_box *out,
                         uint32_t capacity, xt4_face_result *result) {
  if (!result || result->struct_size != sizeof(*result) || result->abi_version != 1)
    return XT4_FACE_INVALID;
  result->count = result->truncated = 0;
  uint32_t entered = xt4_context_enter(c);
  if (entered)
    return entered;
  GateRelease release;
  if (!image || image->struct_size != sizeof(*image))
    return XT4_FACE_INVALID;
  if (image->abi_version != 1 || image->format != XT4_BGR8)
    return XT4_FACE_UNSUPPORTED;
  if (!image->data || image->width != 160 || image->height != 160 || image->stride < 480 ||
      capacity > 16 || (capacity && !out))
    return XT4_FACE_INVALID;
  uint64_t extent = uint64_t(image->height - 1) * image->stride + 480;
  uintptr_t addr = reinterpret_cast<uintptr_t>(image->data),
            output = reinterpret_cast<uintptr_t>(out);
  if (extent > image->data_bytes || extent > SIZE_MAX || extent > UINTPTR_MAX - addr ||
      uint64_t(capacity) * sizeof(*out) > UINTPTR_MAX - output)
    return XT4_FACE_INVALID;
  if (capacity && output < addr + extent && addr < output + uint64_t(capacity) * sizeof(*out))
    return XT4_FACE_INVALID;
  uint64_t start = 0, middle = 0, end = 0;
  bool timing = read_clock(c, start);
  auto status = c->engine.run(image->data, image->stride, c->heads);
  timing = read_clock(c, middle) && timing && middle >= start;
  if (status != Status::Ok) {
    c->heads = {};
    c->clock_available = false;
    return uint32_t(status);
  }
  uint32_t n = c->heads.cls.channels;
  if (n > 1000 || c->heads.obj.channels != n || c->heads.reg.channels != n * 4) {
    c->heads = {};
    return XT4_FACE_INTERNAL;
  }
  for (uint32_t i = 0; i < n; i++) {
    const float *b = c->heads.reg.data + i * 4;
    c->raw_candidates[i] = {
        {{b[0], b[1], b[2], b[3]}, xt4_math::sqrt(c->heads.cls.data[i] * c->heads.obj.data[i])}, i};
  }
  uint32_t selected =
      xt4_select_boxes(c->raw_candidates, n, c->config, c->candidates, out, capacity, *result);
  timing = read_clock(c, end) && timing && end >= middle;
  c->clock_available = timing;
  c->graph_ns = timing ? middle - start : 0;
  c->postprocess_ns = timing ? end - middle : 0;
  return selected;
}
uint32_t xt4_face_get_stats(xt4_face_context *c, xt4_face_stats *s) {
  if (!s || s->struct_size != sizeof(*s) || s->abi_version != 1)
    return XT4_FACE_INVALID;
  auto entered = xt4_context_enter(c);
  if (entered)
    return entered;
  GateRelease release;
  auto a = c->arena.stats();
  s->current_bytes = a.current_bytes + c->control_bytes;
  s->peak_bytes = a.peak_bytes + c->control_bytes;
  s->allocation_count = a.allocation_count;
  s->failed_allocations = a.failed_allocations;
  s->prepared_image_bytes = 76800;
  s->result_storage_bytes = 16 * sizeof(xt4_face_box);
  s->graph_ns = c->graph_ns;
  s->postprocess_ns = c->postprocess_ns;
  s->clock_available = c->clock_available;
  return 0;
}
uint32_t xt4_face_destroy(xt4_face_context *c) {
  auto entered = xt4_context_enter(c);
  if (entered)
    return entered;
  GateRelease release;
  if (c->pipeline || c->overlay)
    return XT4_FACE_BUSY;
  live_context.store(nullptr, std::memory_order_release);
  c->~xt4_face_context();
  return 0;
}
