#include "world/world.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

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
// WORLD-MATERIALS, Placeholder models: the art tools' missing-material texture.
constexpr uint32_t kPlaceholderTextureId = 0x00002CA9u;

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
  // The `.bundle` members' entries by texture record (WORLD-MATERIALS,
  // Small textures), read on the first bundle-only record. A record in
  // several bundles has the same payload in all: the first is kept.
  struct Small {
    uint32_t width = 0, height = 0, levels = 0, format_word = 0;
    std::vector<uint8_t> payload;
  };
  std::mutex small_mutex;
  bool small_read = false;
  std::unordered_map<uint32_t, Small> small;
  void read_small() {
    if (small_read) return;
    small_read = true;
    const auto start = std::chrono::steady_clock::now();
    std::vector<uint8_t> data;
    std::vector<fh1::BundleEntry> entries;
    for (const auto& name : source.with_suffix(".bundle")) {
      if (!source.read(name, data, nullptr) || !fh1::parse_bundle(data, entries, nullptr)) continue;
      for (const auto& e : entries)
        if (!small.count(e.record))
          small[e.record] = {e.width, e.height, e.levels, e.format_word, {e.payload.begin(), e.payload.end()}};
    }
    if (std::getenv("SHIFT_WORLD_DEBUG"))
      std::fprintf(stderr, "world: %zu bundle textures in %.0f ms\n", small.size(),
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }
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
    // Types 1 and 3: the bundle entry is the only copy.
    std::lock_guard<std::mutex> lock(impl_->small_mutex);
    impl_->read_small();
    const auto it = impl_->small.find(record);
    if (it == impl_->small.end()) {
      if (error) *error = "texture record " + std::to_string(record) + " is in no bundle";
      return false;
    }
    const auto& s = it->second;
    return fh1::decode_bundle_entry({record, s.width, s.height, s.levels, s.format_word, s.payload}, out, error);
  }
  if (ok && !exact) {
    if (error) *error = std::string(name) + ": sizes do not fit the layout";
    return false;
  }
  return ok;
}

bool decal_effect(const std::string& e) {
  if (e.find("DECAL") != std::string::npos) return true;
  return effect_blend(e) == Blend::kAlpha && e.rfind("LAKE", 0) != 0;
}

Blend effect_blend(const std::string& e) {
  auto starts = [&](const char* prefix) { return e.rfind(prefix, 0) == 0; };
  if (starts("DIFF_GLOW")) return Blend::kAdditive;  // interim (ours), see Blend
  if ((starts("H_ROAD") && e.find("_BLEND_") != std::string::npos) || starts("H_VBLND_DECAL") || starts("LAKE"))
    return Blend::kAlpha;
  // Props and trees drew with alpha to coverage in the frame; other effects
  // named _OPAC (vertex-blend cut-outs) are given it too (ours).
  if (starts("H_DIFF") || starts("TREE_") || e.find("_OPAC") != std::string::npos) return Blend::kCoverage;
  return Blend::kOpaque;
}

fs::path ribbon_file(const fs::path& world, const std::string& suffix) {
  auto lower = [](std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const fs::path ribbon = world / "Ribbon_00";
  const std::string name = (world.has_filename() ? world : world.parent_path()).filename().string();
  const std::string want = lower(name + suffix);
  std::error_code ec;
  for (const auto& f : fs::directory_iterator(ribbon, ec))
    if (lower(f.path().filename().string()) == want) return f.path();
  return ribbon / (name + suffix);
}

std::string model_prefix(const fs::path& world) {
  std::string s = (world.has_filename() ? world : world.parent_path()).filename().string();
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s + "out";
}

bool World::open(const fs::path& colorado, const fs::path& cache, std::string* error) {
  name_ = (colorado.has_filename() ? colorado : colorado.parent_path()).filename().string();
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
  const fs::path pvs = ribbon_file(colorado, "_00.pvs");
  std::ifstream f(pvs, std::ios::binary);
  if (f) {
    std::vector<uint8_t> pvs_bytes((std::istreambuf_iterator<char>(f)), {});
    fh1::PropLoadOptions options;
    options.census_first_rule = false;
    std::string e;
    if (fh1::parse_object_table(pvs_bytes, impl_->table, &e) &&
        fh1::load_props(impl_->source, impl_->props, &e, options)) {
      impl_->prop_models = std::make_unique<fh1::PropModels>(impl_->source, model_prefix(colorado));
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
                           const std::map<std::string, int>* lowest, bool far_pass = false, uint8_t fade = 0) {
    // The model's texture list (WORLD-MATERIALS): list j for member j + 1.
    const std::vector<uint32_t>* texture_list = nullptr;
    if (options.render_data && model_index < models_.size()) {
      const std::string& name = models_[model_index].name;
      const size_t dot = name.find('.');  // <prefix>.NNNNN.rmb.bin
      const unsigned long member = dot != std::string::npos ? std::strtoul(name.c_str() + dot + 1, nullptr, 10) : 0;
      if (member >= 1 && member - 1 < impl_->table.model_textures.size())
        texture_list = &impl_->table.model_textures[member - 1];
      if (model.uv_offset_scale.empty()) st.default_uv_models.insert(name);
    }
    size_t submesh_number = 0;  // in file order, for uvOffsetScale
    std::array<float, 3> instance{0, 0, 0};
    if (placement) {
      const auto q = placement->apply({0, 0, 0});
      instance = {float(q[0]), float(q[1]), float(-q[2])};
    }
    for (const auto& o : model.objects) {
      const size_t first_submesh = submesh_number;
      submesh_number += o.submeshes.size();
      const uint8_t far = far_pass ? far_level(o.name) : 0;
      if (far_pass ? far == 0 : !placement && skipped_object(o.name)) {
        if (!far_pass) ++st.skipped_far;
        continue;
      }
      // The far ring leaves out triangles wholly near the point.
      const double inner = far == 2 ? options.far_inner_uberlod : options.far_inner;
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
            mm.fade = fade;
            mm.effect = model.effects[m.effect];
            mm.blend = effect_blend(mm.effect);
            // A slot's texture record: a file (types 4 and 0) or a bundle
            // entry (types 1 and 3; WORLD-MATERIALS, Small textures).
            auto slot_record = [&](size_t slot) {
              const int32_t v = slot < m.slots.size() ? m.slots[slot] : -2;
              if (v < 0 || !texture_list || size_t(v) >= texture_list->size()) return kNoTexture;
              const uint32_t record = (*texture_list)[size_t(v)];
              return record < impl_->table.textures.size() ? record : kNoTexture;
            };
            mm.albedo = slot_record(0);
            // WORLD-MATERIALS, Placeholder models: slot 0 on the art tools'
            // missing-material marker (_0x00002CA9) is never drawn, in any
            // pass (shadow stand-ins too, interim).
            if (mm.albedo != kNoTexture && impl_->table.textures[mm.albedo].id == kPlaceholderTextureId) continue;
            for (size_t k = 0; k < 3; ++k) {
              const int32_t v = k < m.slots.size() ? m.slots[k] : -2;
              mm.slot_values[k] = v;
              if (v >= 0 && texture_list && size_t(v) < texture_list->size() &&
                  (*texture_list)[size_t(v)] < impl_->table.textures.size())
                mm.slot_types[k] = int32_t(impl_->table.textures[(*texture_list)[size_t(v)]].type);
            }
            if (mm.effect.rfind("LAKE", 0) == 0) {
              mm.albedo = kNoTexture;  // slot 0 is a cube map there
              mm.is_water = 1;
              mm.normal_map = slot_record(1);
            }
            // Helper: file-backed texture record only (skips all-black Light_Map / all-white AO type-1 stubs).
            auto file_slot = [&](size_t slot) -> uint32_t {
              const uint32_t rec = slot_record(slot);
              if (rec == kNoTexture) return kNoTexture;
              const uint8_t ty = impl_->table.textures[rec].type;
              return (ty == fh1::kPvsTextureBix || ty == fh1::kPvsTextureCaff) ? rec : kNoTexture;
            };
            // Terrain layers (WORLD-MATERIALS, Terrain shading): pixel
            // constants 0 and 1 are the tilings, 4's x the vertex weight's
            // gain; Blend_B in slot 1, Splat in slot 2, Blend_C in slot 5 (H_BLND3).
            auto constant = [&](size_t i) {
              return i < m.pixel_constants.size() ? m.pixel_constants[i] : std::array<float, 4>{1, 1, 1, 1};
            };
            const bool vblend = mm.effect.rfind("H_VBLND", 0) == 0;
            const bool decal = mm.effect.rfind("H_VBLND_DECAL", 0) == 0;
            const bool blnd3 = mm.effect.rfind("H_BLND3", 0) == 0;
            const bool splat = mm.effect.rfind("H_BLND2", 0) == 0 || blnd3;
            const bool road = mm.effect.rfind("H_ROAD", 0) == 0;
            const bool prop_diff = mm.effect.rfind("H_DIFF", 0) == 0;
            const bool has_norm = mm.effect.find("_NORM") != std::string::npos;
            const bool has_ao = mm.effect.find("_AO") != std::string::npos;
            const bool has_lm = mm.effect.ends_with("_LM");
            const bool has_spec = mm.effect.find("_SPEC") != std::string::npos;
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
              if (blnd3) {
                mm.layer_c = file_slot(5);
                mm.tiling_c = {constant(1)[0], constant(1)[1]};
              }
            }
            if (has_norm && !mm.is_water) {
              mm.normal_map = prop_diff ? file_slot(1) : file_slot(3);
            }
            if (has_ao) {
              if (road && mm.effect.find("DIFF3") != std::string::npos &&
                  mm.effect.find("MODULATE") != std::string::npos) {
                mm.ao_map = file_slot(4);
              } else if (blnd3) {
                mm.ao_map = file_slot(6);
              } else {
                mm.ao_map = file_slot(5);
              }
            }
            if (has_lm) {
              mm.light_map = file_slot(7);
              if (m.pixel_constants.size() > 3) mm.emissive_force = std::clamp(m.pixel_constants[3][3], 0.0f, 1.0f);
            }
            if (has_spec && prop_diff) {
              mm.spec_map = file_slot(has_norm ? 2 : 1);
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
            none.fade = fade;
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
          if (options.render_data) out.material.push_back(mat), out.instance.push_back(instance);
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
      if (options.route >= 0 && !fh1::shown_on_route(t.routes, options.route)) {
        ++st.trees_off_route;
        continue;
      }
      auto mit = material_of.find(t.atlas);
      if (mit == material_of.end()) {
        MeshMaterial mm;
        mm.effect = "TREE_BILLBOARD";  // ours: the sprite shader's name is not in the files
        mm.blend = Blend::kCoverage;
        mm.layers = Layers::kBillboard;
        if (t.atlas < impl_->table.textures.size()) mm.albedo = t.atlas;  // a file or a bundle entry
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
        out.instance.push_back(t.base);
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
      if (options.route >= 0 && !fh1::shown_on_route(p.routes, options.route)) {
        ++st.props_off_route;
        continue;
      }
      if (options.hidden && !p.name.empty() && options.hidden->count(p.name)) {
        ++st.props_hidden;
        continue;
      }
      if (!p.has_object || p.object_id >= impl_->table.objects.size()) {
        ++st.prop_unresolved;
        continue;
      }
      const uint32_t m = impl_->table.objects[p.object_id].member;
      // The placeholder marker of activity nodes is never drawn (WORLD-
      // GEOMETRY: a guess from the one marker in view).
      std::string prop_family_low;
      {
        std::string family;
        int lod = -1;
        if (impl_->prop_models->first_name(m, family, lod)) {
          for (char& ch : family) ch = char(std::tolower(static_cast<unsigned char>(ch)));
          prop_family_low = family;
          if (family.rfind("temp_arrow_pointer", 0) == 0) {
            ++st.props_markers;
            continue;
          }
        }
      }
      const bool fading = options.render_data && options.prop_near_radius > 0;
      const bool resolvable = impl_->prop_models->model(m) != nullptr;
      // The levels drawn here, with their fade classes.
      struct Level {
        int64_t member = -1;
        uint8_t fade = 0;
      } levels[2];
      if (resolvable && fading) {
        const int64_t far_member = impl_->prop_models->lod_member(m, 2);
        const int64_t near_member = impl_->prop_models->lod_member(m, 0);
        if (near_member == far_member) {
          levels[0] = {far_member, uint8_t(3)};
        } else {
          levels[0] = {far_member, uint8_t(2)};
          if (d2 < options.prop_near_radius * options.prop_near_radius) levels[1] = {near_member, uint8_t(1)};
        }
      } else if (resolvable) {
        levels[0] = {impl_->prop_models->lod_member(m, d2 < options.prop_lod0 * options.prop_lod0 ? 0 : 2), uint8_t(0)};
      }
      // The gameplay beacon column (WORLD-MATERIALS item 7: coloradoout.06509,
      // the only DIFF_GLOW_1 model, 105 placements) never drew in the frames
      // checked, day or night: interim rule, not drawn.
      if (impl_->prop_models->name_of(m) == "coloradoout.06509.rmb.bin") {
        ++st.props_beacons;
        continue;
      }
      bool drawn = false, beacon = false;
      for (const auto& [member, fade] : levels) {
        const fh1::RenderModel* model = member >= 0 ? impl_->prop_models->model(uint32_t(member)) : nullptr;
        if (!model) continue;
        if (!once.insert({member, std::llround(t[0] * 100), std::llround(t[1] * 100), std::llround(t[2] * 100)})
                 .second) {
          drawn = true;  // the same spot's other LOD object
          continue;
        }
        if (impl_->prop_models->name_of(uint32_t(member)) == "coloradoout.06509.rmb.bin") {
          beacon = true;
          continue;
        }
        const auto it = index_of.find(impl_->prop_models->name_of(uint32_t(member)));
        add_triangles(*model, it != index_of.end() ? it->second : 0, &p.placement, nullptr, false, fade);
        // Emit night streetlamp / festival / outpost glow sprites once per prop instance.
        if (options.render_data && fade != 1 && !prop_family_low.empty()) {
          const bool is_lamp = prop_family_low.find("lamp") != std::string::npos ||
                               prop_family_low.find("street") != std::string::npos ||
                               prop_family_low.find("light") != std::string::npos ||
                               prop_family_low.find("lantern") != std::string::npos;
          const bool is_fest = prop_family_low.find("festival") != std::string::npos ||
                               prop_family_low.find("stage") != std::string::npos ||
                               prop_family_low.find("tower") != std::string::npos ||
                               prop_family_low.find("outpost") != std::string::npos ||
                               prop_family_low.find("gantry") != std::string::npos ||
                               prop_family_low.find("booth") != std::string::npos;
          if (is_lamp || is_fest) {
            const float local_top = std::max(model->box_max[1] * 0.92f, 2.5f);
            const auto gp = p.placement.apply({0.0f, local_top, 0.0f});
            WorldGlow wg;
            wg.position = {float(gp[0]), float(gp[1]), float(-gp[2])};
            if (is_fest && !is_lamp) {
              // Festival / outpost warm-magenta / gold stage glow.
              wg.color = {1.0f, 0.58f, 0.82f};
              wg.radius = 2.6f;
            } else {
              // Streetlamp warm sodium-amber glow.
              wg.color = {1.0f, 0.78f, 0.42f};
              wg.radius = 1.8f;
            }
            // Deterministic per-instance threshold in [0.05, 0.90] for staggered dusk switch-on.
            const uint32_t h = uint32_t(std::llround(t[0] * 7.0)) * 2654435761u ^
                               uint32_t(std::llround(t[2] * 7.0)) * 2246822519u;
            wg.threshold = 0.05f + 0.85f * float(h & 0xFFFFu) / 65535.0f;
            out.world_glows.push_back(wg);
          }
        }
        drawn = true;
        if (fade != 1) ++st.props;
        else ++st.props_near_lod;
      }
      if (beacon) ++st.props_beacons;
      else if (!drawn) ++st.prop_unresolved;
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
