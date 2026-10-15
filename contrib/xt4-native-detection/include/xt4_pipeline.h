#ifndef XT4_PIPELINE_H
#define XT4_PIPELINE_H
#include "xt4_face.h"
#ifdef __cplusplus
extern "C" {
#endif
#define XT4_MODE_DISABLED 0u
#define XT4_MODE_RECORDED 1u
#define XT4_MODE_SYNTHETIC 2u
#define XT4_MODE_VERIFIED_PREVIEW 3u
typedef struct xt4_worker xt4_worker;
typedef struct xt4_result_exchange xt4_result_exchange;
typedef struct {
  void *user;
  uint32_t (*try_acquire)(void *, xt4_frame_view *, uint64_t *);
  void (*release)(void *, uint64_t);
} xt4_frame_provider;
typedef struct {
  void *user;
  uint32_t (*try_enter)(void *);
  void (*leave)(void *);
} xt4_sync;
typedef struct {
  uint32_t struct_size, abi_version, count;
  xt4_face_box boxes[16];
  xt4_prepare_transform prepare;
  uint64_t capture_ns, generation, epoch;
  uint32_t display_id;
} xt4_result_snapshot;
typedef struct {
  uint32_t struct_size, abi_version, mode;
  xt4_frame_provider provider;
  xt4_sync sync;
  xt4_now_ns_fn now_ns;
  void *clock_user;
  const xt4_result_snapshot *synthetic;
} xt4_pipeline_config;
typedef struct {
  uint32_t struct_size, abi_version, mode, running;
  uint64_t epoch, dropped_samples;
} xt4_worker_state;
typedef struct {
  uint32_t struct_size, abi_version, clock_available;
  uint64_t prepare_ns, publication_ns;
} xt4_worker_timings;
uint32_t xt4_pipeline_init(xt4_face_context *, const xt4_pipeline_config *, xt4_worker **,
                           xt4_result_exchange **);
uint32_t xt4_pipeline_destroy(xt4_worker *);
uint32_t xt4_worker_enable(xt4_worker *, uint32_t);
uint32_t xt4_worker_disable(xt4_worker *);
uint32_t xt4_worker_step(xt4_worker *);
uint32_t xt4_worker_get_state(xt4_worker *, xt4_worker_state *);
uint32_t xt4_worker_get_timings(xt4_worker *, xt4_worker_timings *);
uint32_t xt4_result_publish(xt4_result_exchange *, const xt4_result_snapshot *);
uint32_t xt4_result_invalidate(xt4_result_exchange *);
uint32_t xt4_result_try_read(xt4_result_exchange *, uint64_t, uint64_t, uint64_t, uint64_t,
                             xt4_result_snapshot *);
#ifdef __cplusplus
}
#endif
#endif
