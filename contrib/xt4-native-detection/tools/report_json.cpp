#include "report_json.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>
std::string json_string(const std::string &s) {
  const char *hex = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += char(c);
  }
  return out + '"';
}
std::string sha256_bytes(const void *data, size_t n) {
  CC_SHA256_CTX ctx;
  CC_SHA256_Init(&ctx);
  auto *p = static_cast<const uint8_t *>(data);
  while (n) {
    auto chunk = std::min<size_t>(n, UINT32_MAX);
    CC_SHA256_Update(&ctx, p, CC_LONG(chunk));
    p += chunk;
    n -= chunk;
  }
  uint8_t digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(digest, &ctx);
  std::ostringstream out;
  for (auto c : digest)
    out << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
  return out.str();
}
bool atomic_write(const std::string &name, const std::string &data) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path output(name), parent = output.parent_path();
  if (parent.empty())
    parent = ".";
  if (fs::exists(output, ec) &&
      (!fs::is_regular_file(output, ec) || fs::is_symlink(fs::symlink_status(output, ec))))
    return false;
  fs::create_directories(parent, ec);
  if (ec)
    return false;
  std::string pattern = (parent / ".xt4-output-XXXXXX").string();
  std::vector<char> path(pattern.begin(), pattern.end());
  path.push_back(0);
  int fd = mkstemp(path.data());
  if (fd < 0)
    return false;
  bool ok = true;
  size_t offset = 0;
  while (offset < data.size()) {
    ssize_t n = write(fd, data.data() + offset, data.size() - offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    offset += size_t(n);
  }
  if (ok && fsync(fd) != 0)
    ok = false;
  if (close(fd) != 0)
    ok = false;
  if (ok && rename(path.data(), name.c_str()) != 0)
    ok = false;
  if (!ok)
    unlink(path.data());
  return ok;
}
std::string distribution_json(std::vector<uint64_t> values) {
  if (values.empty())
    return "null";
  std::sort(values.begin(), values.end());
  size_t n = values.size();
  double median =
      n % 2 ? double(values[n / 2]) : (double(values[n / 2 - 1]) + double(values[n / 2])) / 2;
  std::ostringstream out;
  out << std::setprecision(9) << "{\"median_ms\":" << median / 1e6
      << ",\"p95_ms\":" << values[(n * 95 + 99) / 100 - 1] / 1e6
      << ",\"max_ms\":" << values.back() / 1e6 << "}";
  return out.str();
}
std::string host_identity_json() {
  struct utsname u{};
  uname(&u);
  char cpu[256]{};
  size_t bytes = sizeof cpu;
  if (sysctlbyname("machdep.cpu.brand_string", cpu, &bytes, nullptr, 0) != 0)
    cpu[0] = 0;
  return "{\"os\":" + json_string(u.sysname) + ",\"release\":" + json_string(u.release) +
         ",\"arch\":" + json_string(u.machine) + ",\"cpu\":" + json_string(cpu) + "}";
}
