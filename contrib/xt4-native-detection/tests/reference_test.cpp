#include "test_support.hpp"
int main() {
  return test_main([] {
    const std::string bin = std::string(XT4_BINARY_DIR) + "/xt4_face_reference";
    const std::string tmp = std::string(XT4_BINARY_DIR) + "/reference-input.bgr";
    const std::string output = std::string(XT4_BINARY_DIR) + "/reference-output.json";
    unlink(output.c_str());
    write_bytes(tmp, 76799);
    auto short_run = run_cli({bin, "--input", tmp, "--output", output});
    CHECK(short_run.exit_status == 2);
    CHECK(access(output.c_str(), F_OK) != 0);
    write_bytes(tmp, 76800);
    auto blank = run_cli({bin, "--input", tmp, "--output", output});
    CHECK(blank.exit_status == 0);
    auto validate =
        run_cli({"/usr/bin/python3", "-c",
                 "import json,sys; r=json.load(open(sys.argv[1])); assert r['boxes']==[]; assert "
                 "r['settings']=={'confidence':0.8,'nms':0.3,'top_k':1000,'keep_top_k':16}; assert "
                 "len(r['model_sha256'])==64; assert len(r['source_sha256'])==64; assert "
                 "len(r['patch_sha256'])==64",
                 output});
    CHECK(validate.exit_status == 0);
  });
}
