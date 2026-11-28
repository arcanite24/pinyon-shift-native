// Versioned local little-endian native asset cache (SHNC0001).
//
// Strictly a local, user-generated runtime cache stored under the user's local
// state directory. Never redistributed; always optional with clean fallback to
// the user's disc files on cache miss, version change, or checksum mismatch.
//
// Eliminates repeated big-endian byte-swapping, Xenos 2D untiling, CPU cutout
// alpha scans, meshlet clustering, and BVH4 construction across sessions.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fh1data/xds.h"

namespace fh1 {

inline constexpr uint32_t kNativeCacheFormatVersion = 2u;

enum class NativeCacheKind : uint32_t {
  kMeshletPack = 1u,
  kTexture2Tier = 2u,
  kCollisionBvh4 = 3u,
  kCarSharedPack = 4u,
};

struct NativeCacheStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t writes = 0;
  uint64_t corrupt_rejections = 0;
  uint64_t evicted_entries = 0;
  uint64_t evicted_bytes = 0;
};

// 2-tier linear GPU-ready texture stored in NativeCache:
// - `tail_mips` (mip1..N, or base if single-mip): always resident on cell load
// - `base_mip0` (mip0): streamed on demand when within LOD budget
// - `has_cutout`: precomputed alpha-cutout flag (eliminates runtime CPU mip-0 scans)
struct NativeTexture2Tier {
  XdsBlockFormat format = XdsBlockFormat::kRgba8;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t block_bytes = 4;
  uint32_t block_texels = 1;
  std::array<uint8_t, 4> swizzle{0, 1, 2, 3};
  bool has_cutout = false;
  XdsBlockLevel base_mip0;
  std::vector<XdsBlockLevel> tail_mips;
};

// Compact pre-clusterized little-endian meshlet pack (matches shift::rhi::MeshletGpu
// and VisVertexGpu layouts without pulling shift_rhi into fh1data).
struct NativeMeshletVertex {
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float u = 0.0f;
  float normal[3] = {0.0f, 1.0f, 0.0f};
  float v = 0.0f;
  float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(NativeMeshletVertex) == 48);

struct NativeMeshletDesc {
  float bound_center[3] = {0.0f, 0.0f, 0.0f};
  float bound_radius = 0.0f;
  float cone_apex[3] = {0.0f, 0.0f, 0.0f};
  float cone_cutoff = 1.0f;
  float cone_axis[3] = {0.0f, 1.0f, 0.0f};
  uint32_t index_offset = 0;
  uint32_t vertex_offset = 0;
  uint32_t triangle_count = 0;
  uint32_t vertex_count = 0;
  uint32_t material_id = 0;
};
static_assert(sizeof(NativeMeshletDesc) == 64);

struct NativeMeshletPack {
  std::vector<NativeMeshletVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<NativeMeshletDesc> meshlets;
};

// FNV-1a 64-bit hash used for content keys, source fingerprints, and integrity checks.
uint64_t native_cache_hash64(std::span<const uint8_t> data, uint64_t seed = 14695981039346656037ull) noexcept;
uint64_t native_cache_hash64(std::string_view text, uint64_t seed = 14695981039346656037ull) noexcept;

// Encode/decode helpers for typed NativeCache payloads.
std::vector<uint8_t> encode_native_texture_2tier(const NativeTexture2Tier& tex);
bool decode_native_texture_2tier(std::span<const uint8_t> bytes, NativeTexture2Tier& out);

std::vector<uint8_t> encode_native_meshlet_pack(const NativeMeshletPack& pack);
bool decode_native_meshlet_pack(std::span<const uint8_t> bytes, NativeMeshletPack& out);

// Thread-safe local disk cache rooted at `root_dir`.
class NativeCache {
 public:
  explicit NativeCache(std::filesystem::path root_dir);

  const std::filesystem::path& root_dir() const noexcept { return root_dir_; }

  // Deterministic cache file path for (kind, key_name).
  std::filesystem::path entry_path(NativeCacheKind kind, std::string_view key_name) const;

  // Atomically writes an SHNC0001 entry via temp file + rename.
  bool put(NativeCacheKind kind, std::string_view key_name, uint64_t source_fingerprint,
           std::span<const uint8_t> payload, std::string* error = nullptr);

  // Loads and validates magic, format version, kind, source fingerprint, and FNV-1a checksum.
  // Returns false on miss or corruption (and removes corrupted files if possible).
  bool get(NativeCacheKind kind, std::string_view key_name, uint64_t expected_source_fingerprint,
           std::vector<uint8_t>& out_payload);

  bool exists(NativeCacheKind kind, std::string_view key_name) const;
  bool remove(NativeCacheKind kind, std::string_view key_name);

  // Computes total bytes of all `.shnc` files under root_dir.
  uint64_t total_disk_bytes() const;

  // Evicts oldest `.shnc` entries by last-write time until total_disk_bytes() <= max_bytes.
  // Returns the number of entries evicted.
  uint64_t trim_to_budget(uint64_t max_bytes);

  NativeCacheStats stats() const;

 private:
  std::filesystem::path root_dir_;
  mutable std::mutex stats_mu_;
  NativeCacheStats stats_{};
};

// Non-blocking background transcoder that runs producer callbacks on
// fh1::JobLane::kBackground and writes their encoded payloads to NativeCache.
class AsyncNativeCacheWriter {
 public:
  explicit AsyncNativeCacheWriter(NativeCache& cache);
  ~AsyncNativeCacheWriter();

  AsyncNativeCacheWriter(const AsyncNativeCacheWriter&) = delete;
  AsyncNativeCacheWriter& operator=(const AsyncNativeCacheWriter&) = delete;

  // Queues an async background write (non-blocking). Producer runs on JobLane::kBackground.
  void enqueue(NativeCacheKind kind, std::string key_name, uint64_t source_fingerprint,
               std::function<std::vector<uint8_t>()> producer);

  // Waits until all queued background transcode jobs have finished.
  void flush();

  uint64_t pending_count() const noexcept { return in_flight_.load(std::memory_order_acquire); }
  uint64_t completed_writes() const noexcept { return completed_.load(std::memory_order_acquire); }
  uint64_t failed_writes() const noexcept { return failed_.load(std::memory_order_acquire); }

 private:
  NativeCache& cache_;
  std::atomic<uint64_t> in_flight_{0};
  std::atomic<uint64_t> completed_{0};
  std::atomic<uint64_t> failed_{0};
  std::mutex flush_mu_;
  std::condition_variable flush_cv_;
};

}  // namespace fh1
