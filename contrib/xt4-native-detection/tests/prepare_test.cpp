#include "test_support.hpp"
#include "xt4_preview.h"
#include <algorithm>
int main() {
  return test_main([] {
    std::vector<uint8_t> pixels(320 * 180 * 3, 42), dst(76800, 0);
    xt4_prepare_transform t{};
    xt4_frame_view v{};
    v.struct_size = sizeof v;
    v.abi_version = 1;
    v.format = XT4_BGR8;
    v.data = pixels.data();
    v.data_bytes = pixels.size();
    v.width = v.source_width = 320;
    v.height = v.source_height = 180;
    v.stride = 960;
    v.generation = 3;
    CHECK(xt4_prepare_bgr160(&v, dst.data(), dst.size(), &t) == 0);
    CHECK(t.resized_width == 160 && t.resized_height == 90 && t.pad_top == 35 &&
          t.pad_bottom == 35);
    CHECK(dst[0] == 0 && dst[35 * 480] == 42 && dst[125 * 480] == 0);
    std::vector<uint8_t> padded(968 * 180, 91);
    for (int i = 0; i < 180; i++)
      memcpy(padded.data() + i * 968, pixels.data() + i * 960, 960);
    auto original = dst;
    v.data = padded.data();
    v.stride = 968;
    v.data_bytes = padded.size();
    CHECK(xt4_prepare_bgr160(&v, dst.data(), dst.size(), &t) == 0);
    CHECK(dst == original);
    auto valid = v;
    v.data_bytes = 100;
    CHECK(xt4_prepare_bgr160(&v, dst.data(), dst.size(), &t) != 0);
    v = valid;
    v.stride = 959;
    CHECK(xt4_prepare_bgr160(&v, dst.data(), dst.size(), &t) != 0);
    v = valid;
    v.source_x = 300;
    CHECK(xt4_prepare_bgr160(&v, dst.data(), dst.size(), &t) != 0);
    v = valid;
    v.width = UINT32_MAX;
    CHECK(xt4_prepare_bgr160(&v, dst.data(), dst.size(), &t) != 0);
    v = valid;
    v.height = 0;
    CHECK(xt4_prepare_bgr160(&v, dst.data(), dst.size(), &t) != 0);
    v = valid;
    CHECK(xt4_prepare_bgr160(&v, dst.data(), 76799, &t) != 0);
    CHECK(xt4_prepare_bgr160(&v, padded.data(), padded.size(), &t) != 0);
    uint8_t tiny[18];
    std::fill(tiny, tiny + 18, 99);
    v.data = tiny;
    v.data_bytes = 18;
    v.width = v.source_width = 3;
    v.height = v.source_height = 2;
    v.stride = 9;
    CHECK(xt4_prepare_bgr160(&v, dst.data(), dst.size(), &t) == 0);
    CHECK(t.resized_width == 160 && t.resized_height == 107 && t.pad_top == 26 &&
          t.pad_bottom == 27);
    uint8_t two[] = {0, 50, 90, 200, 50, 90};
    v.data = two;
    v.data_bytes = 6;
    v.width = v.source_width = 2;
    v.height = v.source_height = 1;
    v.stride = 6;
    CHECK(xt4_prepare_bgr160(&v, dst.data(), dst.size(), &t) == 0);
    CHECK(dst[40 * 480 + 79 * 3] == 99 && dst[40 * 480 + 80 * 3] == 101 &&
          dst[40 * 480 + 80 * 3 + 1] == 50);
  });
}
