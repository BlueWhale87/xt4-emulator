#pragma once
// Small compiler/libm boundary shared by host and freestanding builds.
// libm supplies arithmetic; no hosted C++ special-function headers are needed.
extern "C" float expf(float);
extern "C" float sqrtf(float);
extern "C" double floor(double);
namespace xt4_math {
inline float exp(float x) { return ::expf(x); }
inline float sqrt(float x) { return ::sqrtf(x); }
inline double floor(double x) { return ::floor(x); }
template <typename T> inline bool isfinite(T x) { return __builtin_isfinite(x); }
} // namespace xt4_math
