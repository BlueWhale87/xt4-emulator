#pragma once
#include "pipeline_fixture.hpp"
#include "xt4_overlay.h"
#include <algorithm>
#include <array>
#include <cmath>
struct FakeGraphics {
  std::array<float, 24> state{}, saved{};
  std::array<uint8_t, 76800> base{}, canvas{};
  xt4_worker *worker = nullptr;
  uint32_t fail = 0, draws = 0, saves = 0, restores = 0, display_id = 0;
  uint64_t generation = 1, now = 1000;
  bool context_valid = true, clock_valid = true;
  void *pool = nullptr;
  xt4_overlay_style style{};
  FakeGraphics() {
    for (size_t i = 0; i < state.size(); i++)
      state[i] = float(i) + .25f;
    for (size_t i = 0; i < base.size(); i++)
      base[i] = uint8_t(10 + i % 3 * 10);
    canvas = base;
  }
  static uint64_t size(void *, uint32_t n) { return n == 16 ? 512 : 0; }
  static uint32_t init(void *u, void *p, uint64_t n, uint32_t count) {
    auto &g = *static_cast<FakeGraphics *>(u);
    if (!p || n != 512 || count != 16)
      return XT4_FACE_INVALID;
    g.pool = p;
    return 0;
  }
  static uint32_t destroy(void *u) {
    static_cast<FakeGraphics *>(u)->pool = nullptr;
    return 0;
  }
  static uint32_t context(void *u) {
    return static_cast<FakeGraphics *>(u)->context_valid ? 0 : XT4_FACE_UNAVAILABLE;
  }
  static uint32_t identity(void *u, uint64_t epoch, uint64_t generation, uint32_t display) {
    auto &g = *static_cast<FakeGraphics *>(u);
    xt4_worker_state s{sizeof s, 1, 0, 0, 0, 0};
    return xt4_worker_get_state(g.worker, &s) == 0 && s.mode && s.epoch == epoch &&
                   g.generation == generation && g.display_id == display
               ? 0
               : XT4_FACE_UNAVAILABLE;
  }
  static uint32_t clock(void *u, uint64_t *t) {
    auto &g = *static_cast<FakeGraphics *>(u);
    *t = g.now;
    return g.clock_valid ? 0 : XT4_FACE_UNAVAILABLE;
  }
  static uint32_t save(void *u) {
    auto &g = *static_cast<FakeGraphics *>(u);
    ++g.saves;
    if (g.fail == 1)
      return XT4_FACE_INTERNAL;
    g.saved = g.state;
    return 0;
  }
  static uint32_t set(void *u, const xt4_overlay_style *s) {
    auto &g = *static_cast<FakeGraphics *>(u);
    g.style = *s;
    g.state.fill(-1);
    return g.fail == 2 ? XT4_FACE_INTERNAL : 0;
  }
  static uint32_t draw(void *u, const xt4_box_edges *b) {
    auto &g = *static_cast<FakeGraphics *>(u);
    if (g.fail == 3)
      return XT4_FACE_INTERNAL;
    ++g.draws;
    int x0 = std::clamp(int(std::floor(b->x0)), 0, 159),
        y0 = std::clamp(int(std::floor(b->y0)), 0, 159);
    int x1 = std::clamp(int(std::ceil(b->x1)), 0, 160),
        y1 = std::clamp(int(std::ceil(b->y1)), 0, 160);
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++)
        if (x < x0 + 2 || x >= x1 - 2 || y < y0 + 2 || y >= y1 - 2) {
          auto i = (y * 160 + x) * 3;
          g.canvas[i] = 0;
          g.canvas[i + 1] = 255;
          g.canvas[i + 2] = 0;
        }
    return 0;
  }
  static uint32_t restore(void *u) {
    auto &g = *static_cast<FakeGraphics *>(u);
    ++g.restores;
    if (g.fail == 4)
      return XT4_FACE_INTERNAL;
    g.state = g.saved;
    return 0;
  }
  xt4_overlay_backend backend() {
    return {sizeof(xt4_overlay_backend),
            1,
            this,
            size,
            init,
            destroy,
            context,
            identity,
            save,
            set,
            draw,
            restore,
            clock,
            this,
            nullptr,
            0};
  }
};
inline xt4_display_transform identity_display() {
  xt4_display_transform d{};
  d.struct_size = sizeof d;
  d.abi_version = 1;
  d.generation = 1;
  d.matrix[0] = d.matrix[4] = d.matrix[8] = 1;
  d.viewport = {0, 0, 160, 160};
  return d;
}
struct OverlayFixture {
  PipelineFixture pipeline;
  FakeGraphics graphics;
  xt4_overlay_backend backend;
  xt4_display_transform display = identity_display();
  explicit OverlayFixture(uint64_t max_age_ns = 0) : backend(graphics.backend()) {
    backend.max_result_age_ns = max_age_ns;
    graphics.worker = pipeline.worker;
    CHECK(xt4_worker_enable(pipeline.worker, XT4_MODE_RECORDED) == 0);
    CHECK(xt4_worker_step(pipeline.worker) == 0);
    CHECK(xt4_overlay_init(pipeline.face.ctx, &backend) == 0);
  }
  ~OverlayFixture() {
    graphics.context_valid = true;
    graphics.fail = 0;
    xt4_overlay_destroy(&backend);
  }
  uint32_t draw() {
    return xt4_overlay_draw(pipeline.exchange, &backend, &display, pipeline.epoch(),
                            display.generation);
  }
};
