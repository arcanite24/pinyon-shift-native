// Unit & integration tests for Phase 4 modern data/streaming systems:
//   1. MeshGround BVH4 binary serialization/deserialization (exact raycast & sphere parity + corruption rejection)
//   2. NativeCache (SHNC0001) typed codecs, fingerprint invalidation, checksum rejection, LRU budget trim, AsyncNativeCacheWriter
//   3. 128 m fixed-grid CellGridStreamer incremental entering/leaving deltas, hysteresis, deferred retirement, cache-warm reload
//   4. CarAssetPool refcounted GPU handle deduplication and weak-handle cleanup
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "drive/mesh_ground.h"
#include "fh1data/native_cache.h"
#include "world/cell_streamer.h"

namespace {

#define CHECK(cond, msg)                                                                  \
  do {                                                                                    \
    if (!(cond)) {                                                                        \
      std::fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg));              \
      return false;                                                                       \
    }                                                                                     \
  } while (0)

std::unique_ptr<drive::MeshGround> make_test_terrain(float origin_x, float origin_z, int grid = 32) {
  std::vector<std::array<float, 3>> pos;
  std::vector<float> grip;
  std::vector<uint8_t> surf;
  pos.reserve(size_t(grid) * size_t(grid) * 6);
  grip.reserve(size_t(grid) * size_t(grid) * 2);
  surf.reserve(size_t(grid) * size_t(grid) * 2);

  auto height = [](float x, float z) {
    return 1.5f * std::sin(x * 0.05f) * std::cos(z * 0.04f) + 0.01f * x;
  };

  const float step = 4.0f;
  for (int z = 0; z < grid; ++z) {
    for (int x = 0; x < grid; ++x) {
      const float x0 = origin_x + x * step;
      const float z0 = origin_z + z * step;
      const float x1 = x0 + step;
      const float z1 = z0 + step;
      const std::array<float, 3> p00{x0, height(x0, z0), z0};
      const std::array<float, 3> p10{x1, height(x1, z0), z0};
      const std::array<float, 3> p01{x0, height(x0, z1), z1};
      const std::array<float, 3> p11{x1, height(x1, z1), z1};

      const uint8_t s = static_cast<uint8_t>((x + z) & 7);
      const float g = 0.7f + 0.05f * float(s);

      pos.push_back(p00); pos.push_back(p01); pos.push_back(p10);
      grip.push_back(g); surf.push_back(s);

      pos.push_back(p10); pos.push_back(p01); pos.push_back(p11);
      grip.push_back(g); surf.push_back(s);
    }
  }
  return std::make_unique<drive::MeshGround>(std::move(pos), std::move(grip), std::move(surf));
}

bool test_mesh_ground_bvh4_serialization() {
  auto orig = make_test_terrain(0.0f, 0.0f, 32);
  CHECK(orig->triangle_count() == 2048, "expected 2048 tris");
  CHECK(orig->node_count() > 0 && orig->packet_count() > 0, "BVH4 must be populated");

  const std::vector<uint8_t> blob = orig->serialize_bvh4();
  CHECK(blob.size() > 64, "serialized blob should be non-empty");

  auto restored = drive::MeshGround::deserialize_bvh4(blob);
  CHECK(restored != nullptr, "deserialize_bvh4 failed");
  CHECK(restored->triangle_count() == orig->triangle_count(), "triangle count mismatch");
  CHECK(restored->node_count() == orig->node_count(), "node count mismatch");
  CHECK(restored->packet_count() == orig->packet_count(), "packet count mismatch");
  CHECK(restored->depth() == orig->depth(), "depth mismatch");

  // 200 raycasts across the terrain must match bit-for-bit.
  for (int i = 0; i < 200; ++i) {
    const double rx = 2.0 + double((i * 19) % 120);
    const double rz = 2.0 + double((i * 31) % 120);
    const drive::Vec3 ro{rx, 50.0, rz};
    const drive::Vec3 rd{0.0, -1.0, 0.0};
    drive::Hit h0{}, h1{};
    const bool ok0 = orig->raycast(ro, rd, 200.0, h0);
    const bool ok1 = restored->raycast(ro, rd, 200.0, h1);
    CHECK(ok0 && ok1, "both raycasts must hit");
    CHECK(std::fabs(h0.distance - h1.distance) < 1e-12, "raycast distance mismatch");
    CHECK(h0.triangle == h1.triangle, "raycast triangle mismatch");
    CHECK(h0.surface == h1.surface, "raycast surface mismatch");
    CHECK(std::fabs(h0.grip - h1.grip) < 1e-6f, "raycast grip mismatch");
  }

  // Sphere contacts parity check.
  std::vector<drive::SphereQuery> qs;
  for (int i = 0; i < 16; ++i) {
    qs.push_back({drive::Vec3{10.0 + i * 6.0, 0.5, 10.0 + i * 5.0}, 1.5});
  }
  std::vector<drive::SphereHit> sh0(qs.size()), sh1(qs.size());
  orig->sphere_contacts(drive::Vec3{50.0, 10.0, 50.0}, qs.data(), int(qs.size()), sh0.data());
  restored->sphere_contacts(drive::Vec3{50.0, 10.0, 50.0}, qs.data(), int(qs.size()), sh1.data());
  for (size_t i = 0; i < qs.size(); ++i) {
    CHECK(std::fabs(sh0[i].depth - sh1[i].depth) < 1e-12, "sphere depth mismatch");
    CHECK(sh0[i].triangle == sh1[i].triangle, "sphere triangle mismatch");
  }

  // Corruption rejection tests: bad magic, truncated, NaN float, invalid valid_mask, and BVH4 cycle.
  std::vector<uint8_t> bad_magic = blob;
  bad_magic[0] ^= 0xFFu;
  CHECK(drive::MeshGround::deserialize_bvh4(bad_magic) == nullptr, "bad magic must fail");

  std::vector<uint8_t> truncated(blob.begin(), blob.begin() + 24);
  CHECK(drive::MeshGround::deserialize_bvh4(truncated) == nullptr, "truncated blob must fail");

  // Inject NaN into first vertex position (immediately after 32-byte Bvh4BlobHeader).
  {
    std::vector<uint8_t> nan_blob = blob;
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(nan_blob.data() + 32, &qnan, sizeof(qnan));
    CHECK(drive::MeshGround::deserialize_bvh4(nan_blob) == nullptr, "NaN vertex position must be rejected");
  }

  // Inject a self-cycle (child[0] = 0) into root Bvh4Node.
  {
    std::vector<uint8_t> cyc_blob = blob;
    const size_t pos_bytes = orig->triangle_count() * 3 * sizeof(std::array<float, 3>);
    const size_t grip_bytes = orig->triangle_count() * sizeof(float);
    const size_t surf_bytes = orig->triangle_count() * sizeof(uint8_t);
    const size_t node0_off = 32 + pos_bytes + grip_bytes + surf_bytes;
    // Bvh4Node layout: 6*4 floats (96 bytes) then int32_t child[4].
    const int32_t self_child = 0;
    std::memcpy(cyc_blob.data() + node0_off + 96, &self_child, sizeof(self_child));
    CHECK(drive::MeshGround::deserialize_bvh4(cyc_blob) == nullptr, "cyclic BVH4 node must be rejected");
  }

  std::printf("[native_cache_streaming_tests] MeshGround BVH4 serialization (2048 tris, %zu KB, NaN+cycle guard): OK\n",
              blob.size() / 1024);
  return true;
}

bool test_native_cache(const std::filesystem::path& tmp_root) {
  fh1::NativeCache cache(tmp_root / "cache");

  // 1. Typed 2-tier texture codec + put/get round-trip.
  fh1::NativeTexture2Tier tex{};
  tex.format = fh1::XdsBlockFormat::kBc3;
  tex.width = 64;
  tex.height = 64;
  tex.block_bytes = 16;
  tex.block_texels = 4;
  tex.swizzle = {2, 1, 0, 3};
  tex.has_cutout = true;
  tex.base_mip0.width = 64;
  tex.base_mip0.height = 64;
  tex.base_mip0.blocks.assign(16 * 16 * 16, 0xA5u);
  tex.tail_mips.push_back({32, 32, std::vector<uint8_t>(8 * 8 * 16, 0x5Au)});
  tex.tail_mips.push_back({16, 16, std::vector<uint8_t>(4 * 4 * 16, 0x3Cu)});

  const std::vector<uint8_t> enc_tex = fh1::encode_native_texture_2tier(tex);
  std::string err;
  CHECK(cache.put(fh1::NativeCacheKind::kTexture2Tier, "foliage/pine_needles", 0xCAFEBABEull, enc_tex, &err),
        ("cache.put failed: " + err).c_str());

  std::vector<uint8_t> got_tex_bytes;
  CHECK(cache.get(fh1::NativeCacheKind::kTexture2Tier, "foliage/pine_needles", 0xCAFEBABEull, got_tex_bytes),
        "cache.get must hit");
  fh1::NativeTexture2Tier dec_tex{};
  CHECK(fh1::decode_native_texture_2tier(got_tex_bytes, dec_tex), "decode_native_texture_2tier failed");
  CHECK(dec_tex.has_cutout && dec_tex.format == fh1::XdsBlockFormat::kBc3 &&
            dec_tex.base_mip0.blocks == tex.base_mip0.blocks &&
            dec_tex.tail_mips.size() == 2 && dec_tex.swizzle == tex.swizzle,
        "decoded 2-tier texture mismatch");

  // 2. Source fingerprint mismatch must miss.
  std::vector<uint8_t> wrong_fp;
  CHECK(!cache.get(fh1::NativeCacheKind::kTexture2Tier, "foliage/pine_needles", 0xDEADBEEFull, wrong_fp),
        "wrong fingerprint must miss");

  // 3. Bit-flip corruption must be detected and rejected.
  const std::filesystem::path p = cache.entry_path(fh1::NativeCacheKind::kTexture2Tier, "foliage/pine_needles");
  {
    std::fstream fs(p, std::ios::binary | std::ios::in | std::ios::out);
    CHECK(fs.good(), "open cache file for corruption test");
    fs.seekg(56, std::ios::beg);
    char b = 0;
    fs.read(&b, 1);
    b ^= 0x01;
    fs.seekp(56, std::ios::beg);
    fs.write(&b, 1);
  }
  std::vector<uint8_t> corrupt_out;
  CHECK(!cache.get(fh1::NativeCacheKind::kTexture2Tier, "foliage/pine_needles", 0xCAFEBABEull, corrupt_out),
        "corrupted entry must be rejected");
  CHECK(cache.stats().corrupt_rejections >= 1, "corrupt_rejections counter should increment");

  // 4. AsyncNativeCacheWriter concurrent background writes + rapid destroy stress + LRU trim_to_budget.
  for (int iter = 0; iter < 16; ++iter) {
    fh1::AsyncNativeCacheWriter rapid(cache);
    rapid.enqueue(fh1::NativeCacheKind::kCarSharedPack, "rapid_" + std::to_string(iter), 999ull, []() {
      return std::vector<uint8_t>(256, 0x55u);
    });
    // Destructor must cleanly flush without UAF.
  }
  fh1::AsyncNativeCacheWriter writer(cache);
  for (int i = 0; i < 8; ++i) {
    writer.enqueue(fh1::NativeCacheKind::kCarSharedPack, "car_" + std::to_string(i), 777ull, [i]() {
      return std::vector<uint8_t>(4096, static_cast<uint8_t>(i + 1));
    });
  }
  writer.flush();
  CHECK(writer.completed_writes() == 8 && writer.failed_writes() == 0, "all 8 async writes must succeed");

  const uint64_t before_bytes = cache.total_disk_bytes();
  CHECK(before_bytes >= 8u * 4096u, "disk bytes should reflect 8 entries");
  const uint64_t evicted = cache.trim_to_budget(12000ull);
  CHECK(evicted >= 5u, "LRU trim should evict older entries");
  CHECK(cache.total_disk_bytes() <= 12000ull, "disk usage must be within budget after trim");

  std::printf("[native_cache_streaming_tests] NativeCache SHNC0001 + checksum + async + LRU trim: OK\n");
  return true;
}

bool test_cell_streamer_and_car_pool(const std::filesystem::path& tmp_root) {
  fh1::NativeCache cache(tmp_root / "stream_cache");
  std::atomic<uint32_t> fresh_loader_calls{0};

  auto loader = [&](world::CellKey k) -> std::shared_ptr<world::CellPayload> {
    fresh_loader_calls.fetch_add(1, std::memory_order_relaxed);
    auto cell = std::make_shared<world::CellPayload>();
    cell->key = k;
    const auto center = world::cell_center_world(k);
    // Minimal meshlet pack + instanced props + direct trees + BVH4 collision.
    cell->static_meshlets.vertices.resize(3);
    cell->static_meshlets.indices = {0, 1, 2};
    fh1::NativeMeshletDesc md{};
    md.bound_center[0] = static_cast<float>(center[0]);
    md.bound_center[2] = static_cast<float>(center[1]);
    md.bound_radius = 64.0f;
    md.triangle_count = 1;
    md.vertex_count = 3;
    cell->static_meshlets.meshlets.push_back(md);
    cell->props.push_back({42u, 0u});
    cell->trees.push_back({7u, {static_cast<float>(center[0]), 0.0f, static_cast<float>(center[1])}, 0.5f, 1.0f, 0u});
    cell->collision = make_test_terrain(static_cast<float>(k.cx * world::kCellSizeMeters),
                                        static_cast<float>(k.cz * world::kCellSizeMeters), 4);
    return cell;
  };

  world::CellStreamerConfig cfg{};
  cfg.load_radius_m = 200.0;
  cfg.unload_radius_m = 340.0;
  cfg.lead_time_s = 1.0;
  cfg.max_lead_m = 128.0;
  cfg.max_uploads_per_frame = 32;
  cfg.retire_frame_delay = 2;
  cfg.async_load = true;

  // Cold pass: loads initial 128m ring around (64, 64).
  {
    world::CellGridStreamer streamer(cfg, loader, &cache, 0x12345678ull);
    streamer.update(64.0, 64.0, 0.0, 0.0);
    streamer.wait_idle();
    auto d1 = streamer.update(64.0, 64.0, 0.0, 0.0);
    CHECK(d1.resident_cells >= 5u, "initial 128m ring should be resident");
    CHECK(streamer.is_resident({0, 0}), "center cell (0,0) must be resident");
    const uint32_t initial_calls = fresh_loader_calls.load();
    CHECK(initial_calls == d1.resident_cells, "cold pass should build each cell once");

    // Small oscillation inside hysteresis: zero new loads, zero unloads.
    auto d_hyst = streamer.update(80.0, 64.0, 0.0, 0.0);
    streamer.wait_idle();
    CHECK(d_hyst.left.empty(), "hysteresis must prevent thrashing on small movement");

    // Incremental shift +128m East: ONLY newly entering eastern ring cells load (no full-radius rebuild).
    streamer.update(192.0, 64.0, 0.0, 0.0);
    streamer.wait_idle();
    auto d_shift = streamer.update(192.0, 64.0, 0.0, 0.0);
    const uint32_t incremental_calls = fresh_loader_calls.load() - initial_calls;
    CHECK(incremental_calls > 0 && incremental_calls < initial_calls,
          "moving 1 cell must load strictly fewer cells than a full-radius regather");
    CHECK(streamer.is_resident({0, 0}), "overlapping cell (0,0) must stay resident without reload");
    CHECK(streamer.is_resident({1, 0}), "new center cell (1,0) must be resident");

    // Jump far away (+2000m) -> all previous cells leave and retire after retire_frame_delay frames.
    auto d_jump = streamer.update(2064.0, 64.0, 0.0, 0.0);
    CHECK(!d_jump.left.empty(), "far jump must unload old cells");
    CHECK(d_jump.retired.empty(), "deferred retirement must wait retire_frame_delay frames");
    streamer.wait_idle();
    streamer.update(2064.0, 64.0, 0.0, 0.0);
    auto d_retire = streamer.update(2064.0, 64.0, 0.0, 0.0);
    CHECK(!d_retire.retired.empty(), "leaving cells must retire once frame delay elapses");
  }

  // Warm pass with a brand-new CellGridStreamer sharing the same NativeCache: 100% cache hits!
  const uint32_t calls_before_warm = fresh_loader_calls.load();
  {
    world::CellGridStreamer warm(cfg, loader, &cache, 0x12345678ull);
    warm.update(64.0, 64.0, 0.0, 0.0);
    warm.wait_idle();
    auto dw = warm.update(64.0, 64.0, 0.0, 0.0);
    CHECK(dw.resident_cells >= 5u, "warm pass must load ring");
    CHECK(fresh_loader_calls.load() == calls_before_warm, "warm pass must not invoke cold loader");
    CHECK(warm.total_cache_hits() == dw.resident_cells && warm.total_fresh_builds() == 0,
          "warm pass must achieve 100% NativeCache hit rate");
    auto c00 = warm.get_resident({0, 0});
    CHECK(c00 && c00->loaded_from_cache && c00->collision && c00->collision->triangle_count() == 32,
          "cached cell must restore BVH4 MeshGround directly");
    CHECK(c00->props.size() == 1 && c00->props[0].prop_asset_id == 42u,
          "cached cell must preserve instanced props");
    CHECK(c00->trees.size() == 1 && c00->trees[0].species_id == 7u,
          "cached cell must preserve direct tree instances");
  }

  // Partial cache eviction check: removing bvh4 for (0,0) must trigger cold fallback (never null collision).
  CHECK(cache.remove(fh1::NativeCacheKind::kCollisionBvh4, "cell_0_0_bvh4"), "remove bvh4 entry");
  {
    const uint32_t before_partial = fresh_loader_calls.load();
    world::CellGridStreamer partial(cfg, loader, &cache, 0x12345678ull);
    partial.update(64.0, 64.0, 0.0, 0.0);
    partial.wait_idle();
    partial.update(64.0, 64.0, 0.0, 0.0);
    auto c00 = partial.get_resident({0, 0});
    CHECK(c00 && c00->collision && c00->collision->triangle_count() == 32,
          "partial cache eviction must fall back to cold build so collision is never dropped");
    CHECK(fresh_loader_calls.load() == before_partial + 1u,
          "only the partially evicted cell (0,0) should invoke cold loader");
  }

  // Nullptr loader check: returning nullptr must not leak in_flight_ permanently.
  {
    std::atomic<bool> fail_first{true};
    world::CellGridStreamer retry(
        cfg,
        [&](world::CellKey k) -> std::shared_ptr<world::CellPayload> {
          if (fail_first.load()) return nullptr;
          return loader(k);
        },
        nullptr, 1ull);
    retry.update(64.0, 64.0, 0.0, 0.0);
    retry.wait_idle();
    retry.update(64.0, 64.0, 0.0, 0.0);
    fail_first.store(false);
    retry.wait_idle();
    auto dr = retry.update(64.0, 64.0, 0.0, 0.0);
    retry.wait_idle();
    dr = retry.update(64.0, 64.0, 0.0, 0.0);
    CHECK(retry.is_resident({0, 0}), "cell that returned nullptr once must be retryable without leaking in_flight_");
  }

  // CarAssetPool deduplication + concurrent cold-miss singleflight check.
  world::CarAssetPool pool;
  {
    std::atomic<uint32_t> race_builds{0};
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<world::SharedCarAsset>> race_out(4);
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&, t]() {
        race_out[size_t(t)] = pool.acquire("singleflight_gt40", [&](const std::string& k) {
          race_builds.fetch_add(1, std::memory_order_relaxed);
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          auto a = std::make_shared<world::SharedCarAsset>();
          a->car_key = k;
          a->gpu_vertex_bytes = 1024ull;
          return a;
        });
      });
    }
    for (auto& th : threads) th.join();
    CHECK(race_builds.load() == 1u, "concurrent cold acquires of same car_key must invoke builder strictly once");
    for (int t = 1; t < 4; ++t) {
      CHECK(race_out[0] && race_out[size_t(t)].get() == race_out[0].get(),
            "all racing callers must receive the same SharedCarAsset instance");
    }
  }
  pool.collect_unused();
  world::CarAssetPool pool2;
  std::vector<std::shared_ptr<world::SharedCarAsset>> handles;
  const char* keys[3] = {"car_viper_2013", "car_corvette_zr1", "car_focus_rs"};
  for (int slot = 0; slot < 12; ++slot) {
    auto h = pool2.acquire(keys[slot % 3], [](const std::string& k) {
      auto a = std::make_shared<world::SharedCarAsset>();
      a->car_key = k;
      a->gpu_vertex_bytes = 2ull * 1024ull * 1024ull;
      a->gpu_texture_bytes = 6ull * 1024ull * 1024ull;
      a->meshlet_count = 420u;
      return a;
    });
    CHECK(h != nullptr, "CarAssetPool::acquire failed");
    handles.push_back(std::move(h));
  }
  CHECK(pool2.unique_builds() == 3u, "12 slots across 3 cars must build only 3 GPU car assets");
  CHECK(pool2.cache_hits() == 9u, "expected 9 deduplicated car hits");
  CHECK(pool2.saved_vram_bytes() == 9ull * 8ull * 1024ull * 1024ull, "saved VRAM accounting mismatch");
  CHECK(handles[0].get() == handles[3].get(), "identical car keys must share exact GPU asset instance");

  handles.clear();
  CHECK(pool2.collect_unused() == 3u && pool2.live_unique_assets() == 0u,
        "expired car handles must be collected when external refs drop");

  std::printf("[native_cache_streaming_tests] 128m CellGridStreamer (incremental+hysteresis+warm cache) & CarAssetPool: OK\n");
  return true;
}

}  // namespace

int main() {
  const std::filesystem::path tmp_root =
      std::filesystem::temp_directory_path() / "shift_phase4_cache_stream_tests";
  std::error_code ec;
  std::filesystem::remove_all(tmp_root, ec);

  const bool ok = test_mesh_ground_bvh4_serialization() &&
                  test_native_cache(tmp_root) &&
                  test_cell_streamer_and_car_pool(tmp_root);

  std::filesystem::remove_all(tmp_root, ec);
  if (!ok) return 1;
  std::printf("native_cache_streaming_tests: ALL PASSED\n");
  return 0;
}
