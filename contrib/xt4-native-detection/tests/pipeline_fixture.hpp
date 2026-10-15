#pragma once
#include "face_fixture.hpp"
#include "xt4_pipeline.h"
#include <atomic>
#include <mutex>
struct Provider {
  std::string bytes;
  xt4_frame_view view{};
  uint32_t acquired = 0, released = 0, acquire_status = 0;
  bool short_frame = false;
  Provider() : bytes(read_text(std::string(XT4_SOURCE_DIR) + "/tests/fixtures/astronaut.bgr")) {
    view.struct_size = sizeof view;
    view.abi_version = 1;
    view.format = XT4_BGR8;
    view.data = reinterpret_cast<const uint8_t *>(bytes.data());
    view.data_bytes = bytes.size();
    view.width = view.height = view.source_width = view.source_height = 160;
    view.stride = 480;
    view.generation = 1;
    view.capture_ns = 1000;
  }
  static uint32_t acquire(void *u, xt4_frame_view *out, uint64_t *token) {
    auto &p = *static_cast<Provider *>(u);
    if (p.acquire_status)
      return p.acquire_status;
    ++p.acquired;
    *out = p.view;
    if (p.short_frame)
      out->data_bytes = 1;
    *token = 17;
    return 0;
  }
  static void release(void *u, uint64_t token) {
    auto &p = *static_cast<Provider *>(u);
    CHECK(token == 17);
    ++p.released;
  }
};
struct TryMutex {
  std::mutex lock;
  static uint32_t enter(void *u) {
    return static_cast<TryMutex *>(u)->lock.try_lock() ? 0 : XT4_FACE_BUSY;
  }
  static void leave(void *u) { static_cast<TryMutex *>(u)->lock.unlock(); }
};
inline uint32_t constant_clock(void *, uint64_t *t) {
  *t = 1000;
  return 0;
}
struct PipelineFixture {
  FaceFixture face;
  Provider provider;
  TryMutex sync;
  xt4_worker *worker = nullptr;
  xt4_result_exchange *exchange = nullptr;
  PipelineFixture(xt4_face_config fc = default_config(), bool have_clock = true,
                  xt4_now_ns_fn clock = constant_clock, void *clock_user = nullptr)
      : face(fc) {
    xt4_pipeline_config config{};
    config.struct_size = sizeof config;
    config.abi_version = 1;
    config.mode = XT4_MODE_DISABLED;
    config.provider = {&provider, Provider::acquire, Provider::release};
    config.sync = {&sync, TryMutex::enter, TryMutex::leave};
    config.now_ns = have_clock ? clock : nullptr;
    config.clock_user = clock_user;
    CHECK(xt4_pipeline_init(face.ctx, &config, &worker, &exchange) == 0);
  }
  ~PipelineFixture() {
    if (worker)
      xt4_pipeline_destroy(worker);
  }
  uint64_t epoch() {
    xt4_worker_state s{sizeof s, 1, 0, 0, 0, 0};
    CHECK(xt4_worker_get_state(worker, &s) == 0);
    return s.epoch;
  }
};
