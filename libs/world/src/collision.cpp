#include "world/collision.h"

#include <algorithm>
#include <cmath>

#include "fh1data/members.h"
#include "world/world.h"

namespace fs = std::filesystem;

namespace world {

struct Collision::Impl {
  fh1::MemberSource source;
};

Collision::Collision() : impl_(std::make_unique<Impl>()) {}
Collision::~Collision() = default;

bool Collision::open(const fs::path& colorado, const fs::path& physics_zip, std::string* error, std::string* warning) {
  if (!impl_->source.open(colorado / "bin.zip", error)) return false;
  const fs::path col = ribbon_file(colorado, "_track_00.col");
  fh1::MemberSource ribbon;
  std::vector<uint8_t> bytes;
  if (!ribbon.open(col.parent_path(), error) || !ribbon.read(col.filename().string(), bytes, error)) return false;
  if (!fh1::parse_col(bytes, index_, error)) return false;
  occupied_cells_.clear();
  occupied_cells_.reserve(index_.records.size());
  for (const auto& rec : index_.records) occupied_cells_.insert(rec.cell);
  cache_.clear();

  surfaces_.clear();
  std::string why;
  fh1::MemberSource physics;
  std::error_code ec;
  if (!physics_zip.empty() && fs::exists(physics_zip, ec) && physics.open(physics_zip, &why) &&
      physics.read("surfacetypes.xml", bytes, &why) &&
      fh1::parse_surface_types(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                               surfaces_, &why)) {
    for (size_t i = 0; i < surfaces_.size(); ++i)
      if (!surfaces_[i].defined && warning)
        *warning += "surface " + std::to_string(i) + " (" + surfaces_[i].name + ") has no definition: Asphalt's\n";
  } else {
    surfaces_.clear();
    if (warning) *warning += "no surface table (" + physics_zip.string() + (why.empty() ? "" : ": " + why) +
                             "): grip 1 everywhere\n";
  }
  return true;
}

std::string Collision::surface_name(int surface) const {
  if (surface >= 0 && size_t(surface) < surfaces_.size()) return surfaces_[surface].name;
  return "surface " + std::to_string(surface);
}

float Collision::grip(int surface) const {
  if (surface >= 0 && size_t(surface) < surfaces_.size()) return surfaces_[surface].friction_scale;
  return surfaces_.empty() ? 1.0f : surfaces_[0].friction_scale;
}

bool Collision::read_tile(uint32_t tile, fh1::FizTile& out, std::string* error) {
  std::vector<uint8_t> bytes;
  const std::string name = std::to_string(tile) + ".fiz";
  if (!impl_->source.read(name, bytes, error)) return false;
  if (!fh1::parse_fiz(bytes, out, error)) {
    if (error) *error = name + ": " + *error;
    return false;
  }
  return true;
}

bool Collision::in_cell(const fh1::ColRecord& r, double x, double z) const {
  const uint32_t i = r.cell % index_.width, j = r.cell / index_.width;
  const double x0 = index_.x0 + double(i) * index_.cell_size, z0 = index_.z0 + double(j) * index_.cell_size;
  return x >= x0 && x < x0 + index_.cell_size && z >= z0 && z < z0 + index_.cell_size;
}

bool Collision::gather(double x, double z, double radius, CollisionMesh& out, std::string* error) {
  out = CollisionMesh{};
  const double s = index_.cell_size;
  for (uint32_t k = 0; k < index_.records.size(); ++k) {
    const auto& r = index_.records[k];
    const uint32_t i = r.cell % index_.width, j = r.cell / index_.width;
    const double x0 = index_.x0 + double(i) * s, z0 = index_.z0 + double(j) * s;
    const double dx = std::max({x0 - x, 0.0, x - (x0 + s)}), dz = std::max({z0 - z, 0.0, z - (z0 + s)});
    if (dx * dx + dz * dz > radius * radius) continue;
    auto it = cache_.find(k);
    if (it == cache_.end()) {
      fh1::FizTile tile;
      if (!read_tile(r.tile, tile, error)) return false;
      Core core;
      // The tile's mask for the active route (COLLISION.md, Route groups).
      uint16_t mask = 0x8000;
      for (size_t e = 0; e < tile.routes.size(); ++e)
        if (int(tile.routes[e]) == route_) mask |= uint16_t(1u << e);
      const double cell_x0 = index_.x0 + double(i) * s, cell_z0 = index_.z0 + double(j) * s;
      const double cell_x1 = cell_x0 + s, cell_z1 = cell_z0 + s;
      auto centroid_owned_elsewhere = [&](double cx, double cz) {
        if (cx < index_.x0 || cz < index_.z0) return false;
        const int ci = int(std::floor((cx - index_.x0) / s));
        const int cj = int(std::floor((cz - index_.z0) / s));
        if (ci < 0 || cj < 0 || uint32_t(ci) >= index_.width || uint32_t(cj) >= index_.height) return false;
        const uint32_t cidx = uint32_t(cj) * index_.width + uint32_t(ci);
        return occupied_cells_.find(cidx) != occupied_cells_.end();
      };
      for (const auto& t : tile.triangles) {
        const auto& a = tile.vertices[t.v[0]];
        const auto& b = tile.vertices[t.v[1]];
        const auto& c = tile.vertices[t.v[2]];
        const double cx = (double(a[0]) + b[0] + c[0]) / 3, cz = (double(a[2]) + b[2] + c[2]) / 3;
        if (!in_cell(r, cx, cz)) {
          // Keep large boundary triangles whose centroid overhangs into an empty
          // lattice cell (e.g. 1782.fiz triangle 0 covering flyer_053) when the
          // triangle's XZ extent actually overlaps this tile's 100 m cell.
          if (centroid_owned_elsewhere(cx, cz)) continue;
          const double min_x = std::min({double(a[0]), double(b[0]), double(c[0])});
          const double max_x = std::max({double(a[0]), double(b[0]), double(c[0])});
          const double min_z = std::min({double(a[2]), double(b[2]), double(c[2])});
          const double max_z = std::max({double(a[2]), double(b[2]), double(c[2])});
          if (max_x < cell_x0 || min_x > cell_x1 || max_z < cell_z0 || min_z > cell_z1) continue;
        }
        core.positions.push_back(a);
        core.positions.push_back(b);
        core.positions.push_back(c);
        core.surface.push_back(t.surface);
        core.flags.push_back(t.flags);
        core.word.push_back(t.word);
        core.edges.push_back(t.edges);
        core.active.push_back(mask);
      }
      it = cache_.emplace(k, std::move(core)).first;
    }
    out.positions.insert(out.positions.end(), it->second.positions.begin(), it->second.positions.end());
    out.surface.insert(out.surface.end(), it->second.surface.begin(), it->second.surface.end());
    out.flags.insert(out.flags.end(), it->second.flags.begin(), it->second.flags.end());
    out.word.insert(out.word.end(), it->second.word.begin(), it->second.word.end());
    out.edges.insert(out.edges.end(), it->second.edges.begin(), it->second.edges.end());
    out.active.insert(out.active.end(), it->second.active.begin(), it->second.active.end());
    out.tile.insert(out.tile.end(), it->second.surface.size(), r.tile);
    ++out.tiles;
  }
  return true;
}

double spawn_heading() { return std::atan2(kSpawnForwardX, kSpawnForwardZ); }

bool ground_height(const drive::Ground& ground, double x, double z, double from_y, double& y, drive::Hit* hit) {
  drive::Hit h;
  constexpr double kReach = 20;
  if (!ground.raycast({x, from_y, z}, {0, -1, 0}, kReach, h)) return false;
  y = from_y - h.distance;
  if (hit) *hit = h;
  return true;
}

}  // namespace world
