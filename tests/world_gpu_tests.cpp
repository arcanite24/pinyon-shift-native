// Slice 11: the GPU-driven world's bookkeeping (no Vulkan): the arena
// allocator, the resident set found by content key across regathers, the
// draw table packing, and the culling rule and indirect command packing the
// compute shader (shaders/world_cull.comp) implements.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "world_gpu_logic.h"

using namespace carview::gpuworld;

namespace {

int failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

void test_arena() {
  ArenaAllocator a(100);
  CHECK(a.allocate(30) == 0 && a.allocate(30) == 30 && a.allocate(30) == 60);
  CHECK(a.used() == 90 && a.largest_free() == 10);
  CHECK(a.allocate(11) == kNoSpace);
  CHECK(a.allocate(10) == 90);
  CHECK(a.allocate(1) == kNoSpace);
  // Freeing the middle then its neighbours merges the ranges.
  a.free(30, 30);
  CHECK(a.free_ranges() == 1 && a.largest_free() == 30);
  a.free(0, 30);
  CHECK(a.free_ranges() == 1 && a.largest_free() == 60);
  a.free(60, 30);
  CHECK(a.free_ranges() == 1 && a.largest_free() == 90);
  a.free(90, 10);
  CHECK(a.free_ranges() == 1 && a.largest_free() == 100 && a.used() == 0);
  CHECK(a.allocate(100) == 0);
  // First fit reuses the earliest hole.
  ArenaAllocator b(100);
  b.allocate(10), b.allocate(10), b.allocate(10);
  b.free(10, 10);
  CHECK(b.allocate(5) == 10);
  CHECK(b.allocate(5) == 15);
  CHECK(b.allocate(0) == 0);
  // A hole too small is skipped.
  ArenaAllocator c(50);
  c.allocate(10), c.allocate(10), c.allocate(10), c.allocate(10), c.allocate(10);
  c.free(10, 10);
  c.free(30, 10);
  CHECK(c.allocate(15) == kNoSpace);  // two holes of 10: nothing of 15
  c.free(20, 10);                     // they merge into 30
  CHECK(c.allocate(15) == 10);
}

void test_resident_set() {
  ResidentSet set(850, 2550);
  // A first gather places three draws.
  ResidentDraw* a = set.place(1, 100, 300, 5.0, 7.0, 1);
  ResidentDraw* b = set.place(2, 200, 600, 5.0, 7.0, 1);
  ResidentDraw* c = set.place(3, 300, 900, 5.0, 7.0, 1);
  CHECK(a && b && c);
  CHECK(a->vertex_base == 0 && b->vertex_base == 100 && c->vertex_base == 300);
  CHECK(a->origin_x == 5.0 && a->refs == 1 && !a->valid);
  CHECK(set.count() == 3 && set.referenced() == 3);
  CHECK(set.vertex_used() == 600 && set.index_used() == 1800);
  // The next gather finds draws 1 and 3 again and adds 4; 2 is no longer wanted.
  set.mark_valid(1), set.mark_valid(2), set.mark_valid(3);
  CHECK(set.acquire(1, 2) && set.acquire(3, 2));
  CHECK(!set.acquire(4, 2));
  ResidentDraw* d = set.place(4, 150, 450, 9.0, 9.0, 2);
  CHECK(d && d->vertex_base == 600);
  set.mark_valid(4);
  CHECK(set.find(1)->refs == 2);
  // The old scenery lets go (frame 5 may still draw them).
  set.release(1, 5);
  set.release(2, 5);
  set.release(3, 5);
  CHECK(set.find(2)->refs == 0 && set.find(2)->free_tag == 5);
  CHECK(set.referenced() == 3);  // 1, 3, 4
  // Nothing can be evicted for space while the GPU may still draw 2 (done frames: 4) ...
  ResidentDraw* big = set.place(5, 180, 500, 0, 0, 3, 4);
  CHECK(big == nullptr);  // only 100 vertices are free at the end, 2 is not yet reusable
  CHECK(set.contains(2));
  // ... but can once the frames are done (5): the oldest unreferenced one goes (2, last used 1).
  std::vector<uint64_t> evicted;
  big = set.place(5, 180, 500, 0, 0, 3, 5, &evicted);
  CHECK(big != nullptr);
  CHECK(evicted.size() == 1 && evicted[0] == 2 && !set.contains(2));
  CHECK(set.evictions() == 1);
  // Referenced draws are never evicted.
  set.mark_valid(5);
  CHECK(set.place(6, 5000, 10, 0, 0, 4, 99) == nullptr);
  CHECK(set.contains(1) && set.contains(3) && set.contains(4));
  // A placement whose copy never ran (not valid, no references) is dropped when found.
  ResidentDraw* ghost = set.place(7, 10, 30, 0, 0, 5, 99);
  CHECK(ghost != nullptr);
  set.release(7, 0);
  CHECK(set.contains(7));
  CHECK(set.acquire(7, 6) == nullptr && !set.contains(7));
  // Evicting everything unreferenced.
  set.release(1, 0);
  set.release(3, 0);
  set.release(4, 0);
  set.release(5, 0);
  set.evict_unreferenced(99);
  CHECK(set.count() == 0 && set.vertex_used() == 0 && set.index_used() == 0);
  // The same key twice in one world shares one entry with two references.
  ResidentDraw* x = set.place(9, 10, 30, 0, 0, 6);
  ResidentDraw* y = set.place(9, 10, 30, 0, 0, 6);
  CHECK(x == y && x->refs == 2 && set.vertex_used() == 10);
}

void test_keys() {
  // The same bytes hash the same; one byte differs, the key does.
  unsigned char a[37], b[37];
  for (int i = 0; i < 37; ++i) a[i] = b[i] = (unsigned char)(i * 7);
  CHECK(hash_bytes(1, a, 37) == hash_bytes(1, b, 37));
  b[36] ^= 1;
  CHECK(hash_bytes(1, a, 37) != hash_bytes(1, b, 37));
  CHECK(hash_bytes(1, a, 36) != hash_bytes(1, a, 37));
  CHECK(hash_bytes(1, a, 8) != hash_bytes(2, a, 8));
  // A draw seen by two gathers with different origins: absolute positions quantised to a millimetre agree.
  const double abs_x = 1234.5678, origin_a = 1000.0, origin_b = 1200.0;
  const float rel_a = float(abs_x - origin_a), rel_b = float(abs_x - origin_b);
  CHECK(quantize_mm(double(rel_a) + origin_a) == quantize_mm(double(rel_b) + origin_b));
  CHECK(quantize_mm(-0.0004) == 0 && quantize_mm(-0.0006) == -1 && quantize_mm(0.0006) == 1);
}

DrawGpu make_draw(float x0, float x1, uint32_t batch, uint32_t index_count, uint32_t first_index, int32_t vertex_offset,
                  float flags = 0.0f) {
  DrawGpu d{};
  d.box_min[0] = x0, d.box_max[0] = x1;
  d.box_min[1] = 0, d.box_max[1] = 2;
  d.box_min[2] = 10, d.box_max[2] = 20;
  d.params[1] = flags;
  d.draw[0] = index_count, d.draw[1] = first_index, d.draw[3] = batch;
  std::memcpy(&d.draw[2], &vertex_offset, sizeof vertex_offset);
  return d;
}

CullView forward_view() {
  // An identity-ish "clip" looking down +z: |x| <= z... planes w+x, w-x, w+y, w-y with w = z/10 (a 45 degree cone).
  CullView v{};
  const float p[4][4] = {{1, 0, 0.1f, 0}, {-1, 0, 0.1f, 0}, {0, 1, 0.1f, 0}, {0, -1, 0.1f, 0}};
  std::memcpy(v.planes, p, sizeof p);
  v.eye[2] = 0;
  return v;
}

void test_batches_and_packing() {
  CHECK(batch_of(0, false) == kOpaque && batch_of(0, true) == kOpaqueDecal);
  CHECK(batch_of(1, false) == kCutout && batch_of(1, true) == kCutoutDecal);
  CHECK(in_gpu_batches(0) && in_gpu_batches(1) && !in_gpu_batches(2) && !in_gpu_batches(3));
  CHECK(sizeof(DrawGpu) == 128 && sizeof(IndirectCommand) == 20);
  CHECK(offsetof(DrawGpu, draw) == 96 && offsetof(DrawGpu, shift) == 112);
}

void test_culling() {
  const CullView view = forward_view();
  // In front, inside the cone.
  CHECK(draw_visible(make_draw(-1, 1, 0, 3, 0, 0), view));
  // Far to the side (x 100 at z 10..20: outside w + x? the left plane w - x = z/10 - x < 0).
  CHECK(!draw_visible(make_draw(100, 110, 0, 3, 0, 0), view));
  CHECK(!draw_visible(make_draw(-110, -100, 0, 3, 0, 0), view));
  // A box straddling a plane stays.
  CHECK(draw_visible(make_draw(0.5f, 50, 0, 3, 0, 0), view));
  // With the frustum test off everything passes.
  CullView off = view;
  off.frustum = false;
  CHECK(draw_visible(make_draw(100, 110, 0, 3, 0, 0), off));
  // LOD00 prop cells wholly past the reach go; others do not.
  CullView lod = view;
  lod.lod_reach = 5.0f;  // box z 10..20: 10 m away
  CHECK(!draw_visible(make_draw(-1, 1, 0, 3, 0, 0, float(kFlagLodNear)), lod));
  CHECK(draw_visible(make_draw(-1, 1, 0, 3, 0, 0, 2.0f), lod));  // not a LOD00 draw
  lod.lod_reach = 30.0f;
  CHECK(draw_visible(make_draw(-1, 1, 0, 3, 0, 0, float(kFlagLodNear)), lod));
  lod.lod_reach = 0.0f;  // no LOD band: never culled by it
  CHECK(draw_visible(make_draw(-1, 1, 0, 3, 0, 0, float(kFlagLodNear)), lod));
}

void test_indirect() {
  std::vector<DrawGpu> table;
  table.push_back(make_draw(-1, 1, kOpaque, 300, 0, 0));
  table.push_back(make_draw(100, 110, kOpaque, 600, 300, 100));  // culled
  table.push_back(make_draw(-1, 1, kCutout, 90, 900, -5));
  table.push_back(make_draw(-2, 2, kOpaqueDecal, 30, 990, 7));
  table.push_back(make_draw(-1, 1, kOpaque, 60, 1020, 12));
  table.push_back(make_draw(-1, 1, kCutoutDecal, 9, 1080, 0));
  const IndirectBatches b = build_indirect(table, forward_view(), 8);
  CHECK(b.visible() == 5);
  CHECK(b.commands[kOpaque].size() == 2);
  CHECK(b.commands[kOpaque][0].index_count == 300 && b.commands[kOpaque][0].first_instance == 0);
  CHECK(b.commands[kOpaque][1].first_instance == 4 && b.commands[kOpaque][1].vertex_offset == 12);
  CHECK(b.commands[kCutout].size() == 1 && b.commands[kCutout][0].vertex_offset == -5);
  CHECK(b.commands[kOpaqueDecal][0].first_index == 990 && b.commands[kOpaqueDecal][0].first_instance == 3);
  CHECK(b.commands[kCutoutDecal][0].instance_count == 1);
  // A batch that is full drops the rest, as the shader does (its count then exceeds the limit).
  const IndirectBatches small = build_indirect(table, forward_view(), 1);
  CHECK(small.commands[kOpaque].size() == 1 && small.visible() == 4);
  CHECK(build_indirect({}, forward_view(), 8).visible() == 0);
}

void test_hiz() {
  // Simple forward-looking projection (reverse_z and forward_z).
  const float proj_rev[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0};
  const float bmin[3] = {-1, -1, 10}, bmax[3] = {1, 1, 12};
  const HizBox rbox = hiz_project_box(proj_rev, bmin, bmax, 512, 256, true);
  CHECK(rbox.valid);
  // Nearest corner z = 10 -> ndc_z = 1/10 = 0.1.
  CHECK(rbox.z_near > 0.09f && rbox.z_near < 0.11f);
  CHECK(hiz_is_occluded(rbox.z_near, 0.25f, true));   // occluder closer (0.25 > 0.1)
  CHECK(!hiz_is_occluded(rbox.z_near, 0.05f, true));  // occluder further (0.05 < 0.1)
  CHECK(hiz_reduce(0.1f, 0.2f, 0.3f, 0.4f, true) == 0.1f);
  CHECK(hiz_reduce(0.1f, 0.2f, 0.3f, 0.4f, false) == 0.4f);

  // Forward-Z: nearest depth is smaller; occluded when z_near > hiz_max.
  const float proj_fwd[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, -1, 0};
  const HizBox fbox = hiz_project_box(proj_fwd, bmin, bmax, 512, 256, false);
  CHECK(fbox.valid);
  CHECK(hiz_is_occluded(fbox.z_near, 0.5f, false));
  CHECK(!hiz_is_occluded(fbox.z_near, 0.99f, false));

  // Box straddling near plane returns invalid (never culled).
  const float behind[3] = {-1, -1, -2}, front[3] = {1, 1, 5};
  CHECK(!hiz_project_box(proj_rev, behind, front, 512, 256, true).valid);
  CHECK(sizeof(TreeInstanceGpu) == 64);
}

void test_early_z_batch_routing() {
  // Opaque draws (blend 0) must stay cleanly segregated from alpha-tested cutout draws (blend 1)
  // so discard-free early_fragment_tests pipelines never receive alpha-tested geometry.
  CHECK(batch_of(0, false) != batch_of(1, false));
  CHECK(batch_of(0, true) != batch_of(1, true));
  std::vector<DrawGpu> mixed = {
      make_draw(-1, 1, kOpaque, 120, 0, 0),
      make_draw(-1, 1, kCutout, 60, 120, 0),
      make_draw(-1, 1, kOpaqueDecal, 30, 180, 0),
      make_draw(-1, 1, kCutoutDecal, 15, 210, 0),
  };
  const IndirectBatches b = build_indirect(mixed, forward_view(), 16);
  CHECK(b.commands[kOpaque].size() == 1 && b.commands[kCutout].size() == 1);
  CHECK(b.commands[kOpaqueDecal].size() == 1 && b.commands[kCutoutDecal].size() == 1);
}

void test_flag_encoding_and_visual_invariants() {
  // PT-19 / PT-16 regression: kFlagEmissiveMap (1 << 23 = 8388608) combined with
  // kFlagTextured (1) is an odd integer in [2^23, 2^24). IEEE-754 float32 stores all
  // integers up to 2^24 exactly, so std::lround() decodes every bit losslessly, whereas
  // adding +0.5f rounds odd values in [2^23, 2^24) to even, clearing bit 0 and setting kFlagSky (bit 1).
  constexpr uint32_t kFlagTextured = 1u;
  constexpr uint32_t kFlagAoMap = 8192u;
  constexpr uint32_t kFlagSpecMap = 524288u;
  constexpr uint32_t kFlagEmissiveMap = 8388608u;
  const uint32_t building_flags = kFlagTextured | kFlagAoMap | kFlagSpecMap | kFlagEmissiveMap;
  const float encoded = float(building_flags);
  const uint32_t decoded_exact = uint32_t(std::lround(encoded));
  CHECK(decoded_exact == building_flags);
  CHECK((decoded_exact & kFlagTextured) == kFlagTextured);
  CHECK((decoded_exact & kFlagSky) == 0u);
  // Confirm the buggy +0.5f pattern would indeed corrupt bit 0 -> bit 1 (kFlagSky).
  const uint32_t corrupted = uint32_t(encoded + 0.5f);
  CHECK((corrupted & kFlagSky) != 0u && (corrupted & kFlagTextured) == 0u);

  // PT-18: Animated gantry LED flag combination (kFlagTextured | kFlagUnlitVertexColor = 1025)
  // stays well below 2^23 and never collides with kFlagSky.
  constexpr uint32_t kFlagUnlitVertexColor = 1024u;
  const uint32_t gantry_flags = kFlagTextured | kFlagUnlitVertexColor;
  CHECK(uint32_t(std::lround(float(gantry_flags))) == gantry_flags);
  CHECK((gantry_flags & kFlagSky) == 0u);

  // PT-14 / PT-20: Smooth C1 atmosphere curve starts at 0 at horizon (V.y = 0),
  // increases monotonically without a dead shelf at V.y = 0.03..0.06, and reaches 1 at zenith.
  auto atmo_ht = [](float vy, float color_power, float clamp_hi) {
    const float hi = std::max(clamp_hi, 0.15f);
    const float y = std::max(vy, 0.0f);
    const float sc = std::clamp(color_power * 1.45f, 0.11f, 0.45f) * std::clamp(hi / 0.50f, 0.5f, 1.5f);
    return (1.0f - std::exp(-y / sc)) / (1.0f - std::exp(-1.0f / sc));
  };
  const float h0 = atmo_ht(0.0f, 0.0586f, 0.50f);
  const float h3 = atmo_ht(0.03f, 0.0586f, 0.50f);
  const float h6 = atmo_ht(0.06f, 0.0586f, 0.50f);
  const float h1 = atmo_ht(1.0f, 0.0586f, 0.50f);
  CHECK(std::fabs(h0) < 1e-6f);
  CHECK(h3 > 0.15f && h3 < h6 && h6 < 0.65f);  // smooth gradient across 0..0.06 (no flat shelf or cliff)
  CHECK(std::fabs(h1 - 1.0f) < 1e-5f);

  // PT-20: True premultiplied Porter-Duff OVER preserves full linear cloud luminance without a^2 darkening.
  const float cloud_rgb = 0.80f;
  const float cloud_a = 0.35f;
  const float acc_premul = cloud_rgb * cloud_a;
  const float acc_a = cloud_a;
  const float straight_rgb = acc_premul / acc_a;
  const float out_premul = straight_rgb * acc_a;
  CHECK(std::fabs(straight_rgb - cloud_rgb) < 1e-6f);
  CHECK(std::fabs(out_premul - cloud_rgb * cloud_a) < 1e-6f);

  // R3-REN-18: Sky dome unwrapped azimuth derivative (fract(du + 0.5) - 0.5) prevents 1-px mip seams at -X.
  auto unwrap_du = [](float du) {
    const float v = du + 0.5f;
    return (v - std::floor(v)) - 0.5f;
  };
  CHECK(std::fabs(unwrap_du(0.0012f) - 0.0012f) < 1e-5f);
  CHECK(std::fabs(unwrap_du(-0.0012f) - (-0.0012f)) < 1e-5f);
  CHECK(std::fabs(unwrap_du(-0.9988f) - 0.0012f) < 1e-5f);  // 0.9994 -> 0.0006 wrap
  CHECK(std::fabs(unwrap_du(0.9988f) - (-0.0012f)) < 1e-5f); // 0.0006 -> 0.9994 wrap

  // R3-REN-14: Headlight back-face illumination strictly 0.0 behind surface while preserving grazing road glow.
  auto smoothstep_f = [](float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  };
  auto hl_facing = [&](float nl) {
    return smoothstep_f(-0.05f, 0.02f, nl) * (0.35f + 0.65f * std::clamp(nl, 0.0f, 1.0f));
  };
  CHECK(hl_facing(-1.0f) == 0.0f);
  CHECK(hl_facing(-0.10f) == 0.0f);
  CHECK(hl_facing(-0.05f) == 0.0f);
  CHECK(hl_facing(0.02f) > 0.34f && hl_facing(0.02f) < 0.40f);
  CHECK(std::fabs(hl_facing(1.0f) - 1.0f) < 1e-5f);

  // R3-REN-14: Complementary IGN screen-door partition between 3D tree fade-out and billboard fade-in.
  for (int i = 0; i < 16; ++i) {
    const float door = (float(i) + 0.5f) / 16.0f;
    const float band_t = 0.42f;
    const bool keep_3d = !(door < band_t);       // 3D tree discards when door < e
    const bool keep_bb = !(door >= band_t);      // billboard discards when door >= s (s == e)
    CHECK(keep_3d != keep_bb);                   // exact zero-gap, zero-double-overdraw partition
  }

  // R3-REN-20: Hi-Z camera stability guard (cos(8 deg) yaw/pitch cut + teleport cut + screen-edge guard).
  auto hiz_stable = [](float curr_fx, float curr_fz, float prev_fx, float prev_fz, float eye_dx, float eye_dz) {
    constexpr float kMaxTurnCos = 0.99026807f;  // cos(8 deg)
    const float cl = std::hypot(curr_fx, curr_fz);
    const float pl = std::hypot(prev_fx, prev_fz);
    if (cl < 1e-3f || pl < 1e-3f) return false;
    const float d = (curr_fx * prev_fx + curr_fz * prev_fz) / (cl * pl);
    if (d < kMaxTurnCos) return false;
    if (std::fabs(eye_dz) > 12.0f || (eye_dx * eye_dx) > 144.0f) return false;
    return true;
  };
  const float rad4 = 4.0f * 3.14159265f / 180.0f;
  const float rad12 = 12.0f * 3.14159265f / 180.0f;
  CHECK(hiz_stable(0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 2.0f));                           // steady driving
  CHECK(hiz_stable(std::sin(rad4), std::cos(rad4), 0.0f, 1.0f, 0.2f, 1.5f));       // gentle 4 deg turn
  CHECK(!hiz_stable(std::sin(rad12), std::cos(rad12), 0.0f, 1.0f, 0.2f, 1.5f));    // fast 12 deg hairpin turn
  CHECK(!hiz_stable(0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f));                         // Look-Back 180 deg cut
  CHECK(!hiz_stable(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 25.0f));                         // instant camera teleport
  CHECK(!hiz_stable(0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f));                          // uninitialized prev_vp
}

}  // namespace

int main() {
  test_arena();
  test_resident_set();
  test_keys();
  test_batches_and_packing();
  test_culling();
  test_indirect();
  test_hiz();
  test_early_z_batch_routing();
  test_flag_encoding_and_visual_invariants();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("world_gpu_tests: ok\n");
  return 0;
}
