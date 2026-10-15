#include "engine.hpp"
#include "no_heap.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <new>
struct Trace {
  uint32_t rows, cols, ch;
  std::vector<float> data;
};
struct Traces {
  std::array<Trace, 6> tensors;
  uint32_t compared = 0;
};
static const char *labels[] = {"early", "middle", "cls", "reg", "kps", "obj"};
static void compare(void *user, const char *label, const Tensor &t) {
  auto &all = *static_cast<Traces *>(user);
  uint32_t index = 0;
  while (index < 6 && std::strcmp(label, labels[index]))
    ++index;
  CHECK(index < 6);
  auto &ref = all.tensors[index];
  CHECK(t.rows == ref.rows && t.cols == ref.cols && t.channels == ref.ch);
  size_t i = 0;
  float max_error = 0;
  for (uint32_t r = 0; r < t.rows; r++)
    for (uint32_t c = 0; c < t.cols; c++)
      for (uint32_t ch = 0; ch < t.channels; ch++) {
        float x = t.ptr(r, c)[ch], expected = ref.data[i++];
        CHECK(std::isfinite(x));
        max_error = std::max(max_error, std::fabs(x - expected));
      }
  if (max_error > 0.0001f) {
    deny_heap = false;
    std::cerr << label << " max difference " << max_error << "\n";
  }
  CHECK(max_error <= 0.0001f);
  ++all.compared;
}
int main() {
  return test_main([] {
    std::vector<uint8_t> backing(8388608 + 127);
    auto *memory = reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(backing.data()) + 127) &
                                            ~uintptr_t(127));
    Arena arena;
    CHECK(arena.init(memory, 8388608) == Status::Ok);
    Engine engine;
    CHECK(engine.init(arena) == Status::Ok);
    for (const char *name : {"astronaut", "blank"}) {
      std::string fixture = std::string(XT4_SOURCE_DIR) + "/tests/fixtures/" + name + ".bgr",
                  prefix = std::string(XT4_BINARY_DIR) + "/trace-" + name + "-";
      setenv("XT4_REFERENCE_TRACE_PREFIX", prefix.c_str(), 1);
      CHECK(run_cli({std::string(XT4_BINARY_DIR) + "/xt4_face_reference", "--input", fixture,
                     "--output", std::string(XT4_BINARY_DIR) + "/trace-result.json"})
                .exit_status == 0);
      unsetenv("XT4_REFERENCE_TRACE_PREFIX");
      auto bytes = read_text(fixture);
      CHECK(bytes.size() == 76800);
      Traces refs{};
      for (uint32_t i = 0; i < 6; i++) {
        std::ifstream f(prefix + labels[i] + ".bin", std::ios::binary);
        auto &t = refs.tensors[i];
        f.read(reinterpret_cast<char *>(&t.rows), 4);
        f.read(reinterpret_cast<char *>(&t.cols), 4);
        f.read(reinterpret_cast<char *>(&t.ch), 4);
        CHECK(f.good());
        t.data.resize(uint64_t(t.rows) * t.cols * t.ch);
        f.read(reinterpret_cast<char *>(t.data.data()), t.data.size() * sizeof(float));
        CHECK(f.good());
      }
      RawHeads heads;
      deny_heap = true;
      auto status =
          engine.run(reinterpret_cast<const uint8_t *>(bytes.data()), 480, heads, compare, &refs);
      deny_heap = false;
      CHECK(status == Status::Ok && refs.compared == 6);
      CHECK(arena.stats().peak_bytes <= 8388608);
      auto live = arena.stats().current_bytes;
      for (int i = 0; i < 20; i++) {
        deny_heap = true;
        status = engine.run(reinterpret_cast<const uint8_t *>(bytes.data()), 480, heads);
        deny_heap = false;
        CHECK(status == Status::Ok);
        CHECK(arena.stats().current_bytes == live);
      }
    }
    std::cout << "Engine peak region bytes: " << arena.stats().peak_bytes << "\n";
    CHECK(engine.shutdown() == Status::Ok);
    CHECK(arena.stats().current_bytes == 128);
    std::array<bool, 107> covered{};
    for (uint64_t cap = 256; cap < 300000; cap += 128) {
      Arena small;
      CHECK(small.init(memory, cap) == Status::Ok);
      Engine probe;
      auto result = probe.init(small);
      if (result == Status::Ok) {
        probe.shutdown();
        break;
      }
      CHECK(result == Status::NoMemory);
      auto allocations = small.stats().allocation_count;
      CHECK(allocations < 107);
      covered[allocations] = true;
      CHECK(small.stats().current_bytes == 128);
    }
    for (uint32_t i = 0; i < 106; i++)
      CHECK(covered[i]);
    std::cout << "Initialization failure boundaries verified: 106\n";
  });
}
