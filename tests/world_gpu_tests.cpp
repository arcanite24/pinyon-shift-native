// Slice 11: the GPU-driven world's bookkeeping (no Vulkan): the arena
// allocator, the resident set found by content key across regathers, the
// draw table packing, and the culling rule and indirect command packing the
// compute shader (shaders/world_cull.comp) implements.
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

}  // namespace

int main() {
  test_arena();
  test_resident_set();
  test_keys();
  test_batches_and_packing();
  test_culling();
  test_indirect();
  test_hiz();
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("world_gpu_tests: ok\n");
  return 0;
}
