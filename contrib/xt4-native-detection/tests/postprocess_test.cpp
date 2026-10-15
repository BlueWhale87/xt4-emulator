#include "context.hpp"
#include "face_fixture.hpp"
#include <cmath>
int main() {
  return test_main([] {
    auto cfg = default_config();
    Candidate workspace[1000];
    xt4_face_box out[16];
    auto result = empty_result();
    Candidate input[1100];
    for (uint32_t i = 0; i < 1100; i++) {
      float x = float(i % 100), y = float(i / 100);
      input[i] = {{{x, y, x + .5f, y + .5f}, .9f}, 1099 - i};
    }
    CHECK(xt4_select_boxes(input, 1100, cfg, workspace, out, 16, result) == 0);
    CHECK(result.count == 16);
    CHECK(out[0].edges.x0 == 99 && out[0].edges.y0 == 10);
    input[0] = {{{0, 0, 13, 1}, .8f}, 0};
    input[1] = {{{7, 0, 20, 1}, .8f}, 1}; // intersection6/union20 = .3
    CHECK(xt4_select_boxes(input, 2, cfg, workspace, out, 16, result) == 0 && result.count == 2);
    input[1].box.edges.x0 = 6.99f;
    CHECK(xt4_select_boxes(input, 2, cfg, workspace, out, 16, result) == 0 && result.count == 1);
    input[0].box.confidence = std::nextafter(.8f, 0.f);
    CHECK(xt4_select_boxes(input, 1, cfg, workspace, out, 16, result) == 0 && result.count == 0);
    // Invisible candidates must not consume keep_top_k. NMS still uses raw edges.
    cfg.keep_top_k = 1;
    input[0] = {{{-20, 0, -10, 10}, .99f}, 0};
    input[1] = {{{10, 10, 20, 20}, .9f}, 1};
    CHECK(xt4_select_boxes(input, 2, cfg, workspace, out, 16, result) == 0 && result.count == 1);
    CHECK(out[0].edges.x0 == 10 && out[0].confidence == .9f);
    CHECK(xt4_select_boxes(input, 2, cfg, workspace, nullptr, 0, result) == 0 &&
          result.count == 0 && result.truncated == 1);
    cfg.keep_top_k = 16;
    input[0] = {{{-100, 0, 100, 100}, .99f}, 0};
    input[1] = {{{0, 0, 100, 100}, .9f}, 1};
    cfg.nms_threshold = .75f; // raw IoU=.5; clipped IoU=1
    CHECK(xt4_select_boxes(input, 2, cfg, workspace, out, 16, result) == 0 && result.count == 2);
    cfg.nms_threshold = .3f;
    input[0] = {{{-5, -5, 5, 5}, .9f}, 0};
    input[1] = {{{20, 20, 20, 30}, .9f}, 1};
    input[2] = {{{NAN, 0, 5, 5}, .9f}, 2};
    input[3] = {{{0, 0, 5, 5}, INFINITY}, 3};
    CHECK(xt4_select_boxes(input, 4, cfg, workspace, out, 16, result) == 0 && result.count == 1);
    CHECK(out[0].edges.x0 == 0 && out[0].edges.y0 == 0);
    CHECK(xt4_select_boxes(input, 4, cfg, workspace, nullptr, 0, result) == 0 &&
          result.count == 0 && result.truncated == 1);
  });
}
