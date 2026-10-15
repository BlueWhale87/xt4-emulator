#ifndef XT4_OVERLAY_H
#define XT4_OVERLAY_H
#include "xt4_pipeline.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  float red, green, blue, alpha, stroke_width;
  uint32_t filled, identity_transform;
  xt4_box_edges clip;
} xt4_overlay_style;
typedef struct {
  uint32_t struct_size, abi_version;
  void *user;
  uint64_t (*required_pool_bytes)(void *, uint32_t);
  uint32_t (*init)(void *, void *, uint64_t, uint32_t);
  uint32_t (*destroy)(void *);
  uint32_t (*validate_context)(void *);
  uint32_t (*validate_identity)(void *, uint64_t, uint64_t, uint32_t);
  uint32_t (*save_state)(void *);
  uint32_t (*set_style)(void *, const xt4_overlay_style *);
  uint32_t (*draw_rect)(void *, const xt4_box_edges *);
  uint32_t (*restore_state)(void *);
  xt4_now_ns_fn now_ns;
  void *clock_user;
  void *internal;
  uint64_t max_result_age_ns; /* zero selects the 250 ms default */
} xt4_overlay_backend;
typedef struct {
  uint32_t struct_size, abi_version;
  char firmware_sha256[65];
  uint64_t memory_budget;
  uint32_t runtime_mapping_verified, pixel_format, stride, width, height, layer;
  uint32_t context_owner_verified, frame_lifetime_verified, cache_verified, abi_verified;
} xt4_firmware_profile;
uint32_t xt4_overlay_init(xt4_face_context *, xt4_overlay_backend *);
uint32_t xt4_overlay_draw(xt4_result_exchange *, xt4_overlay_backend *,
                          const xt4_display_transform *, uint64_t, uint64_t);
uint32_t xt4_overlay_destroy(xt4_overlay_backend *);
uint32_t xt4_firmware_provider_open(const xt4_firmware_profile *, xt4_frame_provider *);
uint32_t xt4_firmware_overlay_open(const xt4_firmware_profile *, xt4_overlay_backend *);
#ifdef __cplusplus
}
#endif
#endif
