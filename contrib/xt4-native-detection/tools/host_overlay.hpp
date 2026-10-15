#pragma once
#include "xt4_overlay.h"
#include <algorithm>
#include <array>
#include <cmath>
uint32_t host_now(void *, uint64_t *);
struct HostCanvas {
  std::array<uint8_t, 76800> base{}, pixels{};
  xt4_worker *worker = nullptr;
  xt4_overlay_style style{}, saved{};
  uint64_t generation = 1;
  uint32_t display_id = 0, draw_count = 0;
  static uint64_t pool_size(void *, uint32_t n) { return n == 16 ? 512 : 0; }
  static uint32_t init(void *, void *, uint64_t n, uint32_t count) {
    return n == 512 && count == 16 ? 0 : XT4_FACE_INVALID;
  }
  static uint32_t destroy(void *) { return 0; }
  static uint32_t context(void *) { return 0; }
  static uint32_t identity(void *u, uint64_t epoch, uint64_t generation, uint32_t display) {
    auto &c = *static_cast<HostCanvas *>(u);
    xt4_worker_state state{sizeof state, 1, 0, 0, 0, 0};
    return xt4_worker_get_state(c.worker, &state) == 0 && state.mode && state.epoch == epoch &&
                   generation == c.generation && display == c.display_id
               ? 0
               : XT4_FACE_UNAVAILABLE;
  }
  static uint32_t save(void *u) {
    auto &c = *static_cast<HostCanvas *>(u);
    c.saved = c.style;
    return 0;
  }
  static uint32_t set(void *u, const xt4_overlay_style *s) {
    static_cast<HostCanvas *>(u)->style = *s;
    return 0;
  }
  static uint32_t restore(void *u) {
    auto &c = *static_cast<HostCanvas *>(u);
    c.style = c.saved;
    return 0;
  }
  static uint32_t draw(void *u, const xt4_box_edges *b) {
    auto &c = *static_cast<HostCanvas *>(u);
    ++c.draw_count;
    int x0 = std::clamp(int(std::floor(b->x0)), 0, 159),
        y0 = std::clamp(int(std::floor(b->y0)), 0, 159);
    int x1 = std::clamp(int(std::ceil(b->x1)), 0, 160),
        y1 = std::clamp(int(std::ceil(b->y1)), 0, 160);
    for (int y = y0; y < y1; y++)
      for (int x = x0; x < x1; x++)
        if (x < x0 + 2 || x >= x1 - 2 || y < y0 + 2 || y >= y1 - 2) {
          auto i = (y * 160 + x) * 3;
          c.pixels[i] = 0;
          c.pixels[i + 1] = 255;
          c.pixels[i + 2] = 0;
        }
    return 0;
  }
  xt4_overlay_backend backend() {
    return {sizeof(xt4_overlay_backend),
            1,
            this,
            pool_size,
            init,
            destroy,
            context,
            identity,
            save,
            set,
            draw,
            restore,
            host_now,
            nullptr,
            nullptr,
            0};
  }
};
