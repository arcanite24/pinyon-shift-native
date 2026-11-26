#include "hub_room.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

#include "drive_helpers_runtime.h"
#include "fh1data/world_texture.h"
#include "fh1data/xds.h"
#include "world/world.h"

namespace carview {

const char* hub_room_track(HubRoom room) {
  switch (room) {
    case HubRoom::kAutoshow: return "uiautoshow";
    case HubRoom::kWorkshop: return "uiworkshop";
    case HubRoom::kPaintShop: return "uipaintshop";
    default: return nullptr;
  }
}

uint32_t hub_room_record_base(HubRoom room) { return kHubRoomRecordBase + (uint32_t(room) << 24); }

float hub_room_yaw(HubRoom) { return 0.0f; }

bool load_hub_room(const std::filesystem::path& game_root, HubRoom room, HubRoomScenery& out, std::string* error) {
  out = HubRoomScenery{};
  out.room = room;
  const char* track = hub_room_track(room);
  if (!track) {
    if (error) *error = "the room has no track";
    return false;
  }
  const std::filesystem::path folder = game_root / "media" / "tracks" / track;
  std::error_code ec;
  if (!std::filesystem::exists(folder / "bin.zip", ec)) {
    if (error) *error = folder.string() + ": no bin.zip";
    return false;
  }
  world::World w;
  std::string e;
  if (!w.open(folder, {}, &e)) {
    if (error) *error = folder.string() + ": " + e;
    return false;
  }
  // Every model of the room, wherever it lies: the room is a few tens of
  // metres across and drawn whole.
  world::GatherOptions opt;
  opt.radius = 500;
  opt.prop_radius = 500;
  opt.props = true;
  opt.local_models = true;
  opt.render_data = true;
  opt.route = -1;
  world::Mesh mesh;
  world::GatherStats stats;
  if (!w.gather(0, 0, opt, mesh, &stats, &e)) {
    if (error) *error = folder.string() + ": " + e;
    return false;
  }
  out.models = stats.world_models;
  out.triangles = scenery(mesh, 0, 0);
  // The floor at (x, z): the highest triangle there below `below` (the
  // rooms' ceilings and gantries are higher).
  auto floor_at = [&](float x, float z, float below) {
    float floor = -1e9f;
    const auto& pos = out.triangles.positions;
    for (size_t t = 0; t + 2 < pos.size(); t += 3) {
      const auto& a = pos[t];
      const auto& b = pos[t + 1];
      const auto& c = pos[t + 2];
      auto side = [&](const std::array<float, 3>& p, const std::array<float, 3>& q) {
        return (q[0] - p[0]) * (z - p[2]) - (q[2] - p[2]) * (x - p[0]);
      };
      const float s0 = side(a, b), s1 = side(b, c), s2 = side(c, a);
      if (!((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))) continue;
      const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
      const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      const float nx = e1[1] * e2[2] - e1[2] * e2[1];
      const float ny = e1[2] * e2[0] - e1[0] * e2[2];
      const float nz = e1[0] * e2[1] - e1[1] * e2[0];
      if (std::fabs(ny) < 1e-6f) continue;
      const float y = (nx * (a[0] - x) + ny * a[1] + nz * (a[2] - z)) / ny;
      if (y < below) floor = std::max(floor, y);
    }
    return floor;
  };
  const float floor = floor_at(0.0f, 0.0f, 1.0f);
  out.floor_y = floor > -1e8f ? floor : 0.0f;

  // TrackSettings.xml's light and post chain.
  if (std::ifstream ts(folder / "TrackSettings.xml", std::ios::binary); ts) {
    const std::string xml((std::istreambuf_iterator<char>(ts)), std::istreambuf_iterator<char>());
    // The attribute `name` of the element `tag`, or `fallback`.
    auto attr = [&](const char* tag, const char* name, float fallback) {
      const size_t at = xml.find(std::string("<") + tag + " ");
      if (at == std::string::npos) return fallback;
      const size_t end = xml.find('>', at);
      const std::string key = std::string(" ") + name + "=\"";
      const size_t q = xml.find(key, at);
      if (q == std::string::npos || q > end) return fallback;
      return std::strtof(xml.c_str() + q + key.size(), nullptr);
    };
    auto colour = [&](const char* tag, std::array<float, 3>& c) {
      c[0] = attr(tag, "r", c[0]);
      c[1] = attr(tag, "g", c[1]);
      c[2] = attr(tag, "b", c[2]);
    };
    colour("LightAmbient", out.ambient);
    colour("LightHemiTop", out.hemi_top);
    colour("LightHemiBottom", out.hemi_bottom);
    if (xml.find("<filmicTone ") != std::string::npos) {
      out.tone = true;
      const char* keys[6] = {"shoulderStrength", "linearStrength", "linearAngle", "toeStrength", "toeNumerator",
                             "toeDenominator"};
      for (int i = 0; i < 6; ++i) out.hable[i] = attr("filmicTone", keys[i], out.hable[i]);
      out.white = attr("filmicTone", "white", out.white);
      out.exposure_ev = attr("filmicTone", "exposure", out.exposure_ev) + attr("ExposureBias", "value", 0.0f);
    }
    out.desaturate = attr("DesaturateAmount", "value", out.desaturate);
    out.luminance[0] = attr("DesatLuminosity", "r", out.luminance[0]);
    out.luminance[1] = attr("DesatLuminosity", "g", out.luminance[1]);
    out.luminance[2] = attr("DesatLuminosity", "b", out.luminance[2]);
    if (attr("OverrideShadowLightDirection", "value", 0.0f) > 0.5f) {
      out.light_travel = {attr("ShadowLightDirection", "x", 0.0f), attr("ShadowLightDirection", "y", -1.0f),
                          attr("ShadowLightDirection", "z", 0.0f)};
    }
    const float direct = attr("imageBasedLighting", "iblDirectScale", 1.0f) *
                         attr("imageBasedLighting", "iblDirectScaleCar", 1.0f);
    out.car_key = {attr("imageBasedLighting", "iblSunColorR", 1.0f) * direct,
                   attr("imageBasedLighting", "iblSunColorG", 1.0f) * direct,
                   attr("imageBasedLighting", "iblSunColorB", 1.0f) * direct};
    out.ibl_ambient_scale = attr("imageBasedLighting", "iblAmbientScale", 1.0f);
    out.ibl_ambient_saturation = attr("imageBasedLighting", "iblAmbientSaturation", 1.0f);
  }

  // The room's static cube (staticcarcubemap.xpr, LogLuv): its mean
  // radiance, the image-based ambient's base.
  if (std::ifstream cf(folder / "staticcarcubemap.xpr", std::ios::binary); cf) {
    const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
    fh1::XprCubemap cube;
    if (fh1::decode_xpr_cubemap(raw, cube, nullptr) && cube.size > 0) {
      double acc[3] = {0, 0, 0};
      size_t n = 0;
      for (const auto& face : cube.faces)
        for (const auto& t : face) {
          for (int k = 0; k < 3; ++k) acc[k] += t[size_t(k)];
          ++n;
        }
      if (n)
        for (int k = 0; k < 3; ++k) out.cube_mean[size_t(k)] = float(acc[k] / double(n));
      out.cube = true;
    }
  }
  {
    const auto& m = out.cube_mean;
    const float lum = 0.2125f * m[0] + 0.7154f * m[1] + 0.0721f * m[2];
    // Calibrated on capture 20 (the paint shop's back wall and floor): the
    // cube's mean is a radiance the shader's ambColor carries at twice.
    constexpr float gain = 2.0f;
    for (size_t k = 0; k < 3; ++k)
      out.ibl_ambient[k] = out.ibl_ambient_scale * gain * (lum + (m[k] - lum) * out.ibl_ambient_saturation);
  }

  // The textures: decoded from the room's own archive, records moved past
  // Colorado's. Empty PVS stub records (type 1/3 with no bundle entry, such as
  // uiautoshow record 21 and uipaintshop's door-glass stub) are resolved cleanly
  // without counting as decode failures.
  const uint32_t base = hub_room_record_base(room);
  std::set<uint32_t> records, failed, stubs;
  for (auto& m : out.triangles.materials)
    for (uint32_t* slot : {&m.texture, &m.layer_b, &m.splat, &m.light_map, &m.normal_map, &m.ao_map, &m.layer_c,
                           &m.spec_map})
      if (*slot != kNoWorldTexture) records.insert(*slot);
  for (uint32_t rec : records) {
    fh1::WorldTexture t;
    if (!w.read_texture(rec, t, &e)) {
      const bool is_stub = rec < w.textures().size() &&
                           (w.textures()[rec].type == fh1::kPvsTextureNone ||
                            w.textures()[rec].type == fh1::kPvsTexturePlaceholder);
      if (is_stub) stubs.insert(rec);
      else failed.insert(rec);
      continue;
    }
    auto d = std::make_shared<WorldTextureData>();
    d->record = base + rec;
    d->srgb = t.format.gamma;
    char name[48];
    std::snprintf(name, sizeof name, "%s:_0x%08x", track, rec < w.textures().size() ? w.textures()[rec].id : rec);
    d->texture.name = name;
    d->texture.format = cars::bake::TextureFormat(uint32_t(t.blocks.format));
    d->texture.swizzle = t.blocks.swizzle;
    for (auto& l : t.blocks.levels) d->texture.levels.push_back({l.width, l.height, std::move(l.blocks)});
    out.triangles.textures.push_back(std::move(d));
  }
  for (auto& m : out.triangles.materials) {
    if (m.light_map != kNoWorldTexture && (stubs.count(m.light_map) || failed.count(m.light_map)) &&
        m.ao_map != kNoWorldTexture && !stubs.count(m.ao_map) && !failed.count(m.ao_map)) {
      m.light_map = m.ao_map;
    }
    for (uint32_t* slot : {&m.texture, &m.layer_b, &m.splat, &m.light_map, &m.normal_map, &m.ao_map, &m.layer_c,
                           &m.spec_map})
      if (*slot != kNoWorldTexture)
        *slot = (failed.count(*slot) || stubs.count(*slot)) ? kNoWorldTexture : base + *slot;
  }
  out.textures = records.size() - failed.size() - stubs.size();
  out.failed = failed.size();
  return true;
}

void apply_hub_room_post(const HubRoomScenery& room, PostSettings& post) {
  if (!room.tone) return;
  for (int i = 0; i < 6; ++i) post.hable[i] = room.hable[i];
  post.white = room.white;
  post.auto_exposure = false;
  post.exposure = std::exp2(room.exposure_ev);
  post.desaturate = room.desaturate;
  for (int i = 0; i < 3; ++i) {
    post.luminance[i] = room.luminance[i];
    post.balance[i] = 1.0f;
  }
  post.lut_enabled = false;
  post.vignette_amount = 0.0f;
  post.paired_vignette = false;
  post.contrast = 1.0f;
  post.brightness = 0.0f;
  post.motion_blur = false;
  post.god_rays = false;
  post.lens_flare = false;
  post.dof = false;  // backdrop_blur sets its own bit (renderer_post.cpp)
}

}  // namespace carview
