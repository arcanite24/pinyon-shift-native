#include "car_model.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <unordered_map>

#include "fh1data/archive.h"
#include "fh1data/carbin.h"

namespace carview {
namespace {

std::filesystem::path find_media(const std::filesystem::path& root) {
  for (auto candidate : {root / "media", root / "base" / "media", root}) {
    if (std::filesystem::is_directory(candidate / "cars")) return candidate;
  }
  return root / "media";
}

MaterialKind classify(const std::string& material) {
  std::string m = material;
  for (char& c : m) c = char(std::tolower(static_cast<unsigned char>(c)));
  // `body` is the car-paint technique: atlas alpha 0 shows the paint.
  if (m == "body" || m.rfind("body_", 0) == 0) return MaterialKind::kPaint;
  if (m.find("window") != std::string::npos || m.find("glass") != std::string::npos)
    return MaterialKind::kGlass;
  return MaterialKind::kTextured;
}

// Parts left out unless --all-parts: upgrade variants ("...race") that
// overlap the stock part, and the wheel, whose four positions come from the
// car's physics data rather than the model.
bool hidden_by_default(const std::string& name) {
  std::string n = name;
  for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
  const bool race = n.size() > 4 && n.compare(n.size() - 4, 4, "race") == 0;
  return race || n == "wheel";
}

}  // namespace

bool load_car(const std::filesystem::path& game_root, const std::string& car, int lod,
              const LoadOptions& options, CarModel& out, std::string& error) {
  out = CarModel{};
  const auto media = find_media(game_root);
  const auto zip = media / "cars" / (car + ".zip");
  auto archive = fh1::Archive::open(zip, &error);
  if (!archive) {
    error = zip.string() + ": " + error;
    return false;
  }
  out.model_member = lod == 0 ? car + "_lod0.carbin" : car + ".carbin";
  out.texture_member = lod == 0 ? "nodamage_LOD0.xds" : "nodamage.xds";
  // The close model is split: the hood, headlights, windscreen and cabin
  // are in `_cockpit`, which is drawn with it.
  std::vector<std::string> members = {out.model_member};
  if (lod == 0 && options.cockpit) members.push_back(car + "_cockpit.carbin");
  std::vector<fh1::Carbin> carbins;
  for (const std::string& member : members) {
    auto bytes = archive->read(member);
    if (!bytes.ok()) {
      if (member != out.model_member) continue;  // a car without a cockpit model
      error = member + ": " + bytes.error;
      return false;
    }
    fh1::Carbin carbin;
    if (!fh1::parse_carbin(bytes.data, carbin, &error)) {
      error = member + ": " + error;
      return false;
    }
    carbins.push_back(std::move(carbin));
  }
  if (carbins[0].parts.empty()) {
    error = out.model_member + " has no parts (try --lod 1)";
    return false;
  }

  auto texture_bytes = archive->read(out.texture_member);
  if (!texture_bytes.ok()) {
    error = out.texture_member + ": " + texture_bytes.error;
    return false;
  }
  fh1::XdsResult texture = fh1::decode_xds(texture_bytes.data, true);
  if (texture.levels.empty()) {
    error = out.texture_member + ": " + texture.error;
    return false;
  }
  out.texture_levels = std::move(texture.levels);

  out.bounds_min = {FLT_MAX, FLT_MAX, FLT_MAX};
  out.bounds_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
  std::vector<uint32_t> remap;
  for (const fh1::Carbin& carbin : carbins)
  for (const fh1::CarbinPart& part : carbin.parts) {
    if (!options.all_parts && hidden_by_default(part.name)) {
      ++out.hidden_parts;
      continue;
    }
    ++out.parts;
    for (const fh1::CarbinSubsection& s : part.subsections) {
      // One LOD: 0 is the LOD0 pool; the LOD model's sub-sections name 1-5.
      if (s.lod != lod) continue;
      const fh1::CarbinPool& pool = s.lod == 0 ? part.lod0_pool : part.lod_pool;
      if (pool.vertices.empty()) {
        ++out.skipped_subsections;
        continue;
      }
      ++out.subsections;
      const MaterialKind kind = classify(s.material);
      // Each sub-section has its own UV transform, so its vertices are copied.
      remap.assign(pool.vertices.size(), UINT32_MAX);
      for (uint32_t index : fh1::carbin_triangle_list(s)) {
        if (index >= pool.vertices.size()) continue;  // never seen in the corpus
        uint32_t& mapped = remap[index];
        if (mapped == UINT32_MAX) {
          const fh1::CarbinVertex& v = pool.vertices[index];
          Vertex out_v{};
          const auto p = fh1::carbin_position(part, v);
          const auto n = fh1::carbin_normal(v);
          const auto uv = fh1::carbin_uv(s, v, 0);
          for (int a = 0; a < 3; ++a) {
            out_v.position[a] = p[a];
            out_v.normal[a] = n[a];
            out.bounds_min[a] = std::min(out.bounds_min[a], p[a]);
            out.bounds_max[a] = std::max(out.bounds_max[a], p[a]);
          }
          out_v.uv[0] = uv[0];
          out_v.uv[1] = uv[1];
          out_v.occlusion = std::min(1.0f, v.lighting[0] / 100.0f);
          out_v.kind = float(uint32_t(kind));
          mapped = uint32_t(out.vertices.size());
          out.vertices.push_back(out_v);
        }
        out.indices.push_back(mapped);
      }
    }
  }
  if (out.indices.empty()) {
    error = out.model_member + ": no triangles at LOD " + std::to_string(lod);
    return false;
  }
  return true;
}

}  // namespace carview
