#pragma once
#include "context.hpp"
#include "xt4_pipeline.h"
struct xt4_result_exchange {
  xt4_pipeline_config config{};
  xt4_result_snapshot snapshot{};
  bool has_snapshot = false, has_read_clock = false;
  uint64_t last_read_ns = 0;
};
struct xt4_worker {
  xt4_face_context *ctx = nullptr;
  xt4_result_exchange exchange;
  xt4_result_snapshot synthetic{};
  bool has_synthetic = false;
  std::atomic<uint32_t> running{0}, faulted{0}, dropped{0};
  uint64_t prepare_ns = 0, publication_ns = 0;
  bool timing_available = false;
};
extern std::atomic<xt4_worker *> xt4_live_worker;
extern std::atomic<xt4_result_exchange *> xt4_live_exchange;
extern std::atomic<uint32_t> xt4_pipeline_mode, xt4_pipeline_epoch;
extern std::atomic<uint32_t> xt4_pipeline_exhausted;
bool xt4_pipeline_enter();
void xt4_pipeline_leave();
uint32_t xt4_pipeline_bump_epoch();
bool xt4_exchange_live(xt4_result_exchange *);
struct PipelineRelease {
  ~PipelineRelease() { xt4_pipeline_leave(); }
};
