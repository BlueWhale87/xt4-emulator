#include "no_heap.hpp"
#include "overlay_fixture.hpp"
int main() {
  return test_main([] {
    {
      OverlayFixture f;
      auto before = f.graphics.state;
      deny_heap = true;
      auto status = f.draw();
      deny_heap = false;
      CHECK(status == 0);
      CHECK(f.graphics.draws == 1 && f.graphics.state == before &&
            f.graphics.canvas != f.graphics.base);
      CHECK(f.graphics.style.red == 0 && f.graphics.style.green == 1 &&
            f.graphics.style.blue == 0 && f.graphics.style.alpha == 1 &&
            f.graphics.style.stroke_width == 2 && f.graphics.style.filled == 0 &&
            f.graphics.style.identity_transform == 1);
      auto draws = f.graphics.draws;
      f.graphics.context_valid = false;
      CHECK(f.draw() == XT4_FACE_UNAVAILABLE && f.graphics.draws == draws);
      f.graphics.context_valid = true;
      std::ofstream capture0(std::string(XT4_BINARY_DIR) + "/fake-display-0.ppm", std::ios::binary);
      capture0 << "P6\n160 160\n255\n";
      capture0.write(reinterpret_cast<const char *>(f.graphics.canvas.data()),
                     f.graphics.canvas.size());
      capture0.close();
      f.pipeline.sync.lock.lock();
      CHECK(f.draw() == XT4_FACE_BUSY && f.graphics.draws == draws);
      f.pipeline.sync.lock.unlock();
      f.display.generation = 2;
      CHECK(f.draw() == XT4_FACE_UNAVAILABLE && f.graphics.draws == draws);
      f.pipeline.provider.view.generation = 2;
      f.pipeline.provider.view.display_id = 1;
      f.display.display_id = f.graphics.display_id = 1;
      f.graphics.generation = 2;
      CHECK(xt4_result_invalidate(f.pipeline.exchange) == 0 &&
            xt4_worker_step(f.pipeline.worker) == 0);
      CHECK(f.draw() == 0);
      xt4_result_snapshot many{};
      CHECK(xt4_result_try_read(f.pipeline.exchange, f.pipeline.epoch(), 2, 1000, 250000000,
                                &many) == 0);
      many.count = 16;
      for (uint32_t i = 0; i < 16; i++) {
        float x = float(i * 8);
        many.boxes[i] = {{x, 10, x + 5, 20}, .9f};
      }
      CHECK(xt4_result_publish(f.pipeline.exchange, &many) == 0);
      auto prior = f.graphics.draws;
      CHECK(f.draw() == 0 && f.graphics.draws == prior + 16);
      many.count = 17;
      CHECK(xt4_result_publish(f.pipeline.exchange, &many) == XT4_FACE_INVALID);
      CHECK(xt4_pipeline_destroy(f.pipeline.worker) == XT4_FACE_BUSY);
      std::ofstream capture(std::string(XT4_BINARY_DIR) + "/fake-display-1.ppm", std::ios::binary);
      capture << "P6\n160 160\n255\n";
      capture.write(reinterpret_cast<const char *>(f.graphics.canvas.data()),
                    f.graphics.canvas.size());
      capture.close();
      CHECK(xt4_worker_disable(f.pipeline.worker) == 0);
      f.graphics.canvas = f.graphics.base;
      CHECK(f.draw() == XT4_FACE_UNAVAILABLE && f.graphics.canvas == f.graphics.base);
    }
    for (uint32_t failure = 1; failure <= 4; failure++) {
      OverlayFixture f;
      auto before = f.graphics.state;
      f.graphics.fail = failure;
      CHECK(f.draw() == XT4_FACE_INTERNAL);
      CHECK(f.graphics.restores == (failure == 1 ? 0u : 1u));
      if (failure != 4)
        CHECK(f.graphics.state == before);
      f.graphics.fail = 0;
      CHECK(f.draw() == XT4_FACE_UNAVAILABLE);
      xt4_result_snapshot s{};
      CHECK(xt4_result_try_read(f.pipeline.exchange, f.pipeline.epoch(), 1, 1000, 250000000, &s) ==
            XT4_FACE_UNAVAILABLE);
    }
    {
      OverlayFixture f;
      f.graphics.now = 250001001;
      CHECK(f.draw() == XT4_FACE_UNAVAILABLE && f.graphics.draws == 0);
    }
    {
      OverlayFixture f;
      f.graphics.clock_valid = false;
      CHECK(f.draw() == XT4_FACE_UNAVAILABLE && f.graphics.draws == 0);
    }
    {
      OverlayFixture f(1000);
      f.graphics.now = 2001;
      CHECK(f.draw() == XT4_FACE_UNAVAILABLE && f.graphics.draws == 0);
    }
  });
}
