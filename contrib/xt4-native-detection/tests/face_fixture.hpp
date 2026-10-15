#pragma once
#include "test_support.hpp"
#include "xt4_face.h"
inline xt4_face_config default_config() {
  xt4_face_config c{};
  c.struct_size = sizeof c;
  c.abi_version = 1;
  std::strcpy(c.model_sha256, XT4_FACE_MODEL_SHA256);
  c.confidence_threshold = .8f;
  c.nms_threshold = .3f;
  c.top_k = 1000;
  c.keep_top_k = 16;
  c.memory_limit = 8388608;
  return c;
}
struct FaceFixture {
  std::vector<uint8_t> memory;
  void *region;
  xt4_face_context *ctx = nullptr;
  xt4_face_config config;
  FaceFixture(xt4_face_config c = default_config()) : memory(8388608 + 256, 0xa5), config(c) {
    region = reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(memory.data()) + 127) &
                                      ~uintptr_t(127));
    CHECK(xt4_face_create(&config, region, 8388608, &ctx) == 0 && ctx);
  }
  ~FaceFixture() {
    if (ctx)
      xt4_face_destroy(ctx);
  }
};
inline xt4_face_image fixture_image(const std::string &bytes) {
  xt4_face_image im{};
  im.struct_size = sizeof im;
  im.abi_version = 1;
  im.format = XT4_BGR8;
  im.data = reinterpret_cast<const uint8_t *>(bytes.data());
  im.data_bytes = bytes.size();
  im.width = im.height = 160;
  im.stride = 480;
  return im;
}
inline xt4_face_result empty_result() { return {sizeof(xt4_face_result), 1, 0, 0}; }
