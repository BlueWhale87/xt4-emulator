#include "pipeline.hpp"
#include "pipeline_fixture.hpp"
#include <condition_variable>
#include <thread>
struct Barrier {
  std::mutex mutex;
  std::condition_variable cv;
  bool enabled = false, entered = false, go = false;
  Provider *provider = nullptr;
  bool released_first = false;
  static uint32_t clock(void *u, uint64_t *t) {
    auto &b = *static_cast<Barrier *>(u);
    *t = 1000;
    std::unique_lock<std::mutex> lock(b.mutex);
    if (b.enabled) {
      b.released_first = b.provider && b.provider->released == b.provider->acquired;
      b.entered = true;
      b.cv.notify_all();
      b.cv.wait(lock, [&] { return b.go; });
    }
    return 0;
  }
};
struct SequenceClock {
  uint64_t readings[8];
  uint32_t count, next = 0;
  static uint32_t read(void *user, uint64_t *out) {
    auto &c = *static_cast<SequenceClock *>(user);
    *out = c.readings[c.next < c.count ? c.next++ : c.count - 1];
    return 0;
  }
};
int main() {
  return test_main([] {
    {
      PipelineFixture f;
      CHECK(xt4_worker_enable(f.worker, XT4_MODE_RECORDED) == 0);
      CHECK(xt4_worker_step(f.worker) == 0);
      auto epoch = f.epoch();
      xt4_result_snapshot before{}, after{};
      CHECK(xt4_result_try_read(f.exchange, epoch, 1, 1000, 250000000, &before) == 0);
      f.provider.acquire_status = XT4_FACE_BUSY;
      CHECK(xt4_worker_step(f.worker) == XT4_FACE_BUSY);
      CHECK(f.epoch() == epoch);
      CHECK(f.provider.acquired == 1 && f.provider.released == 1);
      CHECK(xt4_result_try_read(f.exchange, epoch, 1, 1000, 250000000, &after) == 0);
      CHECK(after.count == before.count && after.capture_ns == before.capture_ns &&
            after.boxes[0].confidence == before.boxes[0].confidence);
      xt4_worker_state state{sizeof state, 1, 0, 0, 0, 0};
      CHECK(xt4_worker_get_state(f.worker, &state) == 0 && state.dropped_samples == 1);
    }

    {
      SequenceClock clock{{1000, 1000001000, 1050, 1060, 1070}, 5};
      PipelineFixture f(default_config(), true, SequenceClock::read, &clock);
      CHECK(xt4_worker_enable(f.worker, XT4_MODE_RECORDED) == 0);
      auto epoch = f.epoch();
      CHECK(xt4_worker_step(f.worker) == XT4_FACE_UNAVAILABLE);
      CHECK(f.epoch() != epoch);
      xt4_result_snapshot snapshot{};
      CHECK(xt4_result_try_read(f.exchange, f.epoch(), 1, 1070, 250000000, &snapshot) ==
            XT4_FACE_UNAVAILABLE);
      CHECK(f.provider.acquired == 1 && f.provider.released == 1);
    }
    {
      Barrier barrier;
      auto config = default_config();
      config.now_ns = Barrier::clock;
      config.clock_user = &barrier;
      PipelineFixture f(config);
      barrier.provider = &f.provider;
      CHECK(xt4_worker_step(f.worker) == XT4_FACE_UNAVAILABLE && f.provider.acquired == 0);
      CHECK(xt4_worker_enable(f.worker, XT4_MODE_VERIFIED_PREVIEW) == XT4_FACE_UNAVAILABLE);
      CHECK(xt4_worker_enable(f.worker, XT4_MODE_RECORDED) == 0);
      CHECK(xt4_worker_step(f.worker) == 0 && f.provider.acquired == 1 && f.provider.released == 1);
      xt4_worker_timings timings{};
      timings.struct_size = sizeof timings;
      timings.abi_version = 1;
      CHECK(xt4_worker_get_timings(f.worker, &timings) == 0 && timings.clock_available == 1 &&
            timings.prepare_ns == 0 && timings.publication_ns == 0);
      xt4_result_snapshot snapshot{};
      CHECK(xt4_result_try_read(f.exchange, f.epoch(), 1, 1000, 250000000, &snapshot) == 0 &&
            snapshot.count == 1);
      f.provider.short_frame = true;
      CHECK(xt4_worker_step(f.worker) == XT4_FACE_INVALID);
      CHECK(f.provider.acquired == 2 && f.provider.released == 2);
      CHECK(xt4_result_try_read(f.exchange, f.epoch(), 1, 1000, 250000000, &snapshot) ==
            XT4_FACE_UNAVAILABLE);
      f.provider.short_frame = false;
      barrier.enabled = true;
      uint32_t step_status = 99;
      std::thread thread([&] { step_status = xt4_worker_step(f.worker); });
      {
        std::unique_lock<std::mutex> lock(barrier.mutex);
        CHECK(barrier.cv.wait_for(lock, std::chrono::seconds(10), [&] { return barrier.entered; }));
      }
      CHECK(barrier.released_first);
      CHECK(xt4_worker_step(f.worker) == XT4_FACE_BUSY);
      CHECK(xt4_pipeline_destroy(f.worker) == XT4_FACE_BUSY);
      CHECK(xt4_worker_disable(f.worker) == 0);
      {
        std::lock_guard<std::mutex> lock(barrier.mutex);
        barrier.go = true;
        barrier.cv.notify_all();
      }
      thread.join();
      CHECK(step_status == XT4_FACE_UNAVAILABLE);
      CHECK(xt4_result_try_read(f.exchange, f.epoch(), 1, 1000, 250000000, &snapshot) ==
            XT4_FACE_UNAVAILABLE);
      CHECK(f.provider.acquired == 3 && f.provider.released == 3);
      CHECK(xt4_worker_step(f.worker) == XT4_FACE_UNAVAILABLE && f.provider.acquired == 3);
      {
        std::lock_guard<std::mutex> lock(barrier.mutex);
        barrier.go = false;
        barrier.entered = false;
      }
      CHECK(xt4_worker_enable(f.worker, XT4_MODE_RECORDED) == 0);
      std::thread changed([&] { step_status = xt4_worker_step(f.worker); });
      {
        std::unique_lock<std::mutex> lock(barrier.mutex);
        CHECK(barrier.cv.wait_for(lock, std::chrono::seconds(10), [&] { return barrier.entered; }));
      }
      CHECK(xt4_result_invalidate(f.exchange) == 0);
      f.provider.view.generation = 2;
      {
        std::lock_guard<std::mutex> lock(barrier.mutex);
        barrier.go = true;
        barrier.cv.notify_all();
      }
      changed.join();
      CHECK(step_status == XT4_FACE_UNAVAILABLE);
      barrier.enabled = false;
      CHECK(xt4_worker_step(f.worker) == 0);
      CHECK(xt4_result_try_read(f.exchange, f.epoch(), 1, 1000, 250000000, &snapshot) ==
            XT4_FACE_UNAVAILABLE);
      CHECK(xt4_result_try_read(f.exchange, f.epoch(), 2, 1000, 250000000, &snapshot) == 0);
      CHECK(xt4_face_destroy(f.face.ctx) == XT4_FACE_BUSY);
      CHECK(xt4_pipeline_destroy(f.worker) == 0);
      f.worker = nullptr;
    }
    {
      PipelineFixture f;
      CHECK(xt4_worker_enable(f.worker, XT4_MODE_RECORDED) == 0);
      void *pressure = f.face.ctx->arena.allocate(7 * 1024 * 1024);
      CHECK(pressure);
      CHECK(xt4_worker_step(f.worker) == XT4_FACE_NO_MEMORY);
      CHECK(f.face.ctx->arena.release(pressure) == Status::Ok);
      CHECK(xt4_worker_enable(f.worker, XT4_MODE_RECORDED) == XT4_FACE_UNAVAILABLE);
      CHECK(xt4_worker_step(f.worker) == XT4_FACE_UNAVAILABLE);
    }
  });
}
