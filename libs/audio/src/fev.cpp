#include "audio/fev.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#include "audio/xml.h"
#include "cars/mods.h"

namespace audio {
namespace {

struct Reader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t at = 0;
  std::string err;

  bool fail(std::string what) {
    if (err.empty()) err = std::move(what) + " at offset " + std::to_string(at);
    return false;
  }
  bool need(size_t n) {
    if (at + n > size) return fail("unexpected EOF");
    return true;
  }
  bool skip(size_t n) {
    if (!need(n)) return false;
    at += n;
    return true;
  }
  bool u16(uint16_t& v) {
    if (!need(2)) return false;
    v = uint16_t(data[at]) | uint16_t(data[at + 1]) << 8;
    at += 2;
    return true;
  }
  bool i16(int16_t& v) {
    uint16_t u = 0;
    if (!u16(u)) return false;
    v = int16_t(u);
    return true;
  }
  bool u32(uint32_t& v) {
    if (!need(4)) return false;
    v = uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16 |
        uint32_t(data[at + 3]) << 24;
    at += 4;
    return true;
  }
  bool i32(int32_t& v) {
    uint32_t u = 0;
    if (!u32(u)) return false;
    v = int32_t(u);
    return true;
  }
  bool u64(uint64_t& v) {
    uint32_t lo = 0, hi = 0;
    if (!u32(lo) || !u32(hi)) return false;
    v = uint64_t(lo) | (uint64_t(hi) << 32);
    return true;
  }
  bool f32(float& v) {
    uint32_t u = 0;
    if (!u32(u)) return false;
    std::memcpy(&v, &u, 4);
    return true;
  }
  bool str(std::string& out) {
    uint32_t len = 0;
    if (!u32(len)) return false;
    if (len > 1024 * 1024 || !need(len)) return fail("invalid string length");
    size_t n = len;
    while (n > 0 && data[at + n - 1] == 0) --n;
    out.assign(reinterpret_cast<const char*>(data + at), n);
    at += len;
    return true;
  }
};

bool skip_user_props(Reader& r) {
  uint32_t count = 0;
  if (!r.u32(count)) return false;
  for (uint32_t i = 0; i < count; ++i) {
    std::string name;
    uint32_t type = 0;
    if (!r.str(name) || !r.u32(type)) return false;
    if (type == 0 || type == 1) {
      if (!r.skip(4)) return false;
    } else if (type == 2) {
      std::string val;
      if (!r.str(val)) return false;
    } else {
      return r.fail("unknown user property type " + std::to_string(type));
    }
  }
  return true;
}

bool read_category(Reader& r, FevCategory& out, int depth) {
  if (depth > 32) return r.fail("category nesting too deep");
  uint32_t sub = 0;
  if (!r.str(out.name) || !r.f32(out.volume) || !r.f32(out.pitch) || !r.u32(out.max_playbacks) ||
      !r.u32(out.max_playbacks_behavior) || !r.u32(sub))
    return false;
  out.subcategories.resize(sub);
  for (uint32_t i = 0; i < sub; ++i)
    if (!read_category(r, out.subcategories[i], depth + 1)) return false;
  return true;
}

bool read_sound_instance(Reader& r, FevSoundInstance& s) {
  return r.u16(s.sound_def_index) && r.f32(s.start_pos) && r.f32(s.length) && r.u32(s.start_mode) &&
         r.u32(s.loop_mode) && r.i32(s.loop_count) && r.u32(s.autopitch_enabled) && r.f32(s.autopitch_ref) &&
         r.f32(s.autopitch_min) && r.f32(s.fine_tune) && r.f32(s.volume) && r.f32(s.fadein_len) &&
         r.f32(s.fadeout_len) && r.u32(s.fadein_type) && r.u32(s.fadeout_type);
}

bool read_envelope(Reader& r, uint32_t version, FevEnvelope& e) {
  if (!r.i32(e.parent_index) || !r.str(e.dsp_name) || !r.u32(e.dsp_param_index) || !r.u32(e.flags)) return false;
  if (version >= 0x00400000u && !r.u32(e.ex_flags)) return false;
  uint32_t pts = 0;
  if (!r.u32(pts)) return false;
  e.points.resize(pts);
  for (uint32_t i = 0; i < pts; ++i)
    if (!r.f32(e.points[i].x) || !r.f32(e.points[i].y) || !r.u32(e.points[i].curve_shape)) return false;
  return r.u32(e.param_index) && r.u32(e.mapping_flags);
}

bool read_event(Reader& r, uint32_t version, const std::string& prefix, FevEvent& ev) {
  if (!r.u32(ev.event_type) || !r.str(ev.name)) return false;
  ev.path = prefix + "/" + ev.name;
  if (version >= 0x00400000u && !r.skip(16)) return false;  // GUID
  if (!r.f32(ev.volume) || !r.f32(ev.pitch) || !r.f32(ev.pitch_rand) || !r.f32(ev.volume_rand) ||
      !r.u32(ev.priority) || !r.u32(ev.max_playbacks))
    return false;
  if (version >= 0x00400000u && !r.u32(ev.steal_priority)) return false;
  if (!r.u32(ev.mode)) return false;
  // 25 x 4B fixed 3D/mix fields.
  if (!r.f32(ev.min_distance) || !r.f32(ev.max_distance)) return false;
  for (float& sp : ev.speaker_level)
    if (!r.f32(sp)) return false;
  if (!r.f32(ev.cone_inside_angle) || !r.f32(ev.cone_outside_angle) || !r.f32(ev.cone_outside_volume) ||
      !r.u32(ev.max_playbacks_behavior) || !r.f32(ev.doppler_factor) || !r.f32(ev.reverb_dry_level) ||
      !r.f32(ev.reverb_wet_level) || !r.f32(ev.speaker_spread) || !r.u32(ev.fadein_ms) || !r.u32(ev.fadeout_ms) ||
      !r.f32(ev.spawn_intensity) || !r.f32(ev.pan_level) || !r.u32(ev.position_rand) || !r.skip(16))
    return false;

  if (ev.event_type == 0x10u) {
    uint32_t inst_count = 0;
    if (!r.u32(inst_count)) return false;
    ev.simple_instances.resize(inst_count);
    for (uint32_t i = 0; i < inst_count; ++i)
      if (!read_sound_instance(r, ev.simple_instances[i])) return false;
    uint32_t cat_count = 0;
    if (!r.u32(cat_count)) return false;
    ev.categories.resize(cat_count);
    for (uint32_t i = 0; i < cat_count; ++i)
      if (!r.str(ev.categories[i])) return false;
    return true;
  }
  if (ev.event_type == 0x08u) {
    uint32_t layer_count = 0;
    if (!r.u32(layer_count)) return false;
    ev.layers.resize(layer_count);
    for (uint32_t li = 0; li < layer_count; ++li) {
      FevLayer& ly = ev.layers[li];
      uint16_t ic = 0, ec = 0;
      if (!r.u16(ly.signature) || !r.i16(ly.priority) || !r.u16(ly.control_param_index) || !r.u16(ic) || !r.u16(ec))
        return false;
      ly.instances.resize(ic);
      for (uint16_t i = 0; i < ic; ++i)
        if (!read_sound_instance(r, ly.instances[i])) return false;
      ly.envelopes.resize(ec);
      for (uint16_t i = 0; i < ec; ++i)
        if (!read_envelope(r, version, ly.envelopes[i])) return false;
    }
    uint32_t param_count = 0;
    if (!r.u32(param_count)) return false;
    ev.params.resize(param_count);
    for (uint32_t pi = 0; pi < param_count; ++pi) {
      FevParameter& p = ev.params[pi];
      uint32_t sc = 0;
      if (!r.str(p.name) || !r.f32(p.velocity) || !r.f32(p.min_val) || !r.f32(p.max_val) || !r.u32(p.flags) ||
          !r.f32(p.seek_speed) || !r.u32(p.loop_behavior) || !r.u32(sc))
        return false;
      p.sustain_points.resize(sc);
      for (uint32_t si = 0; si < sc; ++si)
        if (!r.f32(p.sustain_points[si])) return false;
    }
    if (!skip_user_props(r)) return false;
    uint32_t cat_count = 0;
    if (!r.u32(cat_count)) return false;
    ev.categories.resize(cat_count);
    for (uint32_t i = 0; i < cat_count; ++i)
      if (!r.str(ev.categories[i])) return false;
    return true;
  }
  return r.fail("unknown event_type " + std::to_string(ev.event_type));
}

bool read_event_group(Reader& r, uint32_t version, const std::string& prefix, std::vector<FevEvent>& events,
                      int depth) {
  if (depth > 32) return r.fail("event group nesting too deep");
  std::string name;
  if (!r.str(name) || !skip_user_props(r)) return false;
  uint32_t sub_count = 0, ev_count = 0;
  if (!r.u32(sub_count) || !r.u32(ev_count)) return false;
  const std::string group_path = prefix + "/" + name;
  for (uint32_t i = 0; i < sub_count; ++i)
    if (!read_event_group(r, version, group_path, events, depth + 1)) return false;
  for (uint32_t i = 0; i < ev_count; ++i) {
    FevEvent ev;
    if (!read_event(r, version, group_path, ev)) return false;
    events.push_back(std::move(ev));
  }
  return true;
}

}  // namespace

float FevEnvelope::eval(float x) const {
  if (points.empty()) return 1.0f;
  if (x <= points.front().x) return points.front().y;
  if (x >= points.back().x) return points.back().y;
  for (size_t i = 0; i + 1 < points.size(); ++i) {
    const auto& a = points[i];
    const auto& b = points[i + 1];
    if (x <= b.x) {
      const float span = b.x - a.x;
      if (span <= 1e-9f) return b.y;
      return a.y + (b.y - a.y) * ((x - a.x) / span);
    }
  }
  return points.back().y;
}

int FevEvent::find_param(std::string_view param_name) const {
  for (size_t i = 0; i < params.size(); ++i)
    if (iequals(params[i].name, param_name)) return int(i);
  return -1;
}

float fev_normalize(const FevParameter& p, float raw) {
  const float span = p.max_val - p.min_val;
  if (!(std::fabs(span) > 1e-12f)) return 0.0f;
  const float x = (raw - p.min_val) / span;
  if (!(x > 0.0f)) return 0.0f;
  return x < 1.0f ? x : 1.0f;
}

int evaluate_fev_event(const FevEvent& ev, const float* raw, size_t raw_count, FevInstanceLevel* out, int cap) {
  auto norm_of = [&](size_t pi) -> float {
    if (pi >= ev.params.size()) return 0.0f;
    const float v = (raw && pi < raw_count && std::isfinite(raw[pi])) ? raw[pi] : ev.params[pi].min_val;
    return fev_normalize(ev.params[pi], v);
  };
  int n = 0;
  for (size_t li = 0; li < ev.layers.size(); ++li) {
    const FevLayer& ly = ev.layers[li];
    float env_gain = 1.0f;
    float pitch_y = 0.5f;
    bool has_pitch = false;
    for (const auto& e : ly.envelopes) {
      if (!e.dsp_name.empty() || e.parent_index >= 0) continue;
      if (e.flags & kFevEnvVolume) {
        env_gain *= std::max(0.0f, e.eval(norm_of(e.param_index)));
      } else if (e.flags & kFevEnvPitch) {
        pitch_y += e.eval(norm_of(e.param_index)) - 0.5f;
        has_pitch = true;
      }
    }
    const float ctrl = norm_of(ly.control_param_index);
    for (size_t ii = 0; ii < ly.instances.size() && n < cap; ++ii) {
      const FevSoundInstance& s = ly.instances[ii];
      float fade = 0.0f;
      const float end = s.start_pos + s.length;
      if (s.length > 0.0f && ctrl >= s.start_pos - 1e-6f && ctrl <= end + 1e-6f) {
        fade = 1.0f;
        if (s.fadein_len > 0.0f) {
          const float w = s.fadein_len * s.length;
          if (w > 1e-9f) fade = std::min(fade, std::clamp((ctrl - s.start_pos) / w, 0.0f, 1.0f));
        }
        if (s.fadeout_len > 0.0f) {
          const float w = s.fadeout_len * s.length;
          if (w > 1e-9f) fade = std::min(fade, std::clamp((end - ctrl) / w, 0.0f, 1.0f));
        }
      }
      FevInstanceLevel& o = out[n++];
      o.layer = uint16_t(li);
      o.instance = uint16_t(ii);
      o.sound_def = s.sound_def_index;
      o.gain = std::max(0.0f, ev.volume) * std::max(0.0f, s.volume) * fade * env_gain;
      o.pitch_env = pitch_y;
      o.has_pitch_env = has_pitch;
    }
  }
  return n;
}

const FevEvent* FevProject::find_event(std::string_view path) const {
  for (const auto& ev : events) {
    if (iequals(ev.path, path)) return &ev;
    // Also accept path without leading "<project>/" prefix.
    const size_t slash = ev.path.find('/');
    if (slash != std::string::npos && iequals(std::string_view(ev.path).substr(slash + 1), path)) return &ev;
  }
  return nullptr;
}

bool parse_fev(std::span<const uint8_t> bytes, FevProject& out, std::string* error) {
  out = FevProject{};
  Reader r{bytes.data(), bytes.size(), 0, {}};
  auto done_fail = [&]() {
    if (error) *error = r.err;
    return false;
  };
  if (!r.need(16) || std::memcmp(r.data, "FEV1", 4) != 0) {
    r.fail("not an FEV1 file");
    return done_fail();
  }
  r.at = 4;
  uint32_t mem_a = 0, mem_b = 0;
  if (!r.u32(out.version) || !r.u32(mem_a) || !r.u32(mem_b)) return done_fail();
  if (out.version != 0x00370000u && out.version != 0x00400000u) {
    r.fail("unsupported FEV1 version " + std::to_string(out.version));
    return done_fail();
  }
  if (out.version >= 0x00400000u) {
    uint32_t meta = 0;
    if (!r.u32(meta) || !r.skip(size_t(meta) * 8)) return done_fail();
  }
  if (!r.str(out.name)) return done_fail();

  // 3. Wavebanks
  uint32_t wb_count = 0;
  if (!r.u32(wb_count)) return done_fail();
  out.wavebanks.resize(wb_count);
  for (uint32_t i = 0; i < wb_count; ++i) {
    FevWavebank& wb = out.wavebanks[i];
    if (!r.u32(wb.bank_type) || !r.u32(wb.max_streams)) return done_fail();
    if (out.version >= 0x00400000u && !r.u64(wb.hash)) return done_fail();
    if (!r.str(wb.name)) return done_fail();
  }

  // 4. Category tree
  if (!read_category(r, out.root_category, 0)) return done_fail();

  // 5. Event groups
  uint32_t top_groups = 0;
  if (!r.u32(top_groups)) return done_fail();
  for (uint32_t i = 0; i < top_groups; ++i)
    if (!read_event_group(r, out.version, out.name, out.events, 0)) return done_fail();

  // 6. Sound-definition property templates
  uint32_t tpl_count = 0;
  if (!r.u32(tpl_count)) return done_fail();
  const size_t tpl_size = out.version >= 0x00400000u ? 70 : 60;
  if (tpl_count > 100000u) {
    r.fail("implausible template count");
    return done_fail();
  }
  out.templates.resize(tpl_count);
  for (uint32_t i = 0; i < tpl_count; ++i) {
    FevSoundDefTemplate& t = out.templates[i];
    if (!r.u32(t.play_mode) || !r.u32(t.spawn_min_ms) || !r.u32(t.spawn_max_ms) || !r.u32(t.max_spawned) ||
        !r.f32(t.volume) || !r.skip(tpl_size - 20))
      return done_fail();
    if (!std::isfinite(t.volume) || t.volume < 0.0f) t.volume = 1.0f;
  }

  // 7. Sound definitions
  uint32_t sd_count = 0;
  if (!r.u32(sd_count)) return done_fail();
  out.sound_defs.resize(sd_count);
  for (uint32_t i = 0; i < sd_count; ++i) {
    FevSoundDef& sd = out.sound_defs[i];
    uint32_t ec = 0;
    if (!r.str(sd.name) || !r.u32(sd.template_index) || !r.u32(ec)) return done_fail();
    sd.entries.resize(ec);
    for (uint32_t k = 0; k < ec; ++k) {
      FevSoundDefEntry& en = sd.entries[k];
      if (!r.u32(en.entry_type) || !r.u32(en.weight)) return done_fail();
      if (en.entry_type == 0) {
        if (!r.str(en.source_wav) || !r.str(en.wavebank_name) || !r.u32(en.subsound_index) || !r.u32(en.length_ms))
          return done_fail();
      } else if (en.entry_type == 1) {
        if (!r.u32(en.osc_type) || !r.f32(en.osc_freq)) return done_fail();
      } else if (en.entry_type == 2 || en.entry_type == 3) {
        // silence or programmer sound: no extra fields
      } else {
        r.fail("unknown sound def entry_type " + std::to_string(en.entry_type));
        return done_fail();
      }
    }
  }

  // 8. Reverb definitions and trailing chunk ("comp")
  if (!r.u32(out.reverb_count)) return done_fail();
  if (out.reverb_count != 0) {
    r.fail("unexpected non-zero reverb_count " + std::to_string(out.reverb_count));
    return done_fail();
  }
  if (r.at < r.size) {
    // Trailing chunk: u32 chunk_size + "comp" (and optional "sett" subchunk).
    if (!r.need(8)) return done_fail();
    if (std::memcmp(r.data + r.at + 4, "comp", 4) != 0) {
      r.fail("expected trailing comp chunk");
      return done_fail();
    }
    r.at = r.size;
  }
  return true;
}

bool load_fev(const std::filesystem::path& path, FevProject& out, std::string* error) {
  const auto resolved = cars::mods::resolve(path);
  std::ifstream f(resolved, std::ios::binary);
  if (!f) {
    if (error) *error = "cannot open " + resolved.string();
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!parse_fev(bytes, out, error)) return false;
  out.dir = resolved.parent_path();
  return true;
}

}  // namespace audio
