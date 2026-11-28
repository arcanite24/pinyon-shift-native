// Phase 3 End-to-End Hardware Verification:
//   - 64v/124t Meshlet builder with bounding spheres & normal cones
//   - GPU Frustum + Backface Cone Culling (vis_cull.comp on AsyncCompute)
//   - 64-bit Visibility Buffer (Rg32Uint) + Reverse-Z D32F rasterization (Graphics)
//   - 8x8 LDS Tile Material Classification (vis_classify.comp on AsyncCompute)
//   - Indirect Specialized Compute Material Shading + Analytic Derivatives +
//     3D Froxel Clustered Forward+ Dynamic Lights (vis_shade.comp on AsyncCompute)
//   - Stall-free DRS (96x96 viewport inside 128x128 fixed buffers)
//   - Cross-API Vulkan 1.3 vs DirectX 12 HDR parity & zero validation errors
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "rhi/vis_buffer.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

// IEEE 754 float16 -> float32 helper for reading back Rgba16Sfloat / Rg16Sfloat textures.
float f16_to_f32(uint16_t h) noexcept {
  const uint32_t sign = (static_cast<uint32_t>(h & 0x8000u)) << 16;
  const uint32_t exp = (h >> 10) & 0x1Fu;
  const uint32_t mant = h & 0x03FFu;
  uint32_t bits = 0;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      float f = static_cast<float>(mant) * (1.0f / 16777216.0f);
      return sign ? -f : f;
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (mant << 13);
  } else {
    bits = sign | ((exp + 112u) << 23) | (mant << 13);
  }
  float out = 0.0f;
  std::memcpy(&out, &bits, sizeof(float));
  return out;
}

void mat4_identity(float m[16]) {
  std::memset(m, 0, sizeof(float) * 16);
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_translate(float m[16], float x, float y, float z) {
  mat4_identity(m);
  m[12] = x;
  m[13] = y;
  m[14] = z;
}

void mat4_mul(const float a[16], const float b[16], float out[16]) {
  float r[16]{};
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] +
                       a[1 * 4 + row] * b[c * 4 + 1] +
                       a[2 * 4 + row] * b[c * 4 + 2] +
                       a[3 * 4 + row] * b[c * 4 + 3];
    }
  }
  std::memcpy(out, r, sizeof(r));
}

// Right-handed Reverse-Z perspective projection (near -> 1.0, far -> 0.0, +Y up, -Z forward).
void mat4_perspective_reverse_z(float fov_y_rad, float aspect, float z_near, float z_far,
                                float out_proj[16], float out_inv_proj[16]) {
  const float f = 1.0f / std::tan(fov_y_rad * 0.5f);
  std::memset(out_proj, 0, sizeof(float) * 16);
  out_proj[0] = f / aspect;
  out_proj[5] = f;
  // z_ndc = (z_near / (z_far - z_near)) * (z_far / (-z_view) - 1) -> 1 at near, 0 at far.
  const float a = z_near / (z_far - z_near);
  const float b = (z_far * z_near) / (z_far - z_near);
  out_proj[10] = a;
  out_proj[11] = -1.0f;
  out_proj[14] = b;

  std::memset(out_inv_proj, 0, sizeof(float) * 16);
  out_inv_proj[0] = aspect / f;
  out_inv_proj[5] = 1.0f / f;
  out_inv_proj[11] = 1.0f / b;
  out_inv_proj[14] = -1.0f;
  out_inv_proj[15] = a / b;
}

void append_quad(std::vector<shift::rhi::VisVertexGpu>& verts,
                 std::vector<uint32_t>& idxs,
                 float cx, float cy, float cz,
                 float hx, float hy, float hz,
                 float nx, float ny, float nz) {
  using namespace shift::rhi;
  const uint32_t base = static_cast<uint32_t>(verts.size());
  // Build two orthogonal axes in the quad plane.
  VisVertexGpu v[4]{};
  if (std::abs(ny) > 0.5f) {
    // Horizontal quad (XZ plane, normal +Y)
    v[0] = {{cx - hx, cy, cz - hz}, 0.0f, {nx, ny, nz}, 0.0f, {1, 1, 1, 1}};
    v[1] = {{cx - hx, cy, cz + hz}, 0.0f, {nx, ny, nz}, 1.0f, {1, 1, 1, 1}};
    v[2] = {{cx + hx, cy, cz + hz}, 1.0f, {nx, ny, nz}, 1.0f, {1, 1, 1, 1}};
    v[3] = {{cx + hx, cy, cz - hz}, 1.0f, {nx, ny, nz}, 0.0f, {1, 1, 1, 1}};
  } else {
    // Vertical quad facing +Z (toward camera at z > cz)
    v[0] = {{cx - hx, cy - hy, cz}, 0.0f, {nx, ny, nz}, 0.0f, {1, 1, 1, 1}};
    v[1] = {{cx + hx, cy - hy, cz}, 1.0f, {nx, ny, nz}, 0.0f, {1, 1, 1, 1}};
    v[2] = {{cx + hx, cy + hy, cz}, 1.0f, {nx, ny, nz}, 1.0f, {1, 1, 1, 1}};
    v[3] = {{cx - hx, cy + hy, cz}, 0.0f, {nx, ny, nz}, 1.0f, {1, 1, 1, 1}};
  }
  for (const auto& vx : v) verts.push_back(vx);
  idxs.push_back(base + 0);
  idxs.push_back(base + 1);
  idxs.push_back(base + 2);
  idxs.push_back(base + 0);
  idxs.push_back(base + 2);
  idxs.push_back(base + 3);
}

void test_meshlet_builder() {
  using namespace shift::rhi;
  // Build a 16x16 grid (289 vertices, 512 triangles) -> must subdivide into multiple meshlets.
  constexpr uint32_t kN = 16;
  std::vector<VisVertexGpu> verts;
  std::vector<uint32_t> idxs;
  verts.reserve((kN + 1) * (kN + 1));
  for (uint32_t y = 0; y <= kN; ++y) {
    for (uint32_t x = 0; x <= kN; ++x) {
      VisVertexGpu v{};
      v.pos[0] = static_cast<float>(x);
      v.pos[1] = 0.25f * std::sin(static_cast<float>(x) * 0.5f);
      v.pos[2] = static_cast<float>(y);
      v.u = static_cast<float>(x) / kN;
      v.v = static_cast<float>(y) / kN;
      verts.push_back(v);
    }
  }
  for (uint32_t y = 0; y < kN; ++y) {
    for (uint32_t x = 0; x < kN; ++x) {
      const uint32_t i0 = y * (kN + 1) + x;
      const uint32_t i1 = i0 + 1;
      const uint32_t i2 = i0 + (kN + 1);
      const uint32_t i3 = i2 + 1;
      idxs.insert(idxs.end(), {i0, i2, i1, i1, i2, i3});
    }
  }

  MeshletBuildOutput out = build_meshlets(verts, idxs, 3u);
  CHECK(out.meshlets.size() >= 5u);
  CHECK(out.reordered_indices.size() == idxs.size());
  uint32_t total_tris = 0;
  for (const MeshletGpu& m : out.meshlets) {
    CHECK(m.vertex_count <= kMaxMeshletVertices);
    CHECK(m.triangle_count <= kMaxMeshletTriangles);
    CHECK(m.bound_radius > 0.0f);
    CHECK(m.material_id == 3u);
    total_tris += m.triangle_count;
  }
  CHECK(total_tris == 512u);
  std::printf("[rhi_vis_buffer_tests] Meshlet builder (512 tris -> %zu meshlets): OK\n",
              out.meshlets.size());
}

struct BackendCapture {
  bool valid = false;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<float> hdr_rgba;
};

BackendCapture run_vis_buffer_on_backend(shift::rhi::BackendKind backend) {
  using namespace shift::rhi;
  BackendCapture cap{};

  DeviceDesc dd{};
  dd.backend = backend;
  dd.enable_validation = true;
  dd.enable_async_compute = true;
  dd.enable_transfer_queue = true;
  dd.enable_ray_tracing = false;
  std::string err;
  auto dev = create_device(dd, err);
  if (!dev) {
    std::printf("[%s] skipped (%s)\n", backend_kind_name(backend), err.c_str());
    return cap;
  }

  constexpr uint32_t kW = 128;
  constexpr uint32_t kH = 128;

  VisBufferPipeline vis(*dev);
  if (!vis.init(kW, kH, err)) {
    std::printf("[%s] VisBufferPipeline::init failed: %s\n", backend_kind_name(backend), err.c_str());
    CHECK(false);
    return cap;
  }

  // Construct 4 distinct meshes (Road, CarPaint, Terrain, BehindCameraCulled).
  std::vector<VisVertexGpu> all_verts;
  std::vector<uint32_t> all_indices;
  std::vector<MeshletGpu> all_meshlets;
  std::vector<MeshletInstanceGpu> instances;

  auto add_mesh_instance = [&](const std::vector<VisVertexGpu>& v,
                               const std::vector<uint32_t>& idx,
                               VisMaterialClass mclass,
                               uint32_t mat_id,
                               float tx, float ty, float tz,
                               float prev_tx = 0.0f) {
    const uint32_t v_off = static_cast<uint32_t>(all_verts.size());
    const uint32_t i_off = static_cast<uint32_t>(all_indices.size());
    const uint32_t first_m = static_cast<uint32_t>(all_meshlets.size());
    all_verts.insert(all_verts.end(), v.begin(), v.end());
    MeshletBuildOutput mb = build_meshlets(v, idx, mat_id, v_off, i_off);
    all_indices.insert(all_indices.end(), mb.reordered_indices.begin(), mb.reordered_indices.end());
    all_meshlets.insert(all_meshlets.end(), mb.meshlets.begin(), mb.meshlets.end());

    MeshletInstanceGpu inst{};
    mat4_translate(inst.model, tx, ty, tz);
    mat4_translate(inst.prev_model, tx + prev_tx, ty, tz);
    inst.first_meshlet = first_m;
    inst.meshlet_count = static_cast<uint32_t>(mb.meshlets.size());
    inst.material_class = static_cast<uint32_t>(mclass);
    inst.material_id = mat_id;
    instances.push_back(inst);
  };

  // 0. Road surface (bottom half of screen)
  {
    std::vector<VisVertexGpu> v;
    std::vector<uint32_t> idx;
    append_quad(v, idx, 0.0f, -1.2f, -6.0f, 5.0f, 0.0f, 5.0f, 0.0f, 1.0f, 0.0f);
    add_mesh_instance(v, idx, VisMaterialClass::Road, 0u, 0.0f, 0.0f, 0.0f);
  }
  // 1. Moving CarPaint panel right in the center (z = -4.5, prev_x shifted by -0.4 m)
  {
    std::vector<VisVertexGpu> v;
    std::vector<uint32_t> idx;
    append_quad(v, idx, 0.0f, 0.2f, 0.0f, 1.0f, 0.9f, 0.0f, 0.0f, 0.0f, 1.0f);
    add_mesh_instance(v, idx, VisMaterialClass::CarPaint, 1u, 0.0f, 0.0f, -4.5f, -0.4f);
  }
  // 2. Terrain hillside panel on the left (x = -2.2, z = -5.5)
  {
    std::vector<VisVertexGpu> v;
    std::vector<uint32_t> idx;
    append_quad(v, idx, 0.0f, 0.4f, 0.0f, 1.1f, 1.1f, 0.0f, 0.2f, 0.3f, 0.93f);
    add_mesh_instance(v, idx, VisMaterialClass::Terrain, 2u, -2.2f, 0.0f, -5.5f);
  }
  // 3. Behind-camera meshlet (z = +20.0 -> MUST be frustum culled by vis_cull.comp!)
  {
    std::vector<VisVertexGpu> v;
    std::vector<uint32_t> idx;
    append_quad(v, idx, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    add_mesh_instance(v, idx, VisMaterialClass::MetalPlastic, 3u, 0.0f, 0.0f, +20.0f);
  }

  std::vector<VisMaterialGpu> materials(4);
  // 0: Road asphalt
  materials[0].base_color[0] = 0.22f; materials[0].base_color[1] = 0.23f; materials[0].base_color[2] = 0.25f;
  materials[0].roughness = 0.45f; materials[0].material_class = static_cast<uint32_t>(VisMaterialClass::Road);
  // 1: Metallic Crimson CarPaint with clearcoat
  materials[1].base_color[0] = 0.85f; materials[1].base_color[1] = 0.08f; materials[1].base_color[2] = 0.12f;
  materials[1].roughness = 0.22f; materials[1].metallic = 0.75f; materials[1].clearcoat = 1.0f;
  materials[1].material_class = static_cast<uint32_t>(VisMaterialClass::CarPaint);
  // 2: Colorado Red-Rock Terrain
  materials[2].base_color[0] = 0.65f; materials[2].base_color[1] = 0.38f; materials[2].base_color[2] = 0.22f;
  materials[2].roughness = 0.85f; materials[2].material_class = static_cast<uint32_t>(VisMaterialClass::Terrain);
  // 3: Metal
  materials[3].base_color[0] = 0.7f; materials[3].base_color[1] = 0.7f; materials[3].base_color[2] = 0.75f;
  materials[3].roughness = 0.3f; materials[3].metallic = 0.9f;
  materials[3].material_class = static_cast<uint32_t>(VisMaterialClass::MetalPlastic);

  VisSceneUpload scene{all_verts, all_indices, all_meshlets, instances, materials};
  CHECK(vis.upload_scene(scene, err));

  // Build camera view (at origin looking along -Z) + Reverse-Z projection.
  VisFrameView view{};
  view.width = kW;
  view.height = kH;
  mat4_identity(view.view);
  mat4_perspective_reverse_z(1.0472f, 1.0f, 0.1f, 200.0f, view.proj, view.inv_proj);
  mat4_mul(view.proj, view.view, view.view_proj);
  std::memcpy(view.prev_view_proj, view.view_proj, sizeof(view.view_proj));
  view.z_near = 0.1f;
  view.z_far = 200.0f;
  view.sun_dir[0] = 0.25f; view.sun_dir[1] = 0.7f; view.sun_dir[2] = 0.67f;
  view.sun_intensity = 2.5f;

  // Dynamic clustered Forward+ lights (1 bright point lamp + 1 headlight spot).
  ClusterLightGpu dyn_lights[2]{};
  dyn_lights[0].pos_radius[0] = 0.3f; dyn_lights[0].pos_radius[1] = 0.8f; dyn_lights[0].pos_radius[2] = -3.2f;
  dyn_lights[0].pos_radius[3] = 8.0f;
  dyn_lights[0].color_intensity[0] = 1.0f; dyn_lights[0].color_intensity[1] = 0.75f; dyn_lights[0].color_intensity[2] = 0.4f;
  dyn_lights[0].color_intensity[3] = 14.0f;
  dyn_lights[0].type = static_cast<uint32_t>(ClusterLightType::Point);

  dyn_lights[1].pos_radius[0] = 0.0f; dyn_lights[1].pos_radius[1] = 0.0f; dyn_lights[1].pos_radius[2] = -1.0f;
  dyn_lights[1].pos_radius[3] = 15.0f;
  dyn_lights[1].color_intensity[0] = 0.6f; dyn_lights[1].color_intensity[1] = 0.9f; dyn_lights[1].color_intensity[2] = 1.0f;
  dyn_lights[1].color_intensity[3] = 18.0f;
  dyn_lights[1].dir_cos_outer[0] = 0.0f; dyn_lights[1].dir_cos_outer[1] = -0.25f; dyn_lights[1].dir_cos_outer[2] = -0.968f;
  dyn_lights[1].dir_cos_outer[3] = 0.75f;
  dyn_lights[1].cos_inner = 0.90f;
  dyn_lights[1].type = static_cast<uint32_t>(ClusterLightType::Spot);

  RenderGraph rg(*dev);
  VisFrameOutputs outs = vis.add_frame_passes(rg, view, dyn_lights, err);
  CHECK(outs.hdr_color.valid());
  CHECK(rg.compile(err));
  CHECK(rg.stats().cross_queue_syncs >= 2);
  if (!rg.execute(err, true)) {
    std::printf("[%s] rg.execute failed: %s\n", backend_kind_name(backend), err.c_str());
    CHECK(false);
  }

  VisTelemetry telem{};
  CHECK(vis.readback_telemetry(telem, err));
  std::printf("[%s] VisBuf telemetry: tested=%u visible=%u frustum_culled=%u cone_culled=%u tiles=[T:%u R:%u F:%u C:%u M:%u]\n",
              backend_kind_name(backend), telem.tested_meshlets, telem.visible_meshlets,
              telem.frustum_culled, telem.cone_culled,
              telem.active_tiles_per_class[0], telem.active_tiles_per_class[1],
              telem.active_tiles_per_class[2], telem.active_tiles_per_class[3],
              telem.active_tiles_per_class[4]);
  CHECK(telem.tested_meshlets == 4u);
  CHECK(telem.visible_meshlets == 3u);
  CHECK(telem.frustum_culled == 1u);  // Behind-camera object culled on GPU!
  CHECK(telem.active_tiles_per_class[static_cast<uint32_t>(VisMaterialClass::Terrain)] > 0u);
  CHECK(telem.active_tiles_per_class[static_cast<uint32_t>(VisMaterialClass::Road)] > 0u);
  CHECK(telem.active_tiles_per_class[static_cast<uint32_t>(VisMaterialClass::CarPaint)] > 0u);
  CHECK(telem.active_tiles_per_class[static_cast<uint32_t>(VisMaterialClass::Foliage)] == 0u);
  CHECK(telem.active_tiles_per_class[static_cast<uint32_t>(VisMaterialClass::MetalPlastic)] == 0u);

  // Read back HDR color (Rgba16Sfloat, 8 B/px) and Velocity (Rg16Sfloat, 4 B/px).
  const uint64_t hdr_bytes = static_cast<uint64_t>(kW) * kH * 8ull;
  const uint64_t vel_bytes = static_cast<uint64_t>(kW) * kH * 4ull;
  BufferHandle hdr_rb = dev->create_buffer({hdr_bytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "hdr_rb"}, err);
  BufferHandle vel_rb = dev->create_buffer({vel_bytes, BufferUsage::TransferDst, MemoryHint::GpuToCpu, false, "vel_rb"}, err);
  CHECK(hdr_rb && vel_rb);

  bool rb_ok = dev->submit_once([&](CommandList& cmd) {
    BufferTextureCopy hdr_c{};
    hdr_c.width = kW;
    hdr_c.height = kH;
    cmd.copy_texture_to_buffer(vis.hdr_texture(), hdr_rb, std::span(&hdr_c, 1));
    cmd.copy_texture_to_buffer(vis.velocity_texture(), vel_rb, std::span(&hdr_c, 1));
    cmd.barrier({{}, {}, false, false, true});
  }, err);
  CHECK(rb_ok);

  cap.width = kW;
  cap.height = kH;
  cap.hdr_rgba.resize(static_cast<size_t>(kW) * kH * 4);
  if (auto* hp = static_cast<const uint16_t*>(dev->map_buffer(hdr_rb))) {
    for (size_t i = 0; i < static_cast<size_t>(kW) * kH * 4; ++i) {
      cap.hdr_rgba[i] = f16_to_f32(hp[i]);
    }
    dev->unmap_buffer(hdr_rb);
    cap.valid = true;
  }

  // Center pixel (64, 60) hits moving CarPaint panel -> must be bright crimson AND have non-zero motion vector!
  const size_t center_px = static_cast<size_t>(60) * kW + 64;
  const float cr = cap.hdr_rgba[center_px * 4 + 0];
  const float cg = cap.hdr_rgba[center_px * 4 + 1];
  const float cb = cap.hdr_rgba[center_px * 4 + 2];
  std::printf("[%s] center CarPaint HDR=(%.3f, %.3f, %.3f)\n", backend_kind_name(backend), cr, cg, cb);
  CHECK(cr > 0.35f && cr > cg * 1.5f);  // Red CarPaint lit by sun + clustered point light

  if (auto* vp = static_cast<const uint16_t*>(dev->map_buffer(vel_rb))) {
    const float vx = f16_to_f32(vp[center_px * 2 + 0]);
    const float vy = f16_to_f32(vp[center_px * 2 + 1]);
    // Static Road pixel near bottom center (64, 112) must have near-zero velocity.
    const size_t road_px = static_cast<size_t>(112) * kW + 64;
    const float rvx = f16_to_f32(vp[road_px * 2 + 0]);
    const float rvy = f16_to_f32(vp[road_px * 2 + 1]);
    std::printf("[%s] velocity: car=(%.4f, %.4f) road=(%.4f, %.4f)\n",
                backend_kind_name(backend), vx, vy, rvx, rvy);
    CHECK(std::abs(vx) > 0.01f);
    CHECK(std::abs(rvx) < 0.002f && std::abs(rvy) < 0.002f);
    dev->unmap_buffer(vel_rb);
  }

  // Also test stall-free Dynamic Resolution Scaling (96x96 within 128x128 fixed textures).
  rg.reset();
  VisFrameView drs_view = view;
  drs_view.width = 96;
  drs_view.height = 96;
  vis.add_frame_passes(rg, drs_view, dyn_lights, err);
  CHECK(rg.compile(err));
  CHECK(rg.render_extent().width == 96 && rg.render_extent().max_width == 128);
  CHECK(rg.execute(err, true));

  dev->destroy_buffer(vel_rb);
  dev->destroy_buffer(hdr_rb);
  rg.release_pool();
  vis.shutdown();
  dev->wait_idle();
  CHECK(dev->validation_message_count() == 0);
  std::printf("[%s] VisBuffer + Clustered Forward+ + DRS complete (validation_msgs=%u)\n",
              backend_kind_name(backend), dev->validation_message_count());
  return cap;
}

}  // namespace

int main() {
  using namespace shift::rhi;
  test_meshlet_builder();

  BackendCapture vk_cap{};
  BackendCapture dx_cap{};
  if (is_backend_compiled(BackendKind::Vulkan)) {
    vk_cap = run_vis_buffer_on_backend(BackendKind::Vulkan);
  }
  if (is_backend_compiled(BackendKind::D3D12)) {
    dx_cap = run_vis_buffer_on_backend(BackendKind::D3D12);
  }
  if (is_backend_compiled(BackendKind::Metal)) {
    run_vis_buffer_on_backend(BackendKind::Metal);
  }

  // Cross-API Vulkan 1.3 vs DirectX 12 pixel parity verification!
  if (vk_cap.valid && dx_cap.valid && vk_cap.hdr_rgba.size() == dx_cap.hdr_rgba.size()) {
    double sum_abs = 0.0;
    double sum_sq = 0.0;
    float max_abs = 0.0f;
    const size_t n = vk_cap.hdr_rgba.size();
    for (size_t i = 0; i < n; ++i) {
      const float d = std::abs(vk_cap.hdr_rgba[i] - dx_cap.hdr_rgba[i]);
      sum_abs += d;
      sum_sq += static_cast<double>(d) * d;
      max_abs = std::max(max_abs, d);
    }
    const double mean_abs = sum_abs / static_cast<double>(n);
    const double mse = sum_sq / static_cast<double>(n);
    const double psnr = mse > 1e-12 ? 10.0 * std::log10(1.0 / mse) : 999.0;
    std::printf("[parity] Vulkan vs DX12 VisBuffer+ClusteredLights HDR: PSNR=%.2f dB, mean_abs=%.6f, max_abs=%.4f\n",
                psnr, mean_abs, max_abs);
    CHECK(psnr >= 38.0);
    CHECK(mean_abs < 0.005);
  }

  if (g_failures > 0) {
    std::printf("rhi_vis_buffer_tests: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("rhi_vis_buffer_tests: ALL PASSED\n");
  return 0;
}
