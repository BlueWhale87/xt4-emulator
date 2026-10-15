#include "face_fixture.hpp"
#include <iomanip>
int main() {
  return test_main([] {
    FaceFixture fixture;
    for (const char *name :
         {"astronaut", "blank", "translated", "darkened", "non-square", "crowd"}) {
      std::string root = std::string(XT4_SOURCE_DIR) + "/tests/fixtures/" + name;
      auto bytes = read_text(root + ".bgr");
      auto im = fixture_image(bytes);
      xt4_face_box boxes[16];
      auto result = empty_result();
      CHECK(xt4_face_detect(fixture.ctx, &im, boxes, 16, &result) == 0);
      std::string output = std::string(XT4_BINARY_DIR) + "/" + name + ".native.json";
      std::ofstream f(output);
      f << std::setprecision(9) << "[";
      for (uint32_t i = 0; i < result.count; i++) {
        auto &b = boxes[i];
        if (i)
          f << ",";
        f << "[" << b.confidence << "," << b.edges.x0 << "," << b.edges.y0 << "," << b.edges.x1
          << "," << b.edges.y1 << "]";
      }
      f << "]";
      f.close();
      auto compare =
          run_cli({"/usr/bin/python3", "-c",
                   "import json,sys; a=json.load(open(sys.argv[1])); "
                   "b=json.load(open(sys.argv[2]))['boxes']; assert len(a)==len(b); assert "
                   "all(abs(x[0]-y['score'])<=0.0001 and all(abs(x[i+1]-y[k])<=0.25 for i,k in "
                   "enumerate(['x0','y0','x1','y1'])) for x,y in zip(a,b))",
                   output, root + ".reference.json"});
      if (compare.exit_status)
        std::cerr << name << ": " << compare.stderr_text;
      CHECK(compare.exit_status == 0);
    }
  });
}
