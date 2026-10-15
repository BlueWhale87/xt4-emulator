#pragma once
#include "engine.hpp"
#include "xt4_face.h"
#include <atomic>
struct Candidate {
  xt4_face_box box;
  uint32_t index;
};
struct xt4_face_context {
  Arena arena;
  Engine engine;
  RawHeads heads;
  xt4_face_config config{};
  std::atomic<uint32_t> busy{0};
  uint64_t control_bytes = 0, graph_ns = 0, postprocess_ns = 0;
  bool clock_available = false;
  uint8_t *prepared = nullptr;
  xt4_face_box *reserved_results = nullptr;
  Candidate *candidates = nullptr;
  Candidate *raw_candidates = nullptr;
  void *pipeline = nullptr;
  void *overlay = nullptr;
};
uint32_t xt4_select_boxes(const Candidate *, uint32_t, const xt4_face_config &,
                          Candidate *workspace, xt4_face_box *, uint32_t, xt4_face_result &);
bool xt4_context_live(xt4_face_context *);
uint32_t xt4_context_enter(xt4_face_context *);
void xt4_context_leave();
