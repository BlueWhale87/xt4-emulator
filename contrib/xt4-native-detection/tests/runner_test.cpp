#include "test_support.hpp"
#include <filesystem>
int main() {
  return test_main([] {
    std::string bin = std::string(XT4_BINARY_DIR) + "/xt4_face_run",
                input = std::string(XT4_SOURCE_DIR) + "/tests/fixtures/blank.bgr";
    std::string report = std::string(XT4_BINARY_DIR) + "/report with \"quotes\".json";
    auto command = [&](std::vector<std::string> extra) {
      std::vector<std::string> a{bin,   "--input",   input, "--width",  "160",      "--height",
                                 "160", "--stride",  "480", "--mode",   "recorded", "--warmup",
                                 "0",   "--samples", "1",   "--report", report};
      a.insert(a.end(), extra.begin(), extra.end());
      return a;
    };
    auto valid = command({});
    CHECK(run_cli(valid).exit_status == 0);
    CHECK(run_cli({"/usr/bin/python3", "-c",
                   "import json,sys; r=json.load(open(sys.argv[1])); assert r['success'] and "
                   "r['mode']=='recorded'; assert r['samples']==1 and r['warmup']==0; assert "
                   "r['boxes']==[]; assert r['memory']['peak_bytes']<=8388608; assert "
                   "r['stages']['graph']['median_ms']>=0; assert "
                   "r['binary_sizes']['code_bytes']>0; assert len(r['fixture_sha256'])==64",
                   report})
              .exit_status == 0);
    for (const char *bad : {"0", "-1", "18446744073709551616", "oops"}) {
      auto a = valid;
      for (size_t i = 0; i < a.size(); i++)
        if (a[i] == "--samples")
          a[i + 1] = bad;
      CHECK(run_cli(a).exit_status == 2);
    }
    for (auto key : {"--width", "--height", "--stride"}) {
      auto a = valid;
      for (size_t i = 0; i < a.size(); i++)
        if (a[i] == key)
          a[i + 1] = "4294967296";
      CHECK(run_cli(a).exit_status == 2);
    }
    auto stride = valid;
    for (size_t i = 0; i < stride.size(); i++)
      if (stride[i] == "--stride")
        stride[i + 1] = "479";
    CHECK(run_cli(stride).exit_status == 2);
    auto mode = valid;
    for (size_t i = 0; i < mode.size(); i++)
      if (mode[i] == "--mode")
        mode[i + 1] = "unknown";
    CHECK(run_cli(mode).exit_status == 2);
    for (size_t i = 0; i < mode.size(); i++)
      if (mode[i] == "--mode")
        mode[i + 1] = "verified-preview";
    CHECK(run_cli(mode).exit_status == 3);
    std::string short_file = std::string(XT4_BINARY_DIR) + "/short-runner.bgr";
    write_bytes(short_file, 76799);
    auto a = valid;
    for (size_t i = 0; i < a.size(); i++)
      if (a[i] == "--input")
        a[i + 1] = short_file;
    CHECK(run_cli(a).exit_status == 2);
    auto fail = valid;
    for (size_t i = 0; i < fail.size(); i++)
      if (fail[i] == "--report")
        fail[i + 1] = XT4_BINARY_DIR;
    CHECK(run_cli(fail).exit_status == 5);
    auto low = valid;
    low.push_back("--arena-bytes");
    low.push_back("1024");
    CHECK(run_cli(low).exit_status == 4);
    auto synthetic = valid;
    for (size_t i = 0; i < synthetic.size(); i++)
      if (synthetic[i] == "--mode")
        synthetic[i + 1] = "synthetic";
    CHECK(run_cli(synthetic).exit_status == 0);
    CHECK(run_cli({"/usr/bin/python3", "-c",
                   "import json,sys; r=json.load(open(sys.argv[1])); assert "
                   "r['mode']=='synthetic'; assert r['stages']['graph'] is None; assert "
                   "r['synthetic_fixture']=='centered-box-v1'; assert len(r['boxes'])==1",
                   report})
              .exit_status == 0);
    // Path quoting must preserve actual input identity, not invoke a shell.
    std::string quoted = std::string(XT4_BINARY_DIR) + "/input \"quote\".bgr";
    std::filesystem::copy_file(input, quoted, std::filesystem::copy_options::overwrite_existing);
    auto q = valid;
    for (size_t i = 0; i < q.size(); i++)
      if (q[i] == "--input")
        q[i + 1] = quoted;
    CHECK(run_cli(q).exit_status == 0);
    CHECK(run_cli({"/usr/bin/python3", "-c",
                   "import json,sys,hashlib; r=json.load(open(sys.argv[1])); assert "
                   "r['input_path']==sys.argv[2]; assert "
                   "r['fixture_sha256']==hashlib.sha256(open(sys.argv[2],'rb').read()).hexdigest();"
                   " assert "
                   "r['config_sha256']==hashlib.sha256(r['config_canonical'].encode()).hexdigest()",
                   report, quoted})
              .exit_status == 0);
    auto alias = q;
    for (size_t i = 0; i < alias.size(); i++)
      if (alias[i] == "--report")
        alias[i + 1] = quoted;
    CHECK(run_cli(alias).exit_status == 2);
    CHECK(read_text(quoted) == read_text(input));
    alias = q;
    alias.push_back("--overlay");
    alias.push_back(quoted);
    CHECK(run_cli(alias).exit_status == 2);
    CHECK(read_text(quoted) == read_text(input));
  });
}
