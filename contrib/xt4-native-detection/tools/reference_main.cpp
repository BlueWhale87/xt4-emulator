#include "facedetectcnn.h"
#include "reference_identity.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>
void xt4_ref_trace(const char *label, const CDataBlob<float> &t) {
  const char *prefix = std::getenv("XT4_REFERENCE_TRACE_PREFIX");
  if (!prefix)
    return;
  std::ofstream out(std::string(prefix) + label + ".bin", std::ios::binary);
  uint32_t dims[] = {uint32_t(t.rows), uint32_t(t.cols), uint32_t(t.channels)};
  out.write(reinterpret_cast<const char *>(dims), sizeof dims);
  for (int r = 0; r < t.rows; r++)
    for (int c = 0; c < t.cols; c++)
      out.write(reinterpret_cast<const char *>(t.ptr(r, c)), t.channels * sizeof(float));
}
int main(int argc, char **argv) {
  std::string input, output;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc)
      return 2;
    std::string k = argv[i];
    if (k == "--input")
      input = argv[i + 1];
    else if (k == "--output")
      output = argv[i + 1];
    else
      return 2;
  }
  if (input.empty() || output.empty())
    return 2;
  std::ifstream f(input, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() != 76800)
    return 2;
  f.seekg(0);
  std::vector<unsigned char> image(76800);
  if (!f.read(reinterpret_cast<char *>(image.data()), image.size()))
    return 2;
  auto faces = objectdetect_cnn(image.data(), 160, 160, 480);
  std::ofstream out(output);
  if (!out)
    return 5;
  out << std::setprecision(9) << "{\"model_sha256\":\"" << XT4_REF_MODEL
      << "\",\"source_sha256\":\"" << XT4_REF_SOURCE << "\",\"patch_sha256\":\"" << XT4_REF_PATCH
      << "\",\"settings\":{\"confidence\":0.8,\"nms\":0.3,\"top_k\":1000,\"keep_top_k\":16},"
         "\"boxes\":[";
  bool first = true;
  for (auto &b : faces) {
    float x0 = std::clamp(b.x, 0.f, 160.f), y0 = std::clamp(b.y, 0.f, 160.f);
    float x1 = std::clamp(b.x + b.w, 0.f, 160.f), y1 = std::clamp(b.y + b.h, 0.f, 160.f);
    if (x1 <= x0 || y1 <= y0)
      continue;
    if (!first)
      out << ',';
    first = false;
    out << "{\"score\":" << b.score << ",\"x0\":" << x0 << ",\"y0\":" << y0 << ",\"x1\":" << x1
        << ",\"y1\":" << y1 << '}';
  }
  out << "]}\n";
  out.close();
  return out.fail() ? 5 : 0;
}
