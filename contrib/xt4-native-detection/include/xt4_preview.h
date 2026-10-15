#ifndef XT4_PREVIEW_H
#define XT4_PREVIEW_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define XT4_ABI_VERSION 1u
#define XT4_BGR8 1u
typedef struct {
  float x0, y0, x1, y1;
} xt4_box_edges;
typedef struct {
  uint32_t struct_size, abi_version, format;
  const uint8_t *data;
  uint64_t data_bytes;
  uint32_t width, height, stride, source_x, source_y, source_width, source_height, display_id;
  uint64_t generation, capture_ns;
} xt4_frame_view;
typedef struct {
  uint32_t struct_size, abi_version, source_x, source_y, source_width, source_height;
  uint32_t resized_width, resized_height, pad_left, pad_right, pad_top, pad_bottom, display_id;
  float scale_x, scale_y;
  uint64_t generation;
} xt4_prepare_transform;
typedef struct {
  uint32_t struct_size, abi_version, display_id;
  uint64_t generation;
  float matrix[9];
  xt4_box_edges viewport;
} xt4_display_transform;
uint32_t xt4_prepare_bgr160(const xt4_frame_view *, uint8_t *, uint64_t, xt4_prepare_transform *);
uint32_t xt4_map_box(const xt4_box_edges *, const xt4_prepare_transform *,
                     const xt4_display_transform *, xt4_box_edges *);
#ifdef __cplusplus
}
#endif
#endif
