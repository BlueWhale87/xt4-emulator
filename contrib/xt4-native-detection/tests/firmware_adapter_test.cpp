#include "test_support.hpp"
#include "xt4_overlay.h"
int main() {
  return test_main([] {
    xt4_firmware_profile p{};
    p.struct_size = sizeof p;
    p.abi_version = 1;
    std::strcpy(p.firmware_sha256,
                "853d273b5603d3a2b93493f0ebd6a762b0b4b6bdc70317777775ae5f8ed93bc5");
    xt4_frame_provider provider{};
    xt4_overlay_backend backend{};
    for (int mode = 0; mode < 4; mode++) {
      if (mode == 1)
        p.firmware_sha256[0] = '0';
      if (mode == 2) {
        p.pixel_format = 999;
        p.memory_budget = 8388608;
      }
      if (mode == 3) {
        p.runtime_mapping_verified = p.context_owner_verified = p.frame_lifetime_verified =
            p.cache_verified = p.abi_verified = 1;
      }
      CHECK(xt4_firmware_provider_open(&p, &provider) == XT4_FACE_UNAVAILABLE &&
            !provider.try_acquire);
      CHECK(xt4_firmware_overlay_open(&p, &backend) == XT4_FACE_UNAVAILABLE && !backend.draw_rect);
    }
  });
}
