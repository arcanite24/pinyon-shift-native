#include "world/world.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>

#include "fh1data/members.h"
#include "fh1data/placements.h"
#include "fh1data/props.h"
#include "fh1data/rmb.h"

namespace fs = std::filesystem;

namespace world {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// "name_LOD01_x" -> ("name_LOD**_x", 1); no LOD number -> (name, -1).
std::string lod_family(const std::string& name, int& lod) {
  std::string n = lower(name);
  for (size_t at = n.find("lod"); at != std::string::npos; at = n.find("lod", at + 1)) {
    if (at + 4 < n.size() && std::isdigit(static_cast<unsigned char>(n[at + 3])) &&
        std::isdigit(static_cast<unsigned char>(n[at + 4]))) {
      lod = (n[at + 3] - '0') * 10 + (n[at + 4] - '0');
      n[at + 3] = n[at + 4] = '*';
      return n;
    }
  }
  lod = -1;
  return n;
}

constexpr const char* kCacheMagic = "shift-world-index 1";

}  // namespace

const char* surface_name(Surface s) {
  switch (s) {
    case Surface::kRoad: return "road";
    case Surface::kTerrain: return "terrain";
    case Surface::kWater: return "water";
    case Surface::kOther: return "other";
    case Surface::kProp: return "prop";
  }
  return "?";
}

bool skipped_object(const std::string& object) {
  const std::string o = lower(object);
  return has(o, "terr_cube") || has(o, "uberlod") || has(o, "middist") || has(o, "occluder");
}

bool skipped_material(const std::string& material) { return lower(material) == "occluder"; }

bool ground_object(const std::string& object) {
  const std::string o = lower(object);
  for (const char* part : {"armco", "barrier", "railing", "cable", "tether", "gate", "fence", "wall", "truck",
                           "flag", "banner", "sign", "pole"})
    if (has(o, part)) return false;
  return true;
}

Surface classify(const std::string& object, const std::string& material) {
  const std::string o = lower(object);
  if (has(o, "river") || has(o, "water") || has(o, "lake")) return Surface::kWater;
  if (has(o, "_road") || has(o, "road_")) return Surface::kRoad;
  if (has(o, "terr") || has(o, "rocks") || has(o, "patch")) return Surface::kTerrain;
  const std::string m = lower(material);
  if (has(m, "water") || has(m, "river") || has(m, "lake")) return Surface::kWater;
  if (has(m, "road") || has(m, "asphalt") || has(m, "tarmac") || has(m, "concrete") || has(m, "junction") ||
      has(m, "carpark") || has(m, "car_park") || has(m, "parking"))
    return Surface::kRoad;
  if (has(m, "terrain") || has(m, "grass") || has(m, "dirt") || has(m, "gravel") || has(m, "ground") ||
      has(m, "rock") || has(m, "sand") || has(m, "mud") || has(m, "verge"))
    return Surface::kTerrain;
  return Surface::kOther;
}

struct World::Impl {
  fh1::MemberSource source;
  fh1::PvsObjectTable table;
  fh1::PropWorld props;
  std::unique_ptr<fh1::PropModels> prop_models;
  bool have_props = false;
};

World::World() : impl_(std::make_unique<Impl>()) {}
World::~World() = default;

size_t World::placement_count() const { return impl_->have_props ? impl_->props.placements.size() : 0; }

bool World::open(const fs::path& colorado, const fs::path& cache, std::string* error) {
  const fs::path zip = colorado / "bin.zip";
  if (!impl_->source.open(zip, error)) return false;
  std::error_code ec;
  const auto zip_size = fs::file_size(zip, ec);

  // The model index: the cache, or a parse of every model.
  models_.clear();
  bool cached = false;
  if (!cache.empty()) {
    std::ifstream in(cache);
    std::string magic;
    unsigned long long size = 0;
    if (std::getline(in, magic) && magic == kCacheMagic && (in >> size) && size == zip_size) {
      ModelInfo m;
      int ws = 0, ok = 0;
      while (in >> m.name >> m.box_min[0] >> m.box_min[1] >> m.box_min[2] >> m.box_max[0] >> m.box_max[1] >>
             m.box_max[2] >> ws >> ok) {
        m.world_space = ws != 0;
        m.ok = ok != 0;
        models_.push_back(m);
      }
      cached = !models_.empty();
    }
  }
  if (!cached) {
    models_.clear();
    std::vector<uint8_t> bytes;
    for (const auto& name : impl_->source.with_suffix(".rmb.bin")) {
      ModelInfo m;
      m.name = name;
      fh1::RenderModel model;
      std::string e;
      if (impl_->source.read(name, bytes, &e) && fh1::parse_render_model(bytes, model, &e)) {
        m.ok = true;
        m.box_min = model.box_min;
        m.box_max = model.box_max;
        m.world_space = fh1::rmb_is_world_space(model);
      }
      models_.push_back(m);
    }
    if (!cache.empty()) {
      fs::create_directories(cache.parent_path(), ec);
      std::ofstream out(cache, std::ios::binary);
      out << kCacheMagic << "\n" << zip_size << "\n";
      char line[256];
      for (const auto& m : models_) {
        std::snprintf(line, sizeof line, "%s %.9g %.9g %.9g %.9g %.9g %.9g %d %d\n", m.name.c_str(), m.box_min[0],
                      m.box_min[1], m.box_min[2], m.box_max[0], m.box_max[1], m.box_max[2], int(m.world_space),
                      int(m.ok));
        out << line;
      }
    }
  }

  // Props: the object table and every placement.
  const fs::path pvs = colorado / "Ribbon_00" / "Colorado_00.pvs";
  std::ifstream f(pvs, std::ios::binary);
  if (f) {
    std::vector<uint8_t> pvs_bytes((std::istreambuf_iterator<char>(f)), {});
    fh1::PropLoadOptions options;
    options.census_first_rule = false;
    std::string e;
    if (fh1::parse_object_table(pvs_bytes, impl_->table, &e) &&
        fh1::load_props(impl_->source, impl_->props, &e, options)) {
      impl_->prop_models = std::make_unique<fh1::PropModels>(impl_->source);
      impl_->have_props = true;
    } else if (error) {
      *error = "props: " + e;  // not fatal: the caller may go on without props
    }
  }
  return true;
}

bool World::gather(double x, double z, const GatherOptions& options, Mesh& out, GatherStats* stats,
                   std::string* error) {
  GatherStats local;
  GatherStats& st = stats ? *stats : local;
  out = {};

  auto add_triangles = [&](const fh1::RenderModel& model, uint32_t model_index, const fh1::Placement* placement,
                           const std::map<std::string, int>* lowest) {
    for (const auto& o : model.objects) {
      if (!placement && skipped_object(o.name)) {
        ++st.skipped_far;
        continue;
      }
      if (lowest) {
        int lod;
        const std::string family = lod_family(o.name, lod);
        if (lod >= 0) {
          auto it = lowest->find(family);
          if (it != lowest->end() && it->second != lod) {
            ++st.skipped_lod;
            continue;
          }
        }
      }
      ++st.objects;
      // File frame to the game frame: z negated.
      auto position = [&](uint32_t i) {
        const auto p = o.position(i);
        if (!placement) return std::array<float, 3>{p[0], p[1], -p[2]};
        const auto q = placement->apply(p);
        return std::array<float, 3>{float(q[0]), float(q[1]), float(-q[2])};
      };
      for (const auto& s : o.submeshes) {
        if (skipped_material(s.material)) continue;
        const uint8_t solid = !placement && ground_object(o.name);
        const Surface surface = placement ? Surface::kProp : classify(o.name, s.material);
        const auto tri = fh1::rmb_triangle_list(s);
        size_t added = 0;
        for (size_t t = 0; t + 2 < tri.size(); t += 3) {
          out.positions.push_back(position(tri[t]));
          out.positions.push_back(position(tri[t + 1]));
          out.positions.push_back(position(tri[t + 2]));
          out.surface.push_back(surface);
          out.model.push_back(model_index);
          out.ground.push_back(solid);
          ++added;
        }
        st.materials[s.material] += added;
        if (!placement) st.object_names[o.name] += added;
      }
    }
  };

  // The point in the file frame, where the boxes and placements are.
  z = -z;
  // World-space models whose header box comes within the radius.
  std::vector<std::pair<uint32_t, fh1::RenderModel>> selected;
  std::vector<uint8_t> bytes;
  const double r = options.radius;
  for (uint32_t i = 0; i < models_.size(); ++i) {
    const ModelInfo& m = models_[i];
    if (!m.ok || !m.world_space) continue;
    const double dx = std::max({0.0, double(m.box_min[0]) - x, x - double(m.box_max[0])});
    const double dz = std::max({0.0, double(m.box_min[2]) - z, z - double(m.box_max[2])});
    if (dx * dx + dz * dz > r * r) continue;
    fh1::RenderModel model;
    std::string e;
    if (!impl_->source.read(m.name, bytes, &e) || !fh1::parse_render_model(bytes, model, &e)) {
      if (error) *error = m.name + ": " + e;
      return false;
    }
    selected.emplace_back(i, std::move(model));
  }
  std::map<std::string, int> lowest;
  for (const auto& [i, model] : selected)
    for (const auto& o : model.objects) {
      int lod;
      const std::string family = lod_family(o.name, lod);
      if (lod < 0) continue;
      auto [it, added] = lowest.emplace(family, lod);
      if (!added) it->second = std::min(it->second, lod);
    }
  for (const auto& [i, model] : selected) add_triangles(model, i, nullptr, &lowest);
  st.world_models = selected.size();

  // Props near the point, one per model and spot (a zone lists one object
  // per LOD level at the same spot).
  if (options.props && impl_->have_props) {
    std::set<std::tuple<int64_t, long long, long long, long long>> once;
    std::map<std::string, uint32_t> index_of;
    for (uint32_t i = 0; i < models_.size(); ++i) index_of[models_[i].name] = i;
    for (const auto& p : impl_->props.placements) {
      const auto& t = p.placement.offset;
      const double dx = t[0] - x, dz = t[2] - z, d2 = dx * dx + dz * dz;
      if (d2 > options.prop_radius * options.prop_radius) continue;
      if (!p.has_object || p.object_id >= impl_->table.objects.size()) {
        ++st.prop_unresolved;
        continue;
      }
      const uint32_t m = impl_->table.objects[p.object_id].member;
      const int level = d2 < options.prop_lod0 * options.prop_lod0 ? 0 : 2;
      const int64_t member = impl_->prop_models->model(m) ? impl_->prop_models->lod_member(m, level) : -1;
      const fh1::RenderModel* model = member >= 0 ? impl_->prop_models->model(uint32_t(member)) : nullptr;
      if (!model) {
        ++st.prop_unresolved;
        continue;
      }
      if (!once.insert({member, std::llround(t[0] * 100), std::llround(t[1] * 100), std::llround(t[2] * 100)})
               .second)
        continue;
      const auto it = index_of.find(fh1::PropModels::member_name(uint32_t(member)));
      add_triangles(*model, it != index_of.end() ? it->second : 0, &p.placement, nullptr);
      ++st.props;
    }
  }
  return true;
}

bool find_spawn(const Mesh& mesh, double x, double z, Spawn& out, const double* prefer_heading) {
  double best = -1;
  for (size_t t = 0; t < mesh.triangles(); ++t) {
    if (mesh.surface[t] != Surface::kRoad) continue;
    const auto& a = mesh.positions[t * 3];
    const auto& b = mesh.positions[t * 3 + 1];
    const auto& c = mesh.positions[t * 3 + 2];
    const double e1[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
    const double e2[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
    const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(len > 0) || std::fabs(n[1]) / len < 0.95) continue;  // flat enough to stand on
    if (len / 2 < 2.0) continue;                                // at least 2 m^2, a car's footprint shares it
    const double cx = (double(a[0]) + b[0] + c[0]) / 3, cz = (double(a[2]) + b[2] + c[2]) / 3;
    const double d = std::hypot(cx - x, cz - z);
    if (best >= 0 && d >= best) continue;
    best = d;
    out.position = {cx, (double(a[1]) + b[1] + c[1]) / 3, cz};
    out.distance = d;
    // The longest edge.
    const std::array<float, 3>* corners[3] = {&a, &b, &c};
    double longest = -1, hx = 0, hz = 1;
    for (int k = 0; k < 3; ++k) {
      const auto& p = *corners[k];
      const auto& q = *corners[(k + 1) % 3];
      const double ex = double(q[0]) - p[0], ez = double(q[2]) - p[2], l = std::hypot(ex, ez);
      if (l > longest) {
        longest = l;
        hx = ex;
        hz = ez;
      }
    }
    const bool flip = prefer_heading ? hx * std::sin(*prefer_heading) + hz * std::cos(*prefer_heading) < 0
                                     : (hz < 0 || (hz == 0 && hx < 0));
    if (flip) {
      hx = -hx;
      hz = -hz;
    }
    out.heading = std::atan2(hx, hz);
  }
  return best >= 0;
}

}  // namespace world
