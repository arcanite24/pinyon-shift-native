#include "world/world_anim.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>

#include "fh1data/archive.h"

namespace world {

namespace {

constexpr float kPi = 3.14159265358979323846f;

std::string strip_xml_comments(const std::string& xml) {
  std::string out;
  out.reserve(xml.size());
  size_t pos = 0;
  while (pos < xml.size()) {
    const size_t open = xml.find("<!--", pos);
    if (open == std::string::npos) {
      out.append(xml, pos, std::string::npos);
      break;
    }
    out.append(xml, pos, open - pos);
    const size_t close = xml.find("-->", open + 4);
    if (close == std::string::npos) break;
    pos = close + 3;
  }
  return out;
}

std::string attr_value(const std::string& tag, const char* name) {
  const std::string key = std::string(name) + "=\"";
  size_t p = tag.find(key);
  if (p == std::string::npos) return {};
  p += key.size();
  const size_t end = tag.find('"', p);
  return end == std::string::npos ? std::string{} : tag.substr(p, end - p);
}

std::string lower_ascii(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

uint32_t hash_u32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

float hash_01(int ix, int iz, uint32_t salt) {
  const uint32_t h = hash_u32(uint32_t(ix) * 374761393u ^ uint32_t(iz) * 668265263u ^ salt * 2246822519u);
  return float(h & 0x00FFFFFFu) / float(0x01000000u);
}

std::array<float, 3> vsub(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

std::array<float, 3> vcross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

std::array<float, 3> vnorm(const std::array<float, 3>& a) {
  const float l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  return l > 1e-6f ? std::array<float, 3>{a[0] / l, a[1] / l, a[2] / l} : std::array<float, 3>{0.0f, 1.0f, 0.0f};
}

void push_tri(std::vector<DebrisVertex>& out, const std::array<float, 3>& a, const std::array<float, 3>& b,
              const std::array<float, 3>& c, const std::array<uint8_t, 4>& rgba, bool double_sided = false) {
  const auto n = vnorm(vcross(vsub(b, a), vsub(c, a)));
  out.push_back({a, n, rgba});
  out.push_back({b, n, rgba});
  out.push_back({c, n, rgba});
  if (double_sided) {
    const std::array<float, 3> nf{-n[0], -n[1], -n[2]};
    out.push_back({a, nf, rgba});
    out.push_back({c, nf, rgba});
    out.push_back({b, nf, rgba});
  }
}

void push_quad(std::vector<DebrisVertex>& out, const std::array<float, 3>& a, const std::array<float, 3>& b,
               const std::array<float, 3>& c, const std::array<float, 3>& d, const std::array<uint8_t, 4>& rgba,
               bool double_sided = false) {
  push_tri(out, a, b, c, rgba, double_sided);
  push_tri(out, a, c, d, rgba, double_sided);
}

void push_box(std::vector<DebrisVertex>& out, const std::array<float, 3>& c, const std::array<float, 3>& rx,
              const std::array<float, 3>& ry, const std::array<float, 3>& rz, const std::array<uint8_t, 4>& rgba) {
  auto pt = [&](float sx, float sy, float sz) -> std::array<float, 3> {
    return {c[0] + rx[0] * sx + ry[0] * sy + rz[0] * sz, c[1] + rx[1] * sx + ry[1] * sy + rz[1] * sz,
            c[2] + rx[2] * sx + ry[2] * sy + rz[2] * sz};
  };
  const auto p000 = pt(-1, -1, -1), p100 = pt(1, -1, -1), p110 = pt(1, 1, -1), p010 = pt(-1, 1, -1);
  const auto p001 = pt(-1, -1, 1), p101 = pt(1, -1, 1), p111 = pt(1, 1, 1), p011 = pt(-1, 1, 1);
  push_quad(out, p001, p101, p111, p011, rgba);
  push_quad(out, p100, p000, p010, p110, rgba);
  push_quad(out, p011, p111, p110, p010, rgba);
  push_quad(out, p000, p100, p101, p001, rgba);
  push_quad(out, p101, p100, p110, p111, rgba);
  push_quad(out, p000, p001, p011, p010, rgba);
}

uint8_t clamp_u8(float v) { return uint8_t(std::clamp(int(std::lround(v)), 0, 255)); }

}  // namespace

bool SpectatorCatalog::load_spectators_xml(const std::string& raw_xml) {
  models.clear();
  modelsets.clear();
  skeletons.clear();
  crowdclasses.clear();
  const std::string xml = strip_xml_comments(raw_xml);

  // <models ... numSpriteAngles="10" bSpritesBothSides="False">
  {
    const size_t p = xml.find("<models");
    if (p != std::string::npos) {
      const size_t gt = xml.find('>', p);
      if (gt != std::string::npos) {
        const std::string tag = xml.substr(p, gt - p + 1);
        const std::string na = attr_value(tag, "numSpriteAngles");
        if (!na.empty()) num_sprite_angles = std::max(1, std::atoi(na.c_str()));
        const std::string bs = lower_ascii(attr_value(tag, "bSpritesBothSides"));
        sprites_both_sides = (bs == "true" || bs == "1");
      }
      const size_t end = xml.find("</models>", p);
      if (end != std::string::npos && gt != std::string::npos) {
        size_t cur = gt + 1;
        while (cur < end) {
          const size_t mp = xml.find("<model ", cur);
          if (mp == std::string::npos || mp >= end) break;
          const size_t mgt = xml.find('>', mp);
          if (mgt == std::string::npos || mgt >= end) break;
          const std::string mtag = xml.substr(mp, mgt - mp + 1);
          SpectatorModelDef def;
          def.name = attr_value(mtag, "name");
          const std::string spt = attr_value(mtag, "spritesPerTex");
          if (!spt.empty()) def.sprites_per_tex = std::max(1, std::atoi(spt.c_str()));
          const size_t mclose = xml.find("</model>", mgt);
          if (mclose != std::string::npos && mclose < end) {
            const size_t tp = xml.find("<texture ", mgt);
            if (tp != std::string::npos && tp < mclose) {
              const size_t tgt = xml.find('>', tp);
              if (tgt != std::string::npos && tgt < mclose) {
                def.texture = attr_value(xml.substr(tp, tgt - tp + 1), "name");
              }
            }
            cur = mclose + 8;
          } else {
            cur = mgt + 1;
          }
          if (!def.name.empty()) models.push_back(std::move(def));
        }
      }
    }
  }

  // <modelset name="..."> <model name="..."/> </modelset>
  {
    size_t pos = 0;
    while (pos < xml.size()) {
      const size_t ms = xml.find("<modelset ", pos);
      if (ms == std::string::npos) break;
      const size_t gt = xml.find('>', ms);
      const size_t end = xml.find("</modelset>", ms);
      if (gt == std::string::npos || end == std::string::npos) break;
      const std::string set_name = attr_value(xml.substr(ms, gt - ms + 1), "name");
      std::vector<std::string> list;
      size_t cur = gt + 1;
      while (cur < end) {
        const size_t mp = xml.find("<model ", cur);
        if (mp == std::string::npos || mp >= end) break;
        const size_t mgt = xml.find('>', mp);
        if (mgt == std::string::npos || mgt >= end) break;
        const std::string mn = attr_value(xml.substr(mp, mgt - mp + 1), "name");
        if (!mn.empty()) list.push_back(mn);
        cur = mgt + 1;
      }
      if (!set_name.empty()) modelsets[set_name] = std::move(list);
      pos = end + 11;
    }
  }

  // <skeleton ...> <animation .../> </skeleton>
  {
    size_t pos = 0;
    while (pos < xml.size()) {
      const size_t sp = xml.find("<skeleton ", pos);
      if (sp == std::string::npos) break;
      const size_t gt = xml.find('>', sp);
      const size_t end = xml.find("</skeleton>", sp);
      if (gt == std::string::npos || end == std::string::npos) break;
      const std::string stag = xml.substr(sp, gt - sp + 1);
      SpectatorSkeletonDef sk;
      sk.name = attr_value(stag, "name");
      sk.suffix = attr_value(stag, "suffix");
      sk.subdir = attr_value(stag, "subdir");
      size_t cur = gt + 1;
      while (cur < end) {
        const size_t ap = xml.find("<animation ", cur);
        if (ap == std::string::npos || ap >= end) break;
        const size_t agt = xml.find('>', ap);
        if (agt == std::string::npos || agt >= end) break;
        const std::string an = attr_value(xml.substr(ap, agt - ap + 1), "name");
        if (!an.empty()) sk.animations.push_back(an);
        cur = agt + 1;
      }
      if (!sk.name.empty()) skeletons.push_back(std::move(sk));
      pos = end + 11;
    }
  }

  // <crowdclass name="..." skeleton="..." modelset="...">
  {
    size_t pos = 0;
    while (pos < xml.size()) {
      const size_t cp = xml.find("<crowdclass ", pos);
      if (cp == std::string::npos) break;
      const size_t gt = xml.find('>', cp);
      if (gt == std::string::npos) break;
      const std::string ctag = xml.substr(cp, gt - cp + 1);
      SpectatorCrowdClass cc;
      cc.name = attr_value(ctag, "name");
      cc.skeleton = attr_value(ctag, "skeleton");
      cc.modelset = attr_value(ctag, "modelset");
      if (!cc.name.empty()) crowdclasses.push_back(std::move(cc));
      pos = gt + 1;
    }
  }

  return !models.empty();
}

bool SpectatorCatalog::load_from_media(const std::filesystem::path& media_dir, std::string* error) {
  spectators_anim_bins = 0;
  spectators_skinbins = 0;
  spectators_xds = 0;
  animated_pgeos = 0;
  animated_xds = 0;

  bool loaded_any = false;
  const auto spec_zip = media_dir / "Spectators.zip";
  if (std::filesystem::exists(spec_zip)) {
    std::string err;
    if (auto ar = fh1::Archive::open(spec_zip, &err)) {
      for (const auto& entry : ar->members()) {
        const std::string low = lower_ascii(entry.name);
        if (low.ends_with(".anim.bin")) ++spectators_anim_bins;
        else if (low.ends_with(".skinbin")) ++spectators_skinbins;
        else if (low.ends_with(".xds")) ++spectators_xds;
        if (low == "spectators.xml" || low.ends_with("/spectators.xml")) {
          const auto rd = ar->read(entry);
          if (rd.ok()) {
            loaded_any = load_spectators_xml(std::string(rd.data.begin(), rd.data.end())) || loaded_any;
          }
        }
      }
    } else if (error) {
      *error = err;
    }
  }

  const auto anim_zip = media_dir / "animatedobjects.zip";
  if (std::filesystem::exists(anim_zip)) {
    std::string err;
    if (auto ar = fh1::Archive::open(anim_zip, &err)) {
      for (const auto& entry : ar->members()) {
        const std::string low = lower_ascii(entry.name);
        if (low.ends_with(".pgeo")) ++animated_pgeos;
        else if (low.ends_with(".xds")) ++animated_xds;
      }
    }
  }
  return loaded_any;
}

AnimatedWorldSystem::AnimatedWorldSystem() { populate_default_colorado(); }

bool AnimatedWorldSystem::init(const std::filesystem::path& media_dir, const TrackPostConfig& post_cfg,
                               std::string* error) {
  post_cfg_ = post_cfg;
  populate_default_colorado();
  return catalog_.load_from_media(media_dir, error);
}

void AnimatedWorldSystem::populate_default_colorado() {
  // Iconic Colorado Horizon Festival & valley hot-air balloons.
  balloons_ = {
      {{2075.0f, 185.0f, 905.0f}, 7.6f, 18.0f, 0.2f, {236, 46, 86}, {250, 212, 48}, {38, 134, 218}},
      {{1915.0f, 202.0f, 1055.0f}, 8.2f, 19.5f, 1.4f, {244, 132, 34}, {245, 245, 240}, {32, 168, 152}},
      {{2145.0f, 196.0f, 1060.0f}, 7.0f, 16.5f, 2.7f, {58, 126, 224}, {252, 232, 92}, {228, 66, 62}},
      {{1840.0f, 190.0f, 860.0f}, 7.8f, 18.2f, 4.1f, {214, 52, 138}, {248, 242, 226}, {246, 176, 42}},
      {{1650.0f, 215.0f, 1210.0f}, 8.4f, 19.8f, 5.2f, {40, 172, 118}, {246, 222, 68}, {232, 64, 52}},
      {{-1420.0f, 165.0f, -290.0f}, 8.0f, 19.0f, 0.9f, {238, 72, 54}, {248, 244, 232}, {52, 118, 212}},
      {{-980.0f, 225.0f, 1540.0f}, 7.8f, 18.5f, 2.2f, {246, 162, 36}, {56, 132, 218}, {244, 244, 242}},
      {{620.0f, 240.0f, -1820.0f}, 8.0f, 19.0f, 3.6f, {162, 56, 204}, {250, 206, 54}, {244, 242, 238}},
      {{1380.0f, 265.0f, -5250.0f}, 7.5f, 17.5f, 1.8f, {236, 56, 82}, {246, 236, 212}, {44, 156, 192}},
  };

  // Colorado ranch & hillside spinning windmills.
  windmills_ = {
      {{2052.0f, 143.0f, 928.0f}, 0.65f, 13.5f, 4.1f, 0.0f},
      {{1890.0f, 142.0f, 1020.0f}, 1.15f, 14.5f, 4.4f, 1.3f},
      {{2185.0f, 148.0f, 845.0f}, -0.45f, 15.0f, 4.5f, 2.6f},
      {{1428.0f, 197.0f, -5348.0f}, 0.35f, 13.8f, 4.0f, 0.8f},
      {{-1525.0f, 92.0f, -395.0f}, 1.80f, 14.2f, 4.3f, 2.1f},
      {{1120.0f, 155.0f, 640.0f}, -0.95f, 14.0f, 4.2f, 3.4f},
  };

  // Waving Horizon festival flags placed behind festival barriers (well off the asphalt road).
  flags_ = {
      {{2018.0f, 144.2f, 932.0f}, 7.2f, 1.75f, 3.4f, 0.0f, {236, 42, 92}, {252, 212, 44}},
      {{2030.0f, 143.8f, 922.0f}, 7.2f, 1.75f, 3.4f, 1.1f, {38, 158, 224}, {248, 246, 240}},
      {{2042.0f, 143.4f, 912.0f}, 7.2f, 1.75f, 3.4f, 2.2f, {246, 146, 32}, {252, 226, 78}},
      {{1980.0f, 145.0f, 954.0f}, 7.2f, 1.75f, 3.4f, 3.3f, {236, 42, 92}, {252, 212, 44}},
      {{1968.0f, 145.2f, 964.0f}, 7.2f, 1.75f, 3.4f, 4.4f, {48, 184, 132}, {248, 244, 232}},
  };

  // Cheering festival spectators behind the festival viewing barriers (off-road).
  crowds_ = {
      {{2012.0f, 144.2f, 934.0f}, 0.80f, -0.60f, 12, 1.15f, "GigCheer01"},
      {{1984.0f, 145.0f, 955.0f}, -0.80f, 0.60f, 10, 1.20f, "Barrier"},
      {{2034.0f, 143.6f, 918.0f}, 0.78f, -0.62f, 10, 1.18f, "GigCheer02"},
  };
}

void AnimatedWorldSystem::build_mesh(double eye_x, double eye_y, double eye_z, float time_sec,
                                     const drive::Ground* ground, std::vector<DebrisVertex>& out,
                                     bool include_grass) const {
  const float wind_spd = std::clamp(post_cfg_.wind_speed, 0.25f, 5.0f);
  const float wind_amp = std::clamp(post_cfg_.wind_amplitude, 0.04f, 0.85f);
  float wx = post_cfg_.wind_dir[0], wz = post_cfg_.wind_dir[2];
  const float wlen = std::sqrt(wx * wx + wz * wz);
  if (wlen > 1e-4f) {
    wx /= wlen;
    wz /= wlen;
  } else {
    wx = 0.866f;
    wz = 0.50f;
  }

  auto ground_y = [&](float x, float z, float fallback) -> float {
    if (!ground) return fallback;
    drive::Hit h;
    const double start_y = std::max(eye_y, double(fallback)) + 30.0;
    return ground->raycast({double(x), start_y, double(z)}, {0.0, -1.0, 0.0}, 120.0, h) ? float(start_y - h.distance)
                                                                                       : fallback;
  };

  // 1. Hot-air balloons (visible up to 1200 m).
  for (const auto& b : balloons_) {
    const double dx = double(b.base_pos[0]) - eye_x;
    const double dz = double(b.base_pos[2]) - eye_z;
    if (dx * dx + dz * dz > 1200.0 * 1200.0) continue;

    const float bob = std::sin(time_sec * 0.42f + b.phase) * 3.2f;
    const float sway_x = std::cos(time_sec * 0.23f + b.phase) * 2.0f;
    const float sway_z = std::sin(time_sec * 0.19f + b.phase * 1.3f) * 2.0f;
    const float rot = time_sec * 0.11f + b.phase;
    const float cx = b.base_pos[0] + sway_x;
    const float cy = b.base_pos[1] + bob;
    const float cz = b.base_pos[2] + sway_z;

    // Envelope profile: 5 latitude rings, 12 longitude gores.
    constexpr int kSegs = 12;
    constexpr int kRings = 5;
    const float ring_h[kRings] = {0.0f, 0.22f, 0.52f, 0.80f, 1.0f};
    const float ring_r[kRings] = {0.22f, 0.68f, 1.00f, 0.84f, 0.0f};

    for (int r = 0; r < kRings - 1; ++r) {
      const float y0 = cy + ring_h[r] * b.height;
      const float y1 = cy + ring_h[r + 1] * b.height;
      const float r0 = ring_r[r] * b.radius;
      const float r1 = ring_r[r + 1] * b.radius;
      for (int s = 0; s < kSegs; ++s) {
        const float a0 = rot + (2.0f * kPi * float(s)) / float(kSegs);
        const float a1 = rot + (2.0f * kPi * float(s + 1)) / float(kSegs);
        const auto& col = ((s + r) % 3 == 0) ? b.primary_rgb : (((s + r) % 3 == 1) ? b.secondary_rgb : b.accent_rgb);
        const std::array<uint8_t, 4> rgba{col[0], col[1], col[2], 255};
        const std::array<float, 3> p00{cx + std::cos(a0) * r0, y0, cz + std::sin(a0) * r0};
        const std::array<float, 3> p10{cx + std::cos(a1) * r0, y0, cz + std::sin(a1) * r0};
        if (r + 1 == kRings - 1) {
          const std::array<float, 3> top{cx, y1, cz};
          push_tri(out, p00, top, p10, rgba);
        } else {
          const std::array<float, 3> p01{cx + std::cos(a0) * r1, y1, cz + std::sin(a0) * r1};
          const std::array<float, 3> p11{cx + std::cos(a1) * r1, y1, cz + std::sin(a1) * r1};
          push_quad(out, p00, p01, p11, p10, rgba);
        }
      }
    }
    // Wicker gondola basket underneath + 4 thin suspension cables.
    const float by = cy - 3.0f;
    push_box(out, {cx, by, cz}, {1.25f, 0, 0}, {0, 1.05f, 0}, {0, 0, 1.25f}, {154, 108, 62, 255});
    // Burner glow core just under mouth.
    const float pulse = 0.7f + 0.3f * std::sin(time_sec * 3.1f + b.phase * 2.0f);
    push_box(out, {cx, cy - 0.55f, cz}, {0.45f, 0, 0}, {0, 0.35f, 0}, {0, 0, 0.45f},
             {clamp_u8(255.0f * pulse), clamp_u8(178.0f * pulse), 52, 255});
    for (int k = 0; k < 4; ++k) {
      const float ca = rot + float(k) * (kPi * 0.5f) + 0.4f;
      const float ox = std::cos(ca), oz = std::sin(ca);
      const std::array<float, 3> bot{cx + ox * 1.15f, by + 1.0f, cz + oz * 1.15f};
      const std::array<float, 3> top{cx + ox * (b.radius * 0.21f), cy + 0.1f, cz + oz * (b.radius * 0.21f)};
      const std::array<float, 3> side{-oz * 0.08f, 0.0f, ox * 0.08f};
      push_quad(out, bot, top, {top[0] + side[0], top[1], top[2] + side[2]},
                {bot[0] + side[0], bot[1], bot[2] + side[2]}, {72, 68, 64, 255}, true);
    }
  }

  // 2. Spinning ranch windmills (visible up to 650 m).
  for (const auto& w : windmills_) {
    const double dx = double(w.base_pos[0]) - eye_x;
    const double dz = double(w.base_pos[2]) - eye_z;
    if (dx * dx + dz * dz > 650.0 * 650.0) continue;

    const float gy = ground_y(w.base_pos[0], w.base_pos[2], w.base_pos[1]);
    const float bx = w.base_pos[0], bz = w.base_pos[2];
    const float top_y = gy + w.tower_height;
    const std::array<uint8_t, 4> steel{148, 152, 156, 255};
    const std::array<uint8_t, 4> rust{164, 96, 62, 255};
    const std::array<uint8_t, 4> blade_col{208, 212, 216, 255};

    // 4 angled lattice legs.
    for (int k = 0; k < 4; ++k) {
      const float a = float(k) * (kPi * 0.5f) + 0.7854f;
      const float cx = std::cos(a), cz = std::sin(a);
      const std::array<float, 3> b0{bx + cx * 1.75f, gy, bz + cz * 1.75f};
      const std::array<float, 3> t0{bx + cx * 0.42f, top_y, bz + cz * 0.42f};
      const std::array<float, 3> th{-cz * 0.14f, 0.0f, cx * 0.14f};
      push_quad(out, b0, t0, {t0[0] + th[0], t0[1], t0[2] + th[2]}, {b0[0] + th[0], b0[1], b0[2] + th[2]}, steel, true);
    }
    // Hub + tail vane.
    const float fy = std::sin(w.yaw), fx = std::cos(w.yaw);
    const std::array<float, 3> fwd{fx, 0.0f, fy};
    const std::array<float, 3> right{-fy, 0.0f, fx};
    const std::array<float, 3> hub{bx + fwd[0] * 0.75f, top_y + 0.25f, bz + fwd[2] * 0.75f};
    push_box(out, {bx, top_y + 0.25f, bz}, {fwd[0] * 0.85f, 0, fwd[2] * 0.85f}, {0, 0.35f, 0},
             {right[0] * 0.35f, 0, right[2] * 0.35f}, rust);
    // Tail vane.
    const std::array<float, 3> vane_c{bx - fwd[0] * 2.6f, top_y + 0.35f, bz - fwd[2] * 2.6f};
    push_box(out, vane_c, {fwd[0] * 1.2f, 0, fwd[2] * 1.2f}, {0, 0.75f, 0}, {right[0] * 0.06f, 0, right[2] * 0.06f},
             rust);

    // 8 spinning rotor blades.
    const float spin = time_sec * (1.25f + wind_spd * 0.9f) + w.phase;
    constexpr int kBlades = 8;
    for (int k = 0; k < kBlades; ++k) {
      const float a = spin + (2.0f * kPi * float(k)) / float(kBlades);
      const float ca = std::cos(a), sa = std::sin(a);
      const std::array<float, 3> rad{right[0] * ca, sa, right[2] * ca};
      const std::array<float, 3> tan{-right[0] * sa, ca, -right[2] * sa};
      const float r_in = 0.55f, r_out = w.blade_radius;
      const float w_in = 0.16f, w_out = 0.42f;
      const std::array<float, 3> p0{hub[0] + rad[0] * r_in - tan[0] * w_in, hub[1] + rad[1] * r_in - tan[1] * w_in,
                                    hub[2] + rad[2] * r_in - tan[2] * w_in};
      const std::array<float, 3> p1{hub[0] + rad[0] * r_out - tan[0] * w_out + fwd[0] * 0.18f,
                                    hub[1] + rad[1] * r_out - tan[1] * w_out,
                                    hub[2] + rad[2] * r_out - tan[2] * w_out + fwd[2] * 0.18f};
      const std::array<float, 3> p2{hub[0] + rad[0] * r_out + tan[0] * w_out - fwd[0] * 0.18f,
                                    hub[1] + rad[1] * r_out + tan[1] * w_out,
                                    hub[2] + rad[2] * r_out + tan[2] * w_out - fwd[2] * 0.18f};
      const std::array<float, 3> p3{hub[0] + rad[0] * r_in + tan[0] * w_in, hub[1] + rad[1] * r_in + tan[1] * w_in,
                                    hub[2] + rad[2] * r_in + tan[2] * w_in};
      push_quad(out, p0, p1, p2, p3, (k % 2 == 0) ? blade_col : rust, true);
    }
  }

  // 3. Waving Horizon Festival flags (cloth ribbon strips deformed by wind_dir/speed/amplitude).
  const float flag_perp_x = -wz, flag_perp_z = wx;
  for (const auto& f : flags_) {
    const double dx = double(f.base_pos[0]) - eye_x;
    const double dz = double(f.base_pos[2]) - eye_z;
    if (dx * dx + dz * dz > 320.0 * 320.0) continue;

    const float gy = ground_y(f.base_pos[0], f.base_pos[2], f.base_pos[1]);
    const float px = f.base_pos[0], pz = f.base_pos[2];
    // Flagpole.
    push_box(out, {px, gy + f.pole_height * 0.5f, pz}, {0.07f, 0, 0}, {0, f.pole_height * 0.5f, 0}, {0, 0, 0.07f},
             {205, 208, 214, 255});

    // 6-column x 3-row waving cloth grid attached to top of pole.
    constexpr int kCols = 6, kRows = 3;
    const float y_top = gy + f.pole_height;
    const float y_bot = y_top - f.height;
    auto cloth_pt = [&](int c, int r) -> std::array<float, 3> {
      const float u = float(c) / float(kCols);
      const float v = float(r) / float(kRows);
      const float wave = std::sin(time_sec * (3.6f + wind_spd * 1.8f) - u * 4.4f + f.phase + v * 0.7f);
      const float flutter = std::sin(time_sec * 7.8f - u * 8.2f + f.phase * 1.9f) * 0.25f;
      const float disp = (wave + flutter) * u * (0.32f + wind_amp * 0.9f);
      const float droop = u * u * 0.22f;
      const float reach = u * f.width;
      return {px + wx * reach + flag_perp_x * disp, y_top - v * f.height - droop + wave * 0.06f * u,
              pz + wz * reach + flag_perp_z * disp};
    };
    for (int c = 0; c < kCols; ++c) {
      for (int r = 0; r < kRows; ++r) {
        const auto& col = (r == 1) ? f.stripe_rgb : f.primary_rgb;
        const std::array<uint8_t, 4> rgba{col[0], col[1], col[2], 255};
        push_quad(out, cloth_pt(c, r), cloth_pt(c + 1, r), cloth_pt(c + 1, r + 1), cloth_pt(c, r + 1), rgba, true);
      }
    }
    (void)y_bot;
  }

  // 4. Festival crowds / cheering spectators driven by TrackSettings <Crowds> tint & Spectators.zip catalog.
  const std::array<uint8_t, 3> shirt_palette[6] = {
      {232, 54, 78}, {46, 146, 224}, {244, 198, 48}, {68, 186, 118}, {238, 128, 38}, {225, 225, 232}};
  const float ct_r = std::clamp(0.5f * (post_cfg_.crowd_dark[0] + post_cfg_.crowd_light[0]), 0.3f, 1.5f);
  const float ct_g = std::clamp(0.5f * (post_cfg_.crowd_dark[1] + post_cfg_.crowd_light[1]), 0.3f, 1.5f);
  const float ct_b = std::clamp(0.5f * (post_cfg_.crowd_dark[2] + post_cfg_.crowd_light[2]), 0.3f, 1.5f);
  for (size_t ci = 0; ci < crowds_.size(); ++ci) {
    const auto& cl = crowds_[ci];
    const double dx = double(cl.origin[0]) - eye_x;
    const double dz = double(cl.origin[2]) - eye_z;
    if (dx * dx + dz * dz > 190.0 * 190.0) continue;

    const float fx = -cl.dir_z, fz = cl.dir_x;  // facing road
    for (int i = 0; i < cl.count; ++i) {
      const float h = hash_01(int(ci * 31 + size_t(i)), i * 17, 11u);
      const float sx = cl.origin[0] + cl.dir_x * (float(i) * cl.spacing) + fx * (h * 0.55f - 0.25f);
      const float sz = cl.origin[2] + cl.dir_z * (float(i) * cl.spacing) + fz * (h * 0.55f - 0.25f);
      const float sy = ground_y(sx, sz, cl.origin[1]);
      const float phase = h * 6.283f + float(i) * 0.83f;
      const float cheer = std::max(0.0f, std::sin(time_sec * (3.2f + h * 1.4f) + phase));
      const float jump = cheer * cheer * 0.14f;
      const float scale = 0.90f + 0.18f * hash_01(i, int(ci), 29u);

      const auto& pal = shirt_palette[(size_t(i) + ci * 2) % 6];
      const std::array<uint8_t, 4> shirt{clamp_u8(float(pal[0]) * ct_r), clamp_u8(float(pal[1]) * ct_g),
                                         clamp_u8(float(pal[2]) * ct_b), 255};
      const std::array<uint8_t, 4> jeans{42, 54, 78, 255};
      const std::array<uint8_t, 4> skin{216, 174, 142, 255};

      // Legs, torso, head.
      push_box(out, {sx, sy + 0.42f * scale + jump, sz}, {cl.dir_x * 0.17f, 0, cl.dir_z * 0.17f},
               {0, 0.42f * scale, 0}, {fx * 0.12f, 0, fz * 0.12f}, jeans);
      push_box(out, {sx, sy + 1.12f * scale + jump, sz}, {cl.dir_x * 0.22f, 0, cl.dir_z * 0.22f},
               {0, 0.32f * scale, 0}, {fx * 0.13f, 0, fz * 0.13f}, shirt);
      push_box(out, {sx, sy + 1.60f * scale + jump, sz}, {0.12f, 0, 0}, {0, 0.14f * scale, 0}, {0, 0, 0.12f}, skin);

      // Animated cheering arms waving up & down!
      const float arm_lift = 0.18f + 0.48f * cheer;
      for (float side : {-1.0f, 1.0f}) {
        const float ax = sx + cl.dir_x * (0.29f * side);
        const float az = sz + cl.dir_z * (0.29f * side);
        const float ay = sy + (1.28f + arm_lift * 0.5f) * scale + jump;
        push_box(out, {ax, ay, az}, {cl.dir_x * 0.07f, 0, cl.dir_z * 0.07f},
                 {0, (0.22f + arm_lift * 0.28f) * scale, 0}, {fx * 0.07f, 0, fz * 0.07f}, shirt);
      }
    }
  }

  // 5. Roadside wind-swaying grass quads (_0x00005007 / TrackSettings grass parameters).
  if (include_grass && ground && post_cfg_.grass_height_scale > 0.01f) {
    const float max_r = std::clamp(post_cfg_.grass_fade_end, 14.0f, 34.0f);
    const float fade_start = std::clamp(post_cfg_.grass_fade_start, 6.0f, max_r - 2.0f);
    const float cell = 2.6f;
    const int cx0 = int(std::floor((eye_x - max_r) / cell));
    const int cx1 = int(std::floor((eye_x + max_r) / cell));
    const int cz0 = int(std::floor((eye_z - max_r) / cell));
    const int cz1 = int(std::floor((eye_z + max_r) / cell));
    constexpr size_t kMaxGrassVerts = 12000;
    const size_t start_v = out.size();

    for (int gx = cx0; gx <= cx1 && (out.size() - start_v) < kMaxGrassVerts; ++gx) {
      for (int gz = cz0; gz <= cz1 && (out.size() - start_v) < kMaxGrassVerts; ++gz) {
        if (hash_01(gx, gz, 73u) < 0.42f) continue;  // natural patchy distribution
        const float ox = (float(gx) + 0.15f + 0.70f * hash_01(gx, gz, 101u)) * cell;
        const float oz = (float(gz) + 0.15f + 0.70f * hash_01(gx, gz, 137u)) * cell;
        const float dx = ox - float(eye_x), dz = oz - float(eye_z);
        const float d2 = dx * dx + dz * dz;
        if (d2 > max_r * max_r || d2 < 2.5f * 2.5f) continue;

        drive::Hit h;
        const double start_y = eye_y + 20.0;
        if (!ground->raycast({double(ox), start_y, double(oz)}, {0.0, -1.0, 0.0}, 60.0, h) || h.normal.y < 0.75) {
          continue;
        }
        // Strict allowlist from physics/surfaceTypes.xml: only grow grass on Grass (2),
        // Grasscrete (13), and SlowGrass (25) — never on Asphalt (0), Dirt roads, or kerbs.
        if (h.surface != 2 && h.surface != 13 && h.surface != 25) {
          continue;
        }
        const float dist = std::sqrt(d2);
        const float fade = 1.0f - std::clamp((dist - fade_start) / std::max(max_r - fade_start, 1.0f), 0.0f, 1.0f);
        if (fade <= 0.05f) continue;

        const float h_rand = 0.16f + 0.18f * hash_01(gx, gz, 211u);
        const float gh = h_rand * std::clamp(post_cfg_.grass_height_scale, 0.3f, 2.0f) * fade;
        const float w = 0.035f + 0.028f * hash_01(gx, gz, 251u);
        const float gy = float(start_y - h.distance);
        const float sway = std::sin(time_sec * (1.8f + wind_spd * 1.1f) + ox * 0.28f + oz * 0.22f) *
                           (0.08f + wind_amp * 0.35f) * gh;
        const float dry = hash_01(gx, gz, 307u);
        const std::array<uint8_t, 4> col{
            clamp_u8(86.0f + dry * 52.0f),
            clamp_u8(110.0f + (1.0f - dry) * 30.0f),
            clamp_u8(42.0f + dry * 18.0f),
            255};

        // Delicate 3-blade splaying tuft (6 double-sided tris = 18 verts).
        const float ang = hash_01(gx, gz, 353u) * kPi;
        for (int b = 0; b < 3; ++b) {
          const float a = ang + float(b) * (kPi * 0.6667f);
          const float bx = std::cos(a) * w, bz = std::sin(a) * w;
          const float splay = 0.09f;
          const std::array<float, 3> p0{ox - bx, gy, oz - bz};
          const std::array<float, 3> p1{ox + bx, gy, oz + bz};
          const std::array<float, 3> tip{ox - bz * splay + wx * sway, gy + gh * (0.85f + 0.12f * float(b)),
                                         oz + bx * splay + wz * sway};
          push_tri(out, p0, p1, tip, col, true);
        }
      }
    }
  }
  (void)eye_y;
}

}  // namespace world
