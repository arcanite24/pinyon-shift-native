#include "audio/world_acoustics.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>

#include "audio/bake.h"
#include "audio/fev.h"
#include "audio/fsb4.h"
#include "audio/stream.h"
#include "audio/xma_decode.h"
#include "audio/xml.h"
#include "cars/mods.h"
#include "fh1data/archive.h"

namespace audio {
namespace {

std::string read_text(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string lower_str(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Barycentric point-in-triangle test in XZ; returns true if inside and writes
// barycentric weights (w0, w1, w2).
bool barycentric_xz(float px, float pz, const std::array<float, 3>& tx, const std::array<float, 3>& tz, float& w0,
                    float& w1, float& w2) {
  const float v0x = tx[1] - tx[0], v0z = tz[1] - tz[0];
  const float v1x = tx[2] - tx[0], v1z = tz[2] - tz[0];
  const float v2x = px - tx[0], v2z = pz - tz[0];
  const float denom = v0x * v1z - v1x * v0z;
  if (std::fabs(denom) < 1e-6f) return false;
  const float inv = 1.0f / denom;
  w1 = (v2x * v1z - v1x * v2z) * inv;
  w2 = (v0x * v2z - v2x * v0z) * inv;
  w0 = 1.0f - w1 - w2;
  constexpr float kEps = -1e-4f;
  return w0 >= kEps && w1 >= kEps && w2 >= kEps;
}

ReverbTemplate lerp_reverb(const ReverbTemplate& a, const ReverbTemplate& b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  auto L = [t](float u, float v) { return u + (v - u) * t; };
  ReverbTemplate r = t >= 0.5f ? b : a;
  r.decay_time_s = L(a.decay_time_s, b.decay_time_s);
  r.decay_hf_ratio = L(a.decay_hf_ratio, b.decay_hf_ratio);
  r.density_pct = L(a.density_pct, b.density_pct);
  r.diffusion_pct = L(a.diffusion_pct, b.diffusion_pct);
  r.dry_level_mb = L(a.dry_level_mb, b.dry_level_mb);
  r.room_level_mb = L(a.room_level_mb, b.room_level_mb);
  r.room_hf_level_mb = L(a.room_hf_level_mb, b.room_hf_level_mb);
  r.reflections_level_mb = L(a.reflections_level_mb, b.reflections_level_mb);
  r.reflections_delay_s = L(a.reflections_delay_s, b.reflections_delay_s);
  r.reverb_level_mb = L(a.reverb_level_mb, b.reverb_level_mb);
  r.reverb_delay_s = L(a.reverb_delay_s, b.reverb_delay_s);
  r.hf_reference_hz = L(a.hf_reference_hz, b.hf_reference_hz);
  r.near_dist = L(a.near_dist, b.near_dist);
  r.far_dist = L(a.far_dist, b.far_dist);
  r.near_dry = L(a.near_dry, b.near_dry);
  r.near_wet = L(a.near_wet, b.near_wet);
  r.far_dry = L(a.far_dry, b.far_dry);
  r.far_wet = L(a.far_wet, b.far_wet);
  return r;
}

float dist_point_to_segment(float px, float py, float pz, const std::array<float, 3>& a,
                            const std::array<float, 3>& b, std::array<float, 3>* closest_delta = nullptr) {
  const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
  const float len2 = abx * abx + aby * aby + abz * abz;
  float t = 0.0f;
  if (len2 > 1e-4f) {
    t = std::clamp(((px - a[0]) * abx + (py - a[1]) * aby + (pz - a[2]) * abz) / len2, 0.0f, 1.0f);
  }
  const float cx = a[0] + abx * t - px;
  const float cy = a[1] + aby * t - py;
  const float cz = a[2] + abz * t - pz;
  if (closest_delta) *closest_delta = {cx, cy, cz};
  return std::sqrt(cx * cx + cy * cy + cz * cz);
}

uint32_t hash_emitter(std::string_view a, std::string_view b, size_t idx) {
  uint32_t h = 2166136261u ^ uint32_t(idx * 131u);
  for (char c : a) h = (h ^ uint8_t(c)) * 16777619u;
  for (char c : b) h = (h ^ uint8_t(c)) * 16777619u;
  return h == 0u ? 1u : h;
}

void parse_track3d_node(const XmlNode& track3d, std::vector<AmbientEmitter3D>& out) {
  for (const auto& obj : track3d.children) {
    if (!iequals(obj.name, "Object")) continue;
    AmbientEmitter3D e;
    e.object_name = obj.attr("objectName");
    e.event_path = obj.attr("eventPath");
    e.event_name = obj.attr("eventName");
    if (const XmlNode* p1 = obj.child("Position1")) {
      e.pos1 = {float(p1->number("x")), float(p1->number("y")), float(p1->number("z"))};
      e.pos2 = e.pos1;
    }
    if (const XmlNode* p2 = obj.child("Position2")) {
      e.pos2 = {float(p2->number("x")), float(p2->number("y")), float(p2->number("z"))};
    }
    const std::string low = lower_str(e.event_name + " " + e.object_name);
    if (low.find("festival") != std::string::npos || low.find("stage") != std::string::npos) {
      e.max_distance = 380.0f;
    } else if (low.find("crowd") != std::string::npos) {
      e.max_distance = 220.0f;
    } else {
      e.max_distance = 120.0f;
    }
    out.push_back(std::move(e));
  }
}

}  // namespace

uint32_t ambience_event_hash(std::string_view event_name) {
  uint32_t h = 2166136261u;
  for (char c : event_name) h = (h ^ uint8_t(std::tolower(static_cast<unsigned char>(c)))) * 16777619u;
  return h == 0u ? 1u : h;
}

int8_t AmbienceParams::id_of(std::string_view name) {
  auto is = [&](std::string_view n) {
    if (n.size() != name.size()) return false;
    for (size_t i = 0; i < n.size(); ++i)
      if (std::tolower(static_cast<unsigned char>(n[i])) != std::tolower(static_cast<unsigned char>(name[i])))
        return false;
    return true;
  };
  if (is("(distance)")) return kDistance;
  if (is("CurrentTime")) return 0;
  if (is("CarSpeed")) return 1;
  if (is("Speed")) return 2;
  if (is("InRace")) return 3;
  if (is("PreRaceAmbience")) return 4;
  if (is("CameraType")) return 5;
  if (is("RadioIsPlaying")) return 6;
  if (is("Engine_RPM")) return 7;
  if (is("CrowdExcitement")) return 8;
  return kNone;
}

float AmbienceParams::value(int8_t id) const {
  switch (id) {
    case 0: return current_time_s;
    case 1: return car_speed_mph;
    case 2: return speed;
    case 3: return in_race;
    case 4: return pre_race_ambience;
    case 5: return camera_type;
    case 6: return radio_is_playing;
    case 7: return engine_rpm;
    case 8: return crowd_excitement;
    default: return std::numeric_limits<float>::quiet_NaN();
  }
}

bool parse_reverb_xml(std::string_view xml, std::vector<ReverbTemplate>& out, std::string* error) {
  out.clear();
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  const XmlNode* list = root.child("ReverbTemplates");
  if (!list) list = &root;
  for (const auto& c : list->children) {
    if (!iequals(c.name, "Template")) continue;
    ReverbTemplate t;
    t.name = c.attr("Name");
    t.snapshot_mix = c.attr("SnapshotMix");
    if (t.snapshot_mix.empty()) t.snapshot_mix = "NotInTunnel";
    t.decay_time_s = float(c.number("DecayTimeSec", 1.2));
    t.decay_hf_ratio = float(c.number("DecayHFRatio", 1.0));
    t.density_pct = float(c.number("DensityPercent", 100.0));
    t.diffusion_pct = float(c.number("DiffusionPercent", 100.0));
    t.dry_level_mb = float(c.number("DryLevelmB", 0.0));
    t.room_level_mb = float(c.number("RoomLevelmB", -800.0));
    t.room_hf_level_mb = float(c.number("RoomHFLevelmB", -1500.0));
    t.reflections_level_mb = float(c.number("ReflectionsLevelmB", -3000.0));
    t.reflections_delay_s = float(c.number("ReflectionsDelaySec", 0.0));
    t.reverb_level_mb = float(c.number("ReverbLevelmB", 0.0));
    t.reverb_delay_s = float(c.number("ReverbDelaySec", 0.0));
    t.hf_reference_hz = float(c.number("HFReferenceHz", 6260.0));
    t.near_dist = float(c.number("NearDist", 10.0));
    t.far_dist = float(c.number("FarDist", 60.0));
    t.near_dry = float(c.number("NearDry", 1.0));
    t.near_wet = float(c.number("NearWet", 0.0));
    t.far_dry = float(c.number("FarDry", 0.8));
    t.far_wet = float(c.number("FarWet", 0.8));
    out.push_back(std::move(t));
  }
  return !out.empty();
}

bool parse_ambience_xml(std::string_view xml, std::vector<AmbienceTemplate>& out, std::string* error) {
  out.clear();
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  const XmlNode* list = root.child("AmbienceTemplates");
  if (!list) list = &root;
  for (const auto& c : list->children) {
    if (!iequals(c.name, "Template")) continue;
    AmbienceTemplate t;
    t.name = c.attr("Name");
    t.snapshot_mix = c.attr("SnapshotMix");
    if (t.snapshot_mix.empty()) t.snapshot_mix = "DefaultLocation";
    t.background_stream_name = c.attr("BackgroundStreamName");
    t.background_stream_path = c.attr("BackgroundStreamPath");
    t.start_below_speed_mph = float(c.number("BackgroundStreamStartBelowSpeed", 35.0));
    t.stop_above_speed_mph = float(c.number("BackgroundStreamStopAboveSpeed", 40.0));
    t.crowd_event_name = c.attr("CrowdEventName");
    t.tree_event_name = c.attr("TreeEventName");
    out.push_back(std::move(t));
  }
  return !out.empty();
}

bool parse_ribbon_audio_xml(std::string_view xml, std::vector<AmbientEmitter3D>& out, std::string* error) {
  out.clear();
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  if (const XmlNode* t3d = root.child("Track3D")) parse_track3d_node(*t3d, out);
  return !out.empty();
}

bool parse_soundscape_xml(std::string_view xml, SoundscapeTile& out, std::string* error) {
  out = SoundscapeTile{};
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;

  if (const XmlNode* ta = root.child("TrackAudio")) {
    if (const XmlNode* t3d = ta->child("Track3D")) parse_track3d_node(*t3d, out.emitters);
  }
  if (const XmlNode* rzs = root.child("ReverbZones")) {
    for (const auto& rz : rzs->children) {
      if (!iequals(rz.name, "ReverbZone")) continue;
      ReverbZone zone;
      zone.template0 = int(rz.number("Template0", 0));
      zone.template1 = int(rz.number("Template1", zone.template0));
      zone.min_height = float(rz.number("MinHeight", -1e6));
      zone.max_height = float(rz.number("MaxHeight", 1e6));
      bool first = true;
      for (const auto& tri : rz.children) {
        if (!iequals(tri.name, "ReverbTriangle")) continue;
        ReverbTriangle rt;
        int idx = 0;
        for (const auto& pt : tri.children) {
          if (!iequals(pt.name, "ReverbTrianglePoint") || idx >= 3) continue;
          const float px = float(pt.number("positionX"));
          const float pz = float(pt.number("positionZ"));
          const float val = float(pt.number("value"));
          rt.x[size_t(idx)] = px;
          rt.z[size_t(idx)] = pz;
          rt.value[size_t(idx)] = val;
          if (first) {
            zone.min_x = zone.max_x = px;
            zone.min_z = zone.max_z = pz;
            first = false;
          } else {
            zone.min_x = std::min(zone.min_x, px);
            zone.max_x = std::max(zone.max_x, px);
            zone.min_z = std::min(zone.min_z, pz);
            zone.max_z = std::max(zone.max_z, pz);
          }
          ++idx;
        }
        if (idx == 3) zone.triangles.push_back(rt);
      }
      if (!zone.triangles.empty()) out.reverb_zones.push_back(std::move(zone));
    }
  }
  if (const XmlNode* azs = root.child("AmbienceZones")) {
    for (const auto& az : azs->children) {
      if (!iequals(az.name, "AmbienceZone")) continue;
      AmbienceZone zone;
      zone.template_idx = int(az.number("Template", 0));
      zone.min_height = float(az.number("MinHeight", -1e6));
      zone.max_height = float(az.number("MaxHeight", 1e6));
      bool first = true;
      for (const auto& tri : az.children) {
        if (!iequals(tri.name, "AmbienceTriangle")) continue;
        AmbienceTriangle at;
        int idx = 0;
        for (const auto& pt : tri.children) {
          if (!iequals(pt.name, "AmbienceTrianglePoint") || idx >= 3) continue;
          const float px = float(pt.number("positionX"));
          const float pz = float(pt.number("positionZ"));
          at.x[size_t(idx)] = px;
          at.z[size_t(idx)] = pz;
          if (first) {
            zone.min_x = zone.max_x = px;
            zone.min_z = zone.max_z = pz;
            first = false;
          } else {
            zone.min_x = std::min(zone.min_x, px);
            zone.max_x = std::max(zone.max_x, px);
            zone.min_z = std::min(zone.min_z, pz);
            zone.max_z = std::max(zone.max_z, pz);
          }
          ++idx;
        }
        if (idx == 3) zone.triangles.push_back(at);
      }
      if (!zone.triangles.empty()) out.ambience_zones.push_back(std::move(zone));
    }
  }
  if (const XmlNode* trees = root.child("Trees")) out.tree_count = int(trees->children.size());
  if (const XmlNode* crowds = root.child("Crowds")) out.crowd_count = int(crowds->children.size());
  return true;
}

bool WorldAcousticsData::load_configs(const std::filesystem::path& game_root, std::string* note) {
  if (game_root.empty()) return false;
  std::error_code ec;
  const std::string low_root = lower_str(game_root.string());
  const bool want_rally = rally_world_ || low_root.find("coloradodirt") != std::string::npos ||
                          low_root.find("1600_pri_65") != std::string::npos;
  if (want_rally) {
    if (load_rally_configs(game_root, note)) return true;
  }

  const auto rev_path = cars::mods::resolve(game_root / "media" / "tracks" / "colorado" / "colorado_reverb.xml");
  const auto amb_path = cars::mods::resolve(game_root / "media" / "tracks" / "colorado" / "colorado_ambience.xml");
  const auto rib_path =
      cars::mods::resolve(game_root / "media" / "audio" / "tracks" / "colorado" / "Ribbon_00" / "ColoradoRibbon_00.xml");

  bool ok = false;
  if (std::filesystem::exists(rev_path, ec)) ok |= parse_reverb_xml(read_text(rev_path), reverb_templates_);
  if (std::filesystem::exists(amb_path, ec)) ok |= parse_ambience_xml(read_text(amb_path), ambience_templates_);
  if (!want_rally && std::filesystem::exists(rib_path, ec)) {
    std::vector<AmbientEmitter3D> rib;
    if (parse_ribbon_audio_xml(read_text(rib_path), rib)) {
      emitters_.insert(emitters_.end(), rib.begin(), rib.end());
      ok = true;
    }
  }
  if (note) {
    *note = "acoustics: " + std::to_string(reverb_templates_.size()) + " reverb templates, " +
            std::to_string(ambience_templates_.size()) + " ambience templates, " +
            std::to_string(emitters_.size()) + " ribbon emitters";
  }
  return ok;
}

bool WorldAcousticsData::load_rally_configs(const std::filesystem::path& rally_root_or_state_root, std::string* note) {
  if (rally_root_or_state_root.empty()) return false;
  std::error_code ec;
  std::vector<std::filesystem::path> track_dirs = {
      rally_root_or_state_root,
      rally_root_or_state_root / "media" / "tracks" / "coloradodirt",
      rally_root_or_state_root / "Media" / "tracks" / "ColoradoDirt",
      rally_root_or_state_root / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "tracks" / "ColoradoDirt",
  };
  std::vector<std::filesystem::path> zip_cands;
  if (lower_str(rally_root_or_state_root.extension().string()) == ".zip" &&
      std::filesystem::exists(rally_root_or_state_root, ec)) {
    zip_cands.push_back(rally_root_or_state_root);
  }
  zip_cands.push_back(rally_root_or_state_root / "Media" / "DLCZips" / "1600_pri_65.zip");
  zip_cands.push_back(rally_root_or_state_root / "1600_pri_65.zip");
  auto push_pkg_tracks = [&](const std::filesystem::path& pr) {
    if (!std::filesystem::is_directory(pr, ec)) return;
    for (const auto& pkg : std::filesystem::directory_iterator(pr, ec)) {
      if (!pkg.is_directory(ec)) continue;
      const auto dz = pkg.path() / "Media" / "DLCZips";
      if (!std::filesystem::is_directory(dz, ec)) continue;
      for (const auto& z : std::filesystem::directory_iterator(dz, ec)) {
        if (z.is_directory(ec)) {
          track_dirs.push_back(z.path() / "Media" / "tracks" / "ColoradoDirt");
        } else if (z.is_regular_file(ec) && lower_str(z.path().filename().string()) == "1600_pri_65.zip") {
          zip_cands.push_back(z.path());
        }
      }
    }
  };
  push_pkg_tracks(rally_root_or_state_root / "user" / "0000000000000000" / "4D5309C9" / "00000002");
  push_pkg_tracks(rally_root_or_state_root / "00000002");

  bool ok = false;
  for (const auto& d : track_dirs) {
    if (!std::filesystem::is_directory(d, ec)) continue;
    const auto rev = d / "coloradodirt_reverb.xml";
    const auto amb = d / "coloradodirt_ambience.xml";
    if (std::filesystem::exists(rev, ec)) {
      std::vector<ReverbTemplate> rt;
      if (parse_reverb_xml(read_text(rev), rt)) {
        reverb_templates_ = std::move(rt);
        ok = true;
      }
    }
    if (std::filesystem::exists(amb, ec)) {
      std::vector<AmbienceTemplate> at;
      if (parse_ambience_xml(read_text(amb), at)) {
        ambience_templates_ = std::move(at);
        ok = true;
      }
    }
    if (ok) break;
  }
  if (!ok) {
    // Also check Rally DLC archive (`1600_pri_65.zip`) directly when XMLs are not unpacked loose.
    for (const auto& zp : zip_cands) {
      if (!std::filesystem::exists(zp, ec)) continue;
      auto arc = fh1::Archive::open(zp, nullptr);
      if (!arc) continue;
      for (const auto& m : arc->members()) {
        std::string low = lower_str(m.name);
        for (char& c : low) {
          if (c == '\\') c = '/';
        }
        if (low.ends_with("coloradodirt_reverb.xml")) {
          const auto rd = arc->read(m);
          std::vector<ReverbTemplate> rt;
          if (rd.ok() && parse_reverb_xml(std::string_view(reinterpret_cast<const char*>(rd.data.data()), rd.data.size()), rt)) {
            reverb_templates_ = std::move(rt);
            ok = true;
          }
        } else if (low.ends_with("coloradodirt_ambience.xml")) {
          const auto rd = arc->read(m);
          std::vector<AmbienceTemplate> at;
          if (rd.ok() && parse_ambience_xml(std::string_view(reinterpret_cast<const char*>(rd.data.data()), rd.data.size()), at)) {
            ambience_templates_ = std::move(at);
            ok = true;
          }
        }
      }
      if (ok) break;
    }
  }
  if (ok) rally_world_ = true;
  if (note) {
    *note = "rally acoustics: " + std::to_string(reverb_templates_.size()) + " reverb templates, " +
            std::to_string(ambience_templates_.size()) + " ambience templates";
  }
  return ok;
}

int WorldAcousticsData::load_soundscapes_from_archive(const std::filesystem::path& bin_zip, int max_tiles) {
  std::error_code ec;
  if (bin_zip.empty() || !std::filesystem::exists(bin_zip, ec)) return 0;
  auto arc = fh1::Archive::open(bin_zip, nullptr);
  if (!arc) return 0;
  int loaded = 0;
  for (const auto& m : arc->members()) {
    const std::string low = lower_str(m.name);
    if (low.size() < 11 || low.compare(low.size() - 11, 11, ".soundscape") != 0) continue;
    auto rd = arc->read(m);
    if (!rd.ok()) continue;
    std::string_view sv(reinterpret_cast<const char*>(rd.data.data()), rd.data.size());
    SoundscapeTile tile;
    if (parse_soundscape_xml(sv, tile)) {
      add_tile(std::move(tile));
      ++loaded;
      if (max_tiles > 0 && loaded >= max_tiles) break;
    }
  }
  return loaded;
}

void WorldAcousticsData::clear_soundscapes() {
  reverb_zones_.clear();
  ambience_zones_.clear();
}

void WorldAcousticsData::add_tile(SoundscapeTile tile) {
  for (auto& em : tile.emitters)
    if (em.event_hash == 0u) em.event_hash = ambience_event_hash(em.event_name);
  for (auto& rz : tile.reverb_zones) reverb_zones_.push_back(std::move(rz));
  for (auto& az : tile.ambience_zones) ambience_zones_.push_back(std::move(az));
  // Every placed Track3D event (crickets beds, rivers, crowds, the festival)
  // but the Default_Reflections_* whooshes, which follow passing objects
  // (110,000 of the 125,000 placements) and are not modelled.
  for (auto& em : tile.emitters) {
    const std::string low = lower_str(em.event_name);
    if (low.find("reflections") != std::string::npos) continue;
    em.event_hash = ambience_event_hash(em.event_name);
    emitters_.push_back(std::move(em));
  }
}

AcousticsSample WorldAcousticsData::evaluate(float x, float y, float z, float listener_heading_rad) const {
  AcousticsSample out;
  out.is_rally_world = rally_world_;
  if (!reverb_templates_.empty()) out.reverb = reverb_templates_[0];
  if (!ambience_templates_.empty()) out.ambience_snapshot = ambience_templates_[0].snapshot_mix;

  // Evaluate reverb zones (barycentric blend between Template0 and Template1).
  for (const auto& rz : reverb_zones_) {
    if (x < rz.min_x || x > rz.max_x || z < rz.min_z || z > rz.max_z) continue;
    if (y < rz.min_height || y > rz.max_height) continue;
    bool hit = false;
    for (const auto& tri : rz.triangles) {
      float w0 = 0, w1 = 0, w2 = 0;
      if (!barycentric_xz(x, z, tri.x, tri.z, w0, w1, w2)) continue;
      const float blend = std::clamp(w0 * tri.value[0] + w1 * tri.value[1] + w2 * tri.value[2], 0.0f, 1.0f);
      const int idx0 = std::clamp(rz.template0, 0, std::max(0, int(reverb_templates_.size()) - 1));
      const int idx1 = std::clamp(rz.template1, 0, std::max(0, int(reverb_templates_.size()) - 1));
      if (!reverb_templates_.empty()) {
        out.reverb = lerp_reverb(reverb_templates_[size_t(idx0)], reverb_templates_[size_t(idx1)], blend);
        out.reverb_template_idx = blend >= 0.5f ? idx1 : idx0;
        out.in_tunnel = iequals(out.reverb.snapshot_mix, "InTunnel") ||
                        (blend >= 0.35f && iequals(reverb_templates_[size_t(idx1)].snapshot_mix, "InTunnel")) ||
                        (blend <= 0.65f && iequals(reverb_templates_[size_t(idx0)].snapshot_mix, "InTunnel"));
      }
      hit = true;
      break;
    }
    if (hit) break;
  }

  // Evaluate ambience zones.
  for (const auto& az : ambience_zones_) {
    if (x < az.min_x || x > az.max_x || z < az.min_z || z > az.max_z) continue;
    if (y < az.min_height || y > az.max_height) continue;
    bool hit = false;
    for (const auto& tri : az.triangles) {
      float w0 = 0, w1 = 0, w2 = 0;
      if (!barycentric_xz(x, z, tri.x, tri.z, w0, w1, w2)) continue;
      if (!ambience_templates_.empty()) {
        const int idx = std::clamp(az.template_idx, 0, int(ambience_templates_.size()) - 1);
        out.ambience_template_idx = idx;
        out.ambience_snapshot = ambience_templates_[size_t(idx)].snapshot_mix;
      }
      hit = true;
      break;
    }
    if (hit) break;
  }

  // Evaluate festival / crowd proximity and top-4 distance-culled 3D ambient
  // emitters from ribbon & tile Track3D emitters.
  const float rx = std::cos(listener_heading_rad);
  const float rz = -std::sin(listener_heading_rad);
  for (size_t ei = 0; ei < emitters_.size(); ++ei) {
    const auto& em = emitters_[ei];
    std::array<float, 3> delta{};
    const float d = dist_point_to_segment(x, y, z, em.pos1, em.pos2, &delta);
    if (d >= em.max_distance) continue;
    const float prox = std::clamp(1.0f - d / std::max(1.0f, em.max_distance), 0.0f, 1.0f);
    const std::string low = lower_str(em.event_name + " " + em.object_name);
    AmbientEmitterKind kind = AmbientEmitterKind::kGeneric;
    if (low.find("festival") != std::string::npos || low.find("stage") != std::string::npos) {
      out.festival_proximity = std::max(out.festival_proximity, prox);
      kind = AmbientEmitterKind::kFestival;
    }
    if (low.find("crowd") != std::string::npos || low.find("cheer") != std::string::npos) {
      out.crowd_proximity = std::max(out.crowd_proximity, prox);
      if (kind == AmbientEmitterKind::kGeneric) kind = AmbientEmitterKind::kCrowd;
    } else if (low.find("water") != std::string::npos || low.find("river") != std::string::npos ||
               low.find("dam") != std::string::npos || low.find("stream") != std::string::npos) {
      kind = AmbientEmitterKind::kWater;
    } else if (low.find("tree") != std::string::npos || low.find("forest") != std::string::npos ||
               low.find("wind") != std::string::npos) {
      kind = AmbientEmitterKind::kTrees;
    }
    const float att = prox * prox;
    if (att > 1e-4f) {
      const float pan = d > 1e-3f ? std::clamp((delta[0] * rx + delta[2] * rz) / d, -1.0f, 1.0f) : 0.0f;
      int slot = -1;
      float min_g = att;
      for (int s = 0; s < AcousticsSample::kMaxActiveEmitters; ++s) {
        if (!out.active_emitters[size_t(s)].active) {
          slot = s;
          break;
        }
        if (out.active_emitters[size_t(s)].gain < min_g) {
          min_g = out.active_emitters[size_t(s)].gain;
          slot = s;
        }
      }
      if (slot >= 0) {
        auto& dst = out.active_emitters[size_t(slot)];
        dst.active = true;
        dst.kind = kind;
        dst.gain = att;
        dst.pan = pan;
        dst.id_hash = hash_emitter(em.event_name, em.object_name, ei);
        dst.event_hash = em.event_hash != 0u ? em.event_hash : ambience_event_hash(em.event_name);
        dst.distance_m = d;
      }
    }
  }
  if (out.festival_proximity > 0.35f) out.ambience_snapshot = "Festival";
  return out;
}

// ------------------------------------------------ RtReverbParams / RtAcousticsSample

RtReverbParams RtReverbParams::from_template(const ReverbTemplate& t) {
  RtReverbParams r{};
  r.decay_time_s = t.decay_time_s;
  r.decay_hf_ratio = t.decay_hf_ratio;
  r.density_pct = t.density_pct;
  r.diffusion_pct = t.diffusion_pct;
  r.dry_level_mb = t.dry_level_mb;
  r.room_level_mb = t.room_level_mb;
  r.room_hf_level_mb = t.room_hf_level_mb;
  r.reflections_level_mb = t.reflections_level_mb;
  r.reflections_delay_s = t.reflections_delay_s;
  r.reverb_level_mb = t.reverb_level_mb;
  r.reverb_delay_s = t.reverb_delay_s;
  r.hf_reference_hz = t.hf_reference_hz;
  r.near_dry = t.near_dry;
  r.near_wet = t.near_wet;
  r.far_dry = t.far_dry;
  r.far_wet = t.far_wet;
  return r;
}

RtAcousticsSample RtAcousticsSample::from_sample(const AcousticsSample& s) {
  RtAcousticsSample out{};
  out.reverb = RtReverbParams::from_template(s.reverb);
  out.reverb_template_idx = s.reverb_template_idx;
  out.in_tunnel = s.in_tunnel;
  out.ambience_template_idx = s.ambience_template_idx;
  const size_t n = std::min(s.ambience_snapshot.size(), out.ambience_snapshot.size() - 1);
  if (n > 0) std::memcpy(out.ambience_snapshot.data(), s.ambience_snapshot.data(), n);
  out.ambience_snapshot[n] = '\0';
  out.festival_proximity = s.festival_proximity;
  out.crowd_proximity = s.crowd_proximity;
  out.is_rally_world = s.is_rally_world;
  out.active_emitters = s.active_emitters;
  return out;
}

std::string_view RtAcousticsSample::ambience_snapshot_view() const {
  const size_t len = strnlen(ambience_snapshot.data(), ambience_snapshot.size());
  return std::string_view(ambience_snapshot.data(), len);
}

// ------------------------------------------------ EnvironmentalReverb

float EnvironmentalReverb::Comb::tick(float in) {
  if (buf.empty()) return in;
  const float out = buf[idx];
  filter_store = out * (1.0f - damp) + filter_store * damp;
  buf[idx] = in + filter_store * feedback;
  if (++idx >= buf.size()) idx = 0;
  return out;
}

float EnvironmentalReverb::Allpass::tick(float in) {
  if (buf.empty()) return in;
  const float bufout = buf[idx];
  const float g = feedback;
  const float v = in + g * bufout;
  buf[idx] = v;
  if (++idx >= buf.size()) idx = 0;
  return bufout - g * v;
}

void EnvironmentalReverb::prepare(int rate) {
  rate_ = std::max(8000, rate);
  const float scale = float(rate_) / 44100.0f;
  // Classic stereo-decorrelated prime delay lengths.
  static constexpr int kCombBase[4] = {1116, 1188, 1277, 1356};
  static constexpr int kApBase[2] = {556, 441};
  for (int i = 0; i < 4; ++i) {
    comb_l_[size_t(i)].buf.assign(size_t(std::max(16, int(kCombBase[i] * scale))), 0.0f);
    comb_r_[size_t(i)].buf.assign(size_t(std::max(16, int((kCombBase[i] + 23) * scale))), 0.0f);
    comb_l_[size_t(i)].idx = comb_r_[size_t(i)].idx = 0;
    comb_l_[size_t(i)].filter_store = comb_r_[size_t(i)].filter_store = 0.0f;
  }
  for (int i = 0; i < 2; ++i) {
    ap_l_[size_t(i)].buf.assign(size_t(std::max(8, int(kApBase[i] * scale))), 0.0f);
    ap_r_[size_t(i)].buf.assign(size_t(std::max(8, int((kApBase[i] + 19) * scale))), 0.0f);
    ap_l_[size_t(i)].idx = ap_r_[size_t(i)].idx = 0;
  }
  const size_t max_pre = size_t(std::max(64, int(0.12f * rate_)));
  predelay_l_.assign(max_pre, 0.0f);
  predelay_r_.assign(max_pre, 0.0f);
  predelay_idx_ = 0;
  const size_t max_early = size_t(std::max(128, int(0.14f * rate_)));
  early_l_.assign(max_early, 0.0f);
  early_r_.assign(max_early, 0.0f);
  early_idx_ = 0;
  ReverbTemplate def{};
  set_parameters(def);
}

void EnvironmentalReverb::reset() {
  for (auto& c : comb_l_) {
    std::fill(c.buf.begin(), c.buf.end(), 0.0f);
    c.filter_store = 0.0f;
  }
  for (auto& c : comb_r_) {
    std::fill(c.buf.begin(), c.buf.end(), 0.0f);
    c.filter_store = 0.0f;
  }
  for (auto& a : ap_l_) std::fill(a.buf.begin(), a.buf.end(), 0.0f);
  for (auto& a : ap_r_) std::fill(a.buf.begin(), a.buf.end(), 0.0f);
  std::fill(predelay_l_.begin(), predelay_l_.end(), 0.0f);
  std::fill(predelay_r_.begin(), predelay_r_.end(), 0.0f);
  std::fill(early_l_.begin(), early_l_.end(), 0.0f);
  std::fill(early_r_.begin(), early_r_.end(), 0.0f);
}

void EnvironmentalReverb::set_parameters(const ReverbTemplate& tmpl) {
  set_parameters(RtReverbParams::from_template(tmpl));
}

void EnvironmentalReverb::set_parameters(const RtReverbParams& tmpl) {
  decay_time_ = std::clamp(tmpl.decay_time_s, 0.15f, 6.0f);
  // Compute room + late reverb wet scale from I3DL2 millibels, blending both
  // NearWet and FarWet so outdoor canyon/bridge templates (NearWet==0, FarWet>0)
  // remain audible instead of being silenced.
  const float room_lin = std::pow(10.0f, std::clamp(tmpl.room_level_mb, -6000.0f, 0.0f) / 2000.0f);
  const float rev_lin = std::pow(10.0f, std::clamp(tmpl.reverb_level_mb, -6000.0f, 1500.0f) / 2000.0f);
  const float eff_wet = std::clamp(std::max(tmpl.near_wet, 0.55f * tmpl.far_wet), 0.0f, 1.0f);
  target_wet_ = std::clamp(eff_wet * room_lin * std::min(rev_lin, 1.5f) * 0.45f, 0.0f, 0.65f);
  target_dry_ = std::clamp(0.75f * tmpl.near_dry + 0.25f * tmpl.far_dry, 0.2f, 1.0f);

  // Early slap-back reflections driven by I3DL2 reflections_level_mb and reflections_delay_s.
  const float early_lin = std::pow(10.0f, std::clamp(tmpl.reflections_level_mb, -6000.0f, 1000.0f) / 2000.0f);
  const float er_presence = eff_wet > 1e-3f ? std::max(eff_wet, 0.35f)
                                            : (tmpl.reflections_level_mb > -2400.0f ? 0.65f : 0.0f);
  target_early_ = std::clamp(er_presence * room_lin * std::min(early_lin, 1.8f) * 0.55f, 0.0f, 0.65f);
  if (!early_l_.empty()) {
    const float base_s = std::clamp(tmpl.reflections_delay_s, 0.004f, 0.080f);
    static constexpr float kTapOffL[4] = {0.0f, 0.0073f, 0.0157f, 0.0241f};
    static constexpr float kTapOffR[4] = {0.0031f, 0.0113f, 0.0199f, 0.0293f};
    const size_t max_tap = early_l_.size() - 1;
    for (int t = 0; t < 4; ++t) {
      early_taps_l_[size_t(t)] = std::clamp(size_t((base_s + kTapOffL[t]) * float(rate_)), size_t(1), max_tap);
      early_taps_r_[size_t(t)] = std::clamp(size_t((base_s + kTapOffR[t]) * float(rate_)), size_t(1), max_tap);
    }
  }

  const float hf_damp = std::clamp(0.45f - 0.12f * tmpl.decay_hf_ratio + (-tmpl.room_hf_level_mb / 6000.0f) * 0.25f,
                                   0.08f, 0.78f);
  for (int i = 0; i < 4; ++i) {
    const float delay_s = float(std::max<size_t>(1, comb_l_[size_t(i)].buf.size())) / float(rate_);
    const float fb = std::clamp(std::pow(10.0f, -3.0f * delay_s / decay_time_), 0.25f, 0.96f);
    comb_l_[size_t(i)].feedback = comb_r_[size_t(i)].feedback = fb;
    comb_l_[size_t(i)].damp = comb_r_[size_t(i)].damp = hf_damp;
  }
  const float diff = std::clamp(tmpl.diffusion_pct / 100.0f, 0.2f, 1.0f) * 0.5f;
  for (int i = 0; i < 2; ++i) ap_l_[size_t(i)].feedback = ap_r_[size_t(i)].feedback = diff;

  const float pre_s = std::clamp(tmpl.reverb_delay_s + tmpl.reflections_delay_s, 0.0f, 0.10f);
  predelay_samples_ = std::min(predelay_l_.empty() ? size_t(0) : predelay_l_.size() - 1, size_t(pre_s * rate_));
}

void EnvironmentalReverb::process(float* io, int frames) {
  if (!io || frames <= 0 || comb_l_[0].buf.empty()) return;
  if (target_wet_ < 1e-4f && wet_smooth_ < 1e-4f && target_early_ < 1e-4f && early_smooth_ < 1e-4f) return;

  const float sm = 1.0f - std::exp(-1.0f / (0.04f * float(rate_)));
  const size_t pre_cap = predelay_l_.size();
  const size_t er_cap = early_l_.size();
  static constexpr float kErWeight[4] = {0.50f, -0.35f, 0.25f, -0.18f};
  for (int i = 0; i < frames; ++i) {
    wet_smooth_ += (target_wet_ - wet_smooth_) * sm;
    dry_smooth_ += (target_dry_ - dry_smooth_) * sm;
    early_smooth_ += (target_early_ - early_smooth_) * sm;
    const float in_l = io[2 * i];
    const float in_r = io[2 * i + 1];

    float er_l = 0.0f, er_r = 0.0f;
    if (er_cap > 0) {
      early_l_[early_idx_] = in_l;
      early_r_[early_idx_] = in_r;
      if (early_smooth_ > 1e-5f) {
        for (int t = 0; t < 4; ++t) {
          const size_t rl = (early_idx_ + er_cap - early_taps_l_[size_t(t)]) % er_cap;
          const size_t rr = (early_idx_ + er_cap - early_taps_r_[size_t(t)]) % er_cap;
          er_l += early_l_[rl] * kErWeight[t];
          er_r += early_r_[rr] * kErWeight[t];
        }
      }
      if (++early_idx_ >= er_cap) early_idx_ = 0;
    }

    float pre_l = in_l, pre_r = in_r;
    if (pre_cap > 0) {
      predelay_l_[predelay_idx_] = in_l;
      predelay_r_[predelay_idx_] = in_r;
      const size_t ridx = (predelay_idx_ + pre_cap - predelay_samples_) % pre_cap;
      pre_l = predelay_l_[ridx];
      pre_r = predelay_r_[ridx];
      if (++predelay_idx_ >= pre_cap) predelay_idx_ = 0;
    }

    const float mono_in = 0.5f * (pre_l + pre_r);
    float wl = 0.0f, wr = 0.0f;
    for (int c = 0; c < 4; ++c) {
      wl += comb_l_[size_t(c)].tick(mono_in);
      wr += comb_r_[size_t(c)].tick(mono_in);
    }
    wl *= 0.25f;
    wr *= 0.25f;
    for (int a = 0; a < 2; ++a) {
      wl = ap_l_[size_t(a)].tick(wl);
      wr = ap_r_[size_t(a)].tick(wr);
    }
    io[2 * i] = in_l * dry_smooth_ + er_l * early_smooth_ + wl * wet_smooth_;
    io[2 * i + 1] = in_r * dry_smooth_ + er_r * early_smooth_ + wr * wet_smooth_;
  }
}

bool EnvironmentalReverb::process_wet(const float* in_stereo, float* out_wet_stereo, int frames) {
  if (!in_stereo || !out_wet_stereo || frames <= 0 || comb_l_[0].buf.empty()) return false;
  if (target_wet_ < 1e-4f && wet_smooth_ < 1e-4f && target_early_ < 1e-4f && early_smooth_ < 1e-4f) {
    std::fill(out_wet_stereo, out_wet_stereo + size_t(frames) * 2, 0.0f);
    return false;
  }

  const float sm = 1.0f - std::exp(-1.0f / (0.04f * float(rate_)));
  const size_t pre_cap = predelay_l_.size();
  const size_t er_cap = early_l_.size();
  static constexpr float kErWeight[4] = {0.50f, -0.35f, 0.25f, -0.18f};
  for (int i = 0; i < frames; ++i) {
    wet_smooth_ += (target_wet_ - wet_smooth_) * sm;
    dry_smooth_ += (target_dry_ - dry_smooth_) * sm;
    early_smooth_ += (target_early_ - early_smooth_) * sm;
    const float in_l = in_stereo[2 * i];
    const float in_r = in_stereo[2 * i + 1];

    float er_l = 0.0f, er_r = 0.0f;
    if (er_cap > 0) {
      early_l_[early_idx_] = in_l;
      early_r_[early_idx_] = in_r;
      if (early_smooth_ > 1e-5f) {
        for (int t = 0; t < 4; ++t) {
          const size_t rl = (early_idx_ + er_cap - early_taps_l_[size_t(t)]) % er_cap;
          const size_t rr = (early_idx_ + er_cap - early_taps_r_[size_t(t)]) % er_cap;
          er_l += early_l_[rl] * kErWeight[t];
          er_r += early_r_[rr] * kErWeight[t];
        }
      }
      if (++early_idx_ >= er_cap) early_idx_ = 0;
    }

    float pre_l = in_l, pre_r = in_r;
    if (pre_cap > 0) {
      predelay_l_[predelay_idx_] = in_l;
      predelay_r_[predelay_idx_] = in_r;
      const size_t ridx = (predelay_idx_ + pre_cap - predelay_samples_) % pre_cap;
      pre_l = predelay_l_[ridx];
      pre_r = predelay_r_[ridx];
      if (++predelay_idx_ >= pre_cap) predelay_idx_ = 0;
    }

    const float mono_in = 0.5f * (pre_l + pre_r);
    float wl = 0.0f, wr = 0.0f;
    for (int c = 0; c < 4; ++c) {
      wl += comb_l_[size_t(c)].tick(mono_in);
      wr += comb_r_[size_t(c)].tick(mono_in);
    }
    wl *= 0.25f;
    wr *= 0.25f;
    for (int a = 0; a < 2; ++a) {
      wl = ap_l_[size_t(a)].tick(wl);
      wr = ap_r_[size_t(a)].tick(wr);
    }
    out_wet_stereo[2 * i] = er_l * early_smooth_ + wl * wet_smooth_;
    out_wet_stereo[2 * i + 1] = er_r * early_smooth_ + wr * wet_smooth_;
  }
  return true;
}

// ------------------------------------------------ AmbienceBedPlayer

namespace {

bool read_bytes_amb(const std::filesystem::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty();
}

bool write_bytes_amb(const std::filesystem::path& p, const void* data, size_t n) {
  std::ofstream f(p, std::ios::binary);
  f.write(static_cast<const char*>(data), std::streamsize(n));
  return bool(f);
}

bool read_cached_amb(const std::filesystem::path& p, PcmSample& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  int32_t rate = 0;
  uint32_t frames = 0;
  f.read(reinterpret_cast<char*>(&rate), 4);
  f.read(reinterpret_cast<char*>(&frames), 4);
  if (!f || rate <= 0 || frames == 0 || frames > 48000u * 60u) return false;
  std::vector<int16_t> s16(frames);
  f.read(reinterpret_cast<char*>(s16.data()), std::streamsize(frames * 2));
  if (!f) return false;
  out.rate = rate;
  out.data.resize(frames);
  for (size_t i = 0; i < frames; ++i) out.data[i] = float(s16[i]) / 32768.0f;
  return true;
}

void write_cached_amb(const std::filesystem::path& p, const PcmSample& s) {
  if (p.empty() || s.rate <= 0 || s.data.empty()) return;
  std::error_code ec;
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) return;
  const int32_t rate = s.rate;
  const uint32_t frames = uint32_t(s.data.size());
  f.write(reinterpret_cast<const char*>(&rate), 4);
  f.write(reinterpret_cast<const char*>(&frames), 4);
  std::vector<int16_t> s16(frames);
  for (size_t i = 0; i < frames; ++i)
    s16[i] = int16_t(std::clamp(std::lround(double(s.data[i]) * 32767.0), -32768l, 32767l));
  f.write(reinterpret_cast<char*>(s16.data()), std::streamsize(frames * 2));
}

void make_seamless_loop(PcmSample& pcm) {
  const size_t n = pcm.data.size();
  if (n < 1024) return;
  const size_t max_xf = size_t(std::max(1, pcm.rate)) / 2;  // up to 0.5 s crossfade on 25 s beds
  const size_t xfade = std::clamp<size_t>(std::min(n / 8, max_xf), 64, n / 4);
  const size_t keep = n - xfade;
  constexpr double kPi = 3.14159265358979323846;
  for (size_t i = 0; i < xfade; ++i) {
    const float w = 0.5f * (1.0f - float(std::cos(kPi * double(i) / double(xfade))));
    pcm.data[i] = pcm.data[i] * w + pcm.data[keep + i] * (1.0f - w);
  }
  pcm.data.resize(keep);
}

bool decode_amb_subsound(const std::filesystem::path& bank_path, uint32_t subsound, const std::filesystem::path& ffmpeg,
                         const std::filesystem::path& temp_dir, PcmSample& out) {
  std::vector<uint8_t> bank_data;
  if (!read_bytes_amb(bank_path, bank_data)) return false;
  std::vector<FsbSample> samples;
  if (!read_fsb4(bank_data, samples, nullptr) || subsound >= samples.size()) return false;
  const FsbSample& s = samples[subsound];
  if (s.offset + s.bytes > bank_data.size() || s.bytes == 0) return false;
  const int rate = s.rate > 0 ? s.rate : 48000;
  constexpr size_t kMaxAmbFrames = 48000u * 25u;  // cap ambient bed decode to 25 s loop
  if (s.xma()) {
    const auto span = std::span<const uint8_t>(bank_data).subspan(size_t(s.offset), s.bytes);
    if (decode_xma2_mono(s, span, out, nullptr)) {
      if (out.data.size() > kMaxAmbFrames) out.data.resize(kMaxAmbFrames);
      make_seamless_loop(out);
      return true;
    }
    if (ffmpeg.empty()) return false;
    const auto wave = xma2_wave(s, span);
    const auto in_p = temp_dir / ("amb_" + std::to_string(subsound) + ".wav");
    const auto out_p = temp_dir / ("amb_" + std::to_string(subsound) + ".s16");
    std::error_code ec;
    std::filesystem::remove(out_p, ec);
    if (!write_bytes_amb(in_p, wave.data(), wave.size())) return false;
    std::string cmd = "\"" + ffmpeg.string() + "\" -hide_banner -nostdin -v error -y -t 25 -i \"" + in_p.string() +
                      "\" -f s16le -acodec pcm_s16le \"" + out_p.string() + "\"";
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";
#endif
    if (std::system(cmd.c_str()) != 0) return false;
    std::vector<uint8_t> raw;
    if (!read_bytes_amb(out_p, raw) || raw.size() < 2) return false;
    const size_t ch = std::max<size_t>(1, s.channels);
    const size_t frames = std::min(kMaxAmbFrames, raw.size() / (2 * ch));
    if (frames == 0) return false;
    out.rate = rate;
    out.data.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
      double acc = 0.0;
      for (size_t c = 0; c < ch; ++c) {
        const size_t at = (i * ch + c) * 2;
        acc += double(int16_t(raw[at] | raw[at + 1] << 8)) / 32768.0;
      }
      out.data[i] = float(acc / double(ch));
    }
    std::filesystem::remove(in_p, ec);
    std::filesystem::remove(out_p, ec);
    make_seamless_loop(out);
    return true;
  }
  auto src = make_fsb_mpeg_source(bank_path, s.offset, s.bytes, rate);
  if (src && src->open()) {
    out.rate = src->rate();
    out.data.clear();
    float tmp[2048 * 2];
    while (out.data.size() < kMaxAmbFrames) {
      const int64_t got = src->read(tmp, 2048);
      if (got <= 0) break;
      for (int64_t i = 0; i < got && out.data.size() < kMaxAmbFrames; ++i)
        out.data.push_back(0.5f * (tmp[2 * i] + tmp[2 * i + 1]));
    }
    if (!out.data.empty()) make_seamless_loop(out);
    return !out.data.empty();
  }
  return false;
}

bool pick_first_fev_sample(const FevProject& proj, const FevEvent& ev, std::filesystem::path& bank_out,
                           uint32_t& sub_out, int* layer_out = nullptr, int* inst_out = nullptr) {
  auto pick = [&](const std::vector<FevSoundInstance>& insts, int layer) -> bool {
    for (size_t ii = 0; ii < insts.size(); ++ii) {
      const auto& si = insts[ii];
      if (si.sound_def_index >= proj.sound_defs.size()) continue;
      for (const auto& en : proj.sound_defs[si.sound_def_index].entries) {
        if (en.entry_type == 0 && !en.wavebank_name.empty()) {
          bank_out = cars::mods::resolve(proj.dir / (en.wavebank_name + ".fsb"));
          sub_out = en.subsound_index;
          if (layer_out) *layer_out = layer;
          if (inst_out) *inst_out = int(ii);
          return true;
        }
      }
    }
    return false;
  };
  if (pick(ev.simple_instances, -1)) return true;
  for (size_t li = 0; li < ev.layers.size(); ++li)
    if (pick(ev.layers[li].instances, int(li))) return true;
  return false;
}

bool dir_has_rally_amb(const std::filesystem::path& dir) {
  std::error_code ec;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return false;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string fn = lower_str(e.path().filename().string());
    const std::string ext = lower_str(e.path().extension().string());
    if (fn.rfind("amb_", 0) == 0 && (ext == ".fev" || ext == ".fsb")) return true;
  }
  return false;
}

std::filesystem::path find_colorado_dirt_amb_dir(const std::filesystem::path& root) {
  std::error_code ec;
  if (root.empty() || !std::filesystem::exists(root, ec)) return {};
  const std::filesystem::path direct[] = {
      root,
      root / "ColoradoDirt",
      root / "Tracks" / "ColoradoDirt",
      root / "audio" / "Tracks" / "ColoradoDirt",
      root / "Media" / "audio" / "Tracks" / "ColoradoDirt",
      root / "media" / "audio" / "Tracks" / "ColoradoDirt",
      root / "Media" / "DLCZips" / "1600_pri_65" / "Media" / "audio" / "Tracks" / "ColoradoDirt",
  };
  for (const auto& d : direct) {
    if (dir_has_rally_amb(d)) return d;
  }
  auto scan_dlczips = [&](const std::filesystem::path& dz) -> std::filesystem::path {
    if (!std::filesystem::is_directory(dz, ec)) return {};
    for (const auto& z : std::filesystem::directory_iterator(dz, ec)) {
      if (!z.is_directory(ec)) continue;
      for (const auto& cand : {z.path() / "Media" / "audio" / "Tracks" / "ColoradoDirt",
                               z.path() / "Media" / "audio" / "tracks" / "coloradodirt"}) {
        if (dir_has_rally_amb(cand)) return cand;
      }
    }
    return {};
  };
  if (auto hit = scan_dlczips(root / "Media" / "DLCZips"); !hit.empty()) return hit;
  const std::filesystem::path pkg_roots[] = {
      root / "user" / "0000000000000000" / "4D5309C9" / "00000002",
      root / "00000002",
      root,
  };
  for (const auto& pr : pkg_roots) {
    if (!std::filesystem::is_directory(pr, ec)) continue;
    for (const auto& pkg : std::filesystem::directory_iterator(pr, ec)) {
      if (!pkg.is_directory(ec)) continue;
      if (auto hit = scan_dlczips(pkg.path() / "Media" / "DLCZips"); !hit.empty()) return hit;
    }
  }
  return {};
}

std::string normalize_region_token(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) out.push_back(char(std::tolower(u)));
  }
  if (out.rfind("amb", 0) == 0 && out.size() > 3) out.erase(0, 3);
  return out;
}

}  // namespace

AmbienceBedPlayer::AmbienceBedPlayer(int rate) : rate_(std::max(8000, rate)) {
  beds_.fill(nullptr);
  bed_phase_.fill(0.0);
  bed_gain_.fill(0.0f);
  rally_beds_.fill(nullptr);
  rally_phase_.fill(0.0);
  rally_gain_.fill(0.0f);
  for (size_t i = 0; i < emitter_voices_.size(); ++i) {
    emitter_voices_[i].rng = 0x9E3779B9u ^ uint32_t((i + 1) * 0x85EBCA6Bu);
  }
}

AmbienceBedPlayer::~AmbienceBedPlayer() {
  stop_worker();
  stop_rally_worker();
  drain_loaded();
  reclaim_retired();
  for (PcmSample*& p : beds_) {
    delete p;
    p = nullptr;
  }
  for (PcmSample*& p : rally_beds_) {
    delete p;
    p = nullptr;
  }
  for (PcmSample*& p : event_pcm_) {
    delete p;
    p = nullptr;
  }
}

void AmbienceBedPlayer::stop_worker() {
  std::lock_guard<std::mutex> lk(lifecycle_mu_);
  stop_worker_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
}

void AmbienceBedPlayer::stop_rally_worker() {
  std::lock_guard<std::mutex> lk(lifecycle_mu_);
  stop_rally_worker_.store(true, std::memory_order_release);
  if (rally_worker_.joinable()) rally_worker_.join();
}

void AmbienceBedPlayer::retire_pcm(PcmSample* pcm) {
  if (!pcm) return;
  if (!retired_pcms_.push(pcm)) {
    delete pcm;
  }
}

void AmbienceBedPlayer::reclaim_retired() {
  PcmSample* p = nullptr;
  while (retired_pcms_.pop(p)) {
    delete p;
  }
}

void AmbienceBedPlayer::drain_loaded() {
  auto drain_bed = [&](SpscQueue<LoadedBed>& q) {
    LoadedBed lb;
    while (q.pop(lb)) {
      const size_t idx = size_t(lb.slot);
      if (idx < beds_.size()) {
        retire_pcm(beds_[idx]);
        beds_[idx] = lb.pcm;
      } else {
        retire_pcm(lb.pcm);
      }
    }
  };
  drain_bed(loaded_q_);
  drain_bed(direct_loaded_q_);
  LoadedEventPcm le;
  while (event_loaded_q_.pop(le)) {
    if (le.id >= 0 && le.id < kMaxEventSamples) {
      retire_pcm(event_pcm_[size_t(le.id)]);
      event_pcm_[size_t(le.id)] = le.pcm;
    } else {
      retire_pcm(le.pcm);
    }
  }
  auto drain_rally = [&](SpscQueue<LoadedRallyBed>& q) {
    LoadedRallyBed rb;
    while (q.pop(rb)) {
      if (rb.region_idx >= 0 && rb.region_idx < kMaxRallyRegions) {
        const size_t idx = size_t(rb.region_idx);
        retire_pcm(rally_beds_[idx]);
        rally_beds_[idx] = rb.pcm;
        rally_names_[idx] = rb.name;
      } else {
        retire_pcm(rb.pcm);
      }
    }
  };
  drain_rally(rally_loaded_q_);
  drain_rally(rally_direct_q_);
}

void AmbienceBedPlayer::set_bed_sample(AmbienceBedSlot slot, PcmSample pcm) {
  reclaim_retired();
  const size_t idx = size_t(slot);
  if (idx >= beds_.size()) return;
  if (pcm.data.size() >= 1024 && std::fabs(pcm.data.back() - pcm.data.front()) > 0.02f) {
    make_seamless_loop(pcm);
  }
  auto* p = new PcmSample(std::move(pcm));
  std::lock_guard<std::mutex> lk(direct_q_mu_);
  if (!direct_loaded_q_.push({slot, p})) delete p;
}

void AmbienceBedPlayer::set_rally_bed_sample(int region_idx, std::string_view region_name, PcmSample pcm) {
  reclaim_retired();
  if (region_idx < 0 || region_idx >= kMaxRallyRegions) return;
  if (pcm.data.size() >= 1024 && std::fabs(pcm.data.back() - pcm.data.front()) > 0.02f) {
    make_seamless_loop(pcm);
  }
  LoadedRallyBed item{};
  item.region_idx = region_idx;
  const std::string tok = normalize_region_token(region_name);
  const size_t n = std::min(tok.size(), item.name.size() - 1);
  if (n > 0) std::memcpy(item.name.data(), tok.data(), n);
  item.name[n] = '\0';
  item.pcm = new PcmSample(std::move(pcm));
  std::lock_guard<std::mutex> lk(direct_q_mu_);
  if (!rally_direct_q_.push(item)) delete item.pcm;
}

int AmbienceBedPlayer::loaded_bed_count() const {
  int n = 0;
  for (const PcmSample* p : beds_)
    if (p && !p->data.empty()) ++n;
  return n;
}

int AmbienceBedPlayer::loaded_event_count() const {
  const EventTable* t = events_.load(std::memory_order_acquire);
  return t ? int(t->by_name.size()) : 0;
}

int AmbienceBedPlayer::loaded_event_sample_count() const { return event_sample_count_.load(std::memory_order_acquire); }

const AmbienceBedPlayer::EventTable::Ref* AmbienceBedPlayer::EventTable::find(uint32_t hash) const {
  const auto it = std::lower_bound(by_name.begin(), by_name.end(), hash,
                                   [](const Ref& r, uint32_t h) { return r.hash < h; });
  return (it != by_name.end() && it->hash == hash) ? &*it : nullptr;
}

int AmbienceBedPlayer::rally_bed_count() const {
  int n = 0;
  for (const PcmSample* p : rally_beds_)
    if (p && !p->data.empty()) ++n;
  return n;
}

int AmbienceBedPlayer::load_from_disc(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir) {
  stop_worker();
  reclaim_retired();
  if (game_root.empty()) return 0;
  struct Job {
    AmbienceBedSlot slot;
    std::filesystem::path bank;
    uint32_t sub = 0;
    int event_sample = -1;  // >= 0: an event sound def's loop bed (event_pcm_ id)
  };
  std::vector<Job> jobs;
  std::array<bool, int(AmbienceBedSlot::kCount)> assigned{};
  auto table = std::make_unique<EventTable>();
  EventTable::SlotRef cur_ref{};
  auto assign_slot = [&](AmbienceBedSlot slot, const std::filesystem::path& bank, uint32_t sub) {
    if (assigned[size_t(slot)]) return;
    std::error_code ec;
    if (!std::filesystem::exists(bank, ec)) return;
    assigned[size_t(slot)] = true;
    jobs.push_back({slot, bank, sub});
    table->slots[size_t(slot)] = cur_ref;
  };
  auto param_ids = [](const FevEvent& ev) {
    std::array<int8_t, 12> ids{};
    ids.fill(AmbienceParams::kNone);
    for (size_t i = 0; i < ev.params.size() && i < ids.size(); ++i) ids[i] = AmbienceParams::id_of(ev.params[i].name);
    return ids;
  };

  const std::filesystem::path amb_dirs[] = {
      game_root / "media" / "audio" / "Tracks" / "Colorado",
      game_root / "media" / "audio" / "ambience",
  };
  const char* kFevCandidates[] = {
      "AMB_Quads.fev",
      "AMB_Default.fev",
      "AMB_Festival_Ambience.fev",
      "Colorado_Festival.fev",
      "Colorado_Ambience.fev",
  };
  for (const auto& amb_dir : amb_dirs) {
    for (const char* fev_name : kFevCandidates) {
      const auto fev_path = cars::mods::resolve(amb_dir / fev_name);
      FevProject proj;
      if (!load_fev(fev_path, proj, nullptr)) continue;
      // The project's events and their continuous beds (sound defs whose
      // template has no spawn time; spawned one-shots are not played).
      const int pi = int(table->projects.size());
      const int base = table->sample_count;
      table->sd_base.push_back(base);
      table->sample_count += int(proj.sound_defs.size());
      std::vector<bool> queued_sd(proj.sound_defs.size(), false);
      for (size_t ei = 0; ei < proj.events.size(); ++ei) {
        const FevEvent& ev = proj.events[ei];
        EventTable::Ref ref;
        ref.hash = ambience_event_hash(ev.name);
        ref.proj = uint16_t(pi);
        ref.ev = uint16_t(ei);
        ref.param_id = param_ids(ev);
        table->by_name.push_back(ref);
        for (const auto& ly : ev.layers) {
          for (const auto& si : ly.instances) {
            const size_t sd = si.sound_def_index;
            if (sd >= proj.sound_defs.size() || queued_sd[sd] || base + int(sd) >= kMaxEventSamples) continue;
            const FevSoundDefTemplate* tpl = proj.template_of(proj.sound_defs[sd]);
            if (tpl && tpl->spawned()) continue;
            for (const auto& en : proj.sound_defs[sd].entries) {
              if (en.entry_type != 0 || en.wavebank_name.empty()) continue;
              const auto bank = cars::mods::resolve(proj.dir / (en.wavebank_name + ".fsb"));
              std::error_code bec;
              if (!std::filesystem::exists(bank, bec)) break;
              queued_sd[sd] = true;
              jobs.push_back({AmbienceBedSlot::kCount, bank, en.subsound_index, base + int(sd)});
              break;
            }
          }
        }
      }
      for (size_t ei = 0; ei < proj.events.size(); ++ei) {
        const FevEvent& ev = proj.events[ei];
        std::filesystem::path bank;
        uint32_t sub = 0;
        int layer = 0, inst = 0;
        if (!pick_first_fev_sample(proj, ev, bank, sub, &layer, &inst)) continue;
        cur_ref = {pi, int(ei), layer, inst};
        const std::string lp = lower_str(ev.path);
        if (lp.find("crowd") != std::string::npos || lp.find("cheer") != std::string::npos) {
          assign_slot(AmbienceBedSlot::kCrowd, bank, sub);
        } else if (lp.find("festival") != std::string::npos || lp.find("stage") != std::string::npos) {
          assign_slot(AmbienceBedSlot::kFestival, bank, sub);
        } else if (lp.find("mountain") != std::string::npos || lp.find("forest") != std::string::npos ||
                   lp.find("alpine") != std::string::npos) {
          assign_slot(AmbienceBedSlot::kMountains, bank, sub);
        } else if (lp.find("redrock") != std::string::npos || lp.find("canyon") != std::string::npos ||
                   lp.find("desert") != std::string::npos) {
          assign_slot(AmbienceBedSlot::kRedRock, bank, sub);
        } else if (lp.find("town") != std::string::npos || lp.find("urban") != std::string::npos ||
                   lp.find("city") != std::string::npos) {
          assign_slot(AmbienceBedSlot::kMainTown, bank, sub);
        } else if (lp.find("default") != std::string::npos || lp.find("plain") != std::string::npos ||
                   lp.find("quad") != std::string::npos) {
          assign_slot(AmbienceBedSlot::kDefault, bank, sub);
        }
      }
      table->projects.push_back(std::move(proj));
    }
    cur_ref = {};
    // Fallback: if AMB_Quads_Stream.fsb exists directly under amb_dir, queue its subsounds.
    const auto quad_fsb = cars::mods::resolve(amb_dir / "AMB_Quads_Stream.fsb");
    std::error_code ec;
    if (std::filesystem::exists(quad_fsb, ec)) {
      std::vector<uint8_t> raw;
      std::vector<FsbSample> fsb_s;
      if (read_bytes_amb(quad_fsb, raw) && read_fsb4(raw, fsb_s, nullptr)) {
        for (uint32_t i = 0; i < fsb_s.size(); ++i) {
          const std::string nm = lower_str(fsb_s[i].name);
          if (nm.find("mountain") != std::string::npos) assign_slot(AmbienceBedSlot::kMountains, quad_fsb, i);
          else if (nm.find("redrock") != std::string::npos || nm.find("canyon") != std::string::npos)
            assign_slot(AmbienceBedSlot::kRedRock, quad_fsb, i);
          else if (nm.find("town") != std::string::npos) assign_slot(AmbienceBedSlot::kMainTown, quad_fsb, i);
          else if (nm.find("festival") != std::string::npos) assign_slot(AmbienceBedSlot::kFestival, quad_fsb, i);
          else if (nm.find("crowd") != std::string::npos) assign_slot(AmbienceBedSlot::kCrowd, quad_fsb, i);
          else if (!assigned[size_t(AmbienceBedSlot::kDefault)]) assign_slot(AmbienceBedSlot::kDefault, quad_fsb, i);
        }
      }
    }
  }

  const int queued = int(jobs.size());
  if (jobs.empty()) return 0;

  std::lock_guard<std::mutex> lk(lifecycle_mu_);
  stop_worker_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
  stop_worker_.store(false, std::memory_order_release);
  if (!table->projects.empty()) {
    // One name per event; the earlier project (AMB_Default before
    // Colorado_Festival) wins a shared name.
    std::stable_sort(table->by_name.begin(), table->by_name.end(),
                     [](const EventTable::Ref& a, const EventTable::Ref& b) { return a.hash < b.hash; });
    table->by_name.erase(std::unique(table->by_name.begin(), table->by_name.end(),
                                     [](const EventTable::Ref& a, const EventTable::Ref& b) { return a.hash == b.hash; }),
                         table->by_name.end());
    for (size_t s = 0; s < table->slots.size(); ++s) {
      const auto& sr = table->slots[s];
      table->slot_param_id[s].fill(AmbienceParams::kNone);
      if (sr.proj >= 0) table->slot_param_id[s] = param_ids(table->projects[size_t(sr.proj)].events[size_t(sr.ev)]);
    }
    event_tables_.push_back(std::move(table));
    events_.store(event_tables_.back().get(), std::memory_order_release);
  }
  worker_ = std::thread([this, jobs = std::move(jobs), cache_dir, game_root]() {
    const std::filesystem::path ffmpeg = has_inprocess_xma2_decoder() ? std::filesystem::path{} : find_ffmpeg();
    std::error_code tec;
    const std::filesystem::path amb_cache = cache_dir.empty() ? std::filesystem::path{} : (cache_dir / "ambience");
    const std::filesystem::path prebaked_amb = game_root / "play" / "sound" / "ambience";
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path(tec) /
        ("shift-amb-decode-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(temp_dir, tec);
    for (const auto& j : jobs) {
      if (stop_worker_.load(std::memory_order_acquire)) break;
      const std::string fname = lower_str(j.bank.stem().string()) + "." + std::to_string(j.sub) + ".s16";
      const std::filesystem::path cf = amb_cache.empty() ? std::filesystem::path{} : (amb_cache / fname);
      auto pcm = std::make_unique<PcmSample>();
      bool ok = (!cf.empty() && read_cached_amb(cf, *pcm)) || read_cached_amb(prebaked_amb / fname, *pcm);
      if (ok && pcm->data.size() >= 1024 && std::fabs(pcm->data.back() - pcm->data.front()) > 0.02f) {
        make_seamless_loop(*pcm);
      }
      if (!ok) {
        ok = decode_amb_subsound(j.bank, j.sub, ffmpeg, temp_dir, *pcm);
        if (ok && !cf.empty()) write_cached_amb(cf, *pcm);
      }
      if (ok && !pcm->data.empty()) {
        PcmSample* raw = pcm.release();
        if (j.event_sample >= 0) {
          if (event_loaded_q_.push({j.event_sample, raw})) event_sample_count_.fetch_add(1, std::memory_order_acq_rel);
          else delete raw;
        } else if (!loaded_q_.push({j.slot, raw})) {
          delete raw;
        }
      }
    }
    std::filesystem::remove_all(temp_dir, tec);
  });
  return queued;
}

int AmbienceBedPlayer::load_rally_from_disc(const std::filesystem::path& rally_root_or_state_root,
                                            const std::filesystem::path& cache_dir) {
  stop_rally_worker();
  reclaim_retired();
  const auto amb_dir = find_colorado_dirt_amb_dir(rally_root_or_state_root);
  if (amb_dir.empty()) return 0;

  struct RallyJob {
    int region_idx = 0;
    std::string region_name;
    std::filesystem::path bank;
    uint32_t sub = 0;
  };

  std::error_code ec;
  std::vector<std::filesystem::path> fevs;
  std::vector<std::filesystem::path> fsbs;
  for (const auto& e : std::filesystem::directory_iterator(amb_dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string fn = lower_str(e.path().filename().string());
    const std::string ext = lower_str(e.path().extension().string());
    if (fn.rfind("amb_", 0) != 0) continue;
    if (ext == ".fev") fevs.push_back(e.path());
    else if (ext == ".fsb") fsbs.push_back(e.path());
  }
  auto by_lower_name = [](const std::filesystem::path& a, const std::filesystem::path& b) {
    return lower_str(a.filename().string()) < lower_str(b.filename().string());
  };
  std::sort(fevs.begin(), fevs.end(), by_lower_name);
  std::sort(fsbs.begin(), fsbs.end(), by_lower_name);

  std::vector<RallyJob> jobs;
  std::vector<std::string> seen_stems;
  auto stem_seen = [&](const std::string& s) {
    return std::find(seen_stems.begin(), seen_stems.end(), s) != seen_stems.end();
  };

  for (const auto& fev_path : fevs) {
    if (int(jobs.size()) >= kMaxRallyRegions) break;
    const std::string stem = lower_str(fev_path.stem().string());
    FevProject proj;
    std::filesystem::path bank;
    uint32_t sub = 0;
    bool resolved = false;
    if (load_fev(fev_path, proj, nullptr)) {
      for (const auto& ev : proj.events) {
        if (pick_first_fev_sample(proj, ev, bank, sub) && std::filesystem::exists(bank, ec)) {
          resolved = true;
          break;
        }
      }
    }
    if (!resolved) {
      auto sib = fev_path;
      sib.replace_extension(".fsb");
      if (std::filesystem::exists(sib, ec)) {
        bank = sib;
        sub = 0;
        resolved = true;
      }
    }
    if (resolved) {
      seen_stems.push_back(stem);
      jobs.push_back({int(jobs.size()), normalize_region_token(stem), bank, sub});
    }
  }
  for (const auto& fsb_path : fsbs) {
    if (int(jobs.size()) >= kMaxRallyRegions) break;
    const std::string stem = lower_str(fsb_path.stem().string());
    if (stem_seen(stem)) continue;
    seen_stems.push_back(stem);
    jobs.push_back({int(jobs.size()), normalize_region_token(stem), fsb_path, 0});
  }

  const int queued = int(jobs.size());
  if (jobs.empty()) return 0;

  std::lock_guard<std::mutex> lk(lifecycle_mu_);
  stop_rally_worker_.store(true, std::memory_order_release);
  if (rally_worker_.joinable()) rally_worker_.join();
  stop_rally_worker_.store(false, std::memory_order_release);
  rally_worker_ = std::thread([this, jobs = std::move(jobs), cache_dir]() {
    const std::filesystem::path ffmpeg = has_inprocess_xma2_decoder() ? std::filesystem::path{} : find_ffmpeg();
    std::error_code tec;
    const std::filesystem::path amb_cache = cache_dir.empty() ? std::filesystem::path{} : (cache_dir / "ambience_rally");
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path(tec) /
        ("shift-rally-amb-decode-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(temp_dir, tec);
    for (const auto& j : jobs) {
      if (stop_rally_worker_.load(std::memory_order_acquire)) break;
      const std::string fname = lower_str(j.bank.stem().string()) + "." + std::to_string(j.sub) + ".s16";
      const std::filesystem::path cf = amb_cache.empty() ? std::filesystem::path{} : (amb_cache / fname);
      auto pcm = std::make_unique<PcmSample>();
      bool ok = !cf.empty() && read_cached_amb(cf, *pcm);
      if (ok && pcm->data.size() >= 1024 && std::fabs(pcm->data.back() - pcm->data.front()) > 0.02f) {
        make_seamless_loop(*pcm);
      }
      if (!ok) {
        ok = decode_amb_subsound(j.bank, j.sub, ffmpeg, temp_dir, *pcm);
        if (ok && !cf.empty()) write_cached_amb(cf, *pcm);
      }
      if (ok && !pcm->data.empty()) {
        LoadedRallyBed item{};
        item.region_idx = j.region_idx;
        const size_t n = std::min(j.region_name.size(), item.name.size() - 1);
        if (n > 0) std::memcpy(item.name.data(), j.region_name.data(), n);
        item.name[n] = '\0';
        item.pcm = pcm.release();
        if (!rally_loaded_q_.push(item)) delete item.pcm;
      }
    }
    std::filesystem::remove_all(temp_dir, tec);
  });
  return queued;
}

float AmbienceBedPlayer::event_level(const EventTable& t, int proj, const FevEvent& ev,
                                     const std::array<int8_t, 12>& pid, const AmbienceParams& params, float distance,
                                     FevInstanceLevel* out, int& count) const {
  (void)t;
  (void)proj;
  float raw[12];
  const size_t np = std::min(ev.params.size(), size_t(12));
  for (size_t i = 0; i < np; ++i) {
    float x = pid[i] == AmbienceParams::kDistance ? distance : params.value(pid[i]);
    if (!std::isfinite(x)) x = ev.params[i].min_val;  // no source: the parameter's minimum
    raw[i] = x;
  }
  count = evaluate_fev_event(ev, raw, np, out, kMaxEventLevels);
  float sum = 0.0f;
  for (int k = 0; k < count; ++k) sum += out[k].gain;
  return sum;
}

bool AmbienceBedPlayer::render_event_emitter(const EventTable& t, const RtAmbientEmitter& em, EmitterVoice3D& v,
                                             const AmbienceParams& params, float* out_stereo, int frames) {
  const EventTable::Ref* ref = em.active ? t.find(em.event_hash) : nullptr;
  FevInstanceLevel lv[kMaxEventLevels];
  int n = 0;
  const FevProject* pj = ref ? &t.projects[ref->proj] : nullptr;
  if (pj) event_level(t, ref->proj, pj->events[ref->ev], ref->param_id, params, em.distance_m, lv, n);
  if (em.active && em.id_hash != v.id_hash) {
    v.id_hash = em.id_hash;
    v.ev_gain.fill(0.0f);
    for (size_t k = 0; k < v.ev_phase.size(); ++k) v.ev_phase[k] = double(em.id_hash & 0x3FFFu) * 3.0 + double(k) * 7919.0;
  }
  // Gains and pan follow the event at the block rate (~60 ms smoothing).
  const float a = 1.0f - std::exp(-float(frames) / (0.06f * float(rate_)));
  if (em.active) v.pan += (std::clamp(em.pan, -1.0f, 1.0f) - v.pan) * a;
  const float pan_l = std::sqrt(0.5f * (1.0f - v.pan));
  const float pan_r = std::sqrt(0.5f * (1.0f + v.pan));
  float total = 0.0f;
  for (int k = 0; k < kMaxEventLevels; ++k) {
    const PcmSample* pcm = nullptr;
    float target = 0.0f;
    if (pj && k < n) {
      const int id = t.sd_base[ref->proj] + int(lv[k].sound_def);
      if (id >= 0 && id < kMaxEventSamples) pcm = event_pcm_[size_t(id)];
      const FevSoundDefTemplate* tpl =
          lv[k].sound_def < pj->sound_defs.size() ? pj->template_of(pj->sound_defs[lv[k].sound_def]) : nullptr;
      if (pcm && !(tpl && tpl->spawned())) target = lv[k].gain * (tpl ? tpl->volume : 1.0f);
    }
    float& g = v.ev_gain[size_t(k)];
    if (!pcm || pcm->data.empty() || (target < 1e-5f && g < 1e-5f)) {
      g = 0.0f;
      continue;
    }
    const float g0 = g, g1 = g + (target - g) * a;
    const size_t len = pcm->data.size();
    const double step = double(std::max(1, pcm->rate)) / double(rate_);
    double ph = std::fmod(v.ev_phase[size_t(k)], double(len));
    if (ph < 0.0) ph += double(len);
    for (int i = 0; i < frames; ++i) {
      const float gi = g0 + (g1 - g0) * (float(i + 1) / float(frames));
      const size_t s0 = size_t(ph) % len;
      const float frac = float(ph - std::floor(ph));
      const float x = (pcm->data[s0] + (pcm->data[(s0 + 1) % len] - pcm->data[s0]) * frac) * gi;
      out_stereo[2 * i] += x * pan_l;
      out_stereo[2 * i + 1] += x * pan_r;
      ph += step;
      if (ph >= double(len)) ph = std::fmod(ph, double(len));
    }
    v.ev_phase[size_t(k)] = ph;
    g = g1;
    total += g1;
  }
  v.gain = total;
  return true;
}

void AmbienceBedPlayer::render_3d_emitters(const RtAcousticsSample& sample, float* out_stereo, int frames) {
  if (!out_stereo || frames <= 0) return;
  const float sm = 1.0f - std::exp(-1.0f / (0.06f * float(rate_)));
  const EventTable* tbl = events_.load(std::memory_order_acquire);
  for (size_t vi = 0; vi < emitter_voices_.size(); ++vi) {
    const auto& em = sample.active_emitters[vi];
    auto& v = emitter_voices_[vi];
    // With the disc's events an emitter plays its own event (no stand-in
    // when it names one the disc lacks).
    if (tbl) {
      render_event_emitter(*tbl, em, v, params_, out_stereo, frames);
      continue;
    }
    const float target_g = em.active ? std::clamp(em.gain, 0.0f, 1.0f) : 0.0f;
    const float target_pan = em.active ? std::clamp(em.pan, -1.0f, 1.0f) : v.pan;
    if (em.active && em.id_hash != v.id_hash) {
      v.id_hash = em.id_hash;
      v.phase = double((em.id_hash & 0x3FFFu)) * 3.0;
    }
    if (target_g < 1e-4f && v.gain < 1e-4f) {
      v.gain = 0.0f;
      continue;
    }
    const PcmSample* pcm = nullptr;
    float base_scale = 0.055f;
    float lp_a = 0.08f;
    float bp_a = 0.012f;
    if (em.kind == AmbientEmitterKind::kCrowd) {
      pcm = beds_[size_t(AmbienceBedSlot::kCrowd)];
      base_scale = pcm ? 0.14f : 0.045f;
      lp_a = 0.10f;
      bp_a = 0.018f;
    } else if (em.kind == AmbientEmitterKind::kFestival) {
      pcm = beds_[size_t(AmbienceBedSlot::kFestival)];
      if (!pcm) pcm = beds_[size_t(AmbienceBedSlot::kCrowd)];
      base_scale = pcm ? 0.14f : 0.050f;
      lp_a = 0.12f;
      bp_a = 0.015f;
    } else if (em.kind == AmbientEmitterKind::kWater) {
      base_scale = 0.075f;
      lp_a = 0.16f;
      bp_a = 0.025f;
    } else if (em.kind == AmbientEmitterKind::kTrees) {
      base_scale = 0.040f;
      lp_a = 0.07f;
      bp_a = 0.010f;
    }

    if (pcm && !pcm->data.empty()) {
      const size_t n = pcm->data.size();
      const double step = double(std::max(1, pcm->rate)) / double(rate_);
      double ph = std::fmod(v.phase, double(n));
      if (ph < 0.0) ph += double(n);
      for (int i = 0; i < frames; ++i) {
        v.gain += (target_g - v.gain) * sm;
        v.pan += (target_pan - v.pan) * sm;
        const float pan_l = std::sqrt(0.5f * (1.0f - v.pan));
        const float pan_r = std::sqrt(0.5f * (1.0f + v.pan));
        const size_t k = size_t(ph) % n;
        const float frac = float(ph - std::floor(ph));
        const float s = (pcm->data[k] + (pcm->data[(k + 1) % n] - pcm->data[k]) * frac) * v.gain * base_scale;
        out_stereo[2 * i] += s * pan_l;
        out_stereo[2 * i + 1] += s * pan_r;
        ph += step;
        if (ph >= double(n)) ph = std::fmod(ph, double(n));
      }
      v.phase = ph;
    } else {
      for (int i = 0; i < frames; ++i) {
        v.gain += (target_g - v.gain) * sm;
        v.pan += (target_pan - v.pan) * sm;
        const float pan_l = std::sqrt(0.5f * (1.0f - v.pan));
        const float pan_r = std::sqrt(0.5f * (1.0f + v.pan));
        v.rng ^= v.rng << 13;
        v.rng ^= v.rng >> 17;
        v.rng ^= v.rng << 5;
        const float n = float(int32_t(v.rng)) * (1.0f / 2147483648.0f);
        v.lp += (n - v.lp) * lp_a;
        v.bp += (v.lp - v.bp) * bp_a;
        const float s = (v.lp - v.bp) * v.gain * base_scale;
        out_stereo[2 * i] += s * pan_l;
        out_stereo[2 * i + 1] += s * pan_r;
      }
    }
  }
}

void AmbienceBedPlayer::render(const AcousticsSample& sample, float speed_mph, float* out_stereo, int frames,
                               float tod_hour) {
  render(RtAcousticsSample::from_sample(sample), speed_mph, out_stereo, frames, tod_hour);
}

void AmbienceBedPlayer::render(const RtAcousticsSample& sample, float speed_mph, float* out_stereo, int frames,
                               float tod_hour) {
  AmbienceParams p;
  float h = std::fmod(tod_hour, 24.0f);
  if (!(h >= 0.0f)) h = std::isfinite(h) ? h + 24.0f : 12.0f;
  p.current_time_s = h * 3600.0f;
  p.car_speed_mph = p.speed = speed_mph;
  render(sample, p, out_stereo, frames);
}

void AmbienceBedPlayer::render(const RtAcousticsSample& sample, const AmbienceParams& params, float* out_stereo,
                               int frames) {
  if (!out_stereo || frames <= 0) return;
  drain_loaded();
  params_ = params;
  const float speed_mph = std::isfinite(params.car_speed_mph) ? params.car_speed_mph : 0.0f;
  const EventTable* tbl = events_.load(std::memory_order_acquire);
  // A slot's gain from the event its bed came from (its CarSpeed curve, at
  // the parameters; the bed has no position, so (distance) reads its minimum).
  auto slot_gain = [&](int s, float* out) -> bool {
    if (!tbl) return false;
    const auto& sr = tbl->slots[size_t(s)];
    if (sr.proj < 0 || sr.layer < 0) return false;  // no event, or a simple one (no layer curves)
    const FevProject& pj = tbl->projects[size_t(sr.proj)];
    const FevEvent& ev = pj.events[size_t(sr.ev)];
    FevInstanceLevel lv[kMaxEventLevels];
    int n = 0;
    event_level(*tbl, sr.proj, ev, tbl->slot_param_id[size_t(s)], params, 0.0f, lv, n);
    float g = 0.0f;
    for (int k = 0; k < n; ++k) {
      if (int(lv[k].layer) != sr.layer || int(lv[k].instance) != sr.inst) continue;
      const FevSoundDefTemplate* tpl =
          lv[k].sound_def < pj.sound_defs.size() ? pj.template_of(pj.sound_defs[lv[k].sound_def]) : nullptr;
      g = lv[k].gain * (tpl ? tpl->volume : 1.0f);
    }
    *out = g;
    return true;
  };

  // Compute smooth night factor [0..1] from 24h ToD clock (dusk 19:30..21:00, dawn 05:00..06:30).
  // (Only the no-disc fallback uses it; the disc's events carry CurrentTime.)
  float h = std::fmod(params.current_time_s / 3600.0f, 24.0f);
  if (!std::isfinite(h)) h = 12.0f;
  if (h < 0.0f) h += 24.0f;
  float night_amt = 0.0f;
  if (h >= 21.0f || h <= 5.0f) night_amt = 1.0f;
  else if (h > 19.5f) night_amt = (h - 19.5f) / 1.5f;
  else if (h < 6.5f) night_amt = (6.5f - h) / 1.5f;

  // Background ambient stream fades out smoothly above ~35-40 mph (colorado_ambience.xml).
  const float speed_target =
      speed_mph <= 35.0f ? 1.0f : speed_mph >= 55.0f ? 0.15f : 1.0f - 0.85f * ((speed_mph - 35.0f) / 20.0f);
  // Festival energy rises slightly at night; open countryside breeze calms slightly after dusk.
  const float fest_target =
      std::max(sample.festival_proximity, sample.crowd_proximity) * (tbl ? 1.0f : 1.0f + 0.15f * night_amt);
  const float nature_tod_scale = tbl ? 1.0f : 1.0f - 0.25f * night_amt;
  const float sm = 1.0f - std::exp(-1.0f / (0.15f * float(rate_)));
  const std::string_view snap_view = sample.ambience_snapshot_view();

  // ColoradoDirt Rally world: select among the 13 regional AMB_*.fev/.fsb beds without heap allocation.
  const bool use_rally_beds = rally_world_.load(std::memory_order_acquire) || sample.is_rally_world;
  if (use_rally_beds) {
    int first_loaded = -1;
    int loaded_indices[kMaxRallyRegions]{};
    int loaded_count = 0;
    for (int i = 0; i < kMaxRallyRegions; ++i) {
      if (rally_beds_[size_t(i)] && !rally_beds_[size_t(i)]->data.empty()) {
        if (first_loaded < 0) first_loaded = i;
        loaded_indices[loaded_count++] = i;
      }
    }
    if (loaded_count > 0) {
      int active_r = -1;
      char want_buf[48]{};
      size_t want_len = 0;
      for (char c : snap_view) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) && want_len + 1 < sizeof(want_buf)) {
          want_buf[want_len++] = char(std::tolower(u));
        }
      }
      std::string_view want_tok(want_buf, want_len);
      if (want_tok.size() > 3 && want_tok.substr(0, 3) == "amb") want_tok.remove_prefix(3);
      if (!want_tok.empty()) {
        for (int i = 0; i < kMaxRallyRegions; ++i) {
          if (!rally_beds_[size_t(i)] || rally_beds_[size_t(i)]->data.empty()) continue;
          const std::string_view rname(rally_names_[size_t(i)].data());
          if (!rname.empty() && (rname == want_tok || rname.find(want_tok) != std::string_view::npos ||
                                 want_tok.find(rname) != std::string_view::npos)) {
            active_r = i;
            break;
          }
        }
      }
      if (active_r < 0 && sample.ambience_template_idx >= 0) {
        const int direct_idx = sample.ambience_template_idx;
        if (direct_idx < kMaxRallyRegions && rally_beds_[size_t(direct_idx)] &&
            !rally_beds_[size_t(direct_idx)]->data.empty()) {
          active_r = direct_idx;
        } else {
          active_r = loaded_indices[direct_idx % loaded_count];
        }
      }
      if (active_r < 0) active_r = first_loaded;
      last_rally_region_.store(active_r, std::memory_order_release);

      for (int i = 0; i < kMaxRallyRegions; ++i) {
        const PcmSample* pcm = rally_beds_[size_t(i)];
        if (!pcm || pcm->data.empty()) {
          rally_gain_[size_t(i)] = 0.0f;
          continue;
        }
        const float target = (i == active_r) ? (0.20f * speed_target * nature_tod_scale) : 0.0f;
        if (target < 1e-4f && rally_gain_[size_t(i)] < 1e-4f) {
          rally_gain_[size_t(i)] = 0.0f;
          continue;
        }
        const size_t n = pcm->data.size();
        const double step = double(std::max(1, pcm->rate)) / double(rate_);
        const size_t r_off = n > 1024 ? size_t(std::min<size_t>(n / 3, size_t(0.013 * pcm->rate))) : 0;
        double ph = rally_phase_[size_t(i)];
        float g = rally_gain_[size_t(i)];
        for (int f = 0; f < frames; ++f) {
          g += (target - g) * sm;
          const size_t k = size_t(ph);
          const float frac = float(ph - double(k));
          const float sl = pcm->data[k % n] + (pcm->data[(k + 1) % n] - pcm->data[k % n]) * frac;
          const size_t kr = (k + r_off) % n;
          const float sr = pcm->data[kr] + (pcm->data[(kr + 1) % n] - pcm->data[kr]) * frac;
          out_stereo[2 * f] += sl * g;
          out_stereo[2 * f + 1] += sr * g;
          ph += step;
          if (ph >= double(n)) ph = std::fmod(ph, double(n));
        }
        rally_phase_[size_t(i)] = ph;
        rally_gain_[size_t(i)] = g;
      }
      // Mix crowd bed on top if near rally spectators.
      const PcmSample* crowd_pcm = beds_[size_t(AmbienceBedSlot::kCrowd)];
      if (crowd_pcm && !crowd_pcm->data.empty() && fest_target > 1e-4f) {
        const size_t n = crowd_pcm->data.size();
        const double step = double(std::max(1, crowd_pcm->rate)) / double(rate_);
        const size_t r_off = n > 1024 ? size_t(std::min<size_t>(n / 3, size_t(0.013 * crowd_pcm->rate))) : 0;
        double ph = bed_phase_[size_t(AmbienceBedSlot::kCrowd)];
        float g = bed_gain_[size_t(AmbienceBedSlot::kCrowd)];
        const float target = 0.28f * fest_target;
        for (int f = 0; f < frames; ++f) {
          g += (target - g) * sm;
          const size_t k = size_t(ph);
          const float frac = float(ph - double(k));
          const float sl = crowd_pcm->data[k % n] + (crowd_pcm->data[(k + 1) % n] - crowd_pcm->data[k % n]) * frac;
          const size_t kr = (k + r_off) % n;
          const float sr = crowd_pcm->data[kr] + (crowd_pcm->data[(kr + 1) % n] - crowd_pcm->data[kr]) * frac;
          out_stereo[2 * f] += sl * g;
          out_stereo[2 * f + 1] += sr * g;
          ph += step;
          if (ph >= double(n)) ph = std::fmod(ph, double(n));
        }
        bed_phase_[size_t(AmbienceBedSlot::kCrowd)] = ph;
        bed_gain_[size_t(AmbienceBedSlot::kCrowd)] = g;
      }
      render_3d_emitters(sample, out_stereo, frames);
      return;
    }
  } else {
    rally_gain_.fill(0.0f);
  }

  // Pick regional bed slot from active ambience snapshot.
  AmbienceBedSlot region = AmbienceBedSlot::kDefault;
  if (snap_view == "Mountains") region = AmbienceBedSlot::kMountains;
  else if (snap_view == "RedRock") region = AmbienceBedSlot::kRedRock;
  else if (snap_view == "MainTown") region = AmbienceBedSlot::kMainTown;
  else if (snap_view == "Festival") region = AmbienceBedSlot::kFestival;

  const PcmSample* active_reg_pcm = beds_[size_t(region)];
  if (!active_reg_pcm || active_reg_pcm->data.empty()) {
    region = AmbienceBedSlot::kDefault;
    active_reg_pcm = beds_[size_t(AmbienceBedSlot::kDefault)];
  }
  const bool have_reg_pcm = active_reg_pcm && !active_reg_pcm->data.empty();
  const PcmSample* crowd_pcm = beds_[size_t(AmbienceBedSlot::kCrowd)];
  const bool have_crowd_pcm = crowd_pcm && !crowd_pcm->data.empty();

  // Mix any loaded regional/crowd PCM beds with smooth crossfading.
  if (have_reg_pcm || have_crowd_pcm) {
    for (int s = 0; s < int(AmbienceBedSlot::kCount); ++s) {
      const PcmSample* pcm = beds_[size_t(s)];
      if (!pcm || pcm->data.empty()) {
        bed_gain_[size_t(s)] = 0.0f;
        continue;
      }
      float target = (s == int(AmbienceBedSlot::kCrowd))
                         ? (0.28f * fest_target)
                         : ((s == int(region)) ? (0.18f * speed_target * nature_tod_scale) : 0.0f);
      if (tbl) {
        // The disc's events: the region's quad bed at its event's level, and
        // no global crowd bed (each crowd emitter plays its own event).
        float eg = 0.0f;
        if (s == int(AmbienceBedSlot::kCrowd)) target = 0.0f;
        else if (s == int(region)) target = slot_gain(s, &eg) ? eg : target;
        else target = 0.0f;
      }
      if (target < 1e-4f && bed_gain_[size_t(s)] < 1e-4f) {
        bed_gain_[size_t(s)] = 0.0f;
        continue;
      }
      const size_t n = pcm->data.size();
      const double step = double(std::max(1, pcm->rate)) / double(rate_);
      // Decorrelate stereo slightly by reading R channel offset by ~13 ms within the loop.
      const size_t r_off = n > 1024 ? size_t(std::min<size_t>(n / 3, size_t(0.013 * pcm->rate))) : 0;
      double ph = bed_phase_[size_t(s)];
      float g = bed_gain_[size_t(s)];
      for (int i = 0; i < frames; ++i) {
        g += (target - g) * sm;
        const size_t k = size_t(ph);
        const float frac = float(ph - double(k));
        const float sl = pcm->data[k % n] + (pcm->data[(k + 1) % n] - pcm->data[k % n]) * frac;
        const size_t kr = (k + r_off) % n;
        const float sr = pcm->data[kr] + (pcm->data[(kr + 1) % n] - pcm->data[kr]) * frac;
        out_stereo[2 * i] += sl * g;
        out_stereo[2 * i + 1] += sr * g;
        ph += step;
        if (ph >= double(n)) ph = std::fmod(ph, double(n));
      }
      bed_phase_[size_t(s)] = ph;
      bed_gain_[size_t(s)] = g;
    }
  }

  // Only synthesize procedural fallback for layers missing real disc PCM beds.
  if (have_reg_pcm && (have_crowd_pcm || tbl)) {
    render_3d_emitters(sample, out_stereo, frames);
    return;
  }

  // Regional filter cutoff character (Mountains: brisk wind, RedRock: warm desert, Festival: crowd murmur).
  float lp_a = 0.04f;
  if (snap_view == "Mountains") lp_a = 0.07f;
  else if (snap_view == "RedRock") lp_a = 0.03f;
  else if (snap_view == "MainTown") lp_a = 0.055f;
  else if (snap_view == "Festival") lp_a = 0.09f;

  for (int i = 0; i < frames; ++i) {
    env_speed_ += (speed_target - env_speed_) * sm;
    env_festival_ += (fest_target - env_festival_) * sm;
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    const float nl = float(int32_t(rng_)) * (1.0f / 2147483648.0f);
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    const float nr = float(int32_t(rng_)) * (1.0f / 2147483648.0f);

    lp_l_ += (nl - lp_l_) * lp_a;
    lp_r_ += (nr - lp_r_) * lp_a;
    bp_l_ += (lp_l_ - bp_l_) * 0.008f;
    bp_r_ += (lp_r_ - bp_r_) * 0.008f;

    phase_ += 2.0 * 3.14159265 * 0.18 / double(rate_);
    const float swell = 0.85f + 0.15f * std::sin(float(phase_));
    const float nature_gain = have_reg_pcm ? 0.0f : (0.018f * env_speed_ * swell);
    const float crowd_gain = (have_crowd_pcm || tbl) ? 0.0f : (0.045f * env_festival_);

    out_stereo[2 * i] += (lp_l_ - bp_l_) * nature_gain + lp_l_ * crowd_gain;
    out_stereo[2 * i + 1] += (lp_r_ - bp_r_) * nature_gain + lp_r_ * crowd_gain;
  }
  render_3d_emitters(sample, out_stereo, frames);
}

}  // namespace audio
