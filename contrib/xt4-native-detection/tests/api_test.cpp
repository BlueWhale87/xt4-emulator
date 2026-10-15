#include "face_fixture.hpp"
#include "no_heap.hpp"
#include <cmath>
struct Reentry {
  xt4_face_context *ctx = nullptr;
  bool enabled = false;
  uint64_t time = 1;
  uint32_t detect = 99, destroy = 99;
};
static uint32_t clock_read(void *u, uint64_t *t) {
  auto &r = *static_cast<Reentry *>(u);
  *t = r.time++;
  if (r.enabled) {
    auto result = empty_result();
    r.detect = xt4_face_detect(r.ctx, nullptr, nullptr, 0, &result);
    r.destroy = xt4_face_destroy(r.ctx);
  }
  return 0;
}
int main() {
  return test_main([] {
    Reentry reentry;
    auto config = default_config();
    config.now_ns = clock_read;
    config.clock_user = &reentry;
    FaceFixture f(config);
    reentry.ctx = f.ctx;
    auto input = read_text(std::string(XT4_SOURCE_DIR) + "/tests/fixtures/astronaut.bgr");
    auto im = fixture_image(input);
    xt4_face_box boxes[16];
    auto result = empty_result();
    im.data_bytes = 76799;
    result.count = 16;
    CHECK(xt4_face_detect(f.ctx, &im, boxes, 16, &result) == XT4_FACE_INVALID);
    CHECK(result.count == 0);
    im = fixture_image(input);
    CHECK(xt4_face_detect(f.ctx, &im, boxes, 16, &result) == 0);
    CHECK(result.count == 1);
    for (uint32_t capacity : {0u, 1u, 16u}) {
      CHECK(xt4_face_detect(f.ctx, &im, capacity ? boxes : nullptr, capacity, &result) == 0);
      CHECK(result.count == std::min(capacity, 1u));
      CHECK(result.truncated == (capacity == 0));
    }
    CHECK(xt4_face_detect(f.ctx, &im, boxes, 17, &result) == XT4_FACE_INVALID && result.count == 0);
    im.width = UINT32_MAX;
    CHECK(xt4_face_detect(f.ctx, &im, boxes, 16, &result) == XT4_FACE_INVALID);
    im = fixture_image(input);
    im.abi_version = 2;
    CHECK(xt4_face_detect(f.ctx, &im, boxes, 16, &result) == XT4_FACE_UNSUPPORTED);
    im = fixture_image(input);
    reentry.enabled = true;
    CHECK(xt4_face_detect(f.ctx, &im, boxes, 16, &result) == 0);
    CHECK(reentry.detect == XT4_FACE_BUSY && reentry.destroy == XT4_FACE_BUSY);
    reentry.enabled = false;
    xt4_face_context *second = reinterpret_cast<xt4_face_context *>(1);
    CHECK(xt4_face_create(&config, f.region, 8388608, &second) == XT4_FACE_BUSY &&
          second == nullptr);
    xt4_face_stats stats{};
    stats.struct_size = sizeof stats;
    stats.abi_version = 1;
    CHECK(xt4_face_get_stats(f.ctx, &stats) == 0);
    CHECK(stats.peak_bytes <= 8388608 && stats.prepared_image_bytes == 76800 &&
          stats.clock_available == 1);
    std::vector<char> padded(488 * 160, 0);
    for (int row = 0; row < 160; row++)
      memcpy(padded.data() + row * 488, input.data() + row * 480, 480);
    im.data = reinterpret_cast<const uint8_t *>(padded.data());
    im.data_bytes = padded.size();
    im.stride = 488;
    CHECK(xt4_face_detect(f.ctx, &im, boxes, 16, &result) == 0 && result.count == 1);
    CHECK(std::fabs(boxes[0].confidence - .901229203f) < .0001f);
    im = fixture_image(input);
    for (int i = 0; i < 200; i++) {
      deny_heap = true;
      auto status = xt4_face_detect(f.ctx, &im, boxes, 16, &result);
      deny_heap = false;
      CHECK(status == 0 && result.count == 1);
    }
    CHECK(xt4_face_get_stats(f.ctx, &stats) == 0);
    std::cout << "Full detector peak region bytes: " << stats.peak_bytes << "\n";
    auto end = reinterpret_cast<uintptr_t>(f.region) + 8388608;
    for (auto ptr = f.memory.data();
         reinterpret_cast<uintptr_t>(ptr) < reinterpret_cast<uintptr_t>(f.region); ptr++)
      CHECK(*ptr == 0xa5);
    for (auto ptr = reinterpret_cast<uint8_t *>(end); ptr < f.memory.data() + f.memory.size();
         ptr++)
      CHECK(*ptr == 0xa5);
    CHECK(xt4_face_destroy(f.ctx) == 0);
    CHECK(xt4_face_destroy(f.ctx) == XT4_FACE_INVALID);
    f.ctx = nullptr;
    config.model_sha256[0] = '0';
    CHECK(xt4_face_create(&config, f.region, 8388608, &second) == XT4_FACE_UNSUPPORTED &&
          second == nullptr);
    config = default_config();
    config.struct_size--;
    CHECK(xt4_face_create(&config, f.region, 8388608, &second) == XT4_FACE_INVALID);
    config = default_config();
    config.confidence_threshold = -1;
    CHECK(xt4_face_create(&config, f.region, 8388608, &second) == XT4_FACE_INVALID);
    config = default_config();
    config.memory_limit = 1024;
    CHECK(xt4_face_create(&config, f.region, 1024, &second) == XT4_FACE_NO_MEMORY &&
          second == nullptr);
    for (uint64_t cap : {uint64_t(16384), uint64_t(131072), uint64_t(200000)}) {
      config = default_config();
      config.memory_limit = cap;
      CHECK(xt4_face_create(&config, f.region, cap, &second) == XT4_FACE_NO_MEMORY &&
            second == nullptr);
    }
    config = default_config();
    CHECK(xt4_face_create(&config, f.region, 8388608, &second) == 0);
    CHECK(xt4_face_detect(second, &im, boxes, 16, &result) == 0);
    CHECK(xt4_face_get_stats(second, &stats) == 0 && stats.clock_available == 0);
    CHECK(xt4_face_destroy(second) == 0);
  });
}
