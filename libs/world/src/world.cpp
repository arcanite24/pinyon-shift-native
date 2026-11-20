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

// Bumped whenever what the index records changes (3: the far flag leaves
// out the CAGE, TERR_CUBE and occluder objects), so an older index is
// rebuilt rather than reused.
constexpr const char* kCacheMagic = "shift-world-index 3";

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

// The far ring's level of an object: 1 MIDDIST, 2 UberLOD, 0 neither.
// TERR_CUBE objects are not scenery (ours: road and diffuse effects over
// the terrain's footprint, by their name the cube maps' stand-ins).
uint8_t far_level(const std::string& object) {
  const std::string o = lower(object);
  if (has(o, "terr_cube") || has(o, "occluder") || has(o, "cage")) return 0;  // CAGE: walls round a patch
  if (has(o, "uberlod")) return 2;
  return has(o, "middist") ? 1 : 0;
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

const std::vector<fh1::PvsTexture>& World::textures() const { return impl_->table.textures; }

bool World::read_texture(uint32_t record, fh1::WorldTexture& out, std::string* error) {
  if (record >= impl_->table.textures.size()) {
    if (error) *error = "no texture record " + std::to_string(record);
    return false;
  }
  const fh1::PvsTexture& t = impl_->table.textures[record];
  char name[32];
  std::vector<uint8_t> a, b;
  bool exact = false, ok = false;
  if (t.type == fh1::kPvsTextureBix) {
    std::snprintf(name, sizeof name, "_0x%08x.bix", t.id);
    std::string base = name;
    base.insert(base.size() - 4, "_b");
    ok = impl_->source.read(name, a, error) && impl_->source.read(base, b, error) &&
         fh1::decode_bix(a, b, out, error, &exact);
  } else if (t.type == fh1::kPvsTextureCaff) {
    std::snprintf(name, sizeof name, "_0x%08x.bin", t.id);
    ok = impl_->source.read(name, a, error) && fh1::decode_caff(a, out, error, &exact);
  } else {
    if (error) *error = "texture record type " + std::to_string(t.type) + " has no file";
    return false;
  }
  if (ok && !exact) {
    if (error) *error = std::string(name) + ": sizes do not fit the layout";
    return false;
  }
  return ok;
}

Blend effect_blend(const std::string& e) {
  auto starts = [&](const char* prefix) { return e.rfind(prefix, 0) == 0; };
  if ((starts("H_ROAD") && e.find("_BLEND_") != std::string::npos) || starts("H_VBLND_DECAL") || starts("LAKE"))
    return Blend::kAlpha;
  // Props and trees drew with alpha to coverage in the frame; other effects
  // named _OPAC (vertex-blend cut-outs) are given it too (ours).
  if (starts("H_DIFF") || starts("TREE_") || e.find("_OPAC") != std::string::npos) return Blend::kCoverage;
  return Blend::kOpaque;
}

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
      int ws = 0, ok = 0, far = 0;
      while (in >> m.name >> m.box_min[0] >> m.box_min[1] >> m.box_min[2] >> m.box_max[0] >> m.box_max[1] >>
             m.box_max[2] >> ws >> ok >> far) {
        m.world_space = ws != 0;
        m.ok = ok != 0;
        m.far = far != 0;
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
        for (const auto& o : model.objects) m.far = m.far || far_level(o.name) != 0;
      }
      models_.push_back(m);
    }
    if (!cache.empty()) {
      fs::create_directories(cache.parent_path(), ec);
      std::ofstream out(cache, std::ios::binary);
      out << kCacheMagic << "\n" << zip_size << "\n";
      char line[256];
      for (const auto& m : models_) {
        std::snprintf(line, sizeof line, "%s %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d\n", m.name.c_str(),
                      m.box_min[0], m.box_min[1], m.box_min[2], m.box_max[0], m.box_max[1], m.box_max[2],
                      int(m.world_space), int(m.ok), int(m.far));
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

  std::map<decltype(MeshMaterial{}.key()), uint32_t> material_index;
  // far_pass: the far ring's objects only (the near pass skips them).
  auto add_triangles = [&](const fh1::RenderModel& model, uint32_t model_index, const fh1::Placement* placement,
                           const std::map<std::string, int>* lowest, bool far_pass = false) {
    // The model's texture list (WORLD-MATERIALS): list j for member j + 1.
    const std::vector<uint32_t>* texture_list = nullptr;
    if (options.render_data && model_index < models_.size()) {
      const std::string& name = models_[model_index].name;
      const unsigned long member = name.size() > 17 ? std::strtoul(name.c_str() + 12, nullptr, 10) : 0;
      if (member >= 1 && member - 1 < impl_->table.model_textures.size())
        texture_list = &impl_->table.model_textures[member - 1];
      if (model.uv_offset_scale.empty()) st.default_uv_models.insert(name);
    }
    size_t submesh_number = 0;  // in file order, for uvOffsetScale
    for (const auto& o : model.objects) {
      const size_t first_submesh = submesh_number;
      submesh_number += o.submeshes.size();
      const uint8_t far = far_pass ? far_level(o.name) : 0;
      if (far_pass ? far == 0 : !placement && skipped_object(o.name)) {
        if (!far_pass) ++st.skipped_far;
        continue;
      }
      // The far ring leaves out triangles wholly near the point.
      const double inner = far == 2 ? 1.5 * options.far_inner : options.far_inner;
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
      for (size_t si = 0; si < o.submeshes.size(); ++si) {
        const auto& s = o.submeshes[si];
        if (skipped_material(s.material)) continue;
        const uint8_t solid = !placement && !far && ground_object(o.name);
        const Surface surface = placement ? Surface::kProp : far ? Surface::kTerrain : classify(o.name, s.material);
        const auto tri = fh1::rmb_triangle_list(s);
        // The render data: the material, the vertex layout and the UV scale.
        uint32_t mat = kNoTexture;
        fh1::RmbVertexLayout layout;
        std::array<float, 4> uv{0, 0, 1, 1};
        int lightmap = -1;
        int splat_uv = -1;          // kSplat: TEXCOORD1, times splat_tiling
        std::array<float, 2> splat_tiling{1, 1};
        float weight_gain = -1;     // H_VBLND: c4.x of the vertex-colour weight
        bool decal_fade = false;    // H_VBLND_DECAL: the weight is the fade, from COLOR.w
        if (options.render_data) {
          if (s.material_index < model.materials.size()) {
            const fh1::RmbMaterial& m = model.materials[s.material_index];
            MeshMaterial mm;
            mm.far = far;
            mm.effect = model.effects[m.effect];
            mm.blend = effect_blend(mm.effect);
            // A slot's texture record when it has a file (types 4 and 0).
            auto slot_record = [&](size_t slot) {
              const int32_t v = slot < m.slots.size() ? m.slots[slot] : -2;
              if (v < 0 || !texture_list || size_t(v) >= texture_list->size()) return kNoTexture;
              const uint32_t record = (*texture_list)[size_t(v)];
              if (record < impl_->table.textures.size() &&
                  (impl_->table.textures[record].type == fh1::kPvsTextureBix ||
                   impl_->table.textures[record].type == fh1::kPvsTextureCaff))
                return record;
              return kNoTexture;
            };
            mm.albedo = slot_record(0);
            if (mm.effect.rfind("LAKE", 0) == 0) mm.albedo = kNoTexture;  // slot 0 is a cube map there
            // Terrain layers (WORLD-MATERIALS, Terrain shading): pixel
            // constants 0 and 1 are the tilings, 4's x the vertex weight's
            // gain; Blend_B in slot 1, Splat in slot 2.
            auto constant = [&](size_t i) {
              return i < m.pixel_constants.size() ? m.pixel_constants[i] : std::array<float, 4>{1, 1, 1, 1};
            };
            const bool vblend = mm.effect.rfind("H_VBLND", 0) == 0;
            const bool decal = mm.effect.rfind("H_VBLND_DECAL", 0) == 0;
            const bool splat = mm.effect.rfind("H_BLND2", 0) == 0 || mm.effect.rfind("H_BLND3", 0) == 0;
            if (vblend) weight_gain = constant(4)[0];
            if ((vblend && !decal) || splat || mm.effect.rfind("H_BLND1", 0) == 0) mm.tiling = constant(0);
            if (decal) {
              mm.layers = Layers::kVertexFade;
              decal_fade = true;
            }
            if (vblend && !decal) {
              mm.layers = Layers::kVertexBlend;
              mm.layer_b = slot_record(1);
            } else if (splat) {
              mm.layers = Layers::kSplat;
              mm.layer_b = slot_record(1);
              mm.splat = slot_record(2);
              splat_uv = 1;
              splat_tiling = {constant(1)[2], constant(1)[3]};
            }
            auto [it, fresh] = material_index.emplace(mm.key(), uint32_t(out.materials.size()));
            if (fresh) out.materials.push_back(mm);
            mat = it->second;
            layout = fh1::rmb_vertex_layout(mm.effect, o.stride);
            if (mm.effect.ends_with("_LM") || mm.effect.find("_AO") != std::string::npos)
              lightmap = layout.last_texcoord();
            if (splat_uv >= 0) lightmap = -1;  // uv1 carries the Splat UV instead
            if (first_submesh + si < model.uv_offset_scale.size()) uv = model.uv_offset_scale[first_submesh + si];
          } else {
            ++st.no_material;
            MeshMaterial none;
            none.far = far;
            auto [it, fresh] = material_index.emplace(none.key(), uint32_t(out.materials.size()));
            if (fresh) out.materials.push_back(none);
            mat = it->second;
          }
        }
        auto corner = [&](uint32_t i) {
          out.positions.push_back(position(i));
          if (!options.render_data) return;
          const uint8_t* v = &o.vertices[size_t(i) * o.stride];
          std::array<float, 3> n{0, 0, 0};
          if (layout.normal >= 0) {
            const auto f = fh1::rmb_normal(v + layout.normal);
            std::array<double, 3> g{f[0], f[1], f[2]};
            if (placement) {
              const auto& l = placement->linear;
              for (int r = 0; r < 3; ++r) g[size_t(r)] = l[size_t(r)][0] * f[0] + l[size_t(r)][1] * f[1] + l[size_t(r)][2] * f[2];
            }
            const double len = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
            if (len > 0) n = {float(g[0] / len), float(g[1] / len), float(-g[2] / len)};
          }
          out.normals.push_back(n);
          auto texcoord = [&](int k) {
            if (k < 0 || layout.texcoord[size_t(k)] < 0) return std::array<float, 2>{0, 0};
            const auto t = fh1::rmb_texcoord(v + layout.texcoord[size_t(k)]);
            return std::array<float, 2>{uv[0] + uv[2] * t[0], uv[1] + uv[3] * t[1]};
          };
          out.uv0.push_back(texcoord(0));
          if (splat_uv >= 0) {
            const auto s = texcoord(splat_uv);
            out.uv1.push_back({s[0] * splat_tiling[0], s[1] * splat_tiling[1]});
          } else {
            out.uv1.push_back(texcoord(lightmap));
          }
          auto c = layout.colour >= 0 ? fh1::rmb_colour(v + layout.colour)
                                      : std::array<uint8_t, 4>{255, 255, 255, 255};
          if (weight_gain >= 0) {
            // WORLD-MATERIALS, Colour byte order: the layer weight is
            // COLOR.x, the second stored byte (rmb_colour's R); the decal
            // fade is COLOR.w, the first (rmb_colour's A). The decal's
            // noise term is left out (ours).
            const uint8_t byte = decal_fade ? c[3] : c[0];
            const double w = std::clamp((byte / 255.0 - 0.5) * weight_gain + 0.5, 0.0, 1.0);
            c[0] = uint8_t(std::lround(w * 255));
          }
          out.colours.push_back(c);
        };
        size_t added = 0;
        for (size_t t = 0; t + 2 < tri.size(); t += 3) {
          if (far) {
            bool inside = true;
            for (int k = 0; k < 3 && inside; ++k) {
              const auto p = position(tri[t + size_t(k)]);
              inside = std::hypot(p[0] - x, p[2] + z) < inner;  // z is the file frame's here
            }
            if (inside) continue;
            ++st.far_triangles;
          }
          corner(tri[t]);
          corner(tri[t + 1]);
          corner(tri[t + 2]);
          if (options.render_data) out.material.push_back(mat);
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
    if (options.render_data) {
      std::string ignored;
      fh1::parse_render_materials(bytes, model, &ignored);  // without them the model draws flat
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

  // The far ring: MIDDIST and UberLOD objects of the models within
  // far_radius, the finest LOD present per family.
  if (options.render_data && options.far_radius > 0) {
    std::vector<std::pair<uint32_t, fh1::RenderModel>> far_models;
    const double fr = options.far_radius;
    for (uint32_t i = 0; i < models_.size(); ++i) {
      const ModelInfo& m = models_[i];
      if (!m.ok || !m.world_space || !m.far) continue;
      const double dx = std::max({0.0, double(m.box_min[0]) - x, x - double(m.box_max[0])});
      const double dz = std::max({0.0, double(m.box_min[2]) - z, z - double(m.box_max[2])});
      if (dx * dx + dz * dz > fr * fr) continue;
      fh1::RenderModel model;
      std::string e;
      if (!impl_->source.read(m.name, bytes, &e) || !fh1::parse_render_model(bytes, model, &e)) {
        if (error) *error = m.name + ": " + e;
        return false;
      }
      std::string ignored;
      fh1::parse_render_materials(bytes, model, &ignored);
      far_models.emplace_back(i, std::move(model));
    }
    std::map<std::string, int> far_lowest;
    for (const auto& [i, model] : far_models)
      for (const auto& o : model.objects) {
        int lod;
        const std::string family = lod_family(o.name, lod);
        if (lod < 0 || far_level(o.name) == 0) continue;
        auto [it, added] = far_lowest.emplace(family, lod);
        if (!added) it->second = std::min(it->second, lod);
      }
    for (const auto& [i, model] : far_models) add_triangles(model, i, nullptr, &far_lowest, true);
    st.far_models = far_models.size();
  }

  // Tree sprites near the point (WORLD-MATERIALS, Tree billboards): the
  // file records only; the generated records of placed instances are not
  // made (ours, for now). Two triangles per sprite, corners 0 1 2 and 0 2 3.
  if (options.render_data && options.tree_radius > 0 && impl_->have_props) {
    std::map<uint32_t, uint32_t> material_of;  // atlas record -> material
    for (const auto& t : impl_->props.trees) {
      const double dx = t.base[0] - x, dz = t.base[2] + z, d = std::sqrt(dx * dx + dz * dz);  // z: file frame here
      if (d > options.tree_radius || d > t.far) continue;
      auto mit = material_of.find(t.atlas);
      if (mit == material_of.end()) {
        MeshMaterial mm;
        mm.effect = "TREE_BILLBOARD";  // ours: the sprite shader's name is not in the files
        mm.blend = Blend::kCoverage;
        mm.layers = Layers::kBillboard;
        if (t.atlas < impl_->table.textures.size() &&
            (impl_->table.textures[t.atlas].type == fh1::kPvsTextureBix ||
             impl_->table.textures[t.atlas].type == fh1::kPvsTextureCaff))
          mm.albedo = t.atlas;
        auto [it, fresh] = material_index.emplace(mm.key(), uint32_t(out.materials.size()));
        if (fresh) out.materials.push_back(mm);
        mit = material_of.emplace(t.atlas, it->second).first;
      }
      if (out.materials[mit->second].albedo == kNoTexture) {
        ++st.trees_untextured;
        continue;
      }
      double len = std::sqrt(double(t.axis[0]) * t.axis[0] + double(t.axis[1]) * t.axis[1] + double(t.axis[2]) * t.axis[2]);
      const std::array<float, 3> up = len > 0.5 ? std::array<float, 3>{float(t.axis[0] / len), float(t.axis[1] / len),
                                                                       float(t.axis[2] / len)}
                                                : std::array<float, 3>{0, 1, 0};
      const std::array<std::array<float, 2>, 4> offset{{{-t.half_width, 0}, {t.half_width, 0},
                                                        {t.half_width, t.height}, {-t.half_width, t.height}}};
      const std::array<std::array<float, 2>, 4> uv{{{t.u0, t.v1}, {t.u1, t.v1}, {t.u1, t.v0}, {t.u0, t.v0}}};
      for (int c : {0, 1, 2, 0, 2, 3}) {
        out.positions.push_back(t.base);
        out.normals.push_back(up);
        out.uv0.push_back(uv[size_t(c)]);
        out.uv1.push_back(offset[size_t(c)]);
        out.colours.push_back(t.tint);
      }
      for (int k = 0; k < 2; ++k) {
        out.surface.push_back(Surface::kProp);
        out.model.push_back(0);
        out.ground.push_back(0);
        out.material.push_back(mit->second);
      }
      ++st.trees;
    }
  }

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
