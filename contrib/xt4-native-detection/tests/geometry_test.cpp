#include "test_support.hpp"
#include "xt4_preview.h"
#include <cmath>
int main() {
  return test_main([] {
    xt4_prepare_transform p{};
    p.struct_size = sizeof p;
    p.abi_version = 1;
    p.source_width = 320;
    p.source_height = 180;
    p.resized_width = 160;
    p.resized_height = 90;
    p.pad_top = p.pad_bottom = 35;
    p.scale_x = p.scale_y = .5f;
    p.generation = 4;
    xt4_display_transform d{};
    d.struct_size = sizeof d;
    d.abi_version = 1;
    d.generation = 4;
    d.matrix[0] = d.matrix[4] = d.matrix[8] = 1;
    d.viewport = {0, 0, 320, 180};
    xt4_box_edges b{40, 35, 80, 80}, out{};
    auto expect = [&](float x0, float y0, float x1, float y1) {
      CHECK(xt4_map_box(&b, &p, &d, &out) == 0);
      CHECK(std::fabs(out.x0 - x0) < .001f && std::fabs(out.y0 - y0) < .001f &&
            std::fabs(out.x1 - x1) < .001f && std::fabs(out.y1 - y1) < .001f);
    };
    expect(80, 0, 160, 90);
    d.matrix[0] = -1;
    d.matrix[2] = 320;
    expect(160, 0, 240, 90);
    float rotations[3][9] = {{0, -1, 180, 1, 0, 0, 0, 0, 1},
                             {-1, 0, 320, 0, -1, 180, 0, 0, 1},
                             {0, 1, 0, -1, 0, 320, 0, 0, 1}};
    memcpy(d.matrix, rotations[0], sizeof d.matrix);
    d.viewport = {0, 0, 180, 320};
    expect(90, 80, 180, 160);
    memcpy(d.matrix, rotations[1], sizeof d.matrix);
    d.viewport = {0, 0, 320, 180};
    expect(160, 90, 240, 180);
    memcpy(d.matrix, rotations[2], sizeof d.matrix);
    d.viewport = {0, 0, 180, 320};
    expect(0, 160, 90, 240);
    d.generation = 5;
    CHECK(xt4_map_box(&b, &p, &d, &out) != 0);
    d.generation = 4;
    b = {1, 1, 20, 20};
    CHECK(xt4_map_box(&b, &p, &d, &out) != 0);
    b = {40, 35, 80, 80};
    d.matrix[0] = NAN;
    CHECK(xt4_map_box(&b, &p, &d, &out) != 0);
  });
}
