#include "world/collision.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <thread>

#include "fh1data/members.h"
#include "world/world.h"

namespace fs = std::filesystem;

namespace world {
namespace {

std::string lower_ascii(std::string_view sv) {
  std::string out(sv);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool xml_attribute(std::string_view xml, size_t at, std::string_view name, std::string& out) {
  const size_t end = xml.find('>', at);
  if (end == std::string_view::npos) return false;
  const std::string_view tag = xml.substr(at, end - at);
  for (size_t p = tag.find(name); p != std::string_view::npos; p = tag.find(name, p + 1)) {
    if (p == 0 || (tag[p - 1] != ' ' && tag[p - 1] != '\t' && tag[p - 1] != '\r' && tag[p - 1] != '\n')) continue;
    size_t q = p + name.size();
    while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) ++q;
    if (q >= tag.size() || tag[q] != '=') continue;
    ++q;
    while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) ++q;
    if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) continue;
    const size_t close = tag.find(tag[q], q + 1);
    if (close == std::string_view::npos) return false;
    out = std::string(tag.substr(q + 1, close - q - 1));
    return true;
  }
  return false;
}

size_t xml_find_element(std::string_view xml, std::string_view name, size_t from, size_t to) {
  const std::string open = "<" + std::string(name);
  for (size_t p = xml.find(open, from); p != std::string_view::npos && p < to; p = xml.find(open, p + 1)) {
    const size_t q = p + open.size();
    if (q < xml.size() &&
        (xml[q] == ' ' || xml[q] == '\t' || xml[q] == '/' || xml[q] == '>' || xml[q] == '\r' || xml[q] == '\n')) {
      return p;
    }
  }
  return std::string_view::npos;
}

bool xml_read_float(std::string_view xml, size_t from, size_t to, const char* element, float& out_val) {
  const size_t f = xml_find_element(xml, element, from, to);
  std::string v;
  if (f != std::string_view::npos && xml_attribute(xml, f, "value", v)) {
    out_val = float(std::atof(v.c_str()));
    return true;
  }
  return false;
}

bool xml_attr_float_any(std::string_view xml, size_t tag_pos, std::initializer_list<const char*> names, float& out_val) {
  std::string v;
  for (const char* n : names) {
    if (xml_attribute(xml, tag_pos, n, v) && !v.empty()) {
      out_val = float(std::atof(v.c_str()));
      return true;
    }
  }
  return false;
}

bool xml_sub_range(std::string_view xml, size_t from, size_t to, const char* group, size_t& s_from, size_t& s_to) {
  const size_t g = xml_find_element(xml, group, from, to);
  if (g == std::string_view::npos) return false;
  const size_t ge = xml.find(std::string("</") + group + ">", g);
  if (ge == std::string_view::npos || ge > to) return false;
  s_from = g;
  s_to = ge;
  return true;
}

}  // namespace

const CategoryCollisionParams* SurfaceTableExtras::find_category_params(std::string_view cat_a,
                                                                        std::string_view cat_b) const {
  const std::string a = lower_ascii(cat_a);
  const std::string b = lower_ascii(cat_b);
  for (const auto& p : category_params) {
    const std::string pa = lower_ascii(p.category_a);
    const std::string pb = lower_ascii(p.category_b);
    if ((pa == a && pb == b) || (pa == b && pb == a)) return &p;
  }
  return nullptr;
}

float SurfaceTableExtras::car_world_friction_mod(std::string_view surface_name, std::string_view category) const {
  if (!surface_name.empty()) {
    auto it = car_world_coll_fric_mods.find(lower_ascii(surface_name));
    if (it != car_world_coll_fric_mods.end()) return it->second;
  }
  if (!category.empty()) {
    auto it = car_world_coll_fric_mods.find(lower_ascii(category));
    if (it != car_world_coll_fric_mods.end()) return it->second;
  }
  return 1.0f;
}

bool parse_surface_physics_extras(std::string_view xml, SurfaceTableExtras& out, std::string* error) {
  out = {};
  const size_t list = xml_find_element(xml, "SurfaceTypesList", 0, xml.size());
  if (list == std::string_view::npos) {
    if (error) *error = "no SurfaceTypesList";
    return false;
  }
  const size_t list_end = xml.find("</SurfaceTypesList>", list);
  if (list_end == std::string_view::npos) {
    if (error) *error = "SurfaceTypesList not closed";
    return false;
  }
  for (size_t p = xml_find_element(xml, "SurfaceType", list + 1, list_end); p != std::string_view::npos;
       p = xml_find_element(xml, "SurfaceType", p + 1, list_end)) {
    SurfacePhysicsProps s;
    if (!xml_attribute(xml, p, "value", s.name)) {
      if (error) *error = "SurfaceType without a value";
      return false;
    }
    out.surfaces.push_back(std::move(s));
  }
  if (out.surfaces.empty()) {
    if (error) *error = "empty SurfaceTypesList";
    return false;
  }

  // Parse top-level <CarWorldCollFricMods> table if present.
  {
    size_t m0 = 0, m1 = 0;
    if (xml_sub_range(xml, 0, xml.size(), "CarWorldCollFricMods", m0, m1)) {
      const size_t body_start = xml.find('>', m0);
      if (body_start != std::string_view::npos && body_start < m1) {
        size_t pos = body_start + 1;
        while (pos < m1) {
          const size_t lt = xml.find('<', pos);
          if (lt == std::string_view::npos || lt >= m1) break;
          const size_t gt = xml.find('>', lt);
          if (gt == std::string_view::npos || gt > m1) break;
          if (lt + 1 < m1 && xml[lt + 1] != '/' && xml[lt + 1] != '!' && xml[lt + 1] != '?') {
            size_t name_end = lt + 1;
            while (name_end < gt && xml[name_end] != ' ' && xml[name_end] != '\t' && xml[name_end] != '/' &&
                   xml[name_end] != '\r' && xml[name_end] != '\n') {
              ++name_end;
            }
            const std::string tag_name(xml.substr(lt + 1, name_end - lt - 1));
            std::string key;
            if (!xml_attribute(xml, lt, "Surface", key) && !xml_attribute(xml, lt, "SurfaceType", key) &&
                !xml_attribute(xml, lt, "Name", key) && !xml_attribute(xml, lt, "Category", key)) {
              key = tag_name;
            }
            float val = 1.0f;
            if (!key.empty() && xml_attr_float_any(xml, lt, {"value", "FrictionScale", "Friction", "FrictionMod",
                                                             "Multiplier", "Scale"},
                                                   val)) {
              out.car_world_coll_fric_mods[lower_ascii(key)] = val;
            }
          }
          pos = gt + 1;
        }
      }
    }
  }

  // Parse top-level <CategoryToCategoryParams> table if present.
  {
    size_t c0 = 0, c1 = 0;
    if (xml_sub_range(xml, 0, xml.size(), "CategoryToCategoryParams", c0, c1)) {
      const size_t body_start = xml.find('>', c0);
      if (body_start != std::string_view::npos && body_start < c1) {
        std::vector<std::string> stack;
        size_t pos = body_start + 1;
        while (pos < c1) {
          const size_t lt = xml.find('<', pos);
          if (lt == std::string_view::npos || lt >= c1) break;
          const size_t gt = xml.find('>', lt);
          if (gt == std::string_view::npos || gt > c1) break;
          if (lt + 1 < c1 && xml[lt + 1] == '/') {
            if (!stack.empty()) stack.pop_back();
            pos = gt + 1;
            continue;
          }
          if (lt + 1 < c1 && (xml[lt + 1] == '!' || xml[lt + 1] == '?')) {
            pos = gt + 1;
            continue;
          }
          const bool self_closing = (gt > lt && xml[gt - 1] == '/');
          size_t name_end = lt + 1;
          while (name_end < gt && xml[name_end] != ' ' && xml[name_end] != '\t' && xml[name_end] != '/' &&
                 xml[name_end] != '\r' && xml[name_end] != '\n') {
            ++name_end;
          }
          const std::string tag_name(xml.substr(lt + 1, name_end - lt - 1));
          std::string cat_a, cat_b;
          const bool explicit_pair =
              (xml_attribute(xml, lt, "CategoryA", cat_a) || xml_attribute(xml, lt, "CatA", cat_a) ||
               xml_attribute(xml, lt, "Category0", cat_a) || xml_attribute(xml, lt, "SurfaceA", cat_a)) &&
              (xml_attribute(xml, lt, "CategoryB", cat_b) || xml_attribute(xml, lt, "CatB", cat_b) ||
               xml_attribute(xml, lt, "Category1", cat_b) || xml_attribute(xml, lt, "SurfaceB", cat_b));
          if (explicit_pair) {
            CategoryCollisionParams cp;
            cp.category_a = cat_a;
            cp.category_b = cat_b;
            cp.has_friction = xml_attr_float_any(xml, lt, {"Friction", "FrictionScale", "SlidingFriction"}, cp.friction);
            cp.has_restitution = xml_attr_float_any(xml, lt, {"Restitution", "Bounce", "Elasticity"}, cp.restitution);
            if (!self_closing) {
              const size_t close_tag = xml.find("</" + tag_name + ">", gt);
              if (close_tag != std::string_view::npos && close_tag < c1) {
                if (!cp.has_friction) cp.has_friction = xml_read_float(xml, gt, close_tag, "Friction", cp.friction);
                if (!cp.has_restitution) {
                  cp.has_restitution = xml_read_float(xml, gt, close_tag, "Restitution", cp.restitution) ||
                                       xml_read_float(xml, gt, close_tag, "Bounce", cp.restitution);
                }
                pos = close_tag + tag_name.size() + 3;
                out.category_params.push_back(std::move(cp));
                continue;
              }
            }
            out.category_params.push_back(std::move(cp));
          } else if (stack.size() == 1) {
            // Nested form: <CategoryA><CategoryB Friction="..." Restitution="..."/></CategoryA>
            CategoryCollisionParams cp;
            cp.category_a = stack.back();
            cp.category_b = tag_name;
            cp.has_friction = xml_attr_float_any(xml, lt, {"Friction", "FrictionScale", "SlidingFriction"}, cp.friction);
            cp.has_restitution = xml_attr_float_any(xml, lt, {"Restitution", "Bounce", "Elasticity"}, cp.restitution);
            if (!self_closing) {
              const size_t close_tag = xml.find("</" + tag_name + ">", gt);
              if (close_tag != std::string_view::npos && close_tag < c1) {
                if (!cp.has_friction) cp.has_friction = xml_read_float(xml, gt, close_tag, "Friction", cp.friction);
                if (!cp.has_restitution) {
                  cp.has_restitution = xml_read_float(xml, gt, close_tag, "Restitution", cp.restitution) ||
                                       xml_read_float(xml, gt, close_tag, "Bounce", cp.restitution);
                }
                if (cp.has_friction || cp.has_restitution) out.category_params.push_back(std::move(cp));
                pos = close_tag + tag_name.size() + 3;
                continue;
              }
            }
            if (cp.has_friction || cp.has_restitution) out.category_params.push_back(std::move(cp));
          } else if (!self_closing) {
            stack.push_back(tag_name);
          }
          pos = gt + 1;
        }
      }
    }
  }

  const size_t defs = xml_find_element(xml, "SurfaceTypes", list_end, xml.size());
  if (defs == std::string_view::npos) {
    if (error) *error = "no SurfaceTypes";
    return false;
  }
  const size_t defs_end = xml.find("</SurfaceTypes>", defs);
  if (defs_end == std::string_view::npos) {
    if (error) *error = "SurfaceTypes not closed";
    return false;
  }

  for (auto& s : out.surfaces) {
    const size_t at = xml_find_element(xml, s.name, defs + 1, defs_end);
    if (at == std::string_view::npos) continue;
    const size_t end = xml.find("</" + s.name + ">", at);
    if (end == std::string_view::npos || end > defs_end) continue;
    s.defined = true;
    xml_attribute(xml, at, "Category", s.category);

    // Retail SurfaceTypes.xml stores MaxSteeringLockCap in degrees (0 = uncapped).
    float lock_cap_deg = 0.0f;
    if (xml_read_float(xml, at, end, "MaxSteeringLockCap", lock_cap_deg) && lock_cap_deg > 0.0f) {
      s.max_steering_lock_cap_deg = lock_cap_deg;
    }
    xml_read_float(xml, at, end, "LinearDrag", s.linear_drag);
    xml_read_float(xml, at, end, "AngularDrag", s.angular_drag);
    if (s.linear_drag < 0.0f) s.linear_drag = 0.0f;
    if (s.angular_drag < 0.0f) s.angular_drag = 0.0f;

    float cam_mult = 1.0f;
    if (xml_read_float(xml, at, end, "ChaseCamSpringMultiplier", cam_mult) && cam_mult > 0.0f) {
      s.chase_cam_spring_mult = cam_mult;
    }
    float st_torque = 1.0f;
    if ((xml_read_float(xml, at, end, "SteerTorque", st_torque) ||
         xml_read_float(xml, at, end, "SteerTorqueScale", st_torque)) &&
        st_torque > 0.0f) {
      s.steer_torque = st_torque;
    }
    float fric_mod = 1.0f;
    if (xml_read_float(xml, at, end, "CarWorldCollFricMod", fric_mod) && fric_mod > 0.0f) {
      s.car_world_coll_fric_mod = fric_mod;
    } else {
      s.car_world_coll_fric_mod = out.car_world_friction_mod(s.name, s.category);
    }

    size_t g0 = 0, g1 = 0;
    if (xml_sub_range(xml, at, end, "Springiness", g0, g1)) {
      xml_read_float(xml, g0, g1, "NormDepSpringNormYCutoff", s.spring_norm_y_cutoff);
      xml_read_float(xml, g0, g1, "NormDepSpringSpeedCutoff", s.spring_speed_cutoff);
    }
  }

  for (auto& s : out.surfaces) {
    if (s.defined) continue;
    const std::string saved_name = s.name;
    s = out.surfaces[0];
    s.name = saved_name;
    s.defined = false;
    const float explicit_mod = out.car_world_friction_mod(s.name, s.category);
    if (explicit_mod != 1.0f) s.car_world_coll_fric_mod = explicit_mod;
  }
  return true;
}

struct Collision::Impl {
  fh1::MemberSource source;
};

Collision::Collision() : impl_(std::make_unique<Impl>()) {}
Collision::~Collision() = default;

bool Collision::open(const fs::path& colorado, const fs::path& physics_zip, std::string* error, std::string* warning) {
  std::error_code ec;
  fs::path bin_zip = colorado / "bin.zip";
  if (!fs::exists(bin_zip, ec)) {
    const fs::path alt1 = colorado / "ColoradoDirt_pri_65.zip";
    const fs::path alt2 = colorado.parent_path().parent_path().parent_path().parent_path() / "ColoradoDirt_pri_65.zip";
    if (fs::exists(alt1, ec)) bin_zip = alt1;
    else if (fs::exists(alt2, ec)) bin_zip = alt2;
  }
  if (!impl_->source.open(bin_zip, error)) return false;
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
  surface_extras_ = {};
  std::string why;
  fh1::MemberSource physics;
  if (!physics_zip.empty() && fs::exists(physics_zip, ec) && physics.open(physics_zip, &why) &&
      physics.read("surfacetypes.xml", bytes, &why) &&
      fh1::parse_surface_types(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                               surfaces_, &why)) {
    parse_surface_physics_extras(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                 surface_extras_, nullptr);
    for (size_t i = 0; i < surfaces_.size(); ++i)
      if (!surfaces_[i].defined && warning)
        *warning += "surface " + std::to_string(i) + " (" + surfaces_[i].name + ") has no definition: Asphalt's\n";
  } else {
    surfaces_.clear();
    surface_extras_ = {};
    if (warning) *warning += "no surface table (" + physics_zip.string() + (why.empty() ? "" : ": " + why) +
                             "): grip 1 everywhere\n";
  }
  return true;
}

SurfacePhysicsProps Collision::surface_physics(int surface) const {
  if (surface >= 0 && size_t(surface) < surface_extras_.surfaces.size()) {
    return surface_extras_.surfaces[size_t(surface)];
  }
  return surface_extras_.surfaces.empty() ? SurfacePhysicsProps{} : surface_extras_.surfaces[0];
}

drive::SurfaceTyre Collision::surface_tyre(int surface) const {
  drive::SurfaceTyre t;
  if (surface >= 0 && size_t(surface) < surfaces_.size()) {
    const auto& s = surfaces_[size_t(surface)];
    t.rear_grip = s.rear_grip;
    t.off_roadness = s.off_roadness;
    t.off_road_peak_sa = s.off_road_peak_sa;
    t.handbrake_grip = s.handbrake_grip;
    t.tcs_speed_scale = s.tcs_speed_scale;
    t.max_steering_lock_cap = s.max_steering_lock_cap;
    t.steering_rate_mult = s.steering_rate_mult;
    t.aero_drag_scale = s.aero_drag_scale;
    t.vel_peak0 = s.vel_peak0;
    t.vel_peak = s.vel_peak;
    t.vel_long0 = s.vel_long0;
    t.vel_long = s.vel_long;
    t.vel_lat0 = s.vel_lat0;
    t.vel_lat = s.vel_lat;
    t.bump_noise_type = s.bump_noise_type;
    t.bump_frequency = s.bump_frequency;
    t.bump_clamp_min = s.bump_clamp_min;
    t.bump_clamp_max = s.bump_clamp_max;
    t.bump_scale = s.bump_scale;
    t.bump_offset = s.bump_offset;
    t.bump_rumble = s.bump_rumble;
    t.is_springy = s.is_springy;
    t.spring_k = s.spring_k;
    t.spring_comp_d = s.spring_comp_d;
    t.spring_reb_d = s.spring_reb_d;
    t.spring_norm_y_cutoff = s.spring_norm_y_cutoff;
    t.spring_speed_cutoff = s.spring_speed_cutoff;
    t.is_penalty_surface = s.is_penalty_surface;
    t.is_hand_of_god = s.is_hand_of_god;
  }
  if (surface >= 0 && size_t(surface) < surface_extras_.surfaces.size()) {
    const auto& ex = surface_extras_.surfaces[size_t(surface)];
    t.max_steering_lock_cap = ex.max_steering_lock_cap_deg;
    t.linear_drag = ex.linear_drag;
    t.angular_drag = ex.angular_drag;
    t.chase_cam_spring_mult = ex.chase_cam_spring_mult;
    t.steer_torque_scale = ex.steer_torque;
    t.car_world_coll_fric_mod = ex.car_world_coll_fric_mod;
    t.spring_norm_y_cutoff = ex.spring_norm_y_cutoff;
    t.spring_speed_cutoff = ex.spring_speed_cutoff;
  }
  return t;
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
  std::vector<uint32_t> hits;
  std::vector<uint32_t> missing;
  for (uint32_t k = 0; k < index_.records.size(); ++k) {
    const auto& r = index_.records[k];
    const uint32_t i = r.cell % index_.width, j = r.cell / index_.width;
    const double x0 = index_.x0 + double(i) * s, z0 = index_.z0 + double(j) * s;
    const double dx = std::max({x0 - x, 0.0, x - (x0 + s)}), dz = std::max({z0 - z, 0.0, z - (z0 + s)});
    if (dx * dx + dz * dz > radius * radius) continue;
    hits.push_back(k);
    if (!cache_.count(k)) missing.push_back(k);
  }
  auto build_core = [&](uint32_t k, Core& core, std::string* err) -> bool {
    const auto& r = index_.records[k];
    const uint32_t i = r.cell % index_.width, j = r.cell / index_.width;
    fh1::FizTile tile;
    if (!read_tile(r.tile, tile, err)) return false;
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
    return true;
  };
  if (!missing.empty()) {
    std::vector<Core> built(missing.size());
    std::vector<std::string> errs(missing.size());
    std::atomic<bool> failed{false};
    const size_t threads = std::min<size_t>(missing.size(), std::max<size_t>(1, std::thread::hardware_concurrency()));
    if (threads > 1 && missing.size() >= 4) {
      std::atomic<size_t> next{0};
      std::vector<std::thread> workers;
      workers.reserve(threads);
      for (size_t t = 0; t < threads; ++t) {
        workers.emplace_back([&]() {
          for (;;) {
            if (failed.load(std::memory_order_relaxed)) break;
            const size_t idx = next.fetch_add(1, std::memory_order_relaxed);
            if (idx >= missing.size()) break;
            if (!build_core(missing[idx], built[idx], &errs[idx])) failed.store(true, std::memory_order_relaxed);
          }
        });
      }
      for (auto& w : workers) w.join();
    } else {
      for (size_t idx = 0; idx < missing.size(); ++idx) {
        if (!build_core(missing[idx], built[idx], &errs[idx])) {
          failed.store(true, std::memory_order_relaxed);
          break;
        }
      }
    }
    if (failed.load(std::memory_order_relaxed)) {
      if (error) {
        for (const auto& e : errs) {
          if (!e.empty()) {
            *error = e;
            break;
          }
        }
      }
      return false;
    }
    for (size_t idx = 0; idx < missing.size(); ++idx) cache_.emplace(missing[idx], std::move(built[idx]));
  }
  for (uint32_t k : hits) {
    auto it = cache_.find(k);
    out.positions.insert(out.positions.end(), it->second.positions.begin(), it->second.positions.end());
    out.surface.insert(out.surface.end(), it->second.surface.begin(), it->second.surface.end());
    out.flags.insert(out.flags.end(), it->second.flags.begin(), it->second.flags.end());
    out.word.insert(out.word.end(), it->second.word.begin(), it->second.word.end());
    out.edges.insert(out.edges.end(), it->second.edges.begin(), it->second.edges.end());
    out.active.insert(out.active.end(), it->second.active.begin(), it->second.active.end());
    out.tile.insert(out.tile.end(), it->second.surface.size(), index_.records[k].tile);
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
