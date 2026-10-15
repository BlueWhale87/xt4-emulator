#include "arm_fixtures.h"
#include "xt4_face.h"
#include <cstdint>
#include <cstring>
alignas(128) static uint8_t region[8388608];
static uint32_t semihost(uint32_t op, const void *args) {
  register uint32_t r0 asm("r0") = op;
  register const void *r1 asm("r1") = args;
  asm volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory", "cc");
  return r0;
}
static void text(const char *s) { semihost(4, s); }
static void decimal(uint64_t value) {
  char buffer[32];
  uint32_t i = sizeof buffer;
  buffer[--i] = 0;
  do {
    buffer[--i] = char('0' + value % 10);
    value /= 10;
  } while (value);
  text(buffer + i);
}
static void bits(float f) {
  uint32_t value = 0;
  std::memcpy(&value, &f, 4);
  char out[9];
  const char *hex = "0123456789abcdef";
  for (uint32_t i = 0; i < 8; i++)
    out[i] = hex[(value >> (28 - 4 * i)) & 15];
  out[8] = 0;
  text(out);
}
static bool close(float a, float b, float tolerance) {
  float delta = a - b;
  if (delta < 0)
    delta = -delta;
  return delta <= tolerance;
}
extern "C" [[noreturn]] void xt4_semihost_exit(uint32_t code) {
  uint32_t args[] = {0x20026, code};
  semihost(0x20, args);
  for (;;) {
  }
}
extern "C" uint8_t __stack_bottom__[], __stack_top__[];
extern "C" int xt4_smoke_main() {
  text("XT4 ARMv7-A SOFT DIAGNOSTIC\n");
  xt4_face_config config{};
  config.struct_size = sizeof config;
  config.abi_version = 1;
  std::memcpy(config.model_sha256, XT4_FACE_MODEL_SHA256, 65);
  config.confidence_threshold = .8f;
  config.nms_threshold = .3f;
  config.top_k = 1000;
  config.keep_top_k = 16;
  config.memory_limit = sizeof region;
  xt4_face_context *context = nullptr;
  uint32_t status = xt4_face_create(&config, region, sizeof region, &context);
  if (status) {
    text("CREATE FAIL ");
    decimal(status);
    text("\n");
    return 1;
  }
  for (const auto &fixture : fixtures) {
    xt4_face_image image{sizeof image, 1, XT4_BGR8, fixture.image, 76800, 160, 160, 480};
    xt4_face_box boxes[16];
    xt4_face_result result{sizeof result, 1, 0, 0};
    status = xt4_face_detect(context, &image, boxes, 16, &result);
    text("CASE ");
    text(fixture.name);
    text(" ");
    decimal(result.count);
    text("\n");
    if (status || result.count != fixture.count) {
      text("COUNT/STATUS FAIL\n");
      return 2;
    }
    for (uint32_t i = 0; i < result.count; i++) {
      auto &b = boxes[i];
      auto &e = fixture.expected[i];
      text("BOX ");
      bits(b.confidence);
      text(" ");
      bits(b.edges.x0);
      text(" ");
      bits(b.edges.y0);
      text(" ");
      bits(b.edges.x1);
      text(" ");
      bits(b.edges.y1);
      text("\n");
      if (!close(b.confidence, e.confidence, .0001f) || !close(b.edges.x0, e.edges.x0, .25f) ||
          !close(b.edges.y0, e.edges.y0, .25f) || !close(b.edges.x1, e.edges.x1, .25f) ||
          !close(b.edges.y1, e.edges.y1, .25f)) {
        text("PARITY FAIL\n");
        return 3;
      }
    }
    image.data_bytes = 76799;
    result.count = 16;
    if (xt4_face_detect(context, &image, boxes, 16, &result) != XT4_FACE_INVALID || result.count) {
      text("BOUNDS FAIL\n");
      return 4;
    }
  }
  xt4_face_stats stats{};
  stats.struct_size = sizeof stats;
  stats.abi_version = 1;
  if (xt4_face_get_stats(context, &stats) || stats.peak_bytes > sizeof region) {
    text("STATS FAIL\n");
    return 5;
  }
  text("PEAK ");
  decimal(stats.peak_bytes);
  text("\n");
  if (xt4_face_destroy(context)) {
    text("DESTROY FAIL\n");
    return 6;
  }
  auto *p = __stack_bottom__;
  while (p < __stack_top__ && *p == 0xa5)
    ++p;
  text("STACK_HIGH_WATER ");
  decimal(uint64_t(__stack_top__ - p));
  text("\n");
  text("PASS native ARM face inference\n");
  return 0;
}
