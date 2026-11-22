#include "audio/event_configs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

#include "audio/xml.h"
#include "cars/mods.h"

namespace audio {
namespace {

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string read_text(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string join_group_event(const XmlNode* n) {
  if (!n) return {};
  const std::string g = n->attr("Group");
  const std::string e = n->attr("Event");
  if (g.empty() || e.empty()) return {};
  return g + "/" + e;
}

Param read_param_node(const XmlNode* node) {
  Param p;
  if (!node) return p;
  const XmlNode* coeff = node->child("PhysicsCoeff");
  const XmlNode* curve = node->child("ThreePointCurve");
  if (!coeff || !curve) return p;
  p.present = true;
  auto c01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
  p.rpm = c01(coeff->number("RPM"));
  p.throttle = c01(coeff->number("Throttle"));
  p.pos_torque = c01(coeff->number("PosTorque"));
  p.neg_torque = c01(coeff->number("NegTorque"));
  for (int k = 0; k < 3; ++k) {
    const std::string i = std::to_string(k);
    p.x[size_t(k)] = c01(curve->number("x" + i, 0.5 * k));
    p.y[size_t(k)] = curve->number("y" + i, 1.0);
  }
  return p;
}

void read_rolloff(const XmlNode* section, bool replay, RolloffCurve& out) {
  if (!section) return;
  const XmlNode* node = section->child(replay ? "Replay" : "Gameplay");
  if (!node) node = section->child("Gameplay");
  if (!node) return;
  out.min_dist = float(node->number("minDist", out.min_dist));
  out.max_dist = float(node->number("maxDist", out.max_dist));
  out.begin_fade_to_zero_dist = float(node->number("beginFadeToZeroDist", out.begin_fade_to_zero_dist));
  out.fade_to_zero_dist = float(node->number("fadeToZeroDist", out.fade_to_zero_dist));
}

}  // namespace

// --------------------------------------------------------------- TiresConfig

bool TiresConfig::parse_tires(std::string_view xml, std::string* error) {
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  surfaces.clear();
  for (const auto& ch : root.children) {
    if (!iequals(ch.name, "SurfaceMap")) continue;
    TireSurfaceEntry e;
    e.physics_surface = ch.attr("PhysicsSurface");
    e.lod1_group = ch.attr("Lod1EventGroup");
    e.lod2_group = ch.attr("Lod2EventGroup");
    e.surface_index = int(ch.number("SurfaceIndex", 0));
    if (!e.physics_surface.empty()) surfaces.push_back(std::move(e));
  }
  return !surfaces.empty();
}

bool TiresConfig::parse_surface_changes(std::string_view xml, std::string* error) {
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  changes.clear();
  for (const auto& ch : root.children) {
    if (!iequals(ch.name, "Surfaces")) continue;
    SurfaceChangePair p;
    p.s1 = ch.attr("Surface1");
    p.s2 = ch.attr("Surface2");
    if (const XmlNode* a = ch.child("S1toS2")) p.s1_to_s2 = a->attr("Event");
    if (const XmlNode* b = ch.child("S2toS1")) p.s2_to_s1 = b->attr("Event");
    if (!p.s1.empty() && !p.s2.empty()) changes.push_back(std::move(p));
  }
  return !changes.empty();
}

bool TiresConfig::load(const std::filesystem::path& game_root, std::string* error) {
  const auto dir = game_root / "media" / "audio" / "cars" / "Tires";
  const std::string t = read_text(cars::mods::resolve(dir / "Tires.xml"));
  const std::string sc = read_text(cars::mods::resolve(dir / "SurfaceChanges.xml"));
  if (t.empty() || !parse_tires(t, error)) return false;
  if (!sc.empty()) parse_surface_changes(sc, nullptr);
  return true;
}

const TireSurfaceEntry* TiresConfig::find_surface(std::string_view physics_surface) const {
  for (const auto& s : surfaces)
    if (iequals(s.physics_surface, physics_surface)) return &s;
  return surfaces.empty() ? nullptr : &surfaces[0];
}

std::string TiresConfig::surface_change_event(std::string_view from_surface, std::string_view to_surface) const {
  if (iequals(from_surface, to_surface)) return {};
  for (const auto& c : changes) {
    if (iequals(c.s1, from_surface) && iequals(c.s2, to_surface)) return c.s1_to_s2;
    if (iequals(c.s2, from_surface) && iequals(c.s1, to_surface)) return c.s2_to_s1;
  }
  return {};
}

// ------------------------------------------------------ CollisionAudioConfig

bool CollisionAudioConfig::parse(std::string_view xml, std::string* error) {
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  ms_between_diff_collisions = root.number("MSBetweenDiffCollisions", 75.0);
  pairs.clear();
  for (const auto& ch : root.children) {
    const bool is_break = iequals(ch.name, "BreakEvent");
    if (!iequals(ch.name, "CollisionEvent") && !is_break) continue;
    CollisionEventSpec s;
    s.surface1 = ch.attr("Surface1");
    s.surface2 = ch.attr("Surface2");
    s.is_break = is_break;
    s.impact3d = join_group_event(ch.child("Impact3D"));
    s.impact_lfe = join_group_event(ch.child("ImpactLFE"));
    s.scrape3d = join_group_event(ch.child("Scrape3D"));
    s.whoosh3d = join_group_event(ch.child("Whoosh3D"));
    pairs.push_back(std::move(s));
  }
  return !pairs.empty();
}

bool CollisionAudioConfig::load(const std::filesystem::path& game_root, std::string* error) {
  const auto p = cars::mods::resolve(game_root / "media" / "audio" / "collisions" / "CollisionData.xml");
  const std::string t = read_text(p);
  if (t.empty()) {
    if (error) *error = "cannot read " + p.string();
    return false;
  }
  return parse(t, error);
}

const CollisionEventSpec* CollisionAudioConfig::find_pair(std::string_view s1, std::string_view s2) const {
  for (const auto& p : pairs) {
    if ((iequals(p.surface1, s1) && iequals(p.surface2, s2)) ||
        (iequals(p.surface1, s2) && iequals(p.surface2, s1)))
      return &p;
  }
  return nullptr;
}

// ---------------------------------------------------------------- UiAudioMap

bool UiAudioEntry::is_blank() const {
  return cue.empty() || iequals(cue, "Blank") || group.empty() || iequals(group, "UIInGame/UI/Blank");
}

std::string UiAudioEntry::full_path() const {
  if (is_blank()) return {};
  return group + "/" + cue;
}

bool UiAudioMap::parse(std::string_view xml, std::string* error) {
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  defaults.clear();
  overrides.clear();
  scene_overrides.clear();

  auto read_event_node = [](const XmlNode& n) -> UiAudioEntry {
    UiAudioEntry e;
    e.play = n.attr("play");
    // Tolerates the disc typo `ggroup` on VO_PopUp_OthersStorefront.
    e.group = n.has("group") ? n.attr("group") : n.attr("ggroup");
    // XmlNode::attr returns the first matching attribute (first-wins rule).
    e.cue = n.attr("cue");
    e.is_looping = iequals(n.attr("isLooping"), "true");
    return e;
  };

  if (const XmlNode* defs = root.child("defaults")) {
    for (const auto& ch : defs->children)
      if (iequals(ch.name, "event")) defaults.push_back(read_event_node(ch));
  }
  if (const XmlNode* ovs = root.child("overrides")) {
    for (const auto& ov : ovs->children) {
      if (!iequals(ov.name, "override")) continue;
      auto& list = overrides[lower_ascii(ov.attr("name"))];
      for (const auto& ch : ov.children)
        if (iequals(ch.name, "event")) list.push_back(read_event_node(ch));
    }
  }
  if (const XmlNode* sovs = root.child("scene_overrides")) {
    for (const auto& sc : sovs->children)
      if (iequals(sc.name, "scene")) scene_overrides[lower_ascii(sc.attr("name"))] = lower_ascii(sc.attr("override"));
  }
  return !defaults.empty();
}

bool UiAudioMap::load(const std::filesystem::path& game_root, std::string* error) {
  const auto p = cars::mods::resolve(game_root / "media" / "audio" / "UI" / "ui4audio.xml");
  const std::string t = read_text(p);
  if (t.empty()) {
    if (error) *error = "cannot read " + p.string();
    return false;
  }
  return parse(t, error);
}

int UiAudioMap::total_entries() const {
  int n = int(defaults.size());
  for (const auto& [k, list] : overrides) n += int(list.size());
  return n;
}

const UiAudioEntry* UiAudioMap::resolve(std::string_view play_name, std::string_view scene_name) const {
  if (!scene_name.empty()) {
    if (auto sit = scene_overrides.find(lower_ascii(scene_name)); sit != scene_overrides.end()) {
      if (auto oit = overrides.find(sit->second); oit != overrides.end()) {
        for (const auto& e : oit->second)
          if (iequals(e.play, play_name)) return &e;
      }
    }
  }
  for (const auto& e : defaults)
    if (iequals(e.play, play_name)) return &e;
  return nullptr;
}

// ------------------------------------------------------------ BackfireConfig

bool BackfireConfig::parse_timer(std::string_view xml, std::string* error) {
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  size_t idx = 0;
  for (const auto& ch : root.children) {
    if (!iequals(ch.name, "Backfire") || idx >= levels.size()) continue;
    if (const XmlNode* tm = ch.child("Timer")) {
      levels[idx].min_time_ms = tm->number("MinTime", 0);
      levels[idx].max_time_ms = tm->number("MaxTime", 0);
      levels[idx].min_norm_rpm = tm->number("MinNormRPM", 0);
      levels[idx].max_norm_rpm = tm->number("MaxNormRPM", 0);
      levels[idx].silence_weighting = tm->number("SilenceWeighting", 100);
    }
    ++idx;
  }
  return idx > 0;
}

bool BackfireConfig::parse_volume(std::string_view xml, std::string* error) {
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  size_t idx = 0;
  for (const auto& ch : root.children) {
    if (!iequals(ch.name, "Backfire") || idx >= levels.size()) continue;
    levels[idx].volume = read_param_node(ch.child("Volume"));
    ++idx;
  }
  return idx > 0;
}

bool BackfireConfig::load(const std::filesystem::path& game_root, std::string* error) {
  const auto dir = game_root / "media" / "audio";
  const std::string t = read_text(cars::mods::resolve(dir / "BackfireTimerConfig.xml"));
  const std::string v = read_text(cars::mods::resolve(dir / "BackfireVolumeConfig.xml"));
  if (t.empty() || !parse_timer(t, error)) return false;
  if (!v.empty()) parse_volume(v, nullptr);
  return true;
}

// ------------------------------------------------------------- RolloffConfig

float RolloffCurve::eval(float distance_m, float fov_scale) const {
  const float s = std::max(0.01f, fov_scale);
  const float d = std::max(0.0f, distance_m);
  const float p1 = min_dist * s;
  const float p2 = max_dist * s;
  const float p3 = begin_fade_to_zero_dist * s;
  const float p4 = fade_to_zero_dist * s;
  const float p5 = p4 + 100.0f;
  auto lerp_seg = [](float x, float x0, float y0, float x1, float y1) {
    const float span = x1 - x0;
    if (span <= 1e-6f) return y1;
    return y0 + (y1 - y0) * ((x - x0) / span);
  };
  if (d <= p1) return 1.0f;
  if (d <= p2) return lerp_seg(d, p1, 1.0f, p2, 0.35f);
  if (d <= p3) return lerp_seg(d, p2, 0.35f, p3, 0.10f);
  if (d <= p4) return lerp_seg(d, p3, 0.10f, p4, 0.05f);
  if (d <= p5) return lerp_seg(d, p4, 0.05f, p5, 0.0f);
  return 0.0f;
}

bool RolloffConfig::parse(std::string_view xml, bool replay, std::string* error) {
  XmlNode root;
  if (!parse_xml(xml, root, error)) return false;
  read_rolloff(root.child("IntakeChuff"), replay, intake_chuff);
  read_rolloff(root.child("Burble"), replay, burble);
  read_rolloff(root.child("Backfire"), replay, backfire);
  if (const XmlNode* ch = root.child("Chassis")) {
    read_rolloff(ch->child("EngineIntake"), replay, chassis_intake);
    read_rolloff(ch->child("EngineAmbient"), replay, chassis_ambient);
    read_rolloff(ch->child("ExhaustL"), replay, chassis_exhaust_l);
    read_rolloff(ch->child("ExhaustR"), replay, chassis_exhaust_r);
  }
  return true;
}

bool RolloffConfig::load(const std::filesystem::path& game_root, bool traffic, bool replay, std::string* error) {
  const auto p = cars::mods::resolve(game_root / "media" / "audio" /
                                     (traffic ? "GlobalRolloffsTraffic.xml" : "GlobalRolloffs.xml"));
  const std::string t = read_text(p);
  if (t.empty()) {
    if (error) *error = "cannot read " + p.string();
    return false;
  }
  return parse(t, replay, error);
}

float replay_fov_rolloff_scale(float fov_rad, float fov_ref_rad, float listener_scalar) {
  const float u = std::clamp((fov_rad - fov_ref_rad) * (-1.88679f), 0.0f, 1.0f);
  return 1.0f + u * listener_scalar;
}

Vec3f clamp_audio_position(Vec3f p) {
  return {std::clamp(p.x, -20000.0f, 20000.0f), std::clamp(p.y, -20000.0f, 20000.0f),
          std::clamp(p.z, -20000.0f, 20000.0f)};
}

Vec3f clamp_audio_velocity(Vec3f v) {
  return {std::clamp(v.x, -300.0f, 300.0f), std::clamp(v.y, -300.0f, 300.0f), std::clamp(v.z, -300.0f, 300.0f)};
}

float compute_doppler_pitch(Vec3f listener_pos, Vec3f listener_vel, Vec3f emitter_pos, Vec3f emitter_vel,
                            float doppler_factor) {
  if (doppler_factor <= 0.0f) return 1.0f;
  const Vec3f lp = clamp_audio_position(listener_pos);
  const Vec3f ep = clamp_audio_position(emitter_pos);
  const Vec3f lv = clamp_audio_velocity(listener_vel);
  const Vec3f ev = clamp_audio_velocity(emitter_vel);
  const float dx = ep.x - lp.x, dy = ep.y - lp.y, dz = ep.z - lp.z;
  const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (dist < 1e-3f) return 1.0f;
  const float nx = dx / dist, ny = dy / dist, nz = dz / dist;
  // Positive radial speed = moving away from listener.
  const float v_em = (ev.x * nx + ev.y * ny + ev.z * nz) * doppler_factor;
  const float v_ls = (lv.x * nx + lv.y * ny + lv.z * nz) * doppler_factor;
  constexpr float kSpeedOfSound = 340.0f;
  const float num = std::max(10.0f, kSpeedOfSound + v_ls);
  const float den = std::max(10.0f, kSpeedOfSound + v_em);
  return std::clamp(num / den, 0.5f, 2.0f);
}

void compute_stereo_pan(Vec3f listener_pos, Vec3f listener_right, Vec3f emitter_pos, float& left, float& right) {
  const Vec3f lp = clamp_audio_position(listener_pos);
  const Vec3f ep = clamp_audio_position(emitter_pos);
  const float dx = ep.x - lp.x, dy = ep.y - lp.y, dz = ep.z - lp.z;
  const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (dist < 1e-3f) {
    left = right = 0.70710678f;
    return;
  }
  const float pan = std::clamp((dx * listener_right.x + dy * listener_right.y + dz * listener_right.z) / dist,
                               -1.0f, 1.0f);
  const float angle = (pan + 1.0f) * 0.25f * 3.14159265f;  // 0..pi/2
  left = std::cos(angle);
  right = std::sin(angle);
}

BovVariant select_bov_variant(double turbo_psi) {
  if (turbo_psi > 13.0) return BovVariant::kHi;
  if (turbo_psi > 4.5) return BovVariant::kLo;
  return BovVariant::kNone;
}

}  // namespace audio
