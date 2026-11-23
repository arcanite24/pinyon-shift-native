#include "audio/world_acoustics.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>

#include "audio/bake.h"
#include "audio/fev.h"
#include "audio/fsb4.h"
#include "audio/stream.h"
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
                            const std::array<float, 3>& b) {
  const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
  const float len2 = abx * abx + aby * aby + abz * abz;
  float t = 0.0f;
  if (len2 > 1e-4f) {
    t = std::clamp(((px - a[0]) * abx + (py - a[1]) * aby + (pz - a[2]) * abz) / len2, 0.0f, 1.0f);
  }
  const float cx = a[0] + abx * t - px;
  const float cy = a[1] + aby * t - py;
  const float cz = a[2] + abz * t - pz;
  return std::sqrt(cx * cx + cy * cy + cz * cz);
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
  const auto rev_path = cars::mods::resolve(game_root / "media" / "tracks" / "colorado" / "colorado_reverb.xml");
  const auto amb_path = cars::mods::resolve(game_root / "media" / "tracks" / "colorado" / "colorado_ambience.xml");
  const auto rib_path =
      cars::mods::resolve(game_root / "media" / "audio" / "tracks" / "colorado" / "Ribbon_00" / "ColoradoRibbon_00.xml");

  bool ok = false;
  if (std::filesystem::exists(rev_path, ec)) ok |= parse_reverb_xml(read_text(rev_path), reverb_templates_);
  if (std::filesystem::exists(amb_path, ec)) ok |= parse_ambience_xml(read_text(amb_path), ambience_templates_);
  if (std::filesystem::exists(rib_path, ec)) {
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

int WorldAcousticsData::load_soundscapes_from_archive(const std::filesystem::path& bin_zip, int max_tiles) {
  std::error_code ec;
  if (bin_zip.empty() || !std::filesystem::exists(bin_zip, ec)) return 0;
  auto arc = fh1::Archive::open(bin_zip, nullptr);
  if (!arc) return 0;
  int loaded = 0;
  for (const auto& m : arc->members()) {
    const std::string low = lower_str(m.name);
    if (low.size() < 11 || low.compare(low.size() - 11, 11, ".soundscape") != 0) continue;
    auto rd = arc->read(m.name);
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

void WorldAcousticsData::add_tile(SoundscapeTile tile) {
  for (auto& rz : tile.reverb_zones) reverb_zones_.push_back(std::move(rz));
  for (auto& az : tile.ambience_zones) ambience_zones_.push_back(std::move(az));
  for (auto& em : tile.emitters) {
    const std::string low = lower_str(em.event_name);
    if (low.find("festival") != std::string::npos || low.find("crowd") != std::string::npos ||
        low.find("stage") != std::string::npos || low.find("water") != std::string::npos) {
      emitters_.push_back(std::move(em));
    }
  }
}

AcousticsSample WorldAcousticsData::evaluate(float x, float y, float z) const {
  AcousticsSample out;
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

  // Evaluate festival / crowd proximity from ribbon & tile 3D emitters.
  for (const auto& em : emitters_) {
    const float d = dist_point_to_segment(x, y, z, em.pos1, em.pos2);
    if (d >= em.max_distance) continue;
    const float prox = std::clamp(1.0f - d / std::max(1.0f, em.max_distance), 0.0f, 1.0f);
    const std::string low = lower_str(em.event_name + " " + em.object_name);
    if (low.find("festival") != std::string::npos || low.find("stage") != std::string::npos) {
      out.festival_proximity = std::max(out.festival_proximity, prox);
    }
    if (low.find("crowd") != std::string::npos) {
      out.crowd_proximity = std::max(out.crowd_proximity, prox);
    }
  }
  if (out.festival_proximity > 0.35f) out.ambience_snapshot = "Festival";
  return out;
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
  const float out = -in + bufout;
  buf[idx] = in + bufout * feedback;
  if (++idx >= buf.size()) idx = 0;
  return out;
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
}

void EnvironmentalReverb::set_parameters(const ReverbTemplate& tmpl) {
  decay_time_ = std::clamp(tmpl.decay_time_s, 0.15f, 6.0f);
  // Compute room + reverb wet scale from I3DL2 millibels and NearWet.
  const float room_lin = std::pow(10.0f, std::clamp(tmpl.room_level_mb, -6000.0f, 0.0f) / 2000.0f);
  const float rev_lin = std::pow(10.0f, std::clamp(tmpl.reverb_level_mb, -3000.0f, 1500.0f) / 2000.0f);
  target_wet_ = std::clamp(tmpl.near_wet * room_lin * std::min(rev_lin, 1.5f) * 0.45f, 0.0f, 0.65f);
  target_dry_ = std::clamp(tmpl.near_dry, 0.2f, 1.0f);

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
  if (target_wet_ < 1e-4f && wet_smooth_ < 1e-4f) return;

  const float sm = 1.0f - std::exp(-1.0f / (0.04f * float(rate_)));
  const size_t pre_cap = predelay_l_.size();
  for (int i = 0; i < frames; ++i) {
    wet_smooth_ += (target_wet_ - wet_smooth_) * sm;
    dry_smooth_ += (target_dry_ - dry_smooth_) * sm;
    const float in_l = io[2 * i];
    const float in_r = io[2 * i + 1];

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
    io[2 * i] = in_l * dry_smooth_ + wl * wet_smooth_;
    io[2 * i + 1] = in_r * dry_smooth_ + wr * wet_smooth_;
  }
}

bool EnvironmentalReverb::process_wet(const float* in_stereo, float* out_wet_stereo, int frames) {
  if (!in_stereo || !out_wet_stereo || frames <= 0 || comb_l_[0].buf.empty()) return false;
  if (target_wet_ < 1e-4f && wet_smooth_ < 1e-4f) {
    std::fill(out_wet_stereo, out_wet_stereo + size_t(frames) * 2, 0.0f);
    return false;
  }

  const float sm = 1.0f - std::exp(-1.0f / (0.04f * float(rate_)));
  const size_t pre_cap = predelay_l_.size();
  for (int i = 0; i < frames; ++i) {
    wet_smooth_ += (target_wet_ - wet_smooth_) * sm;
    dry_smooth_ += (target_dry_ - dry_smooth_) * sm;
    const float in_l = in_stereo[2 * i];
    const float in_r = in_stereo[2 * i + 1];

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
    out_wet_stereo[2 * i] = wl * wet_smooth_;
    out_wet_stereo[2 * i + 1] = wr * wet_smooth_;
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
    if (ffmpeg.empty()) return false;
    const auto wave = xma2_wave(s, std::span<const uint8_t>(bank_data).subspan(size_t(s.offset), s.bytes));
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
    return !out.data.empty();
  }
  return false;
}

bool pick_first_fev_sample(const FevProject& proj, const FevEvent& ev, std::filesystem::path& bank_out,
                           uint32_t& sub_out) {
  auto pick = [&](const std::vector<FevSoundInstance>& insts) -> bool {
    for (const auto& si : insts) {
      if (si.sound_def_index >= proj.sound_defs.size()) continue;
      for (const auto& en : proj.sound_defs[si.sound_def_index].entries) {
        if (en.entry_type == 0 && !en.wavebank_name.empty()) {
          bank_out = cars::mods::resolve(proj.dir / (en.wavebank_name + ".fsb"));
          sub_out = en.subsound_index;
          return true;
        }
      }
    }
    return false;
  };
  if (pick(ev.simple_instances)) return true;
  for (const auto& ly : ev.layers)
    if (pick(ly.instances)) return true;
  return false;
}

}  // namespace

AmbienceBedPlayer::AmbienceBedPlayer(int rate) : rate_(std::max(8000, rate)) {
  beds_.fill(nullptr);
  bed_phase_.fill(0.0);
  bed_gain_.fill(0.0f);
}

AmbienceBedPlayer::~AmbienceBedPlayer() {
  stop_worker();
  drain_loaded();
  for (PcmSample*& p : beds_) {
    delete p;
    p = nullptr;
  }
}

void AmbienceBedPlayer::stop_worker() {
  stop_worker_.store(true, std::memory_order_release);
  if (worker_.joinable()) worker_.join();
}

void AmbienceBedPlayer::drain_loaded() {
  LoadedBed lb;
  while (loaded_q_.pop(lb)) {
    const size_t idx = size_t(lb.slot);
    if (idx < beds_.size()) {
      delete beds_[idx];
      beds_[idx] = lb.pcm;
    } else {
      delete lb.pcm;
    }
  }
}

void AmbienceBedPlayer::set_bed_sample(AmbienceBedSlot slot, PcmSample pcm) {
  const size_t idx = size_t(slot);
  if (idx >= beds_.size()) return;
  auto* p = new PcmSample(std::move(pcm));
  if (!loaded_q_.push({slot, p})) delete p;
}

int AmbienceBedPlayer::loaded_bed_count() const {
  int n = 0;
  for (const PcmSample* p : beds_)
    if (p && !p->data.empty()) ++n;
  return n;
}

int AmbienceBedPlayer::load_from_disc(const std::filesystem::path& game_root, const std::filesystem::path& cache_dir) {
  stop_worker();
  if (game_root.empty()) return 0;
  struct Job {
    AmbienceBedSlot slot;
    std::filesystem::path bank;
    uint32_t sub = 0;
  };
  std::vector<Job> jobs;
  std::array<bool, int(AmbienceBedSlot::kCount)> assigned{};
  auto assign_slot = [&](AmbienceBedSlot slot, const std::filesystem::path& bank, uint32_t sub) {
    if (assigned[size_t(slot)]) return;
    std::error_code ec;
    if (!std::filesystem::exists(bank, ec)) return;
    assigned[size_t(slot)] = true;
    jobs.push_back({slot, bank, sub});
  };

  const std::filesystem::path amb_dir = game_root / "media" / "audio" / "ambience";
  const char* kFevCandidates[] = {
      "AMB_Quads.fev",
      "AMB_Default.fev",
      "AMB_Festival_Ambience.fev",
      "Colorado_Festival.fev",
      "Colorado_Ambience.fev",
  };
  for (const char* fev_name : kFevCandidates) {
    const auto fev_path = cars::mods::resolve(amb_dir / fev_name);
    FevProject proj;
    if (!load_fev(fev_path, proj, nullptr)) continue;
    for (const auto& ev : proj.events) {
      std::filesystem::path bank;
      uint32_t sub = 0;
      if (!pick_first_fev_sample(proj, ev, bank, sub)) continue;
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
  }
  // Fallback: if AMB_Quads_Stream.fsb exists directly under ambience/, queue its subsounds.
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

  const int queued = int(jobs.size());
  if (jobs.empty()) return 0;

  stop_worker_.store(false, std::memory_order_release);
  worker_ = std::thread([this, jobs = std::move(jobs), cache_dir]() {
    const std::filesystem::path ffmpeg = find_ffmpeg();
    std::error_code tec;
    const std::filesystem::path amb_cache = cache_dir.empty() ? std::filesystem::path{} : (cache_dir / "ambience");
    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path(tec) /
        ("shift-amb-decode-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(temp_dir, tec);
    for (const auto& j : jobs) {
      if (stop_worker_.load(std::memory_order_acquire)) break;
      const std::filesystem::path cf =
          amb_cache.empty()
              ? std::filesystem::path{}
              : (amb_cache / (lower_str(j.bank.stem().string()) + "." + std::to_string(j.sub) + ".s16"));
      auto pcm = std::make_unique<PcmSample>();
      bool ok = !cf.empty() && read_cached_amb(cf, *pcm);
      if (!ok) {
        ok = decode_amb_subsound(j.bank, j.sub, ffmpeg, temp_dir, *pcm);
        if (ok && !cf.empty()) write_cached_amb(cf, *pcm);
      }
      if (ok && !pcm->data.empty()) {
        PcmSample* raw = pcm.release();
        if (!loaded_q_.push({j.slot, raw})) delete raw;
      }
    }
    std::filesystem::remove_all(temp_dir, tec);
  });
  return queued;
}

void AmbienceBedPlayer::render(const AcousticsSample& sample, float speed_mph, float* out_stereo, int frames) {
  if (!out_stereo || frames <= 0) return;
  drain_loaded();

  // Background ambient stream fades out smoothly above ~35-40 mph (colorado_ambience.xml).
  const float speed_target =
      speed_mph <= 35.0f ? 1.0f : speed_mph >= 55.0f ? 0.15f : 1.0f - 0.85f * ((speed_mph - 35.0f) / 20.0f);
  const float fest_target = std::max(sample.festival_proximity, sample.crowd_proximity);
  const float sm = 1.0f - std::exp(-1.0f / (0.15f * float(rate_)));

  // Pick regional bed slot from active ambience snapshot.
  AmbienceBedSlot region = AmbienceBedSlot::kDefault;
  if (sample.ambience_snapshot == "Mountains") region = AmbienceBedSlot::kMountains;
  else if (sample.ambience_snapshot == "RedRock") region = AmbienceBedSlot::kRedRock;
  else if (sample.ambience_snapshot == "MainTown") region = AmbienceBedSlot::kMainTown;
  else if (sample.ambience_snapshot == "Festival") region = AmbienceBedSlot::kFestival;

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
      const float target = (s == int(AmbienceBedSlot::kCrowd))
                               ? (0.28f * fest_target)
                               : ((s == int(region)) ? (0.18f * speed_target) : 0.0f);
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
  if (have_reg_pcm && have_crowd_pcm) return;

  // Regional filter cutoff character (Mountains: brisk wind, RedRock: warm desert, Festival: crowd murmur).
  float lp_a = 0.04f;
  if (sample.ambience_snapshot == "Mountains") lp_a = 0.07f;
  else if (sample.ambience_snapshot == "RedRock") lp_a = 0.03f;
  else if (sample.ambience_snapshot == "MainTown") lp_a = 0.055f;
  else if (sample.ambience_snapshot == "Festival") lp_a = 0.09f;

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
    const float crowd_gain = have_crowd_pcm ? 0.0f : (0.045f * env_festival_);

    out_stereo[2 * i] += (lp_l_ - bp_l_) * nature_gain + lp_l_ * crowd_gain;
    out_stereo[2 * i + 1] += (lp_r_ - bp_r_) * nature_gain + lp_r_ * crowd_gain;
  }
}

}  // namespace audio
