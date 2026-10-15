#pragma once
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#define CHECK(e)                                                                                   \
  do {                                                                                             \
    if (!(e))                                                                                      \
      throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #e);  \
  } while (0)
struct ProcessResult {
  int exit_status;
  std::string stdout_text, stderr_text;
};
inline std::string read_text(const std::string &p) {
  std::ifstream f(p);
  return {std::istreambuf_iterator<char>(f), {}};
}
inline ProcessResult run_cli(const std::vector<std::string> &args) {
  char out[] = "/tmp/xt4-test-out-XXXXXX", err[] = "/tmp/xt4-test-err-XXXXXX";
  int o = mkstemp(out), e = mkstemp(err);
  CHECK(o >= 0 && e >= 0);
  pid_t p = fork();
  CHECK(p >= 0);
  if (p == 0) {
    dup2(o, STDOUT_FILENO);
    dup2(e, STDERR_FILENO);
    close(o);
    close(e);
    std::vector<char *> a;
    for (auto &x : args)
      a.push_back(const_cast<char *>(x.c_str()));
    a.push_back(nullptr);
    execv(a[0], a.data());
    _exit(127);
  }
  close(o);
  close(e);
  int status = 0;
  while (waitpid(p, &status, 0) < 0)
    CHECK(errno == EINTR);
  ProcessResult r{WIFEXITED(status) ? WEXITSTATUS(status) : 128, read_text(out), read_text(err)};
  unlink(out);
  unlink(err);
  return r;
}
inline void write_bytes(const std::string &p, size_t bytes) {
  std::ofstream f(p, std::ios::binary);
  std::vector<char> b(bytes);
  f.write(b.data(), static_cast<std::streamsize>(b.size()));
  CHECK(f.good());
}
template <class F> int test_main(F f) {
  try {
    f();
    std::cout << "PASS\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
