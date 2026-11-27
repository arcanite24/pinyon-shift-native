#include "world/world_anim.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "fh1data/archive.h"
#include "fh1data/xds.h"

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

uint16_t read_be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1])); }

uint32_t read_be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

float read_bef32(const uint8_t* p) {
  const uint32_t bits = read_be32(p);
  float f = 0.0f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

float half_to_float_be(uint16_t h) {
  const uint32_t sign = uint32_t(h >> 15) << 31;
  const uint32_t exp = (h >> 10) & 0x1Fu;
  const uint32_t mant = h & 0x3FFu;
  uint32_t bits = 0;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      const float v = std::ldexp(float(mant), -24);
      std::memcpy(&bits, &v, 4);
      bits |= sign;
    }
  } else if (exp == 31) {
    bits = sign | 0x7F800000u | (mant << 13);
  } else {
    bits = sign | ((exp + 112u) << 23) | (mant << 13);
  }
  float out = 0.0f;
  std::memcpy(&out, &bits, 4);
  return out;
}

bool is_rally_world_path(const std::filesystem::path& world_dir) {
  if (world_dir.empty()) return false;
  const std::string low = lower_ascii(world_dir.string());
  return low.find("coloradodirt") != std::string::npos || low.find("1600_pri_65") != std::string::npos;
}

void blit_atlas_subrect(const fh1::RgbaImage& src, uint32_t sx0, uint32_t sy0, uint32_t sw, uint32_t sh,
                        uint8_t* dst_atlas, uint32_t atlas_size, uint32_t cell_x, uint32_t cell_y,
                        uint32_t cell_size) {
  if (src.width == 0 || src.height == 0 || sw == 0 || sh == 0 ||
      src.rgba.size() < size_t(src.width) * src.height * 4) {
    return;
  }
  for (uint32_t y = 0; y < cell_size; ++y) {
    const float sy = float(sy0) + (float(y) + 0.5f) * float(sh) / float(cell_size) - 0.5f;
    const uint32_t y0 = uint32_t(std::clamp(int(std::floor(sy)), 0, int(src.height) - 1));
    const uint32_t y1 = std::min(y0 + 1, src.height - 1);
    const float fy = std::clamp(sy - float(y0), 0.0f, 1.0f);
    for (uint32_t x = 0; x < cell_size; ++x) {
      const float sx = float(sx0) + (float(x) + 0.5f) * float(sw) / float(cell_size) - 0.5f;
      const uint32_t x0 = uint32_t(std::clamp(int(std::floor(sx)), 0, int(src.width) - 1));
      const uint32_t x1 = std::min(x0 + 1, src.width - 1);
      const float fx = std::clamp(sx - float(x0), 0.0f, 1.0f);
      const uint8_t* p00 = src.rgba.data() + (size_t(y0) * src.width + x0) * 4;
      const uint8_t* p10 = src.rgba.data() + (size_t(y0) * src.width + x1) * 4;
      const uint8_t* p01 = src.rgba.data() + (size_t(y1) * src.width + x0) * 4;
      const uint8_t* p11 = src.rgba.data() + (size_t(y1) * src.width + x1) * 4;
      uint8_t* o = dst_atlas + (size_t(cell_y * cell_size + y) * atlas_size + (cell_x * cell_size + x)) * 4;
      for (int c = 0; c < 4; ++c) {
        const float top = float(p00[c]) * (1.0f - fx) + float(p10[c]) * fx;
        const float bot = float(p01[c]) * (1.0f - fx) + float(p11[c]) * fx;
        o[c] = clamp_u8(top * (1.0f - fy) + bot * fy);
      }
    }
  }
}

uint8_t lin_to_srgb_u8(float c) {
  c = std::clamp(c, 0.0f, 1.0f);
  const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
  return clamp_u8(s * 255.0f);
}

void blit_atlas_cell(const fh1::RgbaImage& src, uint8_t* dst_atlas, uint32_t atlas_size, uint32_t cell_x,
                     uint32_t cell_y, uint32_t cell_size) {
  if (src.width == 0 || src.height == 0 || src.rgba.size() < size_t(src.width) * src.height * 4) return;
  if (src.width == cell_size && src.height == cell_size) {
    // Exact 1:1 native texel copy (zero resampling blur on FH1's native 256x256 spectator maps).
    for (uint32_t y = 0; y < cell_size; ++y) {
      const uint8_t* s = src.rgba.data() + size_t(y) * cell_size * 4;
      uint8_t* o = dst_atlas + (size_t(cell_y * cell_size + y) * atlas_size + cell_x * cell_size) * 4;
      std::memcpy(o, s, size_t(cell_size) * 4);
    }
    return;
  }
  for (uint32_t y = 0; y < cell_size; ++y) {
    const float sy = (float(y) + 0.5f) * float(src.height) / float(cell_size) - 0.5f;
    const uint32_t y0 = uint32_t(std::clamp(int(std::floor(sy)), 0, int(src.height) - 1));
    const uint32_t y1 = std::min(y0 + 1, src.height - 1);
    const float fy = std::clamp(sy - float(y0), 0.0f, 1.0f);
    for (uint32_t x = 0; x < cell_size; ++x) {
      const float sx = (float(x) + 0.5f) * float(src.width) / float(cell_size) - 0.5f;
      const uint32_t x0 = uint32_t(std::clamp(int(std::floor(sx)), 0, int(src.width) - 1));
      const uint32_t x1 = std::min(x0 + 1, src.width - 1);
      const float fx = std::clamp(sx - float(x0), 0.0f, 1.0f);
      const uint8_t* p00 = src.rgba.data() + (size_t(y0) * src.width + x0) * 4;
      const uint8_t* p10 = src.rgba.data() + (size_t(y0) * src.width + x1) * 4;
      const uint8_t* p01 = src.rgba.data() + (size_t(y1) * src.width + x0) * 4;
      const uint8_t* p11 = src.rgba.data() + (size_t(y1) * src.width + x1) * 4;
      uint8_t* o = dst_atlas + (size_t(cell_y * cell_size + y) * atlas_size + (cell_x * cell_size + x)) * 4;
      for (int c = 0; c < 4; ++c) {
        const float top = float(p00[c]) * (1.0f - fx) + float(p10[c]) * fx;
        const float bot = float(p01[c]) * (1.0f - fx) + float(p11[c]) * fx;
        o[c] = clamp_u8(top * (1.0f - fy) + bot * fy);
      }
    }
  }
}

// Fills an irradiance lightmap cell with a smooth top-to-bottom linear RGB gradient (stored as sRGB)
// so world_frag.glsl (`t.rgb * texture(light_map, v_uv2).rgb * lm_gain * 1.4`) produces natural
// overhead festival/outpost spotlighting from head to shoes without flattening albedo contrast.
void fill_atlas_cell_vertical_irradiance(uint8_t* atlas, uint32_t atlas_size, uint32_t slot, uint32_t grid,
                                         uint32_t cell_size, std::array<float, 3> top_lin,
                                         std::array<float, 3> bot_lin) {
  const uint32_t dx0 = (slot % grid) * cell_size;
  const uint32_t dy0 = (slot / grid) * cell_size;
  for (uint32_t y = 0; y < cell_size; ++y) {
    const float t = float(y) / float(std::max(1u, cell_size - 1u));
    const uint8_t r = lin_to_srgb_u8(top_lin[0] * (1.0f - t) + bot_lin[0] * t);
    const uint8_t g = lin_to_srgb_u8(top_lin[1] * (1.0f - t) + bot_lin[1] * t);
    const uint8_t b = lin_to_srgb_u8(top_lin[2] * (1.0f - t) + bot_lin[2] * t);
    uint8_t* drow = atlas + (size_t(dy0 + y) * atlas_size + dx0) * 4;
    for (uint32_t x = 0; x < cell_size; ++x) {
      drow[size_t(x) * 4 + 0] = r;
      drow[size_t(x) * 4 + 1] = g;
      drow[size_t(x) * 4 + 2] = b;
      drow[size_t(x) * 4 + 3] = 255;
    }
  }
}

void fill_atlas_cell_solid(uint8_t* atlas, uint32_t atlas_size, uint32_t slot, uint32_t grid, uint32_t cell_size,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  const uint32_t dx0 = (slot % grid) * cell_size;
  const uint32_t dy0 = (slot / grid) * cell_size;
  for (uint32_t y = 0; y < cell_size; ++y) {
    uint8_t* drow = atlas + (size_t(dy0 + y) * atlas_size + dx0) * 4;
    for (uint32_t x = 0; x < cell_size; ++x) {
      drow[size_t(x) * 4 + 0] = r;
      drow[size_t(x) * 4 + 1] = g;
      drow[size_t(x) * 4 + 2] = b;
      drow[size_t(x) * 4 + 3] = a;
    }
  }
}

// Verified 19-bone FH1 Standing skeleton hierarchy & rest-pose world joint coordinates
// (from media/Spectators.zip/anims/Stand_Idle_01.anim.bin: +X character left, +Y up, +Z forward).
constexpr std::array<SpectatorStandBone, SpectatorCatalog::kStandBoneCount> kDefaultStandSkeleton{{
    {-1, {+0.0001f, +0.9085f, -0.0015f}},  //  0: Bip001
    { 0, {+0.0001f, +0.9085f, -0.0015f}},  //  1: sk1_Bone_root
    { 1, {+0.0878f, +0.9090f, +0.0057f}},  //  2: sk1_Bone_legL_upper
    { 2, {+0.1258f, +0.5122f, +0.0471f}},  //  3: sk1_Bone_legL_lower
    { 3, {+0.1302f, +0.0938f, -0.0385f}},  //  4: sk1_Bone_legL_foot
    { 1, {-0.0876f, +0.9081f, -0.0086f}},  //  5: sk1_Bone_legR_upper
    { 5, {-0.1354f, +0.5122f, +0.0320f}},  //  6: sk1_Bone_legR_lower
    { 6, {-0.1313f, +0.0957f, -0.0621f}},  //  7: sk1_Bone_legR_foot
    { 1, {+0.0000f, +0.9935f, -0.0061f}},  //  8: sk1_Bone_spine
    { 8, {+0.0005f, +1.5133f, +0.0153f}},  //  9: Bip001_Neck
    { 9, {+0.0312f, +1.4542f, +0.0157f}},  // 10: sk1_Bone_armL_shoulder
    {10, {+0.1604f, +1.4119f, +0.0335f}},  // 11: sk1_Bone_armL_upper
    {11, {+0.1952f, +1.1621f, +0.0037f}},  // 12: sk1_Bone_armL_lower
    {12, {+0.2301f, +0.8789f, +0.0668f}},  // 13: sk1_Bone_armL_hand
    { 9, {-0.0302f, +1.4545f, +0.0103f}},  // 14: sk1_Bone_armR_shoulder
    {14, {-0.1653f, +1.4312f, +0.0111f}},  // 15: sk1_Bone_armR_upper
    {15, {-0.2024f, +1.1823f, -0.0234f}},  // 16: sk1_Bone_armR_lower
    {16, {-0.2233f, +0.8978f, +0.0399f}},  // 17: sk1_Bone_armR_hand
    { 9, {+0.0089f, +1.5787f, +0.0174f}},  // 18: sk1_Bone_head
}};

bool parse_stand_skeleton_from_anim_bin(std::span<const uint8_t> data,
                                        std::array<SpectatorStandBone, SpectatorCatalog::kStandBoneCount>& out) {
  constexpr size_t kBase = 0x190;
  if (data.size() < kBase + 0x40 || std::memcmp(data.data(), "CAFF", 4) != 0) return false;
  const uint8_t* p = data.data() + kBase;
  const uint16_t nbones = read_be16(p + 0x20);
  const uint32_t bone_off = read_be32(p + 0x2c);
  if (nbones != SpectatorCatalog::kStandBoneCount) return false;
  if (kBase + size_t(bone_off) + size_t(nbones) * 0x34 > data.size()) return false;

  std::array<SpectatorStandBone, SpectatorCatalog::kStandBoneCount> parsed{};
  for (size_t b = 0; b < SpectatorCatalog::kStandBoneCount; ++b) {
    const uint8_t* bp = p + bone_off + b * 0x34;
    const float wx = read_bef32(bp + 12);
    const float wy = read_bef32(bp + 16);
    const float wz = read_bef32(bp + 20);
    const int16_t parent = int16_t(read_be16(bp + 40));
    if (!std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz)) return false;
    if (parent >= int16_t(b) || parent < -1) return false;
    if (std::fabs(wx) > 2.0f || wy < -0.5f || wy > 2.5f || std::fabs(wz) > 2.0f) return false;
    parsed[b].parent = parent;
    parsed[b].bind_world = {wx, wy, wz};
  }
  out = parsed;
  return true;
}

// Verified 15-bone FH1 Sitting skeleton hierarchy & rest-pose world joint coordinates
// (from media/Spectators.zip/anims/Sit_Idle_01.anim.bin: +X character left, +Y up, +Z forward).
constexpr std::array<SpectatorStandBone, SpectatorCatalog::kSitBoneCount> kDefaultSitSkeleton{{
    {-1, {+0.0000f, +0.4861f, -0.4334f}},  //  0: root
    { 0, {+0.0000f, +0.5374f, -0.4334f}},  //  1: pelvis
    { 1, {+0.0000f, +0.5374f, -0.4334f}},  //  2: hip
    { 2, {+0.0000f, +0.5374f, -0.4334f}},  //  3: thigh_base
    { 3, {+0.0016f, +0.5312f, -0.0354f}},  //  4: knees
    { 4, {+0.0001f, +0.1116f, -0.0766f}},  //  5: feet
    { 2, {-0.0024f, +0.6191f, -0.4577f}},  //  6: spine
    { 6, {-0.0266f, +1.1070f, -0.2787f}},  //  7: chest/neck
    { 7, {+0.0069f, +1.0530f, -0.2984f}},  //  8: armL_shoulder
    { 8, {+0.1344f, +1.0028f, -0.2914f}},  //  9: armL_upper
    { 9, {+0.2024f, +0.7583f, -0.2802f}},  // 10: armL_lower
    { 7, {-0.0546f, +1.0503f, -0.2994f}},  // 11: armR_shoulder
    {11, {-0.1833f, +1.0050f, -0.2863f}},  // 12: armR_upper
    {12, {-0.2471f, +0.7596f, -0.2712f}},  // 13: armR_lower
    { 7, {-0.0282f, +1.1728f, -0.2737f}},  // 14: head
}};

bool parse_sit_skeleton_from_anim_bin(std::span<const uint8_t> data,
                                      std::array<SpectatorStandBone, SpectatorCatalog::kSitBoneCount>& out) {
  constexpr size_t kBase = 0x190;
  if (data.size() < kBase + 0x40 || std::memcmp(data.data(), "CAFF", 4) != 0) return false;
  const uint8_t* p = data.data() + kBase;
  const uint16_t nbones = read_be16(p + 0x20);
  const uint32_t bone_off = read_be32(p + 0x2c);
  if (nbones != SpectatorCatalog::kSitBoneCount) return false;
  if (kBase + size_t(bone_off) + size_t(nbones) * 0x34 > data.size()) return false;

  std::array<SpectatorStandBone, SpectatorCatalog::kSitBoneCount> parsed{};
  for (size_t b = 0; b < SpectatorCatalog::kSitBoneCount; ++b) {
    const uint8_t* bp = p + bone_off + b * 0x34;
    const float wx = read_bef32(bp + 12);
    const float wy = read_bef32(bp + 16);
    const float wz = read_bef32(bp + 20);
    const int16_t parent = int16_t(read_be16(bp + 40));
    if (!std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz)) return false;
    if (parent >= int16_t(b) || parent < -1) return false;
    if (std::fabs(wx) > 2.0f || wy < -0.5f || wy > 2.5f || std::fabs(wz) > 2.0f) return false;
    parsed[b].parent = parent;
    parsed[b].bind_world = {wx, wy, wz};
  }
  out = parsed;
  return true;
}

AnimatedPgeoCategory classify_pgeo_name(const std::string& low_name) {
  if (low_name.find("balloon") != std::string::npos) {
    return AnimatedPgeoCategory::kBalloon;
  }
  if (low_name.find("ferris") != std::string::npos) {
    return AnimatedPgeoCategory::kFerrisWheel;
  }
  if (low_name.find("windmill") != std::string::npos || low_name.find("windturbine") != std::string::npos) {
    return AnimatedPgeoCategory::kWindmill;
  }
  if (low_name.find("planebanner") != std::string::npos || low_name.find("biplane") != std::string::npos ||
      low_name.find("plane") != std::string::npos || low_name.find("helicopter") != std::string::npos ||
      low_name.find("mustang") != std::string::npos || low_name.find("blimp") != std::string::npos) {
    return AnimatedPgeoCategory::kAircraft;
  }
  if (low_name.find("traffic") != std::string::npos) {
    return AnimatedPgeoCategory::kTrafficLight;
  }
  if (low_name.find("banner") != std::string::npos || low_name.find("flag") != std::string::npos) {
    return AnimatedPgeoCategory::kBannerOrFlag;
  }
  if (low_name.find("airwalker") != std::string::npos || low_name.find("airdancer") != std::string::npos) {
    return AnimatedPgeoCategory::kAirwalker;
  }
  if (low_name.find("stage") != std::string::npos || low_name.find("light") != std::string::npos ||
      low_name.find("laser") != std::string::npos) {
    return AnimatedPgeoCategory::kStageLight;
  }
  return AnimatedPgeoCategory::kOther;
}

float smooth01(float x) {
  const float t = std::clamp(x, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

uint64_t pack_grid_key(int32_t gx, int32_t gz) {
  return (uint64_t(uint32_t(gx)) << 32) | uint64_t(uint32_t(gz));
}

}  // namespace

TrafficLightWeights evaluate_traffic_light_cycle(float time_sec, float phase_offset) {
  constexpr float kPeriod = 30.0f;
  constexpr float kGreenEnd = 13.0f;
  constexpr float kYellowEnd = 16.0f;
  constexpr float kBlend = 0.25f;
  float cyc = std::fmod(time_sec + phase_offset, kPeriod);
  if (cyc < 0.0f) cyc += kPeriod;

  TrafficLightWeights w{0.0f, 0.0f, 0.0f};
  if (cyc < kGreenEnd - kBlend) {
    w.green = 1.0f;
  } else if (cyc < kGreenEnd) {
    const float u = smooth01((cyc - (kGreenEnd - kBlend)) / kBlend);
    w.green = 1.0f - u;
    w.yellow = u;
  } else if (cyc < kYellowEnd - kBlend) {
    w.yellow = 1.0f;
  } else if (cyc < kYellowEnd) {
    const float u = smooth01((cyc - (kYellowEnd - kBlend)) / kBlend);
    w.yellow = 1.0f - u;
    w.red = u;
  } else if (cyc < kPeriod - kBlend) {
    w.red = 1.0f;
  } else {
    const float u = smooth01((cyc - (kPeriod - kBlend)) / kBlend);
    w.red = 1.0f - u;
    w.green = u;
  }
  return w;
}

int select_impostor_angle(float rel_azimuth_rad, int num_angles, bool both_sides) {
  const int n = std::max(1, num_angles);
  // Normalize relative azimuth into [0, 2*pi).
  float a = std::fmod(rel_azimuth_rad, 2.0f * kPi);
  if (a < 0.0f) a += 2.0f * kPi;
  if (!both_sides && a > kPi) {
    // Symmetric hemisphere fold when sprites only cover [0..pi].
    a = 2.0f * kPi - a;
  }
  const float span = both_sides ? (2.0f * kPi) : kPi;
  const float norm = std::clamp(a / span, 0.0f, 0.9999f);
  return std::clamp(int(norm * float(n)), 0, n - 1);
}

bool parse_spectator_anim_clip(std::span<const uint8_t> data, const std::string& name, SpectatorAnimClip& out) {
  out = SpectatorAnimClip{};
  out.name = name;
  constexpr size_t kBase = 0x190;
  if (data.size() < kBase + 0x34 || std::memcmp(data.data(), "CAFF", 4) != 0) return false;
  const uint8_t* p = data.data() + kBase;
  const float dur = read_bef32(p + 0x1c);
  const uint16_t nbones = read_be16(p + 0x20);
  const uint16_t nframes = read_be16(p + 0x22);
  if (!std::isfinite(dur) || dur <= 0.05f || dur > 600.0f) return false;
  if (nbones == 0 || nbones > 64) return false;
  out.duration_sec = dur;
  out.bone_count = nbones;
  out.frame_count = nframes;
  const std::string low = lower_ascii(name);
  out.is_sitting = (nbones == SpectatorCatalog::kSitBoneCount) || (low.find("sit") != std::string::npos);
  out.is_cheer = (low.find("cheer") != std::string::npos || low.find("gig") != std::string::npos);
  // Higher cadence / shorter cheer loops carry higher animation energy.
  const float fps = (nframes > 0 && dur > 0.05f) ? (float(nframes) / dur) : 30.0f;
  const float base_e = out.is_cheer ? 0.85f : 0.42f;
  const float cadence = std::clamp(3.2f / std::max(dur, 1.0f), 0.55f, 1.35f);
  out.energy = std::clamp(base_e * (0.75f + 0.25f * cadence) * std::clamp(fps / 30.0f, 0.7f, 1.3f), 0.20f, 1.0f);
  return true;
}

bool parse_animated_pgeo(std::span<const uint8_t> data, AnimatedPgeoObject& out, bool decode_geometry) {
  out = AnimatedPgeoObject{};
  if (data.size() < 0x60 || std::memcmp(data.data(), "OEGP", 4) != 0) return false;
  const uint8_t* p = data.data();
  out.box_min = {read_bef32(p + 0x10), read_bef32(p + 0x14), read_bef32(p + 0x18)};
  out.box_max = {read_bef32(p + 0x20), read_bef32(p + 0x24), read_bef32(p + 0x28)};
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(out.box_min[i]) || !std::isfinite(out.box_max[i])) return false;
  }
  out.kind = read_be32(p + 0x30);
  const uint32_t name_len = read_be32(p + 0x3c);
  const uint32_t total_size = read_be32(p + 0x48);
  const uint32_t gr2_size = read_be32(p + 0x4c);
  const uint32_t geom_size = read_be32(p + 0x50);
  if (total_size != data.size()) return false;
  if (name_len == 0 || name_len > 512 || 0x60 + size_t(name_len) > data.size()) return false;
  if (size_t(gr2_size) + size_t(geom_size) > data.size()) return false;

  std::string name(reinterpret_cast<const char*>(p + 0x60), name_len);
  while (!name.empty() && name.back() == '\0') name.pop_back();
  out.name = std::move(name);
  out.gr2_bytes = gr2_size;
  out.geom_bytes = geom_size;
  const std::string low = lower_ascii(out.name);
  out.category = classify_pgeo_name(low);
  out.is_world_space = (std::fabs(out.box_min[0]) > 50.0f || std::fabs(out.box_min[2]) > 50.0f ||
                        std::fabs(out.box_max[0]) > 50.0f || std::fabs(out.box_max[2]) > 50.0f);
  out.anim_period_sec = (out.category == AnimatedPgeoCategory::kBalloon)     ? 14.0f
                        : (out.category == AnimatedPgeoCategory::kWindmill)  ? 6.5f
                        : (out.category == AnimatedPgeoCategory::kAirwalker) ? 3.2f
                                                                             : 9.0f;

  // Parse texture reference table following the 16-byte-aligned object name.
  const size_t hdr_base = (0x60 + size_t(name_len) + 15u) & ~size_t(15u);
  const size_t geom_off = data.size() - size_t(geom_size);
  const size_t gr2_off = geom_off - size_t(gr2_size);
  if (hdr_base + 0x64 <= gr2_off) {
    const uint32_t ntex = read_be32(p + hdr_base + 0x44);
    if (ntex > 0 && ntex <= 32 && hdr_base + 0x60 + size_t(ntex) * 4 <= gr2_off) {
      size_t str_pos = hdr_base + 0x60 + size_t(ntex) * 4;
      for (uint32_t t = 0; t < ntex; ++t) {
        const uint32_t slen = read_be32(p + hdr_base + 0x60 + size_t(t) * 4);
        if (slen == 0 || slen > 256 || str_pos + slen > gr2_off) break;
        std::string tn(reinterpret_cast<const char*>(p + str_pos), slen);
        while (!tn.empty() && tn.back() == '\0') tn.pop_back();
        if (!tn.empty()) out.textures.push_back(std::move(tn));
        str_pos += slen;
      }
    }
  }

  // Decode half-float mesh geometry from kind-4 animated .pgeo when requested.
  if (decode_geometry && out.kind == 4u && geom_size >= 16u && hdr_base + 0x54 <= gr2_off) {
    const uint32_t nsub = read_be32(p + hdr_base + 0x50);
    if (nsub > 0 && nsub <= 64 && gr2_off >= hdr_base + size_t(nsub) * 0x2cu) {
      const size_t draw_off = gr2_off - size_t(nsub) * 0x2cu;
      std::vector<uint32_t> ntris(nsub, 0);
      bool tris_ok = true;
      for (uint32_t k = 0; k < nsub; ++k) {
        ntris[k] = read_be32(p + draw_off + size_t(k) * 0x2cu + 20u);
        if (ntris[k] == 0 || ntris[k] > 100000u) {
          tris_ok = false;
          break;
        }
      }
      if (tris_ok) {
        std::vector<std::pair<uint32_t, uint32_t>> sub_counts;
        sub_counts.reserve(nsub);
        size_t scan = hdr_base + 0x60;
        for (uint32_t k = 0; k < nsub; ++k) {
          const uint32_t want_ni = ntris[k] * 3u;
          bool found = false;
          for (size_t off = scan; off + 8 <= draw_off; off += 4) {
            if (read_be32(p + off) == want_ni && off >= hdr_base + 4) {
              const uint32_t nv = read_be32(p + off - 4);
              const uint32_t flg = read_be32(p + off + 4);
              if (nv >= 3u && nv <= 65535u && flg <= 2u) {
                sub_counts.push_back({nv, want_ni});
                scan = off + 8;
                found = true;
                break;
              }
            }
          }
          if (!found) break;
        }

        if (sub_counts.size() == nsub) {
          size_t total_v = 0, total_i = 0;
          for (const auto& [nv, ni] : sub_counts) {
            total_v += nv;
            total_i += ni;
          }
          const size_t idx_bytes = total_i * 2u;
          if (total_v > 0 && idx_bytes < geom_size) {
            const size_t rem = geom_size - idx_bytes;
            const size_t stride = rem / total_v;
            const size_t pad = rem - stride * total_v;
            if ((stride == 12 || stride == 16 || stride == 20 || stride == 24 || stride == 28 || stride == 32) &&
                pad <= 8) {
              const size_t vb_base = geom_off + idx_bytes + (pad <= 4 ? pad : 0);
              size_t cur_i = 0;
              size_t cur_v = 0;
              out.submeshes.reserve(nsub);
              for (const auto& [nv, ni] : sub_counts) {
                AnimatedPgeoSubmesh sm;
                sm.vertices.resize(nv);
                sm.indices.reserve(ni);
                sm.box_min = {1e9f, 1e9f, 1e9f};
                sm.box_max = {-1e9f, -1e9f, -1e9f};
                bool verts_valid = true;
                for (uint32_t vi = 0; vi < nv; ++vi) {
                  const size_t vp = vb_base + (cur_v + vi) * stride;
                  if (vp + 6 > data.size()) {
                    verts_valid = false;
                    break;
                  }
                  const float vx = half_to_float_be(read_be16(p + vp + 0));
                  const float vy = half_to_float_be(read_be16(p + vp + 2));
                  const float vz = half_to_float_be(read_be16(p + vp + 4));
                  if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz) ||
                      std::fabs(vx) > 20000.0f || std::fabs(vy) > 20000.0f || std::fabs(vz) > 20000.0f) {
                    verts_valid = false;
                    break;
                  }
                  sm.vertices[vi].pos = {vx, vy, vz};
                  sm.vertices[vi].normal = {0.0f, 0.0f, 0.0f};
                  sm.box_min[0] = std::min(sm.box_min[0], vx);
                  sm.box_min[1] = std::min(sm.box_min[1], vy);
                  sm.box_min[2] = std::min(sm.box_min[2], vz);
                  sm.box_max[0] = std::max(sm.box_max[0], vx);
                  sm.box_max[1] = std::max(sm.box_max[1], vy);
                  sm.box_max[2] = std::max(sm.box_max[2], vz);
                }
                if (verts_valid) {
                  for (uint32_t ii = 0; ii + 2 < ni; ii += 3) {
                    const size_t ip = geom_off + (cur_i + ii) * 2u;
                    if (ip + 6 > data.size()) break;
                    const uint16_t i0 = read_be16(p + ip + 0);
                    const uint16_t i1 = read_be16(p + ip + 2);
                    const uint16_t i2 = read_be16(p + ip + 4);
                    if (i0 >= nv || i1 >= nv || i2 >= nv || i0 == i1 || i1 == i2 || i0 == i2) continue;
                    sm.indices.push_back(i0);
                    sm.indices.push_back(i1);
                    sm.indices.push_back(i2);
                    const auto fn = vcross(vsub(sm.vertices[i1].pos, sm.vertices[i0].pos),
                                           vsub(sm.vertices[i2].pos, sm.vertices[i0].pos));
                    for (uint16_t idx : {i0, i1, i2}) {
                      sm.vertices[idx].normal[0] += fn[0];
                      sm.vertices[idx].normal[1] += fn[1];
                      sm.vertices[idx].normal[2] += fn[2];
                    }
                  }
                  for (auto& v : sm.vertices) v.normal = vnorm(v.normal);
                  if (!sm.indices.empty()) out.submeshes.push_back(std::move(sm));
                }
                cur_i += ni;
                cur_v += nv;
              }
            }
          }
        }
      }
    }
  }
  return true;
}

bool parse_skinbin(std::span<const uint8_t> data, SpectatorMeshLod& out, int preferred_lod) {
  out.vertices.clear();
  out.indices.clear();
  for (auto& li : out.lod_indices) li.clear();
  out.has_stand_skeleton = false;
  out.has_sit_skeleton = false;
  if (data.size() < 0x2c) return false;
  const uint8_t* p = data.data();
  const uint32_t num_lods = read_be32(p + 0x04);
  if (num_lods == 0 || num_lods > 8) return false;
  // Header layout: 0x14 + num_lods * 8 bytes of LOD table, followed by 0x04 marker,
  // vertex count (at 0x18 + num_lods * 8), 0x04 marker, and vertex array (at 0x20 + num_lods * 8).
  // Supports both 4-LOD spectator meshes (verts at 0x40) and 1-LOD driver meshes (verts at 0x28).
  const size_t vert_count_off = 0x18 + size_t(num_lods) * 8u;
  const size_t verts_off = 0x20 + size_t(num_lods) * 8u;
  if (verts_off + 4 > data.size()) return false;

  uint32_t lod_off[4] = {0u, 0u, 0u, 0u};
  uint32_t lod_cnt[4] = {0u, 0u, 0u, 0u};
  lod_cnt[0] = read_be32(p + 0x14);
  for (uint32_t l = 1; l < std::min<uint32_t>(num_lods, 4u); ++l) {
    const size_t lo = 0x14 + size_t(l) * 8u;
    lod_off[l] = read_be16(p + lo);
    lod_cnt[l] = read_be16(p + lo + 2);
  }
  const uint32_t vert_count = read_be32(p + vert_count_off);
  if (vert_count == 0 || vert_count > 32768) return false;

  const size_t idx_base = verts_off + size_t(vert_count) * 36;
  if (idx_base + 4 > data.size()) return false;
  const uint32_t total_idx = read_be32(p + idx_base);
  if (total_idx < 3 || idx_base + 4 + size_t(total_idx) * 2 > data.size()) return false;

  out.vertices.resize(vert_count);
  out.bone_role.fill(SpectatorMeshLod::kBoneLower);
  out.bone_centroid.fill({0.0f, 0.0f, 0.0f});
  out.min_y = 1e9f;
  out.has_arm_bones = false;

  std::array<std::array<double, 3>, SpectatorMeshLod::kMaxBones> b_sum{};
  std::array<uint32_t, SpectatorMeshLod::kMaxBones> b_cnt{};
  uint8_t max_bone = 0;

  for (uint32_t i = 0; i < vert_count; ++i) {
    const uint8_t* vp = p + verts_off + size_t(i) * 36;
    auto& v = out.vertices[i];
    v.pos = {read_bef32(vp + 0), read_bef32(vp + 4), read_bef32(vp + 8)};
    v.normal = vnorm({read_bef32(vp + 12), read_bef32(vp + 16), read_bef32(vp + 20)});
    v.uv = {float(read_be16(vp + 24)) / 65535.0f, float(read_be16(vp + 26)) / 65535.0f};
    v.bone0 = vp[28];
    v.bone1 = vp[29];
    v.weight0 = vp[30];
    v.weight1 = vp[31];
    max_bone = std::max(max_bone, v.bone0);
    if (v.weight1 > 0) max_bone = std::max(max_bone, v.bone1);
    out.min_y = std::min(out.min_y, v.pos[1]);
    if (v.bone0 < SpectatorMeshLod::kMaxBones && v.weight0 >= 128) {
      b_sum[v.bone0][0] += double(v.pos[0]);
      b_sum[v.bone0][1] += double(v.pos[1]);
      b_sum[v.bone0][2] += double(v.pos[2]);
      ++b_cnt[v.bone0];
    }
  }
  if (out.min_y > 1e8f) out.min_y = 0.0f;

  // Detect standard 19-bone FH1 standing spectator skeleton (spine=8, armL=11/12, armR=15/16, head=18)
  // and 15-bone FH1 sitting spectator skeleton (spine=6, armL=9/10, armR=12/13, head=14).
  out.has_stand_skeleton = (max_bone <= 18u && b_cnt[8] > 0 && b_cnt[11] > 0 && b_cnt[12] > 0 &&
                            b_cnt[15] > 0 && b_cnt[16] > 0 && b_cnt[18] > 0);
  out.has_sit_skeleton = (!out.has_stand_skeleton && max_bone <= 14u && b_cnt[6] > 0 && b_cnt[9] > 0 &&
                          b_cnt[10] > 0 && b_cnt[12] > 0 && b_cnt[13] > 0 && b_cnt[14] > 0);
  if (out.has_stand_skeleton || out.has_sit_skeleton) {
    out.has_arm_bones = true;
  }

  // Classify each skeleton bone from its bind-pose vertex centroid (+X = character left, -X = right).
  for (size_t b = 0; b < SpectatorMeshLod::kMaxBones; ++b) {
    if (b_cnt[b] == 0) continue;
    const float cx = float(b_sum[b][0] / double(b_cnt[b]));
    const float cy = float(b_sum[b][1] / double(b_cnt[b])) - out.min_y;
    const float cz = float(b_sum[b][2] / double(b_cnt[b]));
    out.bone_centroid[b] = {cx, cy, cz};
    if (cy > 1.48f && std::fabs(cx) <= 0.16f) {
      out.bone_role[b] = SpectatorMeshLod::kBoneHead;
    } else if (cx > 0.15f && cy >= 1.15f && cy <= 1.54f) {
      out.bone_role[b] = SpectatorMeshLod::kBoneUpperArmL;
      out.has_arm_bones = true;
    } else if (cx > 0.16f && cy >= 0.55f && cy < 1.15f) {
      out.bone_role[b] = SpectatorMeshLod::kBoneForearmL;
      out.has_arm_bones = true;
    } else if (cx < -0.15f && cy >= 1.15f && cy <= 1.54f) {
      out.bone_role[b] = SpectatorMeshLod::kBoneUpperArmR;
      out.has_arm_bones = true;
    } else if (cx < -0.16f && cy >= 0.55f && cy < 1.15f) {
      out.bone_role[b] = SpectatorMeshLod::kBoneForearmR;
      out.has_arm_bones = true;
    } else if (cy >= 0.95f && cy <= 1.48f) {
      out.bone_role[b] = SpectatorMeshLod::kBoneTorso;
    } else {
      out.bone_role[b] = SpectatorMeshLod::kBoneLower;
    }
  }

  const uint8_t* ip = p + idx_base + 4;
  auto extract_lod = [&](uint32_t off, uint32_t cnt, std::vector<uint16_t>& dst) {
    dst.clear();
    if (cnt < 3 || off + cnt > total_idx) return;
    cnt -= cnt % 3;
    dst.reserve(cnt);
    for (uint32_t i = 0; i + 2 < cnt; i += 3) {
      const uint16_t i0 = read_be16(ip + size_t(off + i + 0) * 2);
      const uint16_t i1 = read_be16(ip + size_t(off + i + 1) * 2);
      const uint16_t i2 = read_be16(ip + size_t(off + i + 2) * 2);
      if (i0 >= vert_count || i1 >= vert_count || i2 >= vert_count || i0 == i1 || i1 == i2 || i0 == i2) continue;
      const auto& v0 = out.vertices[i0];
      const auto& v1 = out.vertices[i1];
      const auto& v2 = out.vertices[i2];
      const auto fn = vcross(vsub(v1.pos, v0.pos), vsub(v2.pos, v0.pos));
      const float avg_dot = fn[0] * (v0.normal[0] + v1.normal[0] + v2.normal[0]) +
                            fn[1] * (v0.normal[1] + v1.normal[1] + v2.normal[1]) +
                            fn[2] * (v0.normal[2] + v1.normal[2] + v2.normal[2]);
      if (avg_dot < 0.0f) {
        dst.push_back(i0);
        dst.push_back(i2);
        dst.push_back(i1);
      } else {
        dst.push_back(i0);
        dst.push_back(i1);
        dst.push_back(i2);
      }
    }
  };

  // Extract LOD 0 (full detail), LOD 1 (mid), LOD 2 (far).
  const uint32_t avail_lods = std::min<uint32_t>(num_lods, 3u);
  for (uint32_t l = 0; l < avail_lods; ++l) {
    extract_lod(lod_off[l], lod_cnt[l], out.lod_indices[l]);
  }
  if (out.lod_indices[0].empty()) {
    extract_lod(0u, total_idx, out.lod_indices[0]);
  }
  for (uint32_t l = 1; l < 3u; ++l) {
    if (out.lod_indices[l].empty()) out.lod_indices[l] = out.lod_indices[l - 1];
  }

  const int chosen = std::clamp(preferred_lod, 0, 2);
  out.indices = out.lod_indices[size_t(chosen)];
  return !out.indices.empty();
}

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

  // <crowdclass name="..." ...> <skeleton name="..."/> <modelset name="..."/> <idle/cheer name="..."/> </crowdclass>
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
      const bool self_closing = (gt > cp && xml[gt - 1] == '/');
      size_t next_pos = gt + 1;
      if (!self_closing) {
        const size_t end = xml.find("</crowdclass>", gt + 1);
        if (end != std::string::npos) {
          size_t cur = gt + 1;
          while (cur < end) {
            const size_t lt = xml.find('<', cur);
            if (lt == std::string::npos || lt >= end) break;
            const size_t cgt = xml.find('>', lt);
            if (cgt == std::string::npos || cgt >= end) break;
            const std::string sub = xml.substr(lt, cgt - lt + 1);
            if (sub.rfind("<skeleton ", 0) == 0) {
              const std::string sn = attr_value(sub, "name");
              if (!sn.empty()) cc.skeleton = sn;
            } else if (sub.rfind("<modelset ", 0) == 0) {
              const std::string mn = attr_value(sub, "name");
              if (!mn.empty()) cc.modelset = mn;
            } else if (sub.rfind("<idle ", 0) == 0) {
              const std::string an = attr_value(sub, "name");
              if (!an.empty()) cc.idle_anims.push_back(an);
            } else if (sub.rfind("<cheer ", 0) == 0) {
              const std::string an = attr_value(sub, "name");
              if (!an.empty()) cc.cheer_anims.push_back(an);
            }
            cur = cgt + 1;
          }
          next_pos = end + 13;
        }
      }
      if (!cc.name.empty()) crowdclasses.push_back(std::move(cc));
      pos = next_pos;
    }
  }

  return !models.empty();
}

const AnimatedPgeoObject* SpectatorCatalog::find_pgeo(const std::string& name_substr) const {
  const std::string want = lower_ascii(name_substr);
  for (const auto& obj : pgeo_objects) {
    if (lower_ascii(obj.name).find(want) != std::string::npos) return &obj;
  }
  return nullptr;
}

bool SpectatorCatalog::load_from_media(const std::filesystem::path& media_dir, std::string* error) {
  spectators_anim_bins = 0;
  spectators_skinbins = 0;
  spectators_xds = 0;
  animated_pgeos = 0;
  animated_xds = 0;
  assets.clear();
  sit_assets.clear();
  atlas_rgba.clear();
  has_impostor_sprites = false;
  anim_clips.clear();
  pgeo_objects.clear();

  bool loaded_any = false;
  const auto spec_zip = media_dir / "Spectators.zip";
  if (std::filesystem::exists(spec_zip)) {
    std::string err;
    if (auto ar = fh1::Archive::open(spec_zip, &err)) {
      std::unordered_map<std::string, size_t> member_by_low;
      const auto& members = ar->members();
      member_by_low.reserve(members.size());
      for (size_t idx = 0; idx < members.size(); ++idx) {
        const auto& entry = members[idx];
        std::string low = lower_ascii(entry.name);
        for (char& c : low) {
          if (c == '\\') c = '/';
        }
        member_by_low[low] = idx;
        if (low.ends_with(".anim.bin")) {
          ++spectators_anim_bins;
          const auto ard = ar->read(entry);
          if (ard.ok()) {
            const size_t sl = low.find_last_of('/');
            std::string stem = (sl == std::string::npos) ? low : low.substr(sl + 1);
            if (stem.size() > 9) stem.resize(stem.size() - 9);  // strip ".anim.bin"
            SpectatorAnimClip clip;
            if (parse_spectator_anim_clip(ard.data, stem, clip)) {
              anim_clips[stem] = std::move(clip);
            }
          }
        } else if (low.ends_with(".skinbin")) {
          ++spectators_skinbins;
        } else if (low.ends_with(".xds")) {
          ++spectators_xds;
        }
        if (low == "spectators.xml" || low.ends_with("/spectators.xml")) {
          const auto rd = ar->read(entry);
          if (rd.ok()) {
            loaded_any = load_spectators_xml(std::string(rd.data.begin(), rd.data.end())) || loaded_any;
          }
        }
      }

      // Load exact 19-bone Standing skeleton & 15-bone Sitting skeleton hierarchies from
      // anims/Stand_Idle_01.anim.bin and anims/Sit_Idle_01.anim.bin (with verified defaults).
      stand_skeleton = kDefaultStandSkeleton;
      has_stand_skeleton_data = true;
      if (const auto sit = member_by_low.find("anims/stand_idle_01.anim.bin"); sit != member_by_low.end()) {
        const auto ard = ar->read(members[sit->second]);
        if (ard.ok()) {
          parse_stand_skeleton_from_anim_bin(ard.data, stand_skeleton);
        }
      }
      sit_skeleton = kDefaultSitSkeleton;
      has_sit_skeleton_data = true;
      if (const auto sit = member_by_low.find("anims/sit_idle_01.anim.bin"); sit != member_by_low.end()) {
        const auto ard = ar->read(members[sit->second]);
        if (ard.ok()) {
          parse_sit_skeleton_from_anim_bin(ard.data, sit_skeleton);
        }
      }

      // Decode ALL 20 standing & sitting 3D spectator meshes (.skinbin, LOD0/1/2) + native 256x256
      // textures (.dds.xds) + 10-angle sprites.xds impostor slices into the 2048x2048 RGBA8 atlas:
      //   slots  0..19: native 1:1 sRGB character diffuse textures
      //   slots 20..29: 10-angle distant crowd impostor slices (from sprites.xds)
      //   slot  60    : warm festival/outpost night stage-light irradiance (light_map)
      //   slot  61    : subtle dusk/moonlight ambient fill (light_map)
      //   slot  63    : pure black (unlit)
      std::unordered_map<std::string, std::string> tex_by_model;
      for (const auto& m : models) {
        tex_by_model[lower_ascii(m.name)] = lower_ascii(m.texture);
      }
      std::vector<std::string> candidate_names;
      candidate_names.reserve(models.size());
      for (const auto& m : models) candidate_names.push_back(m.name);

      std::vector<uint8_t> tmp_atlas(size_t(kAtlasSize) * kAtlasSize * 4, 0);
      for (size_t i = 3; i < tmp_atlas.size(); i += 4) tmp_atlas[i] = 255;
      std::unordered_map<std::string, uint32_t> tex_slot_map;
      uint32_t next_slot = 0;
      for (const auto& cname : candidate_names) {
        if (assets.size() >= kMaxDiffuseSlots) break;
        const std::string low_name = lower_ascii(cname);
        const std::string skin_key = "models/" + low_name + "_stand.skinbin";
        const auto skin_it = member_by_low.find(skin_key);
        if (skin_it == member_by_low.end()) continue;

        std::string tex_base = low_name;
        if (const auto tit = tex_by_model.find(low_name); tit != tex_by_model.end() && !tit->second.empty()) {
          tex_base = tit->second;
        }
        std::string tex_key = "textures/" + tex_base + ".dds.xds";
        auto tex_it = member_by_low.find(tex_key);
        if (tex_it == member_by_low.end()) {
          tex_key = "textures/" + tex_base + ".xds";
          tex_it = member_by_low.find(tex_key);
        }
        if (tex_it == member_by_low.end()) continue;

        uint32_t slot = 0;
        if (const auto sit = tex_slot_map.find(tex_key); sit != tex_slot_map.end()) {
          slot = sit->second;
        } else {
          if (next_slot >= kMaxDiffuseSlots) continue;
          const auto trd = ar->read(members[tex_it->second]);
          if (!trd.ok()) continue;
          const auto dec = fh1::decode_xds(trd.data, false);
          if (dec.levels.empty()) continue;
          slot = next_slot++;
          blit_atlas_cell(dec.levels[0], tmp_atlas.data(), kAtlasSize, slot % kAtlasGrid, slot / kAtlasGrid,
                          kAtlasCell);
          tex_slot_map[tex_key] = slot;
        }

        const auto srd = ar->read(members[skin_it->second]);
        if (!srd.ok()) continue;
        SpectatorAsset asset;
        asset.name = cname;
        asset.atlas_slot = slot;
        asset.is_male = (tex_base.rfind("female", 0) != 0);
        asset.is_sitting = false;
        if (!parse_skinbin(srd.data, asset.mesh, 0)) continue;
        assets.push_back(std::move(asset));

        // Decode paired sitting mesh (`models/<name>_sit.skinbin`), sharing the exact same texture slot.
        const std::string sit_key = "models/" + low_name + "_sit.skinbin";
        if (const auto sit_it = member_by_low.find(sit_key); sit_it != member_by_low.end()) {
          const auto sit_rd = ar->read(members[sit_it->second]);
          if (sit_rd.ok()) {
            SpectatorAsset sit_asset;
            sit_asset.name = cname;
            sit_asset.atlas_slot = slot;
            sit_asset.is_male = (tex_base.rfind("female", 0) != 0);
            sit_asset.is_sitting = true;
            if (parse_skinbin(sit_rd.data, sit_asset.mesh, 0)) {
              sit_assets.push_back(std::move(sit_asset));
            }
          }
        }
      }

      // Decode 10-angle distant crowd impostor sprites (`sprites.xds`) into atlas slots 20..29.
      if (const auto sp_it = member_by_low.find("sprites.xds"); sp_it != member_by_low.end()) {
        const auto sp_rd = ar->read(members[sp_it->second]);
        if (sp_rd.ok()) {
          const auto dec = fh1::decode_xds(sp_rd.data, false);
          if (!dec.levels.empty() && dec.levels[0].width >= 64 && dec.levels[0].height >= 32) {
            const auto& img = dec.levels[0];
            const uint32_t angles =
                uint32_t(std::clamp(num_sprite_angles, 1, int(kMaxImpostorSlots)));
            const uint32_t slice_w = std::max(1u, img.width / angles);
            for (uint32_t a = 0; a < angles; ++a) {
              const uint32_t slot = kImpostorBaseSlot + a;
              const uint32_t sx0 = std::min(a * slice_w, img.width - 1u);
              const uint32_t sw = std::min(slice_w, img.width - sx0);
              blit_atlas_subrect(img, sx0, 0u, sw, img.height, tmp_atlas.data(), kAtlasSize, slot % kAtlasGrid,
                                 slot / kAtlasGrid, kAtlasCell);
            }
            has_impostor_sprites = true;
          }
        }
      }

      // Bake vertical overhead irradiance cells for world_frag.glsl night Light_Map (`t.rgb * lm * lm_gain * 1.4`):
      // kNightLitSlot = warm Festival/Outpost stage spotlight falloff; kNightDimSlot = soft dusk/moonlight fill.
      fill_atlas_cell_vertical_irradiance(tmp_atlas.data(), kAtlasSize, kNightLitSlot, kAtlasGrid, kAtlasCell,
                                          {0.58f, 0.49f, 0.40f}, {0.24f, 0.21f, 0.18f});
      fill_atlas_cell_vertical_irradiance(tmp_atlas.data(), kAtlasSize, kNightDimSlot, kAtlasGrid, kAtlasCell,
                                          {0.15f, 0.16f, 0.20f}, {0.07f, 0.08f, 0.10f});
      fill_atlas_cell_solid(tmp_atlas.data(), kAtlasSize, kBlackSlot, kAtlasGrid, kAtlasCell, 0, 0, 0, 255);
      if (!assets.empty()) {
        atlas_rgba = std::move(tmp_atlas);
        atlas_loaded = true;
      }
    } else if (error) {
      *error = err;
    }
  }

  const auto anim_zip = media_dir / "animatedobjects.zip";
  if (std::filesystem::exists(anim_zip)) {
    std::string err;
    if (auto ar = fh1::Archive::open(anim_zip, &err)) {
      size_t decoded_geom_count = 0;
      constexpr size_t kMaxDecodedGeomPgeos = 36;
      for (const auto& entry : ar->members()) {
        const std::string low = lower_ascii(entry.name);
        if (low.ends_with(".pgeo")) {
          ++animated_pgeos;
          const bool want_geom =
              (decoded_geom_count < kMaxDecodedGeomPgeos) &&
              (low.find("balloon") != std::string::npos || low.find("airwalker") != std::string::npos ||
               low.find("windmill") != std::string::npos || low.find("windturbine") != std::string::npos ||
               low.find("planebanner") != std::string::npos || low.find("mainstage_lights") != std::string::npos ||
               low.find("ferris") != std::string::npos || low.find("blimp") != std::string::npos ||
               low.find("traffic") != std::string::npos || low.find("flag") != std::string::npos);
          const bool want_meta = want_geom || low.rfind("anim_", 0) == 0;
          if (want_meta) {
            const auto rd = ar->read(entry);
            if (rd.ok()) {
              AnimatedPgeoObject obj;
              if (parse_animated_pgeo(rd.data, obj, want_geom)) {
                if (!obj.submeshes.empty()) ++decoded_geom_count;
                pgeo_objects.push_back(std::move(obj));
              }
            }
          }
        } else if (low.ends_with(".xds")) {
          ++animated_xds;
        }
      }
    }
  }
  return loaded_any;
}

namespace {

std::atomic<const drive::Ground*> g_last_ground{nullptr};
std::atomic<const AnimatedWorldSystem*> g_last_ground_owner{nullptr};
std::atomic<AnimatedWorldSystem*> g_shared_anim{nullptr};

std::string read_rally_gameobjs_xml(const std::filesystem::path& world_dir) {
  std::error_code ec;
  if (!world_dir.empty()) {
    const auto loose = world_dir / "Ribbon_00" / "GameObjs.xml";
    if (std::filesystem::exists(loose, ec)) {
      std::ifstream f(loose, std::ios::binary);
      if (f) {
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
      }
    }
    // Search ancestor DLCZips archives (e.g. 1600_pri_65.zip) for ColoradoDirt/Ribbon_00/GameObjs.xml.
    std::vector<std::filesystem::path> zip_cands;
    if (lower_ascii(world_dir.extension().string()) == ".zip") {
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
    for (const auto& zp : zip_cands) {
      auto ar = fh1::Archive::open(zp, nullptr);
      if (!ar) continue;
      for (const auto& m : ar->members()) {
        std::string low = lower_ascii(m.name);
        for (char& c : low) {
          if (c == '\\') c = '/';
        }
        if (low.ends_with("coloradodirt/ribbon_00/gameobjs.xml")) {
          const auto rd = ar->read(m);
          if (rd.ok() && !rd.data.empty()) {
            return std::string(reinterpret_cast<const char*>(rd.data.data()), rd.data.size());
          }
        }
      }
    }
  }
  return {};
}

struct Mat3 {
  float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

Mat3 mat3_id() { return Mat3{}; }

Mat3 mat3_mul(const Mat3& a, const Mat3& b) {
  Mat3 o{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
    }
  }
  return o;
}

std::array<float, 3> mat3_vec(const Mat3& a, const std::array<float, 3>& v) {
  return {
      a.m[0][0] * v[0] + a.m[0][1] * v[1] + a.m[0][2] * v[2],
      a.m[1][0] * v[0] + a.m[1][1] * v[1] + a.m[1][2] * v[2],
      a.m[2][0] * v[0] + a.m[2][1] * v[1] + a.m[2][2] * v[2],
  };
}

Mat3 rot_x(float a) {
  const float c = std::cos(a), s = std::sin(a);
  Mat3 o{};
  o.m[1][1] = c;
  o.m[1][2] = -s;
  o.m[2][1] = s;
  o.m[2][2] = c;
  return o;
}

Mat3 rot_y(float a) {
  const float c = std::cos(a), s = std::sin(a);
  Mat3 o{};
  o.m[0][0] = c;
  o.m[0][2] = s;
  o.m[2][0] = -s;
  o.m[2][2] = c;
  return o;
}

Mat3 rot_z(float a) {
  const float c = std::cos(a), s = std::sin(a);
  Mat3 o{};
  o.m[0][0] = c;
  o.m[0][1] = -s;
  o.m[1][0] = s;
  o.m[1][1] = c;
  return o;
}

// Anatomical upper-arm local rotation in FH1's standing frame (+X left, +Y up, +Z forward):
//   side = +1.0f for Left arm (+X), -1.0f for Right arm (-X).
//   forward_up: 0 = hanging at side, +pi/2 = straight forward, +2.6 = raised high overhead.
//   out_abduct: >0 lifts arm laterally outward away from torso (gimbal-free whether down or overhead).
//   inward_twist: >0 rotates elbow hinge inward toward chest/head so elbows bend naturally inward.
Mat3 make_arm_rot(float side, float forward_up, float out_abduct, float inward_twist) {
  return mat3_mul(rot_x(-forward_up), mat3_mul(rot_z(side * out_abduct), rot_y(-side * inward_twist)));
}

// 3x3 rigid transform (rotation + translation) for watertight hierarchical skeletal skinning.
struct BoneXform {
  Mat3 r{};
  std::array<float, 3> t{0.0f, 0.0f, 0.0f};
};

void apply_bone_xform(const BoneXform& x, const std::array<float, 3>& p, const std::array<float, 3>& n,
                      std::array<float, 3>& out_p, std::array<float, 3>& out_n) {
  for (int i = 0; i < 3; ++i) {
    out_p[i] = x.r.m[i][0] * p[0] + x.r.m[i][1] * p[1] + x.r.m[i][2] * p[2] + x.t[i];
    out_n[i] = x.r.m[i][0] * n[0] + x.r.m[i][1] * n[1] + x.r.m[i][2] * n[2];
  }
}

bool is_road_surface(int surface) {
  switch (surface) {
    case 0:   // Asphalt
    case 5:   // Asphault2
    case 6:   // RumbleStrip
    case 20:  // AsphaultNoPenalty
    case 21:  // Asphault2NoPenalty
    case 26:  // SlowAsphault
    case 39:  // BumpyAsphaltSlight
    case 40:  // BumpyAsphaltModerate
    case 41:  // BumpyAsphaltExtreme
    case 57:  // Trackway
    case 58:  // Kerb
      return true;
    default:
      return false;
  }
}

bool is_obstacle_surface(int surface) {
  switch (surface) {
    case 3:   // GuardRail
    case 4:   // ConcreteBarrier
    case 9:   // Invisible
    case 11:  // RubberTire
    case 14:  // WireFence
    case 16:  // SafetyPlastic
    case 22:  // HayBails
    case 27:  // Quicksand
    case 33:  // FoamBarrier
    case 49:  // CameraCollision
      return true;
    default:
      return false;
  }
}

}  // namespace

AnimatedWorldSystem::AnimatedWorldSystem() : state_(std::make_shared<SharedState>()) {
  populate_default_colorado();
}

AnimatedWorldSystem::~AnimatedWorldSystem() {
  if (g_last_ground_owner.load(std::memory_order_relaxed) == this) {
    g_last_ground.store(nullptr, std::memory_order_relaxed);
    g_last_ground_owner.store(nullptr, std::memory_order_relaxed);
  }
  AnimatedWorldSystem* expected = this;
  g_shared_anim.compare_exchange_strong(expected, nullptr, std::memory_order_relaxed);
}

void AnimatedWorldSystem::set_shared(AnimatedWorldSystem* instance) {
  g_shared_anim.store(instance, std::memory_order_relaxed);
}

AnimatedWorldSystem* AnimatedWorldSystem::shared() {
  return g_shared_anim.load(std::memory_order_relaxed);
}

void AnimatedWorldSystem::share_from(const AnimatedWorldSystem& other) {
  if (other.state_) state_ = other.state_;
  reset_frame_cache();
}

void AnimatedWorldSystem::invalidate_spectator_ground_cache() const {
  std::lock_guard<std::mutex> lk(state_->cache_mutex);
  state_->crowd_ground_cache.clear();
  state_->cached_ground_ptr = nullptr;
  state_->grass_ground_cache.clear();
  state_->prop_ground_cache.clear();
  state_->cached_grass_ground_ptr = nullptr;
}

size_t AnimatedWorldSystem::cached_spectator_ground_count() const {
  std::lock_guard<std::mutex> lk(state_->cache_mutex);
  size_t total = 0;
  for (const auto& row : state_->crowd_ground_cache) {
    for (const auto& entry : row) {
      if (entry.valid) ++total;
    }
  }
  return total;
}

void AnimatedWorldSystem::invalidate_grass_ground_cache() const {
  std::lock_guard<std::mutex> lk(state_->cache_mutex);
  state_->grass_ground_cache.clear();
  state_->prop_ground_cache.clear();
  state_->cached_grass_ground_ptr = nullptr;
}

size_t AnimatedWorldSystem::cached_grass_ground_count() const {
  std::lock_guard<std::mutex> lk(state_->cache_mutex);
  return state_->grass_ground_cache.size();
}

void AnimatedWorldSystem::set_post_config(const TrackPostConfig& post_cfg) {
  state_->post_cfg = post_cfg;
}

const TrackPostConfig& AnimatedWorldSystem::post_config() const { return state_->post_cfg; }
const SpectatorCatalog& AnimatedWorldSystem::catalog() const { return state_->catalog; }
bool AnimatedWorldSystem::is_rally_world() const { return state_->rally_world; }
const std::vector<HotAirBalloon>& AnimatedWorldSystem::balloons() const { return state_->balloons; }
const std::vector<WindmillProp>& AnimatedWorldSystem::windmills() const { return state_->windmills; }
const std::vector<FestivalFlagProp>& AnimatedWorldSystem::flags() const { return state_->flags; }
const std::vector<FerrisWheelProp>& AnimatedWorldSystem::ferris_wheels() const { return state_->ferris_wheels; }
const std::vector<StageLightProp>& AnimatedWorldSystem::stage_lights() const { return state_->stage_lights; }
const std::vector<AircraftBannerProp>& AnimatedWorldSystem::aircraft_banners() const {
  return state_->aircraft_banners;
}
const std::vector<TrafficLightProp>& AnimatedWorldSystem::traffic_lights() const { return state_->traffic_lights; }
const std::vector<CrowdCluster>& AnimatedWorldSystem::crowds() const { return state_->crowds; }
const std::vector<AnimatedWorldPlacement>& AnimatedWorldSystem::pgeo_placements() const {
  return state_->pgeo_placements;
}

const drive::Ground* AnimatedWorldSystem::last_ground() {
  return g_last_ground.load(std::memory_order_acquire);
}

void AnimatedWorldSystem::clear_last_ground(const drive::Ground* only_if) {
  if (!only_if) {
    g_last_ground.store(nullptr, std::memory_order_release);
    g_last_ground_owner.store(nullptr, std::memory_order_release);
    return;
  }
  const drive::Ground* expected = only_if;
  if (g_last_ground.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
    g_last_ground_owner.store(nullptr, std::memory_order_release);
  }
}

bool AnimatedWorldSystem::init(const std::filesystem::path& media_dir, const TrackPostConfig& post_cfg,
                               std::string* error, const std::filesystem::path& world_dir) {
  const bool loaded = state_->catalog.load_from_media(media_dir, error);
  set_world(media_dir, world_dir, post_cfg);
  return loaded;
}

void AnimatedWorldSystem::set_world(const std::filesystem::path& media_dir, const std::filesystem::path& world_dir,
                                    const TrackPostConfig& post_cfg) {
  state_->post_cfg = post_cfg;
  state_->rally_world = is_rally_world_path(world_dir);
  state_->balloons.clear();
  state_->windmills.clear();
  state_->flags.clear();
  state_->ferris_wheels.clear();
  state_->stage_lights.clear();
  state_->aircraft_banners.clear();
  state_->traffic_lights.clear();
  state_->crowds.clear();
  state_->pgeo_placements.clear();
  {
    std::lock_guard<std::mutex> lk(state_->cache_mutex);
    state_->crowd_ground_cache.clear();
    state_->cached_ground_ptr = nullptr;
    state_->grass_ground_cache.clear();
    state_->prop_ground_cache.clear();
    state_->cached_grass_ground_ptr = nullptr;
    state_->pools_ready = false;
  }
  reset_frame_cache();

  if (state_->rally_world) {
    populate_rally_world(media_dir, world_dir);
  } else {
    populate_default_colorado();
    populate_pgeo_placements();
    populate_gameobjs_crowds(media_dir);
  }
}

void AnimatedWorldSystem::populate_default_colorado() {
  // WORLD-14/15: Colorado has no engine-placed props. The balloons, blimp,
  // biplane, flags, windmills, wind turbines, Ferris wheels, stage light
  // cones, traffic lights and crowd clusters that stood here at fixed
  // positions are not in the original (the Gauntlet sign-up shows none of
  // them); animated objects come from the animatedobjects.zip `.pgeo`
  // placements (populate_pgeo_placements) and crowds from GameObjs.xml.
}

void AnimatedWorldSystem::populate_pgeo_placements() {
  state_->pgeo_placements.clear();
  const auto& cat = state_->catalog;
  if (cat.pgeo_objects.empty()) return;

  static constexpr std::array<std::array<uint8_t, 3>, 6> kPrimaryPal{{
      {236, 46, 86}, {244, 132, 34}, {58, 126, 224}, {214, 52, 138}, {40, 172, 118}, {162, 56, 204},
  }};
  static constexpr std::array<std::array<uint8_t, 3>, 6> kSecondaryPal{{
      {250, 212, 48}, {245, 245, 240}, {252, 232, 92}, {248, 242, 226}, {246, 222, 68}, {250, 206, 54},
  }};
  static constexpr std::array<std::array<uint8_t, 3>, 6> kAccentPal{{
      {38, 134, 218}, {32, 168, 152}, {228, 66, 62}, {246, 176, 42}, {232, 64, 52}, {244, 242, 238},
  }};

  // 1. Place parsed `.pgeo` hot-air balloons using actual `.pgeo` world-space bounds & submesh offsets.
  const AnimatedPgeoObject* balloon_race = cat.find_pgeo("balloon_race");
  const AnimatedPgeoObject* balloon_intro = cat.find_pgeo("balloon_showcase_intro");
  const AnimatedPgeoObject* balloon_mesh_src =
      (balloon_race && !balloon_race->submeshes.empty())
          ? balloon_race
          : ((balloon_intro && !balloon_intro->submeshes.empty()) ? balloon_intro : nullptr);

  if (balloon_mesh_src) {
    size_t idx = 0;
    // Derive real world balloon cluster from ANIM_Showcase_Event_Balloon_Race.pgeo's bounding volume.
    if (balloon_race && balloon_race->is_world_space) {
      const float x0 = balloon_race->box_min[0], x1 = balloon_race->box_max[0];
      const float y0 = std::max(balloon_race->box_min[1], 120.0f);
      const float y1 = std::max(balloon_race->box_max[1], y0 + 40.0f);
      const float z0 = balloon_race->box_min[2], z1 = balloon_race->box_max[2];
      for (int k = 0; k < 5; ++k) {
        const float u = (float(k) + 0.5f) / 5.0f;
        const float v = 0.25f + 0.50f * hash_01(k * 7, 13, 91u);
        AnimatedWorldPlacement pl;
        pl.pgeo_name = balloon_mesh_src->name;
        pl.category = AnimatedPgeoCategory::kBalloon;
        pl.base_pos = {x0 + (x1 - x0) * u, y0 + (y1 - y0) * (0.45f + 0.35f * v), z0 + (z1 - z0) * v};
        pl.yaw = u * 6.283185f;
        pl.scale = 0.46f + 0.08f * hash_01(k, 3, 19u);
        pl.phase = float(k) * 1.15f;
        pl.primary_rgb = kPrimaryPal[idx % kPrimaryPal.size()];
        pl.secondary_rgb = kSecondaryPal[idx % kSecondaryPal.size()];
        pl.accent_rgb = kAccentPal[idx % kAccentPal.size()];
        state_->pgeo_placements.push_back(std::move(pl));
        ++idx;
      }
    }
    // Derive Horizon Festival & valley balloon formations from ANIM_Balloon_Showcase_Intro_001.pgeo
    // submesh offsets anchored over the Festival Hub & central Colorado vistas.
    if (balloon_intro && !balloon_intro->submeshes.empty()) {
      const std::array<std::array<float, 3>, 2> anchors{{
          {1850.0f, 195.0f, 960.0f},   // North Festival / reservoir vista
          {-1380.0f, 178.0f, -260.0f}, // Main Horizon Festival Hub
      }};
      for (const auto& anc : anchors) {
        for (size_t s = 0; s < balloon_intro->submeshes.size() && s < 5; ++s) {
          const auto& sm = balloon_intro->submeshes[s];
          const float ox = 0.5f * (sm.box_min[0] + sm.box_max[0]);
          const float oy = 0.5f * (sm.box_min[1] + sm.box_max[1]);
          const float oz = 0.5f * (sm.box_min[2] + sm.box_max[2]);
          AnimatedWorldPlacement pl;
          pl.pgeo_name = balloon_mesh_src->name;
          pl.category = AnimatedPgeoCategory::kBalloon;
          pl.base_pos = {anc[0] + std::clamp(ox * 0.45f, -320.0f, 320.0f),
                         anc[1] + std::clamp(oy * 0.25f, -15.0f, 45.0f),
                         anc[2] + std::clamp(oz * 1.4f, -260.0f, 260.0f)};
          pl.yaw = float(idx) * 0.9f;
          pl.scale = 0.44f + 0.06f * float(s % 3);
          pl.phase = float(idx) * 1.25f;
          pl.primary_rgb = kPrimaryPal[idx % kPrimaryPal.size()];
          pl.secondary_rgb = kSecondaryPal[idx % kSecondaryPal.size()];
          pl.accent_rgb = kAccentPal[idx % kAccentPal.size()];
          state_->pgeo_placements.push_back(std::move(pl));
          ++idx;
        }
      }
    }
  }

  // 2. Place parsed `.pgeo` festival airwalkers near the Festival Hub if decoded.
  if (const auto* aw = cat.find_pgeo("airwalker_001"); aw && !aw->submeshes.empty()) {
    const std::array<std::array<float, 3>, 2> spots{{
        {-1448.0f, 26.0f, -330.0f},
        {-1376.0f, 1.0f, -10.0f},
    }};
    for (size_t i = 0; i < spots.size(); ++i) {
      AnimatedWorldPlacement pl;
      pl.pgeo_name = aw->name;
      pl.category = AnimatedPgeoCategory::kAirwalker;
      pl.base_pos = spots[i];
      pl.yaw = float(i) * 1.4f;
      pl.scale = 1.0f;
      pl.phase = float(i) * 2.1f;
      pl.primary_rgb = (i == 0) ? std::array<uint8_t, 3>{238, 44, 96} : std::array<uint8_t, 3>{44, 168, 232};
      pl.secondary_rgb = {252, 214, 48};
      pl.accent_rgb = {246, 246, 244};
      state_->pgeo_placements.push_back(std::move(pl));
    }
  }

  // 3. Place decoded `.pgeo` Horizon Festival landmarks (mainstage lights, circling planebanner/blimp,
  // Ferris wheel, wind turbine) when their half-float geometry is present in animatedobjects.zip.
  if (const auto* sl = cat.find_pgeo("mainstage_lights"); sl && !sl->submeshes.empty()) {
    AnimatedWorldPlacement pl;
    pl.pgeo_name = sl->name;
    pl.category = AnimatedPgeoCategory::kStageLight;
    pl.base_pos = {-1450.0f, 26.0f, -330.0f};
    pl.yaw = 0.35f;
    pl.scale = 1.0f;
    pl.phase = 0.0f;
    pl.primary_rgb = {242, 44, 156};
    pl.secondary_rgb = {38, 206, 250};
    pl.accent_rgb = {254, 196, 42};
    state_->pgeo_placements.push_back(std::move(pl));
  }
  if (const auto* pb = cat.find_pgeo("planebanner"); pb && !pb->submeshes.empty()) {
    AnimatedWorldPlacement pl;
    pl.pgeo_name = pb->name;
    pl.category = AnimatedPgeoCategory::kAircraft;
    pl.base_pos = {1960.0f, 212.0f, 940.0f};
    pl.yaw = 0.0f;
    pl.scale = 1.0f;
    pl.phase = 0.6f;
    pl.primary_rgb = {236, 46, 92};
    pl.secondary_rgb = {252, 212, 48};
    pl.accent_rgb = {248, 246, 242};
    state_->pgeo_placements.push_back(std::move(pl));
  }
  if (const auto* bl = cat.find_pgeo("blimp"); bl && !bl->submeshes.empty()) {
    AnimatedWorldPlacement pl;
    pl.pgeo_name = bl->name;
    pl.category = AnimatedPgeoCategory::kAircraft;
    pl.base_pos = {-1380.0f, 236.0f, -260.0f};
    pl.yaw = 0.0f;
    pl.scale = 1.0f;
    pl.phase = 2.0f;
    pl.primary_rgb = {38, 134, 218};
    pl.secondary_rgb = {252, 212, 48};
    pl.accent_rgb = {248, 246, 242};
    state_->pgeo_placements.push_back(std::move(pl));
  }
  if (const auto* fw = cat.find_pgeo("ferris"); fw && !fw->submeshes.empty()) {
    AnimatedWorldPlacement pl;
    pl.pgeo_name = fw->name;
    pl.category = AnimatedPgeoCategory::kFerrisWheel;
    pl.base_pos = {-1412.0f, 24.0f, -292.0f};
    pl.yaw = 0.45f;
    pl.scale = 1.0f;
    pl.phase = 0.0f;
    pl.primary_rgb = {236, 46, 92};
    pl.secondary_rgb = {252, 212, 48};
    pl.accent_rgb = {38, 158, 224};
    state_->pgeo_placements.push_back(std::move(pl));
  }
}

void AnimatedWorldSystem::populate_rally_world(const std::filesystem::path& /*media_dir*/,
                                               const std::filesystem::path& world_dir) {
  // ColoradoDirt (Horizon Rally): NO Horizon Festival crowds or hot-air balloons.
  // Populate Rally-appropriate roadside spectators, compact stage marshals, and
  // high-visibility Rally stage marker flags from Rally's GameObjs.xml.
  const std::string xml = read_rally_gameobjs_xml(world_dir);

  auto parse_attr_float = [](const std::string& tag, const char* name, float fallback) -> float {
    const std::string v = attr_value(tag, name);
    return v.empty() ? fallback : float(std::atof(v.c_str()));
  };
  auto too_close = [&](float x, float z, float min_d) -> bool {
    const float min_d2 = min_d * min_d;
    for (const auto& c : state_->crowds) {
      const float dx = c.origin[0] - x;
      const float dz = c.origin[2] - z;
      if (dx * dx + dz * dz < min_d2) return true;
    }
    return false;
  };

  size_t pos = 0;
  constexpr size_t kMaxRallyClusters = 48;
  size_t added = 0;
  while (pos < xml.size() && added < kMaxRallyClusters) {
    const size_t gid_pos = xml.find("GameplayID=\"", pos);
    if (gid_pos == std::string::npos) break;
    const size_t gid_end = xml.find('"', gid_pos + 12);
    if (gid_end == std::string::npos) break;
    const std::string gid = xml.substr(gid_pos + 12, gid_end - (gid_pos + 12));
    const std::string low = lower_ascii(gid);
    pos = gid_end + 1;

    const bool is_stage = low.rfind("race_", 0) == 0 || low.rfind("mission_", 0) == 0 ||
                          low.rfind("festival_", 0) == 0 || low.rfind("workshop_", 0) == 0 ||
                          low.rfind("nr", 0) == 0;
    if (!is_stage) continue;

    const size_t block_end = xml.find("</Obj", pos);
    if (block_end == std::string::npos) break;
    const size_t pos_tag = xml.find("<Pos ", pos);
    if (pos_tag == std::string::npos || pos_tag > block_end) continue;
    const size_t pos_gt = xml.find('>', pos_tag);
    if (pos_gt == std::string::npos || pos_gt > block_end) continue;
    const std::string pstr = xml.substr(pos_tag, pos_gt - pos_tag + 1);
    const float ox = parse_attr_float(pstr, "x", 0.0f);
    const float oy = parse_attr_float(pstr, "y", 0.0f);
    const float oz = parse_attr_float(pstr, "z", 0.0f);

    float xx = 1.0f, xz = 0.0f;
    float zx = 0.0f, zz = 1.0f;
    if (const size_t x_tag = xml.find("<XAxis ", pos); x_tag != std::string::npos && x_tag < block_end) {
      if (const size_t x_gt = xml.find('>', x_tag); x_gt != std::string::npos && x_gt < block_end) {
        const std::string xstr = xml.substr(x_tag, x_gt - x_tag + 1);
        xx = parse_attr_float(xstr, "x", 1.0f);
        xz = parse_attr_float(xstr, "z", 0.0f);
      }
    }
    if (const size_t z_tag = xml.find("<ZAxis ", pos); z_tag != std::string::npos && z_tag < block_end) {
      if (const size_t z_gt = xml.find('>', z_tag); z_gt != std::string::npos && z_gt < block_end) {
        const std::string zstr = xml.substr(z_tag, z_gt - z_tag + 1);
        zx = parse_attr_float(zstr, "x", 0.0f);
        zz = parse_attr_float(zstr, "z", 1.0f);
      }
    }
    const float zl = std::hypot(zx, zz);
    if (zl > 1e-4f) {
      zx /= zl;
      zz /= zl;
    } else {
      zx = 0.0f;
      zz = 1.0f;
    }
    const float dot_xz = xx * zx + xz * zz;
    xx -= dot_xz * zx;
    xz -= dot_xz * zz;
    const float xl = std::hypot(xx, xz);
    if (xl > 1e-4f) {
      xx /= xl;
      xz /= xl;
    } else {
      xx = zz;
      xz = -zx;
    }

    // Offset roadside rally spectators & marshals safely off the dirt stage verge (~8.5 m).
    const float lateral = 8.5f;
    const bool is_marshal_post = (added % 3u == 0u);
    const int count = is_marshal_post ? 3 : 6;
    const float spacing = is_marshal_post ? 1.45f : 1.25f;
    const float half_span = 0.5f * float(count - 1) * spacing;
    const float cx = ox + xx * lateral - zx * half_span;
    const float cz = oz + xz * lateral - zz * half_span;
    if (too_close(cx, cz, 28.0f)) continue;

    CrowdCluster cl;
    cl.origin = {cx, oy, cz};
    cl.dir_x = zx;
    cl.dir_y = 0.0f;
    cl.dir_z = zz;
    cl.face_x = -xx;
    cl.face_z = -xz;
    cl.count = count;
    cl.spacing = spacing;
    cl.crowdclass = is_marshal_post ? "Stand" : ((added % 4u == 1u) ? "Sit" : "Barrier");
    cl.require_offroad = true;
    cl.stage_lit = false;  // dirt rally stage verge: no festival neon floodlights
    state_->crowds.push_back(std::move(cl));

    // Place a waving high-visibility Rally stage marker flag opposite the marshal post.
    if (state_->flags.size() < 24) {
      FestivalFlagProp fl;
      fl.base_pos = {ox - xx * 7.2f, oy, oz - xz * 7.2f};
      fl.pole_height = 4.2f;
      fl.width = 0.95f;
      fl.height = 1.85f;
      fl.phase = float(added) * 0.85f;
      fl.primary_rgb = {244, 96, 22};   // rally high-visibility orange
      fl.stripe_rgb = {246, 246, 242};  // rally white chevron stripe
      state_->flags.push_back(fl);
    }
    ++added;
  }

  // If Rally GameObjs.xml was absent (e.g. unit tests without DLC files), add a minimal
  // Rally roadside marshal & spectator baseline distinct from Colorado Festival clusters.
  if (state_->crowds.empty()) {
    state_->crowds = {
        {{120.0f, 10.0f, 80.0f}, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 5, 1.30f, "Barrier", true, false},
        {{145.0f, 10.0f, 72.0f}, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 3, 1.45f, "Stand", true, false},
        {{168.0f, 10.0f, 84.0f}, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 4, 1.35f, "Sit", true, false},
    };
    state_->flags = {
        {{118.0f, 10.0f, 70.0f}, 4.2f, 0.95f, 1.85f, 0.0f, {244, 96, 22}, {246, 246, 242}},
        {{148.0f, 10.0f, 84.0f}, 4.2f, 0.95f, 1.85f, 1.4f, {244, 96, 22}, {246, 246, 242}},
    };
  }
}

void AnimatedWorldSystem::populate_gameobjs_crowds(const std::filesystem::path& media_dir) {
  if (state_->rally_world) return;
  // Auto-populate grounded festival crowd clusters along real Colorado Festival Hub,
  // Outpost, Exhibition, Showcase, and Race start/finish sites from Ribbon_00/GameObjs.xml.
  const auto objs_path = media_dir / "Tracks" / "Colorado" / "Ribbon_00" / "GameObjs.xml";
  std::error_code ec;
  if (!std::filesystem::exists(objs_path, ec)) return;
  std::ifstream f(objs_path, std::ios::binary);
  if (!f) return;
  std::ostringstream ss;
  ss << f.rdbuf();
  const std::string xml = ss.str();

  auto parse_attr_float = [](const std::string& tag, const char* name, float fallback) -> float {
    const std::string v = attr_value(tag, name);
    return v.empty() ? fallback : float(std::atof(v.c_str()));
  };

  auto too_close = [&](float x, float z, float min_d) -> bool {
    const float min_d2 = min_d * min_d;
    for (const auto& c : state_->crowds) {
      const float dx = c.origin[0] - x;
      const float dz = c.origin[2] - z;
      if (dx * dx + dz * dz < min_d2) return true;
    }
    return false;
  };

  // A site slot's ground position by its GameplayID (false: missing, or a
  // placeholder at the world origin).
  auto find_slot = [&](const std::string& id, float& x, float& z) -> bool {
    const size_t at = xml.find("GameplayID=\"" + id + "\"");
    if (at == std::string::npos) return false;
    const size_t end = xml.find("</Obj", at);
    const size_t tag = xml.find("<Pos ", at);
    if (tag == std::string::npos || tag > end) return false;
    const size_t gt = xml.find('>', tag);
    if (gt == std::string::npos || gt > end) return false;
    const std::string s = xml.substr(tag, gt - tag + 1);
    x = parse_attr_float(s, "x", 0.0f);
    z = parse_attr_float(s, "z", 0.0f);
    return std::fabs(x) >= 1.0f || std::fabs(z) >= 1.0f;
  };

  size_t pos = 0;
  // Every site in the file (about 550 clusters before the 22 m merge); a cap
  // of 96 stopped at the planes and exhibitions and left the race starts
  // after them bare, the Gauntlet's (FR06_NODE) among them, where the
  // original's free roam shows a crowd at the barriers (first30b,
  // idle_cam0_first30b_orig.png). build_spectator_mesh culls by distance.
  constexpr size_t kMaxAutoClusters = 1024;
  size_t added = 0;
  while (pos < xml.size() && added < kMaxAutoClusters) {
    const size_t gid_pos = xml.find("GameplayID=\"", pos);
    if (gid_pos == std::string::npos) break;
    const size_t gid_end = xml.find('"', gid_pos + 12);
    if (gid_end == std::string::npos) break;
    const std::string gid = xml.substr(gid_pos + 12, gid_end - (gid_pos + 12));
    const std::string low = lower_ascii(gid);
    pos = gid_end + 1;

    const bool is_festival = low.rfind("festival_", 0) == 0 || low.rfind("workshop_", 0) == 0;
    const bool is_outpost = low.rfind("outpost_", 0) == 0;
    const bool is_race_node = (low.size() >= 8 && low[0] == 'f' && low[1] == 'r' &&
                               std::isdigit(static_cast<unsigned char>(low[2])) &&
                               low.find("_node") != std::string::npos);
    const bool is_exhibition = low.rfind("exhibition_fest", 0) == 0 || low.rfind("plane_race_", 0) == 0;
    if (!is_festival && !is_outpost && !is_race_node && !is_exhibition) continue;

    const size_t block_end = xml.find("</Obj", pos);
    if (block_end == std::string::npos) break;
    const size_t pos_tag = xml.find("<Pos ", pos);
    if (pos_tag == std::string::npos || pos_tag > block_end) continue;
    const size_t pos_gt = xml.find('>', pos_tag);
    if (pos_gt == std::string::npos || pos_gt > block_end) continue;
    const std::string pstr = xml.substr(pos_tag, pos_gt - pos_tag + 1);
    const float ox = parse_attr_float(pstr, "x", 0.0f);
    const float oy = parse_attr_float(pstr, "y", 0.0f);
    const float oz = parse_attr_float(pstr, "z", 0.0f);
    // About 170 placeholders (`*_15`, `*_L`) sit at the world origin.
    if (std::fabs(ox) < 1.0f && std::fabs(oz) < 1.0f) continue;

    // Extract horizontal orientation axes (XAxis = lateral off-road, ZAxis = along road/site).
    float xx = 1.0f, xz = 0.0f;
    float zx = 0.0f, zz = 1.0f;
    const size_t x_tag = xml.find("<XAxis ", pos);
    if (x_tag != std::string::npos && x_tag < block_end) {
      const size_t x_gt = xml.find('>', x_tag);
      if (x_gt != std::string::npos && x_gt < block_end) {
        const std::string xstr = xml.substr(x_tag, x_gt - x_tag + 1);
        xx = parse_attr_float(xstr, "x", 1.0f);
        xz = parse_attr_float(xstr, "z", 0.0f);
      }
    }
    const size_t z_tag = xml.find("<ZAxis ", pos);
    if (z_tag != std::string::npos && z_tag < block_end) {
      const size_t z_gt = xml.find('>', z_tag);
      if (z_gt != std::string::npos && z_gt < block_end) {
        const std::string zstr = xml.substr(z_tag, z_gt - z_tag + 1);
        zx = parse_attr_float(zstr, "x", 0.0f);
        zz = parse_attr_float(zstr, "z", 1.0f);
      }
    }
    const float zl = std::hypot(zx, zz);
    if (zl > 1e-4f) {
      zx /= zl;
      zz /= zl;
    } else {
      zx = 0.0f;
      zz = 1.0f;
    }
    // Guarantee strict orthogonality between along-line `dir` and inward-facing `face`.
    const float dot_xz = xx * zx + xz * zz;
    xx -= dot_xz * zx;
    xz -= dot_xz * zz;
    const float xl = std::hypot(xx, xz);
    if (xl > 1e-4f) {
      xx /= xl;
      xz /= xl;
    } else {
      xx = zz;
      xz = -zx;
    }

    // Offset crowd cluster cleanly off the asphalt shoulder (~9.5 m laterally), facing inward toward the site.
    const float lateral = is_festival ? 10.5f : 9.2f;
    int count = is_festival || is_race_node ? 10 : (is_outpost ? 9 : 7);  // ~10 at FR06 (first30b)
    float spacing = is_festival ? 1.15f : 1.18f;
    const float half_span = 0.5f * float(count - 1) * spacing;
    float cx = ox + xx * lateral - zx * half_span;
    float cz = oz + xz * lateral - zz * half_span;
    // Festival race sites (FR<n>_NODE) share a kit of 17 slots (FR<n>_00..16),
    // but slot numbering differs per site (e.g. FR05 places the RACE SIGN UP
    // stage trailer at _00/_02/_16 while FR06 places it at _01/_16).
    // Locate the stage trailer from _16 (fallback along=0) and place the crowd
    // strictly inside the open fenced spectator pen clear of the trailer's
    // +/-6.4 m along-road footprint, behind the barrier line (lat >= 5.5 m).
    if (is_race_node) {
      const std::string site = gid.substr(0, gid.size() - 5);  // "FR06" of "FR06_NODE"
      float sx = 0.0f, sz = 0.0f;
      float stage_along = 0.0f;
      float stage_side = 1.0f;
      if (find_slot(site + "_16", sx, sz)) {
        const float dx = sx - ox, dz = sz - oz;
        stage_along = dx * zx + dz * zz;
        const float sl = dx * xx + dz * xz;
        if (std::fabs(sl) > 1.0f) stage_side = sl >= 0.0f ? 1.0f : -1.0f;
      }
      if (stage_side < 0.0f) {
        xx = -xx;
        xz = -xz;
      }
      struct PenSlot {
        float along;
        float lat;
      };
      std::vector<PenSlot> pos_pen, neg_pen;
      for (int k = 0; k <= 15; ++k) {
        char sfx[8];
        std::snprintf(sfx, sizeof sfx, "_%02d", k);
        float px = 0.0f, pz = 0.0f;
        if (!find_slot(site + sfx, px, pz)) continue;
        const float dx = px - ox, dz = pz - oz;
        const float along = dx * zx + dz * zz;
        const float lat = dx * xx + dz * xz;
        if (lat < 5.5f || lat > 11.5f) continue;
        if (along >= stage_along + 6.4f) pos_pen.push_back({along, lat});
        else if (along <= stage_along - 6.4f) neg_pen.push_back({along, lat});
      }
      const auto& pen = !pos_pen.empty() ? pos_pen : neg_pen;
      if (!pen.empty()) {
        float min_a = pen[0].along, max_a = pen[0].along;
        float sum_lat = 0.0f;
        for (const auto& s : pen) {
          min_a = std::min(min_a, s.along);
          max_a = std::max(max_a, s.along);
          sum_lat += s.lat;
        }
        const float mean_lat = std::clamp(sum_lat / float(pen.size()), 6.8f, 9.2f);
        // Keep every spectator inside the open pen and clear of the stage trailer.
        const float pen_lo = !pos_pen.empty() ? std::max(min_a, stage_along + 6.6f) : min_a;
        const float pen_hi = !pos_pen.empty() ? std::max(max_a, pen_lo + 4.2f)
                                              : std::min(max_a, stage_along - 6.6f);
        const float span = std::max(2.0f, pen_hi - pen_lo);
        spacing = 0.95f;
        count = std::clamp(int(std::floor(span / spacing)) + 1, 4, 8);
        const float actual_span = float(count - 1) * spacing;
        const float start_along = 0.5f * (pen_lo + pen_hi) - 0.5f * actual_span;
        cx = ox + xx * mean_lat + zx * start_along;
        cz = oz + xz * mean_lat + zz * start_along;
      }
    }
    if (too_close(cx, cz, 22.0f)) continue;

    const bool lit_stage = is_festival || is_outpost || is_exhibition;

    CrowdCluster cl;
    cl.origin = {cx, oy, cz};
    cl.dir_x = zx;
    cl.dir_y = 0.0f;
    cl.dir_z = zz;
    cl.face_x = -xx;
    cl.face_z = -xz;
    cl.count = count;
    cl.spacing = spacing;
    cl.crowdclass = is_festival ? "GigCheer01" : "Barrier";
    cl.require_offroad = true;
    cl.stage_lit = lit_stage;
    state_->crowds.push_back(std::move(cl));
    ++added;

    // Add a staggered second row behind Festival/Outpost/Exhibition crowds (mixing seated bleachers).
    if ((is_festival || is_outpost || is_exhibition) && added < kMaxAutoClusters) {
      const int back_count = is_festival ? 7 : 6;
      const float back_spacing = 1.42f;
      const float back_half = 0.5f * float(back_count - 1) * back_spacing - 0.55f;
      const float back_lat = lateral + 1.12f;
      CrowdCluster back;
      back.origin = {ox + xx * back_lat - zx * back_half, oy, oz + xz * back_lat - zz * back_half};
      back.dir_x = zx;
      back.dir_y = 0.0f;
      back.dir_z = zz;
      back.face_x = -xx;
      back.face_z = -xz;
      back.count = back_count;
      back.spacing = back_spacing;
      back.crowdclass = (added % 4u == 0u) ? "Sit" : "GigCheer01";
      back.require_offroad = true;
      back.stage_lit = lit_stage;
      state_->crowds.push_back(std::move(back));
      ++added;
    }
  }
}

void AnimatedWorldSystem::build_mesh(double eye_x, double eye_y, double eye_z, float time_sec,
                                     const drive::Ground* ground, std::vector<DebrisVertex>& out,
                                     bool include_grass) const {
  if (ground) {
    g_last_ground.store(ground, std::memory_order_relaxed);
    g_last_ground_owner.store(this, std::memory_order_relaxed);
  }
  const auto& post_cfg = state_->post_cfg;
  const float wind_spd = std::clamp(post_cfg.wind_speed, 0.25f, 5.0f);
  const float wind_amp = std::clamp(post_cfg.wind_amplitude, 0.04f, 0.85f);
  float wx = post_cfg.wind_dir[0], wz = post_cfg.wind_dir[2];
  const float wlen = std::sqrt(wx * wx + wz * wz);
  if (wlen > 1e-4f) {
    wx /= wlen;
    wz /= wlen;
  } else {
    wx = 0.866f;
    wz = 0.50f;
  }
  const float wind_yaw = std::atan2(wx, wz);

  // Lock cache once for invalidation check + cached prop & grass ground lookups (eliminates ~800/frame raycasts).
  std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);
  if (ground != state_->cached_grass_ground_ptr) {
    state_->cached_grass_ground_ptr = ground;
    state_->grass_ground_cache.clear();
    state_->prop_ground_cache.clear();
  }
  if (state_->grass_ground_cache.size() > 32768u) {
    state_->grass_ground_cache.clear();
  }
  if (state_->prop_ground_cache.size() > 4096u) {
    state_->prop_ground_cache.clear();
  }

  auto ground_y = [&](float x, float z, float fallback) -> float {
    if (!ground) return fallback;
    const uint64_t pk = pack_grid_key(int32_t(std::lround(x * 2.0f)), int32_t(std::lround(z * 2.0f)));
    if (const auto it = state_->prop_ground_cache.find(pk); it != state_->prop_ground_cache.end()) {
      return it->second;
    }
    drive::Hit h;
    const double start_y = std::max(eye_y, double(fallback)) + 30.0;
    const float y =
        ground->raycast({double(x), start_y, double(z)}, {0.0, -1.0, 0.0}, 120.0, h) ? float(start_y - h.distance)
                                                                                   : fallback;
    state_->prop_ground_cache.emplace(pk, y);
    return y;
  };

  // 1. Waving flags (rendered in BOTH Colorado Free Roam AND ColoradoDirt Rally!).
  for (const auto& fl : state_->flags) {
    const double dx = double(fl.base_pos[0]) - eye_x;
    const double dz = double(fl.base_pos[2]) - eye_z;
    if (dx * dx + dz * dz > 320.0 * 320.0) continue;
    const float gy = ground_y(fl.base_pos[0], fl.base_pos[2], fl.base_pos[1]);
    // Slender matte metal pole + metallic finial cap.
    push_box(out, {fl.base_pos[0], gy + fl.pole_height * 0.5f, fl.base_pos[2]}, {0.045f, 0, 0},
             {0, fl.pole_height * 0.5f, 0}, {0, 0, 0.045f}, {98, 102, 108, 255});
    push_box(out, {fl.base_pos[0], gy + fl.pole_height + 0.08f, fl.base_pos[2]}, {0.07f, 0, 0},
             {0, 0.08f, 0}, {0, 0, 0.07f}, {226, 206, 134, 255});
    // Wind-rippling 4-segment cloth banner aligned with dynamic wind direction.
    constexpr int kSegs = 4;
    const float y_top = gy + fl.pole_height;
    const float y_bot = y_top - fl.height;
    for (int s = 0; s < kSegs; ++s) {
      const float t0 = float(s) / float(kSegs);
      const float t1 = float(s + 1) / float(kSegs);
      const float w0 = std::sin(time_sec * (3.2f + wind_spd * 1.4f) - t0 * 3.4f + fl.phase) *
                       (0.14f + wind_amp * 0.36f) * t0;
      const float w1 = std::sin(time_sec * (3.2f + wind_spd * 1.4f) - t1 * 3.4f + fl.phase) *
                       (0.14f + wind_amp * 0.36f) * t1;
      const std::array<float, 3> p00{fl.base_pos[0] + wx * (t0 * fl.width) - wz * w0, y_bot,
                                     fl.base_pos[2] + wz * (t0 * fl.width) + wx * w0};
      const std::array<float, 3> p10{fl.base_pos[0] + wx * (t1 * fl.width) - wz * w1, y_bot + 0.12f * t1,
                                     fl.base_pos[2] + wz * (t1 * fl.width) + wx * w1};
      const std::array<float, 3> p11{fl.base_pos[0] + wx * (t1 * fl.width) - wz * w1, y_top - 0.12f * t1,
                                     fl.base_pos[2] + wz * (t1 * fl.width) + wx * w1};
      const std::array<float, 3> p01{fl.base_pos[0] + wx * (t0 * fl.width) - wz * w0, y_top,
                                     fl.base_pos[2] + wz * (t0 * fl.width) + wx * w0};
      const auto& c = (s == 1 || s == 2) ? fl.stripe_rgb : fl.primary_rgb;
      push_quad(out, p00, p10, p11, p01, {c[0], c[1], c[2], 255}, true);
    }
  }

  if (!state_->rally_world) {
    bool rendered_pgeo_balloon = false;

    // 2a. Render ALL submeshes of parsed `.pgeo` animated world objects from animatedobjects.zip
    // (restores hot-air balloon baskets/ropes/flames alongside envelopes, plus airwalkers, stage lights,
    // aircraft banners, and Ferris wheels when decoded).
    if (!state_->pgeo_placements.empty()) {
      constexpr size_t kMaxPgeoVertsPerFrame = 56000;
      const size_t pgeo_start_v = out.size();
      for (const auto& pl : state_->pgeo_placements) {
        if ((out.size() - pgeo_start_v) >= kMaxPgeoVertsPerFrame) break;
        const AnimatedPgeoObject* obj = state_->catalog.find_pgeo(pl.pgeo_name);
        if (!obj || obj->submeshes.empty()) continue;

        float cx = pl.base_pos[0], cy = pl.base_pos[1], cz = pl.base_pos[2];
        float yaw = pl.yaw;
        float lean_x = 0.0f, lean_z = 0.0f;
        float spin_sub_angle = 0.0f;
        if (pl.category == AnimatedPgeoCategory::kBalloon) {
          cy += std::sin(time_sec * 0.42f + pl.phase) * 3.2f;
          cx += std::cos(time_sec * 0.23f + pl.phase) * 2.0f;
          cz += std::sin(time_sec * 0.19f + pl.phase * 1.3f) * 2.0f;
          yaw += time_sec * 0.11f;
        } else if (pl.category == AnimatedPgeoCategory::kAirwalker) {
          cy = ground_y(cx, cz, cy);
          const float wave = std::sin(time_sec * (3.6f + wind_spd) + pl.phase) * (0.28f + wind_amp * 0.45f);
          lean_x = wx * wave;
          lean_z = wz * wave;
        } else if (pl.category == AnimatedPgeoCategory::kWindmill) {
          cy = ground_y(cx, cz, cy);
          yaw = wind_yaw;
          spin_sub_angle = time_sec * (1.1f + wind_spd * 0.85f) + pl.phase;
        } else if (pl.category == AnimatedPgeoCategory::kFerrisWheel) {
          cy = ground_y(cx, cz, cy);
          spin_sub_angle = time_sec * 0.22f + pl.phase;
        } else if (pl.category == AnimatedPgeoCategory::kAircraft) {
          const float orbit_a = time_sec * 0.065f + pl.phase;
          const float orbit_r = 380.0f;
          cx = pl.base_pos[0] + std::cos(orbit_a) * orbit_r;
          cz = pl.base_pos[2] + std::sin(orbit_a) * orbit_r;
          cy = pl.base_pos[1] + std::sin(orbit_a * 2.0f) * 6.0f;
          yaw = -orbit_a;
        } else if (pl.category == AnimatedPgeoCategory::kStageLight) {
          cy = ground_y(cx, cz, cy);
          yaw = pl.yaw + std::sin(time_sec * 0.75f + pl.phase) * 0.55f;
        }

        const double dx = double(cx) - eye_x;
        const double dz = double(cz) - eye_z;
        const double max_d =
            (pl.category == AnimatedPgeoCategory::kBalloon || pl.category == AnimatedPgeoCategory::kAircraft) ? 1500.0
                                                                                                             : 450.0;
        if (dx * dx + dz * dz > max_d * max_d) continue;
        if (pl.category == AnimatedPgeoCategory::kBalloon) rendered_pgeo_balloon = true;

        // Compute combined bounds across ALL submeshes so multi-part props stay aligned.
        std::array<float, 3> all_min{1e9f, 1e9f, 1e9f};
        std::array<float, 3> all_max{-1e9f, -1e9f, -1e9f};
        for (const auto& sm : obj->submeshes) {
          for (int k = 0; k < 3; ++k) {
            all_min[size_t(k)] = std::min(all_min[size_t(k)], sm.box_min[size_t(k)]);
            all_max[size_t(k)] = std::max(all_max[size_t(k)], sm.box_max[size_t(k)]);
          }
        }
        const float b_cx = 0.5f * (all_min[0] + all_max[0]);
        const float b_cy = all_min[1];
        const float b_cz = 0.5f * (all_min[2] + all_max[2]);
        const float b_h = std::max(all_max[1] - all_min[1], 1.0f);
        const float cs = std::cos(yaw), sn = std::sin(yaw);
        const double dist = std::sqrt(dx * dx + dz * dz);
        const float pulse = 0.7f + 0.3f * std::sin(time_sec * 3.1f + pl.phase * 2.0f);

        for (size_t sm_i = 0; sm_i < obj->submeshes.size(); ++sm_i) {
          if ((out.size() - pgeo_start_v) >= kMaxPgeoVertsPerFrame) break;
          const auto& sm = obj->submeshes[sm_i];
          const size_t tri_count = sm.indices.size() / 3u;
          if (tri_count == 0) continue;
          const size_t tri_step = (dist > 600.0 && tri_count > 1200) ? 2u : 1u;
          out.reserve(out.size() + (tri_count / tri_step) * 3u);

          const float sm_span = std::max({sm.box_max[0] - sm.box_min[0], sm.box_max[1] - sm.box_min[1],
                                          sm.box_max[2] - sm.box_min[2]});
          const float sm_cy_rel = 0.5f * (sm.box_min[1] + sm.box_max[1]) - b_cy;
          const bool is_balloon_flame =
              (pl.category == AnimatedPgeoCategory::kBalloon && sm_i > 0 && sm_span < b_h * 0.16f &&
               sm_cy_rel > b_h * 0.08f && sm_cy_rel < b_h * 0.35f);
          const bool is_balloon_basket = (pl.category == AnimatedPgeoCategory::kBalloon && sm_i > 0 && !is_balloon_flame);
          const bool spin_this =
              (sm_i > 0 && (pl.category == AnimatedPgeoCategory::kWindmill ||
                            pl.category == AnimatedPgeoCategory::kFerrisWheel));
          const float s_cx = 0.5f * (sm.box_min[0] + sm.box_max[0]) - b_cx;
          const float s_cy = 0.5f * (sm.box_min[1] + sm.box_max[1]) - b_cy;
          const float r_cs = std::cos(spin_sub_angle), r_sn = std::sin(spin_sub_angle);

          for (size_t t = 0; t < tri_count; t += tri_step) {
            if ((out.size() - pgeo_start_v) + 3u > kMaxPgeoVertsPerFrame) break;
            const uint16_t i0 = sm.indices[t * 3u + 0u];
            const uint16_t i1 = sm.indices[t * 3u + 1u];
            const uint16_t i2 = sm.indices[t * 3u + 2u];
            const auto& v0 = sm.vertices[i0];
            const auto& v1 = sm.vertices[i1];
            const auto& v2 = sm.vertices[i2];
            const float mid_h = std::clamp(((v0.pos[1] + v1.pos[1] + v2.pos[1]) / 3.0f - b_cy) / b_h, 0.0f, 1.0f);
            const float mid_ang = std::atan2((v0.pos[2] + v1.pos[2] + v2.pos[2]) / 3.0f - b_cz,
                                             (v0.pos[0] + v1.pos[0] + v2.pos[0]) / 3.0f - b_cx);
            const int band = int(std::floor((mid_ang + kPi) / (2.0f * kPi) * 12.0f)) + int(mid_h * 4.0f);
            std::array<uint8_t, 4> rgba{220, 220, 220, 255};
            if (is_balloon_flame) {
              rgba = {clamp_u8(255.0f * pulse), clamp_u8(180.0f * pulse), 52, 170};
            } else if (is_balloon_basket) {
              rgba = (mid_h < 0.11f) ? std::array<uint8_t, 4>{154, 108, 62, 255}
                                     : std::array<uint8_t, 4>{76, 70, 64, 255};
            } else if (pl.category == AnimatedPgeoCategory::kStageLight && sm_i > 0) {
              rgba = {pl.primary_rgb[0], pl.primary_rgb[1], pl.primary_rgb[2], 24};
            } else {
              const auto& col = (mid_h < 0.14f && pl.category == AnimatedPgeoCategory::kBalloon)
                                    ? std::array<uint8_t, 3>{152, 108, 64}
                                    : ((band % 3 == 0) ? pl.primary_rgb
                                                       : ((band % 3 == 1) ? pl.secondary_rgb : pl.accent_rgb));
              rgba = {col[0], col[1], col[2], 255};
            }
            auto xform_v = [&](const AnimatedPgeoSubmesh::Vertex& v) -> DebrisVertex {
              float lx = (v.pos[0] - b_cx);
              float ly = (v.pos[1] - b_cy);
              float lz = (v.pos[2] - b_cz);
              if (spin_this) {
                const float rx = lx - s_cx;
                const float ry = ly - s_cy;
                lx = s_cx + rx * r_cs - ry * r_sn;
                ly = s_cy + rx * r_sn + ry * r_cs;
              }
              lx *= pl.scale;
              ly *= pl.scale;
              lz *= pl.scale;
              const float h_frac = std::clamp((v.pos[1] - b_cy) / b_h, 0.0f, 1.0f);
              const float px = cx + lx * cs + lz * sn + lean_x * h_frac * ly;
              const float py = cy + ly;
              const float pz = cz - lx * sn + lz * cs + lean_z * h_frac * ly;
              const std::array<float, 3> wn{v.normal[0] * cs + v.normal[2] * sn, v.normal[1],
                                            -v.normal[0] * sn + v.normal[2] * cs};
              return {{px, py, pz}, wn, rgba};
            };
            out.push_back(xform_v(v0));
            out.push_back(xform_v(v1));
            out.push_back(xform_v(v2));
          }
        }
        if (pl.category == AnimatedPgeoCategory::kBalloon) {
          push_box(out, {cx, cy + b_h * pl.scale * 0.18f, cz}, {0.55f, 0, 0}, {0, 0.42f, 0}, {0, 0, 0.55f},
                   {clamp_u8(255.0f * pulse), clamp_u8(178.0f * pulse), 52, 170});
        }
      }
    }

    // 2b. Procedural hot-air balloon fallback when animatedobjects.zip `.pgeo` balloons are absent.
    if (!rendered_pgeo_balloon) {
      for (const auto& b : state_->balloons) {
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
        const float by = cy - 3.0f;
        push_box(out, {cx, by, cz}, {1.25f, 0, 0}, {0, 1.05f, 0}, {0, 0, 1.25f}, {154, 108, 62, 255});
        const float pulse = 0.7f + 0.3f * std::sin(time_sec * 3.1f + b.phase * 2.0f);
        push_box(out, {cx, cy - 0.55f, cz}, {0.45f, 0, 0}, {0, 0.35f, 0}, {0, 0, 0.45f},
                 {clamp_u8(255.0f * pulse), clamp_u8(178.0f * pulse), 52, 170});
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
    }

    // 2c. Spinning ranch windmills & 3-blade ridge wind turbines (aligned with wind direction).
    for (const auto& wm : state_->windmills) {
      const double dx = double(wm.base_pos[0]) - eye_x;
      const double dz = double(wm.base_pos[2]) - eye_z;
      const double max_r = wm.is_turbine ? 1100.0 : 650.0;
      if (dx * dx + dz * dz > max_r * max_r) continue;
      const float gy = ground_y(wm.base_pos[0], wm.base_pos[2], wm.base_pos[1]);
      // Smoothly align rotor axis into the wind with subtle vane hunting.
      const float yaw = wind_yaw + std::sin(time_sec * 0.28f + wm.phase) * 0.06f;
      const float fx = std::sin(yaw), fz = std::cos(yaw);
      const float rx = std::cos(yaw), rz = -std::sin(yaw);
      const float hub_y = gy + wm.tower_height;

      if (wm.is_turbine) {
        // Tapered white tubular wind turbine tower + sleek nacelle + blinking red aviation beacon.
        const std::array<uint8_t, 4> white_mat{232, 234, 238, 255};
        const std::array<uint8_t, 4> grey_mat{186, 192, 198, 255};
        push_box(out, {wm.base_pos[0], gy + wm.tower_height * 0.28f, wm.base_pos[2]}, {0.85f, 0, 0},
                 {0, wm.tower_height * 0.28f, 0}, {0, 0, 0.85f}, white_mat);
        push_box(out, {wm.base_pos[0], gy + wm.tower_height * 0.76f, wm.base_pos[2]}, {0.55f, 0, 0},
                 {0, wm.tower_height * 0.26f, 0}, {0, 0, 0.55f}, white_mat);
        push_box(out, {wm.base_pos[0] - fx * 0.6f, hub_y, wm.base_pos[2] - fz * 0.6f}, {rx * 0.85f, 0, rz * 0.85f},
                 {0, 0.82f, 0}, {fx * 1.9f, 0, fz * 1.9f}, grey_mat);
        // Blinking red aviation beacon atop nacelle.
        const bool blink = std::sin(time_sec * 3.2f + wm.phase) > 0.15f;
        push_box(out, {wm.base_pos[0] - fx * 1.2f, hub_y + 1.12f, wm.base_pos[2] - fz * 1.2f}, {0.18f, 0, 0},
                 {0, 0.22f, 0}, {0, 0, 0.18f},
                 blink ? std::array<uint8_t, 4>{255, 38, 28, 175} : std::array<uint8_t, 4>{92, 24, 22, 255});
        // Rotor hub + 3 aerodynamic tapered blades.
        const float hx = wm.base_pos[0] + fx * 1.45f;
        const float hz = wm.base_pos[2] + fz * 1.45f;
        push_box(out, {hx, hub_y, hz}, {rx * 0.55f, 0, rz * 0.55f}, {0, 0.55f, 0}, {fx * 0.65f, 0, fz * 0.65f},
                 white_mat);
        const float spin = time_sec * (0.72f + wind_spd * 0.55f) + wm.phase;
        for (int b = 0; b < 3; ++b) {
          const float ang = spin + float(b) * (2.0f * kPi / 3.0f);
          const float ca = std::cos(ang), sa = std::sin(ang);
          const float bx = rx * ca, by = sa, bz = rz * ca;
          const float tx = -rx * sa, ty = ca, tz = -rz * sa;
          const float r_len = wm.blade_radius;
          const std::array<float, 3> p0{hx + tx * 0.42f, hub_y + ty * 0.42f, hz + tz * 0.42f};
          const std::array<float, 3> p1{hx - tx * 0.42f, hub_y - ty * 0.42f, hz - tz * 0.42f};
          const std::array<float, 3> p2{hx + bx * r_len - tx * 0.10f, hub_y + by * r_len - ty * 0.10f,
                                        hz + bz * r_len - tz * 0.10f};
          const std::array<float, 3> p3{hx + bx * r_len + tx * 0.10f, hub_y + by * r_len + ty * 0.10f,
                                        hz + bz * r_len + tz * 0.10f};
          push_quad(out, p0, p1, p2, p3, white_mat, true);
        }
      } else {
        // Classic Colorado ranch steel-lattice windmill: 4 angled legs + tail vane + 8-blade wind wheel.
        const std::array<uint8_t, 4> steel{124, 128, 134, 255};
        const std::array<uint8_t, 4> vane_col{184, 62, 46, 255};
        const std::array<uint8_t, 4> blade_col{198, 202, 206, 255};
        for (int leg = 0; leg < 4; ++leg) {
          const float la = float(leg) * (kPi * 0.5f) + 0.7854f;
          const float lx0 = std::cos(la) * 1.45f, lz0 = std::sin(la) * 1.45f;
          const float lx1 = std::cos(la) * 0.28f, lz1 = std::sin(la) * 0.28f;
          const std::array<float, 3> b0{wm.base_pos[0] + lx0 - 0.08f, gy, wm.base_pos[2] + lz0};
          const std::array<float, 3> b1{wm.base_pos[0] + lx0 + 0.08f, gy, wm.base_pos[2] + lz0};
          const std::array<float, 3> t1{wm.base_pos[0] + lx1 + 0.06f, hub_y, wm.base_pos[2] + lz1};
          const std::array<float, 3> t0{wm.base_pos[0] + lx1 - 0.06f, hub_y, wm.base_pos[2] + lz1};
          push_quad(out, b0, b1, t1, t0, steel, true);
        }
        // Horizontal cross-bracing collar + tail rudder pointing downwind.
        push_box(out, {wm.base_pos[0], gy + wm.tower_height * 0.5f, wm.base_pos[2]}, {0.82f, 0, 0}, {0, 0.09f, 0},
                 {0, 0, 0.82f}, steel);
        push_box(out, {wm.base_pos[0] - fx * 1.6f, hub_y, wm.base_pos[2] - fz * 1.6f}, {rx * 0.06f, 0, rz * 0.06f},
                 {0, 0.65f, 0}, {fx * 1.25f, 0, fz * 1.25f}, vane_col);
        // Spinning 8-blade ranch wheel facing into the wind.
        const float hx = wm.base_pos[0] + fx * 0.85f;
        const float hz = wm.base_pos[2] + fz * 0.85f;
        const float spin = time_sec * (1.25f + wind_spd * 0.90f) + wm.phase;
        for (int b = 0; b < 8; ++b) {
          const float ang = spin + float(b) * (2.0f * kPi / 8.0f);
          const float ca = std::cos(ang), sa = std::sin(ang);
          const float bx = rx * ca, by = sa, bz = rz * ca;
          const float tx = -rx * sa, ty = ca, tz = -rz * sa;
          const float r0 = wm.blade_radius * 0.22f;
          const float r1 = wm.blade_radius;
          const std::array<float, 3> p0{hx + bx * r0 + tx * 0.16f, hub_y + by * r0 + ty * 0.16f,
                                        hz + bz * r0 + tz * 0.16f};
          const std::array<float, 3> p1{hx + bx * r0 - tx * 0.16f, hub_y + by * r0 - ty * 0.16f,
                                        hz + bz * r0 - tz * 0.16f};
          const std::array<float, 3> p2{hx + bx * r1 - tx * 0.42f, hub_y + by * r1 - ty * 0.42f,
                                        hz + bz * r1 - tz * 0.42f};
          const std::array<float, 3> p3{hx + bx * r1 + tx * 0.42f, hub_y + by * r1 + ty * 0.42f,
                                        hz + bz * r1 + tz * 0.42f};
          push_quad(out, p0, p1, p2, p3, (b % 2 == 0) ? blade_col : steel, true);
        }
      }
    }

    // 2d. Horizon Festival Ferris wheels (rotating spokes, upright swinging gondolas & rim night lights).
    for (const auto& fw : state_->ferris_wheels) {
      const double dx = double(fw.base_pos[0]) - eye_x;
      const double dz = double(fw.base_pos[2]) - eye_z;
      if (dx * dx + dz * dz > 1100.0 * 1100.0) continue;
      const float gy = ground_y(fw.base_pos[0], fw.base_pos[2], fw.base_pos[1]);
      const float cs = std::cos(fw.yaw), sn = std::sin(fw.yaw);
      // Wheel plane lies along (rx, y), axle runs along (ax, az).
      const float rx = cs, rz = sn;
      const float ax = -sn, az = cs;
      const float hub_y = gy + fw.hub_height;
      const std::array<uint8_t, 4> frame_col{224, 228, 236, 255};
      const std::array<uint8_t, 4> hub_col{236, 46, 92, 255};
      const float half_w = 2.1f;

      // Twin A-frame support legs + central hub axle.
      for (int side = -1; side <= 1; side += 2) {
        const float sx = fw.base_pos[0] + ax * (half_w * 1.35f * float(side));
        const float sz = fw.base_pos[2] + az * (half_w * 1.35f * float(side));
        for (int leg = -1; leg <= 1; leg += 2) {
          const float foot_off = fw.radius * 0.55f * float(leg);
          const std::array<float, 3> b0{sx + rx * (foot_off - 0.35f), gy, sz + rz * (foot_off - 0.35f)};
          const std::array<float, 3> b1{sx + rx * (foot_off + 0.35f), gy, sz + rz * (foot_off + 0.35f)};
          const std::array<float, 3> t1{sx + rx * 0.30f, hub_y, sz + rz * 0.30f};
          const std::array<float, 3> t0{sx - rx * 0.30f, hub_y, sz - rz * 0.30f};
          push_quad(out, b0, b1, t1, t0, frame_col, true);
        }
      }
      push_box(out, {fw.base_pos[0], hub_y, fw.base_pos[2]}, {rx * 1.1f, 0, rz * 1.1f}, {0, 1.1f, 0},
               {ax * (half_w * 1.45f), 0, az * (half_w * 1.45f)}, hub_col);

      const int count = std::clamp(fw.gondolas, 6, 18);
      const float wheel_rot = time_sec * 0.22f + fw.phase;
      static constexpr std::array<std::array<uint8_t, 3>, 4> kGondolaCols{{
          {236, 46, 92}, {38, 168, 232}, {252, 206, 48}, {52, 196, 134},
      }};
      for (int i = 0; i < count; ++i) {
        const float a0 = wheel_rot + (2.0f * kPi * float(i)) / float(count);
        const float a1 = wheel_rot + (2.0f * kPi * float(i + 1)) / float(count);
        const float c0 = std::cos(a0), s0 = std::sin(a0);
        const float c1 = std::cos(a1), s1 = std::sin(a1);
        const float px0 = fw.base_pos[0] + rx * (c0 * fw.radius);
        const float py0 = hub_y + s0 * fw.radius;
        const float pz0 = fw.base_pos[2] + rz * (c0 * fw.radius);
        const float px1 = fw.base_pos[0] + rx * (c1 * fw.radius);
        const float py1 = hub_y + s1 * fw.radius;
        const float pz1 = fw.base_pos[2] + rz * (c1 * fw.radius);

        // Twin rim segments + radial spokes.
        for (int side = -1; side <= 1; side += 2) {
          const float ox = ax * (half_w * float(side));
          const float oz = az * (half_w * float(side));
          const std::array<float, 3> r0{px0 + ox, py0, pz0 + oz};
          const std::array<float, 3> r1{px1 + ox, py1, pz1 + oz};
          const std::array<float, 3> r0i{fw.base_pos[0] + rx * (c0 * (fw.radius - 0.55f)) + ox,
                                         hub_y + s0 * (fw.radius - 0.55f),
                                         fw.base_pos[2] + rz * (c0 * (fw.radius - 0.55f)) + oz};
          const std::array<float, 3> r1i{fw.base_pos[0] + rx * (c1 * (fw.radius - 0.55f)) + ox,
                                         hub_y + s1 * (fw.radius - 0.55f),
                                         fw.base_pos[2] + rz * (c1 * (fw.radius - 0.55f)) + oz};
          push_quad(out, r0, r1, r1i, r0i, frame_col, true);
          // Radial spoke from hub to rim.
          const float nx = -s0 * 0.14f, ny = c0 * 0.14f;
          const std::array<float, 3> h0{fw.base_pos[0] + ox - rx * nx, hub_y - ny, fw.base_pos[2] + oz - rz * nx};
          const std::array<float, 3> h1{fw.base_pos[0] + ox + rx * nx, hub_y + ny, fw.base_pos[2] + oz + rz * nx};
          const std::array<float, 3> e1{r0[0] + rx * nx, r0[1] + ny, r0[2] + rz * nx};
          const std::array<float, 3> e0{r0[0] - rx * nx, r0[1] - ny, r0[2] - rz * nx};
          push_quad(out, h0, h1, e1, e0, frame_col, true);
        }

        // Upright gravity-hanging gondola cabin with subtle pendulum sway + night rim LED bulb.
        const float swing = std::sin(time_sec * 1.35f + float(i) * 0.9f) * 0.18f;
        const float gx_c = px0 + rx * swing;
        const float gy_c = py0 - 1.45f;
        const float gz_c = pz0 + rz * swing;
        const auto& gc = kGondolaCols[size_t(i) % kGondolaCols.size()];
        push_box(out, {gx_c, gy_c, gz_c}, {rx * 0.92f, 0, rz * 0.92f}, {0, 0.95f, 0},
                 {ax * 1.15f, 0, az * 1.15f}, {gc[0], gc[1], gc[2], 255});
        // Festival rim night light bulb (a=96 -> subtle bulb by day, glowing HDR LED at dusk/night).
        const float led_pulse = 0.75f + 0.25f * std::sin(time_sec * 4.0f - float(i) * 0.8f);
        push_box(out, {px0, py0, pz0}, {0.36f, 0, 0}, {0, 0.36f, 0}, {0, 0, 0.36f},
                 {clamp_u8(float(gc[0]) * led_pulse), clamp_u8(float(gc[1]) * led_pulse),
                  clamp_u8(float(gc[2]) * led_pulse), 96});
      }
    }

    // 2e. Main Stage sweeping night searchlight / laser cones (truss housings + skyward sweeping beams).
    for (const auto& sl : state_->stage_lights) {
      const double dx = double(sl.base_pos[0]) - eye_x;
      const double dz = double(sl.base_pos[2]) - eye_z;
      if (dx * dx + dz * dz > 950.0 * 950.0) continue;
      const float gy = ground_y(sl.base_pos[0], sl.base_pos[2], sl.base_pos[1]);
      // Compact stage-truss projector housing (visible day & night).
      push_box(out, {sl.base_pos[0], gy + 0.55f, sl.base_pos[2]}, {0.42f, 0, 0}, {0, 0.55f, 0}, {0, 0, 0.42f},
               {48, 52, 58, 255});
      // Dual-axis Lissajous sweeping beam direction (collapses off-screen in daylight via rgba.a = 24 in world_vert).
      const float az = sl.yaw + std::sin(time_sec * 0.85f + sl.phase) * 0.95f;
      const float tilt = 0.32f + 0.24f * std::cos(time_sec * 1.15f + sl.phase * 1.4f);
      const float st = std::sin(tilt), ct = std::cos(tilt);
      const std::array<float, 3> bdir{std::sin(az) * st, ct, std::cos(az) * st};
      const std::array<float, 3> side1 = vnorm(vcross(bdir, {0.0f, 0.0f, 1.0f}));
      const std::array<float, 3> side2 = vnorm(vcross(bdir, side1));
      const std::array<float, 3> base{sl.base_pos[0], gy + 1.05f, sl.base_pos[2]};
      const std::array<float, 3> tip{base[0] + bdir[0] * sl.beam_length, base[1] + bdir[1] * sl.beam_length,
                                     base[2] + bdir[2] * sl.beam_length};
      const float r_base = 0.16f;
      const float r_tip = 0.72f;
      const std::array<uint8_t, 4> beam_rgba{sl.beam_rgb[0], sl.beam_rgb[1], sl.beam_rgb[2], 24};
      for (int card = 0; card < 2; ++card) {
        const auto& sd = (card == 0) ? side1 : side2;
        const std::array<float, 3> b0{base[0] - sd[0] * r_base, base[1] - sd[1] * r_base, base[2] - sd[2] * r_base};
        const std::array<float, 3> b1{base[0] + sd[0] * r_base, base[1] + sd[1] * r_base, base[2] + sd[2] * r_base};
        const std::array<float, 3> t1{tip[0] + sd[0] * r_tip, tip[1] + sd[1] * r_tip, tip[2] + sd[2] * r_tip};
        const std::array<float, 3> t0{tip[0] - sd[0] * r_tip, tip[1] - sd[1] * r_tip, tip[2] - sd[2] * r_tip};
        push_quad(out, b0, b1, t1, t0, beam_rgba, true);
      }
    }

    // 2f. Circling Horizon Festival blimps & banner-towing planes overhead.
    for (const auto& ab : state_->aircraft_banners) {
      const float theta = time_sec * ab.angular_speed + ab.phase;
      const float cx = ab.center_pos[0] + std::cos(theta) * ab.orbit_radius;
      const float cz = ab.center_pos[2] + std::sin(theta) * ab.orbit_radius;
      const float cy = ab.center_pos[1] + std::sin(theta * 2.0f + ab.phase) * (ab.is_blimp ? 4.0f : 6.0f);
      const double dx = double(cx) - eye_x;
      const double dz = double(cz) - eye_z;
      if (dx * dx + dz * dz > 1600.0 * 1600.0) continue;

      // Tangent velocity unit vector along circular orbit.
      const float fx = -std::sin(theta), fz = std::cos(theta);
      const float rx = std::cos(theta), rz = std::sin(theta);

      if (ab.is_blimp) {
        // Tapered Horizon Festival dirigible hull + 4 stabilizer tail fins + cabin gondola + night marquee strip.
        const std::array<uint8_t, 4> hull_main{ab.primary_rgb[0], ab.primary_rgb[1], ab.primary_rgb[2], 255};
        const std::array<uint8_t, 4> hull_acc{ab.accent_rgb[0], ab.accent_rgb[1], ab.accent_rgb[2], 255};
        const std::array<uint8_t, 4> cabin_col{64, 72, 82, 255};
        // Center body, tapered nose, tapered tail.
        push_box(out, {cx, cy, cz}, {rx * 4.4f, 0, rz * 4.4f}, {0, 4.6f, 0}, {fx * 11.5f, 0, fz * 11.5f}, hull_main);
        push_box(out, {cx + fx * 14.2f, cy, cz + fz * 14.2f}, {rx * 2.6f, 0, rz * 2.6f}, {0, 2.7f, 0},
                 {fx * 4.2f, 0, fz * 4.2f}, hull_acc);
        push_box(out, {cx - fx * 14.5f, cy, cz - fz * 14.5f}, {rx * 2.3f, 0, rz * 2.3f}, {0, 2.4f, 0},
                 {fx * 4.5f, 0, fz * 4.5f}, hull_main);
        // Vertical & horizontal tail fins.
        push_box(out, {cx - fx * 16.5f, cy, cz - fz * 16.5f}, {rx * 0.25f, 0, rz * 0.25f}, {0, 5.2f, 0},
                 {fx * 2.8f, 0, fz * 2.8f}, hull_acc);
        push_box(out, {cx - fx * 16.5f, cy, cz - fz * 16.5f}, {rx * 5.0f, 0, rz * 5.0f}, {0, 0.25f, 0},
                 {fx * 2.8f, 0, fz * 2.8f}, hull_acc);
        // Underside gondola cabin + glowing side Festival marquee strips (a=96).
        push_box(out, {cx + fx * 1.2f, cy - 5.2f, cz + fz * 1.2f}, {rx * 1.3f, 0, rz * 1.3f}, {0, 1.1f, 0},
                 {fx * 3.8f, 0, fz * 3.8f}, cabin_col);
        const float mq = 0.78f + 0.22f * std::sin(time_sec * 3.5f + ab.phase);
        const std::array<uint8_t, 4> marquee{clamp_u8(float(ab.accent_rgb[0]) * mq),
                                             clamp_u8(float(ab.accent_rgb[1]) * mq),
                                             clamp_u8(float(ab.accent_rgb[2]) * mq), 96};
        for (int s = -1; s <= 1; s += 2) {
          push_box(out, {cx + rx * (4.55f * float(s)), cy, cz + rz * (4.55f * float(s))},
                   {rx * 0.12f, 0, rz * 0.12f}, {0, 1.35f, 0}, {fx * 7.8f, 0, fz * 7.8f}, marquee);
        }
      } else {
        // Banked banner-towing biplane + spinning propeller + tow cable + 5-segment waving Horizon sky banner.
        const float bank = 0.24f;
        const float cb = std::cos(bank), sb = std::sin(bank);
        const std::array<float, 3> r_bank{rx * cb, sb, rz * cb};
        const std::array<float, 3> u_bank{-rx * sb, cb, -rz * sb};
        const std::array<uint8_t, 4> body_col{ab.primary_rgb[0], ab.primary_rgb[1], ab.primary_rgb[2], 255};
        const std::array<uint8_t, 4> wing_col{ab.accent_rgb[0], ab.accent_rgb[1], ab.accent_rgb[2], 255};
        const std::array<uint8_t, 4> dark_col{58, 62, 68, 255};
        // Fuselage, main wing, tailplane, vertical fin.
        push_box(out, {cx, cy, cz}, {r_bank[0] * 0.65f, r_bank[1] * 0.65f, r_bank[2] * 0.65f},
                 {u_bank[0] * 0.65f, u_bank[1] * 0.65f, u_bank[2] * 0.65f}, {fx * 4.4f, 0, fz * 4.4f}, body_col);
        push_box(out, {cx + fx * 0.7f, cy, cz + fz * 0.7f},
                 {r_bank[0] * 6.2f, r_bank[1] * 6.2f, r_bank[2] * 6.2f},
                 {u_bank[0] * 0.16f, u_bank[1] * 0.16f, u_bank[2] * 0.16f}, {fx * 1.15f, 0, fz * 1.15f}, wing_col);
        push_box(out, {cx - fx * 3.8f, cy, cz - fz * 3.8f},
                 {r_bank[0] * 2.1f, r_bank[1] * 2.1f, r_bank[2] * 2.1f},
                 {u_bank[0] * 0.12f, u_bank[1] * 0.12f, u_bank[2] * 0.12f}, {fx * 0.65f, 0, fz * 0.65f}, wing_col);
        push_box(out, {cx - fx * 3.8f + u_bank[0] * 0.9f, cy + u_bank[1] * 0.9f, cz - fz * 3.8f + u_bank[2] * 0.9f},
                 {r_bank[0] * 0.12f, r_bank[1] * 0.12f, r_bank[2] * 0.12f},
                 {u_bank[0] * 1.1f, u_bank[1] * 1.1f, u_bank[2] * 1.1f}, {fx * 0.75f, 0, fz * 0.75f}, body_col);
        // Spinning nose propeller blades.
        const float pa = time_sec * 24.0f + ab.phase;
        const float pc = std::cos(pa), ps = std::sin(pa);
        const std::array<float, 3> p_dir{r_bank[0] * pc + u_bank[0] * ps, r_bank[1] * pc + u_bank[1] * ps,
                                         r_bank[2] * pc + u_bank[2] * ps};
        push_box(out, {cx + fx * 4.55f, cy, cz + fz * 4.55f}, {p_dir[0] * 1.35f, p_dir[1] * 1.35f, p_dir[2] * 1.35f},
                 {fx * 0.08f, 0.12f, fz * 0.08f}, {0.08f, 0, 0.08f}, dark_col);
        // Trailing tow rope + 5-segment rippling Horizon Festival sky banner.
        const float rope_len = 11.0f;
        const float ban_len = 22.0f;
        const float ban_h = 3.2f;
        const std::array<float, 3> tail_pt{cx - fx * 4.4f, cy, cz - fz * 4.4f};
        const std::array<float, 3> ban_start{cx - fx * (4.4f + rope_len), cy - 0.6f, cz - fz * (4.4f + rope_len)};
        push_quad(out, tail_pt, ban_start, {ban_start[0], ban_start[1] + 0.12f, ban_start[2]},
                  {tail_pt[0], tail_pt[1] + 0.12f, tail_pt[2]}, dark_col, true);
        constexpr int kBanSegs = 5;
        for (int s = 0; s < kBanSegs; ++s) {
          const float t0 = float(s) / float(kBanSegs);
          const float t1 = float(s + 1) / float(kBanSegs);
          const float d0 = t0 * ban_len;
          const float d1 = t1 * ban_len;
          const float w0 = std::sin(time_sec * 5.2f - t0 * 4.8f + ab.phase) * 0.85f * t0;
          const float w1 = std::sin(time_sec * 5.2f - t1 * 4.8f + ab.phase) * 0.85f * t1;
          const std::array<float, 3> q00{ban_start[0] - fx * d0 + rx * w0, ban_start[1] - ban_h * 0.5f,
                                         ban_start[2] - fz * d0 + rz * w0};
          const std::array<float, 3> q10{ban_start[0] - fx * d1 + rx * w1, ban_start[1] - ban_h * 0.5f,
                                         ban_start[2] - fz * d1 + rz * w1};
          const std::array<float, 3> q11{q10[0], ban_start[1] + ban_h * 0.5f, q10[2]};
          const std::array<float, 3> q01{q00[0], ban_start[1] + ban_h * 0.5f, q00[2]};
          const auto& bc = (s % 2 == 0) ? body_col : wing_col;
          push_quad(out, q00, q10, q11, q01, bc, true);
        }
      }
    }

    // 2g. Intersection traffic light housings with smooth green/yellow/red emissive cycling.
    for (const auto& tl : state_->traffic_lights) {
      const double dx = double(tl.base_pos[0]) - eye_x;
      const double dz = double(tl.base_pos[2]) - eye_z;
      if (dx * dx + dz * dz > 320.0 * 320.0) continue;
      const float gy = ground_y(tl.base_pos[0], tl.base_pos[2], tl.base_pos[1]);
      const float ax = std::cos(tl.yaw), az = std::sin(tl.yaw);
      const float fx = -az, fz = ax;  // signal face perpendicular to cantilever arm
      const std::array<uint8_t, 4> pole_col{88, 94, 100, 255};
      const std::array<uint8_t, 4> housing_col{34, 36, 40, 255};
      // Vertical shoulder pole + horizontal cantilever mast arm.
      push_box(out, {tl.base_pos[0], gy + tl.pole_height * 0.5f, tl.base_pos[2]}, {0.14f, 0, 0},
               {0, tl.pole_height * 0.5f, 0}, {0, 0, 0.14f}, pole_col);
      const float arm_y = gy + tl.pole_height - 0.35f;
      push_box(out, {tl.base_pos[0] + ax * (tl.arm_length * 0.5f), arm_y, tl.base_pos[2] + az * (tl.arm_length * 0.5f)},
               {ax * (tl.arm_length * 0.5f), 0, az * (tl.arm_length * 0.5f)}, {0, 0.11f, 0},
               {fx * 0.11f, 0, fz * 0.11f}, pole_col);
      // Hanging dark signal head housing + 3 smooth-cycling Red/Yellow/Green emissive lenses.
      const float hx = tl.base_pos[0] + ax * (tl.arm_length - 0.55f);
      const float hz = tl.base_pos[2] + az * (tl.arm_length - 0.55f);
      const float hy = arm_y - 0.72f;
      push_box(out, {hx, hy, hz}, {ax * 0.24f, 0, az * 0.24f}, {0, 0.62f, 0}, {fx * 0.20f, 0, fz * 0.20f},
               housing_col);

      const TrafficLightWeights lw = evaluate_traffic_light_cycle(time_sec, tl.phase_offset);
      auto lens_rgba = [](float w, uint8_t r, uint8_t g, uint8_t b) -> std::array<uint8_t, 4> {
        const float lit = 0.14f + 0.86f * std::clamp(w, 0.0f, 1.0f);
        // Active lens uses alpha < 0.98 (175) so world_frag adds crisp emissive signal glow day & night.
        const uint8_t alpha = (w > 0.15f) ? clamp_u8(255.0f - 85.0f * std::clamp(w, 0.0f, 1.0f)) : 255u;
        return {clamp_u8(float(r) * lit), clamp_u8(float(g) * lit), clamp_u8(float(b) * lit), alpha};
      };
      const std::array<uint8_t, 4> red_c = lens_rgba(lw.red, 255, 42, 34);
      const std::array<uint8_t, 4> yel_c = lens_rgba(lw.yellow, 255, 198, 28);
      const std::array<uint8_t, 4> grn_c = lens_rgba(lw.green, 38, 245, 96);
      const float lens_y[3] = {hy + 0.36f, hy, hy - 0.36f};
      const std::array<uint8_t, 4> lens_cols[3] = {red_c, yel_c, grn_c};
      for (int li = 0; li < 3; ++li) {
        // Emit lenses on both front and back faces of the intersection signal housing.
        for (int dir = -1; dir <= 1; dir += 2) {
          push_box(out, {hx + fx * (0.21f * float(dir)), lens_y[li], hz + fz * (0.21f * float(dir))},
                   {ax * 0.14f, 0, az * 0.14f}, {0, 0.14f, 0}, {fx * 0.03f, 0, fz * 0.03f}, lens_cols[li]);
        }
      }
    }
  }

  // 3. Roadside wind-swaying grass quads (_0x00005007 / TrackSettings grass parameters)
  // with per-spatial-cell cached ground heights (eliminates ~800 uncached CPU BVH raycasts/frame).
  if (include_grass && ground && post_cfg.grass_height_scale > 0.01f) {
    const float max_r = std::clamp(post_cfg.grass_fade_end * 0.20f, 22.0f, 52.0f);
    const float fade_start = std::clamp(post_cfg.grass_fade_start * 0.14f, 10.0f, max_r - 4.0f);
    const float cell = 2.8f;
    const int cx0 = int(std::floor((eye_x - max_r) / cell));
    const int cx1 = int(std::floor((eye_x + max_r) / cell));
    const int cz0 = int(std::floor((eye_z - max_r) / cell));
    const int cz1 = int(std::floor((eye_z + max_r) / cell));
    constexpr size_t kMaxGrassVerts = 18000;
    const size_t start_v = out.size();

    for (int gx = cx0; gx <= cx1 && (out.size() - start_v) < kMaxGrassVerts; ++gx) {
      for (int gz = cz0; gz <= cz1 && (out.size() - start_v) < kMaxGrassVerts; ++gz) {
        if (hash_01(gx, gz, 73u) < 0.42f) continue;  // natural patchy distribution
        const float ox = (float(gx) + 0.15f + 0.70f * hash_01(gx, gz, 101u)) * cell;
        const float oz = (float(gz) + 0.15f + 0.70f * hash_01(gx, gz, 137u)) * cell;
        const float dx = ox - float(eye_x), dz = oz - float(eye_z);
        const float d2 = dx * dx + dz * dz;
        if (d2 > max_r * max_r || d2 < 2.5f * 2.5f) continue;

        const uint64_t ckey = pack_grid_key(gx, gz);
        CachedGrassCell sample{};
        if (const auto it = state_->grass_ground_cache.find(ckey); it != state_->grass_ground_cache.end()) {
          sample = it->second;
        } else {
          drive::Hit h;
          const double start_y = eye_y + 20.0;
          if (ground->raycast({double(ox), start_y, double(oz)}, {0.0, -1.0, 0.0}, 60.0, h) && h.normal.y >= 0.75) {
            // Strict allowlist from physics/surfaceTypes.xml: only grow grass on Grass (2),
            // Grasscrete (13), and SlowGrass (25) — never on Asphalt (0), Dirt roads, or kerbs.
            if (h.surface == 2 || h.surface == 13 || h.surface == 25) {
              sample.y = float(start_y - h.distance);
              sample.grow = true;
            }
          }
          state_->grass_ground_cache.emplace(ckey, sample);
        }
        if (!sample.grow) continue;

        const float dist = std::sqrt(d2);
        const float fade = 1.0f - std::clamp((dist - fade_start) / std::max(max_r - fade_start, 1.0f), 0.0f, 1.0f);
        if (fade <= 0.05f) continue;

        const float h_rand = 0.16f + 0.18f * hash_01(gx, gz, 211u);
        const float gh = h_rand * std::clamp(post_cfg.grass_height_scale, 0.3f, 2.0f) * fade;
        const float w = 0.035f + 0.028f * hash_01(gx, gz, 251u);
        const float gy = sample.y;
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
}

void AnimatedWorldSystem::build_spectator_mesh(double eye_x, double eye_y, double eye_z, float time_sec,
                                               const drive::Ground* ground, std::vector<SpectatorVertex>& out) const {
  build_spectator_mesh(eye_x, eye_y, eye_z, time_sec, ground, out, nullptr, nullptr, nullptr);
}

void AnimatedWorldSystem::build_spectator_mesh(double eye_x, double eye_y, double eye_z, float time_sec,
                                               const drive::Ground* ground, std::vector<SpectatorVertex>& out,
                                               std::vector<std::array<float, 4>>* shadow_discs) const {
  build_spectator_mesh(eye_x, eye_y, eye_z, time_sec, ground, out, shadow_discs, nullptr, nullptr);
}

void AnimatedWorldSystem::build_spectator_mesh(double eye_x, double eye_y, double eye_z, float time_sec,
                                               const drive::Ground* ground, std::vector<SpectatorVertex>& out,
                                               std::vector<std::array<float, 4>>* shadow_discs,
                                               const SpectatorFrustum* frustum,
                                               float* out_nearest_dist) const {
  (void)eye_y;
  if (out_nearest_dist) *out_nearest_dist = 1e6f;
  const auto& cat = state_->catalog;
  const auto& crowds = state_->crowds;
  const auto& post_cfg = state_->post_cfg;
  if (!cat.has_3d_spectators() || crowds.empty()) return;
  const float inv_grid = 1.0f / float(SpectatorCatalog::kAtlasGrid);
  const float uv_pad = 1.0f / float(SpectatorCatalog::kAtlasCell);
  const auto& stand_skel = cat.has_stand_skeleton_data ? cat.stand_skeleton : kDefaultStandSkeleton;
  const auto& sit_skel = cat.has_sit_skeleton_data ? cat.sit_skeleton : kDefaultSitSkeleton;
  const float sprite_dist = std::clamp(post_cfg.crowd_sprite_distance, 25.0f, 115.0f);
  const float max_crowd_dist = std::max(120.0f, sprite_dist + 65.0f);

  const bool use_frustum = (frustum != nullptr && frustum->active);
  auto frustum_visible = [&](double gx, float gy, double gz, float radius) -> bool {
    if (!use_frustum) return true;
    const float sx = float(gx - frustum->origin_x);
    const float sz = float(gz - frustum->origin_z);
    for (const auto& pl : frustum->planes) {
      if (pl[0] * sx + pl[1] * gy + pl[2] * sz + pl[3] < -radius) return false;
    }
    return true;
  };

  // Precompute normalized TrackSettings <Crowds> dark/light tint bounds once per call.
  std::array<float, 3> tint_dark{}, tint_light{};
  for (int ch = 0; ch < 3; ++ch) {
    tint_dark[size_t(ch)] = std::clamp(0.72f + 0.28f * post_cfg.crowd_dark[ch], 0.55f, 1.0f) * 255.0f;
    tint_light[size_t(ch)] = std::clamp(0.84f + 0.16f * std::min(post_cfg.crowd_light[ch], 1.5f), 0.75f, 1.0f) * 255.0f;
  }
  auto eval_crowd_tint = [&](float ny) -> std::array<uint8_t, 4> {
    const float up_f = std::clamp(0.5f + 0.5f * ny, 0.0f, 1.0f);
    const float dn_f = 1.0f - up_f;
    return {
        clamp_u8(tint_dark[0] * dn_f + tint_light[0] * up_f),
        clamp_u8(tint_dark[1] * dn_f + tint_light[1] * up_f),
        clamp_u8(tint_dark[2] * dn_f + tint_light[2] * up_f),
        255u,
    };
  };

  struct GroundSample {
    float y = 0.0f;
    bool hit = false;
    bool blocked = false;
    bool on_road = false;
    bool steep = false;
  };
  auto sample_ground_uncached = [&](float x, float z, float fallback) -> GroundSample {
    GroundSample gs{fallback, false, false, false, false};
    if (!ground) return gs;
    drive::Hit h;
    // Start 3.5 m above the cluster's known ground elevation (never 16 m+ above eye_y)
    // so overhead outpost canopies, festival tents, and banners never trap spectators on roofs.
    const double start_y = double(fallback) + 3.5;
    if (ground->raycast({double(x), start_y, double(z)}, {0.0, -1.0, 0.0}, 12.0, h)) {
      const float hy = float(start_y - h.distance);
      if (std::fabs(hy - fallback) <= 4.0f) {
        gs.hit = true;
        gs.y = hy;
        gs.on_road = is_road_surface(h.surface);
        gs.blocked = is_obstacle_surface(h.surface);
        gs.steep = (h.normal.y < 0.72);
        return gs;
      }
    }
    // Fallback shorter probe right around cluster elevation in case of steep canopy/bridge above.
    const double low_start = double(fallback) + 1.6;
    if (ground->raycast({double(x), low_start, double(z)}, {0.0, -1.0, 0.0}, 6.0, h)) {
      gs.hit = true;
      gs.y = float(low_start - h.distance);
      gs.on_road = is_road_surface(h.surface);
      gs.blocked = is_obstacle_surface(h.surface);
      gs.steep = (h.normal.y < 0.72);
    }
    return gs;
  };

  // Hold cache lock for fast cache/pool prep and per-spectator cached lookup.
  std::lock_guard<std::mutex> cache_lock(state_->cache_mutex);

  // Lazily build standing/sitting asset pools & crowdclass lookup once per catalog load.
  if (!state_->pools_ready || state_->pools_stand_count != cat.assets.size() ||
      state_->pools_sit_count != cat.sit_assets.size() || state_->pools_cc_count != cat.crowdclasses.size()) {
    state_->all_stand_pool.clear();
    state_->male_stand_pool.clear();
    state_->all_stand_pool.reserve(cat.assets.size());
    state_->male_stand_pool.reserve(cat.assets.size());
    for (size_t a = 0; a < cat.assets.size(); ++a) {
      state_->all_stand_pool.push_back(a);
      if (cat.assets[a].is_male) state_->male_stand_pool.push_back(a);
    }
    if (state_->male_stand_pool.empty()) state_->male_stand_pool = state_->all_stand_pool;

    state_->all_sit_pool.clear();
    state_->male_sit_pool.clear();
    state_->all_sit_pool.reserve(cat.sit_assets.size());
    state_->male_sit_pool.reserve(cat.sit_assets.size());
    for (size_t a = 0; a < cat.sit_assets.size(); ++a) {
      state_->all_sit_pool.push_back(a);
      if (cat.sit_assets[a].is_male) state_->male_sit_pool.push_back(a);
    }
    if (state_->male_sit_pool.empty()) state_->male_sit_pool = state_->all_sit_pool;

    state_->cc_by_low.clear();
    state_->cc_by_low.reserve(cat.crowdclasses.size());
    for (const auto& cc : cat.crowdclasses) {
      state_->cc_by_low[lower_ascii(cc.name)] = &cc;
    }
    state_->pools_stand_count = cat.assets.size();
    state_->pools_sit_count = cat.sit_assets.size();
    state_->pools_cc_count = cat.crowdclasses.size();
    state_->pools_ready = true;
  }

  // Reset cached spectator ground heights whenever the Ground pointer changes or crowd layout resizes.
  if (ground != state_->cached_ground_ptr || state_->crowd_ground_cache.size() != crowds.size()) {
    state_->cached_ground_ptr = ground;
    state_->crowd_ground_cache.assign(crowds.size(), {});
  }

  const auto& all_stand_pool = state_->all_stand_pool;
  const auto& male_stand_pool = state_->male_stand_pool;
  const auto& all_sit_pool = state_->all_sit_pool;
  const auto& male_sit_pool = state_->male_sit_pool;
  const auto& cc_by_low = state_->cc_by_low;

  // Sort nearby visible clusters closest-first so high-detail near spectators are never truncated by GPU vertex cap.
  struct ClusterRef {
    size_t idx;
    double dist2;
  };
  std::vector<ClusterRef> active_clusters;
  active_clusters.reserve(16);
  const double cull_r = double(max_crowd_dist) + 25.0;
  for (size_t ci = 0; ci < crowds.size(); ++ci) {
    const auto& cl = crowds[ci];
    const double mid = 0.5 * double(std::max(0, cl.count - 1)) * double(cl.spacing);
    const double mid_x = double(cl.origin[0]) + double(cl.dir_x) * mid;
    const double mid_z = double(cl.origin[2]) + double(cl.dir_z) * mid;
    const double cdx = mid_x - eye_x;
    const double cdz = mid_z - eye_z;
    const double d2 = cdx * cdx + cdz * cdz;
    if (d2 > cull_r * cull_r) continue;
    const float cl_radius = float(mid) + 3.5f;
    if (!frustum_visible(mid_x, cl.origin[1] + 1.0f, mid_z, cl_radius)) continue;
    active_clusters.push_back({ci, d2});
  }
  std::sort(active_clusters.begin(), active_clusters.end(),
            [](const ClusterRef& a, const ClusterRef& b) { return a.dist2 < b.dist2; });

  auto slot_uv_off = [&](uint32_t slot) -> std::array<float, 2> {
    return {float(slot % SpectatorCatalog::kAtlasGrid) * inv_grid,
            float(slot / SpectatorCatalog::kAtlasGrid) * inv_grid};
  };

  // Thread-local scratch buffer for unique-vertex skinning (4-6x faster than per-index skinning).
  thread_local std::vector<SpectatorVertex> tl_posed;
  thread_local std::vector<uint32_t> tl_stamp;
  thread_local uint32_t tl_gen = 1u;

  float nearest_dist = 1e6f;
  std::vector<const SpectatorAnimClip*> class_clips;
  class_clips.reserve(16);

  for (const auto& cref : active_clusters) {
    const size_t ci = cref.idx;
    const auto& cl = crowds[ci];
    const std::string low_cc = lower_ascii(cl.crowdclass);
    const bool is_gig = (low_cc.find("gig") != std::string::npos || low_cc.find("cheer") != std::string::npos);
    const bool is_sit_class =
        !cat.sit_assets.empty() &&
        ((low_cc == "sit" || low_cc == "loopingsit") ||
         (low_cc.find("sit") != std::string::npos && low_cc.find("satshoulders") == std::string::npos));
    const bool is_male_only = (low_cc.find("sat") != std::string::npos && low_cc.find("02") == std::string::npos &&
                               low_cc.find("03") == std::string::npos);
    const auto& active_assets = is_sit_class ? cat.sit_assets : cat.assets;
    const auto& pool = is_sit_class ? (is_male_only ? male_sit_pool : all_sit_pool)
                                    : (is_male_only ? male_stand_pool : all_stand_pool);
    const size_t pool_n = pool.size();
    if (pool_n == 0) continue;

    auto& cl_ground_cache = state_->crowd_ground_cache[ci];
    if (cl.count > 0 && cl_ground_cache.size() != size_t(cl.count)) {
      cl_ground_cache.assign(size_t(cl.count), CachedSpectatorGround{});
    }

    // Resolve `.anim.bin` clip list for this crowdclass from parsed spectators.xml.
    class_clips.clear();
    if (const auto cc_it = cc_by_low.find(low_cc); cc_it != cc_by_low.end() && cc_it->second) {
      auto add_clips = [&](const std::vector<std::string>& names) {
        for (const auto& an : names) {
          if (const auto clip_it = cat.anim_clips.find(lower_ascii(an)); clip_it != cat.anim_clips.end()) {
            class_clips.push_back(&clip_it->second);
          }
        }
      };
      add_clips(cc_it->second->cheer_anims);
      add_clips(cc_it->second->idle_anims);
    }

    // Pick a coprime stride per cluster so adjacent spectators never repeat the same character/outfit.
    size_t stride = 1;
    if (pool_n > 3) {
      static constexpr size_t kPrimes[5] = {3, 7, 11, 13, 17};
      for (size_t p_idx = 0; p_idx < 5; ++p_idx) {
        const size_t cand = kPrimes[(ci + p_idx) % 5] % pool_n;
        if (cand == 0) continue;
        size_t a = cand, b = pool_n;
        while (b != 0) {
          const size_t t = a % b;
          a = b;
          b = t;
        }
        if (a == 1) {
          stride = cand;
          break;
        }
      }
    }
    const size_t start_asset = size_t(hash_u32(uint32_t(ci) * 4099u + 97u)) % pool_n;

    const float base_yaw = std::atan2(cl.face_x, cl.face_z);
    float prev_along = -1e9f;

    for (int i = 0; i < cl.count; ++i) {
      const uint32_t h0 = hash_u32(uint32_t(ci) * 1973u + uint32_t(i) * 9277u + 1337u);
      const uint32_t h1 = hash_u32(h0 ^ 0x517cc1b7u);
      const uint32_t h2 = hash_u32(h1 ^ 0x9e3779b9u);
      const uint32_t h3 = hash_u32(h2 ^ 0x85ebca6bu);
      const float r0 = float(h0 & 0xFFFFu) / 65535.0f;
      const float r1 = float(h1 & 0xFFFFu) / 65535.0f;
      const float r2 = float(h2 & 0xFFFFu) / 65535.0f;
      const float r3 = float(h3 & 0xFFFFu) / 65535.0f;

      // Natural pair/small-group clumping along the barrier line (never ruler-straight soldiers),
      // while enforcing >= 0.62 m shoulder clearance so adjacent spectators never clip.
      const float clump = std::sin(float(i) * 1.85f + float(ci) * 0.73f) * 0.20f * cl.spacing;
      float along = float(i) * cl.spacing + clump + (r0 - 0.5f) * 0.26f * cl.spacing;
      if (i > 0 && along < prev_along + 0.62f) {
        along = prev_along + 0.62f;
      }
      prev_along = along;

      // Non-negative perpendicular offset pushes slightly back behind barrier rails, never into them.
      const float perp = r1 * 0.26f;
      const float px = cl.origin[0] + cl.dir_x * along - cl.face_x * perp;
      const float pz = cl.origin[2] + cl.dir_z * along - cl.face_z * perp;

      // Distance cull BEFORE any ground raycast or skinning.
      const double dx = double(px) - eye_x;
      const double dz = double(pz) - eye_z;
      const double d2 = dx * dx + dz * dz;
      if (d2 > double(max_crowd_dist) * double(max_crowd_dist)) continue;

      // Use cached ground height once probed with a real terrain hit (eliminates per-frame raycasts).
      float py = cl.origin[1] + cl.dir_y * along;
      auto& gcache = cl_ground_cache[size_t(i)];
      if (gcache.valid) {
        if (gcache.culled) continue;
        py = gcache.y;
      } else if (ground) {
        const GroundSample gs = sample_ground_uncached(px, pz, py);
        if (gs.hit) {
          gcache.valid = true;
          gcache.y = gs.y;
          gcache.culled = (gs.blocked || gs.steep || (cl.require_offroad && gs.on_road));          if (gcache.culled) continue;
          py = gs.y;
        }
      }

      // Per-spectator frustum sphere test (generous 2.2 m margin so waving arms & shadow discs never pop).
      if (!frustum_visible(double(px), py + 0.9f, double(pz), 2.2f)) continue;

      const float dist = float(std::sqrt(d2));
      if (dist < nearest_dist) nearest_dist = dist;

      const auto& asset = active_assets[pool[(start_asset + size_t(i) * stride) % pool_n]];
      const auto diff_off = slot_uv_off(asset.atlas_slot);
      const auto lm_off = slot_uv_off(cl.stage_lit ? SpectatorCatalog::kNightLitSlot : SpectatorCatalog::kNightDimSlot);

      const float yaw = base_yaw + (r2 - 0.5f) * 0.38f;
      const float cyaw = std::cos(yaw);
      const float syaw = std::sin(yaw);
      // Natural human height variation (~1.68 m to 1.86 m for FH1's ~1.79 m rigs).
      const float scale = asset.is_male ? (0.96f + 0.08f * r0) : (0.93f + 0.07f * r0);

      // Distant 10-angle impostor billboard beyond TrackSettings `crowd_sprite_distance` (when sprites.xds loaded).
      if (dist >= sprite_dist && cat.has_impostor_sprites) {
        const float to_ex = float(-dx) / std::max(dist, 1e-3f);
        const float to_ez = float(-dz) / std::max(dist, 1e-3f);
        const float fwd_dot = to_ex * syaw + to_ez * cyaw;
        const float left_dot = to_ex * cyaw - to_ez * syaw;
        const float rel_az = std::atan2(left_dot, fwd_dot);
        const int angle_idx = select_impostor_angle(rel_az, cat.num_sprite_angles, cat.sprites_both_sides);
        const uint32_t imp_slot = SpectatorCatalog::kImpostorBaseSlot + uint32_t(angle_idx % int(SpectatorCatalog::kMaxImpostorSlots));
        const auto imp_off = slot_uv_off(imp_slot);

        // Camera-facing vertical quad (6 vertices instead of 500+ 3D mesh vertices).
        const float right_x = -to_ez;
        const float right_z = to_ex;
        const float half_w = 0.36f * scale;
        const float height = (is_sit_class ? 1.38f : 1.78f) * scale;
        const float bob = std::sin(time_sec * (3.2f + r2 * 1.4f) + r1 * 6.283f) * 0.06f;
        const float u0 = imp_off[0] + uv_pad * inv_grid;
        const float u1 = imp_off[0] + (1.0f - uv_pad) * inv_grid;
        const float v0 = imp_off[1] + uv_pad * inv_grid;
        const float v1 = imp_off[1] + (1.0f - uv_pad) * inv_grid;
        const std::array<float, 3> n_face{to_ex, 0.25f, to_ez};
        const auto tint_top = eval_crowd_tint(0.85f);
        const auto tint_bot = eval_crowd_tint(-0.35f);
        const float lm_u = lm_off[0] + 0.5f * inv_grid;
        const float lm_vt = lm_off[1] + 0.12f * inv_grid;
        const float lm_vb = lm_off[1] + 0.90f * inv_grid;

        const SpectatorVertex bl{{px - right_x * half_w, py, pz - right_z * half_w}, n_face, {u0, v1}, tint_bot, {lm_u, lm_vb}};
        const SpectatorVertex br{{px + right_x * half_w, py, pz + right_z * half_w}, n_face, {u1, v1}, tint_bot, {lm_u, lm_vb}};
        const SpectatorVertex tr{{px + right_x * half_w, py + height + bob, pz + right_z * half_w}, n_face, {u1, v0}, tint_top, {lm_u, lm_vt}};
        const SpectatorVertex tl{{px - right_x * half_w, py + height + bob, pz - right_z * half_w}, n_face, {u0, v0}, tint_top, {lm_u, lm_vt}};
        out.push_back(bl);
        out.push_back(br);
        out.push_back(tr);
        out.push_back(bl);
        out.push_back(tr);
        out.push_back(tl);
        continue;
      }

      // WORLD-14: spectators do not turn their heads toward the car; the
      // original plays its clips as authored (no gaze).
      const float gaze_yaw = 0.0f;

      // Derive per-spectator tempo, phase, and lively energy envelope from parsed `.anim.bin` clips,
      // boosted naturally as the player car approaches within 35 m.
      float clip_dur = is_gig ? 1.85f : 2.65f;
      float clip_energy = is_gig ? 0.95f : 0.78f;
      if (!class_clips.empty()) {
        const auto* clip = class_clips[(h2 >> 8) % class_clips.size()];
        clip_dur = std::clamp(clip->duration_sec, 0.8f, 10.0f);
        clip_energy = std::max(clip->energy, 0.55f);
      }
      const float phase = r1 * 6.2831853f;
      const float clip_cycle = (2.0f * kPi) / clip_dur;
      const float freq = std::clamp(clip_cycle * (is_gig ? 1.65f : 1.45f) + 0.85f * r2, 2.2f, 5.4f);
      const float prox_excite = 1.0f + 0.35f * std::clamp((35.0f - dist) / 25.0f, 0.0f, 1.0f);
      const float env =
          std::clamp(((0.70f + 0.35f * clip_energy) + 0.22f * std::sin(time_sec * (0.55f + 0.30f * r3) + phase * 2.1f)) *
                         prox_excite,
                     0.65f, 1.35f);
      const float beat = std::sin(time_sec * freq + phase);
      const float beat_off = std::sin(time_sec * freq + phase + 0.75f);
      const float beat2 = std::sin(time_sec * (freq * 2.0f) + phase * 1.5f);
      const float sway = std::cos(time_sec * (freq * 0.55f) + phase * 1.3f);
      const float idle_look = std::sin(time_sec * (0.85f + 0.45f * r3) + phase * 1.7f) * 0.36f;
      const bool mirror = ((h3 >> 16) & 1u) != 0u;

      // Choose from 8 authentic FH1 festival poses (lively candid mix: cheering, fist-pumping,
      // clapping, overhead waving, barrier wave-bursts, grooving/bouncing, and filming passing cars).
      const uint32_t style_roll = (h0 >> 16) % 10u;
      uint32_t style = 5u;
      if (is_gig) {
        static constexpr uint32_t kGigStyles[10] = {0, 1, 2, 3, 6, 1, 2, 6, 0, 3};
        style = kGigStyles[style_roll];
      } else {
        static constexpr uint32_t kBarrierStyles[10] = {1, 2, 4, 5, 6, 7, 0, 3, 4, 2};
        style = kBarrierStyles[style_roll];
      }

      // Build exact hierarchical Forward Kinematics transforms (19-bone Standing or 15-bone Sitting rig).
      std::array<BoneXform, SpectatorMeshLod::kMaxBones> bone_xf{};
      if (asset.mesh.has_sit_skeleton) {
        std::array<Mat3, SpectatorCatalog::kSitBoneCount> local_r{};
        for (auto& m : local_r) m = mat3_id();

        const float spine_pitch = 0.06f + 0.11f * beat * env;
        const float spine_yaw = 0.16f * sway + gaze_yaw * 0.30f;
        const float head_yaw = gaze_yaw * 0.80f + idle_look * 0.70f;
        const float head_pitch = -0.04f + 0.12f * beat * env;
        float l_up = 0.45f, l_out = 0.16f, l_twist = 0.36f, l_elbow = 1.05f;
        float r_up = 0.45f, r_out = 0.16f, r_twist = 0.36f, r_elbow = 1.05f;
        if (style == 0u || style == 3u || style == 6u) {
          const float pump = 0.54f * beat * env;
          l_up = 1.75f + pump;
          l_out = 0.32f + 0.22f * sway * env;
          l_elbow = 0.95f - 0.45f * beat * env;
          r_up = 1.72f + 0.48f * beat_off * env;
          r_out = 0.30f - 0.22f * sway * env;
          r_elbow = 0.98f - 0.40f * beat_off * env;
        } else if (style == 2u) {
          const float clap = 0.5f + 0.5f * beat2;
          l_up = 0.68f + 0.14f * beat;
          l_out = 0.04f + 0.32f * clap * env;
          l_twist = 0.86f - 0.40f * clap * env;
          l_elbow = 1.58f - 0.24f * clap * env;
          r_up = l_up;
          r_out = l_out;
          r_twist = l_twist;
          r_elbow = l_elbow;
        } else if (style == 1u || style == 4u) {
          l_up = 1.65f + 0.62f * beat * env;
          l_out = 0.28f + 0.16f * sway;
          l_elbow = 1.18f - 0.55f * beat * env;
          r_up = 0.30f + 0.24f * sway;
          r_out = 0.18f;
          r_elbow = 0.82f + 0.24f * beat;
        } else {
          // Seated groove / cheering sway.
          l_up = 0.55f + 0.42f * sway * env;
          l_out = 0.18f + 0.14f * beat;
          l_elbow = 1.05f - 0.28f * sway;
          r_up = 0.55f - 0.42f * sway * env;
          r_out = 0.18f - 0.14f * beat;
          r_elbow = 1.05f + 0.28f * sway;
        }
        if (mirror) {
          std::swap(l_up, r_up);
          std::swap(l_out, r_out);
          std::swap(l_twist, r_twist);
          std::swap(l_elbow, r_elbow);
        }
        local_r[6] = mat3_mul(rot_y(spine_yaw), rot_x(spine_pitch));
        local_r[7] = rot_y(head_yaw * 0.35f);
        local_r[14] = mat3_mul(rot_y(head_yaw * 0.65f), rot_x(head_pitch));
        local_r[9] = make_arm_rot(+1.0f, l_up, l_out, l_twist);
        local_r[10] = rot_x(-std::clamp(l_elbow, 0.1f, 2.15f));
        local_r[12] = make_arm_rot(-1.0f, r_up, r_out, r_twist);
        local_r[13] = rot_x(-std::clamp(r_elbow, 0.1f, 2.15f));

        std::array<std::array<float, 3>, SpectatorCatalog::kSitBoneCount> bind_p{};
        std::array<BoneXform, SpectatorCatalog::kSitBoneCount> world_xf{};
        for (size_t b = 0; b < SpectatorCatalog::kSitBoneCount; ++b) {
          bind_p[b] = {sit_skel[b].bind_world[0], sit_skel[b].bind_world[1] - asset.mesh.min_y, sit_skel[b].bind_world[2]};
          const int16_t par = sit_skel[b].parent;
          if (par < 0) {
            world_xf[b].r = local_r[b];
            world_xf[b].t = bind_p[b];
          } else {
            const auto& pw = world_xf[size_t(par)];
            const std::array<float, 3> local_off{
                bind_p[b][0] - bind_p[size_t(par)][0],
                bind_p[b][1] - bind_p[size_t(par)][1],
                bind_p[b][2] - bind_p[size_t(par)][2],
            };
            const auto rot_off = mat3_vec(pw.r, local_off);
            world_xf[b].r = mat3_mul(pw.r, local_r[b]);
            world_xf[b].t = {pw.t[0] + rot_off[0], pw.t[1] + rot_off[1], pw.t[2] + rot_off[2]};
          }
          const auto neg_bind = mat3_vec(world_xf[b].r, bind_p[b]);
          bone_xf[b].r = world_xf[b].r;
          bone_xf[b].t = {world_xf[b].t[0] - neg_bind[0], world_xf[b].t[1] - neg_bind[1],
                          world_xf[b].t[2] - neg_bind[2]};
        }
      } else if (asset.mesh.has_stand_skeleton) {
        std::array<Mat3, SpectatorCatalog::kStandBoneCount> local_r{};
        for (auto& m : local_r) m = mat3_id();

        float hip_x = sway * 0.042f;
        float hip_z = 0.0f;
        float hop_y = 0.0f;
        float knee_bend = 0.06f + 0.14f * (0.5f + 0.5f * beat) * env;
        float spine_pitch = 0.05f;  // + = lean forward toward road
        float spine_yaw = 0.14f * sway;
        float spine_roll = 0.08f * sway;
        float head_pitch = -0.02f + 0.11f * beat * env;
        float head_yaw = gaze_yaw + idle_look * (1.0f - std::min(std::fabs(gaze_yaw) * 1.2f, 0.75f));

        // Left & right arm anatomical parameters (lively festival choreography — clearly animated from road).
        float l_up = 0.24f, l_out = 0.10f, l_twist = 0.22f, l_elbow = 0.52f, l_wrist_z = 0.0f;
        float r_up = 0.24f, r_out = 0.10f, r_twist = 0.22f, r_elbow = 0.52f, r_wrist_z = 0.0f;

        if (style == 0u) {
          // 0: Enthusiastic double-arm concert cheer with big vertical arm pumps & rhythmic bounce.
          l_up = 1.85f + 0.58f * beat * env;
          l_out = 0.36f + 0.18f * sway * env;
          l_twist = 0.30f;
          l_elbow = 0.92f - 0.48f * beat * env;
          r_up = 1.80f + 0.54f * beat_off * env;
          r_out = 0.34f - 0.18f * sway * env;
          r_twist = 0.30f;
          r_elbow = 0.96f - 0.44f * beat_off * env;
          spine_pitch = -0.05f + 0.11f * beat * env;
          spine_roll = 0.11f * sway * env;
          head_pitch = -0.08f + 0.13f * beat * env;
          knee_bend = 0.06f + 0.22f * (0.5f + 0.5f * beat) * env;
          hop_y = std::max(0.0f, -beat) * 0.045f * env;
        } else if (style == 1u) {
          // 1: Big one-arm concert fist pump + active off-arm swing & torso twist.
          l_up = 1.65f + 0.68f * beat * env;
          l_out = 0.28f + 0.14f * sway * env;
          l_twist = 0.32f;
          l_elbow = 1.28f - 0.60f * beat * env;
          r_up = 0.24f + 0.28f * sway * env;
          r_out = 0.16f + 0.08f * beat;
          r_twist = 0.35f;
          r_elbow = 0.72f + 0.26f * beat;
          spine_pitch = 0.02f - 0.09f * beat * env;
          spine_yaw = -0.10f + 0.18f * sway * env;
          head_pitch = -0.06f + 0.12f * beat * env;
          knee_bend = 0.06f + 0.18f * (0.5f + 0.5f * beat) * env;
        } else if (style == 2u) {
          // 2: Rhythmic chest-height festival clapping (hands visibly open wide and clap together!).
          const float clap = 0.5f + 0.5f * beat2;  // 0 = hands together, 1 = hands wide apart
          l_up = 0.64f + 0.16f * beat * env;
          l_out = 0.04f + 0.34f * clap * env;
          l_twist = 0.88f - 0.44f * clap * env;
          l_elbow = 1.62f - 0.28f * clap * env;
          r_up = 0.64f + 0.16f * beat * env;
          r_out = l_out;
          r_twist = l_twist;
          r_elbow = l_elbow;
          spine_pitch = 0.08f + 0.08f * beat * env;
          knee_bend = 0.06f + 0.16f * clap * env;
        } else if (style == 3u) {
          // 3: Wide overhead two-hand side-to-side concert wave (sweeping arcs).
          const float wave = 0.52f * sway * env;
          l_up = 2.18f + 0.22f * beat * env;
          l_out = 0.36f + wave;
          l_twist = 0.22f;
          l_elbow = 0.72f + 0.26f * std::fabs(sway);
          l_wrist_z = wave * 0.65f;
          r_up = 2.18f - 0.22f * beat * env;
          r_out = 0.36f - wave;
          r_twist = 0.22f;
          r_elbow = 0.72f + 0.26f * std::fabs(sway);
          r_wrist_z = -wave * 0.65f;
          spine_roll = 0.18f * sway * env;
          hip_x = 0.058f * sway * env;
          knee_bend = 0.05f + 0.14f * (0.5f + 0.5f * beat) * env;
        } else if (style == 4u) {
          // 4: Barrier lean & enthusiastic waving burst toward passing cars.
          const float raw_burst = 0.5f + 0.5f * std::sin(time_sec * 1.15f + phase * 1.9f);
          const float wave_t = raw_burst * raw_burst * (3.0f - 2.0f * raw_burst);
          spine_pitch = 0.16f - 0.12f * wave_t + 0.06f * beat;
          spine_yaw = gaze_yaw * 0.35f + 0.14f * sway;
          hip_z = 0.025f;
          l_up = 0.38f + 1.85f * wave_t;
          l_out = 0.14f + 0.34f * wave_t + 0.32f * sway * wave_t;
          l_twist = 0.40f - 0.14f * wave_t;
          l_elbow = 1.12f - 0.36f * wave_t + 0.30f * beat * wave_t;
          l_wrist_z = 0.45f * sway * wave_t;
          r_up = 0.34f + 0.18f * sway;
          r_out = 0.14f + 0.06f * beat;
          r_twist = 0.42f;
          r_elbow = 1.05f + 0.18f * sway;
          knee_bend = 0.08f + 0.10f * (0.5f + 0.5f * beat);
        } else if (style == 5u) {
          // 5: Festival groove & rhythmic sway with intermittent single-arm cheer.
          const float cheer_w = std::clamp(std::sin(time_sec * 0.85f + phase * 1.4f) * 1.6f - 0.3f, 0.0f, 1.0f);
          l_up = 0.28f + 0.32f * sway * env + 1.55f * cheer_w;
          l_out = 0.12f + 0.22f * cheer_w + 0.14f * sway;
          l_twist = 0.26f;
          l_elbow = 0.65f + 0.30f * beat - 0.20f * cheer_w;
          r_up = 0.28f - 0.32f * sway * env;
          r_out = 0.12f + 0.08f * beat;
          r_twist = 0.26f;
          r_elbow = 0.65f - 0.30f * beat;
          spine_pitch = 0.05f + 0.07f * beat;
          spine_yaw = 0.22f * sway * env;
          spine_roll = 0.12f * sway * env;
          hip_x = 0.055f * sway * env;
          knee_bend = 0.06f + 0.18f * (0.5f + 0.5f * beat) * env;
        } else if (style == 6u) {
          // 6: Energetic festival jump-bounce / dual shoulder-height fist pump!
          const float bounce = 0.5f + 0.5f * beat;
          l_up = 1.38f + 0.64f * beat * env;
          l_out = 0.30f + 0.14f * sway * env;
          l_twist = 0.26f;
          l_elbow = 1.52f - 0.58f * beat * env;
          r_up = 1.34f + 0.60f * beat_off * env;
          r_out = 0.30f - 0.14f * sway * env;
          r_twist = 0.26f;
          r_elbow = 1.52f - 0.54f * beat_off * env;
          spine_pitch = 0.04f - 0.12f * beat * env;
          knee_bend = 0.05f + 0.28f * (1.0f - bounce) * env;
          hop_y = std::max(0.0f, beat - 0.2f) * 0.06f * env;
        } else {
          // 7: Panning phone/camera to track passing cars + lively sway & gesture.
          const float pan = gaze_yaw * 0.70f + 0.26f * sway;
          l_up = 1.02f + 0.18f * sway + 0.12f * beat;
          l_out = 0.10f + 0.08f * sway;
          l_twist = 0.72f;
          l_elbow = 1.68f - 0.14f * beat;
          r_up = 0.98f - 0.16f * sway + 0.12f * beat;
          r_out = 0.10f - 0.08f * sway;
          r_twist = 0.72f;
          r_elbow = 1.66f + 0.14f * beat;
          spine_yaw = pan;
          spine_pitch = 0.06f + 0.06f * beat;
          head_yaw = gaze_yaw * 0.85f + 0.18f * sway;
          knee_bend = 0.06f + 0.10f * (0.5f + 0.5f * beat);
        }

        if (mirror) {
          std::swap(l_up, r_up);
          std::swap(l_out, r_out);
          std::swap(l_twist, r_twist);
          std::swap(l_elbow, r_elbow);
          std::swap(l_wrist_z, r_wrist_z);
          spine_yaw = -spine_yaw;
          spine_roll = -spine_roll;
          hip_x = -hip_x;
        }

        // Exact foot-planted knee bounce vertical drop + subtle festival cheer hop.
        const float thigh_a = knee_bend * 0.55f;
        const float hip_dy = -0.82f * (1.0f - std::cos(thigh_a)) + hop_y;

        // Legs (bones 2..7): symmetric soft knee flex keeping both feet flat on ground.
        local_r[2] = rot_x(-thigh_a);        // legL_upper
        local_r[3] = rot_x(thigh_a * 2.0f);  // legL_lower
        local_r[4] = rot_x(-thigh_a);        // legL_foot
        local_r[5] = rot_x(-thigh_a);        // legR_upper
        local_r[6] = rot_x(thigh_a * 2.0f);  // legR_lower
        local_r[7] = rot_x(-thigh_a);        // legR_foot

        // Spine (bone 8), Neck (bone 9), Head (bone 18).
        local_r[8] = mat3_mul(rot_y(spine_yaw), mat3_mul(rot_x(spine_pitch), rot_z(spine_roll)));
        local_r[9] = rot_y(head_yaw * 0.35f);
        local_r[18] = mat3_mul(rot_y(head_yaw * 0.65f), rot_x(head_pitch));

        // Clavicles (bones 10 L, 14 R): natural upward shoulder shrug when arms rise overhead.
        const float l_shrug = std::clamp((l_up - 1.1f) / 1.6f, 0.0f, 1.0f) * 0.24f;
        const float r_shrug = std::clamp((r_up - 1.1f) / 1.6f, 0.0f, 1.0f) * 0.24f;
        local_r[10] = rot_z(+l_shrug);
        local_r[14] = rot_z(-r_shrug);

        // Upper arms (11 L, 15 R), Forearms/elbows (12 L, 16 R), Hands (13 L, 17 R).
        local_r[11] = make_arm_rot(+1.0f, l_up, l_out, l_twist);
        local_r[12] = mat3_mul(rot_x(-std::clamp(l_elbow, 0.05f, 2.25f)), rot_z(l_wrist_z * 0.35f));
        local_r[13] = rot_z(l_wrist_z);
        local_r[15] = make_arm_rot(-1.0f, r_up, r_out, r_twist);
        local_r[16] = mat3_mul(rot_x(-std::clamp(r_elbow, 0.05f, 2.25f)), rot_z(r_wrist_z * 0.35f));
        local_r[17] = rot_z(r_wrist_z);

        // Forward kinematics pass: M_world[b] = M_world[parent] * T(local_off) * R_local[b],
        // M_skin[b] = M_world[b] * T(-bind_pos[b]).
        std::array<std::array<float, 3>, SpectatorCatalog::kStandBoneCount> bind_p{};
        std::array<BoneXform, SpectatorCatalog::kStandBoneCount> world_xf{};
        for (size_t b = 0; b < SpectatorCatalog::kStandBoneCount; ++b) {
          bind_p[b] = {stand_skel[b].bind_world[0], stand_skel[b].bind_world[1] - asset.mesh.min_y, stand_skel[b].bind_world[2]};
          const int16_t par = stand_skel[b].parent;
          if (par < 0) {
            world_xf[b].r = local_r[b];
            world_xf[b].t = {bind_p[b][0] + hip_x, bind_p[b][1] + hip_dy, bind_p[b][2] + hip_z};
          } else {
            const auto& pw = world_xf[size_t(par)];
            const std::array<float, 3> local_off{
                bind_p[b][0] - bind_p[size_t(par)][0],
                bind_p[b][1] - bind_p[size_t(par)][1],
                bind_p[b][2] - bind_p[size_t(par)][2],
            };
            const auto rot_off = mat3_vec(pw.r, local_off);
            world_xf[b].r = mat3_mul(pw.r, local_r[b]);
            world_xf[b].t = {pw.t[0] + rot_off[0], pw.t[1] + rot_off[1], pw.t[2] + rot_off[2]};
          }
          const auto neg_bind = mat3_vec(world_xf[b].r, bind_p[b]);
          bone_xf[b].r = world_xf[b].r;
          bone_xf[b].t = {world_xf[b].t[0] - neg_bind[0], world_xf[b].t[1] - neg_bind[1],
                          world_xf[b].t[2] - neg_bind[2]};
        }
      }

      // Select crisp multi-LOD mesh by distance scaled to TrackSettings crowd_sprite_distance.
      const float lod0_end = std::min(22.0f, sprite_dist * 0.45f);
      const float lod1_end = std::min(50.0f, sprite_dist * 0.85f);
      const size_t lod_idx = (dist < lod0_end) ? 0u : ((dist < lod1_end) ? 1u : 2u);
      const auto& verts = asset.mesh.vertices;
      const auto& inds =
          !asset.mesh.lod_indices[lod_idx].empty() ? asset.mesh.lod_indices[lod_idx] : asset.mesh.indices;

      if (shadow_discs) {
        shadow_discs->push_back({px, py + 0.03f, pz, 0.42f * scale});
      }

      const size_t max_rig_bones = asset.mesh.has_sit_skeleton ? SpectatorCatalog::kSitBoneCount
                                                               : SpectatorCatalog::kStandBoneCount;
      const bool has_rig = asset.mesh.has_stand_skeleton || asset.mesh.has_sit_skeleton;
      const float lm_u = lm_off[0] + 0.5f * inv_grid;

      // Skin each unique vertex referenced by `inds` ONCE into thread-local scratch, then emit triangles.
      if (tl_posed.size() < verts.size()) {
        tl_posed.resize(verts.size());
        tl_stamp.resize(verts.size(), 0u);
      }
      if (++tl_gen == 0u) {
        std::fill(tl_stamp.begin(), tl_stamp.end(), 0u);
        tl_gen = 1u;
      }
      const uint32_t cur_gen = tl_gen;

      out.reserve(out.size() + inds.size());
      for (uint16_t idx : inds) {
        if (tl_stamp[idx] != cur_gen) {
          tl_stamp[idx] = cur_gen;
          const auto& v = verts[idx];
          // Normalize feet strictly to y=0 so every spectator sits/stands flush on the terrain surface.
          const std::array<float, 3> bp{v.pos[0], v.pos[1] - asset.mesh.min_y, v.pos[2]};
          std::array<float, 3> posed_p = bp;
          std::array<float, 3> posed_n = v.normal;

          if (has_rig && v.bone0 < max_rig_bones) {
            const float w0 = float(v.weight0);
            const float w1 = (v.bone1 < max_rig_bones) ? float(v.weight1) : 0.0f;
            const float wsum = w0 + w1;
            if (wsum > 1e-3f) {
              const float inv_w = 1.0f / wsum;
              const float a0 = w0 * inv_w;
              const float a1 = w1 * inv_w;
              std::array<float, 3> p0{}, n0{};
              apply_bone_xform(bone_xf[v.bone0], bp, v.normal, p0, n0);
              if (a1 > 0.004f) {
                std::array<float, 3> p1{}, n1{};
                apply_bone_xform(bone_xf[v.bone1], bp, v.normal, p1, n1);
                for (int k = 0; k < 3; ++k) {
                  posed_p[k] = p0[k] * a0 + p1[k] * a1;
                  posed_n[k] = n0[k] * a0 + n1[k] * a1;
                }
                posed_n = vnorm(posed_n);
              } else {
                posed_p = p0;
                posed_n = n0;
              }
            }
          } else {
            // Fallback upper-body sway for synthetic meshes without a skeletal rig.
            const float upper = std::clamp((bp[1] - 0.85f) / 0.85f, 0.0f, 1.0f);
            const float mid = std::clamp((bp[1] - 0.12f) / 0.60f, 0.0f, 1.0f);
            posed_p[0] += sway * 0.065f * upper;
            posed_p[1] += (0.5f + 0.5f * beat) * 0.045f * mid;
            posed_p[2] += beat * 0.050f * upper;
          }

          const float lx = posed_p[0] * scale;
          const float ly = posed_p[1] * scale;
          const float lz = posed_p[2] * scale;

          const float wx = px + lx * cyaw + lz * syaw;
          const float wy = py + ly;
          const float wz = pz - lx * syaw + lz * cyaw;

          const float nx = posed_n[0] * cyaw + posed_n[2] * syaw;
          const float ny = posed_n[1];
          const float nz = -posed_n[0] * syaw + posed_n[2] * cyaw;

          const float cu = std::clamp(v.uv[0], uv_pad, 1.0f - uv_pad) * inv_grid;
          const float cv = std::clamp(v.uv[1], uv_pad, 1.0f - uv_pad) * inv_grid;
          const float u = diff_off[0] + cu;
          const float tv = diff_off[1] + cv;
          // Sample overhead vertical irradiance gradient cell by height (0 = head/top, 1 = shoes/bottom)
          // plus subtle normal upward tilt so faces & shoulders catch stage spotlights cleanly.
          const float h_norm = std::clamp(1.0f - bp[1] / 1.82f - posed_n[1] * 0.14f, 0.04f, 0.96f);
          const float lm_v = lm_off[1] + h_norm * inv_grid;
          tl_posed[idx] = {{wx, wy, wz}, {nx, ny, nz}, {u, tv}, eval_crowd_tint(ny), {lm_u, lm_v}};
        }
        out.push_back(tl_posed[idx]);
      }
    }
  }
  if (out_nearest_dist) *out_nearest_dist = nearest_dist;
}

}  // namespace world
