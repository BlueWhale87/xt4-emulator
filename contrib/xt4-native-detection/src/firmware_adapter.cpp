#include "xt4_overlay.h"
uint32_t xt4_firmware_provider_open(const xt4_firmware_profile *, xt4_frame_provider *out) {
  if (out)
    *out = {};
  return XT4_FACE_UNAVAILABLE;
}
uint32_t xt4_firmware_overlay_open(const xt4_firmware_profile *, xt4_overlay_backend *out) {
  if (out)
    *out = {};
  return XT4_FACE_UNAVAILABLE;
}
