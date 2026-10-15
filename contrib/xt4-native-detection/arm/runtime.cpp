// Minimal services for this standalone single-core diagnostic, not firmware stubs.
#include <cstddef>
#include <cstdint>
extern "C" {
void *memcpy(void *dst, const void *src, std::size_t n) {
  auto *d = static_cast<unsigned char *>(dst);
  auto *s = static_cast<const unsigned char *>(src);
  for (std::size_t i = 0; i < n; i++)
    d[i] = s[i];
  return dst;
}
void *memset(void *dst, int value, std::size_t n) {
  auto *d = static_cast<unsigned char *>(dst);
  for (std::size_t i = 0; i < n; i++)
    d[i] = static_cast<unsigned char>(value);
  return dst;
}
void *memmove(void *dst, const void *src, std::size_t n) {
  auto *d = static_cast<unsigned char *>(dst);
  auto *s = static_cast<const unsigned char *>(src);
  if (reinterpret_cast<uintptr_t>(d) < reinterpret_cast<uintptr_t>(s))
    for (std::size_t i = 0; i < n; i++)
      d[i] = s[i];
  else
    while (n) {
      --n;
      d[n] = s[n];
    }
  return dst;
}
int memcmp(const void *a, const void *b, std::size_t n) {
  auto *x = static_cast<const unsigned char *>(a);
  auto *y = static_cast<const unsigned char *>(b);
  for (std::size_t i = 0; i < n; i++)
    if (x[i] != y[i])
      return int(x[i]) - int(y[i]);
  return 0;
}
int *__errno() {
  static int value;
  return &value;
}
void __aeabi_memcpy(void *d, const void *s, std::size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, std::size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, std::size_t n) { memcpy(d, s, n); }
void __aeabi_memclr(void *d, std::size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, std::size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, std::size_t n) { memset(d, 0, n); }
void __aeabi_memset(void *d, std::size_t n, int c) { memset(d, c, n); }
void __aeabi_memset4(void *d, std::size_t n, int c) { memset(d, c, n); }
void __aeabi_memset8(void *d, std::size_t n, int c) { memset(d, c, n); }
}
