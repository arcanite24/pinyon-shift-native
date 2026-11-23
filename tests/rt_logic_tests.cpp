#include "render_modern.h"
#include "rt_logic.h"
#include "settings_page.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_failed = 0;

void check(bool cond, const char* msg) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_failed;
  }
}

using namespace carview::rt;

RtDeviceCaps make_rtx4080_caps() {
  RtDeviceCaps c{};
  c.compiled_in = true;
  c.buffer_device_address = true;
  c.acceleration_structure = true;
  c.ray_query = true;
  c.ray_tracing_pipeline = true;
  c.deferred_host_operations = true;
  c.rt_maintenance1 = true;
  c.opacity_micromap = true;
  c.invocation_reorder_nv = true;
  c.scalar_block_layout = true;
  c.shader_int64 = true;
  c.async_compute_queue = true;
  c.max_ray_recursion_depth = 31;
  c.shader_group_handle_size = 32;
  c.shader_group_base_alignment = 64;
  c.min_as_scratch_alignment = 128;
  c.device_local_bytes = 16ull << 30;
  return c;
}

void test_capability_negotiation() {
  const auto rtx = make_rtx4080_caps();
  check(rtx.supports_hybrid(), "RTX 4080 supports Hybrid RT");
  check(rtx.supports_pathtracing(), "RTX 4080 supports real-time Path Tracing");
  check(rtx.supports_photo_pt(), "RTX 4080 supports Photo Mode PT");

  // Off -> zero overhead, not unavailable.
  const auto d_off = resolve_rt_mode(RtMode::kOff, rtx);
  check(d_off.active_mode == RtMode::kOff && !d_off.unavailable, "RtMode::kOff resolves cleanly");

  // Hybrid RT on RTX 4080.
  const auto d_hyb = resolve_rt_mode(RtMode::kHybrid, rtx);
  check(d_hyb.active_mode == RtMode::kHybrid && !d_hyb.unavailable, "RTX Hybrid RT active");
  check(!d_hyb.half_res_reflections && !d_hyb.half_res_shadows, "RTX Hybrid RT full-res");
  check(d_hyb.use_omm && d_hyb.use_ser, "RTX detects OMM + SER");

  // Real-Time Path Tracing on RTX 4080.
  const auto d_pt = resolve_rt_mode(RtMode::kPathTraced, rtx);
  check(d_pt.active_mode == RtMode::kPathTraced && !d_pt.unavailable, "RTX Path Tracing active");

  // Photo Mode PT on RTX 4080.
  const auto d_photo = resolve_rt_mode(RtMode::kPhotoModePt, rtx);
  check(d_photo.active_mode == RtMode::kPhotoModePt && !d_photo.unavailable, "RTX Photo Mode PT active");

  // Non-RT GPU -> falls back to Off with unavailable flag.
  RtDeviceCaps non_rt{};
  non_rt.compiled_in = true;
  const auto d_no = resolve_rt_mode(RtMode::kHybrid, non_rt);
  check(d_no.active_mode == RtMode::kOff && d_no.unavailable, "Non-RT GPU falls back to Off");

  // Compiled with SHIFT_ENABLE_RAY_TRACING=OFF -> falls back to Off.
  RtDeviceCaps uncompiled = rtx;
  uncompiled.compiled_in = false;
  const auto d_unc = resolve_rt_mode(RtMode::kPathTraced, uncompiled);
  check(d_unc.active_mode == RtMode::kOff && d_unc.unavailable, "Uncompiled RT falls back to Off");

  // Apple M4 MoltenVK: supports Hybrid RT (half-res), clamps real-time PT -> Hybrid, allows Photo Mode PT!
  RtDeviceCaps m4 = rtx;
  m4.is_moltenvk = true;
  m4.ray_tracing_pipeline = false;
  m4.opacity_micromap = false;
  m4.invocation_reorder_nv = false;
  const auto d_m4_pt = resolve_rt_mode(RtMode::kPathTraced, m4);
  check(d_m4_pt.active_mode == RtMode::kHybrid && d_m4_pt.half_res_reflections,
        "MoltenVK clamps real-time PT to half-res Hybrid RT");
  const auto d_m4_photo = resolve_rt_mode(RtMode::kPhotoModePt, m4);
  check(d_m4_photo.active_mode == RtMode::kPhotoModePt && !d_m4_photo.half_res_reflections,
        "MoltenVK allows full-res convergent Photo Mode PT");

  // Android Adreno / Mesa Turnip: clamps real-time PT -> half-res Hybrid RT, disables OMM/SER.
  RtDeviceCaps turnip = rtx;
  turnip.is_turnip = true;
  turnip.is_mobile_tbdr = true;
  const auto d_tu = resolve_rt_mode(RtMode::kPathTraced, turnip);
  check(d_tu.active_mode == RtMode::kHybrid && d_tu.half_res_reflections && d_tu.half_res_shadows,
        "Turnip clamps real-time PT to half-res Hybrid RT");
  check(!d_tu.use_omm && !d_tu.use_ser, "Turnip disables OMM/SER quirks");
}

void test_instance_mask_packing() {
  check((kMaskShadowCasters & kMaskGlass) == 0, "Shadow rays exclude transparent glass");
  check((kMaskShadowCasters & kMaskPlayerCar) != 0, "Shadow rays include player car");
  check((kMaskReflectionHits & kMaskPlayerCar) != 0, "Reflection rays capture player car self-reflections");
  check((kMaskReflectionHits & kMaskTrafficCar) != 0, "Reflection rays capture oncoming traffic");
  check((kMaskReflectionHits & kMaskWorldOpaque) != 0, "Reflection rays capture Colorado terrain/buildings");

  const uint32_t c_car = pack_instance_custom_index(42u, InstanceKind::kPlayerCar);
  check(unpack_instance_info_index(c_car) == 42u, "Unpack player car info index");
  check(unpack_instance_kind(c_car) == InstanceKind::kPlayerCar, "Unpack player car kind");

  const uint32_t c_world = pack_instance_custom_index(1048575u, InstanceKind::kWorld);
  check(unpack_instance_info_index(c_world) == 1048575u, "Unpack max 20-bit info index");
  check(unpack_instance_kind(c_world) == InstanceKind::kWorld, "Unpack world kind");

  const uint32_t packed = pack_instance_custom_and_mask(c_car, kMaskPlayerCar | kMaskEmissive);
  check(unpack_instance_mask(packed) == uint8_t(kMaskPlayerCar | kMaskEmissive), "Unpack 8-bit visibility mask");
  check((packed & kMaxInstanceCustomIndex) == c_car, "Packed lower 24 bits preserve custom index");

  // World cell key determinism & sensitivity.
  const uint64_t k1 = make_world_cell_key(3, -5, 0, 0x12345678ull);
  const uint64_t k2 = make_world_cell_key(3, -5, 0, 0x12345678ull);
  const uint64_t k3 = make_world_cell_key(3, -5, 1, 0x12345678ull);
  check(k1 == k2 && k1 != k3 && k1 != 0, "World cell BLAS keys are deterministic and class-distinct");
}

void test_blas_cache_lru_and_scratch() {
  BlasCacheLru lru(1000);
  lru.insert(10, 400, 1);
  lru.insert(20, 400, 2);
  lru.insert(30, 400, 3);
  check(lru.used_bytes() == 1200 && lru.entry_count() == 3, "LRU tracks inserted BLAS bytes");

  // Compaction reduces footprint cleanly.
  check(lru.mark_compacted(20, 200), "Compaction updates entry bytes");
  check(lru.used_bytes() == 1000 && lru.find(20)->compacted, "Compacted BLAS updates used_bytes and flag");

  // Releasing entries tags them with frames_submitted; eviction waits until frames_done >= free_tag.
  lru.release(10, 5);
  lru.release(30, 6);
  lru.insert(40, 500, 4);  // used = 1500 > 1000
  auto ev_early = lru.evict(4);
  check(ev_early.empty(), "LRU never evicts BLAS still in flight on GPU");

  auto ev_ready = lru.evict(6);
  check(ev_ready.size() == 2 && ev_ready[0] == 10 && ev_ready[1] == 30,
        "LRU evicts oldest unreferenced completed BLASes first");
  check(lru.used_bytes() == 700 && lru.entry_count() == 2 && lru.eviction_count() == 2,
        "LRU accounting matches post-eviction state");

  // ScratchRingAllocator alignment & overflow.
  ScratchRingAllocator ring(1024, 256);
  check(ring.allocate(100) == 0, "First scratch allocation at offset 0");
  check(ring.allocate(200) == 256, "Second scratch allocation 256-byte aligned");
  check(ring.allocate(500) == 512, "Third scratch allocation fits at 512");
  check(ring.allocate(64) == UINT64_MAX, "Overflow returns UINT64_MAX");
  ring.begin_frame();
  check(ring.allocate(1024) == 0, "begin_frame resets scratch ring");
}

void test_settings_and_cli_roundtrip() {
  using namespace carview::modern;

  check(parse_rt_mode("off") == RtMode::kOff, "parse off");
  check(parse_rt_mode("hybrid") == RtMode::kHybrid, "parse hybrid");
  check(parse_rt_mode("pt") == RtMode::kPathTraced, "parse pt");
  check(parse_rt_mode("photo-pt") == RtMode::kPhotoModePt, "parse photo-pt");
  check(clamp_rt_bounces(0) == 1 && clamp_rt_bounces(99) == 4, "clamp bounces 1..4");
  check(clamp_rt_spp(0) == 1 && clamp_rt_spp(99) == 16, "clamp spp 1..16");
  check(clamp_photo_pt_spp(4) == 16 && clamp_photo_pt_spp(999) == 256, "clamp photo spp 16..256");

  GraphicsSettings g{};
  g.rt_mode = RtMode::kPathTraced;
  g.rt_bounces = 3;
  g.rt_spp = 4;
  g.rt_denoise = false;

  const auto toks = graphics_tokens(g);
  GraphicsSettings parsed{};
  for (size_t i = 0; i < toks.size(); ++i) {
    check(apply_graphics_option(parsed, toks, i), "apply_graphics_option accepts emitted RT token");
  }
  check(parsed == g, "GraphicsSettings RT fields round-trip losslessly");

  // ModernTargetPlan allocates motion_vec when RT is active, zero extra when RT is off.
  const UpscalerChoice up{};
  const auto plan_off = plan_modern_targets(GraphicsSettings{}, {1920, 1080}, {1920, 1080}, true, HdrMode::kOff, 0,
                                            false, false, up, false);
  check(!plan_off.rt_active && !plan_off.motion_vec, "RT Off adds zero RT/motion target overhead");

  const auto plan_rt =
      plan_modern_targets(g, {1920, 1080}, {1920, 1080}, true, HdrMode::kOff, 0, false, false, up, false);
  check(plan_rt.rt_active && plan_rt.motion_vec, "RT Active plans motion vectors for SVGF temporal reuse");

  // SettingsPage UI round-trip.
  carview::SettingsPage page;
  page.load({"--rt", "hybrid", "--rt-bounces", "3", "--rt-spp", "2", "--no-rt-denoise"});
  check(page.value("ray tracing") == "hybrid rt", "SettingsPage loads ray tracing hybrid");
  check(page.value("rt bounces") == "3 bounces", "SettingsPage loads rt bounces 3");
  check(page.value("rt samples") == "2 spp", "SettingsPage loads rt samples 2");
  check(page.value("rt denoiser") == "off", "SettingsPage loads rt denoiser off");
  const auto live = page.live_settings();
  check(live.graphics.rt_mode == RtMode::kHybrid && live.graphics.rt_bounces == 3 && live.graphics.rt_spp == 2 &&
            !live.graphics.rt_denoise,
        "SettingsPage live_settings exposes exact RT parameters");
}

}  // namespace

int main() {
  test_capability_negotiation();
  test_instance_mask_packing();
  test_blas_cache_lru_and_scratch();
  test_settings_and_cli_roundtrip();
  if (g_failed == 0) {
    std::printf("rt_logic_tests: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "rt_logic_tests: %d check(s) failed\n", g_failed);
  return 1;
}
