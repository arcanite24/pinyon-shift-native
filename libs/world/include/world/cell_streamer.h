// 128 m x 128 m Fixed-Grid Incremental Cell Streamer & Shared Car Asset Pool.
//
// Replaces monolithic full-radius world re-gathers with incremental cell deltas:
//   - entering_cells = desired - resident
//   - leaving_cells  = resident - keep_set (with hysteresis unload_radius > load_radius)
//   - velocity-based forward lead so fast cars preload upcoming cells ahead of arrival
//   - async loading on fh1::JobLane::kBackground, consulting fh1::NativeCache first
//   - GPU-instanced prop & direct tree records (no per-instance geometry duplication)
//   - frame-budgeted GPU promotion + safe frame-delayed retirement of leaving cells
//   - CarAssetPool for refcounted GPU mesh/texture sharing across AI/traffic slots
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "drive/mesh_ground.h"
#include "fh1data/native_cache.h"

namespace world {

inline constexpr double kCellSizeMeters = 128.0;

struct CellKey {
  int32_t cx = 0;
  int32_t cz = 0;

  bool operator==(const CellKey& o) const noexcept { return cx == o.cx && cz == o.cz; }
  bool operator!=(const CellKey& o) const noexcept { return !(*this == o); }
};

struct CellKeyHash {
  size_t operator()(const CellKey& k) const noexcept {
    const uint64_t u = (static_cast<uint64_t>(static_cast<uint32_t>(k.cx)) << 32) |
                       static_cast<uint64_t>(static_cast<uint32_t>(k.cz));
    uint64_t h = u ^ (u >> 33);
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return static_cast<size_t>(h);
  }
};

inline CellKey cell_key_for_world(double x, double z) noexcept {
  return {static_cast<int32_t>(std::floor(x / kCellSizeMeters)),
          static_cast<int32_t>(std::floor(z / kCellSizeMeters))};
}

inline std::array<double, 2> cell_center_world(CellKey k) noexcept {
  return {(static_cast<double>(k.cx) + 0.5) * kCellSizeMeters,
          (static_cast<double>(k.cz) + 0.5) * kCellSizeMeters};
}

// GPU-instanced prop record: one shared prop archetype + 4x4 transform per instance.
struct PropInstanceRecord {
  uint32_t prop_asset_id = 0;
  uint32_t material_flags = 0;
  float transform[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1,
  };
};

// Direct GPU-instanced tree record (replaces CPU-side triangle-expanded forest blobs).
struct TreeInstanceRecord {
  uint32_t species_id = 0;
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float yaw_rad = 0.0f;
  float scale = 1.0f;
  uint32_t lod_bias = 0;
};

// Immutable per-128m-cell data built on fh1::JobLane::kBackground (or restored from NativeCache).
struct CellPayload {
  CellKey key{};
  uint64_t source_fingerprint = 0;
  fh1::NativeMeshletPack static_meshlets;
  std::vector<PropInstanceRecord> props;
  std::vector<TreeInstanceRecord> trees;
  std::shared_ptr<drive::MeshGround> collision;
  bool loaded_from_cache = false;
  uint64_t estimated_gpu_bytes = 0;
};

struct CellStreamerConfig {
  double load_radius_m = 512.0;    // cells whose center is within load_radius_m are requested
  double unload_radius_m = 640.0;  // hysteresis radius (> load_radius_m) before eviction
  double lead_time_s = 1.5;        // velocity lead seconds
  double max_lead_m = 160.0;       // clamp forward lead distance
  uint32_t max_uploads_per_frame = 4u;
  uint32_t retire_frame_delay = 3u;  // frames in flight before releasing retired cell handles
  bool async_load = true;
};

struct CellStreamFrameDelta {
  std::vector<CellKey> entered;       // newly promoted to resident this frame
  std::vector<CellKey> left;          // removed from resident set this frame
  std::vector<CellKey> retired;       // safe for GPU resource free this frame
  uint32_t in_flight_loads = 0;
  uint32_t resident_cells = 0;
  uint32_t cache_hits = 0;
  uint32_t fresh_builds = 0;
};

// Incremental 128 m cell streamer with hysteresis, velocity lead, NativeCache
// integration, and frame-budgeted promotion/retirement.
class CellGridStreamer {
 public:
  using CellLoaderFn = std::function<std::shared_ptr<CellPayload>(CellKey)>;

  CellGridStreamer(CellStreamerConfig cfg, CellLoaderFn loader, fh1::NativeCache* cache = nullptr,
                   uint64_t source_fingerprint = 1ull);
  ~CellGridStreamer();

  CellGridStreamer(const CellGridStreamer&) = delete;
  CellGridStreamer& operator=(const CellGridStreamer&) = delete;

  // Advances streaming for current camera/car position and horizontal velocity (m/s).
  // Only newly entering cells are scheduled; resident cells remain untouched.
  CellStreamFrameDelta update(double x, double z, double vx = 0.0, double vz = 0.0);

  // Waits until all in-flight async cell loads finish (for tests or shutdown).
  void wait_idle();

  bool is_resident(CellKey key) const;
  std::shared_ptr<const CellPayload> get_resident(CellKey key) const;
  size_t resident_count() const noexcept { return resident_.size(); }
  size_t pending_load_count() const;

  uint64_t total_cells_loaded() const noexcept { return total_loaded_; }
  uint64_t total_cache_hits() const noexcept { return total_cache_hits_; }
  uint64_t total_fresh_builds() const noexcept { return total_fresh_builds_; }
  uint64_t frame_index() const noexcept { return frame_index_; }

 private:
  struct RetiredCell {
    CellKey key;
    std::shared_ptr<CellPayload> payload;
    uint64_t retire_at_frame = 0;
  };

  std::shared_ptr<CellPayload> load_cell_impl(CellKey key);

  CellStreamerConfig cfg_;
  CellLoaderFn loader_;
  fh1::NativeCache* cache_ = nullptr;
  std::unique_ptr<fh1::AsyncNativeCacheWriter> cache_writer_;
  uint64_t source_fingerprint_ = 1ull;
  uint64_t frame_index_ = 0;

  std::unordered_map<CellKey, std::shared_ptr<CellPayload>, CellKeyHash> resident_;
  std::unordered_set<CellKey, CellKeyHash> in_flight_;
  std::vector<RetiredCell> retiring_;

  struct ReadyItem {
    CellKey key{};
    std::shared_ptr<CellPayload> payload;
  };

  mutable std::mutex ready_mu_;
  std::condition_variable ready_cv_;
  std::vector<ReadyItem> ready_queue_;
  std::atomic<uint32_t> active_jobs_{0};

  uint64_t total_loaded_ = 0;
  uint64_t total_cache_hits_ = 0;
  uint64_t total_fresh_builds_ = 0;
};

// Refcounted shared GPU car asset handle pool: eliminates `@slot` / `@dup` VRAM
// duplication when multiple AI or traffic vehicles share the same car ordinal/model.
struct SharedCarAsset {
  std::string car_key;
  uint64_t gpu_vertex_bytes = 0;
  uint64_t gpu_texture_bytes = 0;
  uint32_t meshlet_count = 0;
  std::shared_ptr<void> gpu_payload;
};

class CarAssetPool {
 public:
  using CarBuilderFn = std::function<std::shared_ptr<SharedCarAsset>(const std::string& car_key)>;

  // Acquires a shared refcounted car asset by logical key (e.g., "car_1234").
  // Concurrent/repeated requests for the same car share one GPU asset instance
  // (singleflight deduplication prevents duplicate concurrent cold builds).
  std::shared_ptr<SharedCarAsset> acquire(const std::string& car_key, const CarBuilderFn& builder);

  // Removes expired weak entries whose last external shared_ptr was released.
  size_t collect_unused();

  size_t live_unique_assets() const;
  uint64_t total_acquires() const noexcept { return total_acquires_.load(std::memory_order_relaxed); }
  uint64_t cache_hits() const noexcept { return cache_hits_.load(std::memory_order_relaxed); }
  uint64_t unique_builds() const noexcept { return unique_builds_.load(std::memory_order_relaxed); }
  uint64_t saved_vram_bytes() const noexcept { return saved_vram_bytes_.load(std::memory_order_relaxed); }

 private:
  struct InFlightBuild {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::shared_ptr<SharedCarAsset> result;
  };

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::weak_ptr<SharedCarAsset>> entries_;
  std::unordered_map<std::string, std::shared_ptr<InFlightBuild>> in_flight_;
  std::atomic<uint64_t> total_acquires_{0};
  std::atomic<uint64_t> cache_hits_{0};
  std::atomic<uint64_t> unique_builds_{0};
  std::atomic<uint64_t> saved_vram_bytes_{0};
};

}  // namespace world
