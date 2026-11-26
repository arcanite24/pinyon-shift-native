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

bool load_hub_room(const std::filesystem::path& game_root, HubRoom room, const std::filesystem::path& cache_dir,
                   HubRoomScenery& out, std::string* error) {
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
  std::filesystem::create_directories(cache_dir, ec);
  world::World w;
  std::string e;
  if (!w.open(folder, cache_dir / (std::string(track) + ".models"), &e)) {
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

  // The lightmap scales the surface (an interior's light), so it takes the
  // AO slot, which the world shader multiplies in; the room's AO is in it.
  for (auto& m : out.triangles.materials)
    if (m.light_map != kNoWorldTexture) {
      m.ao_map = m.light_map;
      m.light_map = kNoWorldTexture;
    }

  // TrackSettings.xml's light.
  if (std::ifstream ts(folder / "TrackSettings.xml", std::ios::binary); ts) {
    const std::string xml((std::istreambuf_iterator<char>(ts)), std::istreambuf_iterator<char>());
    auto colour = [&](const char* tag, std::array<float, 3>& c) {
      const size_t at = xml.find(std::string("<") + tag + " ");
      if (at == std::string::npos) return;
      const size_t end = xml.find('>', at);
      int k = 0;
      for (const char* a : {"r=\"", "g=\"", "b=\""}) {
        const size_t q = xml.find(a, at);
        if (q != std::string::npos && q < end) c[size_t(k)] = std::strtof(xml.c_str() + q + 3, nullptr);
        ++k;
      }
    };
    colour("LightAmbient", out.ambient);
    colour("LightHemiTop", out.hemi_top);
    colour("LightHemiBottom", out.hemi_bottom);
  }

  // The textures: decoded from the room's own archive, records moved past
  // Colorado's.
  const uint32_t base = hub_room_record_base(room);
  std::set<uint32_t> records, failed;
  for (auto& m : out.triangles.materials)
    for (uint32_t* slot : {&m.texture, &m.layer_b, &m.splat, &m.light_map, &m.normal_map, &m.ao_map, &m.layer_c,
                           &m.spec_map})
      if (*slot != kNoWorldTexture) records.insert(*slot);
  for (uint32_t rec : records) {
    fh1::WorldTexture t;
    if (!w.read_texture(rec, t, &e)) {
      failed.insert(rec);
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
  for (auto& m : out.triangles.materials)
    for (uint32_t* slot : {&m.texture, &m.layer_b, &m.splat, &m.light_map, &m.normal_map, &m.ao_map, &m.layer_c,
                           &m.spec_map})
      if (*slot != kNoWorldTexture) *slot = failed.count(*slot) ? kNoWorldTexture : base + *slot;
  out.textures = records.size() - failed.size();
  out.failed = failed.size();
  return true;
}

}  // namespace carview
