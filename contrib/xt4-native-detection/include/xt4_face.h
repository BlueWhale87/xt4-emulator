#ifndef XT4_FACE_H
#define XT4_FACE_H
#include "xt4_preview.h"
#ifdef __cplusplus
extern "C" {
#endif
#define XT4_FACE_OK 0u
#define XT4_FACE_INVALID 1u
#define XT4_FACE_UNSUPPORTED 2u
#define XT4_FACE_NO_MEMORY 3u
#define XT4_FACE_BUSY 4u
#define XT4_FACE_UNAVAILABLE 5u
#define XT4_FACE_INTERNAL 6u
#define XT4_FACE_MODEL_SHA256 "cf019963f413528540d6de741e72eea4f2f0b572ca85b22dd4baa8fc71b42bb6"
typedef struct xt4_face_context xt4_face_context;
typedef uint32_t (*xt4_now_ns_fn)(void *, uint64_t *);
typedef struct {
  uint32_t struct_size, abi_version;
  char model_sha256[65];
  float confidence_threshold, nms_threshold;
  uint32_t top_k, keep_top_k;
  uint64_t memory_limit;
  xt4_now_ns_fn now_ns;
  void *clock_user;
} xt4_face_config;
typedef struct {
  uint32_t struct_size, abi_version, format;
  const uint8_t *data;
  uint64_t data_bytes;
  uint32_t width, height, stride;
} xt4_face_image;
typedef struct {
  xt4_box_edges edges;
  float confidence;
} xt4_face_box;
typedef struct {
  uint32_t struct_size, abi_version, count, truncated;
} xt4_face_result;
typedef struct {
  uint32_t struct_size, abi_version;
  uint64_t current_bytes, peak_bytes, allocation_count, failed_allocations;
  uint64_t prepared_image_bytes, result_storage_bytes, graph_ns, postprocess_ns;
  uint32_t clock_available;
} xt4_face_stats;
uint32_t xt4_face_create(const xt4_face_config *, void *, uint64_t, xt4_face_context **);
uint32_t xt4_face_detect(xt4_face_context *, const xt4_face_image *, xt4_face_box *, uint32_t,
                         xt4_face_result *);
uint32_t xt4_face_get_stats(xt4_face_context *, xt4_face_stats *);
uint32_t xt4_face_destroy(xt4_face_context *);
#ifdef __cplusplus
}
#endif
#endif
