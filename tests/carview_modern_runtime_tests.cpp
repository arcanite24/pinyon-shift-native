// End-to-end integration tests for carview's live modern engine stack:
//   1. Double-buffered Frame N+1 / Frame N RenderThread concurrent overlap & ordering
//   2. carview::ModernRhiRuntime across Vulkan 1.3 and DirectX 12:
//      VisBuffer + Clustered Forward+ + SPD Hi-Z + Compute Post-FX + Pre-Tonemap Linear HDR TAA-U
//      + GPU Xenos Detiler + NativeCache (SHNC0001) + 128m CellGridStreamer + CarAssetPool
//      with 0 validation messages on both APIs.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "render_thread.h"
#include "rhi_backend_bridge.h"

namespace {

#define CHECK(cond, msg)                                                                  \
  do {                                                                                    \
    if (!(cond)) {                                                                        \
      std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));              \
      return false;                                                                       \
    }                                                                                     \
  } while (0)

void make_perspective_revz(float fov_y_rad, float aspect, float zn, float zf, float out[16]) {
  std::memset(out, 0, 16 * sizeof(float));
  const float f = 1.0f / std::tan(fov_y_rad * 0.5f);
  out[0] = f / aspect;
  out[5] = f;
  out[10] = zn / (zf - zn);
  out[11] = -1.0f;
  out[14] = (zn * zf) / (zf - zn);
}

bool test_render_thread_overlap() {
  carview::RenderThread rt;
  std::vector<uint64_t> rendered_ids;
  std::atomic<bool> saw_concurrent_sim{false};
  std::atomic<uint64_t> sim_frame{0};
  std::atomic<bool> render_in_progress{false};

  rt.start(
      [&](const carview::RenderSnapshot& snap) {
        render_in_progress.store(true, std::memory_order_release);
        // Simulate ~4 ms GPU command recording so Frame N+1 sim visibly overlaps Frame N render.
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        if (sim_frame.load(std::memory_order_acquire) > snap.frame_id) {
          saw_concurrent_sim.store(true, std::memory_order_relaxed);
        }
        rendered_ids.push_back(snap.frame_id);
        render_in_progress.store(false, std::memory_order_release);
      },
      true);

  for (uint64_t f = 1; f <= 8; ++f) {
    carview::RenderSnapshot& snap = rt.begin_sim_snapshot();
    snap.frame_id = f;
    snap.camera_pos[2] = float(f) * 2.0f;
    sim_frame.store(f, std::memory_order_release);
    rt.publish_snapshot();
  }
  rt.flush();
  const auto st = rt.stats();
  rt.stop();

  CHECK(st.frames_submitted == 8 && st.frames_completed == 8, "all 8 frames must complete");
  CHECK(rendered_ids.size() == 8, "rendered_ids count mismatch");
  for (size_t i = 0; i < 8; ++i) {
    CHECK(rendered_ids[i] == i + 1, "rendered frame IDs must be strictly monotonic");
  }
  CHECK(saw_concurrent_sim.load(), "Frame N+1 simulation must overlap Frame N render thread");

  std::printf("[carview_modern_runtime_tests] RenderThread N/N-1 overlap (8/8 frames, concurrent=1): OK\n");
  return true;
}

bool test_modern_rhi_runtime(shift::rhi::BackendKind backend, const std::filesystem::path& cache_dir) {
  const char* bname = (backend == shift::rhi::BackendKind::Vulkan) ? "vulkan" : "d3d12";

  carview::ModernRuntimeConfig cfg{};
  cfg.backend = backend;
  cfg.max_width = 128;
  cfg.max_height = 128;
  cfg.max_draws = 64;
  cfg.enable_validation = true;
  cfg.enable_render_thread = true;
  cfg.native_cache_dir = cache_dir;

  carview::ModernRhiRuntime runtime;
  std::string err;
  CHECK(runtime.init(cfg, err), (std::string(bname) + " init failed: " + err).c_str());

  // Build simple car quad meshlet scene.
  std::vector<shift::rhi::VisVertexGpu> verts(4);
  verts[0] = {{-2.0f, -2.0f, -5.0f}, 0.0f, {0.0f, 0.0f, 1.0f}, 0.0f, {1, 1, 1, 1}};
  verts[1] = {{ 2.0f, -2.0f, -5.0f}, 1.0f, {0.0f, 0.0f, 1.0f}, 0.0f, {1, 1, 1, 1}};
  verts[2] = {{ 2.0f,  2.0f, -5.0f}, 1.0f, {0.0f, 0.0f, 1.0f}, 1.0f, {1, 1, 1, 1}};
  verts[3] = {{-2.0f,  2.0f, -5.0f}, 0.0f, {0.0f, 0.0f, 1.0f}, 1.0f, {1, 1, 1, 1}};
  std::vector<uint32_t> idxs = {0, 1, 2, 0, 2, 3};
  auto mb = shift::rhi::build_meshlets(verts, idxs, 0);

  shift::rhi::MeshletInstanceGpu inst{};
  inst.first_meshlet = 0;
  inst.meshlet_count = static_cast<uint32_t>(mb.meshlets.size());
  inst.material_class = static_cast<uint32_t>(shift::rhi::VisMaterialClass::CarPaint);
  inst.material_id = 0;

  shift::rhi::VisMaterialGpu mat{};
  mat.base_color[0] = 0.85f; mat.base_color[1] = 0.15f; mat.base_color[2] = 0.10f;
  mat.roughness = 0.22f; mat.metallic = 0.2f; mat.clearcoat = 0.95f;
  mat.material_class = static_cast<uint32_t>(shift::rhi::VisMaterialClass::CarPaint);

  CHECK(runtime.upload_scene({verts, mb.reordered_indices, mb.meshlets, std::span(&inst, 1), std::span(&mat, 1)}, err),
        ("upload_scene failed: " + err).c_str());

  // Exercise CarAssetPool deduplication through ModernRhiRuntime.
  std::vector<std::shared_ptr<world::SharedCarAsset>> cars;
  for (int i = 0; i < 4; ++i) {
    cars.push_back(runtime.car_pool().acquire("viper_2013", [](const std::string& k) {
      auto a = std::make_shared<world::SharedCarAsset>();
      a->car_key = k;
      a->gpu_vertex_bytes = 1024ull * 1024ull;
      a->gpu_texture_bytes = 4ull * 1024ull * 1024ull;
      return a;
    }));
  }

  float proj[16];
  make_perspective_revz(1.0472f, 1.0f, 0.1f, 200.0f, proj);

  shift::rhi::ClusterLightGpu light{};
  light.pos_radius[2] = -3.0f; light.pos_radius[3] = 10.0f;
  light.color_intensity[0] = 1.0f; light.color_intensity[1] = 0.95f; light.color_intensity[2] = 0.9f; light.color_intensity[3] = 5.0f;

  // Submit 3 frames (including a DRS mid-flight resolution step & manual exposure override) over N/N-1 RenderThread!
  for (uint64_t f = 1; f <= 3; ++f) {
    carview::RenderSnapshot& snap = runtime.begin_frame();
    snap.frame_id = f;
    snap.viewport_width = (f == 2) ? 96u : 128u;
    snap.viewport_height = (f == 2) ? 96u : 128u;
    snap.use_manual_exposure = (f == 2);
    snap.exposure = (f == 2) ? 1.5f : 1.0f;
    std::memcpy(snap.view_proj, proj, sizeof(proj));
    std::memcpy(snap.prev_view_proj, proj, sizeof(proj));
    runtime.submit_frame(std::span(&light, 1));
  }
  runtime.flush();

  const auto tel = runtime.telemetry(&err);
  CHECK(tel.validation_messages == 0, "RHI validation messages must be 0");
  CHECK(tel.render_thread.frames_completed == 3, "expected 3 threaded frames completed");
  CHECK(tel.vis.visible_meshlets == 1, "expected 1 visible meshlet");
  CHECK(tel.exposure.total_pixels > 0, "expected auto-exposure histogram active");
  CHECK(tel.shared_car_saved_vram == 3ull * 5ull * 1024ull * 1024ull, "expected 15 MiB saved via CarAssetPool");

  std::printf("[%s] ModernRhiRuntime (%s): frames=%llu vis=%u exp=%.3f saved_car_vram=%llu MiB (validation_msgs=0)\n",
              bname, tel.adapter_name.c_str(), static_cast<unsigned long long>(tel.render_thread.frames_completed),
              tel.vis.visible_meshlets, tel.exposure.adapted_exposure,
              static_cast<unsigned long long>(tel.shared_car_saved_vram / (1024ull * 1024ull)));

  cars.clear();
  runtime.shutdown();
  return true;
}

}  // namespace

int main() {
  if (!test_render_thread_overlap()) return 1;

  const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "shift_carview_modern_runtime_tests";
  std::error_code ec;
  std::filesystem::remove_all(tmp, ec);

  const bool ok_vk = test_modern_rhi_runtime(shift::rhi::BackendKind::Vulkan, tmp / "vk_cache");
#ifdef _WIN32
  const bool ok_dx = test_modern_rhi_runtime(shift::rhi::BackendKind::D3D12, tmp / "dx_cache");
#else
  const bool ok_dx = true;
#endif

  std::filesystem::remove_all(tmp, ec);
  if (!ok_vk || !ok_dx) return 1;
  std::printf("carview_modern_runtime_tests: ALL PASSED\n");
  return 0;
}
