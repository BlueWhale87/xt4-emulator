#include "pipeline_fixture.hpp"
#include <thread>
int main() {
  return test_main([] {
    {
      PipelineFixture f;
      CHECK(xt4_worker_enable(f.worker, XT4_MODE_RECORDED) == 0);
      xt4_result_snapshot s{};
      s.struct_size = sizeof s;
      s.abi_version = 1;
      s.epoch = f.epoch();
      s.generation = 1;
      s.capture_ns = 1000;
      s.count = 1;
      s.boxes[0] = {{1, 2, 3, 4}, .9f};
      s.boxes[1] = {{10, 10, 20, 20}, .8f};
      xt4_result_snapshot out{};
      CHECK(xt4_result_publish(f.exchange, &s) == 0);
      f.sync.lock.lock();
      CHECK(xt4_result_try_read(f.exchange, s.epoch, 1, 1000, 250000000, &out) == XT4_FACE_BUSY);
      CHECK(xt4_result_invalidate(f.exchange) == 0);
      f.sync.lock.unlock();
      CHECK(xt4_result_try_read(f.exchange, f.epoch(), 1, 1000, 250000000, &out) ==
            XT4_FACE_UNAVAILABLE);
      s.epoch = f.epoch();
      CHECK(xt4_result_publish(f.exchange, &s) == 0);
      CHECK(xt4_result_try_read(f.exchange, s.epoch, 2, 1000, 250000000, &out) ==
            XT4_FACE_UNAVAILABLE);
      std::atomic<bool> done{false}, ready{false};
      std::atomic<uint32_t> bad{0}, reads{0};
      uint64_t epoch = s.epoch;
      std::thread writer([&] {
        while (!ready.load())
          std::this_thread::yield();
        for (int i = 0; i < 20000; i++) {
          s.count = (i & 1) ? 1 : 2;
          s.boxes[0].confidence = (i & 1) ? .9f : .8f;
          auto status = xt4_result_publish(f.exchange, &s);
          if (status != 0 && status != XT4_FACE_BUSY)
            ++bad;
          if ((i & 127) == 0)
            std::this_thread::yield();
        }
        done = true;
      });
      std::thread reader([&] {
        ready = true;
        while (!done.load()) {
          xt4_result_snapshot copy{};
          if (xt4_result_try_read(f.exchange, epoch, 1, 1000, 250000000, &copy) == 0) {
            ++reads;
            if (!((copy.count == 1 && copy.boxes[0].confidence == .9f) ||
                  (copy.count == 2 && copy.boxes[0].confidence == .8f)))
              ++bad;
          }
        }
      });
      writer.join();
      reader.join();
      CHECK(bad == 0);
      CHECK(reads > 0);
      CHECK(xt4_result_try_read(f.exchange, s.epoch, 1, 250001000, 250000000, &out) == 0);
      CHECK(xt4_result_try_read(f.exchange, s.epoch, 1, 250001001, 250000000, &out) ==
            XT4_FACE_UNAVAILABLE);
      CHECK(xt4_result_try_read(f.exchange, s.epoch, 1, 999, 250000000, &out) ==
            XT4_FACE_UNAVAILABLE);
      CHECK(xt4_result_try_read(f.exchange, s.epoch, 1, 250001002, 250000000, &out) ==
            XT4_FACE_UNAVAILABLE);
    }
    {
      PipelineFixture no_clock(default_config(), false);
      CHECK(xt4_worker_enable(no_clock.worker, XT4_MODE_RECORDED) == 0);
      CHECK(xt4_worker_step(no_clock.worker) == XT4_FACE_UNAVAILABLE);
      CHECK(no_clock.provider.acquired == 0);
    }
  });
}
