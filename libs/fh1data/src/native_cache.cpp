#include "fh1data/native_cache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

#include "fh1data/worker_pool.h"

namespace fh1 {

namespace {

constexpr char kMagic[8] = {'S', 'H', 'N', 'C', '0', '0', '0', '1'};
constexpr uint64_t kSecondaryKeyHashSeed = 0x9e3779b97f4a7c15ull;

struct EnvelopeHeader {
  char magic[8];
  uint32_t version;
  uint32_t kind;
  uint64_t key_hash_secondary;
  uint64_t source_fingerprint;
  uint64_t payload_bytes;
  uint64_t payload_checksum;
};
static_assert(sizeof(EnvelopeHeader) == 48);

void append_bytes(std::vector<uint8_t>& out, const void* src, size_t n) {
  if (n == 0) return;
  const size_t pos = out.size();
  out.resize(pos + n);
  std::memcpy(out.data() + pos, src, n);
}

template <typename T>
void append_pod(std::vector<uint8_t>& out, const T& val) {
  append_bytes(out, &val, sizeof(T));
}

template <typename T>
bool read_pod(std::span<const uint8_t> in, size_t& cursor, T& out) {
  if (cursor + sizeof(T) > in.size()) return false;
  std::memcpy(&out, in.data() + cursor, sizeof(T));
  cursor += sizeof(T);
  return true;
}

const char* kind_subdir(NativeCacheKind kind) noexcept {
  switch (kind) {
    case NativeCacheKind::kMeshletPack:   return "meshlets";
    case NativeCacheKind::kTexture2Tier:  return "textures";
    case NativeCacheKind::kCollisionBvh4: return "collision";
    case NativeCacheKind::kCarSharedPack: return "cars";
  }
  return "other";
}

std::string hex_u64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf, 16);
}

}  // namespace

uint64_t native_cache_hash64(std::span<const uint8_t> data, uint64_t seed) noexcept {
  uint64_t h = seed;
  for (uint8_t b : data) {
    h ^= static_cast<uint64_t>(b);
    h *= 1099511628211ull;
  }
  return h;
}

uint64_t native_cache_hash64(std::string_view text, uint64_t seed) noexcept {
  return native_cache_hash64(
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()), seed);
}

// -----------------------------------------------------------------------------
// Typed Payload Codecs
// -----------------------------------------------------------------------------
std::vector<uint8_t> encode_native_texture_2tier(const NativeTexture2Tier& tex) {
  struct Header {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t block_bytes;
    uint32_t block_texels;
    uint8_t swizzle[4];
    uint32_t has_cutout;
    uint32_t base_w;
    uint32_t base_h;
    uint32_t base_bytes;
    uint32_t tail_count;
  };
  Header h{};
  h.format = static_cast<uint32_t>(tex.format);
  h.width = tex.width;
  h.height = tex.height;
  h.block_bytes = tex.block_bytes;
  h.block_texels = tex.block_texels;
  for (size_t i = 0; i < 4; ++i) h.swizzle[i] = tex.swizzle[i];
  h.has_cutout = tex.has_cutout ? 1u : 0u;
  h.base_w = tex.base_mip0.width;
  h.base_h = tex.base_mip0.height;
  h.base_bytes = static_cast<uint32_t>(tex.base_mip0.blocks.size());
  h.tail_count = static_cast<uint32_t>(tex.tail_mips.size());

  size_t total = sizeof(Header) + tex.base_mip0.blocks.size();
  for (const XdsBlockLevel& lvl : tex.tail_mips) {
    total += sizeof(uint32_t) * 3 + lvl.blocks.size();
  }
  std::vector<uint8_t> out;
  out.reserve(total);
  append_pod(out, h);
  append_bytes(out, tex.base_mip0.blocks.data(), tex.base_mip0.blocks.size());
  for (const XdsBlockLevel& lvl : tex.tail_mips) {
    const uint32_t lw = lvl.width;
    const uint32_t lh = lvl.height;
    const uint32_t lb = static_cast<uint32_t>(lvl.blocks.size());
    append_pod(out, lw);
    append_pod(out, lh);
    append_pod(out, lb);
    append_bytes(out, lvl.blocks.data(), lvl.blocks.size());
  }
  return out;
}

bool decode_native_texture_2tier(std::span<const uint8_t> bytes, NativeTexture2Tier& out) {
  struct Header {
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t block_bytes;
    uint32_t block_texels;
    uint8_t swizzle[4];
    uint32_t has_cutout;
    uint32_t base_w;
    uint32_t base_h;
    uint32_t base_bytes;
    uint32_t tail_count;
  };
  size_t cur = 0;
  Header h{};
  if (!read_pod(bytes, cur, h)) return false;
  if (h.format > static_cast<uint32_t>(XdsBlockFormat::kBc5) || h.tail_count > 32u) return false;
  if (cur + h.base_bytes > bytes.size()) return false;

  NativeTexture2Tier res{};
  res.format = static_cast<XdsBlockFormat>(h.format);
  res.width = h.width;
  res.height = h.height;
  res.block_bytes = h.block_bytes;
  res.block_texels = h.block_texels;
  for (size_t i = 0; i < 4; ++i) res.swizzle[i] = h.swizzle[i];
  res.has_cutout = (h.has_cutout != 0u);
  res.base_mip0.width = h.base_w;
  res.base_mip0.height = h.base_h;
  res.base_mip0.blocks.assign(bytes.begin() + static_cast<ptrdiff_t>(cur),
                              bytes.begin() + static_cast<ptrdiff_t>(cur + h.base_bytes));
  cur += h.base_bytes;

  res.tail_mips.reserve(h.tail_count);
  for (uint32_t i = 0; i < h.tail_count; ++i) {
    uint32_t lw = 0, lh = 0, lb = 0;
    if (!read_pod(bytes, cur, lw) || !read_pod(bytes, cur, lh) || !read_pod(bytes, cur, lb)) {
      return false;
    }
    if (cur + lb > bytes.size()) return false;
    XdsBlockLevel lvl{};
    lvl.width = lw;
    lvl.height = lh;
    lvl.blocks.assign(bytes.begin() + static_cast<ptrdiff_t>(cur),
                      bytes.begin() + static_cast<ptrdiff_t>(cur + lb));
    cur += lb;
    res.tail_mips.push_back(std::move(lvl));
  }
  if (cur != bytes.size()) return false;
  out = std::move(res);
  return true;
}

std::vector<uint8_t> encode_native_meshlet_pack(const NativeMeshletPack& pack) {
  struct Header {
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t meshlet_count;
    uint32_t reserved;
  };
  const Header h{
      static_cast<uint32_t>(pack.vertices.size()),
      static_cast<uint32_t>(pack.indices.size()),
      static_cast<uint32_t>(pack.meshlets.size()),
      0u,
  };
  const size_t total = sizeof(Header) +
                       pack.vertices.size() * sizeof(NativeMeshletVertex) +
                       pack.indices.size() * sizeof(uint32_t) +
                       pack.meshlets.size() * sizeof(NativeMeshletDesc);
  std::vector<uint8_t> out;
  out.reserve(total);
  append_pod(out, h);
  append_bytes(out, pack.vertices.data(), pack.vertices.size() * sizeof(NativeMeshletVertex));
  append_bytes(out, pack.indices.data(), pack.indices.size() * sizeof(uint32_t));
  append_bytes(out, pack.meshlets.data(), pack.meshlets.size() * sizeof(NativeMeshletDesc));
  return out;
}

bool decode_native_meshlet_pack(std::span<const uint8_t> bytes, NativeMeshletPack& out) {
  struct Header {
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t meshlet_count;
    uint32_t reserved;
  };
  size_t cur = 0;
  Header h{};
  if (!read_pod(bytes, cur, h)) return false;

  const uint64_t v_bytes = static_cast<uint64_t>(h.vertex_count) * sizeof(NativeMeshletVertex);
  const uint64_t i_bytes = static_cast<uint64_t>(h.index_count) * sizeof(uint32_t);
  const uint64_t m_bytes = static_cast<uint64_t>(h.meshlet_count) * sizeof(NativeMeshletDesc);
  if (cur + v_bytes + i_bytes + m_bytes != bytes.size()) return false;

  NativeMeshletPack res{};
  res.vertices.resize(h.vertex_count);
  res.indices.resize(h.index_count);
  res.meshlets.resize(h.meshlet_count);
  if (v_bytes) std::memcpy(res.vertices.data(), bytes.data() + cur, static_cast<size_t>(v_bytes));
  cur += static_cast<size_t>(v_bytes);
  if (i_bytes) std::memcpy(res.indices.data(), bytes.data() + cur, static_cast<size_t>(i_bytes));
  cur += static_cast<size_t>(i_bytes);
  if (m_bytes) std::memcpy(res.meshlets.data(), bytes.data() + cur, static_cast<size_t>(m_bytes));

  // Validate meshlet index and vertex ranges before accepting cached data.
  for (const NativeMeshletDesc& m : res.meshlets) {
    if (static_cast<uint64_t>(m.index_offset) + static_cast<uint64_t>(m.triangle_count) * 3ull >
        res.indices.size()) {
      return false;
    }
    if (static_cast<uint64_t>(m.vertex_offset) + static_cast<uint64_t>(m.vertex_count) >
        res.vertices.size()) {
      return false;
    }
  }
  out = std::move(res);
  return true;
}

// -----------------------------------------------------------------------------
// NativeCache
// -----------------------------------------------------------------------------
NativeCache::NativeCache(std::filesystem::path root_dir) : root_dir_(std::move(root_dir)) {}

std::filesystem::path NativeCache::entry_path(NativeCacheKind kind, std::string_view key_name) const {
  const uint64_t h = native_cache_hash64(key_name);
  return root_dir_ / kind_subdir(kind) / (hex_u64(h) + ".shnc");
}

bool NativeCache::put(NativeCacheKind kind, std::string_view key_name, uint64_t source_fingerprint,
                      std::span<const uint8_t> payload, std::string* error) {
  const std::filesystem::path dst = entry_path(kind, key_name);
  std::error_code ec;
  std::filesystem::create_directories(dst.parent_path(), ec);
  if (ec) {
    if (error) *error = "NativeCache::put: create_directories failed: " + ec.message();
    return false;
  }

  EnvelopeHeader hdr{};
  std::memcpy(hdr.magic, kMagic, sizeof(kMagic));
  hdr.version = kNativeCacheFormatVersion;
  hdr.kind = static_cast<uint32_t>(kind);
  hdr.key_hash_secondary = native_cache_hash64(key_name, kSecondaryKeyHashSeed);
  hdr.source_fingerprint = source_fingerprint;
  hdr.payload_bytes = static_cast<uint64_t>(payload.size());
  hdr.payload_checksum = native_cache_hash64(payload);

  // Unique per-process + atomic counter temp path so concurrent writers/processes never collide.
  static const uint64_t s_proc_salt =
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
      reinterpret_cast<uintptr_t>(&s_proc_salt);
  static std::atomic<uint64_t> s_tmp_seq{0};
  const uint64_t seq = s_tmp_seq.fetch_add(1, std::memory_order_relaxed);
  const std::filesystem::path tmp =
      dst.parent_path() /
      (dst.filename().string() + ".tmp." + hex_u64(s_proc_salt) + "." + std::to_string(seq));

  {
    std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
    if (!ofs) {
      if (error) *error = "NativeCache::put: failed to open temp file for writing";
      return false;
    }
    ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    if (!payload.empty()) {
      ofs.write(reinterpret_cast<const char*>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
    }
    ofs.flush();
    if (!ofs.good()) {
      ofs.close();
      std::filesystem::remove(tmp, ec);
      if (error) *error = "NativeCache::put: write failed";
      return false;
    }
  }

  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    // On Windows rename over an existing file can fail if a reader holds a handle; retry replace.
    std::filesystem::remove(dst, ec);
    ec.clear();
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
      std::filesystem::remove(tmp, ec);
      if (error) *error = "NativeCache::put: atomic rename failed: " + ec.message();
      return false;
    }
  }

  std::lock_guard<std::mutex> lk(stats_mu_);
  ++stats_.writes;
  return true;
}

bool NativeCache::get(NativeCacheKind kind, std::string_view key_name,
                      uint64_t expected_source_fingerprint, std::vector<uint8_t>& out_payload) {
  out_payload.clear();
  const std::filesystem::path src = entry_path(kind, key_name);
  std::ifstream ifs(src, std::ios::binary | std::ios::ate);
  if (!ifs) {
    std::lock_guard<std::mutex> lk(stats_mu_);
    ++stats_.misses;
    return false;
  }

  const std::streamoff file_size = ifs.tellg();
  if (file_size < static_cast<std::streamoff>(sizeof(EnvelopeHeader))) {
    ifs.close();
    std::error_code ec;
    std::filesystem::remove(src, ec);
    std::lock_guard<std::mutex> lk(stats_mu_);
    ++stats_.corrupt_rejections;
    ++stats_.misses;
    return false;
  }

  ifs.seekg(0, std::ios::beg);
  EnvelopeHeader hdr{};
  ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
  const uint64_t avail_payload_bytes = static_cast<uint64_t>(file_size) - sizeof(EnvelopeHeader);
  const uint64_t expected_secondary = native_cache_hash64(key_name, kSecondaryKeyHashSeed);
  const bool header_corrupt =
      !ifs.good() || std::memcmp(hdr.magic, kMagic, sizeof(kMagic)) != 0 ||
      hdr.payload_bytes != avail_payload_bytes;
  if (header_corrupt || hdr.version != kNativeCacheFormatVersion ||
      hdr.kind != static_cast<uint32_t>(kind) ||
      hdr.key_hash_secondary != expected_secondary ||
      hdr.source_fingerprint != expected_source_fingerprint) {
    ifs.close();
    std::lock_guard<std::mutex> lk(stats_mu_);
    if (header_corrupt) {
      std::error_code ec;
      std::filesystem::remove(src, ec);
      ++stats_.corrupt_rejections;
    }
    ++stats_.misses;
    return false;
  }

  std::vector<uint8_t> buf(static_cast<size_t>(hdr.payload_bytes));
  if (!buf.empty()) {
    ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!ifs.good()) {
      ifs.close();
      std::lock_guard<std::mutex> lk(stats_mu_);
      ++stats_.corrupt_rejections;
      ++stats_.misses;
      return false;
    }
  }
  ifs.close();

  const uint64_t actual_hash = native_cache_hash64(buf);
  if (actual_hash != hdr.payload_checksum) {
    std::error_code ec;
    std::filesystem::remove(src, ec);
    std::lock_guard<std::mutex> lk(stats_mu_);
    ++stats_.corrupt_rejections;
    ++stats_.misses;
    return false;
  }

  // Best-effort LRU touch so recently-hit entries survive trim_to_budget().
  {
    std::error_code touch_ec;
    std::filesystem::last_write_time(src, std::filesystem::file_time_type::clock::now(), touch_ec);
  }

  out_payload = std::move(buf);
  std::lock_guard<std::mutex> lk(stats_mu_);
  ++stats_.hits;
  return true;
}

bool NativeCache::exists(NativeCacheKind kind, std::string_view key_name) const {
  std::error_code ec;
  return std::filesystem::is_regular_file(entry_path(kind, key_name), ec);
}

bool NativeCache::remove(NativeCacheKind kind, std::string_view key_name) {
  std::error_code ec;
  return std::filesystem::remove(entry_path(kind, key_name), ec);
}

uint64_t NativeCache::total_disk_bytes() const {
  std::error_code exists_ec;
  if (!std::filesystem::exists(root_dir_, exists_ec)) return 0;
  uint64_t total = 0;
  std::error_code iter_ec;
  for (auto it = std::filesystem::recursive_directory_iterator(
           root_dir_, std::filesystem::directory_options::skip_permission_denied, iter_ec);
       !iter_ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(iter_ec)) {
    std::error_code file_ec;
    if (it->is_regular_file(file_ec) && !file_ec && it->path().extension() == ".shnc") {
      std::error_code sz_ec;
      const auto sz = it->file_size(sz_ec);
      if (!sz_ec) total += static_cast<uint64_t>(sz);
    }
  }
  return total;
}

uint64_t NativeCache::trim_to_budget(uint64_t max_bytes) {
  std::error_code exists_ec;
  if (!std::filesystem::exists(root_dir_, exists_ec)) return 0;

  struct Item {
    std::filesystem::path path;
    std::filesystem::file_time_type mtime;
    uint64_t size = 0;
  };
  std::vector<Item> items;
  std::vector<std::filesystem::path> orphan_tmps;
  uint64_t total = 0;
  std::error_code iter_ec;
  for (auto it = std::filesystem::recursive_directory_iterator(
           root_dir_, std::filesystem::directory_options::skip_permission_denied, iter_ec);
       !iter_ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(iter_ec)) {
    std::error_code file_ec;
    if (!it->is_regular_file(file_ec) || file_ec) continue;
    const auto& p = it->path();
    if (p.extension() == ".shnc") {
      std::error_code mt_ec, sz_ec;
      Item item{};
      item.path = p;
      item.mtime = it->last_write_time(mt_ec);
      const auto sz = it->file_size(sz_ec);
      if (mt_ec || sz_ec) continue;
      item.size = static_cast<uint64_t>(sz);
      total += item.size;
      items.push_back(std::move(item));
    } else if (p.filename().string().find(".shnc.tmp.") != std::string::npos) {
      orphan_tmps.push_back(p);
    }
  }
  for (const auto& tmp : orphan_tmps) {
    std::error_code rm_ec;
    std::filesystem::remove(tmp, rm_ec);
  }
  if (total <= max_bytes) return 0;

  // Oldest files first (LRU by last-write / last-hit time).
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.mtime != b.mtime) return a.mtime < b.mtime;
    return a.path < b.path;
  });

  uint64_t evicted = 0;
  uint64_t evicted_bytes = 0;
  for (const Item& it : items) {
    if (total <= max_bytes) break;
    std::error_code rm_ec;
    if (std::filesystem::remove(it.path, rm_ec) && !rm_ec) {
      total = (total >= it.size) ? (total - it.size) : 0;
      ++evicted;
      evicted_bytes += it.size;
    }
  }

  if (evicted > 0) {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.evicted_entries += evicted;
    stats_.evicted_bytes += evicted_bytes;
  }
  return evicted;
}

NativeCacheStats NativeCache::stats() const {
  std::lock_guard<std::mutex> lk(stats_mu_);
  return stats_;
}

// -----------------------------------------------------------------------------
// AsyncNativeCacheWriter
// -----------------------------------------------------------------------------
AsyncNativeCacheWriter::AsyncNativeCacheWriter(NativeCache& cache) : cache_(cache) {}

AsyncNativeCacheWriter::~AsyncNativeCacheWriter() {
  flush();
}

void AsyncNativeCacheWriter::enqueue(NativeCacheKind kind, std::string key_name,
                                     uint64_t source_fingerprint,
                                     std::function<std::vector<uint8_t>()> producer) {
  if (!producer) return;
  {
    std::lock_guard<std::mutex> lk(flush_mu_);
    in_flight_.fetch_add(1, std::memory_order_acq_rel);
  }
  submit_job(
      [this, kind, key = std::move(key_name), fp = source_fingerprint,
       fn = std::move(producer)]() mutable {
        // Hold flush_mu_ across decrement + notify so ~AsyncNativeCacheWriter() / flush()
        // cannot destroy flush_mu_/flush_cv_ before notify_all() completes.
        struct DoneGuard {
          AsyncNativeCacheWriter* self;
          ~DoneGuard() {
            std::lock_guard<std::mutex> lk(self->flush_mu_);
            if (self->in_flight_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
              self->flush_cv_.notify_all();
            }
          }
        } guard{this};
        try {
          std::vector<uint8_t> payload = fn();
          if (!payload.empty() && cache_.put(kind, key, fp, payload)) {
            completed_.fetch_add(1, std::memory_order_relaxed);
          } else {
            failed_.fetch_add(1, std::memory_order_relaxed);
          }
        } catch (...) {
          failed_.fetch_add(1, std::memory_order_relaxed);
        }
      },
      JobLane::kBackground);
}

void AsyncNativeCacheWriter::flush() {
  std::unique_lock<std::mutex> lk(flush_mu_);
  flush_cv_.wait(lk, [this] { return in_flight_.load(std::memory_order_acquire) == 0; });
}

}  // namespace fh1
