#include "world/vfx.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "fh1data/archive.h"
#include "world/world.h"

namespace world {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string normalize_xds_name(std::string_view raw) {
  std::string s = to_lower(raw);
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  if (s.empty()) return {};
  if (const size_t slash = s.find_last_of("/\\"); slash != std::string::npos) {
    s = s.substr(slash + 1);
  }
  if (s.empty()) return {};
  for (const std::string_view ext : {".xds", ".tga", ".dds", ".png", ".bmp"}) {
    if (s.size() > ext.size() && s.compare(s.size() - ext.size(), ext.size(), ext) == 0) {
      s.resize(s.size() - ext.size());
      break;
    }
  }
  s += ".xds";
  return s;
}

bool is_rally_world_dir(const std::filesystem::path& world_dir) {
  if (world_dir.empty()) return false;
  const std::string low = to_lower(world_dir.string());
  return low.find("coloradodirt") != std::string::npos || low.find("1600_pri_65") != std::string::npos;
}

std::string read_archive_ribbon_xml(const std::filesystem::path& world_dir, std::string_view world_key_low,
                                    std::string_view filename_low) {
  std::error_code ec;
  std::vector<std::filesystem::path> zip_cands;
  if (to_lower(world_dir.extension().string()) == ".zip" && std::filesystem::exists(world_dir, ec)) {
    zip_cands.push_back(world_dir);
  }
  std::filesystem::path cur = world_dir;
  for (int depth = 0; depth < 6 && !cur.empty(); ++depth) {
    if (cur.has_filename() && cur.has_parent_path()) {
      const auto sib = cur.parent_path() / (cur.filename().string() + ".zip");
      if (std::filesystem::exists(sib, ec)) zip_cands.push_back(sib);
    }
    const auto z1600 = cur / "1600_pri_65.zip";
    if (std::filesystem::exists(z1600, ec)) zip_cands.push_back(z1600);
    const auto dlcz = cur / "Media" / "DLCZips" / "1600_pri_65.zip";
    if (std::filesystem::exists(dlcz, ec)) zip_cands.push_back(dlcz);
    const auto par = cur.parent_path();
    if (par == cur) break;
    cur = par;
  }
  const std::string suffix = std::string(world_key_low) + "/ribbon_00/" + std::string(filename_low);
  for (const auto& zp : zip_cands) {
    auto ar = fh1::Archive::open(zp, nullptr);
    if (!ar) continue;
    for (const auto& m : ar->members()) {
      std::string low = to_lower(m.name);
      for (char& c : low) {
        if (c == '\\') c = '/';
      }
      if (low.size() >= suffix.size() && low.compare(low.size() - suffix.size(), suffix.size(), suffix) == 0) {
        const auto rd = ar->read(m);
        if (rd.ok() && !rd.data.empty()) {
          return std::string(reinterpret_cast<const char*>(rd.data.data()), rd.data.size());
        }
      }
    }
  }
  return {};
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool read_attr(std::string_view tag, std::string_view name, std::string& out) {
  for (size_t p = tag.find(name); p != std::string_view::npos; p = tag.find(name, p + 1)) {
    if (p == 0 || (tag[p - 1] != ' ' && tag[p - 1] != '\t' && tag[p - 1] != '\n' && tag[p - 1] != '\r')) continue;
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

bool read_attr_f(std::string_view tag, std::string_view name, float& out) {
  std::string v;
  if (!read_attr(tag, name, v)) return false;
  out = float(std::atof(v.c_str()));
  return true;
}

bool read_attr_i(std::string_view tag, std::string_view name, int& out) {
  std::string v;
  if (!read_attr(tag, name, v)) return false;
  out = std::atoi(v.c_str());
  return true;
}

size_t find_tag(std::string_view xml, std::string_view name, size_t from, size_t to) {
  const std::string open = "<" + std::string(name);
  for (size_t p = xml.find(open, from); p != std::string_view::npos && p < to; p = xml.find(open, p + 1)) {
    const size_t q = p + open.size();
    if (q < xml.size() && (xml[q] == ' ' || xml[q] == '\t' || xml[q] == '/' || xml[q] == '>' || xml[q] == '\r' ||
                           xml[q] == '\n'))
      return p;
  }
  return std::string_view::npos;
}

std::string_view tag_slice(std::string_view xml, size_t start) {
  const size_t end = xml.find('>', start);
  return end == std::string_view::npos ? std::string_view{} : xml.substr(start, end - start + 1);
}

std::array<float, 3> vec_norm(std::array<float, 3> v, std::array<float, 3> fallback = {0.0f, 1.0f, 0.0f}) {
  const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  if (len2 < 1e-8f) return fallback;
  const float inv = 1.0f / std::sqrt(len2);
  return {v[0] * inv, v[1] * inv, v[2] * inv};
}

std::array<float, 3> vec_cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

}  // namespace

bool ParticleEffectDef::active_at_hour(float hour) const {
  float h = std::fmod(hour, 24.0f);
  if (h < 0.0f) h += 24.0f;
  const float start = float(start_h) + float(start_m) / 60.0f;
  const float end = float(end_h) + float(end_m) / 60.0f;
  if (start <= 0.0f && end >= 24.0f) return true;
  if (std::fabs(end - start) < 1e-3f) return true;
  if (start < end) return h >= start && h <= end;
  // Wraps past midnight (e.g. 20:00 .. 04:00 for night fireworks/flares).
  return h >= start || h <= end;
}

bool parse_particle_effect_xml(std::string_view xml, const std::string& name, ParticleEffectDef& out) {
  out = ParticleEffectDef{};
  out.name = to_lower(name);
  const size_t eff = find_tag(xml, "Effect", 0, xml.size());
  if (eff == std::string_view::npos) return false;
  const auto eff_tag = tag_slice(xml, eff);
  read_attr_i(eff_tag, "Version", out.version);
  read_attr_i(eff_tag, "budget", out.budget);
  read_attr_i(eff_tag, "lowerbudget", out.lower_budget);

  if (const size_t tod = find_tag(xml, "TimeOfDayData", eff, xml.size()); tod != std::string_view::npos) {
    const auto t = tag_slice(xml, tod);
    read_attr_i(t, "startH", out.start_h);
    read_attr_i(t, "startM", out.start_m);
    read_attr_i(t, "endH", out.end_h);
    read_attr_i(t, "endM", out.end_m);
  }

  auto find_in = [&](std::string_view elem, size_t from, size_t to) {
    const size_t p = find_tag(xml, elem, from, to);
    return p == std::string_view::npos ? std::string_view{} : tag_slice(xml, p);
  };

  if (const size_t ep = find_tag(xml, "EmitterProperties", eff, xml.size()); ep != std::string_view::npos) {
    const size_t ep_end = std::min(xml.size(), xml.find("</EmitterProperties>", ep));
    if (auto t = find_in("OffAxis", ep, ep_end); !t.empty()) {
      read_attr_f(t, "value", out.off_axis_deg);
      read_attr_f(t, "variation", out.off_axis_var_deg);
    }
    if (auto t = find_in("OffPlane", ep, ep_end); !t.empty()) {
      read_attr_f(t, "value", out.off_plane_deg);
      read_attr_f(t, "variation", out.off_plane_var_deg);
    }
    if (auto t = find_in("Quantity", ep, ep_end); !t.empty()) {
      read_attr_f(t, "value", out.quantity);
      read_attr_f(t, "var", out.quantity_var);
    }
    if (auto t = find_in("Velocity", ep, ep_end); !t.empty()) {
      read_attr_f(t, "value", out.velocity);
      read_attr_f(t, "variation", out.velocity_var);
    }
    if (auto t = find_in("EmitTime", ep, ep_end); !t.empty()) {
      int inf = 1;
      if (read_attr_i(t, "infinite", inf)) out.emit_infinite = (inf != 0);
    }
    if (auto t = find_in("Life", ep, ep_end); !t.empty()) {
      read_attr_f(t, "value", out.life);
      read_attr_f(t, "variation", out.life_var);
    }
    if (auto t = find_in("Density", ep, ep_end); !t.empty()) {
      read_attr_f(t, "value", out.density);
    }
    if (auto t = find_in("ParticleShaderType", ep, ep_end); !t.empty()) {
      std::string st;
      if (read_attr(t, "value", st)) {
        st = to_lower(st);
        if (st == "_soft") out.shader_kind = ParticleShaderKind::kSoft;
        else if (st == "_animated_soft") out.shader_kind = ParticleShaderKind::kAnimatedSoft;
        else if (st == "_animated_rowonly") out.shader_kind = ParticleShaderKind::kAnimatedRowOnly;
        else if (st == "_animated_rowonly_gradient") out.shader_kind = ParticleShaderKind::kAnimatedRowOnlyGradient;
        else if (st == "_gradient") out.shader_kind = ParticleShaderKind::kGradient;
        else if (st == "_gradient_soft") out.shader_kind = ParticleShaderKind::kGradientSoft;
        else if (st == "_animated") out.shader_kind = ParticleShaderKind::kAnimated;
        else if (st == "_animated_gradient") out.shader_kind = ParticleShaderKind::kAnimatedGradient;
        else if (st == "_animated_rowonly_soft") out.shader_kind = ParticleShaderKind::kAnimatedRowOnlySoft;
        else out.shader_kind = ParticleShaderKind::kDefault;
      }
    }
    if (auto t = find_in("Animation", ep, ep_end); !t.empty()) {
      read_attr_f(t, "rate", out.anim_rate);
      read_attr_i(t, "numFramesX", out.num_frames_x);
      read_attr_i(t, "numFramesY", out.num_frames_y);
      out.num_frames_x = std::max(1, out.num_frames_x);
      out.num_frames_y = std::max(1, out.num_frames_y);
    }
    if (auto t = find_in("Gradient", ep, ep_end); !t.empty()) {
      read_attr_i(t, "numGradientRows", out.num_gradient_rows);
      out.num_gradient_rows = std::max(1, out.num_gradient_rows);
    }
    if (auto t = find_in("RenderInMirror", ep, ep_end); !t.empty()) {
      int rm = 1;
      if (read_attr_i(t, "value", rm)) out.render_in_mirror = (rm != 0);
    }
    // Box emitters have a <Size length width height/> before the particle <Size valueX valueY .../>.
    for (size_t sp = find_tag(xml, "Size", ep, ep_end); sp != std::string_view::npos;
         sp = find_tag(xml, "Size", sp + 1, ep_end)) {
      const auto t = tag_slice(xml, sp);
      float vx = 0.0f;
      if (read_attr_f(t, "valueX", vx)) {
        out.size_x = vx;
        read_attr_f(t, "valueY", out.size_y);
        read_attr_f(t, "variationX", out.size_var_x);
        read_attr_f(t, "variationY", out.size_var_y);
        read_attr_f(t, "growforX", out.grow_for_x);
        read_attr_f(t, "growforY", out.grow_for_y);
        read_attr_f(t, "shrinkforX", out.shrink_for_x);
        read_attr_f(t, "shrinkforY", out.shrink_for_y);
        read_attr_f(t, "maxValueX", out.max_size_x);
        read_attr_f(t, "maxValueY", out.max_size_y);
        read_attr_f(t, "endValueX", out.end_size_x);
        read_attr_f(t, "endValueY", out.end_size_y);
        read_attr_f(t, "growFrom", out.grow_from);
        read_attr_f(t, "shrinkTo", out.shrink_to);
        break;
      }
    }
    if (auto t = find_in("SpinTime", ep, ep_end); !t.empty()) {
      read_attr_f(t, "value", out.spin_time);
      read_attr_f(t, "variation", out.spin_time_var);
    }
    if (auto t = find_in("SpinPhase", ep, ep_end); !t.empty()) {
      read_attr_f(t, "value", out.spin_phase);
      read_attr_f(t, "variation", out.spin_phase_var);
    }
    if (auto t = find_in("SpinAxis", ep, ep_end); !t.empty()) {
      int alt = 0;
      if (read_attr_i(t, "alternate", alt)) out.spin_alternate = (alt != 0);
    }
  }

  if (const size_t bh = find_tag(xml, "Behavior", eff, xml.size()); bh != std::string_view::npos) {
    const size_t bh_end = std::min(xml.size(), xml.find("</Behavior>", bh));
    if (auto t = find_in("MotionInheritance", bh, bh_end); !t.empty()) {
      read_attr_f(t, "influence", out.motion_influence);
      read_attr_f(t, "multiplier", out.motion_multiplier);
    }
    if (auto t = find_in("Gravity", bh, bh_end); !t.empty()) {
      read_attr_f(t, "strength", out.gravity_strength);
      read_attr_f(t, "decay", out.gravity_decay);
    }
    if (auto t = find_in("Friction", bh, bh_end); !t.empty()) {
      read_attr_f(t, "strength", out.friction_strength);
      read_attr_f(t, "velocityKept", out.friction_vel_kept);
    }
    if (auto t = find_in("ParticleSpawn", bh, bh_end); !t.empty()) {
      std::string cs;
      if (read_attr(t, "type", cs) && to_lower(cs) != "none") out.child_spawn = to_lower(cs);
    }
  }

  if (const size_t rn = find_tag(xml, "Render", eff, xml.size()); rn != std::string_view::npos) {
    const size_t rn_end = std::min(xml.size(), xml.find("</Render>", rn));
    if (auto t = find_in("ParticleType", rn, rn_end); !t.empty()) {
      std::string mode;
      if (read_attr(t, "value", mode)) out.velocity_quad = (to_lower(mode) == "quad");
      int dl = 1, zs = 0;
      if (read_attr_i(t, "dynamic_lighting", dl)) out.dynamic_lighting = (dl != 0);
      if (read_attr_i(t, "zsort", zs)) out.zsort = (zs != 0);
      read_attr_f(t, "max_screen_size", out.max_screen_size);
    }
    if (auto t = find_in("light_influence", rn, rn_end); !t.empty()) {
      read_attr_f(t, "v.x", out.light_influence[0]);
      read_attr_f(t, "v.y", out.light_influence[1]);
      read_attr_f(t, "v.z", out.light_influence[2]);
      read_attr_f(t, "v.w", out.light_influence[3]);
    }
    if (auto t = find_in("HeadLightPower", rn, rn_end); !t.empty()) {
      read_attr_f(t, "value", out.headlight_power);
    }
    if (auto t = find_in("Texture", rn, rn_end); !t.empty()) {
      std::string tex;
      if (read_attr(t, "value", tex)) out.texture_name = normalize_xds_name(tex);
    }
    if (auto t = find_in("Texture2", rn, rn_end); !t.empty()) {
      std::string tex2;
      if (read_attr(t, "value", tex2)) out.texture2_name = normalize_xds_name(tex2);
    }
    if (auto t = find_in("BlendType", rn, rn_end); !t.empty()) {
      std::string bt;
      if (read_attr(t, "value", bt)) out.additive = (to_lower(bt) == "additive");
    }
    auto read_rgba = [&](const char* elem, std::array<float, 4>& c) {
      if (auto t = find_in(elem, rn, rn_end); !t.empty()) {
        read_attr_f(t, "r", c[0]);
        read_attr_f(t, "g", c[1]);
        read_attr_f(t, "b", c[2]);
        read_attr_f(t, "a", c[3]);
      }
    };
    read_rgba("RGBStart", out.rgb_start);
    read_rgba("RGBMid", out.rgb_mid);
    read_rgba("RGBEnd", out.rgb_end);
    if (auto t = find_in("FadeInTo", rn, rn_end); !t.empty()) read_attr_f(t, "fadeInTo", out.fade_in_to);
    if (auto t = find_in("FadeOutFrom", rn, rn_end); !t.empty()) read_attr_f(t, "fadeOutFrom", out.fade_out_from);
    if (auto t = find_in("NearFade", rn, rn_end); !t.empty()) read_attr_f(t, "nearFade", out.near_fade);
  }
  return true;
}

bool parse_smashable_effects_xml(std::string_view xml, std::map<std::string, std::string>& out) {
  out.clear();
  for (size_t p = find_tag(xml, "FileEffectPair", 0, xml.size()); p != std::string_view::npos;
       p = find_tag(xml, "FileEffectPair", p + 1, xml.size())) {
    const auto t = tag_slice(xml, p);
    std::string stem, eff;
    if (read_attr(t, "smashableName", stem) && read_attr(t, "system_name", eff)) {
      out[to_lower(stem)] = to_lower(eff);
    }
  }
  return !out.empty();
}

bool parse_backfire_timer_config_xml(std::string_view xml, BackfireTimerConfig& out) {
  out = BackfireTimerConfig{};
  size_t idx = 0;
  for (size_t p = find_tag(xml, "Timer", 0, xml.size()); p != std::string_view::npos && idx < out.tiers.size();
       p = find_tag(xml, "Timer", p + 1, xml.size())) {
    const auto t = tag_slice(xml, p);
    auto& tier = out.tiers[idx++];
    read_attr_f(t, "MinTime", tier.min_time_ms);
    read_attr_f(t, "MaxTime", tier.max_time_ms);
    read_attr_f(t, "MinNormRPM", tier.min_norm_rpm);
    read_attr_f(t, "MaxNormRPM", tier.max_norm_rpm);
    read_attr_f(t, "SilenceWeighting", tier.silence_weighting);
  }
  out.loaded = (idx == 4);
  return out.loaded;
}

bool parse_particle_emitters_xml(std::string_view xml, std::vector<WorldEmitter>& out) {
  out.clear();
  auto parse_kind = [&](std::string_view block_tag, WorldEmitterKind kind) {
    const std::string close_tag = "</" + std::string(block_tag) + ">";
    for (size_t p = find_tag(xml, block_tag, 0, xml.size()); p != std::string_view::npos;
         p = find_tag(xml, block_tag, p + 1, xml.size())) {
      const size_t end = xml.find(close_tag, p);
      if (end == std::string_view::npos) break;
      WorldEmitter e;
      e.kind = kind;
      auto get_val = [&](std::string_view elem, std::string& dst) {
        if (const size_t q = find_tag(xml, elem, p, end); q != std::string_view::npos)
          read_attr(tag_slice(xml, q), "value", dst);
      };
      get_val("Name", e.name);
      get_val("GameplayID", e.gameplay_id);
      if (kind == WorldEmitterKind::kTriggerZone) {
        if (const size_t q = find_tag(xml, "Emitter", p, end); q != std::string_view::npos)
          read_attr(tag_slice(xml, q), "name", e.effect);
      } else {
        get_val("Effect", e.effect);
      }
      std::string vtype;
      get_val("Type", vtype);
      const std::string vt = to_lower(vtype);
      if (vt == "sphere") e.volume = WorldEmitterVolume::kSphere;
      else if (vt == "plane") e.volume = WorldEmitterVolume::kPlane;
      else if (vt == "box") e.volume = WorldEmitterVolume::kBox;
      else e.volume = WorldEmitterVolume::kSuperspray;

      auto read_vec3_file = [&](std::string_view elem, std::array<float, 3>& dst) {
        if (const size_t q = find_tag(xml, elem, p, end); q != std::string_view::npos) {
          const auto t = tag_slice(xml, q);
          read_attr_f(t, "v.x", dst[0]);
          read_attr_f(t, "v.y", dst[1]);
          read_attr_f(t, "v.z", dst[2]);
          // ParticleEmitters.xml uses the file frame; game frame negates Z.
          dst[2] = -dst[2];
        }
      };
      read_vec3_file("Position", e.pos);
      read_vec3_file("XAxis", e.x_axis);
      read_vec3_file("YAxis", e.y_axis);
      read_vec3_file("ZAxis", e.z_axis);

      if (const size_t q = find_tag(xml, "Radius", p, end); q != std::string_view::npos)
        read_attr_f(tag_slice(xml, q), "value", e.radius);
      if (const size_t q = find_tag(xml, "RetriggerDelay", p, end); q != std::string_view::npos)
        read_attr_f(tag_slice(xml, q), "value", e.retrigger_delay);
      if (const size_t q = find_tag(xml, "Spray", p, end); q != std::string_view::npos) {
        const auto t = tag_slice(xml, q);
        read_attr_f(t, "offAxisSpread", e.off_axis_spread);
        read_attr_f(t, "planeSpread", e.plane_spread);
      }
      if (const size_t q = find_tag(xml, "Plane", p, end); q != std::string_view::npos) {
        const auto t = tag_slice(xml, q);
        read_attr_f(t, "width", e.width);
        read_attr_f(t, "length", e.length);
      }
      if (const size_t q = find_tag(xml, "Box", p, end); q != std::string_view::npos) {
        const auto t = tag_slice(xml, q);
        read_attr_f(t, "width", e.width);
        read_attr_f(t, "length", e.length);
        read_attr_f(t, "height", e.height);
      }
      out.push_back(std::move(e));
      p = end;
    }
  };
  parse_kind("SimpleEmitter", WorldEmitterKind::kSimple);
  parse_kind("TriggerZone", WorldEmitterKind::kTriggerZone);
  parse_kind("TriggerableEmitter", WorldEmitterKind::kTriggerable);
  return !out.empty();
}

std::vector<std::string> expand_composite_effect(std::string_view effect_name) {
  const std::string low = to_lower(effect_name);
  if (low.rfind("_composite_", 0) != 0) return {low};
  if (low.find("mainstage") != std::string::npos) {
    return {"amb_confetti_smoke", "amb_main_stage_large_ambient_smoke", "amb_sparkcannon_smoke", "amb_fireworksmoke"};
  }
  if (low.find("substage") != std::string::npos) {
    return {"amb_confetti_smoke", "amb_substage_large_ambient_smoke"};
  }
  if (low.find("flaregun") != std::string::npos) {
    return {"amb_flaregun_redtrailsmoke", "amb_flaregun_sparks"};
  }
  if (low.find("flare") != std::string::npos) {
    // Ground road flares at event/outpost/showcase activation zones (`_Composite_Flare` / `_Composite_Flare_1..7`).
    // Map to the matching ground flare glow + handflare smoke (never aerial flare-gun trail smoke).
    std::string ground_flare = "amb_ground_flare";
    if (low.size() >= 2 && low[low.size() - 2] == '_' &&
        low.back() >= '1' && low.back() <= '7') {
      ground_flare += '_';
      ground_flare += low.back();
    }
    return {ground_flare, "amb_handflare_smoke"};
  }
  if (low.find("campfire") != std::string::npos) {
    return {"amb_fire_smoke"};
  }
  if (low.find("chimney") != std::string::npos) {
    return {"amb_housesmoke"};
  }
  return {};
}

ParticleLibrary::ParticleLibrary() = default;
ParticleLibrary::~ParticleLibrary() = default;

bool ParticleLibrary::load(const std::filesystem::path& media_dir, const std::filesystem::path& world_dir,
                           std::string* error) {
  effects_.clear();
  xds_members_.clear();
  smashable_effects_.clear();
  world_emitters_.clear();
  backfire_config_ = BackfireTimerConfig{};
  rally_world_ = is_rally_world_dir(world_dir);

  const auto zip_path = media_dir / "effects.zip";
  if (std::filesystem::exists(zip_path)) {
    std::string zerr;
    effects_zip_ = fh1::Archive::open(zip_path, &zerr);
    if (!effects_zip_) {
      if (error) *error = "effects.zip: " + zerr;
      return false;
    }
    for (const auto& m : effects_zip_->members()) {
      const std::string low = to_lower(m.name);
      const size_t slash = low.find_last_of("/\\");
      const std::string base = (slash == std::string::npos) ? low : low.substr(slash + 1);
      if (base.size() > 4 && base.substr(base.size() - 4) == ".xml") {
        const auto res = effects_zip_->read(m);
        if (res.ok() && !res.data.empty()) {
          const std::string stem = base.substr(0, base.size() - 4);
          ParticleEffectDef def;
          const std::string_view view(reinterpret_cast<const char*>(res.data.data()), res.data.size());
          if (parse_particle_effect_xml(view, stem, def)) {
            effects_[stem] = std::move(def);
          }
        }
      } else if (base.size() > 4 && base.substr(base.size() - 4) == ".xds") {
        xds_members_[base] = m.name;
      }
    }
  }

  auto load_ribbon_xml = [&](const char* filename, const char* filename_low) -> std::string {
    std::error_code ec;
    if (!world_dir.empty()) {
      const auto loose = world_dir / "Ribbon_00" / filename;
      if (std::filesystem::exists(loose, ec)) {
        std::string txt = read_text_file(loose);
        if (!txt.empty()) return txt;
      }
      if (rally_world_) {
        std::string arc_txt = read_archive_ribbon_xml(world_dir, "coloradodirt", filename_low);
        if (!arc_txt.empty()) return arc_txt;
      }
    }
    // Fallback to base Colorado XMLs only when the active world / Rally archive lacks them.
    if (!media_dir.empty()) {
      const auto fb = media_dir / "tracks" / "colorado" / "Ribbon_00" / filename;
      if (std::filesystem::exists(fb, ec)) return read_text_file(fb);
    }
    return {};
  };

  if (!world_dir.empty()) {
    const std::string se_xml = load_ribbon_xml("SmashableEffects.xml", "smashableeffects.xml");
    if (!se_xml.empty()) {
      parse_smashable_effects_xml(se_xml, smashable_effects_);
    }
    const std::string pe_xml = load_ribbon_xml("ParticleEmitters.xml", "particleemitters.xml");
    if (!pe_xml.empty()) {
      parse_particle_emitters_xml(pe_xml, world_emitters_);
    }
  }

  const auto bf_path = media_dir / "audio" / "BackfireTimerConfig.xml";
  if (std::filesystem::exists(bf_path)) {
    parse_backfire_timer_config_xml(read_text_file(bf_path), backfire_config_);
  }
  return !effects_.empty();
}

std::vector<std::string> ParticleLibrary::xds_names() const {
  std::vector<std::string> names;
  names.reserve(xds_members_.size());
  for (const auto& [k, _] : xds_members_) names.push_back(k);
  std::sort(names.begin(), names.end());
  return names;
}

const ParticleEffectDef* ParticleLibrary::find_effect(std::string_view name) const {
  const auto it = effects_.find(to_lower(name));
  return it == effects_.end() ? nullptr : &it->second;
}

std::string ParticleLibrary::smashable_effect_for(std::string_view stem) const {
  const auto it = smashable_effects_.find(to_lower(stem));
  return it == smashable_effects_.end() ? std::string{} : it->second;
}

bool ParticleLibrary::decode_texture(std::string_view filename, fh1::RgbaImage& out) const {
  if (!effects_zip_) return false;
  const std::string key = normalize_xds_name(filename);
  auto it = xds_members_.find(key);
  if (it == xds_members_.end()) return false;
  const auto res = effects_zip_->read(it->second);
  if (!res.ok() || res.data.empty()) return false;
  const auto dec = fh1::decode_xds(res.data, false);
  if ((dec.status != fh1::XdsStatus::kOk && dec.status != fh1::XdsStatus::kBaseOnly) || dec.levels.empty()) {
    return false;
  }
  out = dec.levels[0];
  return out.width > 0 && out.height > 0 && !out.rgba.empty();
}

void ParticleLibrary::add_effect(ParticleEffectDef def) {
  const std::string k = to_lower(def.name);
  def.name = k;
  effects_[k] = std::move(def);
}

void ParticleSystem::set_library(const ParticleLibrary* lib) {
  lib_ = lib;
  clear();
  triggerable_by_name_.clear();
  if (lib_) {
    const auto& ems = lib_->world_emitters();
    for (size_t i = 0; i < ems.size(); ++i) {
      if (ems[i].kind != WorldEmitterKind::kTriggerable) continue;
      if (!ems[i].name.empty()) triggerable_by_name_[to_lower(ems[i].name)] = i;
      if (!ems[i].gameplay_id.empty()) triggerable_by_name_[to_lower(ems[i].gameplay_id)] = i;
    }
  }
}

void ParticleSystem::clear() {
  particles_.clear();
  accumulators_.clear();
  trigger_cooldowns_.clear();
  fallback_backfire_cooldown_ = 0.0f;
}

uint16_t ParticleSystem::resolve_texture_slot(std::string_view tex_name) {
  const std::string key = to_lower(tex_name);
  if (key.empty()) return 0;
  auto it = tex_slot_map_.find(key);
  if (it != tex_slot_map_.end()) return it->second;
  // Slot 0 is reserved for procedural soft puff fallback.
  const uint16_t slot = uint16_t(used_textures_.size() + 1);
  used_textures_.push_back(key);
  tex_slot_map_[key] = slot;
  return slot;
}

float ParticleSystem::rand01() {
  uint32_t x = rng_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_ = x ? x : 0x9E3779B9u;
  return float(rng_ & 0x00FFFFFFu) / float(0x01000000u);
}

float ParticleSystem::rand_sym() { return rand01() * 2.0f - 1.0f; }

void ParticleSystem::spawn_one(const ParticleEffectDef& def, const std::array<float, 3>& pos,
                               const std::array<float, 3>& emit_dir, const std::array<float, 3>& emitter_vel,
                               float intensity_scale, uint8_t child_depth, bool world_ambient) {
  if (particles_.size() >= kMaxParticles) {
    // FIFO oldest eviction so bursts never stall.
    particles_.erase(particles_.begin());
  }
  LiveParticle p;
  p.def = &def;
  p.tex_slot = resolve_texture_slot(def.texture_name);
  p.tex2_slot = resolve_texture_slot(def.texture2_name);
  p.child_depth = child_depth;
  p.world_ambient = world_ambient;
  p.pos = pos;
  p.age = 0.0f;
  const float base_life = std::max(0.05f, def.life);
  p.life = std::max(0.04f, base_life * (1.0f + rand_sym() * std::clamp(def.life_var, 0.0f, 0.8f)));

  // Build cone basis around emit_dir.
  const auto fwd = vec_norm(emit_dir, {0.0f, 1.0f, 0.0f});
  const std::array<float, 3> ref = std::fabs(fwd[1]) < 0.9f ? std::array<float, 3>{0, 1, 0} : std::array<float, 3>{1, 0, 0};
  const auto right = vec_norm(vec_cross(ref, fwd), {1.0f, 0.0f, 0.0f});
  const auto up = vec_cross(fwd, right);

  const float spread_a = (def.off_axis_deg + rand_sym() * def.off_axis_var_deg) * kDegToRad;
  const float spread_p = (def.off_plane_deg + rand_sym() * def.off_plane_var_deg) * kDegToRad;
  const float az = rand01() * 2.0f * kPi;
  const float min_spread = world_ambient ? 0.24f : 0.12f;
  const float cone_r =
      std::sin(std::clamp(std::fabs(spread_a) + std::fabs(spread_p) + min_spread, 0.0f, 1.35f)) * rand01();
  auto dir = vec_norm({fwd[0] + (right[0] * std::cos(az) + up[0] * std::sin(az)) * cone_r,
                       fwd[1] + (right[1] * std::cos(az) + up[1] * std::sin(az)) * cone_r,
                       fwd[2] + (right[2] * std::cos(az) + up[2] * std::sin(az)) * cone_r},
                      fwd);

  const float raw_spd = std::max(0.0f, def.velocity * (1.0f + rand_sym() * std::clamp(def.velocity_var, 0.0f, 0.8f)));
  // Keep ambient world smoke/flare plumes drifting gently instead of firing like vertical rockets.
  const float spd = world_ambient ? std::min(raw_spd, 4.5f) : raw_spd;
  const float inh = std::clamp(def.motion_influence * 0.01f, 0.0f, 1.0f) * def.motion_multiplier;
  p.vel = {
      dir[0] * spd + emitter_vel[0] * inh,
      dir[1] * spd + emitter_vel[1] * inh,
      dir[2] * spd + emitter_vel[2] * inh,
  };

  p.size_scale_x = std::max(0.25f, 1.0f + rand_sym() * std::clamp(def.size_var_x, 0.0f, 0.7f));
  p.size_scale_y = std::max(0.25f, 1.0f + rand_sym() * std::clamp(def.size_var_y, 0.0f, 0.7f));
  p.spin = def.spin_phase * kDegToRad + rand_sym() * def.spin_phase_var;
  if (def.spin_time > 0.05f) {
    const float st = std::max(0.1f, def.spin_time * (1.0f + rand_sym() * std::clamp(def.spin_time_var, 0.0f, 0.7f)));
    const float sign = (def.spin_alternate && (rand01() < 0.5f)) ? -1.0f : 1.0f;
    p.spin_rate = sign * (2.0f * kPi / st);
  }
  p.alpha_scale = std::clamp(intensity_scale, 0.1f, 2.0f);
  if (def.num_frames_y > 1) {
    p.anim_row = int(rand01() * float(def.num_frames_y)) % def.num_frames_y;
  }
  if (def.num_gradient_rows > 1) {
    p.grad_row = int(rand01() * float(def.num_gradient_rows)) % def.num_gradient_rows;
  }
  particles_.push_back(p);
}

void ParticleSystem::emit_world_continuous(const ParticleEffectDef& def, const std::array<float, 3>& pos,
                                           const std::array<float, 3>& emit_dir, float dt, float intensity_scale,
                                           uint64_t emitter_key, bool is_point_emitter) {
  if (dt <= 0.0f || intensity_scale <= 0.01f) return;
  // Cap point/superspray world emitter spawn rates so stacked quads never blow out to solid white piles.
  const float max_rate = is_point_emitter ? 8.5f : 14.0f;
  const float rate = std::min(std::max(0.0f, def.quantity), max_rate) * std::clamp(intensity_scale, 0.0f, 1.25f);
  float& acc = accumulators_[emitter_key];
  acc += rate * dt;
  const int to_spawn = std::min(int(acc), 6);
  acc -= float(to_spawn);
  if (acc > 2.0f) acc = 2.0f;
  for (int i = 0; i < to_spawn; ++i) {
    std::array<float, 3> p = pos;
    if (is_point_emitter) {
      p[0] += rand_sym() * 0.18f;
      p[1] += rand01() * 0.12f;
      p[2] += rand_sym() * 0.18f;
    }
    spawn_one(def, p, emit_dir, {0.0f, 0.0f, 0.0f}, intensity_scale, 0, true);
  }
}

void ParticleSystem::spawn_burst(std::string_view effect_name, const std::array<float, 3>& pos,
                                 const std::array<float, 3>& emit_dir, const std::array<float, 3>& emitter_vel,
                                 int count, float intensity_scale) {
  if (!lib_ || count <= 0) return;
  const auto* def = lib_->find_effect(effect_name);
  if (!def) return;
  const int capped = std::min(count, std::max(1, def->budget));
  for (int i = 0; i < capped; ++i) {
    spawn_one(*def, pos, emit_dir, emitter_vel, intensity_scale);
  }
}

void ParticleSystem::emit_continuous(std::string_view effect_name, const std::array<float, 3>& pos,
                                     const std::array<float, 3>& emit_dir, const std::array<float, 3>& emitter_vel,
                                     float dt, float intensity_scale, uint64_t emitter_key) {
  if (!lib_ || dt <= 0.0f || intensity_scale <= 0.01f) return;
  const auto* def = lib_->find_effect(effect_name);
  if (!def) return;
  const float rate = std::max(0.0f, def->quantity) * std::clamp(intensity_scale, 0.0f, 2.0f);
  float& acc = accumulators_[emitter_key];
  acc += rate * dt;
  const int to_spawn = std::min(int(acc), 16);
  acc -= float(to_spawn);
  if (acc > 4.0f) acc = 4.0f;
  for (int i = 0; i < to_spawn; ++i) {
    spawn_one(*def, pos, emit_dir, emitter_vel, intensity_scale);
  }
}

void ParticleSystem::update_vehicle(const VehicleVfxInput& in, float dt) {
  if (!lib_ || dt <= 0.0f) return;

  for (int w = 0; w < 4; ++w) {
    const auto& wh = in.wheels[size_t(w)];
    if (!wh.grounded) continue;

    // Emit slightly behind and above the contact patch so plumes billow out cleanly.
    const std::array<float, 3> patch = {
        wh.contact_pos[0] + wh.surface_normal[0] * 0.14f - wh.wheel_forward[0] * 0.18f,
        wh.contact_pos[1] + wh.surface_normal[1] * 0.14f - wh.wheel_forward[1] * 0.18f,
        wh.contact_pos[2] + wh.surface_normal[2] * 0.14f - wh.wheel_forward[2] * 0.18f,
    };
    const std::array<float, 3> plume_dir = vec_norm({
        wh.surface_normal[0] * 0.65f - wh.wheel_forward[0] * 0.75f,
        wh.surface_normal[1] * 0.65f - wh.wheel_forward[1] * 0.75f + 0.25f,
        wh.surface_normal[2] * 0.65f - wh.wheel_forward[2] * 0.75f,
    });
    const float speed_factor = std::clamp((wh.wheel_speed_mps - 2.0f) / 22.0f, 0.0f, 1.0f);
    const float slip_factor = std::clamp((wh.slip_combined - 0.50f) / 1.10f, 0.0f, 1.0f);

    // Asphalt tyre smoke: triggered above slip threshold.
    if (wh.surface.smoke_weight > 0.01f && slip_factor > 0.05f) {
      const float inten = slip_factor * wh.surface.smoke_weight;
      emit_continuous("smoke", patch, plume_dir, in.car_vel, dt, inten, 0x100u + uint64_t(w));
      if (slip_factor > 0.45f) {
        emit_continuous("smokerim0", patch, plume_dir, in.car_vel, dt, inten * 0.6f, 0x110u + uint64_t(w));
      }
    }

    // Off-road dust, dirt, gravel, grass, leaves & litter plumes (scale with speed + slip).
    const float offroad_drive = std::max(slip_factor, speed_factor * std::max(wh.surface.skid_min_intensity, 0.45f));
    if (wh.surface.dust_weight > 0.01f && offroad_drive > 0.08f) {
      emit_continuous("dust1", patch, plume_dir, in.car_vel, dt, offroad_drive * wh.surface.dust_weight,
                      0x120u + uint64_t(w));
    }
    if (wh.surface.dirt_weight > 0.01f && offroad_drive > 0.08f) {
      emit_continuous("dirt1", patch, plume_dir, in.car_vel, dt, offroad_drive * wh.surface.dirt_weight * 0.75f,
                      0x130u + uint64_t(w));
      // Dirt also kicks up soft dust plumes on Colorado trails.
      if (wh.surface.dust_weight < 0.05f) {
        emit_continuous("dust1", patch, plume_dir, in.car_vel, dt, offroad_drive * 0.55f, 0x138u + uint64_t(w));
      }
    }
    if (wh.surface.gravel_weight > 0.01f && offroad_drive > 0.08f) {
      emit_continuous("gravelbits", patch, plume_dir, in.car_vel, dt, offroad_drive * wh.surface.gravel_weight * 0.8f,
                      0x140u + uint64_t(w));
      emit_continuous("dust1", patch, plume_dir, in.car_vel, dt, offroad_drive * 0.65f, 0x148u + uint64_t(w));
    }
    if (wh.surface.grass_weight > 0.01f && offroad_drive > 0.12f) {
      emit_continuous("grass1", patch, plume_dir, in.car_vel, dt, offroad_drive * wh.surface.grass_weight * 0.7f,
                      0x150u + uint64_t(w));
    }
    if (wh.surface.leaf_weight > 0.01f && offroad_drive > 0.12f) {
      emit_continuous("leaf1", patch, plume_dir, in.car_vel, dt, offroad_drive * wh.surface.leaf_weight * 0.6f,
                      0x160u + uint64_t(w));
    }
    if (wh.surface.litter_weight > 0.01f && offroad_drive > 0.12f) {
      emit_continuous("litter", patch, plume_dir, in.car_vel, dt, offroad_drive * wh.surface.litter_weight * 0.6f,
                      0x170u + uint64_t(w));
    }
  }

  // Continuous wall scrape or bottoming sparks on ShouldSpark surfaces.
  if (in.scraping && in.scrape_should_spark) {
    const std::array<float, 3> spark_dir = vec_norm({
        in.scrape_normal[0] * 0.5f - in.car_forward[0] * 0.85f,
        in.scrape_normal[1] * 0.5f + 0.35f,
        in.scrape_normal[2] * 0.5f - in.car_forward[2] * 0.85f,
    });
    emit_continuous("sparks1", in.scrape_pos, spark_dir, in.car_vel, dt, 1.0f, 0x200u);
  }

  // Exhaust backfire flames: directly coupled to Slice 2 audio backfire triggers,
  // plus fallback simulation timer when audio thread is inactive (e.g. headless mode).
  fallback_backfire_cooldown_ = std::max(0.0f, fallback_backfire_cooldown_ - dt);
  bool fire_backfire = (in.audio_backfire_pulses > 0);
  if (!fire_backfire && in.backfire_level > 0 && fallback_backfire_cooldown_ <= 0.0f) {
    const int lv_idx = std::clamp(in.backfire_level, 0, 3);
    const auto& tier = lib_->backfire_config().tiers[size_t(lv_idx)];
    const bool in_rpm = (in.norm_rpm >= tier.min_norm_rpm && in.norm_rpm <= tier.max_norm_rpm);
    if ((in.upshift_event && in.norm_rpm > 0.65f) || (in.throttle < 0.05f && in_rpm && tier.max_time_ms > 0.0f)) {
      if (in.upshift_event || rand01() * 100.0f >= tier.silence_weighting) {
        fire_backfire = true;
      }
      const float lo = std::max(0.15f, tier.min_time_ms * 0.001f);
      const float hi = std::max(lo + 0.05f, tier.max_time_ms * 0.001f);
      fallback_backfire_cooldown_ = lo + rand01() * (hi - lo);
    }
  }
  if (fire_backfire && in.exhaust_tip_count > 0) {
    const std::array<float, 3> exhaust_dir = {-in.car_forward[0], -in.car_forward[1] + 0.05f, -in.car_forward[2]};
    for (int t = 0; t < std::min(in.exhaust_tip_count, 2); ++t) {
      spawn_burst("backfire", in.exhaust_tips[size_t(t)], exhaust_dir, in.car_vel, 2, 1.4f);
    }
  }
}

void ParticleSystem::spawn_collision_impact(const std::array<float, 3>& pos, const std::array<float, 3>& normal,
                                            const std::array<float, 3>& vel, float rel_speed_mps, bool should_spark) {
  if (!lib_ || rel_speed_mps < 2.5f) return;
  const float inten = std::clamp(rel_speed_mps / 18.0f, 0.25f, 1.5f);
  const auto dir = vec_norm({normal[0], normal[1] + 0.4f, normal[2]});
  if (should_spark) {
    spawn_burst("sparkscollision", pos, dir, vel, int(10.0f * inten), inten);
    spawn_burst("sparks1", pos, dir, vel, int(8.0f * inten), inten);
  }
  spawn_burst("smokecollision", pos, dir, vel, std::max(3, int(6.0f * inten)), inten * 0.85f);
}

void ParticleSystem::spawn_smashable_burst(std::string_view prop_stem, const std::array<float, 3>& pos,
                                           const std::array<float, 3>& impulse_vel) {
  if (!lib_) return;
  std::string eff = lib_->smashable_effect_for(prop_stem);
  if (eff.empty()) {
    const std::string low = to_lower(prop_stem);
    if (low.find("fence") != std::string::npos || low.find("wood") != std::string::npos ||
        low.find("sign") != std::string::npos || low.find("flyer") != std::string::npos) {
      eff = "bi_wood";
    } else if (low.find("mail") != std::string::npos || low.find("pole") != std::string::npos ||
               low.find("metal") != std::string::npos) {
      eff = "bi_sparks_medium";
    } else {
      eff = "bi_dust_small";
    }
  }
  const std::array<float, 3> up_dir = vec_norm({impulse_vel[0] * 0.35f, 1.0f, impulse_vel[2] * 0.35f});
  spawn_burst(eff, pos, up_dir, impulse_vel, 18, 1.2f);
  // Subtle impact dust puff alongside wood/metal splinters.
  spawn_burst("bi_dust_small", pos, up_dir, impulse_vel, 8, 0.9f);
}

void ParticleSystem::update_world_emitters(const std::array<float, 3>& focus_pos, float tod_hour, float dt) {
  if (!lib_ || dt <= 0.0f) return;
  constexpr float kMaxDist = 180.0f;
  constexpr float kMaxDist2 = kMaxDist * kMaxDist;
  const auto& emitters = lib_->world_emitters();

  // Advance TriggerZone cooldown timers.
  for (auto it = trigger_cooldowns_.begin(); it != trigger_cooldowns_.end();) {
    it->second -= dt;
    if (it->second <= 0.0f) it = trigger_cooldowns_.erase(it);
    else ++it;
  }

  for (size_t i = 0; i < emitters.size(); ++i) {
    const auto& em = emitters[i];
    if (em.effect.empty()) continue;
    // Speed-camera flashes are triggered by gameplay events, not ambiently.
    if (em.effect == "AMB_Camera_Flash") continue;
    const float dx = em.pos[0] - focus_pos[0];
    const float dz = em.pos[2] - focus_pos[2];
    const float d2 = dx * dx + dz * dz;
    if (em.kind == WorldEmitterKind::kTriggerZone) {
      const float trig_r = std::clamp(std::max({em.radius, em.width * 0.5f, em.length * 0.5f}), 4.0f, 90.0f);
      if (d2 > trig_r * trig_r) continue;
      if (trigger_cooldowns_.find(i) != trigger_cooldowns_.end()) continue;
      trigger_cooldowns_[i] = std::max(em.retrigger_delay, 2.0f);
      std::string target_eff = em.effect;
      std::array<float, 3> spawn_p = em.pos;
      std::array<float, 3> spawn_dir = em.y_axis;
      if (const auto tit = triggerable_by_name_.find(to_lower(em.effect));
          tit != triggerable_by_name_.end() && tit->second < emitters.size()) {
        const auto& linked = emitters[tit->second];
        if (!linked.effect.empty()) target_eff = linked.effect;
        spawn_p = linked.pos;
        spawn_dir = linked.y_axis;
      }
      const auto expanded = expand_composite_effect(target_eff);
      for (const auto& ef_name : expanded) {
        const auto* def = lib_->find_effect(ef_name);
        if (!def && ef_name.rfind("amb_ground_flare_", 0) == 0) def = lib_->find_effect("amb_ground_flare");
        if (!def || !def->active_at_hour(tod_hour)) continue;
        const int burst_n = std::clamp(int(std::lround(std::max(4.0f, def->quantity * 0.4f))), 4, 14);
        for (int b = 0; b < burst_n; ++b) {
          spawn_one(*def, spawn_p, spawn_dir, {0, 0, 0}, 0.9f, 0, true);
        }
      }
      continue;
    }
    if (em.kind == WorldEmitterKind::kTriggerable) continue;
    if (d2 > kMaxDist2) continue;

    const auto expanded = expand_composite_effect(em.effect);
    for (size_t k = 0; k < expanded.size(); ++k) {
      const auto* def = lib_->find_effect(expanded[k]);
      if (!def && expanded[k].rfind("amb_ground_flare_", 0) == 0) def = lib_->find_effect("amb_ground_flare");
      if (!def || !def->active_at_hour(tod_hour)) continue;
      // Offset within emitter volume (Sphere / Box / Plane).
      std::array<float, 3> spawn_p = em.pos;
      if (em.volume == WorldEmitterVolume::kBox) {
        spawn_p[0] += rand_sym() * em.width * 0.5f;
        spawn_p[1] += rand_sym() * em.height * 0.5f;
        spawn_p[2] += rand_sym() * em.length * 0.5f;
      } else if (em.volume == WorldEmitterVolume::kPlane) {
        spawn_p[0] += rand_sym() * em.width * 0.5f;
        spawn_p[2] += rand_sym() * em.length * 0.5f;
      } else if (em.volume == WorldEmitterVolume::kSphere && em.radius > 0.1f && em.radius < 25.0f) {
        spawn_p[0] += rand_sym() * em.radius * 0.5f;
        spawn_p[1] += rand_sym() * em.radius * 0.35f;
        spawn_p[2] += rand_sym() * em.radius * 0.5f;
      }
      const float dist_fade = 1.0f - std::clamp(std::sqrt(d2) / kMaxDist, 0.0f, 1.0f);
      const float scale = std::clamp(0.35f + 0.65f * dist_fade, 0.25f, 1.0f);
      const uint64_t key = 0x100000ull + (uint64_t(i) << 4) + uint64_t(k);
      const bool is_point = (em.volume == WorldEmitterVolume::kSuperspray);
      emit_world_continuous(*def, spawn_p, em.y_axis, dt, scale, key, is_point);
    }
  }
}

void ParticleSystem::step(float dt, const std::array<float, 3>& wind_dir, float wind_speed) {
  if (dt <= 0.0f) return;
  const float step_dt = std::min(dt, 0.1f);
  struct ChildSpawnReq {
    std::string effect;
    std::array<float, 3> pos;
    std::array<float, 3> vel;
    uint8_t depth;
    bool world_ambient;
  };
  std::vector<ChildSpawnReq> child_spawns;
  size_t write = 0;
  for (size_t i = 0; i < particles_.size(); ++i) {
    LiveParticle& p = particles_[i];
    p.age += step_dt;
    if (!p.def || p.age >= p.life) {
      if (p.def && !p.def->child_spawn.empty() && p.child_depth < 2 && child_spawns.size() < 24) {
        child_spawns.push_back({p.def->child_spawn, p.pos, p.vel, uint8_t(p.child_depth + 1), p.world_ambient});
      }
      continue;
    }

    // Gravity scale: positive XML values (60..300) represent downward gravity (~2..10 m/s^2);
    // negative XML values represent gentle upward smoke buoyancy (clamped so smoke plumes never
    // accelerate upward like a vertical rocket column).
    const float g_xml = p.def->gravity_strength;
    float accel_y = 0.0f;
    if (g_xml >= 0.0f) {
      accel_y = -g_xml * (9.81f / 300.0f);
    } else if (g_xml >= -5.0f) {
      accel_y = std::min(-g_xml * 0.40f, 2.0f);
    } else {
      accel_y = std::min(-g_xml * (9.81f / 300.0f), 2.2f);
    }
    p.vel[1] += accel_y * step_dt;
    if (p.world_ambient && g_xml <= 0.0f && p.vel[1] > 3.2f) {
      p.vel[1] = 3.2f;
    }

    // Exponential air friction drag.
    if (p.def->friction_strength > 0.001f) {
      const float damp = std::exp(-p.def->friction_strength * 1.4f * step_dt);
      p.vel[0] *= damp;
      p.vel[1] *= damp;
      p.vel[2] *= damp;
    }

    // Gentle wind drift & dispersion on lightweight smoke/dust/leaf particles (low positive gravity).
    if (g_xml <= 80.0f && wind_speed > 0.01f) {
      const float w_inf = (g_xml <= 0.0f) ? (p.world_ambient ? 0.55f : 0.35f) : 0.20f;
      p.vel[0] += wind_dir[0] * wind_speed * w_inf * step_dt;
      p.vel[2] += wind_dir[2] * wind_speed * w_inf * step_dt;
    }

    p.pos[0] += p.vel[0] * step_dt;
    p.pos[1] += p.vel[1] * step_dt;
    p.pos[2] += p.vel[2] * step_dt;
    p.spin += p.spin_rate * step_dt;

    if (write != i) particles_[write] = p;
    ++write;
  }
  particles_.resize(write);
  if (lib_ && !child_spawns.empty()) {
    for (const auto& cs : child_spawns) {
      const auto* cdef = lib_->find_effect(cs.effect);
      if (!cdef) continue;
      const int n = std::clamp(int(std::lround(std::max(3.0f, cdef->quantity * 0.25f))), 3, 12);
      for (int k = 0; k < n && particles_.size() < kMaxParticles; ++k) {
        spawn_one(*cdef, cs.pos, {0.0f, 1.0f, 0.0f}, cs.vel, 1.0f, cs.depth, cs.world_ambient);
      }
    }
  }
}

void ParticleSystem::build_sprites(const std::array<float, 3>& eye_game, std::vector<ParticleSprite>& alpha_out,
                                   std::vector<ParticleSprite>& additive_out) const {
  alpha_out.clear();
  additive_out.clear();
  if (particles_.empty()) return;

  struct KeyedSprite {
    float dist2;
    ParticleSprite sprite;
  };
  std::vector<KeyedSprite> alpha_sorted;
  alpha_sorted.reserve(particles_.size());

  for (const auto& p : particles_) {
    if (!p.def) continue;
    const auto& d = *p.def;
    const float u = std::clamp(p.age / std::max(p.life, 1e-4f), 0.0f, 1.0f);

    // Evaluate size growth/shrink curve over normalized lifetime [0, 1].
    auto eval_size = [](float s0, float smax, float send, float grow_for, float shrink_for, float t) {
      const float start = (s0 <= 1e-4f && grow_for <= 1e-4f) ? smax : s0;
      float cur = start;
      if (grow_for > 1e-4f && t < grow_for) {
        cur = start + (smax - start) * (t / grow_for);
      } else if (grow_for > 1e-4f) {
        cur = smax;
      }
      if (shrink_for > 1e-4f && t > 1.0f - shrink_for) {
        const float k = std::clamp((t - (1.0f - shrink_for)) / shrink_for, 0.0f, 1.0f);
        cur = cur + (send - cur) * k;
      }
      return std::max(cur, 0.015f);
    };
    float sx = eval_size(d.size_x, d.max_size_x, d.end_size_x, d.grow_for_x, d.shrink_for_x, u) * p.size_scale_x;
    float sy = eval_size(d.size_y, d.max_size_y, d.end_size_y, d.grow_for_y, d.shrink_for_y, u) * p.size_scale_y;
    float max_scr = std::clamp(d.max_screen_size, 0.06f, 0.85f);

    // Evaluate 3-stop RGBA colour & alpha ramp (RGBStart -> RGBMid -> RGBEnd).
    const float f_in = std::clamp(d.fade_in_to, 0.01f, 0.95f);
    const float f_out = std::clamp(d.fade_out_from, f_in, 0.99f);
    std::array<float, 4> rgba{};
    if (u < f_in) {
      const float k = u / f_in;
      for (int c = 0; c < 4; ++c) rgba[c] = d.rgb_start[c] + (d.rgb_mid[c] - d.rgb_start[c]) * k;
    } else if (u < f_out) {
      rgba = d.rgb_mid;
    } else {
      const float k = (u - f_out) / std::max(1.0f - f_out, 1e-4f);
      for (int c = 0; c < 4; ++c) rgba[c] = d.rgb_mid[c] + (d.rgb_end[c] - d.rgb_mid[c]) * k;
    }
    rgba[3] = std::clamp(rgba[3] * p.alpha_scale, 0.0f, 1.0f);

    // Constrain world-ambient flare & festival emitters to authentic FH1 billboard scales
    // so ground activation flares and smoke plumes never expand into giant overlapping sheets.
    if (p.world_ambient) {
      const bool is_flare = (d.name.find("flare") != std::string::npos);
      const float max_dim = is_flare ? (d.additive ? 1.55f : 2.10f) : 4.0f;
      sx = std::min(sx, max_dim);
      sy = std::min(sy, max_dim);
      max_scr = std::min(max_scr, is_flare ? 0.28f : 0.40f);
      if (is_flare) {
        rgba[3] *= d.additive ? 0.62f : 0.72f;
      }
    }
    if (rgba[3] <= 0.004f) continue;

    ParticleSprite sp;
    sp.pos = p.pos;
    sp.vel = p.vel;
    sp.size_x = sx;
    sp.size_y = sy;
    sp.spin_rad = p.spin;
    const bool is_soft = (d.shader_kind == ParticleShaderKind::kSoft ||
                          d.shader_kind == ParticleShaderKind::kAnimatedSoft ||
                          d.shader_kind == ParticleShaderKind::kAnimatedRowOnlySoft ||
                          d.shader_kind == ParticleShaderKind::kGradientSoft ||
                          p.world_ambient);
    sp.soft_depth_m = is_soft ? 0.55f : 0.0f;
    sp.rgba = rgba;
    sp.light_influence = d.light_influence;
    sp.headlight_power = d.headlight_power;
    sp.max_screen_size = max_scr;
    sp.frames_x = std::max(1, d.num_frames_x);
    sp.frames_y = std::max(1, d.num_frames_y);
    if (d.shader_kind == ParticleShaderKind::kAnimatedRowOnly ||
        d.shader_kind == ParticleShaderKind::kAnimatedRowOnlyGradient ||
        d.shader_kind == ParticleShaderKind::kAnimatedRowOnlySoft) {
      const int col = int(u * float(sp.frames_x) * std::max(d.anim_rate, 0.5f)) % sp.frames_x;
      sp.frame_index = p.anim_row * sp.frames_x + col;
    } else {
      const int total = sp.frames_x * sp.frames_y;
      sp.frame_index = (total > 1) ? (int(u * float(total) * std::max(d.anim_rate, 0.5f)) % total) : 0;
    }
    sp.texture_slot = p.tex_slot;
    sp.texture2_slot = p.tex2_slot;
    sp.gradient_row_v = (float(p.grad_row) + 0.5f) / float(std::max(1, d.num_gradient_rows));
    sp.use_gradient = (d.shader_kind == ParticleShaderKind::kGradient ||
                       d.shader_kind == ParticleShaderKind::kGradientSoft ||
                       d.shader_kind == ParticleShaderKind::kAnimatedGradient ||
                       d.shader_kind == ParticleShaderKind::kAnimatedRowOnlyGradient);
    sp.additive = d.additive;
    sp.velocity_aligned = d.velocity_quad;
    sp.dynamic_lighting = d.dynamic_lighting;

    if (sp.additive) {
      additive_out.push_back(sp);
    } else {
      const float dx = p.pos[0] - eye_game[0];
      const float dy = p.pos[1] - eye_game[1];
      const float dz = p.pos[2] - eye_game[2];
      alpha_sorted.push_back({dx * dx + dy * dy + dz * dz, sp});
    }
  }

  std::sort(alpha_sorted.begin(), alpha_sorted.end(),
            [](const KeyedSprite& a, const KeyedSprite& b) { return a.dist2 > b.dist2; });
  alpha_out.reserve(alpha_sorted.size());
  for (auto& ks : alpha_sorted) alpha_out.push_back(ks.sprite);
}

// ---------------------------------------------------------------------------
// SkidMarkSystem (CSkidMarks + surfaceTypes.xml <SkidData>)
// ---------------------------------------------------------------------------

SkidMarkSystem::SkidMarkSystem() { ring_.resize(kMaxSegments); }

void SkidMarkSystem::clear() {
  head_ = 0;
  count_ = 0;
  next_seq_ = 1;
  current_time_s_ = 0.0f;
  break_all();
}

void SkidMarkSystem::break_wheel(size_t wheel_slot) {
  if (wheel_slot < strips_.size()) strips_[wheel_slot].active = false;
}

void SkidMarkSystem::break_all() {
  for (auto& s : strips_) s.active = false;
}

void SkidMarkSystem::truncate_after(float time_s) {
  break_all();
  if (count_ == 0) {
    current_time_s_ = std::min(current_time_s_, time_s);
    return;
  }
  const size_t start = (head_ + kMaxSegments - count_) % kMaxSegments;
  std::vector<Segment> kept;
  kept.reserve(count_);
  for (size_t i = 0; i < count_; ++i) {
    const Segment& seg = ring_[(start + i) % kMaxSegments];
    if (seg.time_s <= time_s) kept.push_back(seg);
  }
  const size_t new_count = std::min(kept.size(), kMaxSegments);
  for (size_t i = 0; i < new_count; ++i) ring_[i] = kept[i];
  count_ = new_count;
  head_ = new_count % kMaxSegments;
  current_time_s_ = std::min(current_time_s_, time_s);
}

float SkidMarkSystem::slip_intensity(float slip_combined, float min_intensity, float wheel_speed_mps) {
  // Map combined normalized slip [0.55, 1.45] -> [0, 1] per SPEC-VFX-AND-PARTICLES.
  const float slip_i = std::clamp((slip_combined - 0.55f) / 0.90f, 0.0f, 1.0f);
  const float min_i = (wheel_speed_mps > 0.6f) ? std::clamp(min_intensity, 0.0f, 1.0f) : 0.0f;
  return std::max(slip_i, min_i);
}

void SkidMarkSystem::update_wheel(size_t wheel_slot, const WheelVfxInput& in, float time_s) {
  if (time_s >= 0.0f) current_time_s_ = time_s;
  if (wheel_slot >= strips_.size()) return;
  auto& st = strips_[wheel_slot];
  if (!in.grounded || in.surface.skid_a == 0) {
    st.active = false;
    return;
  }

  const float intensity = slip_intensity(in.slip_combined, in.surface.skid_min_intensity, in.wheel_speed_mps);
  const float alpha_f = (float(in.surface.skid_a) / 255.0f) * intensity;
  if (alpha_f <= 0.015f) {
    st.active = false;
    return;
  }

  const int tex_idx = (in.surface.skid_texture_index == 1) ? 1 : 0;
  const auto n = vec_norm(in.surface_normal, {0.0f, 1.0f, 0.0f});
  // Lift 3 cm above the collision polygon to avoid Z-fighting on curved road crests.
  constexpr float kLift = 0.030f;
  const std::array<float, 3> center = {
      in.contact_pos[0] + n[0] * kLift,
      in.contact_pos[1] + n[1] * kLift,
      in.contact_pos[2] + n[2] * kLift,
  };
  const float half_w = std::clamp(in.tyre_width_m * 0.55f, 0.09f, 0.25f);
  const auto right = vec_norm(in.wheel_right, {1.0f, 0.0f, 0.0f});
  const std::array<float, 3> left_p = {center[0] - right[0] * half_w, center[1] - right[1] * half_w,
                                       center[2] - right[2] * half_w};
  const std::array<float, 3> right_p = {center[0] + right[0] * half_w, center[1] + right[1] * half_w,
                                        center[2] + right[2] * half_w};
  const std::array<uint8_t, 4> rgba = {
      in.surface.skid_r,
      in.surface.skid_g,
      in.surface.skid_b,
      uint8_t(std::clamp(alpha_f * 255.0f, 0.0f, 255.0f)),
  };

  if (!st.active || st.texture_index != tex_idx) {
    st.active = true;
    st.prev_center = center;
    st.prev_left = left_p;
    st.prev_right = right_p;
    st.prev_normal = n;
    st.prev_rgba = rgba;
    st.v_coord = 0.0f;
    st.texture_index = tex_idx;
    return;
  }

  const float dx = center[0] - st.prev_center[0];
  const float dy = center[1] - st.prev_center[1];
  const float dz = center[2] - st.prev_center[2];
  const float dist2 = dx * dx + dy * dy + dz * dz;
  if (dist2 > 16.0f) {
    // Teleport or rewind jump: restart strip cleanly without stretching a giant quad.
    st.prev_center = center;
    st.prev_left = left_p;
    st.prev_right = right_p;
    st.prev_normal = n;
    st.prev_rgba = rgba;
    st.v_coord = 0.0f;
    return;
  }
  if (dist2 < 0.0625f) return;  // wait until >= 0.25 m travelled

  const float dist = std::sqrt(dist2);
  const float next_v = st.v_coord + dist * 0.65f;

  Segment seg;
  seg.texture_index = tex_idx;
  seg.seq = next_seq_++;
  seg.time_s = (time_s >= 0.0f) ? time_s : current_time_s_;
  seg.v[0] = {st.prev_left, st.prev_normal, {0.0f, st.v_coord}, st.prev_rgba};
  seg.v[1] = {st.prev_right, st.prev_normal, {1.0f, st.v_coord}, st.prev_rgba};
  seg.v[2] = {right_p, n, {1.0f, next_v}, rgba};
  seg.v[3] = {left_p, n, {0.0f, next_v}, rgba};

  ring_[head_] = seg;
  head_ = (head_ + 1) % kMaxSegments;
  if (count_ < kMaxSegments) ++count_;

  st.prev_center = center;
  st.prev_left = left_p;
  st.prev_right = right_p;
  st.prev_normal = n;
  st.prev_rgba = rgba;
  st.v_coord = next_v;
}

size_t SkidMarkSystem::count_by_texture(int texture_index) const {
  size_t n = 0;
  const size_t start = (head_ + kMaxSegments - count_) % kMaxSegments;
  for (size_t i = 0; i < count_; ++i) {
    const auto& seg = ring_[(start + i) % kMaxSegments];
    if (seg.texture_index == texture_index) ++n;
  }
  return n;
}

void SkidMarkSystem::build_geometry(std::vector<SkidVertex>& asphalt_tris, std::vector<SkidVertex>& rut_tris) const {
  asphalt_tris.clear();
  rut_tris.clear();
  if (count_ == 0) return;

  const size_t start = (head_ + kMaxSegments - count_) % kMaxSegments;
  // Smoothly fade the oldest 15% of segments once the ring approaches capacity.
  const size_t fade_len = (count_ > kMaxSegments / 2) ? (kMaxSegments * 15 / 100) : 0;

  for (size_t i = 0; i < count_; ++i) {
    const auto& seg = ring_[(start + i) % kMaxSegments];
    float tail = 1.0f;
    if (fade_len > 0 && i < fade_len) {
      tail = float(i + 1) / float(fade_len + 1);
    }
    auto apply_tail = [tail](SkidVertex v) {
      if (tail < 0.999f) v.rgba[3] = uint8_t(float(v.rgba[3]) * tail);
      return v;
    };
    const SkidVertex v0 = apply_tail(seg.v[0]);
    const SkidVertex v1 = apply_tail(seg.v[1]);
    const SkidVertex v2 = apply_tail(seg.v[2]);
    const SkidVertex v3 = apply_tail(seg.v[3]);

    auto& dst = (seg.texture_index == 1) ? rut_tris : asphalt_tris;
    dst.push_back(v0);
    dst.push_back(v1);
    dst.push_back(v2);
    dst.push_back(v0);
    dst.push_back(v2);
    dst.push_back(v3);
  }
}

}  // namespace world
