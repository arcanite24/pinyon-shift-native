#include "world/cell_streamer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "fh1data/worker_pool.h"

namespace world {

namespace {

std::string cell_cache_key_mesh(CellKey k) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "cell_%d_%d_mesh", k.cx, k.cz);
  return std::string(buf);
}

std::string cell_cache_key_meta(CellKey k) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "cell_%d_%d_meta", k.cx, k.cz);
  return std::string(buf);
}

std::string cell_cache_key_bvh4(CellKey k) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "cell_%d_%d_bvh4", k.cx, k.cz);
  return std::string(buf);
}

constexpr char kCellMetaMagic[4] = {'S', 'H', 'C', 'M'};

struct CellMetaHeader {
  char magic[4];
  uint32_t version;
  uint32_t prop_count;
  uint32_t tree_count;
  uint32_t has_collision;
  uint32_t reserved;
};
static_assert(sizeof(CellMetaHeader) == 24);

std::vector<uint8_t> encode_cell_meta(const CellPayload& p) {
  CellMetaHeader hdr{};
  std::memcpy(hdr.magic, kCellMetaMagic, 4);
  hdr.version = 1u;
  hdr.prop_count = static_cast<uint32_t>(p.props.size());
  hdr.tree_count = static_cast<uint32_t>(p.trees.size());
  hdr.has_collision = (p.collision && p.collision->triangle_count() > 0) ? 1u : 0u;
  const size_t prop_bytes = p.props.size() * sizeof(PropInstanceRecord);
  const size_t tree_bytes = p.trees.size() * sizeof(TreeInstanceRecord);
  std::vector<uint8_t> out(sizeof(CellMetaHeader) + prop_bytes + tree_bytes);
  std::memcpy(out.data(), &hdr, sizeof(hdr));
  if (prop_bytes) std::memcpy(out.data() + sizeof(hdr), p.props.data(), prop_bytes);
  if (tree_bytes) std::memcpy(out.data() + sizeof(hdr) + prop_bytes, p.trees.data(), tree_bytes);
  return out;
}

bool decode_cell_meta(std::span<const uint8_t> bytes, std::vector<PropInstanceRecord>& out_props,
                      std::vector<TreeInstanceRecord>& out_trees, bool& out_has_collision) {
  if (bytes.size() < sizeof(CellMetaHeader)) return false;
  CellMetaHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  if (std::memcmp(hdr.magic, kCellMetaMagic, 4) != 0 || hdr.version != 1u) return false;
  const uint64_t prop_bytes = static_cast<uint64_t>(hdr.prop_count) * sizeof(PropInstanceRecord);
  const uint64_t tree_bytes = static_cast<uint64_t>(hdr.tree_count) * sizeof(TreeInstanceRecord);
  if (sizeof(CellMetaHeader) + prop_bytes + tree_bytes != bytes.size()) return false;
  out_props.resize(hdr.prop_count);
  out_trees.resize(hdr.tree_count);
  if (prop_bytes) {
    std::memcpy(out_props.data(), bytes.data() + sizeof(hdr), static_cast<size_t>(prop_bytes));
  }
  if (tree_bytes) {
    std::memcpy(out_trees.data(), bytes.data() + sizeof(hdr) + static_cast<size_t>(prop_bytes),
                static_cast<size_t>(tree_bytes));
  }
  out_has_collision = (hdr.has_collision != 0u);
  return true;
}

uint64_t estimate_cell_gpu_bytes(const CellPayload& p) noexcept {
  return static_cast<uint64_t>(p.static_meshlets.vertices.size()) * sizeof(fh1::NativeMeshletVertex) +
         static_cast<uint64_t>(p.static_meshlets.indices.size()) * sizeof(uint32_t) +
         static_cast<uint64_t>(p.static_meshlets.meshlets.size()) * sizeof(fh1::NativeMeshletDesc) +
         static_cast<uint64_t>(p.props.size()) * sizeof(PropInstanceRecord) +
         static_cast<uint64_t>(p.trees.size()) * sizeof(TreeInstanceRecord);
}

}  // namespace

// -----------------------------------------------------------------------------
// CellGridStreamer
// -----------------------------------------------------------------------------
CellGridStreamer::CellGridStreamer(CellStreamerConfig cfg, CellLoaderFn loader,
                                   fh1::NativeCache* cache, uint64_t source_fingerprint)
    : cfg_(cfg),
      loader_(std::move(loader)),
      cache_(cache),
      source_fingerprint_(source_fingerprint) {
  if (cfg_.unload_radius_m < cfg_.load_radius_m + kCellSizeMeters * 0.5) {
    cfg_.unload_radius_m = cfg_.load_radius_m + kCellSizeMeters;
  }
  if (cache_) {
    cache_writer_ = std::make_unique<fh1::AsyncNativeCacheWriter>(*cache_);
  }
}

CellGridStreamer::~CellGridStreamer() {
  wait_idle();
  if (cache_writer_) cache_writer_->flush();
}

std::shared_ptr<CellPayload> CellGridStreamer::load_cell_impl(CellKey key) {
  // 1. Try NativeCache fast path (pre-clusterized meshlets + props/trees meta + BVH4).
  // Require complete bundle: if collision was present when cached and BVH4 was evicted,
  // fall back to cold build so collision is never silently dropped.
  if (cache_) {
    std::vector<uint8_t> mesh_bytes;
    std::vector<uint8_t> meta_bytes;
    const bool has_mesh = cache_->get(fh1::NativeCacheKind::kMeshletPack, cell_cache_key_mesh(key),
                                      source_fingerprint_, mesh_bytes);
    const bool has_meta = has_mesh && cache_->get(fh1::NativeCacheKind::kMeshletPack,
                                                  cell_cache_key_meta(key), source_fingerprint_, meta_bytes);
    if (has_mesh && has_meta) {
      fh1::NativeMeshletPack pack;
      std::vector<PropInstanceRecord> props;
      std::vector<TreeInstanceRecord> trees;
      bool expects_collision = false;
      if (fh1::decode_native_meshlet_pack(mesh_bytes, pack) &&
          decode_cell_meta(meta_bytes, props, trees, expects_collision)) {
        std::shared_ptr<drive::MeshGround> collision;
        bool collision_ok = true;
        if (expects_collision) {
          std::vector<uint8_t> bvh_bytes;
          if (cache_->get(fh1::NativeCacheKind::kCollisionBvh4, cell_cache_key_bvh4(key),
                          source_fingerprint_, bvh_bytes)) {
            auto ground = drive::MeshGround::deserialize_bvh4(bvh_bytes);
            if (ground) {
              collision = std::shared_ptr<drive::MeshGround>(std::move(ground));
            } else {
              collision_ok = false;
            }
          } else {
            collision_ok = false;
          }
        }
        if (collision_ok) {
          auto payload = std::make_shared<CellPayload>();
          payload->key = key;
          payload->source_fingerprint = source_fingerprint_;
          payload->static_meshlets = std::move(pack);
          payload->props = std::move(props);
          payload->trees = std::move(trees);
          payload->collision = std::move(collision);
          payload->loaded_from_cache = true;
          payload->estimated_gpu_bytes = estimate_cell_gpu_bytes(*payload);
          return payload;
        }
      }
    }
  }

  // 2. Fallback to live cell loader callback and enqueue background NativeCache population.
  std::shared_ptr<CellPayload> payload = loader_ ? loader_(key) : std::make_shared<CellPayload>();
  if (!payload) return nullptr;
  payload->key = key;
  payload->source_fingerprint = source_fingerprint_;
  payload->loaded_from_cache = false;
  if (payload->estimated_gpu_bytes == 0) {
    payload->estimated_gpu_bytes = estimate_cell_gpu_bytes(*payload);
  }

  if (cache_writer_) {
    const bool has_meshlets =
        !payload->static_meshlets.meshlets.empty() || !payload->static_meshlets.vertices.empty();
    const bool has_content = has_meshlets || !payload->props.empty() || !payload->trees.empty() ||
                             (payload->collision && payload->collision->triangle_count() > 0);
    if (has_content) {
      auto pack_ref = payload;
      cache_writer_->enqueue(
          fh1::NativeCacheKind::kMeshletPack, cell_cache_key_mesh(key), source_fingerprint_,
          [pack_ref]() { return fh1::encode_native_meshlet_pack(pack_ref->static_meshlets); });
      cache_writer_->enqueue(
          fh1::NativeCacheKind::kMeshletPack, cell_cache_key_meta(key), source_fingerprint_,
          [pack_ref]() { return encode_cell_meta(*pack_ref); });
      if (payload->collision && payload->collision->triangle_count() > 0) {
        auto col_ref = payload->collision;
        cache_writer_->enqueue(
            fh1::NativeCacheKind::kCollisionBvh4, cell_cache_key_bvh4(key), source_fingerprint_,
            [col_ref]() { return col_ref->serialize_bvh4(); });
      }
    }
  }
  return payload;
}

CellStreamFrameDelta CellGridStreamer::update(double x, double z, double vx, double vz) {
  ++frame_index_;
  CellStreamFrameDelta delta{};

  // Compute velocity-based forward lead target.
  double lead_x = vx * cfg_.lead_time_s;
  double lead_z = vz * cfg_.lead_time_s;
  const double lead_len = std::hypot(lead_x, lead_z);
  if (lead_len > cfg_.max_lead_m && lead_len > 1e-6) {
    const double s = cfg_.max_lead_m / lead_len;
    lead_x *= s;
    lead_z *= s;
  }
  const double focus_x = x + lead_x;
  const double focus_z = z + lead_z;

  auto min_dist_sq = [&](CellKey k) -> double {
    const auto c = cell_center_world(k);
    const double d_car = (c[0] - x) * (c[0] - x) + (c[1] - z) * (c[1] - z);
    const double d_foc = (c[0] - focus_x) * (c[0] - focus_x) + (c[1] - focus_z) * (c[1] - focus_z);
    return std::min(d_car, d_foc);
  };
  auto cancel_pending_retire = [&](CellKey k) {
    retiring_.erase(std::remove_if(retiring_.begin(), retiring_.end(),
                                   [&](const RetiredCell& rc) { return rc.key == k; }),
                    retiring_.end());
  };

  // 1. Drain completed async loads up to max_uploads_per_frame.
  std::vector<ReadyItem> ready;
  {
    std::lock_guard<std::mutex> lk(ready_mu_);
    const size_t take = std::min<size_t>(ready_queue_.size(),
                                         std::max<uint32_t>(cfg_.max_uploads_per_frame, 1u));
    ready.reserve(take);
    for (size_t i = 0; i < take; ++i) {
      ready.push_back(std::move(ready_queue_[i]));
    }
    ready_queue_.erase(ready_queue_.begin(), ready_queue_.begin() + static_cast<ptrdiff_t>(take));
  }

  const double unload_r2 = cfg_.unload_radius_m * cfg_.unload_radius_m;
  for (auto& item : ready) {
    in_flight_.erase(item.key);
    auto& cell = item.payload;
    if (!cell) continue;
    if (min_dist_sq(cell->key) > unload_r2) {
      // Cell fell outside hysteresis radius while loading; retire without activating.
      retiring_.push_back({cell->key, std::move(cell), frame_index_ + cfg_.retire_frame_delay});
      continue;
    }
    cancel_pending_retire(cell->key);
    if (cell->loaded_from_cache) {
      ++delta.cache_hits;
      ++total_cache_hits_;
    } else {
      ++delta.fresh_builds;
      ++total_fresh_builds_;
    }
    ++total_loaded_;
    delta.entered.push_back(cell->key);
    resident_[cell->key] = std::move(cell);
  }

  // 2. Compute desired 128 m cell ring around (focus_x, focus_z), sorted nearest-first.
  const int32_t cell_span = static_cast<int32_t>(std::ceil(cfg_.load_radius_m / kCellSizeMeters)) + 1;
  const CellKey center_key = cell_key_for_world(focus_x, focus_z);
  const double load_r2 = cfg_.load_radius_m * cfg_.load_radius_m;

  struct DesiredCell {
    CellKey key;
    double dist2;
  };
  std::vector<DesiredCell> desired;
  for (int32_t dz = -cell_span; dz <= cell_span; ++dz) {
    for (int32_t dx = -cell_span; dx <= cell_span; ++dx) {
      const CellKey k{center_key.cx + dx, center_key.cz + dz};
      const auto c = cell_center_world(k);
      const double ddx = c[0] - focus_x;
      const double ddz = c[1] - focus_z;
      const double d2 = ddx * ddx + ddz * ddz;
      if (d2 <= load_r2) {
        desired.push_back({k, d2});
      }
    }
  }
  std::sort(desired.begin(), desired.end(),
            [](const DesiredCell& a, const DesiredCell& b) { return a.dist2 < b.dist2; });

  // 3. Schedule only entering cells (desired - resident - in_flight).
  for (const DesiredCell& dc : desired) {
    if (resident_.find(dc.key) != resident_.end() || in_flight_.find(dc.key) != in_flight_.end()) {
      continue;
    }
    if (!cfg_.async_load) {
      auto cell = load_cell_impl(dc.key);
      if (cell) {
        cancel_pending_retire(dc.key);
        if (cell->loaded_from_cache) {
          ++delta.cache_hits;
          ++total_cache_hits_;
        } else {
          ++delta.fresh_builds;
          ++total_fresh_builds_;
        }
        ++total_loaded_;
        delta.entered.push_back(dc.key);
        resident_[dc.key] = std::move(cell);
      }
    } else {
      in_flight_.insert(dc.key);
      {
        std::lock_guard<std::mutex> lk(ready_mu_);
        active_jobs_.fetch_add(1, std::memory_order_acq_rel);
      }
      const CellKey k = dc.key;
      fh1::submit_job(
          [this, k]() {
            std::shared_ptr<CellPayload> payload;
            try {
              payload = load_cell_impl(k);
            } catch (...) {
              payload.reset();
            }
            {
              std::lock_guard<std::mutex> lk(ready_mu_);
              ready_queue_.push_back({k, std::move(payload)});
              if (active_jobs_.fetch_sub(1, std::memory_order_acq_rel) == 1u) {
                ready_cv_.notify_all();
              }
            }
          },
          fh1::JobLane::kBackground);
    }
  }

  // 4. Unload leaving cells beyond hysteresis unload_radius_m of BOTH car and focus.
  for (auto it = resident_.begin(); it != resident_.end();) {
    if (min_dist_sq(it->first) > unload_r2) {
      delta.left.push_back(it->first);
      retiring_.push_back({it->first, std::move(it->second), frame_index_ + cfg_.retire_frame_delay});
      it = resident_.erase(it);
    } else {
      ++it;
    }
  }

  // 5. Retire cells whose GPU frames-in-flight fence delay has elapsed.
  auto ret_it = retiring_.begin();
  while (ret_it != retiring_.end()) {
    if (ret_it->retire_at_frame <= frame_index_) {
      delta.retired.push_back(ret_it->key);
      ret_it = retiring_.erase(ret_it);
    } else {
      ++ret_it;
    }
  }

  delta.in_flight_loads = static_cast<uint32_t>(in_flight_.size());
  delta.resident_cells = static_cast<uint32_t>(resident_.size());
  return delta;
}

void CellGridStreamer::wait_idle() {
  {
    std::unique_lock<std::mutex> lk(ready_mu_);
    ready_cv_.wait(lk, [this] { return active_jobs_.load(std::memory_order_acquire) == 0u; });
  }
  if (cache_writer_) cache_writer_->flush();
}

bool CellGridStreamer::is_resident(CellKey key) const {
  return resident_.find(key) != resident_.end();
}

std::shared_ptr<const CellPayload> CellGridStreamer::get_resident(CellKey key) const {
  auto it = resident_.find(key);
  return (it != resident_.end()) ? it->second : nullptr;
}

size_t CellGridStreamer::pending_load_count() const {
  return in_flight_.size();
}

// -----------------------------------------------------------------------------
// CarAssetPool (with per-key singleflight deduplication & atomic telemetry)
// -----------------------------------------------------------------------------
std::shared_ptr<SharedCarAsset> CarAssetPool::acquire(const std::string& car_key,
                                                      const CarBuilderFn& builder) {
  total_acquires_.fetch_add(1, std::memory_order_relaxed);
  std::shared_ptr<InFlightBuild> wait_flight;
  std::shared_ptr<InFlightBuild> own_flight;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(car_key);
    if (it != entries_.end()) {
      if (auto existing = it->second.lock()) {
        cache_hits_.fetch_add(1, std::memory_order_relaxed);
        saved_vram_bytes_.fetch_add(existing->gpu_vertex_bytes + existing->gpu_texture_bytes,
                                    std::memory_order_relaxed);
        return existing;
      }
    }
    auto fit = in_flight_.find(car_key);
    if (fit != in_flight_.end()) {
      wait_flight = fit->second;
    } else {
      own_flight = std::make_shared<InFlightBuild>();
      in_flight_[car_key] = own_flight;
    }
  }

  if (wait_flight) {
    std::unique_lock<std::mutex> flk(wait_flight->mu);
    wait_flight->cv.wait(flk, [&] { return wait_flight->done; });
    if (wait_flight->result) {
      cache_hits_.fetch_add(1, std::memory_order_relaxed);
      saved_vram_bytes_.fetch_add(
          wait_flight->result->gpu_vertex_bytes + wait_flight->result->gpu_texture_bytes,
          std::memory_order_relaxed);
    }
    return wait_flight->result;
  }

  std::shared_ptr<SharedCarAsset> built;
  try {
    built = builder ? builder(car_key) : nullptr;
    if (built) built->car_key = car_key;
  } catch (...) {
    built.reset();
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    in_flight_.erase(car_key);
    if (built) {
      entries_[car_key] = built;
      unique_builds_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  {
    std::lock_guard<std::mutex> flk(own_flight->mu);
    own_flight->result = built;
    own_flight->done = true;
  }
  own_flight->cv.notify_all();
  return built;
}

size_t CarAssetPool::collect_unused() {
  std::lock_guard<std::mutex> lk(mu_);
  size_t removed = 0;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->second.expired()) {
      it = entries_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

size_t CarAssetPool::live_unique_assets() const {
  std::lock_guard<std::mutex> lk(mu_);
  size_t live = 0;
  for (const auto& [k, wp] : entries_) {
    if (!wp.expired()) ++live;
  }
  return live;
}

}  // namespace world
