// A static triangle mesh as ground: a 4-wide SIMD bounding-volume hierarchy
// (BVH4) with inline SoA triangle packets, raycast by suspension and body
// collision like any drive::Ground.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "drive/vehicle.h"

namespace drive {

class MeshGround : public Ground {
 public:
  // `positions`: three corners per triangle, in the drive's world frame.
  // `grip`: one factor per triangle (empty: 1 for every triangle).
  // `surface`: one index per triangle, reported in Hit::surface (empty: -1).
  MeshGround(std::vector<std::array<float, 3>> positions, std::vector<float> grip = {},
             std::vector<uint8_t> surface = {});

  // The nearest hit along the ray within `max_distance`, either face. The
  // normal faces against the ray.
  bool raycast(const Vec3& origin, const Vec3& dir, double max_distance, Hit& hit) const override;
  // The triangles near the spheres, gathered once for all of them; per
  // sphere the deepest overlap, the normal facing `inside`'s side.
  void sphere_contacts(const Vec3& inside, const SphereQuery* spheres, int n, SphereHit* hits) const override;
  // The tyre columns by surface index, reported in Hit::tyre (empty, or an
  // index past the end: the defaults).
  void set_surface_tyres(std::vector<SurfaceTyre> tyres) { tyres_ = std::move(tyres); }

  size_t triangle_count() const { return positions_.size() / 3; }
  size_t node_count() const { return bvh4_.size(); }
  size_t packet_count() const { return packets_.size(); }
  int depth() const { return depth_; }

  // Serializes the pre-built SIMD BVH4 + triangle packets + collision attributes
  // into a compact little-endian blob so NativeCache can reload tiles without
  // rebuilding the BVH at runtime.
  std::vector<uint8_t> serialize_bvh4() const;

  // Restores a MeshGround directly from a serialize_bvh4() blob.
  // Returns nullptr if the header, bounds, or node/packet indices are invalid.
  static std::unique_ptr<MeshGround> deserialize_bvh4(std::span<const uint8_t> bytes);

 private:
  MeshGround() = default;
  // 128-byte (2 cache lines) 4-wide SoA BVH4 node.
  // child[c] >= 0: internal Bvh4Node index;
  // child[c] < 0 and != INT32_MIN: leaf starting at packet -(child[c] + 1) with count[c] TriPacket4s;
  // child[c] == INT32_MIN: empty slot (lo = +FLT_MAX, hi = -FLT_MAX).
  struct alignas(64) Bvh4Node {
    float lo_x[4];
    float lo_y[4];
    float lo_z[4];
    float hi_x[4];
    float hi_y[4];
    float hi_z[4];
    int32_t child[4];
    uint16_t count[4];
    uint32_t pad[2];
  };
  static_assert(sizeof(Bvh4Node) == 128);

  // 4 triangles packed contiguously in SoA layout for 4-wide SIMD Moller-Trumbore.
  struct alignas(16) TriPacket4 {
    float ax[4], ay[4], az[4];
    float e1x[4], e1y[4], e1z[4];
    float e2x[4], e2y[4], e2z[4];
    uint32_t tri_idx[4];
    uint32_t valid_mask;  // bits 0..3 indicate active lanes
    uint32_t pad[3];
  };
  static_assert(sizeof(TriPacket4) == 176);

  std::vector<std::array<float, 3>> positions_;
  std::vector<float> grip_;
  std::vector<uint8_t> surface_;
  std::vector<SurfaceTyre> tyres_;
  std::vector<Bvh4Node> bvh4_;
  std::vector<TriPacket4> packets_;
  int depth_ = 0;
};

}  // namespace drive
