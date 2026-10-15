#include "build_identity.h"
#include "host_overlay.hpp"
#include "report_json.hpp"
#include "xt4_overlay.h"
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mach-o/dyld.h>
#include <map>
#include <sstream>
#include <vector>
uint32_t host_now(void *, uint64_t *out) {
  *out = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
  return 0;
}
static uint64_t tick() {
  uint64_t n = 0;
  host_now(nullptr, &n);
  return n;
}
static std::string read_file(const std::string &path, uint64_t maximum = 268435456) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    throw std::runtime_error("Cannot read file");
  auto length = f.tellg();
  if (length < 0 || uint64_t(length) > maximum)
    throw std::runtime_error("File exceeds input limit");
  f.seekg(0);
  std::string data(size_t(length), 0);
  if (!f.read(data.data(), std::streamsize(data.size())) ||
      f.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("File changed or read failed");
  return data;
}
static bool aliases(const std::string &a, const std::string &b) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (fs::equivalent(a, b, ec) && !ec)
    return true;
  ec.clear();
  auto first = fs::weakly_canonical(a, ec);
  if (ec)
    return false;
  auto second = fs::weakly_canonical(b, ec);
  return !ec && first == second;
}
static int exit_status(uint32_t status) {
  if (status == 0)
    return 0;
  if (status == XT4_FACE_NO_MEMORY)
    return 4;
  if (status == XT4_FACE_INVALID || status == XT4_FACE_UNSUPPORTED)
    return 2;
  if (status == XT4_FACE_UNAVAILABLE || status == XT4_FACE_BUSY)
    return 3;
  return 5;
}
struct FileProvider {
  xt4_frame_view view{};
  uint64_t issued = 0, active = 0;
  bool error = false;
  static uint32_t acquire(void *u, xt4_frame_view *out, uint64_t *token) {
    auto &p = *static_cast<FileProvider *>(u);
    if (p.active)
      return XT4_FACE_BUSY;
    *out = p.view;
    out->capture_ns = tick();
    *token = ++p.issued;
    p.active = *token;
    return 0;
  }
  static void release(void *u, uint64_t token) {
    auto &p = *static_cast<FileProvider *>(u);
    if (!p.active || p.active != token)
      p.error = true;
    p.active = 0;
  }
};
// The runner is serial; the backend still exercises the same try-lock interface.
struct SerialSync {
  bool held = false;
  static uint32_t enter(void *u) {
    auto &s = *static_cast<SerialSync *>(u);
    if (s.held)
      return XT4_FACE_BUSY;
    s.held = true;
    return 0;
  }
  static void leave(void *u) { static_cast<SerialSync *>(u)->held = false; }
};
struct Resources {
  xt4_face_context *ctx = nullptr;
  xt4_worker *worker = nullptr;
  xt4_overlay_backend *backend = nullptr;
  ~Resources() {
    if (backend && backend->internal)
      xt4_overlay_destroy(backend);
    if (worker)
      xt4_pipeline_destroy(worker);
    if (ctx)
      xt4_face_destroy(ctx);
  }
};
static bool number(const std::string &s, uint64_t &out) {
  if (s.empty() || s[0] == '-' || s[0] == '+')
    return false;
  auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
static int run(int argc, char **argv) {
  namespace fs = std::filesystem;
  std::map<std::string, std::string> args;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc || !args.emplace(argv[i], argv[i + 1]).second)
      return 2;
  }
  for (auto &arg : args)
    if (arg.first != "--input" && arg.first != "--width" && arg.first != "--height" &&
        arg.first != "--stride" && arg.first != "--mode" && arg.first != "--warmup" &&
        arg.first != "--samples" && arg.first != "--report" && arg.first != "--arena-bytes" &&
        arg.first != "--overlay")
      return 2;
  for (const char *k : {"--input", "--width", "--height", "--stride", "--mode", "--report"})
    if (!args.count(k) || args[k].empty())
      return 2;
  std::string mode = args["--mode"];
  if (mode != "recorded" && mode != "synthetic" && mode != "verified-preview")
    return 2;
  if (mode == "verified-preview") {
    xt4_frame_provider unused{};
    return exit_status(xt4_firmware_provider_open(nullptr, &unused));
  }
  uint64_t width = 0, height = 0, stride = 0, warmup = 20, samples = 200, arena_bytes = 8388608;
  if (!number(args["--width"], width) || !number(args["--height"], height) ||
      !number(args["--stride"], stride) || !width || !height || width > UINT32_MAX ||
      height > UINT32_MAX || stride > UINT32_MAX || stride < width * 3)
    return 2;
  if (args.count("--warmup") && !number(args["--warmup"], warmup))
    return 2;
  if (args.count("--samples") && !number(args["--samples"], samples))
    return 2;
  if (args.count("--arena-bytes") && !number(args["--arena-bytes"], arena_bytes))
    return 2;
  if (!samples || samples > 1000000 || warmup > 1000000 || !arena_bytes || arena_bytes > 8388608)
    return 2;
  if (aliases(args["--input"], args["--report"]) ||
      (args.count("--overlay") && (aliases(args["--input"], args["--overlay"]) ||
                                   aliases(args["--report"], args["--overlay"]))))
    return 2;
  std::error_code ec;
  if (!fs::is_regular_file(args["--input"], ec))
    return 2;
  uint64_t file_bytes = fs::file_size(args["--input"], ec);
  uint64_t extent = (height - 1) * stride + width * 3, max_extent = height * stride;
  if (ec || file_bytes < extent || file_bytes > max_extent || file_bytes > 268435456)
    return 2;
  auto input = read_file(args["--input"], file_bytes);
  if (input.size() != file_bytes)
    return 2;
  auto fixture_hash = sha256_bytes(input.data(), input.size());
  std::vector<uint8_t> region(size_t(arena_bytes) + 127);
  void *aligned = reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(region.data()) + 127) &
                                           ~uintptr_t(127));
  HostCanvas canvas;
  auto backend = canvas.backend();
  FileProvider provider;
  SerialSync sync;
  Resources resources;
  resources.backend = &backend;
  xt4_face_config cfg{};
  cfg.struct_size = sizeof cfg;
  cfg.abi_version = 1;
  std::char_traits<char>::copy(cfg.model_sha256, XT4_FACE_MODEL_SHA256, 65);
  cfg.confidence_threshold = .8f;
  cfg.nms_threshold = .3f;
  cfg.top_k = 1000;
  cfg.keep_top_k = 16;
  cfg.memory_limit = arena_bytes;
  cfg.now_ns = host_now;
  uint32_t created = xt4_face_create(&cfg, aligned, arena_bytes, &resources.ctx);
  if (created) {
    std::string failure = "{\"success\":false,\"mode\":" + json_string(mode) +
                          ",\"status\":" + std::to_string(created) +
                          ",\"arena_limit_bytes\":" + std::to_string(arena_bytes) + "}\n";
    return atomic_write(args["--report"], failure) ? exit_status(created) : 5;
  }
  auto &view = provider.view;
  view.struct_size = sizeof view;
  view.abi_version = 1;
  view.format = XT4_BGR8;
  view.data = reinterpret_cast<const uint8_t *>(input.data());
  view.data_bytes = input.size();
  view.width = view.source_width = uint32_t(width);
  view.height = view.source_height = uint32_t(height);
  view.stride = uint32_t(stride);
  view.generation = 1;
  xt4_prepare_transform prepared{};
  if (xt4_prepare_bgr160(&view, canvas.base.data(), canvas.base.size(), &prepared) != 0)
    return 2;
  xt4_result_snapshot synthetic{};
  synthetic.struct_size = sizeof synthetic;
  synthetic.abi_version = 1;
  synthetic.count = 1;
  synthetic.boxes[0] = {{40, 40, 80, 80}, 1};
  synthetic.prepare = prepared;
  synthetic.generation = 1;
  xt4_pipeline_config pc{};
  pc.struct_size = sizeof pc;
  pc.abi_version = 1;
  pc.provider = {&provider, FileProvider::acquire, FileProvider::release};
  pc.sync = {&sync, SerialSync::enter, SerialSync::leave};
  pc.now_ns = host_now;
  pc.synthetic = &synthetic;
  xt4_result_exchange *exchange = nullptr;
  uint32_t initialized = xt4_pipeline_init(resources.ctx, &pc, &resources.worker, &exchange);
  if (initialized)
    return exit_status(initialized);
  canvas.worker = resources.worker;
  initialized = xt4_worker_enable(resources.worker,
                                  mode == "synthetic" ? XT4_MODE_SYNTHETIC : XT4_MODE_RECORDED);
  if (initialized)
    return exit_status(initialized);
  initialized = xt4_overlay_init(resources.ctx, &backend);
  if (initialized)
    return exit_status(initialized);
  xt4_display_transform display{};
  display.struct_size = sizeof display;
  display.abi_version = 1;
  display.generation = 1;
  display.matrix[0] = prepared.scale_x;
  display.matrix[4] = prepared.scale_y;
  display.matrix[8] = 1;
  display.matrix[2] = float(prepared.pad_left) - prepared.scale_x * prepared.source_x;
  display.matrix[5] = float(prepared.pad_top) - prepared.scale_y * prepared.source_y;
  display.viewport = {0, 0, 160, 160};
  std::vector<uint64_t> prep_ns, graph_ns, post_ns, publish_ns, render_ns, total_ns;
  for (auto *v : {&prep_ns, &graph_ns, &post_ns, &publish_ns, &render_ns, &total_ns})
    v->reserve(samples);
  xt4_face_stats stats{};
  stats.struct_size = sizeof stats;
  stats.abi_version = 1;
  xt4_worker_state worker_state{sizeof worker_state, 1, 0, 0, 0, 0};
  xt4_worker_timings worker_timings{};
  worker_timings.struct_size = sizeof worker_timings;
  worker_timings.abi_version = 1;
  xt4_result_snapshot last{};
  bool result_available = false;
  uint64_t result_age = 0, skipped_renders = 0;
  for (uint64_t i = 0; i < warmup + samples; i++) {
    uint64_t start = tick();
    uint32_t status = xt4_worker_step(resources.worker);
    if (status)
      return exit_status(status);
    if (provider.active || provider.error)
      return 5;
    if (xt4_worker_get_state(resources.worker, &worker_state) ||
        xt4_worker_get_timings(resources.worker, &worker_timings) ||
        xt4_face_get_stats(resources.ctx, &stats))
      return 5;
    uint64_t now = tick();
    result_available =
        xt4_result_try_read(exchange, worker_state.epoch, 1, now, 250000000, &last) == 0;
    if (result_available)
      result_age = now - last.capture_ns;
    canvas.pixels = canvas.base;
    canvas.draw_count = 0;
    uint64_t before_render = tick();
    uint32_t draw = xt4_overlay_draw(exchange, &backend, &display, worker_state.epoch, 1);
    uint64_t finished = tick();
    if (draw != 0 && draw != XT4_FACE_UNAVAILABLE && draw != XT4_FACE_BUSY)
      return 5;
    if (i >= warmup) {
      if (mode == "recorded" && worker_timings.clock_available)
        prep_ns.push_back(worker_timings.prepare_ns);
      if (mode == "recorded" && stats.clock_available) {
        graph_ns.push_back(stats.graph_ns);
        post_ns.push_back(stats.postprocess_ns);
      }
      if (worker_timings.clock_available)
        publish_ns.push_back(worker_timings.publication_ns);
      render_ns.push_back(finished - before_render);
      total_ns.push_back(finished - start);
      if (draw)
        ++skipped_renders;
    }
  }
  if (xt4_face_get_stats(resources.ctx, &stats))
    return 5;
  std::ostringstream config;
  config << "{\"confidence\":0.8,\"nms\":0.3,\"top_k\":1000,\"keep_top_k\":16,\"width\":" << width
         << ",\"height\":" << height << ",\"stride\":" << stride
         << ",\"arena_bytes\":" << arena_bytes << ",\"mode\":" << json_string(mode) << "}";
  auto config_json = config.str();
  std::string sizes = "null";
  {
    std::ifstream f(XT4_BINARY_SIZES_PATH);
    if (f)
      sizes = {std::istreambuf_iterator<char>(f), {}};
  }
  char executable[4096];
  uint32_t capacity = sizeof executable;
  std::string binary_hash;
  if (_NSGetExecutablePath(executable, &capacity) == 0) {
    auto binary = read_file(executable);
    binary_hash = sha256_bytes(binary.data(), binary.size());
  }
  if (args.count("--overlay")) {
    std::string ppm = "P6\n160 160\n255\n";
    ppm.resize(ppm.size() + 76800);
    size_t offset = ppm.size() - 76800;
    for (size_t i = 0; i < 76800; i += 3) {
      ppm[offset + i] = char(canvas.pixels[i + 2]);
      ppm[offset + i + 1] = char(canvas.pixels[i + 1]);
      ppm[offset + i + 2] = char(canvas.pixels[i]);
    }
    if (!atomic_write(args["--overlay"], ppm))
      return 5;
  }
  std::ostringstream report;
  report << std::setprecision(9) << "{\"success\":true,\"mode\":" << json_string(mode)
         << ",\"synthetic_fixture\":" << (mode == "synthetic" ? "\"centered-box-v1\"" : "null")
         << ",\"input_path\":" << json_string(args["--input"])
         << ",\"fixture_sha256\":" << json_string(fixture_hash) << ",\"model_sha256\":\""
         << XT4_FACE_MODEL_SHA256
         << "\",\"model_variant\":\"libfacedetection-native-rounded-fp32\",\"config_canonical\":"
         << json_string(config_json) << ",\"config_sha256\":"
         << json_string(sha256_bytes(config_json.data(), config_json.size()))
         << ",\"build\":" << XT4_BUILD_METADATA << ",\"binary_sha256\":" << json_string(binary_hash)
         << ",\"host\":" << host_identity_json() << ",\"warmup\":" << warmup
         << ",\"samples\":" << samples
         << ",\"stages\":{\"preparation\":" << distribution_json(prep_ns)
         << ",\"graph\":" << distribution_json(graph_ns)
         << ",\"postprocessing\":" << distribution_json(post_ns)
         << ",\"publication\":" << distribution_json(publish_ns)
         << ",\"rendering\":" << distribution_json(render_ns)
         << ",\"total\":" << distribution_json(total_ns) << "}"
         << ",\"memory\":{\"limit_bytes\":" << arena_bytes
         << ",\"current_bytes\":" << stats.current_bytes << ",\"peak_bytes\":" << stats.peak_bytes
         << ",\"allocation_count\":" << stats.allocation_count
         << ",\"failed_allocations\":" << stats.failed_allocations
         << ",\"host_source_bytes_outside_region\":" << input.size()
         << ",\"host_canvas_bytes_outside_region\":153600,\"stack_high_water_bytes\":null},"
            "\"binary_sizes\":"
         << sizes << ",\"dropped_samples\":" << worker_state.dropped_samples
         << ",\"skipped_renders\":" << skipped_renders
         << ",\"result_available\":" << (result_available ? "true" : "false")
         << ",\"last_result_age_ns\":" << (result_available ? std::to_string(result_age) : "null")
         << ",\"clock_available\":" << (worker_timings.clock_available ? "true" : "false")
         << ",\"overlay_kind\":\"host CPU raster; not firmware graphics\",\"overlay_path\":"
         << (args.count("--overlay") ? json_string(args["--overlay"]) : "null") << ",\"boxes\":[";
  if (result_available)
    for (uint32_t i = 0; i < last.count; i++) {
      auto &b = last.boxes[i];
      if (i)
        report << ',';
      report << "{\"score\":" << b.confidence << ",\"x0\":" << b.edges.x0
             << ",\"y0\":" << b.edges.y0 << ",\"x1\":" << b.edges.x1 << ",\"y1\":" << b.edges.y1
             << '}';
    }
  report << "]}\n";
  if (!atomic_write(args["--report"], report.str()))
    return 5;
  std::cout << "Saved " << mode << " benchmark: " << samples << " samples\n";
  return 0;
}
int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::bad_alloc &) {
    std::cerr << "Insufficient host memory\n";
    return 4;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 5;
  }
}
